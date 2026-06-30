// Self-contained test for gmod_screenshot_panel. Creates a known popup frame at a
// fixed position with a focused text entry, then captures it three ways:
//   1. by name substring   2. by screen point   3. by keyboard focus
// Saves PNGs and prints the returned region so the crop can be verified.
import { GmodIpc } from "./dist/ipc.js";
import fs from "node:fs";

const ipc = new GmodIpc();
const dir = process.argv[2] || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (l, v) => console.log(`\n=== ${l} ===\n` + (typeof v === "string" ? v : JSON.stringify(v, null, 2)));

function saveShot(name, env) {
  if (env.ok && env.result?.base64) {
    const r = env.result;
    const file = `${dir}/${name}.${r.format === "png" ? "png" : "jpg"}`;
    fs.writeFileSync(file, Buffer.from(r.base64, "base64"));
    return `saved ${r.width}x${r.height} ${r.format} (panel=${r.panel ?? "?"}${r.name ? ` "${r.name}"` : ""}, region=${JSON.stringify(r.region)}) -> ${file}`;
  }
  return `NO IMAGE: ${JSON.stringify(env.result ?? env.error)}`;
}

const FX = 300, FY = 200, FW = 420, FH = 260; // known frame rect

try {
  log("ping", (await ipc.ping())?.result);

  // Build a deterministic popup: named DFrame + focused text entry, at a known rect.
  const setup = await ipc.request("lua_run", {
    code: `if IsValid(_G.__mcp_test) then _G.__mcp_test:Remove() end
      local f = vgui.Create("DFrame")
      f:SetName("MCPTestFrame"); f:SetTitle("MCP Panel Test")
      f:SetPos(${FX}, ${FY}); f:SetSize(${FW}, ${FH}); f:MakePopup()
      local e = vgui.Create("DTextEntry", f)
      e:SetName("MCPTestEntry"); e:SetPos(20, 60); e:SetSize(${FW - 40}, 30)
      e:SetText("focused entry"); e:RequestFocus()
      _G.__mcp_test = f
      return f:GetClassName(), tostring(vgui.GetKeyboardFocus()), f:IsVisible()`,
  }, 8000);
  log("setup (frame class, focus, visible)", setup.result);
  await sleep(150); // let layout + focus settle a frame

  log("by name substring 'MCPTestFrame' (pad 16)",
    saveShot("panel_by_substring", await ipc.request("screenshot_panel", { panel: "MCPTestFrame", padding: 16, format: "png" }, 20000)));

  const cx = FX + Math.floor(FW / 2), cy = FY + Math.floor(FH / 2);
  log(`by point ${cx},${cy} (pad 0)`,
    saveShot("panel_by_point", await ipc.request("screenshot_panel", { x: cx, y: cy, format: "png" }, 20000)));

  log("focused panel (pad 24)",
    saveShot("panel_focused", await ipc.request("screenshot_panel", { focused: true, padding: 24, format: "png" }, 20000)));

  // For comparison: also capture the surrounding context with bigger padding.
  log("by name substring, pad 80 (context)",
    saveShot("panel_pad80", await ipc.request("screenshot_panel", { panel: "MCPTestFrame", padding: 80, format: "png" }, 20000)));

  await ipc.request("lua_run", { code: "if IsValid(_G.__mcp_test) then _G.__mcp_test:Remove() end" }, 4000);
  log("cleanup", "removed test frame");
} catch (e) {
  console.error("TEST ERROR:", e.message);
} finally {
  process.exit(0);
}
