#pragma once
#include "LuauTypes.h"

struct LuauDumpState {
    int64_t L;
    lua_Writer writer;
    void* data;
    int strip;
    int status;
    std::vector<const TString*> strings;
    std::unordered_map<const TString*, uint32_t> stringIds;
    std::vector<const Proto*> protos;
    std::unordered_map<const Proto*, uint32_t> protoIds;
};

static void DumpBlock(LuauDumpState* D, const void* data, size_t size) {
    if (D->status == 0 && size)
        D->status = D->writer(D->L, data, size, D->data);
}

template<typename T>
static void DumpRaw(LuauDumpState* D, const T& value) { DumpBlock(D, &value, sizeof(T)); }
static void DumpByte(LuauDumpState* D, uint8_t value) { DumpRaw(D, value); }

static void DumpVarInt(LuauDumpState* D, uint32_t value) {
    do {
        uint8_t byte = value & 127;
        value >>= 7;
        if (value) byte |= 128;
        DumpByte(D, byte);
    } while (value);
}

static void DumpVarInt64(LuauDumpState* D, uint64_t value) {
    do {
        uint8_t byte = value & 127;
        value >>= 7;
        if (value) byte |= 128;
        DumpByte(D, byte);
    } while (value);
}

static uint32_t AddString(LuauDumpState* D, const TString* s) {
    if (!s) return 0;
    auto it = D->stringIds.find(s);
    if (it != D->stringIds.end()) return it->second;
    uint32_t id = (uint32_t)(D->strings.size() + 1);
    D->strings.push_back(s);
    D->stringIds[s] = id;
    return id;
}

static bool ConstantEquals(const TValue* a, const TValue* b) {
    if (ttype(a) != ttype(b)) return false;
    switch (ttype(a)) {
        case LUA_TNIL: return true;
        case LUA_TBOOLEAN: return bvalue(a) == bvalue(b);
        case LUA_TNUMBER: return nvalue(a) == nvalue(b);
        case LUA_TINTEGER: return lvalue(a) == lvalue(b);
        case LUA_TSTRING: return tsvalue(a) == tsvalue(b);
        case LUA_TVECTOR: {
            const float* av = vvalue(a);
            const float* bv = vvalue(b);
            return av[0] == bv[0] && av[1] == bv[1] && av[2] == bv[2];
        }
        default: return false;
    }
}

static int FindConstantIndex(const Proto* p, const TValue* key) {
    for (int i = 0; i < p->sizek; i++)
        if (ConstantEquals(&p->k[i], key)) return i;
    return -1;
}

static void CollectImportConstants(const Proto* p, std::unordered_map<int, uint32_t>& imports) {
    for (int pc = 0; pc < p->sizecode; pc++) {
        uint32_t insn = (uint32_t)p->code[pc];
        uint8_t op = LUAU_INSN_OP(insn);
        if (op == LOP_GETIMPORT && pc + 1 < p->sizecode) {
            int constantIndex = LUAU_INSN_D(insn);
            if (constantIndex >= 0) {
                uint32_t importId = (uint32_t)p->code[pc + 1];
                imports[constantIndex] = importId;
            }
            pc++;
        }
    }
}

static bool DumpTableConstant(LuauDumpState* D, const Proto* p, const TValue* o) {
    LuaTable* table = hvalue(o);
    if (!table) return false;
    std::vector<int> keyIndices;
    for (int i = 0; i < table->sizearray; i++) {
        TValue key{};
        key.tt = LUA_TNUMBER;
        key.value.n = (double)(i + 1);
        int keyIndex = FindConstantIndex(p, &key);
        if (keyIndex >= 0) keyIndices.push_back(keyIndex);
    }
    int nodeCount = sizenode(table);
    for (int i = 0; i < nodeCount; i++) {
        LuaNode* node = &table->node[i];
        if (node->key.tt == LUA_TNIL || node->key.tt == LUA_TDEADKEY) continue;
        TValue key{};
        key.value = node->key.value;
        memcpy(key.extra, node->key.extra, sizeof(key.extra));
        key.tt = node->key.tt;
        int keyIndex = FindConstantIndex(p, &key);
        if (keyIndex >= 0) keyIndices.push_back(keyIndex);
    }
    DumpByte(D, LBC_CONSTANT_TABLE);
    DumpVarInt(D, (uint32_t)keyIndices.size());
    for (int keyIndex : keyIndices) DumpVarInt(D, (uint32_t)keyIndex);
    return true;
}

