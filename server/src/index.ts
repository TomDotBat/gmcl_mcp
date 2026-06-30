#!/usr/bin/env node
// index.ts — gmod-mcp MCP server entry point.
//
// Exposes tools that drive Garry's Mod by talking to the injected core.dll over a
// named pipe (Lua execution, structured state, D3D9 screenshots, engine-level
// input, VGUI introspection, console logs) plus status/inject via the native
// launcher. No OS mouse/keyboard hijacking, no RCON.

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

import { GmodIpc, Envelope } from "./ipc.js";
import { injector } from "./injector.js";

const ipc = new GmodIpc();
const server = new McpServer({ name: "gmod-mcp", version: "0.1.0" });

type Content = { content: any[]; isError?: boolean };

function textResult(obj: unknown): Content {
  return { content: [{ type: "text", text: typeof obj === "string" ? obj : JSON.stringify(obj, null, 2) }] };
}

function errorResult(msg: string): Content {
  return { content: [{ type: "text", text: `Error: ${msg}` }], isError: true };
}

// Run an IPC method and unwrap the envelope into MCP content.
async function call(method: string, params: Record<string, unknown> = {}, timeoutMs?: number): Promise<Content> {
  let env: Envelope;
  try {
    env = await ipc.request(method, params, timeoutMs);
  } catch (e: any) {
    return errorResult(String(e?.message ?? e));
  }
  if (!env.ok) return errorResult(env.error ?? "unknown error");
  return textResult(env.result ?? {});
}

// Run a capture method and return image content. Handles the pause-menu dance:
// the Lua side closes the menu and signals retry, so we wait a frame and retry once.
async function captureImage(method: string, args: Record<string, unknown>): Promise<Content> {
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
  for (let attempt = 0; attempt < 2; attempt++) {
    let env: Envelope;
    try {
      env = await ipc.request(method, args, 20000);
    } catch (e: any) {
      return errorResult(String(e?.message ?? e));
    }
    if (!env.ok) return errorResult(env.error ?? "screenshot failed");

    const r = env.result ?? {};
    if (r.base64) {
      const mime = r.format === "png" ? "image/png" : "image/jpeg";
      const label =
        (r.panel ? `${r.panel}${r.name ? ` "${r.name}"` : ""} — ` : "") + `${r.width}x${r.height} ${r.format}`;
      return {
        content: [
          { type: "image", data: r.base64, mimeType: mime },
          { type: "text", text: label },
        ],
      };
    }
    // Lua-level failure. If the agent just hid the pause menu, the next frame
    // should capture cleanly — wait briefly and retry once.
    if (r.retry && attempt === 0) {
      await sleep(150);
      continue;
    }
    return errorResult(r.error ?? "screenshot failed");
  }
  return errorResult("screenshot failed");
}

// ---------------------------------------------------------------------------
// Status / lifecycle
// ---------------------------------------------------------------------------
server.registerTool(
  "gmod_status",
  {
    description:
      "Report whether Garry's Mod is running, whether core.dll is injected, and whether the in-game IPC pipe is responding. Call this first.",
    inputSchema: {},
  },
  async () => {
    const status = await injector.status();
    const ping = await ipc.ping();
    return textResult({ injector: status, ping: ping ?? "no response" });
  }
);

server.registerTool(
  "gmod_inject",
  {
    description:
      "Inject core.dll into the running Garry's Mod process (idempotent). Use when gmod_status reports injected=false. Requires the game to be running.",
    inputSchema: {},
  },
  async () => textResult(await injector.inject())
);

server.registerTool(
  "gmod_reload_agent",
  {
    description:
      "Hot-reload the in-game Lua agent (bootstrap.lua) from disk without restarting the game. Use after editing the agent script.",
    inputSchema: {},
  },
  async () => call("reload_bootstrap")
);

// ---------------------------------------------------------------------------
// Lua + state + console
// ---------------------------------------------------------------------------
server.registerTool(
  "gmod_lua_run",
  {
    description:
      "Run Lua in the client realm and return printed output plus return values. Full persistent Lua environment (no length limit, locals persist within the snippet). Example: 'print(LocalPlayer():Nick())'.",
    inputSchema: { code: z.string().describe("Lua source to execute in the client state") },
  },
  async ({ code }) => call("lua_run", { code })
);

