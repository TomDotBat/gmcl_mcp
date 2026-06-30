# gmod-mcp

An [MCP](https://modelcontextprotocol.io) server that lets an AI agent **play Garry's
Mod by hooking the game directly**.

A small C++ DLL (`core.dll`) is injected into the game and:

- **bridges the Lua runtime** — run Lua, read structured game state (player, weapon,
  aim, nearby entities), introspect the VGUI panel tree;
- **hooks Direct3D9** — capture screenshots straight from the backbuffer (works even
  with menus open);
- **injects input at the engine level** — movement, look, fire, jump, VGUI clicks and
  typing — via a `CreateMove`/`CUserCmd` model and the `gui`/`input` libraries, so
  **your real mouse and keyboard are never touched**;
- **captures the console** — engine `Msg`/`Warning`/`Error` and Lua `print`.

> Built and verified against the **Garry's Mod x86-64 (64-bit) branch** (`gmod.exe`,
> `bin/win64`). See [Branch &amp; bitness](#branch--bitness).

## Architecture

```
Claude (AI agent)
  │  MCP stdio
  ▼
server/  (TypeScript MCP server)  ──shells out──►  injector.exe  (finds gmod.exe, LoadLibrary-injects core.dll)
  │  named pipe \\.\pipe\gmod_mcp (length-prefixed JSON)
  ▼
core.dll  (injected, x64)
  ├─ ipc_server     pipe server thread
  ├─ command_queue  hand-off to the main thread
  ├─ dx9_hook       Present hook = main-thread "pump" (+ screenshots)
  ├─ lua_bridge     CreateInterface("LUASHARED003") → client ILuaInterface
  ├─ console_log    tier0 LoggingSystem listener (x64) / spew func (legacy)
  └─ bootstrap.lua  the in-game agent: input model, state, VGUI (hot-reloadable)
```

See [`docs/protocol.md`](docs/protocol.md) for the wire protocol.

## Prerequisites

- Windows, **Garry's Mod** (x86-64 branch — see below)
- **Visual Studio 2022** (Desktop C++ / MSVC x64) + Windows SDK
- **premake5** (`premake5.exe` on PATH)
- **Node.js ≥ 18**

Third-party deps are vendored under `core/third_party/` (`garrysmod_common` and
`minhook` as submodules; `json.hpp` and `stb_image_write.h` as single headers).
After cloning:

```sh
git submodule update --init --recursive
```

## Build

```powershell
# Native (core.dll + injector.exe) -> build/bin/Release/
premake5 vs2022
# open a "x64 Native Tools" prompt, or run vcvars64.bat, then:
msbuild build/gmod-mcp.sln /p:Configuration=Release /p:Platform=x64 /m

# MCP server
cd server
npm install
npm run build
```

Outputs land in `build/bin/Release/`: `core.dll`, `injector.exe`, and a copy of
`bootstrap.lua` (the bridge loads it from next to the DLL at runtime).

## Run

1. Launch Garry's Mod (x86-64 branch).
2. Wire the MCP server into your client. Example (`claude_desktop_config.json` /
   Claude Code / Cursor):

   ```json
   {
     "mcpServers": {
       "gmod": {
         "command": "node",
         "args": ["E:/path/to/gmod-mcp/server/dist/index.js"]
       }
     }
   }
   ```

   The server auto-discovers `injector.exe` under `build/bin/Release|Debug`; override
   with the `GMOD_MCP_INJECTOR` env var if you moved it.
3. Ask the agent to call `gmod_status`, then `gmod_inject`. Once injected and you are
   **in a map**, all tools are live.

## Tools

| Tool                     | What it does                                                      |
| ------------------------ | ----------------------------------------------------------------- |
| `gmod_status`          | Is the game up? DLL injected? pipe alive? Lua realm?              |
| `gmod_inject`          | Inject`core.dll` into the running game                          |
| `gmod_reload_agent`    | Hot-reload`bootstrap.lua` (no restart)                          |
| `gmod_lua_run`         | Run Lua in the client realm; returns print output + return values |
| `gmod_get_state`       | Structured player/world snapshot                                  |
| `gmod_console_log`     | Recent console output (engine + Lua`print`)                     |
| `gmod_console_command` | Run a console command                                             |
| `gmod_screenshot`      | D3D9 backbuffer image (PNG/JPEG, crop + scale)                    |
| `gmod_input`           | Hold/clear buttons and analog movement (−1..1)                   |
| `gmod_input_clear`     | Release all synthesised input                                     |
| `gmod_look`            | Aim toward an angle (smooth or instant; abs/rel)                  |
| `gmod_press`           | Tap a button once                                                 |
| `gmod_dump_vgui`       | Dump the VGUI panel tree (class/name/rect/text)                   |
| `gmod_cursor`          | Move the in-game cursor / click a VGUI panel                      |
| `gmod_type`            | Type into a VGUI text entry                                       |

## Development workflow

- **Lua-only changes** (`bootstrap.lua`): edit, copy it next to `core.dll`, then call
  `gmod_reload_agent` (or just reload — the agent is re-read on a menu→map switch). No
  rebuild, no restart.
- **Native changes** (`core.dll`): the DLL file is locked while loaded, and a clean
  hot-unload isn't possible under the Windows loader lock. **Restart the game**, then
  rebuild and `gmod_inject` again. (`injector.exe eject` exists but cannot fully
  unload a running DLL.)
- A `server/probe.mjs` / `server/verify_ingame.mjs` are included for quick checks.

## Branch & bitness

The injector targets `gmod.exe` and prefers the process that has `lua_shared.dll`
loaded — the x86-64 branch runs Chromium (CEF) helper subprocesses that share the
`gmod.exe` name, and only the real game process hosts the engine. `core.dll` and
`injector.exe` are **x64**; they will not inject into a 32-bit game. The 32-bit branch
would need an x86 build (the code is bitness-agnostic; only the project platform
differs) — not currently produced.

## Safety & scope

Intended for **single-player and your own servers**. Injecting into
multiplayer servers with anti-cheat can get you banned — that's on you. The launcher
warns but does not police it. Held inputs auto-expire (a dropped agent can't leave you
walking into a wall forever), and `gmod_input_clear` releases everything.

## License

Licensed under the [MIT License](LICENSE).

**No warranty, no responsibility.** This software is provided "as is", without
warranty of any kind, express or implied. I provide **absolutely no warranty** and
accept **no responsibility or liability** whatsoever for anything arising from its use
— including but not limited to game bans, crashes, data loss, or any other damage. You
use it entirely at your own risk.