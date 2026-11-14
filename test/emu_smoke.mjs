// Runs the browser emulator's own script (with its embedded WebAssembly firmware) under Node,
// with a minimal stand-in for the page, and checks the MIDI it sends through the same window.__fcb
// hook a browser test would use. It checks the emulator's glue and the compiled ctl.c / keys.c
// together; it does not check drawing or the real 8051 image.
// Usage: node test/emu_smoke.mjs emulator/index.html   (make emulator runs it)

import { readFileSync } from "node:fs";
import vm from "node:vm";

const html = readFileSync(process.argv[2] || "emulator/index.html", "utf8");
const script = html.slice(html.indexOf("<script>") + 8, html.lastIndexOf("</script>"));

function fakeElement() {
  return {
    classList: { toggle() {} }, style: {}, dataset: {}, textContent: "", innerHTML: "", value: "0",
    setAttribute() {}, addEventListener() {}, setPointerCapture() {}
  };
}
const elements = {};
const store = {};
const sandbox = {
  document: {
    getElementById: (id) => (elements[id] ||= fakeElement()),
    querySelectorAll: () => [],
    addEventListener() {}
  },
  localStorage: {
    getItem: (k) => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
    removeItem: (k) => { delete store[k]; }
  },
  performance: { now: () => 0 },
  setInterval: () => 0,
  atob: (s) => Buffer.from(s, "base64").toString("binary"),
  WebAssembly, Promise, console
};
sandbox.window = sandbox;
sandbox.window.addEventListener = () => {};
vm.createContext(sandbox);
vm.runInContext(script, sandbox);

const fcb = sandbox.__fcb;
let fail = 0;
function check(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) console.log("  ok   " + name);
  else { console.log("  FAIL " + name + "\n       expected: " + w + "\n       got:      " + g); fail = 1; }
}
// MIDI messages sent since the last call.
let seen = 0;
function sent() { const l = fcb.log().filter((x) => x); const out = l.slice(seen); seen = l.length; return out; }
const ms = (n) => fcb.advance(n);
// Advance in small steps, the way the page's 8ms timer does.
function wait(n) { for (let t = 0; t < n; t += 8) ms(8); }
function press(code, n) { fcb.down(code); wait(n); fcb.up(code); wait(200); }
const UP = 11, DOWN = 5;

await fcb.ready;

console.log("emulator: power-on and idle");
wait(1500);
check("nothing sent while idle", sent(), []);

console.log("emulator: stomp switches (default mode: stomp with momentary)");
press(0, 300);
check("switch 1 tap turns CC 20 on", sent(), ["b0 14 7f"]);
fcb.down(0); wait(500);
check("second press toggles it off at once", sent(), ["b0 14 00"]);
wait(800); fcb.up(0); wait(200);
check("held past 1s, the release undoes the toggle", sent(), ["b0 14 7f"]);
press(6, 840);
check("switch 6 held 0.84s stays latched", sent(), ["b0 19 7f"]);
check("its LED is on", fcb.hw.leds[5], 1);

console.log("emulator: program change");
press(UP, 100);
check("UP tap sends program 1 on release", sent(), ["c0 01"]);
fcb.down(UP); wait(1500);
check("held UP repeats on the display only", sent(), []);
fcb.up(UP); wait(200);
const p = fcb.state().program;
check("one program change on release, matching the display", sent(), ["c0 " + p.toString(16).padStart(2, "0")]);
check("the repeat moved several steps (400 ms delay, then 4 Hz)", p >= 5 && p <= 7, true);

console.log("emulator: MIDI IN");
fcb.midiIn([0xb0, 21, 127]); wait(20);
check("CC 21 = 127 on channel 1 lights switch 2", fcb.hw.leds[1], 1);
check("and nothing is echoed", sent(), []);

console.log("emulator: config mode, channel change, save and reload");
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("UP + DOWN held 1s opens config mode", [fcb.state().mode, fcb.hw.status], [1, 1]);
press(UP, 100);
check("UP on the channel page selects channel 2", fcb.state().midiChannel, 2);
press(0, 100);
check("footswitches send nothing in config mode", sent(), []);
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("a second hold saves and leaves", [fcb.state().mode, fcb.hw.status], [0, 0]);
check("the settings record was stored", JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0], 2);
fcb.reset(); wait(400);
check("after a power cycle the channel is still 2", fcb.state().midiChannel, 2);
press(0, 100);
check("and switch 1 now sends on channel 2", sent(), ["b1 14 7f"]);

console.log("emulator: expression pedal");
fcb.setRaw(200, 0); wait(100);
check("pedal A at 200 of 255 sends CC 7", sent(), ["b1 07 64"]);

console.log(fail ? "emulator smoke test FAILED" : "emulator smoke test passed");
process.exit(fail);
