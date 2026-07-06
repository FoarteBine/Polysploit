#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <sstream>
#include <regex>
#include <windows.h>
#include <tlhelp32.h>

typedef char* (*luau_compile_fn)(const char* source, size_t sourceSize, void* options, size_t* outSize);

struct TargetClient {
    DWORD pid;
    bool attached;
    char name[64];
};

std::vector<TargetClient> clients;
luau_compile_fn luau_compile = nullptr;
HMODULE compiler_dll = nullptr;

static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return s.substr(0, pos + 1);
}

static std::wstring get_process_dir(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    MODULEENTRY32W me;
    me.dwSize = sizeof(MODULEENTRY32W);
    std::wstring dir;
    if (Module32FirstW(snap, &me))
        dir = me.szExePath;
    CloseHandle(snap);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return dir.substr(0, pos + 1);
}

static uintptr_t get_remote_module_base(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me;
    me.dwSize = sizeof(MODULEENTRY32W);
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) {
                base = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static bool inject_dll(DWORD pid) {
    std::wstring dll_path = get_exe_dir() + L"Polysploitlib.dll";
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) return false;

    size_t path_size = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_mem = VirtualAllocEx(proc, nullptr, path_size, MEM_COMMIT, PAGE_READWRITE);
    if (!remote_mem) { CloseHandle(proc); return false; }
    WriteProcessMemory(proc, remote_mem, dll_path.c_str(), path_size, nullptr);

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadlib, remote_mem, 0, nullptr);
    if (!thread) { VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE); CloseHandle(proc); return false; }
    WaitForSingleObject(thread, INFINITE);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);
    return true;
}

static uintptr_t get_export_addr(DWORD pid, const char* export_name) {
    uintptr_t remote_base = get_remote_module_base(pid, L"Polysploitlib.dll");
    if (!remote_base) return 0;

    std::wstring dll_path = get_exe_dir() + L"Polysploitlib.dll";
    HMODULE local = LoadLibraryW(dll_path.c_str());
    if (!local) return 0;
    FARPROC local_fn = GetProcAddress(local, export_name);
    if (!local_fn) { FreeLibrary(local); return 0; }
    uintptr_t rva = (uintptr_t)local_fn - (uintptr_t)local;
    FreeLibrary(local);
    return remote_base + rva;
}

static uintptr_t get_execute_lua_addr(DWORD pid) {
    return get_export_addr(pid, "ExecuteLua");
}

static bool init_compiler(DWORD pid) {
    if (luau_compile) return true;

    std::wstring paths[] = { get_exe_dir(), get_process_dir(pid) };
    for (auto& dir : paths) {
        if (dir.empty()) continue;
        std::wstring dll_path = dir + L"Luau.Compiler.dll";
        compiler_dll = LoadLibraryW(dll_path.c_str());
        if (compiler_dll) {
            luau_compile = (luau_compile_fn)GetProcAddress(compiler_dll, "luau_compile");
            if (luau_compile) return true;
            FreeLibrary(compiler_dll);
            compiler_dll = nullptr;
        }
    }
    return false;
}

static void get_clients_list() {
    std::map<DWORD, std::string> old_names;
    for (auto& c : clients) if (c.name[0]) old_names[c.pid] = c.name;
    clients.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            if (name.find(L"Polytoria") != std::wstring::npos || name.find(L"Roblox") != std::wstring::npos) {
                TargetClient tc;
                tc.pid = pe.th32ProcessID;
                tc.attached = get_remote_module_base(pe.th32ProcessID, L"Polysploitlib.dll") != 0;
                tc.name[0] = 0;
                auto it = old_names.find(tc.pid);
                if (it != old_names.end() && it->second.size() < sizeof(tc.name))
                    memcpy(tc.name, it->second.c_str(), it->second.size() + 1);
                clients.push_back(tc);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Run bytecode via ExecuteLua export, return result string
static bool execute_bytecode(DWORD pid, const char* bytecode, size_t bc_size, char* result_buf, int result_max) {
    uintptr_t exec_addr = get_execute_lua_addr(pid);
    if (!exec_addr) return false;

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    size_t total_size = bc_size + result_max;
    void* remote_mem = VirtualAllocEx(proc, nullptr, total_size, MEM_COMMIT, PAGE_READWRITE);
    if (!remote_mem) { CloseHandle(proc); return false; }

    void* result_ptr = (char*)remote_mem + bc_size;
    WriteProcessMemory(proc, remote_mem, bytecode, bc_size, nullptr);

    struct RemoteParams {
        const char* bytecode;
        int bytecode_len;
        char* result_buf;
        int result_max;
    };
    RemoteParams params = { (const char*)remote_mem, (int)bc_size, (char*)result_ptr, result_max };

    void* params_mem = VirtualAllocEx(proc, nullptr, sizeof(params), MEM_COMMIT, PAGE_READWRITE);
    if (!params_mem) { VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE); CloseHandle(proc); return false; }
    WriteProcessMemory(proc, params_mem, &params, sizeof(params), nullptr);

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)exec_addr, params_mem, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(proc, params_mem, 0, MEM_RELEASE);
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    ReadProcessMemory(proc, result_ptr, result_buf, result_max - 1, nullptr);
    result_buf[result_max - 1] = 0;

    VirtualFreeEx(proc, params_mem, 0, MEM_RELEASE);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

void cmd_list() {
    get_clients_list();
    for (auto& c : clients)
        std::cout << "PID: " << c.pid << " | Attached: " << (c.attached ? "YES" : "NO")
                  << " | Player: " << (c.name[0] ? c.name : "UNKNOWN") << "\n";
    if (clients.empty())
        std::cout << "No running clients found.\n";
}

void cmd_attach() {
    get_clients_list();
    int attached = 0;
    for (auto& c : clients) {
        if (c.attached) continue;
        if (inject_dll(c.pid)) {
            c.attached = true;
            std::cout << "PID: " << c.pid << " attached\n";
            attached++;
        }
    }
    if (attached == 0) std::cout << "No new clients attached.\n";
}

void cmd_terminate() {
    for (auto& c : clients) {
        HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, c.pid);
        if (proc) { TerminateProcess(proc, 0); CloseHandle(proc); }
    }
    clients.clear();
    std::cout << "All clients terminated.\n";
}

