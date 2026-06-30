// In-game verification: waits for the client realm, then exercises lua/state/
// input/look/screenshot and reports a before/after position delta.
import { GmodIpc } from "./dist/ipc.js";
import fs from "node:fs";

const ipc = new GmodIpc();
const dir = process.argv[2] || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (l, v) => console.log(`\n=== ${l} ===\n` + (typeof v === "string" ? v : JSON.stringify(v, null, 2)));

async function shot(name) {
  const s = await ipc.request("screenshot", { format: "jpeg", quality: 70, scale: 0.5 }, 20000);
  if (s.ok) { fs.writeFileSync(`${dir}/${name}`, Buffer.from(s.result.base64, "base64")); return `${s.result.width}x${s.result.height} -> ${name}`; }
  return `screenshot failed: ${s.error}`;
}

// 1) Wait for client realm (map loaded).
let realm = "?";
for (let i = 0; i < 45; i++) {
  const p = await ipc.ping();
  realm = p?.result?.realm ?? "no-pipe";
  if (realm === "client") break;
  if (i % 3 === 0) console.log(`waiting for client realm... (currently: ${realm})`);
  await sleep(2000);
}
if (realm !== "client") {
  console.log(`\nStill not in client realm (${realm}). Load a sandbox map (e.g. gm_construct) and re-run.`);
  process.exit(0);
}
console.log("client realm is up — running in-game checks");

try {
  log("lua_run", await ipc.request("lua_run", {
    code: "return LocalPlayer():Nick(), game.GetMap(), math.Round(LocalPlayer():GetPos().x)",
  }));

  const before = await ipc.request("get_state", { radius: 400, maxEntities: 5 });
  log("get_state.player (before)", before.result?.player);
  log("screenshot (before)", await shot("shot_before.jpg"));

  // 2) Movement test: walk forward at full speed for ~1s.
  log("input forward", await ipc.request("input_set", { forward: 1.0, durationMs: 1000 }));
  await sleep(1300);
  await ipc.request("input_clear", {});

  const after = await ipc.request("get_state", { radius: 400, maxEntities: 5 });
  log("get_state.player (after)", after.result?.player);

  const a = before.result?.player?.pos, b = after.result?.player?.pos;
  if (a && b) {
    const d = Math.hypot(b.x - a.x, b.y - a.y, b.z - a.z);
    log("MOVEMENT DELTA (units)", d.toFixed(1) + (d > 20 ? "  <-- moved!" : "  <-- little/no movement"));
  }

  // 3) Look test: turn 90 degrees to the right, smoothly.
  log("look +90 yaw", await ipc.request("look", { yaw: 90, relative: true, speed: 10 }));
  await sleep(900);

  // 4) Single jump.
  log("press jump", await ipc.request("press", { button: "jump", durationMs: 120 }));
  await sleep(400);
  log("screenshot (after)", await shot("shot_after.jpg"));

  // 5) Console capture (will be empty unless the DLL has the LoggingSystem build).
  const cl = await ipc.request("console_log", { lines: 15 });
  log("console_log", { lines: cl.result?.lines?.length, sample: (cl.result?.lines || []).slice(-5) });
} catch (e) {
  console.error("VERIFY ERROR:", e.message);
} finally {
  process.exit(0);
}
