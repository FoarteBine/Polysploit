# Polysploit API Reference

## Environment

| Function | Returns | Description |
|---|---|---|
| `identifyexecutor()` | `string` | Returns `"Polysploit"` |
| `getgenv()` | `table` | Returns the global environment table |
| `getrenv()` | `table` | Returns the global environment table (same as getgenv) |
| `getreg()` | `nil` | Stub |
| `getsenv()` | `nil` | Stub |

## Globals

| Global | Type | Description |
|---|---|---|
| `_VERSION` | `string` | `"Polysploit v0.1"` |
| `_SHARED` | `table` | Empty table shared across all script executions |

## File I/O

All file operations are sandboxed to the `workspace\` directory next to `Injector.exe`. Path traversal is blocked.

| Function | Returns | Description |
|---|---|---|
| `readfile(path)` | `string, nil` or `nil, error` | Read file contents |
| `writefile(path, content)` | `boolean` | Write file (creates parent dirs) |
| `appendfile(path, content)` | `boolean` | Append to file (creates if missing) |
| `isfile(path)` | `boolean` | Check if path is a file |
| `isfolder(path)` | `boolean` | Check if path is a directory |
| `makefolder(path)` | `boolean` | Create directory (true if already exists) |
| `delfile(path)` | `boolean` | Delete file |
| `delfolder(path)` | `boolean` | Remove directory |
| `listfiles([path])` | `table` | List files in path (defaults to workspace root) |

## HTTP

| Function | Returns | Description |
|---|---|---|
| `httpget(url)` | `string` | GET request via WinHTTP |
| `httpupload(url [, data])` | `string` | POST request via WinHTTP |

## Clipboard

| Function | Returns | Description |
|---|---|---|
| `setclipboard(text)` | `boolean` | Copy text to clipboard |
| `toclipboard()` | `string` | Get clipboard text |

## Mouse & Input

| Function | Returns | Description |
|---|---|---|
| `mouse_move(x, y)` | - | Move cursor to screen coordinates |
| `mouse_get_position()` | `string` | Returns `"x,y"` |
| `mouse_click(button)` | - | Click: `0`=left, `1`=right, `2`=middle |
| `mouse_scroll(amount)` | - | Scroll wheel (amount * 120) |
| `mouse_lock(bool)` | - | Lock cursor to game window |
| `mouse_visible(bool)` | - | Show/hide cursor |

## Metatables

| Function | Returns | Description |
|---|---|---|
| `setreadonly(table, bool)` | `table` | Set table/userdata readonly flag |
| `make_writeable(table)` | `table` | Make table/userdata writable |
| `make_readonly(table)` | `table` | Make table/userdata readonly |
| `setrawmetatable(obj, metatable)` | `boolean` | Set object's metatable |
| `getrawmetatable(obj)` | `table` or `nil` | Get object's metatable |
| `hookfunction(...)` | `boolean` | Stub — returns `true` |

## Decompiler

| Function | Returns | Description |
|---|---|---|
| `decompile(func [, filename])` | `string` | Save bytecode to workspace. If `Unluau.CLI.exe` is present, also runs decompiler. |

## FPS Cap

| Function | Returns | Description |
|---|---|---|
| `setfpscap(n)` | - | Cap FPS at `n`. `n <= 0` removes the limit. |
| `getfpscap()` | `number` | Current FPS cap value (`0` = uncapped) |

## Drawing API (D3D12 ImGui Overlay)

Polysploit includes a D3D12 hook-based drawing system using Dear ImGui. The overlay hooks `IDXGISwapChain::Present` and renders shapes each frame.

### Shape Types

| Shape | Properties |
|---|---|
| `Line` | `Visible`, `ZIndex`, `From` (Vector2), `To` (Vector2), `Color` (Color3), `Thickness`, `Transparency` |
| `Square` | `Visible`, `ZIndex`, `Position` (Vector2), `Size` (Vector2), `Color` (Color3), `Thickness`, `Filled`, `Transparency` |
| `Circle` | `Visible`, `ZIndex`, `Position` (Vector2), `Radius`, `Color` (Color3), `Thickness`, `Filled`, `NumSides`, `Transparency` |
| `Triangle` | `Visible`, `ZIndex`, `PointA/B/C` (Vector2), `Color` (Color3), `Thickness`, `Filled`, `Transparency` |
| `Text` | `Visible`, `ZIndex`, `Position` (Vector2), `Text` (string), `Color` (Color3), `Size`, `Center`, `Outline`, `Transparency` |

### Functions

| Function | Description |
|---|---|
| `Drawing.new(type)` | Create a new shape; `type` is one of `"Line"`, `"Square"`, `"Circle"`, `"Triangle"`, `"Text"` |
| `Drawing.clear()` | Remove all shapes |
| `Drawing.GetViewportSize()` | Returns `Vector2` with window size |

### Example

```lua
local line = Drawing.new("Line")
line.From = Vector2.new(100, 100)
line.To = Vector2.new(400, 100)
line.Color = Color3.new(1, 0, 0)
line.Thickness = 2
line.Visible = true

task.wait(5)
line:Remove()
```
