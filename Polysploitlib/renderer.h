#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

enum DrawType { DrawLine, DrawRect, DrawCircle, DrawTextCmd, DrawClear };

struct DrawCommand {
    DrawType type;
    float x1, y1, x2, y2;
    float r, g, b, a, thickness, radius;
    int segments, filled;
    std::string text;
};

class Renderer {
public:
    static Renderer& instance();
    void init();
    void shutdown();
    HWND game_window() const { return game_hwnd_; }
    void queue(const DrawCommand& cmd);
    void clear();

private:
    Renderer() = default;
    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static HWND find_game_window();
    static DWORD WINAPI thread_proc(LPVOID param);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

    void create_overlay();
    void render_frame();

    HWND game_hwnd_ = nullptr;
    HWND overlay_hwnd_ = nullptr;
    HANDLE thread_ = nullptr;
    ULONG_PTR gdi_token_ = 0;
    std::vector<DrawCommand> cmds_;
    std::mutex mtx_;
};
