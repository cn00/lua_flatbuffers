#include "lflatbuffers.hpp"

#include <flatbuffers/util.h>

#include <cerrno>
#include <iostream>

/* linux open dir */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>


#define LIB_NAME "lua_flatbuffers"

/* check if postfix match */
static int is_postfix_file( const char *path,const char *postfix )
{
    /* simply check,not consider file like ./subdir/.bfbs */
    size_t sz = strlen( postfix );
    size_t ps = strlen( path );

    /* file like .bfbs will be ignore */
    if ( ps <= sz + 2 ) return 0;

    if ( '.' == path[ps-sz-1] && 0 == strcmp( path + ps - sz,postfix ) )
    {
        return true;
    }

    return false;
}

/* load all the schema files in this directory */
int lflatbuffers::load_bfbs_path( const char *path,const char *postfix )
{
    DIR *dir = opendir( path );
    if ( !dir )
    {
        _error_collector.what = "can not open directory:";
        _error_collector.what.append( path );
        _error_collector.what.append( "," );
        _error_collector.what.append( strerror(errno) );

        return -1;
    }

    int count = 0;
    struct dirent *dt = NULL;
    while ( (dt = readdir( dir )) )
    {
        struct stat path_stat;
        stat( dt->d_name, &path_stat );

        /* dt->d_type == DT_REG not supported by all file system types */
        if ( S_ISREG( path_stat.st_mode ) //This is a regular file
            && is_postfix_file( dt->d_name,postfix) )
        {
            if ( !load_bfbs_file( dt->d_name ) )
            {
                _bfbs_buffer.clear();
                return -1;
            }
            ++ count;
        }
    }
    return count;
}

bool lflatbuffers::load_bfbs_file( const char *file )
{
    /* if the file already loaded,consider it need to update,not dumplicate */
    std::string &bfbs = _bfbs_buffer[file];
    if ( !flatbuffers::LoadFile( file,true,&bfbs ) )
    {
        _bfbs_buffer.erase( file );

        _error_collector.what = "can not load file:";
        _error_collector.what.append( file );
        _error_collector.what.append( "," );
        _error_collector.what.append( strerror(errno) );

        return false;
    }

    flatbuffers::Verifier encode_verifier(
        reinterpret_cast<const uint8_t *>(bfbs.c_str()), bfbs.length());
    if ( !reflection::VerifySchemaBuffer(encode_verifier) )
    {
        _bfbs_buffer.erase( file );

        _error_collector.what = "invalid flatbuffers binary schema file:";
        _error_collector.what.append( file );

        return false;
    }

    make_build_sequence( file,reflection::GetSchema( bfbs.c_str() ) );
    return true;
}

const char *lflatbuffers::last_error()
{
    return _error_collector.what.c_str();
}

/* flatbuffers has to be built in post-order,so we need to iterate schema to
 * mark which field to be built first.to be more convenient,we put all
 * object(include struct,not just table those have offset table) into nested,
 * so we don't need to pass schema as parameter or call Get to get a object
 * pointer.
 */
void lflatbuffers::make_object_sequence( const reflection::Schema *schema,
    struct sequence &seq,const reflection::Object *object )
{
#define PUSH_SEQUENCE()   \
    do{\
        struct sequence sub_seq;\
        sub_seq.field = *field_itr;\
        sub_seq.object = sub_object;\
        make_object_sequence( schema,sub_seq,sub_object );\
        seq.nested.push_back( sub_seq );\
    }while(0)