// Transform loadstring("code")() into local function/end + call
static std::string transformLuaCode(const std::string& source) {
    std::regex pattern(R"(loadstring\s*\(\s*(['"])(.*?)\1\s*\)\s*\(\s*\))");
    std::string replacement = "(function()\n\t$2\nend)()";
    return std::regex_replace(source, pattern, replacement);
}

void cmd_execute(DWORD pid, const std::string& script) {
    bool attached = false;
    for (auto& c : clients)
        if (c.pid == pid && c.attached) { attached = true; break; }
    if (!attached) { std::cout << "Client not attached.\n"; return; }
    if (!init_compiler(pid)) { std::cout << "Compiler failed.\n"; return; }

    std::string transformed = transformLuaCode(script);
    size_t bc_size = 0;
    char* bytecode = luau_compile(transformed.c_str(), transformed.length(), nullptr, &bc_size);
    if (!bytecode || bc_size == 0) { std::cout << "Compilation failed.\n"; return; }

    char result[256] = {};
    bool ok = execute_bytecode(pid, bytecode, bc_size, result, sizeof(result));
    free(bytecode);
    if (!ok) { std::cout << "ExecuteLua failed.\n"; return; }

    std::cout << result << "\n";
}

static uintptr_t get_unload_addr(DWORD pid) {
    return get_export_addr(pid, "Unload");
}

void cmd_unload() {
    for (auto& c : clients) {
        if (!c.attached) continue;
        uintptr_t remote_base = get_remote_module_base(c.pid, L"Polysploitlib.dll");
        if (!remote_base) { std::cout << "PID: " << c.pid << " not found.\n"; continue; }
        HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION, FALSE, c.pid);
        if (!proc) { std::cout << "PID: " << c.pid << " OpenProcess failed.\n"; continue; }
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        LPTHREAD_START_ROUTINE freeLib = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "FreeLibrary");
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, freeLib, (LPVOID)remote_base, 0, nullptr);
        if (!thread) { CloseHandle(proc); std::cout << "PID: " << c.pid << " CreateRemoteThread failed.\n"; continue; }
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        CloseHandle(proc);
        c.attached = false;
        std::cout << "PID: " << c.pid << " unloaded.\n";
    }
}

int main() {
    std::cout << "Polysploit Injector (internal mode)\n";
    std::cout << "Commands: list, attach, terminate, unload, execute <pid> <code>\n";

    // Create workspace folder next to injector
    std::wstring exe_dir = get_exe_dir();
    if (!exe_dir.empty()) {
        std::wstring ws = exe_dir + L"workspace";
        if (CreateDirectoryW(ws.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
            std::wcout << L"Workspace: " << ws << L"\n";
        else
            std::wcout << L"Failed to create workspace: " << ws << L"\n";
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "list") cmd_list();
        else if (cmd == "attach") cmd_attach();
        else if (cmd == "terminate") cmd_terminate();
        else if (cmd == "execute") {
            DWORD pid;
            iss >> pid;
            std::string rest;
            std::getline(iss, rest);
            if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
            cmd_execute(pid, rest);
        }
        else if (cmd == "unload") cmd_unload();
        else std::cout << "Unknown command.\n";
    }

    if (compiler_dll) FreeLibrary(compiler_dll);
    return 0;
}