static bool DumpConstant(LuauDumpState* D, const Proto* p, const TValue* o) {
    switch (ttype(o)) {
        case LUA_TNIL: DumpByte(D, LBC_CONSTANT_NIL); return true;
        case LUA_TBOOLEAN: DumpByte(D, LBC_CONSTANT_BOOLEAN); DumpByte(D, (uint8_t)bvalue(o)); return true;
        case LUA_TNUMBER: DumpByte(D, LBC_CONSTANT_NUMBER); DumpRaw(D, nvalue(o)); return true;
        case LUA_TINTEGER: {
            int64_t v = lvalue(o);
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)(~v + 1) : (uint64_t)v;
            DumpByte(D, LBC_CONSTANT_INTEGER);
            DumpByte(D, (uint8_t)neg);
            DumpVarInt64(D, mag);
            return true;
        }
        case LUA_TSTRING: DumpByte(D, LBC_CONSTANT_STRING); DumpVarInt(D, AddString(D, tsvalue(o))); return true;
        case LUA_TVECTOR: {
            const float* v = vvalue(o);
            DumpByte(D, LBC_CONSTANT_VECTOR);
            DumpRaw(D, v[0]); DumpRaw(D, v[1]); DumpRaw(D, v[2]);
            DumpRaw(D, (float)0.0f);
            return true;
        }
        case LUA_TFUNCTION: {
            Closure* cl = clvalue(o);
            if (cl->isC) return false;
            DumpByte(D, LBC_CONSTANT_CLOSURE);
            DumpVarInt(D, D->protoIds[cl->l.p]);
            return true;
        }
        case LUA_TTABLE: return DumpTableConstant(D, p, o);
        default: return false;
    }
}

static void DumpStringId(LuauDumpState* D, const TString* s) { DumpVarInt(D, AddString(D, s)); }
static void DumpStringTable(LuauDumpState* D) {
    DumpVarInt(D, (uint32_t)D->strings.size());
    for (const TString* s : D->strings) {
        DumpVarInt(D, (uint32_t)s->len);
        DumpBlock(D, getstr(s), s->len);
    }
}

static uint32_t AddProto(LuauDumpState* D, const Proto* p) {
    auto it = D->protoIds.find(p);
    if (it != D->protoIds.end()) return it->second;
    uint32_t id = (uint32_t)D->protos.size();
    D->protos.push_back(p);
    D->protoIds[p] = id;
    AddString(D, p->source);
    AddString(D, p->debugname);
    for (int i = 0; i < p->sizek; i++) {
        const TValue* o = &p->k[i];
        if (ttisstring(o)) AddString(D, tsvalue(o));
        else if (ttisfunction(o) && !clvalue(o)->isC) AddProto(D, clvalue(o)->l.p);
    }
    for (int i = 0; i < p->sizep; i++) AddProto(D, p->p[i]);
    if (!D->strip) {
        for (int i = 0; i < p->sizelocvars; i++) AddString(D, p->locvars[i].varname);
        for (int i = 0; i < p->sizeupvalues; i++) AddString(D, p->upvalues[i]);
    }
    return id;
}

static void CollectProtoTree(LuauDumpState* D, const Proto* root) { AddProto(D, root); }

