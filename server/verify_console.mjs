// Verifies console capture (x64 LoggingSystem) + reload_bootstrap, after the
// final DLL rebuild. Waits for the client realm, prints from Lua + echoes via a
// console command, then reads it back through console_log.
import { GmodIpc } from "./dist/ipc.js";
const ipc = new GmodIpc();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (l, v) => console.log(`\n=== ${l} ===\n` + JSON.stringify(v, null, 2));

let realm = "?";
for (let i = 0; i < 45; i++) {
  const p = await ipc.ping();
  realm = p?.result?.realm ?? "no-pipe";
  if (realm === "client") break;
  if (i % 3 === 0) console.log(`waiting for client realm... (${realm})`);
  await sleep(2000);
}
if (realm !== "client") { console.log(`not in client realm (${realm}); load a map.`); process.exit(0); }

try {
  log("reload_bootstrap", await ipc.request("reload_bootstrap", {}));

  await ipc.request("lua_run", { code: "print('[gmod-mcp] CONSOLE_MARKER_ALPHA 4242')" });
  await ipc.request("console_command", { command: "echo gmod_mcp_CONSOLE_MARKER_BETA" });
  await sleep(400);

  const all = await ipc.request("console_log", { lines: 40 });
  console.log(`\ntotal lines captured: ${all.result?.lines?.length}, nextId: ${all.result?.nextId}`);

  const hit = await ipc.request("console_log", { lines: 20, filter: "CONSOLE_MARKER" });
  log("console_log (filter CONSOLE_MARKER)", hit.result?.lines);

  const sawAlpha = JSON.stringify(all.result?.lines || []).includes("CONSOLE_MARKER_ALPHA");
  const sawBeta = JSON.stringify(all.result?.lines || []).includes("CONSOLE_MARKER_BETA");
  console.log(`\nLua print captured:    ${sawAlpha ? "YES" : "no"}`);
  console.log(`console echo captured: ${sawBeta ? "YES" : "no"}`);
} catch (e) {
  console.error("ERROR:", e.message);
} finally {
  process.exit(0);
}
