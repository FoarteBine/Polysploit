# Polysploit

Status: UP🟢
Polysploit is a script executor for **Polytoria**. It injects a DLL into the target process, hooks the Luau VM, and exposes a custom Lua API for automation, drawing, and debugging.
<img width="895" height="594" alt="image" src="https://github.com/user-attachments/assets/d595c7b8-b3a2-4290-bc68-87f2574d4c9b" />


## Architecture

```
Polysploit.exe (WPF GUI)
  └── spawns --> Injector.exe (CLI)
                   └── injects --> Polysploitlib.dll (into target process)
                                    └── hooks --> Luau.VM.dll (runtime)
```

## Building

Requires Visual Studio 2022, Windows SDK 10.0, .NET 10.0 SDK.

Open `Polysploit.slnx` and build. Outputs go to `x64\Release\`:

| Project | Output |
|---|---|
| `Polysploit/` | `Polysploit.exe` (WPF GUI) |
| `Injector/` | `Injector.exe` (CLI host) |
| `Polysploitlib/` | `Polysploitlib.dll` (injected DLL) |

Place `Polysploitlib.dll` next to `Injector.exe`. Runtime dependencies: `Luau.VM.dll`, `Luau.Compiler.dll`.

## Usage

### GUI
Run `Polysploit.exe`. Select a client from the list, write Lua code in the editor, execute.

### CLI
```
Injector.exe
  list              - show running clients
  attach            - inject DLL into un-attached clients
  execute <pid> <code> - run Lua code on a client
  unload            - unload DLL from all clients
  terminate         - kill all tracked clients
```

## Lua API

See [docs.md](docs.md) for full API reference.
