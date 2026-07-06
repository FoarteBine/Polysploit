#pragma once
#include <string>
#include <vector>
#include <d3d12.h>
#include <dxgi1_4.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3Dcompiler.lib")

struct Vec2 { float x = 0, y = 0; };
struct Color3 { float r = 0, g = 0, b = 0; };

struct Shape {
    bool Visible = true;
    int ZIndex = 1;
    virtual ~Shape() = default;
};

struct ShapeLine : Shape { Vec2 From, To; Color3 Color; float Thickness = 1; float Transparency = 1; };
struct ShapeSquare : Shape { Vec2 Position, Size; Color3 Color; float Thickness = 1, Rounding = 0; bool Filled = false; float Transparency = 1; };
struct ShapeCircle : Shape { Vec2 Position; Color3 Color; float Radius = 0, Thickness = 1; bool Filled = false; float NumSides = 128; float Transparency = 1; };
struct ShapeTriangle : Shape { Vec2 P1, P2, P3; Color3 Color; float Thickness = 1; bool Filled = false; float Transparency = 1; };
struct ShapeText : Shape { std::string Text; Vec2 Position; Color3 Color; bool Center = false, Outline = false; float Transparency = 1; float Size = 14; Vec2 TextBounds = {0, 14}; };

namespace DrawStorage {
    extern std::vector<ShapeLine*> Lines;
    extern std::vector<ShapeSquare*> Squares;
    extern std::vector<ShapeCircle*> Circles;
    extern std::vector<ShapeTriangle*> Triangles;
    extern std::vector<ShapeText*> Texts;
}

class DrawingAPI {
public:
    static bool Init();
    static bool Shutdown();
    static HWND GameWindow;
};
