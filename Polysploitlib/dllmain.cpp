#include "pch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <psapi.h>
#include <windows.h>
#include <winhttp.h>
#include "LuauTypes.h"
#include "LuauDumper.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")

#define VERSION "0.1"
#define LUA_GLOBALSINDEX (-10002)
#define LUA_REGISTRYINDEX (-1001000)

// Luau API typedefs
typedef void*(__fastcall* fn_newthread)(void*);
typedef int(__fastcall* fn_load)(void*, const char*, const char*, size_t, int);
typedef int(__fastcall* fn_pcall)(void*, int, int, int);
typedef const char*(__fastcall* fn_tolstring)(void*, int, size_t*);
typedef int(__fastcall* fn_gettop)(void*);
typedef void(__fastcall* fn_settop)(void*, int);
typedef void(__fastcall* fn_rawsetfield)(void*, int, const char*);
typedef void(__fastcall* fn_getfield)(void*, int, const char*);
typedef int(__fastcall* fn_type)(void*, int);
typedef void(__fastcall* fn_pushcclosurek)(void*, void*, const char*, int, void*);
typedef void(__fastcall* fn_pushstring)(void*, const char*);
typedef void(__fastcall* fn_pushnil)(void*);
typedef void(__fastcall* fn_pushlstring)(void*, const char*, size_t);
typedef void(__fastcall* fn_pushboolean)(void*, int);
typedef void(__fastcall* fn_pushinteger)(void*, int);
typedef void(__fastcall* fn_createtable)(void*, int, int);
typedef void(__fastcall* fn_rawseti)(void*, int, int);
typedef void(__fastcall* fn_pushvalue)(void*, int);
typedef int(__fastcall* fn_toboolean)(void*, int);
typedef void(__fastcall* fn_setreadonly)(void*, int, int);
typedef int(__fastcall* fn_getmetatable)(void*, int);
typedef int(__fastcall* fn_setmetatable)(void*, int);
typedef int(__fastcall* fn_resume)(void*, void*, int);
typedef double(__fastcall* fn_tonumber)(void*, int);
typedef void(__fastcall* fn_setfield)(void*, int, const char*);
typedef void(__fastcall* fn_pushnumber)(void*, double);
typedef void(__fastcall* fn_pop)(void*, int);
typedef void*(__fastcall* fn_newuserdata)(void*, size_t);
typedef void*(__fastcall* fn_touserdata)(void*, int);

static void* L_game = nullptr;
static int l_G_off = 0;
static void* saved_frealloc = nullptr;
static void* gl_game = nullptr;

static fn_newthread my_newthread = nullptr;
static fn_load my_load = nullptr;
static fn_pcall my_pcall = nullptr;
static fn_resume my_resume = nullptr;
static fn_tolstring my_tolstring = nullptr;
static fn_gettop my_gettop = nullptr;
static fn_settop my_settop = nullptr;
static fn_getfield my_getfield = nullptr;
static fn_rawsetfield my_rawsetfield = nullptr;
static fn_type my_type = nullptr;
static fn_pushcclosurek my_pushcclosurek = nullptr;
static fn_pushstring my_pushstring = nullptr;
static fn_pushnil my_pushnil = nullptr;
static fn_pushlstring my_pushlstring = nullptr;
static fn_pushboolean my_pushboolean = nullptr;
static fn_pushinteger my_pushinteger = nullptr;
static fn_createtable my_createtable = nullptr;
static fn_rawseti my_rawseti = nullptr;
static fn_pushvalue my_pushvalue = nullptr;
static fn_toboolean my_toboolean = nullptr;
static fn_setreadonly my_setreadonly = nullptr;
static fn_getmetatable my_getmetatable = nullptr;
static fn_setmetatable my_setmetatable = nullptr;
static fn_tonumber my_tonumber = nullptr;
static fn_setfield my_setfield = nullptr;
static fn_pushnumber my_pushnumber = nullptr;
static fn_pop my_pop = nullptr;
static fn_newuserdata my_newuserdata = nullptr;
static fn_touserdata my_touserdata = nullptr;
static std::string workspace_path;

// ---- FPS cap system (D3D12 Present hook) ----
struct FpsHook {
    void* target = nullptr, *detour = nullptr, *trampoline = nullptr;
    uint8_t saved[14]{};
};
static FpsHook g_fpsHook;
static int g_fpsCap = 0;
static double g_fpsLastFrame = 0;
static double g_fpsFreq = 0;

