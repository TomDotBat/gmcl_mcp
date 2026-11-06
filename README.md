# gmod-mcp

[![Build](https://github.com/TomDotBat/gmcl_mcp/actions/workflows/build.yml/badge.svg)](https://github.com/TomDotBat/gmcl_mcp/actions/workflows/build.yml)

An [MCP](https://modelcontextprotocol.io) server that lets an AI agent **play Garry's
Mod by hooking the game directly**.

A small C++ DLL (`core.dll`) is injected into the game and:

- **bridges the Lua runtime** — run Lua, read structured game state (player, weapon,
  aim, nearby entities), introspect the VGUI panel tree;
- **captures screenshots** — via GMod's `render.Capture` from a per-frame render pass
  (no Direct3D device is created, so injection can't destabilise the renderer);
- **injects input at the engine level** — movement, look, fire, jump, VGUI clicks and
  typing — via a `CreateMove`/`CUserCmd` model and the `gui`/`input` libraries, so
  **your real mouse and keyboard are never touched**;
- **runs console commands** through the engine console (`ClientCmd_Unrestricted`) —
  `map`, `connect`, `disconnect`, cvars, all unrestricted;
- **captures the console** — engine `Msg`/`Warning`/`Error` and Lua `print`.

> Built and verified against the **64-bit Garry's Mod client on Windows**
> (`gmod_win64.exe`), which is the default on the main branch since the 64-bit update.
> See [Branch &amp; bitness](#branch--bitness).

## Architecture

```
Claude (AI agent)
  │  MCP stdio
  ▼
server/  (TypeScript MCP server)  ──shells out──►  injector.exe  (finds gmod.exe, LoadLibrary-injects core.dll)
  │  named pipe \\.\pipe\gmod_mcp (length-prefixed JSON)
  ▼
core.dll  (injected, x64)
  ├─ ipc_server      pipe server thread
  ├─ command_queue   hand-off to the main thread
  ├─ pump            window subclass + a C fn the agent calls from DrawOverlay; drains
  │                  the queue on the main thread (no D3D device — nothing to crash)
  ├─ lua_bridge      CreateInterface("LUASHARED003") → ILuaInterface (re-resolved each
  │                  frame so it survives map changes); registers _mcp_native_pump
  ├─ engine_console  IVEngineClient::ClientCmd_Unrestricted (map/connect/cvars)
  ├─ console_log     tier0 spew func (SpewOutputFunc) / LoggingSystem listener
  │                  (old x86-64 beta branch)
  └─ bootstrap.lua   the in-game agent: input, state, VGUI, render.Capture screenshots,
                     DrawOverlay pump hook, marks, error capture (hot-reloadable)
```

See [`docs/protocol.md`](docs/protocol.md) for the wire protocol.

## Install

**Prerequisites:** Windows, **Garry's Mod** (64-bit — the default; see below), **Node.js ≥ 18**.

Every push to `main` is built by [GitHub Actions](.github/workflows/build.yml), and
every tagged version publishes prebuilt binaries — no Visual Studio or premake5 needed
unless you're changing the native code.

1. Grab the latest [release](../../releases/latest) and unzip both archives:
   - `gmod-mcp-native-*-win64.zip` → `core.dll`, `injector.exe`, `bootstrap.lua`
     (keep these three together in one folder — the injector loads `core.dll` from its
     own directory, which in turn loads `bootstrap.lua` from its own directory).
   - `gmod-mcp-server-*.zip` → the built MCP server (`dist/`, `package.json`,
     `package-lock.json`).
2. Install the server's runtime dependencies:

   ```sh
   cd gmod-mcp-server
   npm install --omit=dev
   ```
3. Point the server at the injector — either set `GMOD_MCP_INJECTOR` to the extracted
   `injector.exe` path, or drop the native zip's contents into `build/bin/Release/` next
   to a checkout of this repo (that's where the server auto-discovers it from).

Skip to [Run](#run) once both zips are in place.

### Build from source

Needed if you're changing `core/` or `injector/`.

- **Visual Studio 2022** or **VS 2022 Build Tools** (Desktop C++ / MSVC x64) + Windows SDK
- **premake5** (`premake5.exe` on PATH)

Third-party deps are vendored under `core/third_party/` (`garrysmod_common` and
`minhook` as submodules; `json.hpp` and `stb_image_write.h` as single headers).
After cloning:

```sh
git submodule update --init --recursive
```

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

1. Launch Garry's Mod (64-bit, `gmod_win64.exe` — the default on Windows).
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
| `gmod_console_command` | Run **any** command via the engine console (unrestricted): `map`, `connect`, `disconnect`, cvars |
| `gmod_wait_for`        | Block until a condition holds: Lua expression, console regex, hook fired, new Lua error, or in-game (survives map loads) |
| `gmod_lua_errors`      | Structured Lua errors (message, file/line, stack, addon, repeat count), incremental via `since`/`epoch` |
| `gmod_screenshot`      | Full frame incl. HUD/menus via render.Capture (PNG/JPEG, optional region). `marks` overlays numbered labels on clickable panels and/or entities, with a legend |
| `gmod_screenshot_panel`| Screenshot one VGUI panel (by substring / x,y / focused) + padding |
| `gmod_input`           | Hold/clear buttons and analog movement (−1..1)                   |
| `gmod_input_clear`     | Release all synthesised input                                     |
| `gmod_look`            | Aim toward an angle (smooth or instant; abs/rel)                  |
| `gmod_press`           | Tap a button once                                                 |
| `gmod_dump_vgui`       | Dump the VGUI panel tree (class/name/rect/text)                   |
| `gmod_cursor`          | Move the in-game cursor / click a VGUI panel (by `mark`, x/y, or panel text) |
| `gmod_type`            | Type into a VGUI text entry (focused, by `mark`, or by panel text) |

### Agent workflow

A few patterns that make agents faster and more reliable:

- **Wait, don't poll.** After `gmod_console_command map gm_construct` (or `connect`),
  call `gmod_wait_for {inGame: true, timeoutMs: 60000}`. To wait for something in the
  game, use `{lua: "IsValid(LocalPlayer()) and LocalPlayer():Alive()"}`,
  `{hook: "PlayerSpawn"}`, or `{console: "regex"}`. A timeout returns `matched: false`
  rather than an error.
- **Act on what you see.** `gmod_screenshot {marks: "panels"}` numbers every clickable
  panel and returns a legend. Then call `gmod_cursor {mark: 4, click: true}` or
  `gmod_type {mark: 5, text: "...", enter: true}`. No pixel guessing needed.
  `marks: "entities"` tags visible entities with their ids.
- **Edit, test, fix.** After changing Lua, call `gmod_lua_errors` (pass back the
  `nextSeq`/`epoch` it returned to see only new errors). Or call
  `gmod_wait_for {luaError: true}` to catch an error your action triggers.
- **Lua runs inside a render hook.** Wrap world-changing calls (e.g. removing entities)
  in `timer.Simple(0, function() ... end)` in `gmod_lua_run`.

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

Since the 64-bit update, Garry's Mod on Windows ships 64-bit on the **main branch**:
the game runs as `gmod_win64.exe` in the install root (engine binaries in
`bin/win64`), while the root `gmod.exe` is now the 32-bit build. No beta branch is
needed.

The injector looks for `gmod_win64.exe` (falling back to `gmod.exe`/`hl2.exe`, which
also covers the old x86-64 beta's `bin/win64/gmod.exe`) and picks the process that has
`lua_shared.dll` loaded. The game runs Chromium (CEF) helper subprocesses that share
its exe name, and only the real game process hosts the engine.

`core.dll` and `injector.exe` are **x64**, so they can't be injected into a 32-bit game.
If the 32-bit `gmod.exe` is running, `gmod_status` reports `wrongBitness: true` and
`gmod_inject` refuses with an explanation. Relaunch the default 64-bit build. A
32-bit build of this project would need an x86 project platform (the code is
otherwise bitness-agnostic). None is currently produced.

## Troubleshooting

**Inject only once the game is at the main menu** (`gmod_status` → `running:true`).
The injector refuses to target a still-loading process — injecting into an early/CEF
subprocess that hasn't loaded `lua_shared.dll` yet can crash the launch.

**`gmod_screenshot` returns an error / black frame.** Screenshots use `render.Capture`
from the agent's `DrawOverlay` hook (after all HUD/VGUI painting, so menus are in the
frame). It doesn't work at the **main menu** (no client render pass); the Escape menu
is closed automatically before capturing. Use `gmod_dump_vgui` to inspect main-menu UI.

**Tools return "lua interface not ready" right after a map load.** The client Lua
state is recreated on `map`; the bridge re-resolves it and auto-reloads the agent
within a frame or two — retry, or call `gmod_reload_agent`.

> Design note: the pump never creates a Direct3D device. It drives the main thread via
> a window subclass plus a `DrawOverlay` hook in the agent, so injection can't
> destabilise the renderer.

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