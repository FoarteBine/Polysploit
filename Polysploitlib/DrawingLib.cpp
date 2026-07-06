#include "pch.h"
#include "DrawingLib.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
#include <d3dcompiler.h>

#define LUA_GLOBALSINDEX (-10002)
#define LUA_REGISTRYINDEX (-10000)

// ---- Shape storage ----
std::vector<ShapeLine*> DrawStorage::Lines;
std::vector<ShapeSquare*> DrawStorage::Squares;
std::vector<ShapeCircle*> DrawStorage::Circles;
std::vector<ShapeTriangle*> DrawStorage::Triangles;
std::vector<ShapeText*> DrawStorage::Texts;
HWND DrawingAPI::GameWindow = nullptr;

// ---- Inline hook helpers ----
struct Hook {
    void* target = nullptr, *detour = nullptr, *trampoline = nullptr;
    uint8_t saved[14]{};
};

static void* AllocNear(void* target, size_t size) {
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

static bool InstallHook(Hook* h, void* target, void* detour) {
    h->target = target; h->detour = detour;
    memcpy(h->saved, target, 14);
    h->trampoline = AllocNear(target, 32);
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

static void UninstallHook(Hook* h) {
    if (h->target) {
        DWORD old; VirtualProtect(h->target, 14, PAGE_EXECUTE_READWRITE, &old);
        memcpy(h->target, h->saved, 14);
        VirtualProtect(h->target, 14, old, &old);
    }
    if (h->trampoline) { VirtualFree(h->trampoline, 0, MEM_RELEASE); h->trampoline = nullptr; }
    memset(h, 0, sizeof(*h));
}

// ---- D3D12 hook state ----
static Hook g_presentHook, g_execHook, g_resizeHook;
static ID3D12Device* g_device = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr, *g_srvHeap = nullptr;
static ID3D12GraphicsCommandList* g_cl = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE g_fenceEvent = nullptr;
static UINT64 g_fenceVal = 0;
static UINT g_bufCount = 0, g_rtvSize = 0;
static bool g_ready = false;
static WNDPROC g_oldWndProc = nullptr;

struct FrameCtx {
    ID3D12CommandAllocator* Alloc = nullptr;
    ID3D12Resource* RT = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE RTDesc{};
    INT64 FenceVal = 0;
};
static FrameCtx* g_frames = nullptr;

// ---- Forward declarations ----
static void RenderImGui();
static void InitImGui(IDXGISwapChain3* swap);
static void CleanupImGui();

// ---- D3D12 hook functions ----
static void __stdcall ExecHook(ID3D12CommandQueue* q, UINT cnt, ID3D12CommandList* const* lists) {
    if (!g_queue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_queue = q; g_queue->AddRef();
    }
    ((void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*))g_execHook.trampoline)(q, cnt, lists);
}

static HRESULT __stdcall ResizeHook(IDXGISwapChain3* swap, UINT cnt, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    CleanupImGui();
    HRESULT hr = ((HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT))g_resizeHook.trampoline)(swap, cnt, w, h, fmt, flags);
    if (SUCCEEDED(hr)) InitImGui(swap);
    return hr;
}

static HRESULT __stdcall PresentHook(IDXGISwapChain3* swap, UINT sync, UINT flags) {
    if (!g_ready) InitImGui(swap);
    if (g_ready) {
        UINT idx = swap->GetCurrentBackBufferIndex();
        FrameCtx& f = g_frames[idx];
        if (f.FenceVal && g_fence->GetCompletedValue() < (UINT64)f.FenceVal) {
            g_fence->SetEventOnCompletion(f.FenceVal, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, INFINITE);
        }
        f.Alloc->Reset();
        g_cl->Reset(f.Alloc, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.RT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cl->ResourceBarrier(1, &barrier);
        g_cl->OMSetRenderTargets(1, &f.RTDesc, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
        g_cl->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderImGui();
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cl);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cl->ResourceBarrier(1, &barrier);
        g_cl->Close();
        ID3D12CommandList* cmds[] = { g_cl };
        g_queue->ExecuteCommandLists(1, cmds);
        g_fenceVal++;
        g_queue->Signal(g_fence, g_fenceVal);
        f.FenceVal = g_fenceVal;
    }
    return ((HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT))g_presentHook.trampoline)(swap, sync, flags);
}