    const auto *fields = object->fields();
    for ( auto field_itr = fields->begin();field_itr != fields->end();field_itr++ )
    {
        const reflection::Type *type = (*field_itr)->type();
        switch ( type->base_type() )
        {
            case reflection::Obj:
            {
                auto *sub_object = schema->objects()->Get( type->index() );
                PUSH_SEQUENCE();
            }break;
            case reflection::Vector:
            {
                if ( reflection::Obj == type->element() )
                {
                    auto *sub_object = schema->objects()->Get( type->index() );
                    PUSH_SEQUENCE();
                }
                else
                {
                    seq.scalar.push_back( *field_itr );
                }
            }break;
            /* those type fall through */
            case reflection::UType:
            case reflection::Byte:
            case reflection::Bool:
            case reflection::UByte:
            case reflection::Short:
            case reflection::UShort:
            case reflection::Int:
            case reflection::UInt:
            case reflection::Long:
            case reflection::ULong:
            case reflection::Float:
            case reflection::Double:
            case reflection::String:
            case reflection::Union:
            {
                seq.scalar.push_back( *field_itr );
            }break;
            case reflection::None:
            {
                assert( false );
            }break;
        }
    }

#undef PUSH_SEQUENCE
}

/* flatbuffers has to be built in post-order,but lua table is not
 * we have to iterate schema and make a build sequence cache
 */
void lflatbuffers::make_build_sequence(
    const char *schema_name,const reflection::Schema *schema )
{
    auto &schema_sequence = _schema[schema_name];
    /* clear old data if exist,in case update operation */
    schema_sequence.clear();

    const auto *objects = schema->objects();
    for ( auto itr = objects->begin();itr != objects->end(); itr++ )
    {
        auto &seq = schema_sequence[(*itr)->name()->c_str()];

        seq.object = *itr; /* root object,field is NULL */
        make_object_sequence( schema,seq,*itr );
    }
}

int lflatbuffers::encode_struct(
    uint8_t *buffer,lua_State *L,const struct sequence &seq,int index )
{
#define CHECK_FIELD()    \
    do{\
        lua_getfield( L,index,field->name()->c_str() );\
        if ( lua_isnil( L,index + 1 ) )\
        {\
            _error_collector.what = "missing required field";\
            _error_collector.backtrace.push( field->name()->c_str() );\
            lua_pop( L,1 );\
            return -1;\
        }\
    }while(0)

#define SET_INTEGER(T)   \
    do{\
        if ( lua_isnumber( L,index + 1 ) )\
        {\
            _error_collector.what = std::string( "expect number,got" )\
                + lua_typename( L,lua_type(L,index+1) );\
            _error_collector.backtrace.push( field->name()->c_str() );\
            lua_pop( L,1 );\
            return -1;\
        }\
        int64_t val = lua_tointeger( L,index + 1 );\
        flatbuffers::WriteScalar(data, static_cast<T>(val));\
    }while(0)

#define SET_NUMBER(T)   \
    do{\
        if ( lua_isnumber( L,index + 1 ) )\
        {\
            _error_collector.what = std::string( "expect number,got" )\
                + lua_typename( L,lua_type(L,index+1) );\
            _error_collector.backtrace.push( field->name()->c_str() );\
            lua_pop( L,1 );\
            return -1;\
        }\
        double val = lua_tonumber( L,index + 1 );\
        flatbuffers::WriteScalar(data, static_cast<T>(val));\
    }while(0)

    assert( seq.object->is_struct() );
    for ( auto nested_itr = seq.nested.begin();nested_itr != seq.nested.end();nested_itr ++ )
    {
        const auto field = nested_itr->field; //reflection::Field
        assert( nested_itr->object && nested_itr->object->is_struct() );

        CHECK_FIELD();
        int r = encode_struct( buffer + field->offset(),L,*nested_itr,index + 1 );
        lua_pop( L,1 );

        if ( r < 0 ) return -1;
    }

    assert( lua_gettop( L ) == index );

    for ( auto scalar_itr = seq.scalar.begin();scalar_itr != seq.scalar.end();scalar_itr ++ )
    {
        const auto field = *scalar_itr;

        CHECK_FIELD();

        uint8_t *data = buffer + field->offset();
        switch ( field->type()->base_type() )
        {
            case reflection::None: /* auto fall through */
            case reflection::String:
            case reflection::Vector:
            case reflection::Union:
            case reflection::Obj: /* table or struct */
            {
                assert( false ); /* struct never contain those types */
            }break;
            case reflection::Bool:
            {
                bool val = lua_toboolean( L,index + 1 );
                flatbuffers::WriteScalar( data, static_cast<uint8_t>(val) );
            }break;
            case reflection::UType: SET_INTEGER(uint8_t );break;
            case reflection::Byte:  SET_INTEGER(int8_t  );break;
            case reflection::UByte: SET_INTEGER(uint8_t );break;
            case reflection::Short: SET_INTEGER(int16_t );break;
            case reflection::UShort:SET_INTEGER(uint16_t);break;
            case reflection::Int:   SET_INTEGER(int32_t );break;
            case reflection::UInt:  SET_INTEGER(uint32_t);break;
            case reflection::Long:  SET_INTEGER(int64_t );break;
            case reflection::ULong: SET_INTEGER(uint64_t);break;
            case reflection::Float: SET_NUMBER (float   );break;
            case reflection::Double:SET_NUMBER (double  );break;
        }

        lua_pop( L,1 ); /* pop the value which push at CHECK_FIELD */
    }

    return 0;

#undef SET_INTEGER
#undef SET_NUMBER
#undef CHECK_FIELD
}

