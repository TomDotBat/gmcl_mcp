# IPC protocol (MCP server ↔ injected core.dll)

Transport: a Windows **named pipe** at `\\.\pipe\gmod_mcp`.

Framing: each message is a **4-byte little-endian unsigned length** followed by that
many bytes of **UTF-8 JSON**. Requests and responses use the same framing.

```
+--------------------+-------------------------------+
| uint32 LE length N | N bytes of UTF-8 JSON          |
+--------------------+-------------------------------+
```

### Request

```json
{ "id": 7, "method": "lua_run", "params": { "code": "print('hi')" } }
```

- `id` — client-chosen integer, echoed back for correlation.
- `method` — see table below.
- `params` — object (may be omitted/empty).

### Response (envelope)

```json
{ "id": 7, "ok": true, "result": { "output": "hi", "returns": [] } }
```

or on failure:

```json
{ "id": 7, "ok": false, "error": "lua interface not ready" }
```

## Threading

Most methods touch the Lua state or the D3D9 device and therefore run on the game's
**main thread**, pumped from inside the `IDirect3DDevice9::Present` hook. The IPC
thread enqueues the request and waits (up to ~8s) for the pump to fulfil it.
`ping`, `console_log` are served directly on the IPC thread (read-only).

## Methods

| method | params | result | thread |
|---|---|---|---|
| `ping` | — | `{pong, version, lua_ready, realm, bootstrapped}` | ipc |
| `console_log` | `{lines?, sinceId?, filter?, severity?}` | `{lines:[{id,severity,text}], nextId}` | ipc |
| `lua_run` | `{code}` | `{output, returns[], error?}` | main |
| `get_state` | `{radius?, maxEntities?}` | player/world snapshot | main |
| `screenshot` | `{format?, quality?, x?,y?,w?,h?, scale?}` | `{format, width, height, base64}` | main (Present) |
| `screenshot_panel` | `{panel?, focused?, x?, y?, padding?, format?, quality?}` | `{format, width, height, base64, panel, name?, region}` | main (Present) |
| `input_set` | `{buttons?[], forward?, side?, up?, durationMs?, clearOthers?}` | `{held[], move}` | main |
| `input_clear` | `{look?}` | `{cleared}` | main |
| `look` | `{pitch?, yaw?, relative?, instant?, speed?}` | `{target, speed}` | main |
| `press` | `{button, durationMs?}` | `{pressed, durationMs}` | main |
| `dump_vgui` | `{filter?, maxDepth?, includeInvisible?, limit?}` | `{tree, count, truncated}` | main |
| `cursor` | `{x?, y?, panel?, click?, button?}` | `{x, y, cursorVisible, clicked?}` | main |
| `type` | `{text, panel?, append?, enter?}` | `{typed, into}` | main |
| `console_command` | `{command}` | `{ran}` | main |
| `reload_bootstrap` | — | `{reloaded, realm}` | main |

`realm` is `client` (in a map — full features), `menu` (main menu — limited; no
`LocalPlayer`/`CompileString`), or `none`.

`console_command` is handled natively in C++ (engine console via
`IVEngineClient::ClientCmd_Unrestricted` in `engine_console.cpp`). Every other
`main`-thread method — including `screenshot` (GMod `render.Capture`) — is implemented
in [`core/lua/bootstrap.lua`](../core/lua/bootstrap.lua) and routed through
`MCP._dispatch(name, argJson)`; unknown methods are forwarded there by name.

`main`-thread methods run from the agent's `PostRender` hook (client realm, a render
pass — required for `render.Capture`) or, at the main menu, from the window subclass.
No Direct3D device is ever created.
