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

Most methods touch the Lua state and therefore run on the game's **main thread**,
pumped from the agent's `DrawOverlay` hook (see below). The IPC thread enqueues the
request and waits (up to ~8s) for the pump to fulfil it. `ping`, `console_log` are
served directly on the IPC thread (read-only). Each connected client gets its own IPC
thread, so several clients can be connected at once.

## Methods

| method | params | result | thread |
|---|---|---|---|
| `ping` | — | `{pong, version, lua_ready, realm, bootstrapped}` | ipc |
| `console_log` | `{lines?, sinceId?, filter?, severity?}` | `{lines:[{id,severity,text}], nextId}` | ipc |
| `lua_run` | `{code}` | `{output, returns[], error?}` | main |
| `get_state` | `{radius?, maxEntities?}` | player/world snapshot | main |
| `screenshot` | `{format?, quality?, x?,y?,w?,h?, marks?, maxMarks?, radius?, includeOccluded?}` | `{format, width, height, base64, marks?[]}` | main (render) |
| `screenshot_panel` | `{panel?, focused?, x?, y?, padding?, format?, quality?}` | `{format, width, height, base64, panel, name?, region}` | main (render) |
| `lua_errors` | `{since?, epoch?, limit?, clear?}` | `{epoch, nextSeq, total, errors[]}` | main |
| `events` | `{hook, since?, epoch?}` | `{epoch, seq, events[]}` (registers a watch on `hook`) | main |
| `eval_condition` | `{expr}` | `{truthy, value?, error?}` | main |
| `input_set` | `{buttons?[], forward?, side?, up?, durationMs?, clearOthers?}` | `{held[], move}` | main |
| `input_clear` | `{look?}` | `{cleared}` | main |
| `look` | `{pitch?, yaw?, relative?, instant?, speed?}` | `{target, speed}` | main |
| `press` | `{button, durationMs?}` | `{pressed, durationMs}` | main |
| `dump_vgui` | `{filter?, maxDepth?, includeInvisible?, limit?}` | `{tree, count, truncated}` | main |
| `cursor` | `{mark?, x?, y?, panel?, click?, button?}` | `{x, y, cursorVisible, clicked?}` | main |
| `type` | `{text, mark?, panel?, append?, enter?}` | `{typed, into}` | main |
| `console_command` | `{command}` | `{ran}` | main |
| `reload_bootstrap` | — | `{reloaded, realm}` | main |

`realm` is `client` (in a map — full features), `menu` (main menu — limited; no
`LocalPlayer`/`CompileString`), or `none`.

`console_command` is handled natively in C++ (engine console via
`IVEngineClient::ClientCmd_Unrestricted` in `engine_console.cpp`). Every other
`main`-thread method — including `screenshot` (GMod `render.Capture`) — is implemented
in [`core/lua/bootstrap.lua`](../core/lua/bootstrap.lua) and routed through
`MCP._dispatch(name, argJson)`; unknown methods are forwarded there by name.

`main`-thread methods run from the agent's `DrawOverlay` hook (client realm, after all
HUD/VGUI painting — a render pass, required for `render.Capture`, and late enough
that captures include menus), with `PostRender` as a fallback if `DrawOverlay`
stops firing, or, at the main menu, from the window subclass.

`marks` (`"panels" | "entities" | "both"`) draws numbered labels into an off-screen
copy of the frame and returns a legend: panel marks carry their rect and click centre
(`cx, cy`); entity marks carry the entity id. `cursor`/`type` accept `mark` to target a
panel from the most recent marked screenshot.

`lua_errors` and `events` number entries with a per-Lua-state `seq`; `epoch`
identifies the state. When a map load recreates the state, `epoch` changes and seqs
restart, so clients pass back the `epoch` they saw and the agent ignores a stale
`since`. `gmod_wait_for` is implemented in the MCP server by polling these methods
(plus `console_log`, `ping`, `get_state`).
No Direct3D device is ever created.