static void* AllocNearFps(void* target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint64_t min = (uint64_t)target - 0x7FFFFFFF, max = (uint64_t)target + 0x7FFFFFFF;
    uint64_t page = (uint64_t)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (page < max) {
        if (VirtualQuery((void*)page, &mbi, sizeof(mbi)) && mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            uint64_t addr = (uint64_t)mbi.BaseAddress;
            if (addr >= min && addr + size <= max) {
                void* a = VirtualAlloc(mbi.BaseAddress, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (a) return a;
            }
        }
        page = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

static bool InstallFpsHook(FpsHook* h, void* target, void* detour) {
    h->target = target; h->detour = detour;
    memcpy(h->saved, target, 14);
    h->trampoline = AllocNearFps(target, 32);
    if (!h->trampoline) return false;
    uint8_t* t = (uint8_t*)h->trampoline;
    memcpy(t, h->saved, 14); t += 14;
    t[0] = 0xFF; t[1] = 0x25; *(uint32_t*)(t + 2) = 0;
    *(uintptr_t*)(t + 6) = (uintptr_t)target + 14;
    DWORD old; VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* d = (uint8_t*)target;
    d[0] = 0xFF; d[1] = 0x25; *(uint32_t*)(d + 2) = 0;
    *(uintptr_t*)(d + 6) = (uintptr_t)detour;
    VirtualProtect(target, 14, old, &old);
    return true;
}

static void UninstallFpsHook(FpsHook* h) {
    if (h->target) {
        DWORD old; VirtualProtect(h->target, 14, PAGE_EXECUTE_READWRITE, &old);
        memcpy(h->target, h->saved, 14);
        VirtualProtect(h->target, 14, old, &old);
    }
    if (h->trampoline) { VirtualFree(h->trampoline, 0, MEM_RELEASE); h->trampoline = nullptr; }
    memset(h, 0, sizeof(*h));
}

static HRESULT STDMETHODCALLTYPE FpsPresentHook(IDXGISwapChain* swap, UINT sync, UINT flags) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_fpsCap > 0 && g_fpsFreq > 0) {
        double frameTime = (now.QuadPart - g_fpsLastFrame) / g_fpsFreq;
        double targetTime = 1.0 / g_fpsCap;
        if (frameTime < targetTime) {
            double ms = (targetTime - frameTime) * 1000.0;
            if (ms > 0.5) Sleep((DWORD)ms);
        }
    }
    g_fpsLastFrame = (double)now.QuadPart;
    auto real = (HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT))g_fpsHook.trampoline;
    return real(swap, sync, flags);
}

static bool InitFpsCap() {
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    g_fpsFreq = (double)freq.QuadPart;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_fpsLastFrame = (double)now.QuadPart;

    HWND wnd = FindWindowW(L"Polytoria", nullptr);
    if (!wnd) wnd = FindWindowW(L"Roblox", nullptr);
    if (!wnd) wnd = GetDesktopWindow();

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) { factory->Release(); return false; }
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)))) { device->Release(); factory->Release(); return false; }
    IDXGISwapChain1* sc = nullptr;
    DXGI_SWAP_CHAIN_DESC1 scdesc = {};
    scdesc.BufferCount = 2;
    scdesc.Width = 1;
    scdesc.Height = 1;
    scdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scdesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scdesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scdesc.SampleDesc.Count = 1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, wnd, &scdesc, nullptr, nullptr, &sc))) {
        queue->Release(); device->Release(); factory->Release(); return false;
    }
    IDXGISwapChain* swap3 = nullptr;
    sc->QueryInterface(IID_PPV_ARGS(&swap3));
    sc->Release();
    if (!swap3) { queue->Release(); device->Release(); factory->Release(); return false; }

    void* vtab = *(void**)swap3;
    void* present = ((void**)vtab)[8];

    bool ok = InstallFpsHook(&g_fpsHook, present, FpsPresentHook);

    swap3->Release(); queue->Release(); device->Release(); factory->Release();
    return ok;
}

static void ShutdownFpsCap() {
    UninstallFpsHook(&g_fpsHook);
}

static int __fastcall setfpscap_func(void* L) {
    g_fpsCap = (int)my_tonumber(L, 1);
    return 0;
}
static int __fastcall getfpscap_func(void* L) {
    my_pushnumber(L, (double)g_fpsCap);
    return 1;
}

// ----- Workspace init & path validation -----
static void init_workspace() {
    wchar_t dll_path[MAX_PATH];
    HMODULE mod;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&init_workspace, &mod)) {
        GetModuleFileNameW(mod, dll_path, MAX_PATH);
        std::filesystem::path p(dll_path);
        workspace_path = p.parent_path().string() + "\\workspace";
        CreateDirectoryA(workspace_path.c_str(), NULL);
    }
}

static bool path_in_workspace(const std::string& sub) {
    namespace fs = std::filesystem;
    if (workspace_path.empty()) return false;
    fs::path base(fs::absolute(workspace_path));
    fs::path target(fs::absolute(base / sub));
    std::string t = target.lexically_normal().string();
    std::string b = base.lexically_normal().string();
    return t.compare(0, b.size(), b) == 0 && (t.size() == b.size() || t[b.size()] == '\\');
}

