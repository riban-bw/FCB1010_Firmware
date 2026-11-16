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
let lastBlob = null, lastDownload = null;
const store = {};
const sandbox = {
  document: {
    getElementById: (id) => (elements[id] ||= fakeElement()),
    createElement: () => Object.assign(fakeElement(), { click() { lastDownload = this.download; }, remove() {} }),
    body: { appendChild() {} },
    querySelectorAll: () => [],
    addEventListener() {}
  },
  localStorage: {
    getItem: (k) => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
    removeItem: (k) => { delete store[k]; }
  },
  performance: { now: () => 0 },
  // Save to file: the download path (an <a download> clicked) and, when a test sets it,
  // window.showSaveFilePicker.
  Blob: class { constructor(parts) { this.text = parts.join(""); } },
  URL: { createObjectURL: (b) => { lastBlob = b; return "blob:1"; }, revokeObjectURL() {} },
  setInterval: () => 0,
  setTimeout: () => 0,
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
const hex = (n) => n.toString(16).padStart(2, "0");
const cc = (ch, sw, v) => "b" + ch + " " + hex(STOMP_CC[sw]) + " " + hex(v);
function chordTap() { fcb.down(UP); fcb.down(DOWN); wait(100); fcb.up(UP); fcb.up(DOWN); wait(200); }
// MIDI CHANNEL, MIDI FUNCTION, EXP 1, EXP 2.
const pageLeds = () => [fcb.hw.aux[0], fcb.hw.aux[1], fcb.hw.aux[2], fcb.hw.aux[3]];
const TEXT_ER = 1, TEXT_LO = 2, TEXT_HI = 3, TEXT_DASHES = 13;   // hw.text is HAL_TEXT_* + 1

await fcb.ready;

console.log("emulator: power-on and idle");
// Power-on: every LED lit at once (no blinking) while "riban" scrolls once (no version),
// then run mode with program 0.
// Times from config.h; the scan starts after STARTUP_TICKS of settling.
const cfgMs = (k) => Number(cfgText.match(new RegExp("#define " + k + "\\s+(\\d+)"))[1]);
const BOOT_LAMP = cfgMs("BOOT_LAMP_MS"), BOOT_SHOW = cfgMs("VERSION_SHOW_MS"), BOOT_SETTLE = cfgMs("STARTUP_TICKS") * cfgMs("TICK_MS");
// One scroll frame; power-on is ten of them, the start-up settling included.
const FRAME = cfgMs("SCROLL_FRAME_MS");
// The "riban" frames as hw.text values (HAL_TEXT_SCROLL + 1 onwards), the dot of each as
// the strip ". riban ." scrolls across the three dots (hw.dots; bit 2 is after the "1"),
// and what the page draws for each, its dot included (full-height letters: the scroll's
// r 4 segments, i 2, b 5, a 6, n 5): " r" = 4 + dot, "ri" 6, "ib" 7, "ba" 11, "an" 11,
// "n " 5 + dot. Before " r" a lead-in (blank, the dot far right
// then between the digits, a frame each), after "n " the dot running off (blank, between
// the digits, then after the "1").
const SCROLL = [" r", "ri", "ib", "ba", "an", "n "], SCROLL_TEXT = SCROLL.map((w, i) => 4 + i);
const SCROLL_DOTS = [4, 0, 0, 0, 0, 1];
const SCROLL_SEGS = [5, 6, 7, 11, 11, 6];
const litSegs = () => (sandbox.document.getElementById("display").innerHTML.match(/var\(--seg-on\)/g) || []).length;
// Sampled every 8 ms (wait() runs in 8 ms steps) until it ends: frames in order, LEDs lit.
const bootFrames = [], bootLamps = new Set(), bootSegs = new Set(), bootDots = [], bootSeq = [];
const dispKey = () => fcb.hw.text + ":" + fcb.hw.dots;
bootSeq.push(dispKey());
// Lead-in 2 frames, text 6, tail 2: ten frames in all, the settling included.
for (let t = 0; t < FRAME * 10 - 48; t += 8) {
  wait(8);
  if (bootSeq[bootSeq.length - 1] !== dispKey()) bootSeq.push(dispKey());
  if (fcb.hw.text && bootFrames[bootFrames.length - 1] !== fcb.hw.text) bootDots.push(fcb.hw.dots);
  // Every LED lit for BOOT_LAMP_MS from power-on, then the normal LEDs (8ms either side).
  if (t + 8 < BOOT_LAMP - 8) bootLamps.add("lit:" + fcb.hw.lamp);
  if (t + 8 > BOOT_LAMP + 8) bootLamps.add("after:" + fcb.hw.lamp);
  if (fcb.hw.text && bootFrames[bootFrames.length - 1] !== fcb.hw.text) bootFrames.push(fcb.hw.text);
  if (fcb.hw.text) bootSegs.add(fcb.hw.text + ":" + litSegs());
}
check("at power-on \"riban\" scrolls: " + SCROLL.map((w) => '"' + w + '"').join(", "), bootFrames, SCROLL_TEXT);
check("drawn with the right segments", [...bootSegs], SCROLL_TEXT.map((t, i) => t + ":" + SCROLL_SEGS[i]));
check("each text frame's dot, as \". riban .\" scrolls across the three dots", bootDots, SCROLL_DOTS);
check("the whole sequence (text:dots): lead-in dot, the text, the trailing dot running off", bootSeq,
      ["0:1", "0:2"].concat(SCROLL_TEXT.map((t, i) => t + ":" + SCROLL_DOTS[i]), ["0:2", "0:4"]));
check("every LED lit for BOOT_LAMP_MS, then the normal LEDs; no version shown", [[...bootLamps], fcb.hw.hex],
      [BOOT_LAMP < FRAME * 10 - 40 ? ["lit:3", "after:0"] : ["lit:3"], -1]);
wait(100);
check("then run mode: normal LEDs, no dots, bank 1 (bank mode, DIRECT SELECT dark)", [fcb.hw.lamp, fcb.hw.hex, fcb.hw.text, fcb.hw.dots, fcb.hw.display, fcb.state().mode, fcb.hw.aux[4]], [0, -1, 0, 0, 1, 0, 0]);
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
chordTap();
check("UP + DOWN tapped: program mode, DIRECT SELECT lit, the program (0) shown, nothing sent", [fcb.hw.aux[4], fcb.hw.display, sent()], [1, 0, []]);
check("the hint says program mode", /program mode \(DIRECT SELECT lit\)/.test(sandbox.document.getElementById("hint").textContent), true);
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
// Footswitches choose programs by number: 1-9 at once; 10 shows "--", then digits.
// Key codes: 0-4 are footswitches 1-5, 6-10 footswitches 6-10 (5 is DOWN).
const swKey = (n) => n <= 5 ? n - 1 : n;
const typeDigits = (d) => { for (const c of String(d)) press(c === "0" ? 10 : swKey(Number(c)), 60); };
press(2, 60);
check("footswitch 3 sends program 3 at once, toggling no stomp", [sent(), fcb.hw.display, fcb.hw.leds[2]], [["c0 03"], 3, 0]);
press(10, 60);
check("footswitch 10 shows -- and sends nothing", [fcb.hw.text, sent()], [TEXT_DASHES, []]);
// The right-hand dot flashes while the number is incomplete.
const dotSeen = [];
for (let t = 0; t < 600; t += 50) { wait(50); dotSeen.push(fcb.hw.dots & 1); }
check("the right-hand dot flashes while typing", dotSeen.includes(0) && dotSeen.includes(1), true);
wait(50);
check("the hint says a number is being typed", /typing a program number/.test(sandbox.document.getElementById("hint").textContent), true);
typeDigits(56);
check("5, 6: 56 is complete, sent at once", [sent(), fcb.hw.display], [["c0 38"], 56]);
wait(600);
check("and the dot is out once it is complete", fcb.hw.dots, 0);
press(10, 60); typeDigits(123);
check("10, 1, 2, 3: 123 is sent", [sent(), fcb.hw.display], [["c0 7b"], 123]);
press(10, 60); typeDigits(4); press(DOWN, 100);
check("10, 4, DOWN: cancelled, back to the current program", [sent(), fcb.hw.display, fcb.state().program], [[], 123, 123]);
press(10, 60); typeDigits(p); press(UP, 100);
check("10, " + p + ", UP (ENTER): program " + p + " sent", [sent(), fcb.state().program], [["c0 " + hex(p)], p]);
chordTap();
check("UP + DOWN again: bank mode, DIRECT SELECT dark, bank 1 shown", [fcb.hw.aux[4], fcb.hw.display, sent()], [0, 1, []]);

console.log("emulator: MIDI IN");
fcb.midiIn([0xb0, STOMP_CC[1], 127]); wait(20);
check("CC " + STOMP_CC[1] + " = 127 on channel 1 lights switch 2", fcb.hw.leds[1], 1);
check("and nothing is echoed", sent(), []);
// CC 20 (once the UP + DOWN controller) is an ordinary CC: it leaves DIRECT SELECT alone.
fcb.midiIn([0xb0, 20, 127]); wait(20);
check("CC 20 = 127 does not light DIRECT SELECT", [fcb.hw.aux[4], sent()], [0, []]);

console.log("emulator: footswitch banks (the default, DIRECT SELECT dark)");
check("the display shows the footswitch bank (1)", [fcb.hw.aux[4], fcb.hw.display, fcb.state().bank], [0, 1, 0]);
press(UP, 100); wait(50);
const bankMsgs = sent();
check("UP chooses bank 2 instead of sending a program change",
      [fcb.hw.display, fcb.state().bank, fcb.state().program, bankMsgs.some((m) => /^c/.test(m))], [2, 1, p, false]);
check("the footswitches of a bank never saved are the defaults", fcb.state().msgs.map((m) => m.num), STOMP_CC);
check("switch 2's CC (the same in both banks) stays on, and the change sends nothing",
      [fcb.hw.leds[1], bankMsgs], [1, []]);
press(DOWN, 100); wait(50); sent();
check("DOWN goes back to bank 1", [fcb.hw.display, fcb.state().bank], [1, 0]);
chordTap();
check("an UP + DOWN tap lights DIRECT SELECT, the program on the display, sending nothing", [fcb.hw.aux[4], fcb.hw.display, fcb.state().program, sent()], [1, p, p, []]);
chordTap();
check("a second tap turns it off, the bank back on the display", [fcb.hw.aux[4], fcb.hw.display, fcb.state().bank, sent()], [0, 1, 0, []]);

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
press(UP, 100);
// Page 7: the bank's program change, DIRECT SELECT lit, "--" (none) until set.
check("UP goes to page 7: DIRECT SELECT lit, \"--\"", [fcb.state().cfgPage, fcb.hw.aux[4], fcb.hw.aux[6], fcb.hw.text, fcb.hw.lamp, fcb.state().bankPc], [6, 1, 0, 13, 0, 0]);
press(1, 100); press(4, 100);
check("footswitches 2 and 5 type program 25", [fcb.hw.display, fcb.state().bankPc, sent()], [25, 26, []]);
chordTap();
check("a short UP + DOWN: none (\"--\")", [fcb.hw.text, fcb.state().bankPc], [13, 0]);
// Page 8: every LED lit at once while "riban" scrolls, then the version from version.h,
// repeating.
const verText = readFileSync(new URL("../src/version.h", import.meta.url), "utf8");
// Major and minor from version.h, the build number from version_build.inc.
const verText2 = verText + readFileSync(new URL("../src/version_build.inc", import.meta.url), "utf8");
const VER = ["MAJOR", "MINOR", "BUILD"].map((k) => Number(verText2.match(new RegExp("#define FW_VERSION_" + k + " (0x[0-9A-Fa-f]+)"))[1]));
fcb.down(UP); wait(60); fcb.up(UP); wait(40);
check("UP goes to page 8: every LED lit at once, the scroll's lead-in dot", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [7, 4, 0, 7, 1]);
// Page 8 opened at most 40 ms ago; the lead-in and the text are eight frames.
const pageFrames = [];
for (let t = 0; t < FRAME * 8 - 104; t += 8) { wait(8); if (fcb.hw.text && pageFrames[pageFrames.length - 1] !== fcb.hw.text) pageFrames.push(fcb.hw.text); }
check("the scroll runs through every frame", pageFrames, SCROLL_TEXT);
// The trailing dot (two frames), then halfway into major.
wait(FRAME * 2 + 64 + BOOT_SHOW / 2);
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
// From halfway into the blank to halfway into the lead-in's first frame.
wait(BOOT_SHOW / 2 + FRAME / 2);
check("then the lead-in: the leading dot alone, far right", [fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [0, 7, 1]);
wait(FRAME);
check("then the leading dot between the digits", [fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [0, 7, 2]);
wait(FRAME);
check("then the scroll again", [fcb.hw.text, fcb.hw.dots], [SCROLL_TEXT[0], SCROLL_DOTS[0]]);
// Halfway into " r": the rest of the text and the trailing dot, then halfway into major.
wait(FRAME * 7.5 + BOOT_SHOW / 2);
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
// Settings were changed on the earlier pages, so the drawn CONFIG LED flashes on page 8
// while the other LEDs stay lit.
const drawnOver = (id, ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(sandbox.document.getElementById(id).classList.contains("on")); } return [...seen].sort(); };
check("with unsaved changes the CONFIG LED flashes on page 8", [fcb.hw.lamp, drawnOver("ledCfg", CFG_FLASH_MS * 3)], [4, [false, true]]);
check("while the other LEDs stay lit", drawnOver("ledChan", CFG_FLASH_MS * 3), [true]);
// Footswitch 8 held: the minor version stays, LED 8 dark; released, it carries on.
fcb.down(8); wait(60);
check("holding footswitch 8 shows the minor version, its LED dark", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
wait(BOOT_SHOW * 9);
check("and keeps it while held", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
fcb.up(8); wait(BOOT_SHOW + 600);
check("released: it carries on to the build (its LED lit again after its flash)", [fcb.hw.hex, fcb.hw.dots, darkNow()], [VER[2], 4, []]);
fcb.down(0); wait(60);
check("footswitch 1 shows \" r\", its LED dark", [fcb.hw.text, fcb.hw.dots, darkNow()], [SCROLL_TEXT[0], SCROLL_DOTS[0], [0]]);
fcb.up(0); wait(FRAME + 100);
check("then \"ri\"", fcb.hw.text, SCROLL_TEXT[1]);
check("and nothing is sent", sent(), []);
press(UP, 100);
check("UP on page 8 stays on page 8 (no wrap)", fcb.state().cfgPage, 7);
press(DOWN, 100);
check("DOWN leaves page 8 for page 7: lamp test off, dots off, DIRECT SELECT lit", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.dots, fcb.hw.aux[4]], [6, 0, 0, 1]);
press(DOWN, 100);
check("then page 6", fcb.state().cfgPage, 5);
press(DOWN, 100); press(DOWN, 100); press(DOWN, 100); press(DOWN, 100); press(DOWN, 100);
check("DOWN back to page 1", fcb.state().cfgPage, 0);
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
// Switch 3 was cycled back to its mode, so saving puts it in that mode's resting state,
// which it is already in (on for inverted momentary, from power-on; off otherwise): no CC
// is sent. No program change either way.
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

console.log("emulator: footswitch editor");
fcb.setRaw(0, 0); wait(100); sent();
const TEXT_CC = 10, TEXT_NOTE = 11;   // HAL_TEXT_CC / HAL_TEXT_NOTE + 1
const label = (i) => sandbox.document.getElementById("cc" + i).textContent;
const chHex = (fcb.state().midiChannel - 1).toString(16);
// SELECT, NUMBER, VALUE 1, VALUE 2 (HAL_LED_SELECT .. HAL_LED_VALUE_2).
const editLeds = () => [fcb.hw.aux[7], fcb.hw.aux[8], fcb.hw.aux[9], fcb.hw.aux[10]];
longChord();
check("config mode, page 1", [fcb.state().mode, fcb.state().cfgPage], [1, 0]);
// A long press of footswitch 3 opens its editor on the message type.
press(2, OVER_MS);
check("a long footswitch press opens its editor on the type: SELECT lit, \"CC\" shown",
      [fcb.state().edit, editLeds(), fcb.hw.text], [1, [1, 0, 0, 0], TEXT_CC]);
press(1, 100);
check("footswitch 2 makes it a note (\"nt\")", fcb.hw.text, TEXT_NOTE);
press(UP, 100);
check("UP: the number, NUMBER lit", [fcb.state().edit, editLeds()], [2, [0, 1, 0, 0]]);
press(6, 100); press(10, 100);
check("footswitches 6 and 10 type 60", [fcb.hw.display, fcb.hw.text], [60, 0]);
press(UP, 100);
press(0, 100); press(10, 100); press(10, 100);
check("UP: the on value (VALUE 1 lit), 1 0 0 types 100", [fcb.state().edit, editLeds(), fcb.hw.display], [3, [0, 0, 1, 0], 100]);
press(UP, 100);
press(1, 100); press(10, 100);
check("UP: the off value (VALUE 2 lit), 2 10 types 20", [fcb.state().edit, editLeds(), fcb.hw.display], [4, [0, 0, 0, 1], 20]);
press(UP, OVER_MS);
check("a long UP accepts: editor closed, its LEDs dark, still in config mode", [fcb.state().edit, editLeds(), fcb.state().mode], [0, [0, 0, 0, 0], 1]);
check("the footswitch label shows the new message", label(2), "Note 60");
press(UP, OVER_MS);
check("a long UP then saves and leaves", fcb.state().mode, 0);
check("footswitch 3's record was stored (note, 60, on 100, off 20)", JSON.parse(store["fcb1010-eeprom-v2"]).fs2, [1 | (fcb.state().pedalModes[2] << 2), 60, 100, 20]);
wait(20); sent();
press(2, 100); wait(20);
const notes = sent();
check("footswitch 3 now sends note 60 (velocity 100 on, note off velocity 20)",
      notes.length > 0 && notes.every((m) => (m === "9" + chHex + " 3c 64" || m === "8" + chHex + " 3c 14")) &&
      notes.some((m) => m === "9" + chHex + " 3c 64"), true);

console.log("emulator: program change toggle");
longChord();
press(3, OVER_MS);                     // footswitch 4's editor
press(2, 100);                         // type: program change
press(UP, 100);
check("UP skips NUMBER: a program change starts at VALUE 1", [fcb.state().edit, editLeds()], [3, [0, 0, 1, 0]]);
press(4, 100);                         // program sent on: 5
press(UP, 100); press(6, 100);         // program sent off: 6
check("VALUE 2 is the second program", [fcb.state().edit, editLeds(), fcb.state().msgs[3].on, fcb.state().msgs[3].off], [4, [0, 0, 0, 1], 5, 6]);
press(UP, 100);
check("UP stops there", fcb.state().edit, 4);
press(UP, OVER_MS);
check("footswitch 4's label shows both programs", label(3), "PC 5/6");
press(UP, OVER_MS);
wait(50); sent();
const pc = (n) => "c" + chHex + " " + hex(n);
const twoPresses = [[], []];
press(3, 100); wait(20); twoPresses[0] = sent();
press(3, 100); wait(20); twoPresses[1] = sent();
const wantPc = DEF_MODE === 2 ? [[pc(5), pc(6)], [pc(5), pc(6)]] :
               DEF_MODE === 3 ? [[pc(6), pc(5)], [pc(6), pc(5)]] : [[pc(5)], [pc(6)]];
check("it toggles between programs 5 and 6 (following the default pedal mode)", twoPresses, wantPc);

console.log("emulator: configuration over MIDI (the page's configurator, NRPN)");
const conf = fcb.conf, NP = fcb.NP, NC = fcb.NC;
const rec = () => JSON.parse(store["fcb1010-eeprom-v2"]);
wait(100); sent();
check("before any read the panel holds the emulator's settings from page load (the defaults here)",
      [conf.known, conf.v[NP.NUM + 2], conf.v[NP.TYPE + 3], conf.v[NP.MODE + 4], typeof conf.v[NP.CHANNEL]], [false, STOMP_CC[2], 0, DEF_MODE, "number"]);
conf.ch = 1 + ((fcb.state().midiChannel) % 16);    // the wrong channel: Find has to locate it
fcb.confRead(true); wait(400);
check("Find reads the pedal and learns its channel", [conf.waiting, conf.ch, !!conf.v], ["", fcb.state().midiChannel, true]);
check("the dump has footswitch 3 (note 60, 100, 20) and 4 (programs 5 / 6)",
      [conf.v[NP.TYPE + 2], conf.v[NP.NUM + 2], conf.v[NP.ON + 2], conf.v[NP.OFF + 2], conf.v[NP.TYPE + 3], conf.v[NP.ON + 3], conf.v[NP.OFF + 3]],
      [1, 60, 100, 20, 2, 5, 6]);
check("and the channel, a mode and a stomp CC", [conf.v[NP.CHANNEL], conf.v[NP.MODE], conf.v[NP.NUM + 9]], [fcb.state().midiChannel, DEF_MODE, STOMP_CC[9]]);
const dumpMsgs = sent();
check("the dump went out as NRPN controllers on the pedal's channel, ending with the end marker",
      [dumpMsgs.length > 0, dumpMsgs.every((m) => new RegExp("^b" + chHex + " (63 46|62 [0-5][0-9a-f]|06 [0-7][0-9a-f]|26 [0-7][0-9a-f])$").test(m)),
       dumpMsgs.slice(-2)], [true, true, ["b" + chHex + " 62 05", "b" + chHex + " 06 00"]]);
fcb.confChanged(NP.NUM + 9, 99);
fcb.confChanged(NP.TRIGGER + 2, 70);
wait(20);
check("a set applies at once, unsaved", [fcb.state().msgs[9].num, fcb.state().swThr[2], rec().fs9 === undefined || rec().fs9[1] !== 99], [99, 70, true]);
fcb.confCommand(NC.SAVE, "saved"); wait(600);
check("Save stores it and the configurator reads it back", [rec().fs9, rec().sw2[2], conf.waiting, conf.v[NP.NUM + 9]], [[fcb.state().pedalModes[9] << 2, 99, 127, 0], 70, "", 99]);
fcb.confChanged(NP.NUM + 9, 98); wait(20);
fcb.confCommand(NC.RELOAD, "dropped"); wait(600);
check("Discard goes back to the saved settings", [fcb.state().msgs[9].num, conf.v[NP.NUM + 9]], [99, 99]);
check("the dump has the bank (1) and the bank count", [conf.v[NP.BANK], conf.v[NP.BANKS] > 0], [1, true]);
fcb.confBank(3); wait(600);
check("choosing bank 3 switches the pedal and reads that bank back",
      [fcb.state().bank, conf.v[NP.BANK], conf.known, conf.v[NP.NUM + 9], fcb.state().msgs[9].num], [2, 3, true, STOMP_CC[9], STOMP_CC[9]]);
fcb.confChanged(NP.NUM + 0, 88); wait(20);
fcb.confCommand(NC.SAVE, "saved"); wait(600);
check("Store keeps bank 3's footswitches in its own records", [rec().b3fs0 && rec().b3fs0[1], rec().fs0 === undefined || rec().fs0[1] !== 88], [88, true]);
fcb.confBank(1); wait(600);
check("and bank 1 is as it was", [fcb.state().bank, conf.v[NP.NUM + 9], conf.v[NP.NUM + 0]], [0, 99, STOMP_CC[0]]);
longChord();
fcb.confRead(false); wait(2000);
check("in config mode the pedal does not answer", [conf.waiting, fcb.state().mode], ["", 1]);
press(DOWN, OVER_MS);

console.log("emulator: settings files (save to file, load from file)");
const el2 = (id) => sandbox.document.getElementById(id);
wait(200);
fcb.confRead(false); wait(400);
const file = JSON.parse(JSON.stringify(fcb.confToFile()));
check("a saved file has the pedal's settings in readable form",
      [file.format, file.channel, file.footswitches[2], file.footswitches[3].type, file.relays.length],
      ["fcb1010-settings", fcb.state().midiChannel, { mode: ["stomp", "stomp + momentary", "momentary", "inverted momentary"][DEF_MODE], type: "note", number: 60, on: 100, off: 20 }, "program", 2]);
const oldCh = file.channel;
file.footswitches[1] = { mode: "momentary", type: "note", number: 77, on: 90, off: 10 };
file.relays[0] = { follows: [3, 7], followsUpDown: true, pedalA: 0, pedalB: 64 };
file.channel = oldCh % 16 + 1;
check("loading it is accepted", fcb.confLoad(file), true);
wait(600);
const st = fcb.state();
check("the pedal took the file's settings, the channel last",
      [st.msgs[1], st.pedalModes[1], st.swMask[0], st.swThr[1], st.midiChannel, conf.ch, conf.unsaved],
      [{ type: 1, num: 77, on: 90, off: 10 }, 2, (1 << 2) | (1 << 6) | (1 << 10), 64, file.channel, file.channel, true]);
check("and reading it back gives the same file", fcb.confToFile(), file);
check("an invalid file is refused and changes nothing",
      [fcb.confLoad({ format: "fcb1010-settings", version: 1, channel: 1, footswitches: [], relays: [] }),
       fcb.confLoad({ format: "other" }), fcb.state().midiChannel], [false, false, file.channel]);
const bad = JSON.parse(JSON.stringify(file)); bad.footswitches[4].on = 200;
check("a value out of range is refused", fcb.confLoad(bad), false);
file.channel = oldCh; fcb.confLoad(file); wait(600);
check("loading a file with only the channel different sends just that", [fcb.state().midiChannel, conf.ch], [oldCh, oldCh]);

// The bank's program change (configuration page 7; NRPN 92 / 93): null for none.
check("a file holds the bank's program change (none: null)", [file.bankProgram, fcb.state().bankPc], [null, 0]);
file.bankProgram = 42; fcb.confLoad(file); wait(600);
check("loading a program sets it, and reading back gives the same file", [fcb.state().bankPc, fcb.confToFile().bankProgram, el2("cfBankPc").value], [43, 42, "42"]);
file.bankProgram = null; fcb.confLoad(file); wait(600);
check("null sets none", [fcb.state().bankPc, fcb.confToFile().bankProgram, el2("cfBankPc").value], [0, null, "-1"]);
const noPc = JSON.parse(JSON.stringify(file)); delete noPc.bankProgram;
fcb.confChanged(NP.BANK_PC, 9); wait(100);
check("the panel sets it over MIDI", fcb.state().bankPc, 10);
fcb.confLoad(noPc); wait(600);
check("a file without it leaves it as it is", fcb.state().bankPc, 10);
const badPc = JSON.parse(JSON.stringify(file)); badPc.bankProgram = 200;
check("a program out of range is refused", fcb.confLoad(badPc), false);
fcb.confChanged(NP.BANK_PC_ON, 0); wait(100);
check("and turns it off", fcb.state().bankPc, 0);

// Save to file: the name in the File box (.json added), downloaded where the browser has no save dialog.
el2("cfFileName").value = "my rig";
fcb.confDownload();
check("without a save dialog the file downloads under the File box's name", [lastDownload, JSON.parse(lastBlob.text).format], ["my rig.json", "fcb1010-settings"]);
el2("cfFileName").value = "a/b:c.json";
check("characters a file name cannot hold become _", fcb.confFileName(), "a_b_c.json");
el2("cfFileName").value = "";
check("an empty name saves as fcb1010-settings.json", fcb.confFileName(), "fcb1010-settings.json");
// With a save dialog (showSaveFilePicker), it offers that name and writes where the user chose.
let offered = null, written = null;
sandbox.showSaveFilePicker = (o) => { offered = o.suggestedName; return Promise.resolve({ name: "chosen.json",
  createWritable: () => Promise.resolve({ write: (t) => { written = t; return Promise.resolve(); }, close: () => Promise.resolve() }) }); };
el2("cfFileName").value = "song 1.json"; lastDownload = null;
fcb.confDownload();
for (let k = 0; k < 10; k++) await new Promise((r) => setImmediate(r));
check("with a save dialog it offers the File box's name and saves where chosen, the box taking the chosen name",
      [offered, JSON.parse(written).format, el2("cfFileName").value, lastDownload], ["song 1.json", "fcb1010-settings", "chosen.json", null]);
sandbox.showSaveFilePicker = () => Promise.reject(Object.assign(new Error("cancelled"), { name: "AbortError" }));
fcb.confDownload();
for (let k = 0; k < 10; k++) await new Promise((r) => setImmediate(r));
check("cancelling the dialog saves nothing", [lastDownload, el2("confStatus").textContent], [null, "Not saved."]);
delete sandbox.showSaveFilePicker;
el2("cfFileName").value = "fcb1010-settings.json";

const el = (id) => sandbox.document.getElementById(id);
conf.known = false; fcb.confRead(false); wait(400);
check("the pedal, port and channel selectors are never disabled; the file and store buttons wait for a read",
      [el("cfTarget").disabled, el("cfOut").disabled, el("cfIn").disabled, el("cfChannel").disabled, el("cfRead").disabled, el("cfFind").disabled],
      [undefined, undefined, undefined, undefined, undefined, undefined]);

console.log("emulator: configurator through Web MIDI (a fake USB interface wired to the emulated pedal)");
// The interface's output feeds the pedal's MIDI IN at cable speed; the pedal's MIDI OUT reaches its input
// as Web MIDI delivers it: one complete message per event, status byte included.
const fakeIn = { id: "in1", name: "USB MIDI In", state: "connected", onmidimessage: null, open: () => Promise.resolve() };
const fakeOut = { id: "out1", name: "USB MIDI Out", state: "connected", open: () => Promise.resolve(), send: (b) => fcb.wire(Array.from(b)) };
sandbox.navigator = { requestMIDIAccess: () => Promise.resolve({ inputs: new Map([["in1", fakeIn]]), outputs: new Map([["out1", fakeOut]]), onstatechange: null }) };
const delivered = new Set(fcb.hw.logLines);
// Wall-clock time (performance.now), which the configurator uses for a real port, follows the test's clock.
let wall = 0;
sandbox.performance.now = () => wall;
function waitMidi(n) {
  for (let t = 0; t < n; t += 8) {
    ms(8); wall += 8;
    [...fcb.hw.logLines].reverse().forEach((l) => {
      if (delivered.has(l)) return;
      delivered.add(l);
      if (l.raw && fakeIn.onmidimessage) fakeIn.onmidimessage({ data: Uint8Array.from(l.raw.split(" ").map((h) => parseInt(h, 16))) });
    });
  }
}
fcb.confTarget("midi");
await new Promise((r) => setTimeout(r, 0));
check("access granted: the only ports are chosen, and nothing is disabled",
      [conf.out && conf.out.id, conf.inp && conf.inp.id, el("cfOut").value, el("cfIn").value, el("cfTarget").disabled, el("cfOut").disabled],
      ["out1", "in1", "out1", "in1", undefined, undefined]);
conf.ch = oldCh % 16 + 1;
fcb.confRead(true); waitMidi(600);
check("Find through the interface reads the pedal", [conf.waiting, conf.ch, !!conf.v, conf.v && conf.v[NP.NUM + 1]], ["", oldCh, true, 77]);
fcb.confChanged(NP.ON + 1, 55); waitMidi(50);
fcb.confCommand(NC.SAVE, "stored"); waitMidi(600);
// The record's type byte carries the pedal mode in bits 2-3.
check("a change and Store reach the pedal", [fcb.state().msgs[1].on, rec().fs1, conf.v[NP.ON + 1]],
      [55, [1 | (fcb.state().pedalModes[1] << 2), 77, 55, 10], 55]);
fakeIn.onmidimessage = null;
const heardBefore = conf.heard;
fcb.confRead(false); waitMidi(2000);
check("with nothing coming back, it says so", [conf.waiting, /nothing at all came in/.test(el("confStatus").textContent) || heardBefore > 0], ["", true]);
console.log("emulator: configurator with a MIDI output only (no input, so no reads)");
conf.inp = null; conf.known = false;
const st0 = JSON.stringify(fcb.state().msgs);
fcb.confRead(false);
check("Read says it needs an input", [conf.waiting, /need a MIDI input/.test(el("confStatus").textContent)], ["", true]);
const oneWay = fcb.confToFile();
oneWay.footswitches[5] = { mode: "stomp", type: "cc", number: 33, on: 100, off: 1 };
oneWay.relays[1] = { follows: [10], followsUpDown: false, pedalA: 12, pedalB: 0 };
check("a file loads with no read first", fcb.confLoad(oneWay), true);
waitMidi(800);
check("every setting was sent (no read, so nothing is assumed), and the pedal took them",
      [fcb.state().msgs[5], fcb.state().pedalModes[5], fcb.state().swMask[1], fcb.state().swThr[2], JSON.stringify(fcb.state().msgs.filter((m, i) => i !== 5)) === JSON.stringify(JSON.parse(st0).filter((m, i) => i !== 5))],
      [{ type: 0, num: 33, on: 100, off: 1 }, 0, 1 << 9, 12, true]);
check("the status says it cannot read back", /cannot read it back/.test(el("confStatus").textContent), true);
fcb.confChanged(NP.OFF + 5, 2); waitMidi(50);
fcb.confCommand(NC.SAVE, "Stored."); waitMidi(600);
check("a change and Store work without an input", [rec().fs5, rec().sw2[2]], [[fcb.state().pedalModes[5] << 2, 33, 100, 2], 12]);
check("saving a file without a read leaves out the firmware version", [fcb.confToFile().firmware, fcb.confToFile().footswitches[5].off], [undefined, 2]);
conf.out = null;
check("with no output either, a load is refused", fcb.confLoad(oneWay), false);
fcb.confTarget("emu");
check("switching back to the emulator works", [conf.target, el("cfTarget").disabled], ["emu", undefined]);

console.log(fail ? "emulator smoke test FAILED" : "emulator smoke test passed");
process.exit(fail);
