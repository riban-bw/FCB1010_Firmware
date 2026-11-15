// Runs the browser emulator's own script (with its embedded WebAssembly firmware) under Node,
// with a minimal stand-in for the page, and checks the MIDI it sends through the same window.__fcb
// hook a browser test would use. It checks the emulator's glue and the compiled ctl.c / keys.c
// together; it does not check drawing or the real 8051 image.
// Usage: node test/emu_smoke.mjs emulator/index.html   (make emulator runs it)

import { readFileSync } from "node:fs";
import vm from "node:vm";

const html = readFileSync(process.argv[2] || "emulator/index.html", "utf8");
const script = html.slice(html.indexOf("<script>") + 8, html.lastIndexOf("</script>"));

// classList records its classes, so checks can read what the page drew.
function fakeClassList() {
  const set = new Set();
  return {
    toggle(c, on) { if (on === undefined) on = !set.has(c); if (on) set.add(c); else set.delete(c); return on; },
    add(c) { set.add(c); }, remove(c) { set.delete(c); }, contains(c) { return set.has(c); }
  };
}
function fakeElement() {
  return {
    classList: fakeClassList(), style: {}, dataset: {}, textContent: "", innerHTML: "", value: "0",
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
// Stomp CCs and the default pedal mode come from src/config.h, so this follows the config.
const cfgText = readFileSync(new URL("../src/config.h", import.meta.url), "utf8");
const STOMP_CC = cfgText.match(/#define STOMP_CC_LIST ([0-9, ]+)/)[1].split(",").map(Number);
const DEF_MODE = Number(cfgText.match(/#define DEFAULT_PEDAL_MODE (\d)/)[1]);
const CHORD_CC = Number(cfgText.match(/#define CHORD_CC (\d+)/)[1]);
const hex = (n) => n.toString(16).padStart(2, "0");
const cc = (ch, sw, v) => "b" + ch + " " + hex(STOMP_CC[sw]) + " " + hex(v);
function chordTap() { fcb.down(UP); fcb.down(DOWN); wait(100); fcb.up(UP); fcb.up(DOWN); wait(200); }
// MIDI CHANNEL, MIDI FUNCTION, EXP 1, EXP 2.
const pageLeds = () => [fcb.hw.aux[0], fcb.hw.aux[1], fcb.hw.aux[2], fcb.hw.aux[3]];
const TEXT_ER = 1, TEXT_LO = 2, TEXT_HI = 3;   // hw.text is HAL_TEXT_* + 1

await fcb.ready;

console.log("emulator: power-on and idle");
// Power-on: every LED lit at once (no blinking) while "riban" scrolls once (no version),
// then run mode with program 0.
// Times from config.h; the scan starts after STARTUP_TICKS of settling.
const cfgMs = (k) => Number(cfgText.match(new RegExp("#define " + k + "\\s+(\\d+)"))[1]);
const BOOT_SHOW = cfgMs("VERSION_SHOW_MS"), BOOT_SETTLE = cfgMs("STARTUP_TICKS") * cfgMs("TICK_MS");
// The "riban" frames as hw.text values (HAL_TEXT_SCROLL + 1 onwards), and what the page
// draws for each: " r" = 2 segments, "ri" 3, "ib" 6, "ba" 11, "an" 9, "n " 3.
const SCROLL = [" r", "ri", "ib", "ba", "an", "n "], SCROLL_TEXT = SCROLL.map((w, i) => 4 + i);
const SCROLL_SEGS = [2, 3, 6, 11, 9, 3];
const litSegs = () => (sandbox.document.getElementById("display").innerHTML.match(/var\(--seg-on\)/g) || []).length;
// Sampled every 8 ms (wait() runs in 8 ms steps) until it ends: frames in order, LEDs lit.
const bootFrames = [], bootLamps = new Set(), bootSegs = new Set();
for (let t = 0; t < BOOT_SETTLE + BOOT_SHOW * 3 - 48; t += 8) {
  wait(8);
  if (fcb.hw.text) bootLamps.add(fcb.hw.lamp);
  if (fcb.hw.text && bootFrames[bootFrames.length - 1] !== fcb.hw.text) bootFrames.push(fcb.hw.text);
  if (fcb.hw.text) bootSegs.add(fcb.hw.text + ":" + litSegs());
}
check("at power-on \"riban\" scrolls: " + SCROLL.map((w) => '"' + w + '"').join(", "), bootFrames, SCROLL_TEXT);
check("drawn with the right segments", [...bootSegs], SCROLL_TEXT.map((t, i) => t + ":" + SCROLL_SEGS[i]));
check("every LED lit throughout (no blinking), no version shown", [[...bootLamps], fcb.hw.hex], [[3], -1]);
wait(100);
check("then run mode: normal LEDs, no dots, program 0", [fcb.hw.lamp, fcb.hw.hex, fcb.hw.text, fcb.hw.dots, fcb.hw.display, fcb.state().mode], [0, -1, 0, 0, 0, 0]);
wait(BOOT_SHOW * 4);
check("and it does not repeat", [fcb.hw.lamp, fcb.hw.hex], [0, -1]);
check("nothing sent while idle", sent(), []);

console.log("emulator: stomp switches (default mode " + DEF_MODE + ")");
press(0, 300);
const MOM = DEF_MODE >= 2, ON = DEF_MODE === 3 ? 0 : 127, OFF = 127 - ON;
check("switch 1 tap sends CC " + STOMP_CC[0], sent(), MOM ? [cc(0, 0, ON), cc(0, 0, OFF)] : [cc(0, 0, 127)]);
// Holds over and under the long-press time (MOMENTARY_HOLD_MS, checked every 12 ms).
const HOLD_MS = Number(cfgText.match(/#define MOMENTARY_HOLD_MS (\d+)/)[1]);
const OVER_MS = HOLD_MS + 300, UNDER_MS = Math.max(60, HOLD_MS - 150);
fcb.down(0); wait(Math.min(500, OVER_MS - 100));
check("second press acts at once", sent(), MOM ? [cc(0, 0, ON)] : [cc(0, 0, 0)]);
wait(OVER_MS - Math.min(500, OVER_MS - 100)); fcb.up(0); wait(200);
check("release after a " + OVER_MS + " ms hold (reverts only in stomp with momentary)", sent(),
      DEF_MODE === 1 ? [cc(0, 0, 127)] : MOM ? [cc(0, 0, OFF)] : []);
press(6, UNDER_MS);
check("switch 6 held " + UNDER_MS + " ms (under the hold time)", sent(), MOM ? [cc(0, 5, ON), cc(0, 5, OFF)] : [cc(0, 5, 127)]);
check("its LED", fcb.hw.leds[5], DEF_MODE === 2 ? 0 : 1);

console.log("emulator: program change");
press(UP, 100);
check("UP tap sends program 1 on release", sent(), ["c0 01"]);
fcb.down(UP); wait(1500);
check("held UP repeats on the display only", sent(), []);
fcb.up(UP); wait(200);
const p = fcb.state().program;
check("one program change on release, matching the display", sent(), ["c0 " + p.toString(16).padStart(2, "0")]);
check("the repeat moved several steps (400 ms delay, then 4 Hz)", p >= 5 && p <= 7, true);
// Held 6 s: 4 Hz to about 4 s (16 steps), then 20 Hz (about 37 more by 6 s).
const WRAP = Number(cfgText.match(/#define PROGRAM_WRAP (\d)/)[1]);
// Repeat steps after holding T ms, from the timings in config.h (as ctl.c counts them).
const cfgNum = (name) => Number(cfgText.match(new RegExp("#define " + name + " (\\d+)"))[1]);
const RD = cfgNum("PROGRAM_REPEAT_DELAY_MS"), RP = 1000 / cfgNum("PROGRAM_REPEAT_HZ");
const RF = 1000 / cfgNum("PROGRAM_REPEAT_FAST_HZ"), RA = cfgNum("PROGRAM_REPEAT_FAST_AFTER_MS");
const FAST_STEPS = Math.ceil((RA - RD) / RP) + 1, FAST_AT = RD + (FAST_STEPS - 1) * RP;
const repeatSteps = (t) => t < RD ? 0 : t < FAST_AT ? Math.floor((t - RD) / RP) + 1 : FAST_STEPS + Math.floor((t - FAST_AT) / RF);
fcb.down(UP); wait(6000);
const fastP = fcb.state().program;
const want = Math.min(repeatSteps(6000), 127 - p), slowOnly = Math.floor((6000 - RD) / RP) + 1;
check("held 6 s, the repeat speeds up after about " + RA + " ms (" + want + " steps expected; " + slowOnly + " without)",
      Math.abs((fastP - p) - want) <= 2, true);
fcb.up(UP); wait(200); sent();
// Held DOWN long enough to pass 0: it stops there without wrap.
fcb.down(DOWN); wait(9000);
check("held DOWN stops at 0 (no wrap)" + (WRAP ? " - skipped, PROGRAM_WRAP is 1" : ""), WRAP ? 0 : fcb.state().program, 0);
fcb.up(DOWN); wait(200);
check("and sends program 0 once on release", WRAP ? [] : sent(), WRAP ? [] : ["c0 00"]);
// Back to p for the checks below.
if (WRAP) { while (fcb.state().program !== p) { press(UP, 60); } } else { for (let i = 0; i < p; i++) press(UP, 60); }
wait(100); sent();
check("back at program " + p, fcb.state().program, p);

console.log("emulator: MIDI IN");
fcb.midiIn([0xb0, STOMP_CC[1], 127]); wait(20);
check("CC " + STOMP_CC[1] + " = 127 on channel 1 lights switch 2", fcb.hw.leds[1], 1);
check("and nothing is echoed", sent(), []);
// CHORD_CC on MIDI IN sets the DIRECT SELECT state and LED, again with no echo.
fcb.midiIn([0xb0, CHORD_CC, 127]); wait(20);
check("CC " + CHORD_CC + " = 127 lights DIRECT SELECT", [fcb.hw.aux[4], sent()], [1, []]);
fcb.midiIn([0xb0, CHORD_CC, 0]); wait(20);
check("CC " + CHORD_CC + " = 0 turns it off", [fcb.hw.aux[4], sent()], [0, []]);

console.log("emulator: UP + DOWN tap in run mode");
chordTap();
check("toggles CC " + CHORD_CC + " on", sent(), ["b0 " + hex(CHORD_CC) + " 7f"]);
check("and lights DIRECT SELECT", fcb.hw.aux[4], 1);
check("without changing the program", fcb.state().program, p);
chordTap();
check("a second tap turns it off", sent(), ["b0 " + hex(CHORD_CC) + " 00"]);
check("and DIRECT SELECT goes out", fcb.hw.aux[4], 0);

console.log("emulator: config mode pages, channel entry, save, cancel and reload");
const longKey = (k) => { fcb.down(k); wait(1300); fcb.up(k); wait(200); };
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("each footswitch shows its CC number", STOMP_CC.map((n, i) => sandbox.document.getElementById("cc" + i).textContent), STOMP_CC.map((n) => "CC " + n));
check("the UP and DOWN switches are labelled UP [ENTER] and DOWN [ESCAPE]",
      [...sandbox.document.getElementById("rows").innerHTML.matchAll(/class="num">([^<]*)</g)].map((m) => m[1]).filter((t) => /UP|DOWN/.test(t)), ["UP [ENTER]", "DOWN [ESCAPE]"]);
check("UP + DOWN held 1s opens config mode on page 1", [fcb.state().mode, fcb.hw.status, fcb.state().cfgPage], [1, 1, 0]);
check("page 1 lights MIDI FUNCTION only", pageLeds(), [0, 1, 0, 0]);
check("DIRECT SELECT is dark while editing", fcb.hw.aux[4], 0);
check("page 1 shows switch 1's mode digit", fcb.hw.display, fcb.state().pedalModes[0] + 1);
press(2, 100);
check("a footswitch selects itself and shows its mode", [fcb.state().cfgSel, fcb.hw.display], [2, fcb.state().pedalModes[2] + 1]);
const CFG_FLASH_MS = Number(cfgText.match(/#define CONFIG_CHANGED_FLASH_MS\s+(\d+)/)[1]);
const statusOver = (ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(fcb.hw.status); } return [...seen].sort(); };
check("CONFIG LED steady before any change", statusOver(CFG_FLASH_MS * 3), [1]);
const mode3 = fcb.state().pedalModes[2];
press(2, 100);
check("pressing it again changes its mode", fcb.state().pedalModes[2], (mode3 + 1) % 4);
check("after a change the CONFIG LED flashes", statusOver(CFG_FLASH_MS * 3), [0, 1]);
press(2, 100); press(2, 100); press(2, 100);
check("four presses cycle back (four modes)", fcb.state().pedalModes[2], mode3);
check("with the mode back as it was, the CONFIG LED is steady again", statusOver(CFG_FLASH_MS * 3), [1]);
press(UP, 100);
check("UP goes to page 2, which lights MIDI CHANNEL only", [fcb.state().cfgPage, pageLeds()], [1, [1, 0, 0, 0]]);
press(0, 100); press(6, 100);
check("footswitches 1 then 6 enter channel 1, then 6", fcb.state().midiChannel, 6);
fcb.down(10); wait(60);
check("switch 10 enters channel 10 and shows 10", [fcb.state().midiChannel, fcb.hw.display, fcb.hw.blank], [10, 10, 0]);
fcb.up(10); wait(260);
check("its 0 flashes (off after 250 ms)", fcb.hw.blank & 1, 1);
wait(250);
check("(on again after 500 ms)", fcb.hw.blank & 1, 0);
press(3, 100);
check("then switch 4 makes channel 14", [fcb.state().midiChannel, fcb.hw.blank], [14, 0]);
press(10, 100); wait(5200);
check("with no second digit, after 5 s it stays 10, shown steadily", [fcb.state().midiChannel, fcb.hw.display, fcb.hw.blank], [10, 10, 0]);
press(1, 100);
check("then switch 2 enters channel 2", fcb.state().midiChannel, 2);
check("footswitches send nothing in config mode", sent(), []);
chordTap();
check("UP + DOWN tapped in config mode does nothing", [fcb.state().mode, fcb.state().cfgPage, fcb.hw.aux[4]], [1, 1, 0]);
press(UP, 100);
check("page 3 lights both EXP LEDs and shows Lo", [pageLeds(), fcb.hw.text], [[0, 0, 1, 1], TEXT_LO]);
fcb.setRaw(0, 0); wait(100);
press(0, 100);
check("a footswitch takes the heel reading (0)", [fcb.hw.text, fcb.hw.display], [0, 0]);
press(UP, 100);
check("page 4 lights both EXP LEDs and shows Hi", [pageLeds(), fcb.hw.text], [[0, 0, 1, 1], TEXT_HI]);
press(0, 100);
check("a toe reading with unmoved pedals shows Er", fcb.hw.text, TEXT_ER);
fcb.setRaw(255, 255); wait(100);
press(0, 100);
check("toe at 255 (heel 0, the default range, so later checks hold): 0", [fcb.hw.text, fcb.hw.display], [0, 0]);
press(UP, 100);
check("page 5 lights SWITCH 1, the display is blank", [fcb.state().cfgPage, fcb.hw.aux[5], fcb.hw.aux[6], fcb.hw.blank], [4, 1, 0, 7]);
press(3, 100); chordTap();
check("switch 4 and UP + DOWN are relay 1 triggers (their LEDs lit)", [fcb.state().swMask[0], fcb.hw.leds[3], fcb.hw.aux[4]], [8 | 1024, 1, 1]);
fcb.setRaw(255, 255); wait(100); fcb.setRaw(128, 255); wait(100);
check("moving pedal A sets relay 1's trigger value, shown, EXP 1 lit", [fcb.state().swThr[0] > 0, fcb.hw.display === fcb.state().swThr[0], fcb.hw.aux[2]], [true, true, 1]);
press(UP, 100);
check("page 6 lights SWITCH 2", [fcb.state().cfgPage, fcb.hw.aux[5], fcb.hw.aux[6], fcb.hw.leds[3]], [5, 0, 1, 0]);
// Page 7: every LED lit at once while "riban" scrolls, then the version from version.h,
// repeating.
const verText = readFileSync(new URL("../src/version.h", import.meta.url), "utf8");
// Major and minor from version.h, the build number from version_build.inc.
const verText2 = verText + readFileSync(new URL("../src/version_build.inc", import.meta.url), "utf8");
const VER = ["MAJOR", "MINOR", "BUILD"].map((k) => Number(verText2.match(new RegExp("#define FW_VERSION_" + k + " (0x[0-9A-Fa-f]+)"))[1]));
fcb.down(UP); wait(60); fcb.up(UP); wait(40);
check("UP goes to page 7: every LED lit at once, \"riban\" starts", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.text], [6, 4, SCROLL_TEXT[0]]);
// Page 7 opened at most 40 ms ago; each frame is half a version part's time.
const pageFrames = [];
for (let t = 0; t < BOOT_SHOW * 3 - 104; t += 8) { wait(8); if (fcb.hw.text && pageFrames[pageFrames.length - 1] !== fcb.hw.text) pageFrames.push(fcb.hw.text); }
check("the scroll runs through every frame", pageFrames, SCROLL_TEXT);
wait(560);
check("major version with a trailing dot", [fcb.hw.hex, fcb.hw.dots, fcb.hw.lamp], [VER[0], 1, 4]);
wait(1000);
check("then minor with leading and trailing dots", [fcb.hw.hex, fcb.hw.dots], [VER[1], 5]);
wait(1000);
check("then build with a leading dot", [fcb.hw.hex, fcb.hw.dots], [VER[2], 4]);
// The page draws it: lit segments of the two hex digits plus the leading dot.
const SEGS = [6, 2, 5, 5, 4, 5, 6, 3, 7, 6, 6, 5, 4, 5, 5, 4];
const litNow = () => (sandbox.document.getElementById("display").innerHTML.match(/var\(--seg-on\)/g) || []).length;
check("the display draws it (segments lit)", litNow(), SEGS[VER[2] >> 4] + SEGS[VER[2] & 15] + 1);
check("and sends nothing", sent(), []);
// Then a blank display, LEDs still lit, before major again.
wait(1000);
check("then the display is blank (no dots), LEDs still lit", [fcb.hw.hex, fcb.hw.blank, fcb.hw.dots, fcb.hw.lamp, litNow()], [-1, 7, 0, 4, 0]);
wait(1000);
check("then the scroll again", [fcb.hw.text, fcb.hw.dots], [SCROLL_TEXT[0], 0]);
wait(BOOT_SHOW * 3);
check("then major again", [fcb.hw.hex, fcb.hw.dots], [VER[0], 1]);
// It keeps repeating while the page is open, the LEDs lit all along.
// Over two more cycles every part (and the blank, -1) shows again.
// The drawn footswitch LEDs: at most one dark at a time, switches 1 to 10 in turn.
const darkNow = () => [...Array(10).keys()].filter((i) => !sandbox.document.getElementById("led" + i).classList.contains("on"));
const pageLamps = new Set(), pageParts = new Set(), darkSeq = [];
let darkMax = 0;
for (let t = 0; t < 14000; t += 8) {
  wait(8); pageLamps.add(fcb.hw.lamp); pageParts.add(fcb.hw.hex);
  const d = darkNow(); darkMax = Math.max(darkMax, d.length);
  if (d.length && darkSeq[darkSeq.length - 1] !== d[0]) darkSeq.push(d[0]);
}
check("and keeps repeating with every LED lit", [[...pageParts].sort((x, y) => x - y), [...pageLamps]], [[-1, ...new Set(VER)].sort((x, y) => x - y), [4]]);
check("each step blinks its footswitch LED, 1 to 10 in turn (one at a time)",
  [darkMax, new Set(darkSeq).size, darkSeq.every((v, i) => i === 0 || v === (darkSeq[i - 1] + 1) % 10)], [1, 10, true]);
// Settings were changed on the earlier pages, so the drawn CONFIG LED flashes on page 7
// while the other LEDs stay lit.
const drawnOver = (id, ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(sandbox.document.getElementById(id).classList.contains("on")); } return [...seen].sort(); };
check("with unsaved changes the CONFIG LED flashes on page 7", [fcb.hw.lamp, drawnOver("ledCfg", CFG_FLASH_MS * 3)], [4, [false, true]]);
check("while the other LEDs stay lit", drawnOver("ledChan", CFG_FLASH_MS * 3), [true]);
// Footswitch 8 held: the minor version stays, LED 8 dark; released, it carries on.
fcb.down(8); wait(60);
check("holding footswitch 8 shows the minor version, its LED dark", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
wait(BOOT_SHOW * 9);
check("and keeps it while held", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
fcb.up(8); wait(BOOT_SHOW + 600);
check("released: it carries on to the build (its LED lit again after its flash)", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[2], 4, []]);
fcb.down(0); wait(60);
check("footswitch 1 shows \" r\", its LED dark", [fcb.hw.text, fcb.hw.dots, darkNow()], [SCROLL_TEXT[0], 0, [0]]);
fcb.up(0); wait(BOOT_SHOW / 2 + 100);
check("then \"ri\"", fcb.hw.text, SCROLL_TEXT[1]);
check("and nothing is sent", sent(), []);
press(UP, 100);
check("UP on page 7 stays on page 7 (no wrap)", fcb.state().cfgPage, 6);
press(DOWN, 100);
check("DOWN leaves page 7: lamp test off, dots off", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.dots], [5, 0, 0]);
press(DOWN, 100); press(DOWN, 100); press(DOWN, 100); press(DOWN, 100); press(DOWN, 100);
check("DOWN six times in all goes back to page 1", fcb.state().cfgPage, 0);
press(DOWN, 100);
check("DOWN on page 1 stays on page 1 (no wrap)", fcb.state().cfgPage, 0);
fcb.setRaw(0, 0); wait(100);
longKey(UP);
check("a long UP saves and leaves", [fcb.state().mode, fcb.hw.status], [0, 0]);
check("and the CONFIG LED stays dark in run mode", statusOver(CFG_FLASH_MS * 3), [0]);
// The save reached the emulator's EEPROM, so going back in finds nothing unsaved: steady.
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("re-entering after the save: CONFIG LED steady (settings match the EEPROM)", [fcb.state().mode, statusOver(CFG_FLASH_MS * 3)], [1, [1]]);
longKey(DOWN);
check("and a cancel leaves with nothing changed", [fcb.state().mode, fcb.hw.status], [0, 0]);
// Switch 3 was cycled back to its mode; if that is inverted momentary (default 3), saving
// asserts it (state on, LED lit) without sending a CC. No program change either way.
check("with nothing sent and no program change", [sent(), fcb.state().program], [[], p]);
check("switch 3's LED after saving", fcb.hw.leds[2], DEF_MODE === 3 ? 1 : 0);
check("the SWITCH 1 record was stored", JSON.parse(store["fcb1010-eeprom-v2"]).sw1.slice(0, 2), [8, 4]);
check("the settings record was stored", JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0], 2);
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
press(UP, 100);
press(8, 100);
check("in config again, switch 8 enters channel 8 on page 2", fcb.state().midiChannel, 8);
longKey(DOWN);
check("a long DOWN cancels and leaves: channel 2 again", [fcb.state().mode, fcb.state().midiChannel], [0, 2]);
check("with no program change", [sent(), fcb.state().program], [[], p]);
// A long UP + DOWN in config mode saves and leaves too.
const longChord = () => { fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200); };
longChord(); press(UP, 100); press(4, 100);
longChord();
check("a long UP + DOWN saves and leaves: channel 5 stored", [fcb.state().mode, fcb.hw.status, JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0]], [0, 0, 5]);
check("without toggling DIRECT SELECT or changing the program", [sent(), fcb.hw.aux[4], fcb.state().program], [[], 0, p]);
longChord(); press(UP, 100); press(1, 100); longChord();
check("and back to channel 2", [fcb.state().mode, fcb.state().midiChannel], [0, 2]);
fcb.reset(); wait(400);
check("after a power cycle the channel is still 2", fcb.state().midiChannel, 2);
// Start-up lights the inverted momentary switches (their resting state), sending nothing.
check("after a power cycle only inverted momentary switches are lit, nothing sent",
      [fcb.hw.leds.slice(), sent()], [fcb.state().pedalModes.map((m) => (m === 3 ? 1 : 0)), []]);
press(0, 100);
check("and switch 1 now sends on channel 2", sent(), MOM ? [cc(1, 0, ON), cc(1, 0, OFF)] : [cc(1, 0, 127)]);

console.log("emulator: relays and MIDI IN flash");
fcb.setRaw(0, 0); wait(100);
// Relay 1 follows footswitch 4 (and pedal A, at the heel): it matches switch 4's LED, which
// is lit from start-up when the default mode is inverted momentary.
check("relay 1 matches its only trigger that is on (footswitch 4)", [fcb.state().relays & 1, fcb.hw.relays & 1, fcb.hw.aux[5]], [fcb.hw.leds[3], fcb.hw.leds[3], fcb.hw.leds[3]]);
// Relay 1 follows footswitch 4 whatever its mode: compare with the switch LED.
fcb.down(3); wait(60);
check("relay 1 and SWITCH 1 follow footswitch 4 while pressed", [fcb.hw.relays & 1, fcb.hw.aux[5]], [fcb.hw.leds[3], fcb.hw.leds[3]]);
fcb.up(3); wait(200);
check("and after release", [fcb.hw.relays & 1, fcb.hw.aux[5]], [fcb.hw.leds[3], fcb.hw.leds[3]]);
if (fcb.hw.leds[3] && DEF_MODE < 2) { press(3, 100); }
wait(20); sent();
check("relay 1 is open whenever footswitch 4 is off", fcb.hw.leds[3] ? 1 : fcb.hw.relays & 1, fcb.hw.leds[3] ? 1 : 0);
if (!fcb.hw.leds[3]) {
  fcb.setRaw(255, 0); wait(100); sent();
  check("pedal A past its trigger value closes relay 1", fcb.hw.relays & 1, 1);
  fcb.setRaw(0, 0); wait(100); sent();
  check("and back below opens it", fcb.hw.relays & 1, 0);
}
fcb.midiIn([0xfe]); wait(4);
check("active sensing does not flash MIDI CHAN", fcb.hw.aux[0], 0);
fcb.midiIn([0xb5, 1, 2]); wait(4);
check("received MIDI flashes MIDI CHAN", fcb.hw.aux[0], 1);
wait(100);
check("for a moment only", fcb.hw.aux[0], 0);

console.log("emulator: expression pedal");
fcb.setRaw(200, 0); wait(100);
check("pedal A at 200 of 255 sends CC 7", sent(), ["b1 07 64"]);

console.log(fail ? "emulator smoke test FAILED" : "emulator smoke test passed");
process.exit(fail);