// ----- Workspace file/folder Lua functions -----
static int __fastcall readfile_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path) { my_pushnil(L); my_pushstring(L, "bad argument #1 (path expected)"); return 2; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    if (!path_in_workspace(std::string(path, len))) { my_pushnil(L); my_pushstring(L, "path traversal blocked"); return 2; }
    HANDLE f = CreateFileA(full.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { my_pushnil(L); my_pushstring(L, "file not found"); return 2; }
    DWORD sz = GetFileSize(f, NULL);
    std::vector<char> buf(sz + 1);
    DWORD rd;
    ReadFile(f, buf.data(), sz, &rd, NULL);
    CloseHandle(f);
    my_pushlstring(L, buf.data(), rd);
    return 1;
}

static int __fastcall writefile_func(void* L) {
    size_t plen, clen;
    const char* path = my_tolstring(L, 1, &plen);
    const char* content = my_tolstring(L, 2, &clen);
    if (!path || !content) { my_pushnil(L); my_pushstring(L, "bad arguments"); return 2; }
    if (!path_in_workspace(std::string(path, plen))) { my_pushnil(L); my_pushstring(L, "path traversal blocked"); return 2; }
    std::string full = workspace_path + "\\" + std::string(path, plen);
    std::filesystem::path p(full);
    CreateDirectoryA(p.parent_path().string().c_str(), NULL);
    HANDLE f = CreateFileA(full.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { my_pushnil(L); my_pushstring(L, "write failed"); return 2; }
    DWORD wr;
    WriteFile(f, content, (DWORD)clen, &wr, NULL);
    CloseHandle(f);
    my_pushboolean ? my_pushboolean(L, 1) : (void)0;
    return 1;
}

static int __fastcall appendfile_func(void* L) {
    size_t plen, clen;
    const char* path = my_tolstring(L, 1, &plen);
    const char* content = my_tolstring(L, 2, &clen);
    if (!path || !content) { my_pushnil(L); my_pushstring(L, "bad arguments"); return 2; }
    if (!path_in_workspace(std::string(path, plen))) { my_pushnil(L); my_pushstring(L, "path traversal blocked"); return 2; }
    std::string full = workspace_path + "\\" + std::string(path, plen);
    std::filesystem::path p(full);
    CreateDirectoryA(p.parent_path().string().c_str(), NULL);
    HANDLE f = CreateFileA(full.c_str(), FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { my_pushnil(L); my_pushstring(L, "append failed"); return 2; }
    DWORD wr;
    WriteFile(f, content, (DWORD)clen, &wr, NULL);
    CloseHandle(f);
    my_pushboolean ? my_pushboolean(L, 1) : (void)0;
    return 1;
}

static int __fastcall isfile_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path || !path_in_workspace(std::string(path, len))) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    DWORD attr = GetFileAttributesA(full.c_str());
    my_pushboolean ? my_pushboolean(L, (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))) : (void)0;
    return 1;
}

static int __fastcall isfolder_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path || !path_in_workspace(std::string(path, len))) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    DWORD attr = GetFileAttributesA(full.c_str());
    my_pushboolean ? my_pushboolean(L, (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))) : (void)0;
    return 1;
}

static int __fastcall makefolder_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    if (!path_in_workspace(std::string(path, len))) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    BOOL ok = CreateDirectoryA(full.c_str(), NULL);
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) ok = TRUE;
    my_pushboolean ? my_pushboolean(L, ok) : (void)0;
    return 1;
}

static int __fastcall delfile_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path || !path_in_workspace(std::string(path, len))) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    BOOL ok = DeleteFileA(full.c_str());
    my_pushboolean ? my_pushboolean(L, ok) : (void)0;
    return 1;
}

static int __fastcall delfolder_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    if (!path || !path_in_workspace(std::string(path, len))) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    std::string full = workspace_path + "\\" + std::string(path, len);
    BOOL ok = RemoveDirectoryA(full.c_str());
    my_pushboolean ? my_pushboolean(L, ok) : (void)0;
    return 1;
}

static int __fastcall listfiles_func(void* L) {
    size_t len;
    const char* path = my_tolstring(L, 1, &len);
    std::string spath = (path && len > 0) ? std::string(path, len) : "";
    if (!path_in_workspace(spath)) { my_pushnil(L); my_pushstring(L, "path traversal blocked"); return 2; }
    std::string full = workspace_path + "\\" + spath;
    if (full.back() != '\\') full += '\\';
    full += "*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(full.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { my_createtable(L, 0, 0); return 1; }
    my_createtable(L, 0, 8);
    int idx = 1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        my_pushstring(L, fd.cFileName);
        my_rawseti(L, -2, idx++);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}

// ----- Clipboard -----
static int __fastcall setclipboard_func(void* L) {
    size_t len;
    const char* text = my_tolstring(L, 1, &len);
    if (!text) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    if (!OpenClipboard(NULL)) { my_pushboolean ? my_pushboolean(L, 0) : (void)0; return 1; }
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (h) {
        memcpy(GlobalLock(h), text, len + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
    my_pushboolean ? my_pushboolean(L, h != NULL) : (void)0;
    return 1;
}

static int __fastcall toclipboard_func(void* L) {
    if (!OpenClipboard(NULL)) { my_pushstring(L, ""); return 1; }
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); my_pushstring(L, ""); return 1; }
    const char* text = (const char*)GlobalLock(h);
    if (text) my_pushstring(L, text);
    else my_pushstring(L, "");
    GlobalUnlock(h);
    CloseClipboard();
    return 1;
}
static int __fastcall BytecodeWriter(int64_t, const void* p, size_t sz, void* ud) {
    std::string* out = (std::string*)ud;
    out->append((const char*)p, sz);
    return 0;
}

static std::string random_filename() {
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string r;
    r.reserve(10);
    for (int i = 0; i < 10; i++) r += alpha[rand() % (sizeof(alpha) - 1)];
    return r + ".luau";
}

// ----- lua_State field offsets (dynamically discovered) -----
static int LUA_STATE_TOP_OFFSET = 16;
static int LUA_STATE_BASE_OFFSET = 24;

