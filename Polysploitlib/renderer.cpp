#include "pch.h"
#include "renderer.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

Renderer& Renderer::instance() {
    static Renderer inst;
    return inst;
}

HWND Renderer::find_game_window() {
    DWORD pid = GetCurrentProcessId();
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD wpid;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == *(DWORD*)lParam && IsWindowVisible(hwnd)) {
            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            if (strlen(title) > 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&result);
    return result;
}

DWORD WINAPI Renderer::thread_proc(LPVOID param) {
    Renderer* self = (Renderer*)param;
    self->game_hwnd_ = find_game_window();
    if (!self->game_hwnd_) return 0;

    self->create_overlay();
    if (!self->overlay_hwnd_) return 0;

    SetTimer(self->overlay_hwnd_, 1, 16, timer_proc);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DestroyWindow(self->overlay_hwnd_);
    self->overlay_hwnd_ = NULL;
    return 0;
}

LRESULT CALLBACK Renderer::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void CALLBACK Renderer::timer_proc(HWND, UINT, UINT_PTR, DWORD) {
    instance().render_frame();
}

void Renderer::create_overlay() {
    const wchar_t CLASS_NAME[] = L"PolysploitOverlay";

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassEx(&wc);

    RECT rc;
    GetClientRect(game_hwnd_, &rc);
    POINT pt = {0, 0};
    ClientToScreen(game_hwnd_, &pt);

    overlay_hwnd_ = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        CLASS_NAME, L"", WS_POPUP,
        pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, wc.hInstance, NULL
    );

    if (overlay_hwnd_) {
        ShowWindow(overlay_hwnd_, SW_SHOW);
        SetWindowPos(overlay_hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static Gdiplus::Color make_color(float r, float g, float b, float a) {
    return Gdiplus::Color((BYTE)(int)a, (BYTE)(int)r, (BYTE)(int)g, (BYTE)(int)b);
}

void Renderer::render_frame() {
    if (!game_hwnd_ || !overlay_hwnd_) return;
    if (!IsWindowVisible(game_hwnd_)) return;

    RECT rc;
    GetClientRect(game_hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    POINT pt = {0, 0};
    ClientToScreen(game_hwnd_, &pt);
    SetWindowPos(overlay_hwnd_, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPV5HEADER bi = { sizeof(BITMAPV5HEADER) };
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hBitmap) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);
    memset(bits, 0, (size_t)w * h * 4);

    std::vector<DrawCommand> cmds;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cmds.swap(cmds_);
    }

    if (!cmds.empty()) {
        Gdiplus::Graphics g(hdcMem);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        Gdiplus::Font font(L"Arial", 12);

        for (auto& c : cmds) {
            Gdiplus::Color color = make_color(c.r, c.g, c.b, c.a);

            switch (c.type) {
            case DrawLine: {
                Gdiplus::Pen pen(color, c.thickness > 0 ? c.thickness : 1.0f);
                g.DrawLine(&pen, c.x1, c.y1, c.x2, c.y2);
                break;
            }
            case DrawRect: {
                Gdiplus::RectF rect(c.x1, c.y1, c.x2, c.y2);
                if (c.filled) {
                    Gdiplus::SolidBrush brush(color);
                    g.FillRectangle(&brush, rect);
                } else {
                    Gdiplus::Pen pen(color, c.thickness > 0 ? c.thickness : 1.0f);
                    g.DrawRectangle(&pen, rect.X, rect.Y, rect.Width, rect.Height);
                }
                break;
            }
            case DrawCircle: {
                float d = c.radius * 2;
                Gdiplus::RectF rect(c.x1 - c.radius, c.y1 - c.radius, d, d);
                if (c.filled) {
                    Gdiplus::SolidBrush brush(color);
                    g.FillEllipse(&brush, rect);
                } else {
                    Gdiplus::Pen pen(color, c.thickness > 0 ? c.thickness : 1.0f);
                    g.DrawEllipse(&pen, rect);
                }
                break;
            }
            case DrawText: {
                int n = MultiByteToWideChar(CP_UTF8, 0, c.text.c_str(), (int)c.text.size(), NULL, 0);
                if (n > 0) {
                    std::wstring ws(n, 0);
                    MultiByteToWideChar(CP_UTF8, 0, c.text.c_str(), (int)c.text.size(), &ws[0], n);
                    Gdiplus::SolidBrush brush(color);
                    g.DrawString(ws.c_str(), n, &font, Gdiplus::PointF(c.x1, c.y1), &brush);
                }
                break;
            }
            case DrawClear:
                break;
            }
        }
    }

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    SIZE sz = { w, h };
    POINT pos = { pt.x, pt.y };
    POINT src = { 0, 0 };
    UpdateLayeredWindow(overlay_hwnd_, hdcScreen, &pos, &sz, hdcMem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void Renderer::queue(const DrawCommand& cmd) {
    std::lock_guard<std::mutex> lock(mtx_);
    cmds_.push_back(cmd);
}

void Renderer::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    cmds_.clear();
}

void Renderer::init() {
    if (thread_) return;

    Gdiplus::GdiplusStartupInput gsi;
    GdiplusStartup(&gdi_token_, &gsi, NULL);

    thread_ = CreateThread(NULL, 0, thread_proc, this, 0, NULL);
}

void Renderer::shutdown() {
    if (overlay_hwnd_) {
        PostMessage(overlay_hwnd_, WM_CLOSE, 0, 0);
    }
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = NULL;
    }
    if (gdi_token_) {
        GdiplusShutdown(gdi_token_);
        gdi_token_ = 0;
    }
}