int lflatbuffers::encode_table( flatbuffers::uoffset_t &offset,
                lua_State *L,const struct sequence &seq,int index )
{
#define CHECK_FIELD()   \
    do{\
        lua_getfield( L,index,field->name()->c_str() );\
        if ( lua_isnil( L,index + 1 ) )\
        {\
            lua_pop( L,1 );\
            continue; /* all field in table is optional */\
        }\
    }while(0)

#define ADD_INTEGER(T)   \
    do{\
        if ( lua_isnumber( L,index + 1 ) )\
        {\
            _error_collector.what = std::string( "expect number,got" )\
                + lua_typename( L,lua_type(L,index+1) );\
            _error_collector.backtrace.push( field->name()->c_str() );\
            lua_pop( L,1 );\
            return -1;\
        }\
        int64_t val = lua_tointeger( L,index + 1 );\
        _fbb.AddElement<T>(off, val,0);\
    }while(0)

#define ADD_NUMBER(T)   \
    do{\
        if ( lua_isnumber( L,index + 1 ) )\
        {\
            _error_collector.what = std::string( "expect number,got" )\
                + lua_typename( L,lua_type(L,index+1) );\
            _error_collector.backtrace.push( field->name()->c_str() );\
            lua_pop( L,1 );\
            return -1;\
        }\
        double val = lua_tonumber( L,index + 1 );\
        _fbb.AddElement<T>(off, val,0 );\
    }while(0)

    assert( !seq.object->is_struct() );

    std::vector< std::pair<uint16_t,flatbuffers::uoffset_t> > nested_offset;
    for ( auto nested_itr = seq.nested.begin();nested_itr != seq.nested.end();nested_itr ++ )
    {
        const auto field = (*nested_itr).field; //reflection::Field
        assert( (*nested_itr).object && field );

        CHECK_FIELD();
        flatbuffers::uoffset_t one_nested_offset = 0;
        int r = encode_object( one_nested_offset,L,*nested_itr,index );

        lua_pop( L,1 );
        if ( r < 0 )
        {
            _error_collector.backtrace.push( field->name()->c_str() );
            return -1;
        }
        nested_offset.push_back(
            std::make_pair( field->offset(),one_nested_offset) );
    }

    flatbuffers::uoffset_t start = _fbb.StartTable();
    for ( auto scalar_itr = seq.scalar.begin();scalar_itr != seq.scalar.end();scalar_itr ++ )
    {
        const auto field = *scalar_itr;

        CHECK_FIELD();

        uint16_t off = field->offset();
        switch ( field->type()->base_type() )
        {
            /* object shouble be handled at nested */
            case reflection::Obj: assert( false );break;
            case reflection::None: assert( false );break;
            case reflection::String:
            {
            }break;
            case reflection::Vector:
            {
            }break;
            case reflection::Union:
            {
            }break;
            case reflection::Bool:
            {
                bool bool_val = lua_toboolean( L,index + 1 );
                _fbb.AddElement<uint8_t>( off, bool_val,0 );
            }break;
            case reflection::UType: ADD_INTEGER(uint8_t );break;
            case reflection::Byte:  ADD_INTEGER(int8_t  );break;
            case reflection::UByte: ADD_INTEGER(uint8_t );break;
            case reflection::Short: ADD_INTEGER(int16_t );break;
            case reflection::UShort:ADD_INTEGER(uint16_t);break;
            case reflection::Int:   ADD_INTEGER(int32_t );break;
            case reflection::UInt:  ADD_INTEGER(uint32_t);break;
            case reflection::Long:  ADD_INTEGER(int64_t );break;
            case reflection::ULong: ADD_INTEGER(uint64_t);break;
            case reflection::Float: ADD_NUMBER (float   );break;
            case reflection::Double:ADD_NUMBER (double  );break;
        }

        lua_pop( L,1 ); /* pop the value which push at CHECK_FIELD */
    }

    for ( auto itr = nested_offset.begin();itr != nested_offset.end();itr++ )
    {
        _fbb.AddOffset( itr->first,flatbuffers::Offset<void>( itr->second ) );
    }

    offset = _fbb.EndTable( start,seq.object->fields()->size() );

    return 0;

#undef ADD_INTEGER
#undef ADD_NUMBER
#undef CHECK_FIELD
}
/* encode into a flatbuffers object( struct or table ) */
int lflatbuffers::encode_object( flatbuffers::uoffset_t &offset,
                lua_State *L,const struct sequence &seq,int index )
{

    if ( seq.object->is_struct() )
    {
        /* "structs" are flat structures that do not have an offset table
         * always have all members present.so we "new" a flat buffer at
         * FlatBufferBuilder,then fill every member.
         */
        _fbb.StartStruct( seq.object->minalign() );
        uint8_t* buffer = _fbb.ReserveElements( seq.object->bytesize(), 1 );
        encode_struct( buffer,L,seq,index );
        offset =  _fbb.EndStruct();

        return 0;
    }

    return 0;
}