static LRESULT IMGUI_IMPL_API WndProcHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_ready && ImGui::GetCurrentContext()) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    return CallWindowProc(g_oldWndProc, hwnd, msg, wp, lp);
}

// ---- Init ImGui ----
static void WaitGPU() {
    if (!g_queue || !g_fence || !g_fenceEvent) return;
    g_fenceVal++;
    if (SUCCEEDED(g_queue->Signal(g_fence, g_fenceVal)) && g_fence->GetCompletedValue() < g_fenceVal) {
        g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

static void CleanupImGui() {
    if (!g_ready) return;
    WaitGPU();
    ImGui_ImplDX12_Shutdown(); ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (g_cl) { g_cl->Release(); g_cl = nullptr; }
    if (g_frames) {
        for (UINT i = 0; i < g_bufCount; i++) {
            if (g_frames[i].RT) { g_frames[i].RT->Release(); g_frames[i].RT = nullptr; }
            if (g_frames[i].Alloc) { g_frames[i].Alloc->Release(); g_frames[i].Alloc = nullptr; }
            g_frames[i].FenceVal = 0;
        }
        delete[] g_frames; g_frames = nullptr;
    }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    g_ready = false;
}

static void InitImGui(IDXGISwapChain3* swap) {
    if (g_ready || !g_queue) return;
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc))) return;
    g_bufCount = desc.BufferCount;
    DrawingAPI::GameWindow = desc.OutputWindow;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&g_device)))) return;
    g_frames = new FrameCtx[g_bufCount];
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = g_bufCount;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) return;
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = g_bufCount + 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_bufCount; i++) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].Alloc)))) return;
        if (FAILED(swap->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].RT)))) return;
        g_device->CreateRenderTargetView(g_frames[i].RT, nullptr, rtvH);
        g_frames[i].RTDesc = rtvH;
        rtvH.ptr += g_rtvSize;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].Alloc, nullptr, IID_PPV_ARGS(&g_cl)))) return;
    g_cl->Close();
    if (!g_fence && FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return;
    if (!g_fenceEvent) { g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); if (!g_fenceEvent) return; }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(DrawingAPI::GameWindow);
    ImGui_ImplDX12_Init(g_device, g_bufCount, DXGI_FORMAT_R8G8B8A8_UNORM, g_srvHeap,
        g_srvHeap->GetCPUDescriptorHandleForHeapStart(), g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    ImGui_ImplDX12_CreateDeviceObjects();
    g_oldWndProc = (WNDPROC)SetWindowLongPtrW(DrawingAPI::GameWindow, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    g_ready = true;
}

// ---- Get VMTs via temporary D3D12 objects ----
static bool GetD3D12Addrs(void** outPresent, void** outExec, void** outResize) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProc, 0, 0, GetModuleHandleW(nullptr), 0,0,0,0, L"TMP_D3D12", nullptr };
    RegisterClassExW(&wc);
    HWND hw = CreateWindowW(L"TMP_D3D12", 0, WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, wc.hInstance, 0);
    if (!hw) return false;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll"), dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!d3d12 || !dxgi) { DestroyWindow(hw); return false; }
    auto createFactory = (decltype(&CreateDXGIFactory))GetProcAddress(dxgi, "CreateDXGIFactory");
    auto createDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!createFactory || !createDevice) { DestroyWindow(hw); return false; }
    IDXGIFactory* fac = nullptr;
    if (FAILED(createFactory(IID_PPV_ARGS(&fac)))) { DestroyWindow(hw); return false; }
    IDXGIAdapter* ada = nullptr; fac->EnumAdapters(0, &ada);
    if (!ada) { fac->Release(); DestroyWindow(hw); return false; }
    ID3D12Device* dev = nullptr;
    if (FAILED(createDevice(ada, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { ada->Release(); fac->Release(); DestroyWindow(hw); return false; }
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { dev->Release(); ada->Release(); fac->Release(); DestroyWindow(hw); return false; }
    ID3D12CommandAllocator* alc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alc));
    if (alc) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alc, nullptr, IID_PPV_ARGS(&cl));
    if (cl) cl->Close();
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2; scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.OutputWindow = hw;
    scd.SampleDesc.Count = 1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(fac->CreateSwapChain(q, &scd, &sc))) {
        if (cl) cl->Release(); if (alc) alc->Release(); q->Release(); dev->Release(); ada->Release(); fac->Release(); DestroyWindow(hw); return false;
    }
    void** qvt = *(void***)q, **svt = *(void***)sc;
    *outExec = qvt[10]; *outPresent = svt[8]; *outResize = svt[13];
    sc->Release(); if (cl) cl->Release(); if (alc) alc->Release();
    q->Release(); dev->Release(); ada->Release(); fac->Release(); DestroyWindow(hw);
    return true;
}

