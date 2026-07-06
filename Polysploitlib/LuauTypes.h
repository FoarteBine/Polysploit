#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>

// Luau internal type tags
enum LuauType {
    LUA_TNIL = 0,
    LUA_TBOOLEAN = 1,
    LUA_TLIGHTUSERDATA = 2,
    LUA_TNUMBER = 3,
    LUA_TINTEGER = 4,
    LUA_TVECTOR = 5,
    LUA_TSTRING = 6,
    LUA_TTABLE = 7,
    LUA_TFUNCTION = 8,
    LUA_TUSERDATA = 9,
    LUA_TTHREAD = 10,
    LUA_TBUFFER = 11,
    LUA_TPROTO = 12,
    LUA_TUPVAL = 13,
    LUA_TDEADKEY = 14,
};

struct TString {
    void* next;
    uint8_t tt;
    uint8_t marked;
    uint8_t extra;
    uint32_t hash;
    size_t len;
    char data[1];
};

#define getstr(ts) ((ts)->data)

union Value {
    double n;
    int64_t i;
    void* gc;
    void* p;
    float v[4];
    bool b;
};

struct TValue {
    Value value;
    uint8_t tt;
    uint8_t extra[7];
};

#define ttype(o) ((o)->tt)
#define ttisnil(o) (ttype(o) == LUA_TNIL)
#define ttisboolean(o) (ttype(o) == LUA_TBOOLEAN)
#define ttisnumber(o) (ttype(o) == LUA_TNUMBER)
#define ttisinteger(o) (ttype(o) == LUA_TINTEGER)
#define ttisstring(o) (ttype(o) == LUA_TSTRING)
#define ttisvector(o) (ttype(o) == LUA_TVECTOR)
#define ttisfunction(o) (ttype(o) == LUA_TFUNCTION)
#define ttistable(o) (ttype(o) == LUA_TTABLE)

#define nvalue(o) ((o)->value.n)
#define lvalue(o) ((o)->value.i)
#define bvalue(o) ((o)->value.b)
#define vvalue(o) ((o)->value.v)
#define tsvalue(o) ((TString*)((o)->value.gc))
#define clvalue(o) ((Closure*)((o)->value.gc))
#define hvalue(o) ((LuaTable*)((o)->value.gc))
#define sizenode(t) (1 << (t)->lsizenode)

struct LocVar {
    TString* varname;
    int startpc;
    int endpc;
    int reg;
};

struct UpvalDesc {
    TString* name;
};

struct Proto {
    void* next;
    uint8_t tt;
    uint8_t marked;
    uint8_t extra;
    int sizecode;
    int sizek;
    int sizep;
    int sizelocvars;
    int sizeupvars;
    int sizeupvalues;
    int sizelineinfo;
    int padd;
    int* code;
    TValue* k;
    Proto** p;
    LocVar* locvars;
    TString** upvalues;
    TString* source;
    TString* debugname;
    int linedefined;
    int maxstacksize;
    int numparams;
    int nups;
    int is_vararg;
    int flags;
    int sizetypeinfo;
    char* typeinfo;
    uint8_t* lineinfo;
    int8_t linegaplog2;
    int* abslineinfo;
};

struct Closure {
    void* next;
    uint8_t tt;
    uint8_t marked;
    uint8_t extra;
    bool isC;
    int nups;
    void* upvals;
    union {
        struct { Proto* p; } l;
        struct { void* f; int env; } c;
    };
};

struct TKey {
    Value value;
    uint8_t tt;
    uint8_t extra[7];
};

struct LuaNode {
    TKey key;
    TValue val;
};

struct LuaTable {
    void* next;
    uint8_t tt;
    uint8_t marked;
    uint8_t extra;
    int lsizenode;
    int sizearray;
    TValue* array;
    LuaNode* node;
    TValue* metatable;
};

typedef int (*lua_Writer)(int64_t L, const void* p, size_t sz, void* ud);

#define LBC_VERSION_TARGET 6
#define LBC_TYPE_VERSION_TARGET 3

#define LBC_CONSTANT_NIL 0
#define LBC_CONSTANT_BOOLEAN 1
#define LBC_CONSTANT_NUMBER 2
#define LBC_CONSTANT_STRING 3
#define LBC_CONSTANT_IMPORT 4
#define LBC_CONSTANT_TABLE 5
#define LBC_CONSTANT_CLOSURE 6
#define LBC_CONSTANT_VECTOR 7
#define LBC_CONSTANT_TABLE_WITH_CONSTANTS 8
#define LBC_CONSTANT_INTEGER 9

#define LUAU_VECTOR_SIZE 3
#define LUAU_INSN_OP(insn) ((uint8_t)((insn) & 0xff))
#define LUAU_INSN_D(insn) ((int32_t)(insn) >> 16)
#define LOP_GETIMPORT 12