int lflatbuffers::encode( lua_State *L,
    const char *schema,const char *object,int index )
{
    schema_map::iterator sch_itr = _schema.find( schema );
    if ( sch_itr == _schema.end() )
    {
        _error_collector.what = "no such schema";
        return -1;
    }

    sequence_map::iterator seq_itr = (sch_itr->second).find( object );
    if ( seq_itr == (sch_itr->second).end() )
    {
        _error_collector.what = std::string("no such object(")
            + object + ") at schema(" + schema + ").";
        return -1;
    }

    /* Reset all the state in this FlatBufferBuilder so it can be reused
     * to construct another buffer
     */
    _fbb.Clear();

    flatbuffers::uoffset_t offset;
    if ( encode_object( offset,L,seq_itr->second,index ) <  0 )
    {
        _error_collector.schema = schema;
        return -1;
    }
    _fbb.Finish( flatbuffers::Offset<void>(offset) );

    return 0;
}

int lflatbuffers::decode( lua_State *L,
    const char *schema,const char *object,int index )
{
    return 0;
}

/* ========================== static function for lua ======================= */

static int load_bfbs_path( lua_State *L )
{
    class lflatbuffers** lfb =
        (class lflatbuffers**)luaL_checkudata( L, 1, LIB_NAME );
    if ( lfb == NULL || *lfb == NULL )
    {
        return luaL_error(L, "load_bfbs_path argument #1 expect %s", LIB_NAME);
    }

    const char *path = luaL_checkstring( L,2 );
    const char *postfix = luaL_optstring( L,3,"bfbs" );

    int count = (*lfb)->load_bfbs_path( path,postfix );
    if ( count < 0 )
    {
        return luaL_error( L,(*lfb)->last_error() );
    }

    lua_pushinteger( L,count );
    return 1;
}