// ---- Rendering ----
static ImColor ToImC(const Color3& c, float a) {
    float r = c.r, g = c.g, b = c.b;
    if (r > 1 || g > 1 || b > 1) { r /= 255; g /= 255; b /= 255; }
    return ImColor(r, g, b, a);
}

static void RenderImGui() {
    auto dl = ImGui::GetBackgroundDrawList();
    for (auto* l : DrawStorage::Lines) {
        if (!l->Visible) continue;
        dl->AddLine({l->From.x, l->From.y}, {l->To.x, l->To.y}, ToImC(l->Color, l->Transparency), l->Thickness);
    }
    for (auto* s : DrawStorage::Squares) {
        if (!s->Visible) continue;
        ImVec2 mn = {s->Position.x, s->Position.y}, mx = {s->Position.x + s->Size.x, s->Position.y + s->Size.y};
        auto c = ToImC(s->Color, s->Transparency);
        if (s->Filled) dl->AddRectFilled(mn, mx, c, s->Rounding);
        else dl->AddRect(mn, mx, c, s->Rounding, 15, s->Thickness);
    }
    for (auto* c : DrawStorage::Circles) {
        if (!c->Visible) continue;
        auto col = ToImC(c->Color, c->Transparency);
        ImVec2 p = {c->Position.x, c->Position.y};
        if (c->Filled) dl->AddCircleFilled(p, c->Radius, col, (int)c->NumSides);
        else dl->AddCircle(p, c->Radius, col, (int)c->NumSides, c->Thickness);
    }
    for (auto* t : DrawStorage::Triangles) {
        if (!t->Visible) continue;
        auto col = ToImC(t->Color, t->Transparency);
        ImVec2 a = {t->P1.x, t->P1.y}, b = {t->P2.x, t->P2.y}, cc = {t->P3.x, t->P3.y};
        if (t->Filled) dl->AddTriangleFilled(a, b, cc, col);
        else dl->AddTriangle(a, b, cc, col, t->Thickness);
    }
    for (auto* t : DrawStorage::Texts) {
        if (!t->Visible) continue;
        auto col = ToImC(t->Color, t->Transparency);
        ImVec2 p = {t->Position.x, t->Position.y};
        dl->AddText(nullptr, t->Size, p, col, t->Text.c_str(), t->Text.c_str() + t->Text.size());
        ImVec2 bnd = ImGui::CalcTextSize(t->Text.c_str(), t->Text.c_str() + t->Text.size());
        t->TextBounds = {bnd.x, bnd.y};
    }
}

// ---- Public API ----
bool DrawingAPI::Init() {
    void *pa, *ea, *ra;
    if (!GetD3D12Addrs(&pa, &ea, &ra)) return false;
    InstallHook(&g_execHook, ea, ExecHook);
    InstallHook(&g_presentHook, pa, PresentHook);
    InstallHook(&g_resizeHook, ra, ResizeHook);
    return true;
}

bool DrawingAPI::Shutdown() {
    // Clear shapes
    for (auto* p : DrawStorage::Lines) delete p; DrawStorage::Lines.clear();
    for (auto* p : DrawStorage::Squares) delete p; DrawStorage::Squares.clear();
    for (auto* p : DrawStorage::Circles) delete p; DrawStorage::Circles.clear();
    for (auto* p : DrawStorage::Triangles) delete p; DrawStorage::Triangles.clear();
    for (auto* p : DrawStorage::Texts) delete p; DrawStorage::Texts.clear();

    CleanupImGui();
    if (g_oldWndProc && GameWindow) {
        SetWindowLongPtrW(GameWindow, GWLP_WNDPROC, (LONG_PTR)g_oldWndProc);
        g_oldWndProc = nullptr;
    }
    UninstallHook(&g_presentHook); UninstallHook(&g_execHook); UninstallHook(&g_resizeHook);
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    g_fenceVal = 0; g_bufCount = 0; g_rtvSize = 0; GameWindow = nullptr;
    return true;
}