static TValue* lua_index2addr(void* L, int idx) {
    TValue* result = NULL;
    __try {
        if (idx == 0) { result = NULL; }
        else {
            TValue* top = *(TValue**)((uintptr_t)L + LUA_STATE_TOP_OFFSET);
            TValue* base = *(TValue**)((uintptr_t)L + LUA_STATE_BASE_OFFSET);
            if (idx > 0) {
                TValue* o = base + (idx - 1);
                result = (o >= top) ? NULL : o;
            } else if (idx > -10002) {
                result = top + idx;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// Dynamically discover lua_State top/base offsets using lua_gettop
static void find_state_offsets(void* L) {
    if (!L || !my_gettop) return;
    int n = my_gettop(L);
    if (n < 0) return;
    int sizes[] = {12, 16, 20, 24, 32};
    for (int si = 0; si < 5; si++) {
        int tsz = sizes[si];
        for (int off = 8; off < 80; off += 8) {
            uintptr_t a = *(uintptr_t*)((uint8_t*)L + off);
            for (int off2 = 8; off2 < 80; off2 += 8) {
                if (off == off2) continue;
                uintptr_t b = *(uintptr_t*)((uint8_t*)L + off2);
                if (a > b && (int)((a - b) / tsz) == n) {
                    LUA_STATE_TOP_OFFSET = off;
                    LUA_STATE_BASE_OFFSET = off2;
                    return;
                }
            }
        }
    }
}

static TValue* luaA_toobject(void* L, int idx) {
    TValue* p = lua_index2addr(L, idx);
    if (p == NULL) return NULL;
    if (p->tt == 0 && p->value.gc == NULL) return NULL;
    return p;
}

// ----- SEH-safe helpers (no C++ objects) -----
static Proto* __fastcall get_closure_proto(TValue* tv, bool* errOut, bool* isCOut) {
    Proto* result = nullptr;
    __try {
        Closure* cl = (Closure*)tv->value.gc;
        *isCOut = cl->isC;
        if (!cl->isC) result = cl->l.p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *errOut = true; }
    return result;
}

static int __fastcall call_luau_dump(const Proto* proto, lua_Writer writer, void* data) {
    int result = -1;
    __try {
        result = luau_dump(0, proto, writer, data, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// ----- decompile(func[, filename]) -----
static int __fastcall decompile_func(void* L) {
    if (workspace_path.empty()) { my_pushnil(L); my_pushstring(L, "workspace not initialized"); return 2; }
    if (workspace_path.back() != '\\') workspace_path += '\\';

    int nargs = my_gettop ? my_gettop(L) : 0;
    if (nargs < 1) { my_pushnil(L); my_pushstring(L, "usage: decompile(func[, filename])"); return 2; }
    int argType = my_type(L, 1);
    if (argType != 8) { my_pushnil(L); my_pushstring(L, "expected function at arg 1"); return 2; }

    TValue* tv = luaA_toobject(L, 1);
    if (!tv || tv->tt != 8) { my_pushnil(L); my_pushstring(L, "cannot read function value"); return 2; }

    bool err = false, isC = false;
    Proto* proto = get_closure_proto(tv, &err, &isC);
    if (err) { my_pushnil(L); my_pushstring(L, "exception reading closure"); return 2; }
    if (isC) { my_pushnil(L); my_pushstring(L, "cannot decompile C function"); return 2; }
    if (!proto) { my_pushnil(L); my_pushstring(L, "function has no proto"); return 2; }

    // Dump bytecode
    std::string bytecode;
    int dump_ok = call_luau_dump(proto, BytecodeWriter, &bytecode);
    if (dump_ok < 0) { my_pushnil(L); my_pushstring(L, "exception in luau_dump"); return 2; }
    if (dump_ok != 0) { my_pushnil(L); my_pushstring(L, "luau_dump failed"); return 2; }

    // Save to workspace
    std::string fname;
    if (nargs >= 2) {
        size_t fnamelen;
        const char* fn = my_tolstring(L, 2, &fnamelen);
        if (fn && fnamelen > 0) fname.assign(fn, fnamelen);
    }
    if (fname.empty()) fname = random_filename();
    if (fname.find('/') != std::string::npos || fname.find('\\') != std::string::npos)
        fname = random_filename();

    std::string fullpath = workspace_path + fname;
    std::ofstream f(fullpath.c_str(), std::ios::binary);
    if (!f.is_open()) { my_pushnil(L); my_pushstring(L, "failed to write file"); return 2; }
    f.write(bytecode.data(), bytecode.size());
    f.close();

    // Try Unluau.CLI.exe
    std::string source_path = fullpath;
    size_t dot = source_path.rfind('.');
    source_path = (dot != std::string::npos) ? source_path.substr(0, dot) + ".lua" : source_path + ".lua";

    std::string unluau_path = workspace_path + "Unluau.CLI.exe";
    DWORD attrs = GetFileAttributesA(unluau_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string cmd = "\"" + unluau_path + "\" \"" + fullpath + "\" \"" + source_path + "\"";
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    // Determine result name
    std::string result;
    attrs = GetFileAttributesA(source_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t pos = source_path.rfind('\\');
        result = (pos != std::string::npos) ? source_path.substr(pos + 1) : source_path;
    } else {
        result = fname;
    }

    my_pushlstring(L, result.data(), result.size());
    return 1;
}

static void* __fastcall my_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    void* p = realloc(ptr, nsize);
    if (p) return p;
    p = VirtualAlloc(NULL, nsize, MEM_COMMIT, PAGE_READWRITE);
    if (ptr && p) { memcpy(p, ptr, osize < nsize ? osize : nsize); free(ptr); }
    return p;
}

// ----- HTTP helper (shared by httpget/httpupload) -----
static int http_request(void* L, const char* method, int method_len) {
    size_t urllen, datalen = 0;
    const char* url_str = my_tolstring(L, 1, &urllen);
    if (!url_str) { my_pushnil(L); my_pushstring(L, "bad argument #1 (url expected)"); return 2; }

    const char* data = NULL;
    if (my_gettop(L) >= 2) {
        data = my_tolstring(L, 2, &datalen);
        if (!data) datalen = 0;
    }

    std::string s(url_str, urllen);
    bool https = s.compare(0, 8, "https://") == 0;
    bool http = !https && s.compare(0, 7, "http://") == 0;
    if (!https && !http) { my_pushnil(L); my_pushstring(L, "invalid URL"); return 2; }

    size_t start = https ? 8 : 7;
    size_t slash = s.find('/', start);
    std::string host = s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    std::string path = slash == std::string::npos ? "/" : s.substr(slash);

    auto to_w = [](const std::string& s) -> std::wstring {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        if (n <= 0) return {};
        std::wstring r(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n);
        return r;
    };

    std::wstring wh = to_w(host), wp = to_w(path);
    if (wh.empty()) { my_pushnil(L); my_pushstring(L, "invalid host"); return 2; }

    std::wstring ua = L"Polysploit/" VERSION;
    std::wstring wmeth(method, method + method_len);

    HINTERNET hSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { my_pushnil(L); my_pushstring(L, "WinHttpOpen failed"); return 2; }

    INTERNET_PORT port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConnect = WinHttpConnect(hSession, wh.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); my_pushnil(L); my_pushstring(L, "WinHttpConnect failed"); return 2; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmeth.c_str(), wp.c_str(), NULL, NULL, NULL, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); my_pushnil(L); my_pushstring(L, "WinHttpOpenRequest failed"); return 2; }

    if (data && datalen > 0) {
        std::wstring ct = std::wstring(L"Content-Type: ") + L"application/octet-stream";
        WinHttpAddRequestHeaders(hRequest, ct.c_str(), (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    std::string response;
    void* body = data ? (void*)data : NULL;
    DWORD bodylen = data ? (DWORD)datalen : 0;
    if (WinHttpSendRequest(hRequest, NULL, 0, body, bodylen, bodylen, NULL) && WinHttpReceiveResponse(hRequest, NULL)) {
        char buf[4096]; DWORD read;
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0)
            response.append(buf, read);
    }

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    my_pushlstring(L, response.data(), response.size());
    return 1;
}

// ----- Custom Lua functions -----
static int __fastcall httpget_func(void* L) { return http_request(L, "GET", 3); }
static int __fastcall httpupload_func(void* L) { return http_request(L, "POST", 4); }

static int __fastcall identifyexecutor_func(void* L) {
    my_pushstring(L, "Polysploit");
    return 1;
}

// ----- Polycheat-style environment functions -----

static int __fastcall getgenv_func(void* L) {
    if (my_pushvalue) my_pushvalue(L, LUA_GLOBALSINDEX);
    else my_pushnil(L);
    return 1;
}

static int __fastcall getrenv_func(void* L) {
    if (my_pushvalue) my_pushvalue(L, LUA_GLOBALSINDEX);
    else my_pushnil(L);
    return 1;
}

static int __fastcall getreg_func(void* L) {
    my_pushnil(L);
    return 1;
}

static int __fastcall getsenv_func(void* L) {
    my_pushnil(L);
    return 1;
}

static int __fastcall setreadonly_func(void* L) {
    int nargs = my_gettop ? my_gettop(L) : 0;
    if (nargs < 2 || !my_setreadonly) return 0;

    int t = my_type(L, 1);
    if (t == 7 || t == 9) {
        int ro = my_toboolean ? my_toboolean(L, 2) : 0;
        my_setreadonly(L, 1, ro);
    }
    my_pushvalue ? my_pushvalue(L, 1) : my_pushnil(L);
    return 1;
}

static int __fastcall make_writeable_func(void* L) {
    if (!my_setreadonly) { my_pushnil(L); return 1; }
    int t = my_type(L, 1);
    if (t != 7 && t != 9) { my_pushnil(L); return 1; }
    my_setreadonly(L, 1, 0);
    my_pushvalue ? my_pushvalue(L, 1) : my_pushnil(L);
    return 1;
}

static int __fastcall make_readonly_func(void* L) {
    if (!my_setreadonly) { my_pushnil(L); return 1; }
    int t = my_type(L, 1);
    if (t != 7 && t != 9) { my_pushnil(L); return 1; }
    my_setreadonly(L, 1, 1);
    my_pushvalue ? my_pushvalue(L, 1) : my_pushnil(L);
    return 1;
}

static int __fastcall hookfunction_func(void* L) {
    my_pushboolean(L, 1);
    return 1;
}

static int __fastcall setrawmetatable_func(void* L) {
    if (!my_setmetatable) { my_pushboolean(L, 0); return 1; }
    int t = my_type(L, 2);
    if (t != 0 && t != 7) { my_pushboolean(L, 0); return 1; }
    int ok = my_setmetatable(L, 1);
    my_pushboolean(L, ok);
    return 1;
}

static int __fastcall getrawmetatable_func(void* L) {
    if (!my_getmetatable) { my_pushnil(L); return 1; }
    if (!my_getmetatable(L, 1)) { my_pushnil(L); }
    return 1;
}

static int __fastcall mouse_move_func(void* L) {
    if (!my_tonumber) return 0;
    int x = (int)my_tonumber(L, 1);
    int y = (int)my_tonumber(L, 2);
    SetCursorPos(x, y);
    return 0;
}

static int __fastcall mouse_get_position_func(void* L) {
    POINT pt;
    GetCursorPos(&pt);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d,%d", pt.x, pt.y);
    my_pushstring(L, buf);
    return 1;
}

static int __fastcall mouse_click_func(void* L) {
    if (!my_tonumber) return 0;
    int button = (int)my_tonumber(L, 1);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[1].type = INPUT_MOUSE;
    switch (button) {
    case 0:
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        break;
    case 2:
        in[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        break;
    }
    SendInput(2, in, sizeof(INPUT));
    return 0;
}

static int __fastcall mouse_scroll_func(void* L) {
    if (!my_tonumber) return 0;
    int amount = (int)my_tonumber(L, 1);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = amount * WHEEL_DELTA;
    SendInput(1, &in, sizeof(INPUT));
    return 0;
}

static int __fastcall mouse_lock_func(void* L) {
    int locked = my_toboolean ? my_toboolean(L, 1) : 0;
    HWND hwnd = FindWindowA("RobloxWindow", NULL);
    if (locked && hwnd) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MapWindowPoints(hwnd, NULL, (POINT*)&rc, 2);
        ClipCursor(&rc);
    } else {
        ClipCursor(NULL);
    }
    return 0;
}

static int __fastcall mouse_visible_func(void* L) {
    int visible = my_toboolean ? my_toboolean(L, 1) : 0;
    if (visible) {
        while (ShowCursor(TRUE) < 0) {}
    } else {
        while (ShowCursor(FALSE) >= 0) {}
    }
    return 0;
}

// ----- Init -----
static void init_exports() {
    HMODULE vm = GetModuleHandleA("Luau.VM.dll");
    if (!vm) return;
    my_newthread = (fn_newthread)GetProcAddress(vm, "lua_newthread");
    my_load = (fn_load)GetProcAddress(vm, "luau_load");
    my_pcall = (fn_pcall)GetProcAddress(vm, "lua_pcall");
    my_resume = (fn_resume)GetProcAddress(vm, "lua_resume");
    my_tolstring = (fn_tolstring)GetProcAddress(vm, "lua_tolstring");
    my_gettop = (fn_gettop)GetProcAddress(vm, "lua_gettop");
    my_settop = (fn_settop)GetProcAddress(vm, "lua_settop");
    my_getfield = (fn_getfield)GetProcAddress(vm, "lua_getfield");
    my_rawsetfield = (fn_rawsetfield)GetProcAddress(vm, "lua_rawsetfield");
    my_type = (fn_type)GetProcAddress(vm, "lua_type");
    my_pushcclosurek = (fn_pushcclosurek)GetProcAddress(vm, "lua_pushcclosurek");
    my_pushstring = (fn_pushstring)GetProcAddress(vm, "lua_pushstring");
    my_pushnil = (fn_pushnil)GetProcAddress(vm, "lua_pushnil");
    my_pushlstring = (fn_pushlstring)GetProcAddress(vm, "lua_pushlstring");
    my_pushboolean = (fn_pushboolean)GetProcAddress(vm, "lua_pushboolean");
    my_pushinteger = (fn_pushinteger)GetProcAddress(vm, "lua_pushinteger");
    my_createtable = (fn_createtable)GetProcAddress(vm, "lua_createtable");
    my_rawseti = (fn_rawseti)GetProcAddress(vm, "lua_rawseti");
    my_pushvalue = (fn_pushvalue)GetProcAddress(vm, "lua_pushvalue");
    my_toboolean = (fn_toboolean)GetProcAddress(vm, "lua_toboolean");
    my_setreadonly = (fn_setreadonly)GetProcAddress(vm, "lua_setreadonly");
    my_getmetatable = (fn_getmetatable)GetProcAddress(vm, "lua_getmetatable");
    my_setmetatable = (fn_setmetatable)GetProcAddress(vm, "lua_setmetatable");
    my_tonumber = (fn_tonumber)GetProcAddress(vm, "lua_tonumber");
my_setfield = (fn_setfield)GetProcAddress(vm, "lua_setfield");
my_pushnumber = (fn_pushnumber)GetProcAddress(vm, "lua_pushnumber");
    my_pop = (fn_pop)GetProcAddress(vm, "lua_settop");
    my_newuserdata = (fn_newuserdata)GetProcAddress(vm, "lua_newuserdata");
    my_touserdata = (fn_touserdata)GetProcAddress(vm, "lua_touserdata");
}

// Find l_G offset by analysing our own throw-away lua_State
static int find_l_G_offset() {
    HMODULE vm = GetModuleHandleA("Luau.VM.dll");
    if (!vm) return 0;
    MODULEINFO vmmi;
    if (!GetModuleInformation(GetCurrentProcess(), vm, &vmmi, sizeof(vmmi))) return 0;
    uintptr_t vm_start = (uintptr_t)vmmi.lpBaseOfDll;
    uintptr_t vm_end = vm_start + vmmi.SizeOfImage;

    auto newstate = (void*(__fastcall*)())GetProcAddress(vm, "luaL_newstate");
    if (!newstate) return 0;
    void* L = newstate();
    if (!L) return 0;

    unsigned char* b = (unsigned char*)L;
    for (int off = 16; off < 256; off += 8) {
        void* gl = *(void**)(b + off);
        if (!gl || (uintptr_t)gl < 0x10000) continue;
        int found = 0;
        __try {
            for (int foff = 0; foff < 80; foff += 8) {
                uintptr_t fn = *(uintptr_t*)((unsigned char*)gl + foff);
                if (fn >= vm_start && fn < vm_end) { found = 1; break; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (found) { l_G_off = off; return 1; }
    }
    return 0;
}

// Scan memory for the game's lua_State (has 'game' global)
static void* find_game_state() {
    if (!l_G_off || !my_type || !my_getfield || !my_settop) return nullptr;
    HMODULE vm = GetModuleHandleA("Luau.VM.dll");
    if (!vm) return nullptr;
    MODULEINFO vmmi;
    if (!GetModuleInformation(GetCurrentProcess(), vm, &vmmi, sizeof(vmmi))) return nullptr;
    uintptr_t vm_start = (uintptr_t)vmmi.lpBaseOfDll;
    uintptr_t vm_end = vm_start + vmmi.SizeOfImage;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t top = (uintptr_t)si.lpMaximumApplicationAddress;

    while (addr < top) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
        addr += mbi.RegionSize;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || (mbi.Protect & PAGE_GUARD)) continue;

        unsigned char* base = (unsigned char*)mbi.BaseAddress;
        SIZE_T size = mbi.RegionSize;
        for (SIZE_T off = 0; off + 16 < size; off += 8) {
            void* maybe = *(void**)(base + off);
            if (!maybe || (uintptr_t)maybe % 8 != 0) continue;
            int found_game = 0;
            __try {
                unsigned char tt = *(unsigned char*)((unsigned char*)maybe + 8);
                if (tt == 8 || tt == 10) {
                    void* gl = *(void**)((unsigned char*)maybe + l_G_off);
                    if (gl && (uintptr_t)gl >= 0x10000) {
                        for (int foff = 0; foff < 80; foff += 8) {
                            uintptr_t fn = *(uintptr_t*)((unsigned char*)gl + foff);
                            if (fn >= vm_start && fn < vm_end) { found_game = 1; break; }
                        }
                        if (found_game) {
                            my_getfield(maybe, LUA_GLOBALSINDEX, "game");
                            int has_game = my_type(maybe, -1) != 0;
                            my_settop(maybe, -2);
                            if (has_game) {
                                gl_game = gl;
                                saved_frealloc = *(void**)((unsigned char*)gl + 0x10);
                                *(void**)((unsigned char*)gl + 0x10) = my_alloc;
                                found_game = 2;
                            }
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (found_game == 2) return maybe;
        }
    }
    return nullptr;
}

// Register our custom functions in the shared globals table
static void register_custom_fns(void* L) {
    auto reg = [&](const char* name, void* fn) {
        my_pushcclosurek(L, fn, name, 0, NULL);
        my_rawsetfield(L, LUA_GLOBALSINDEX, name);
    };
    reg("httpget", (void*)httpget_func);
    reg("httpupload", (void*)httpupload_func);
    reg("identifyexecutor", (void*)identifyexecutor_func);
    reg("setclipboard", (void*)setclipboard_func);
    reg("toclipboard", (void*)toclipboard_func);
    reg("readfile", (void*)readfile_func);
    reg("writefile", (void*)writefile_func);
    reg("appendfile", (void*)appendfile_func);
    reg("isfile", (void*)isfile_func);
    reg("isfolder", (void*)isfolder_func);
    reg("makefolder", (void*)makefolder_func);
    reg("delfile", (void*)delfile_func);
    reg("delfolder", (void*)delfolder_func);
    reg("listfiles", (void*)listfiles_func);
    reg("decompile", (void*)decompile_func);
    reg("getgenv", (void*)getgenv_func);
    reg("getrenv", (void*)getrenv_func);
    reg("getreg", (void*)getreg_func);
    reg("getsenv", (void*)getsenv_func);
    reg("setreadonly", (void*)setreadonly_func);
    reg("make_writeable", (void*)make_writeable_func);
    reg("make_readonly", (void*)make_readonly_func);
    reg("hookfunction", (void*)hookfunction_func);
    reg("setrawmetatable", (void*)setrawmetatable_func);
    reg("getrawmetatable", (void*)getrawmetatable_func);
    reg("mouse_move", (void*)mouse_move_func);
    reg("mouse_get_position", (void*)mouse_get_position_func);
    reg("mouse_click", (void*)mouse_click_func);
    reg("mouse_scroll", (void*)mouse_scroll_func);
    reg("mouse_lock", (void*)mouse_lock_func);
    reg("mouse_visible", (void*)mouse_visible_func);
    reg("setfpscap", (void*)setfpscap_func);
    reg("getfpscap", (void*)getfpscap_func);

    // Create executor-global table _SHARED
    my_createtable(L, 0, 0);
    my_rawsetfield(L, LUA_GLOBALSINDEX, "_SHARED");
    my_pushstring(L, "Polysploit v" VERSION);
    my_rawsetfield(L, LUA_GLOBALSINDEX, "_VERSION");
}

// Safe unload: restore original allocator + nil our globals
static void cleanup() {
    if (gl_game && saved_frealloc) {
        *(void**)((unsigned char*)gl_game + 0x10) = saved_frealloc;
    }
    ShutdownFpsCap();
    if (L_game && my_pushnil && my_rawsetfield) {
        for (auto name : { "httpget", "httpupload", "identifyexecutor", "setclipboard", "toclipboard",
            "readfile", "writefile", "appendfile", "isfile", "isfolder",
            "makefolder", "delfile", "delfolder", "listfiles", "decompile",
            "getgenv", "getrenv", "getreg", "getsenv",
            "setreadonly", "make_writeable", "make_readonly",
            "hookfunction", "setrawmetatable", "getrawmetatable",
            "mouse_move", "mouse_get_position", "mouse_click", "mouse_scroll",
            "mouse_lock", "mouse_visible",
            "setfpscap", "getfpscap",
            "_SHARED", "_VERSION",
            }) {
            my_pushnil(L_game);
            my_rawsetfield(L_game, LUA_GLOBALSINDEX, name);
        }
    }
}

// ----- Exported interface -----
struct LuaParams {
    const char* bytecode;
    int bytecode_len;
    char* result_buf;
    int result_max;
};

extern "C" __declspec(dllexport) int ExecuteLua(LuaParams* p) {
    if (!p) return -1;
    if (!L_game) {
        init_exports();
        if (!my_load || !my_resume) { snprintf(p->result_buf, p->result_max, "FAIL exports"); return -1; }
        if (!find_l_G_offset()) { snprintf(p->result_buf, p->result_max, "FAIL offsets"); return -1; }
        L_game = find_game_state();
        if (!L_game) { snprintf(p->result_buf, p->result_max, "FAIL no state"); return -1; }
        find_state_offsets(L_game);
        init_workspace();
        register_custom_fns(L_game);
        // FPS cap hook temporarily disabled
        // InitFpsCap();
    } else if (my_pushcclosurek && my_rawsetfield && my_tonumber && my_pushnumber) {
        // Re-injection: register FPS cap functions without full init
        auto reg = [&](const char* name, void* fn) {
            my_pushcclosurek(L_game, fn, name, 0, NULL);
            my_rawsetfield(L_game, LUA_GLOBALSINDEX, name);
        };
        reg("setfpscap", (void*)setfpscap_func);
        reg("getfpscap", (void*)getfpscap_func);
    }

    int saved = my_gettop ? my_gettop(L_game) : 0;

    void* T = my_newthread(L_game);
    if (!T) { my_settop(L_game, saved); snprintf(p->result_buf, p->result_max, "FAIL newthread"); return -1; }
    my_settop(L_game, saved);

    int r = my_load(T, "=s", p->bytecode, p->bytecode_len, 0);
    if (r != 0) {
        const char* err = my_tolstring ? my_tolstring(T, -1, NULL) : NULL;
        if (err && err[0]) snprintf(p->result_buf, p->result_max, "FAIL load %s", err);
        else snprintf(p->result_buf, p->result_max, "FAIL load %d", r);
        return -1;
    }

    // Execute via resume so yields are allowed
    r = my_resume(T, NULL, 0);
    if (r == 0) {
        // Completed successfully, read result from T's stack
        int nret = my_gettop ? my_gettop(T) : 0;
        if (nret > 0) {
            size_t result_len;
            const char* result_str = my_tolstring ? my_tolstring(T, -1, &result_len) : NULL;
            if (result_str && result_len > 0) {
                int max = p->result_max - 1;
                if ((int)result_len > max) result_len = max;
                memcpy(p->result_buf, result_str, result_len);
                p->result_buf[result_len] = 0;
            } else snprintf(p->result_buf, p->result_max, "OK");
        } else snprintf(p->result_buf, p->result_max, "OK");
    } else if (r == 1) {
        // Code yielded � resume until done
        while (r == 1) r = my_resume(T, NULL, 0);
        if (r == 0) snprintf(p->result_buf, p->result_max, "OK");
        else {
            const char* err = my_tolstring ? my_tolstring(T, -1, NULL) : NULL;
            if (err && err[0]) snprintf(p->result_buf, p->result_max, "FAIL %s", err);
            else snprintf(p->result_buf, p->result_max, "FAIL resume %d", r);
        }
    } else {
        const char* err = my_tolstring ? my_tolstring(T, -1, NULL) : NULL;
        if (err && err[0]) snprintf(p->result_buf, p->result_max, "FAIL %s", err);
        else snprintf(p->result_buf, p->result_max, "FAIL resume %d", r);
    }
    return 0;
}

// Called by injector via CreateRemoteThread to trigger safe unload
extern "C" __declspec(dllexport) int Unload() {
    cleanup();
    HMODULE self;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&Unload, &self)) {
        FreeLibrary(self);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_DETACH) cleanup();
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(m);
    return TRUE;
}