static int load_bfbs_file( lua_State *L )
{
    class lflatbuffers** lfb =
        (class lflatbuffers**)luaL_checkudata( L, 1, LIB_NAME );
    if ( lfb == NULL || *lfb == NULL )
    {
        return luaL_error(L, "load_bfbs_file argument #1 expect %s", LIB_NAME);
    }

    const char *path = luaL_checkstring( L,2 );
    if ( !(*lfb)->load_bfbs_file( path ) )
    {
        return luaL_error( L,(*lfb)->last_error() );
    }

    lua_pushinteger( L,1 );
    return 1;
}

static int encode( lua_State *L )
{
    class lflatbuffers** lfb =
        (class lflatbuffers**)luaL_checkudata( L, 1, LIB_NAME );
    if ( lfb == NULL || *lfb == NULL )
    {
        return luaL_error( L, "encode argument #1 expect %s", LIB_NAME );
    }

    const char *schema = luaL_checkstring( L,2 );
    const char *object = luaL_checkstring( L,3 );
    if ( !lua_istable( L,4) )
    {
        return luaL_error( L,"argument #1 expect table,got %s",
            lua_typename(L, lua_type(L, 4)) );
    }

    if ( (*lfb)->encode( L,schema,object,4 ) < 0 )
    {
        return luaL_error( L,(*lfb)->last_error() );
    }
    return 1;
}

static int decode( lua_State *L )
{
    return 1;
}

/* create a C++ object and push to lua stack */
static int __call( lua_State* L )
{
    /* lua调用__call,第一个参数是该元表所属的table.取构造函数参数要注意 */
    class lflatbuffers* obj = new class lflatbuffers();

    lua_settop( L,1 ); /* 清除所有构造函数参数,只保留元表 */

    class lflatbuffers** ptr =
        (class lflatbuffers**)lua_newuserdata(L, sizeof(class lflatbuffers*));
    *ptr = obj;

    /* 把新创建的userdata和元表交换堆栈位置 */
    lua_insert(L,1);

    /* 弹出元表,并把元表设置为userdata的元表 */
    lua_setmetatable(L, -2);

    return 1;
}

/* 元方法,__tostring */
static int __tostring( lua_State* L )
{
    class lflatbuffers** ptr = (class lflatbuffers**)luaL_checkudata(L, 1,LIB_NAME);
    if(ptr != NULL)
    {
        lua_pushfstring(L, "%s: %p", LIB_NAME, *ptr);
        return 1;
    }
    return 0;
}

/*  元方法,__gc */
static int __gc( lua_State* L )
{
    class lflatbuffers** ptr = (class lflatbuffers**)luaL_checkudata(L, 1,LIB_NAME);
    if ( *ptr != NULL ) delete *ptr;
    *ptr = NULL;

    return 0;
}

/* ====================LIBRARY INITIALISATION FUNCTION======================= */

int luaopen_lua_flatbuffers(lua_State *L)
{
    //luaL_newlib(L, lua_parson_lib);
    if ( 0 == luaL_newmetatable( L,LIB_NAME ) )
    {
        assert( false );
        return 0;
    }

    lua_pushcfunction(L, __gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, __tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, encode);
    lua_setfield(L, -2, "encode");

    lua_pushcfunction(L, decode);
    lua_setfield(L, -2, "decode");

    lua_pushcfunction(L, load_bfbs_path);
    lua_setfield(L, -2, "load_bfbs_path");

    lua_pushcfunction(L, load_bfbs_file);
    lua_setfield(L, -2, "load_bfbs_file");

    /* metatable as value and pop metatable */
    lua_pushvalue( L,-1 );
    lua_setfield(L, -2, "__index");

    lua_newtable( L );
    lua_pushcfunction(L, __call);
    lua_setfield(L, -2, "__call");
    lua_setmetatable( L,-2 );

    return 1;  /* return metatable */
}
