// Ad-hoc verification probe — talks to the injected DLL over the pipe.
import { GmodIpc } from "./dist/ipc.js";
import fs from "node:fs";

const ipc = new GmodIpc();
const out = process.argv[2] || "shot.jpg";

function log(label, v) { console.log(`\n=== ${label} ===\n` + JSON.stringify(v, null, 2)); }

try {
  log("ping", await ipc.request("ping", {}, 4000));

  log("lua_run", await ipc.request("lua_run", {
    code: "print('[probe] hello from mcp'); return tostring(LocalPlayer()), game.GetMap(), VERSION",
  }, 8000));

  const state = await ipc.request("get_state", { radius: 600, maxEntities: 8 }, 8000);
  log("get_state", state);

  const shot = await ipc.request("screenshot", { format: "jpeg", quality: 70, scale: 0.5 }, 20000);
  if (shot.ok) {
    fs.writeFileSync(out, Buffer.from(shot.result.base64, "base64"));
    console.log(`\n=== screenshot ===\nsaved ${shot.result.width}x${shot.result.height} ${shot.result.format} -> ${out}`);
  } else {
    log("screenshot", shot);
  }

  log("console_log", await ipc.request("console_log", { lines: 12, filter: "probe" }, 4000));
} catch (e) {
  console.error("PROBE ERROR:", e.message);
} finally {
  process.exit(0);
}