server.registerTool(
  "gmod_get_state",
  {
    description:
      "Structured snapshot of the game: player position/angles/health/armor/weapon/ammo, eye-trace target, nearby entities (sorted by distance), map, and UI/cursor state. Prefer this over a screenshot for situational awareness.",
    inputSchema: {
      radius: z.number().optional().describe("Entity search radius in units (default 1024)"),
      maxEntities: z.number().optional().describe("Max nearby entities to return (default 30)"),
    },
  },
  async (args) => call("get_state", args)
);

server.registerTool(
  "gmod_console_log",
  {
    description:
      "Pull recent game console output (engine Msg/Warning/Error and Lua print). Use sinceId for incremental reads, filter for a substring, severity for a minimum level (0=msg,1=warning,3=error).",
    inputSchema: {
      limit: z.number().optional().describe("Max most-recent entries to return (default 200). Alias: lines"),
      lines: z.number().optional().describe("Alias for limit"),
      sinceId: z.number().optional().describe("Only return lines with id greater than this"),
      filter: z.string().optional().describe("Case-insensitive substring filter"),
      severity: z.number().optional().describe("Minimum severity (0=msg,1=warning,2=assert,3=error)"),
    },
  },
  async (args) => {
    const { limit, lines, ...rest } = args as any;
    return call("console_log", { ...rest, lines: limit ?? lines });
  }
);

server.registerTool(
  "gmod_console_command",
  {
    description:
      "Run any console command through the engine's command buffer (unrestricted, like typing in the console). This is how you change level, join/leave servers, and run cvars/concommands — e.g. 'map gm_construct', 'connect 1.2.3.4:27015', 'disconnect', 'gamemode sandbox', 'say hi', 'noclip'. Commands run on the next frame; read results via gmod_console_log.",
    inputSchema: { command: z.string().describe("The full console command line, e.g. 'map gm_construct'") },
  },
  async ({ command }) => call("console_command", { command })
);

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------
server.registerTool(
  "gmod_screenshot",
  {
    description:
      "Capture the current frame (via GMod's render.Capture) and return it as an image. Optional crop region (x,y,w,h in pixels). Note: returns an error when the Escape menu is open or at the main menu (no client render pass) — use gmod_dump_vgui for menu UI.",
    inputSchema: {
      format: z.enum(["jpeg", "png"]).optional().describe("Image format (default jpeg)"),
      quality: z.number().min(1).max(100).optional().describe("JPEG quality 1-100 (default 80)"),
      x: z.number().optional().describe("Crop region left (px)"),
      y: z.number().optional().describe("Crop region top (px)"),
      w: z.number().optional().describe("Crop region width (px); omit for full width"),
      h: z.number().optional().describe("Crop region height (px); omit for full height"),
    },
  },
  async (args) => captureImage("screenshot", args)
);

server.registerTool(
  "gmod_screenshot_panel",
  {
    description:
      "Screenshot a single VGUI panel, cropped to its on-screen bounds. Target it by panel (a class/name/text substring, like gmod_dump_vgui's filter), by a screen point x,y (topmost panel there), or set focused to capture the keyboard-focused panel. padding adds a margin (px) around the panel for surrounding context. Same render-pass/pause-menu caveats as gmod_screenshot.",
    inputSchema: {
      panel: z.string().optional().describe("Locate panel by class/name/text substring"),
      focused: z.boolean().optional().describe("Capture the keyboard-focused panel instead of using a selector"),
      x: z.number().optional().describe("Screen x to hit-test a panel (with y)"),
      y: z.number().optional().describe("Screen y to hit-test a panel (with x)"),
      padding: z.number().optional().describe("Extra margin around the panel in px (default 0)"),
      format: z.enum(["jpeg", "png"]).optional().describe("Image format (default jpeg)"),
      quality: z.number().min(1).max(100).optional().describe("JPEG quality 1-100 (default 80)"),
    },
  },
  async (args) => captureImage("screenshot_panel", args)
);