static bool DumpProto(LuauDumpState* D, const Proto* p) {
    DumpByte(D, (uint8_t)p->maxstacksize);
    DumpByte(D, (uint8_t)p->numparams);
    DumpByte(D, (uint8_t)p->nups);
    DumpByte(D, (uint8_t)p->is_vararg);
    DumpByte(D, (uint8_t)p->flags);
    if (p->sizetypeinfo > 0 && p->typeinfo) {
        DumpVarInt(D, (uint32_t)p->sizetypeinfo);
        DumpBlock(D, p->typeinfo, p->sizetypeinfo);
    } else {
        DumpVarInt(D, 0);
    }
    DumpVarInt(D, (uint32_t)p->sizecode);
    for (int i = 0; i < p->sizecode; i++) DumpRaw(D, p->code[i]);
    std::unordered_map<int, uint32_t> importConstants;
    CollectImportConstants(p, importConstants);
    int constantCount = p->sizek;
    for (const auto& pair : importConstants)
        if (pair.first + 1 > constantCount) constantCount = pair.first + 1;
    DumpVarInt(D, (uint32_t)constantCount);
    for (int i = 0; i < constantCount; i++) {
        auto importIt = importConstants.find(i);
        if (importIt != importConstants.end()) {
            DumpByte(D, LBC_CONSTANT_IMPORT);
            DumpRaw(D, importIt->second);
            continue;
        }
        if (i >= p->sizek) { DumpByte(D, LBC_CONSTANT_NIL); continue; }
        if (!DumpConstant(D, p, &p->k[i])) return false;
    }
    DumpVarInt(D, (uint32_t)p->sizep);
    for (int i = 0; i < p->sizep; i++) DumpVarInt(D, D->protoIds[p->p[i]]);
    DumpVarInt(D, (uint32_t)p->linedefined);
    DumpStringId(D, D->strip ? nullptr : p->debugname);
    if (!D->strip && p->lineinfo && p->sizelineinfo > 0) {
        DumpByte(D, 1);
        DumpByte(D, (uint8_t)p->linegaplog2);
        uint8_t last = 0;
        for (int i = 0; i < p->sizecode; i++) {
            uint8_t current = p->lineinfo[i];
            uint8_t delta = (uint8_t)(current - last);
            DumpByte(D, delta);
            last = current;
        }
        int intervals = ((p->sizecode - 1) >> p->linegaplog2) + 1;
        int lastLine = 0;
        for (int i = 0; i < intervals; i++) {
            int current = p->abslineinfo[i];
            int delta = current - lastLine;
            DumpRaw(D, delta);
            lastLine = current;
        }
    } else {
        DumpByte(D, 0);
    }
    if (!D->strip && (p->sizelocvars > 0 || p->sizeupvalues > 0)) {
        DumpByte(D, 1);
        DumpVarInt(D, (uint32_t)p->sizelocvars);
        for (int i = 0; i < p->sizelocvars; i++) {
            DumpVarInt(D, AddString(D, p->locvars[i].varname));
            DumpVarInt(D, (uint32_t)p->locvars[i].startpc);
            DumpVarInt(D, (uint32_t)p->locvars[i].endpc);
            DumpByte(D, (uint8_t)p->locvars[i].reg);
        }
        DumpVarInt(D, (uint32_t)p->sizeupvalues);
        for (int i = 0; i < p->sizeupvalues; i++)
            DumpVarInt(D, AddString(D, p->upvalues[i]));
    } else {
        DumpByte(D, 0);
    }
    return true;
}

static int luau_dump(int64_t L, const Proto* root, lua_Writer writer, void* data, int strip) {
    LuauDumpState D{};
    D.L = L; D.writer = writer; D.data = data; D.strip = strip; D.status = 0;
    CollectProtoTree(&D, root);
    DumpByte(&D, LBC_VERSION_TARGET);
    DumpByte(&D, LBC_TYPE_VERSION_TARGET);
    DumpStringTable(&D);
    DumpByte(&D, 0);
    DumpVarInt(&D, (uint32_t)D.protos.size());
    for (const Proto* p : D.protos) {
        if (!DumpProto(&D, p)) return 1;
    }
    DumpVarInt(&D, D.protoIds[root]);
    return D.status;
}