// ---------------------------------------------------------------------------
// Input (no OS mouse/keyboard involved)
// ---------------------------------------------------------------------------
server.registerTool(
  "gmod_input",
  {
    description:
      "Set or clear held movement input. buttons holds keys down (forward/back/moveleft/moveright/jump/duck/use/attack/attack2/reload/speed/...); forward/side/up are analog movement (-1..1 of move speed; positive forward/right/up). durationMs auto-releases after the time; omit to hold until cleared. Set clearOthers to replace currently-held keys.",
    inputSchema: {
      buttons: z.array(z.string()).optional().describe("Button names to hold"),
      forward: z.number().optional().describe("Forward move, e.g. 1 (full) / -1 (back)"),
      side: z.number().optional().describe("Strafe, positive = right"),
      up: z.number().optional().describe("Up move (ladders/water/noclip)"),
      durationMs: z.number().optional().describe("Auto-release after this many ms"),
      clearOthers: z.boolean().optional().describe("Clear currently-held buttons first"),
    },
  },
  async (args) => call("input_set", args)
);

server.registerTool(
  "gmod_input_clear",
  {
    description: "Release all synthesised input (held buttons, movement, taps; optionally the look target).",
    inputSchema: { look: z.boolean().optional().describe("Also cancel an in-progress look") },
  },
  async (args) => call("input_clear", args)
);

server.registerTool(
  "gmod_look",
  {
    description:
      "Aim the view toward an angle, smoothly over a few ticks. Absolute pitch/yaw in degrees (pitch -89..89, up is negative). Set relative to offset from the current angle, instant to snap immediately, or speed for degrees-per-tick.",
    inputSchema: {
      pitch: z.number().optional(),
      yaw: z.number().optional(),
      relative: z.boolean().optional(),
      instant: z.boolean().optional(),
      speed: z.number().optional().describe("Max degrees per tick (default 8)"),
    },
  },
  async (args) => call("look", args)
);

server.registerTool(
  "gmod_press",
  {
    description:
      "Tap a button once for a short duration (default 100ms). Good for a single fire/jump/use/reload. button names match gmod_input.",
    inputSchema: {
      button: z.string().describe("Button name, e.g. 'attack', 'jump', 'use', 'reload'"),
      durationMs: z.number().optional(),
    },
  },
  async (args) => call("press", args)
);

// ---------------------------------------------------------------------------
// VGUI introspection + menu interaction
// ---------------------------------------------------------------------------
server.registerTool(
  "gmod_dump_vgui",
  {
    description:
      "Dump the VGUI panel tree (class, name, screen rect x/y/w/h, visibility, text). Use this to find clickable panels reliably, then click their center with gmod_cursor. filter narrows to panels whose class/name/text contains a substring.",
    inputSchema: {
      filter: z.string().optional().describe("Substring filter on class/name/text"),
      maxDepth: z.number().optional().describe("Max tree depth (default 12)"),
      includeInvisible: z.boolean().optional().describe("Include hidden panels (default false)"),
      limit: z.number().optional().describe("Max nodes (default 1500)"),
    },
  },
  async (args) => call("dump_vgui", args)
);

server.registerTool(
  "gmod_cursor",
  {
    description:
      "Move the in-game cursor and optionally click a VGUI panel. Provide screen x,y OR panel (a class/name/text substring to locate and click its center). This drives the game's cursor only — your real mouse is not moved. The cursor must be active (a menu open) for clicks to register.",
    inputSchema: {
      x: z.number().optional(),
      y: z.number().optional(),
      panel: z.string().optional().describe("Locate a panel by class/name/text substring and target its center"),
      click: z.boolean().optional().describe("Click after moving"),
      button: z.enum(["left", "right"]).optional().describe("Mouse button (default left)"),
    },
  },
  async (args) => call("cursor", args)
);

server.registerTool(
  "gmod_type",
  {
    description:
      "Type text into a VGUI text entry. Targets the keyboard-focused panel, or a panel located by name/class substring. Set append to add to existing text, enter to fire the entry's enter handler.",
    inputSchema: {
      text: z.string(),
      panel: z.string().optional().describe("Locate target by class/name substring instead of using focus"),
      append: z.boolean().optional(),
      enter: z.boolean().optional(),
    },
  },
  async (args) => call("type", args)
);

// ---------------------------------------------------------------------------
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  // stderr is safe for logging under stdio transport (stdout is the protocol).
  console.error("[gmod-mcp] server ready");
}

main().catch((e) => {
  console.error("[gmod-mcp] fatal:", e);
  process.exit(1);
});
