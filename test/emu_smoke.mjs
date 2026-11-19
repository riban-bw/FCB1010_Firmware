// Runs the browser editor's own script (with its embedded WebAssembly firmware) under Node,
// with a minimal stand-in for the page, and checks the MIDI it sends through the same window.__fcb
// hook a browser test would use. It checks the editor's glue and the compiled ctl.c / keys.c
// together; it does not check drawing or the real 8051 image.
// Usage: node test/emu_smoke.mjs editor/index.html   (make editor runs it)

import { readFileSync } from "node:fs";
import vm from "node:vm";

const html = readFileSync(process.argv[2] || "editor/index.html", "utf8");
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
    attrs: {}, setAttribute(k, v) { this.attrs[k] = String(v); }, getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; },
    // Listeners are kept, so a check can fire one (fire, below).
    listeners: {}, addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }, setPointerCapture() {}
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
const el0 = (id) => sandbox.document.getElementById(id);
// Run an element's listeners for event type t, as the browser would.
const fire = (id, t) => (el0(id).listeners[t] || []).forEach((f) => f.call(el0(id), { target: el0(id) }));
const pageLeds = () => [fcb.hw.aux[0], fcb.hw.aux[1], fcb.hw.aux[2], fcb.hw.aux[3]];
const TEXT_ER = 1, TEXT_LO = 2, TEXT_HI = 3, TEXT_DASHES = 13;   // hw.text is HAL_TEXT_* + 1

await fcb.ready;

console.log("editor: power-on and idle");
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
{
  const id = sent();
  check("at power-on the SysEx identity goes out, once", [id.length, (id[0] || "").slice(0, 29)], [1, "f0 7e 00 06 02 7d 46 43 31 30"]);
}
wait(500);
check("then nothing sent while idle", sent(), []);

console.log("editor: stomp switches (default mode " + DEF_MODE + ")");
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

console.log("editor: program change");
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
const cfgNum = (name) => Number(cfgText.match(new RegExp("#define " + name + "\\s+(\\d+)"))[1]);
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

console.log("editor: MIDI IN");
fcb.midiIn([0xb0, STOMP_CC[1], 127]); wait(20);
check("CC " + STOMP_CC[1] + " = 127 on channel 1 lights switch 2", fcb.hw.leds[1], 1);
check("and nothing is echoed", sent(), []);
// The MIDI IN card sends a CC, note on, note off or program change on the chosen channel.
fcb.midiIn([0xb0, STOMP_CC[1], 0]); wait(20);
el0("inType").value = "cc"; el0("inCc").value = String(STOMP_CC[1]); el0("inVal").value = "127"; el0("inCh").value = "1";
fire("btnSendIn", "click"); wait(20);
check("the card's CC lights switch 2", fcb.hw.leds[1], 1);
fcb.midiIn([0xb0, STOMP_CC[1], 0]); wait(20);
el0("inType").value = "pc"; fire("inType", "change"); el0("inCc").value = "5";
fire("btnSendIn", "click"); wait(20);
check("a program change from the card is ignored (labels Program, none)", [fcb.state().program, sent(), el0("inNumLabel").textContent, el0("inValLabel").textContent], [p, [], "Program", "(none)"]);
el0("inType").value = "on"; fire("inType", "change");
check("note on labels: Note, Velocity", [el0("inNumLabel").textContent, el0("inValLabel").textContent], ["Note", "Velocity"]);
// Switch 2 on again, as the bank checks below expect.
fcb.midiIn([0xb0, STOMP_CC[1], 127]); wait(20);

console.log("editor: SysEx Identity Request");
sent();
fcb.midiIn([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]); wait(20);
{
  const vn = ["MAJOR", "MINOR"].map((k) => Number(readFileSync(new URL("../src/version.h", import.meta.url), "utf8").match(new RegExp("#define FW_VERSION_" + k + " (\\d+)"))[1]));
  const reply = sent();
  check("an Identity Request (device 7F) gets the identity reply: non-commercial ID 7D, \"FC10\", the version",
        [reply.length, (reply[0] || "").split(" ").slice(0, 12).join(" ")],
        [1, "f0 7e 00 06 02 7d 46 43 31 30 " + hex(vn[0] & 127) + " " + hex(vn[1] & 127)]);
  check("the log names it", fcb.hw.logLines.some((l) => /^SysEx identity: FC10, version /.test(l.text)), true);
}
fcb.midiIn([0xF0, 0x7E, 0x05, 0x06, 0x01, 0xF7]); wait(20);
check("one for another device gets none", sent(), []);

console.log("editor: footswitch banks (the default, DIRECT SELECT dark)");
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

console.log("editor: config mode pages, channel entry, save, cancel and reload");
const longKey = (k) => { fcb.down(k); wait(1300); fcb.up(k); wait(200); };
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("each footswitch shows its CC number", STOMP_CC.map((n, i) => sandbox.document.getElementById("cc" + i).textContent), STOMP_CC.map((n) => "CC " + n));
check("the UP and DOWN switches are labelled UP [ENTER] and DOWN [ESCAPE]",
      [...sandbox.document.getElementById("rows").innerHTML.matchAll(/class="num">(UP|DOWN)<\/span><span class="sub">([^<]*)</g)].map((m) => m[1] + " " + m[2]), ["UP [ENTER]", "DOWN [ESCAPE]"]);
check("UP + DOWN held 1s opens config mode on page 1", [fcb.state().mode, fcb.hw.status, fcb.state().cfgPage], [1, 1, 0]);
check("page 1 lights MIDI FUNCTION only", pageLeds(), [0, 1, 0, 0]);
check("DIRECT SELECT is dark while editing", fcb.hw.aux[4], 0);
// The modes as words (HAL_TEXT_MODE, the 15th text): "St", "+St", "Pu", "-Pu".
const MODE_SHOWN = ["St", "+St", "Pu", "-Pu"];
check("page 1 shows switch 1's number first", [fcb.hw.text, fcb.hw.display], [0, 1]);
wait(cfgMs("CONFIG_SWITCH_SHOW_MS"));
check("then its mode as a word", [fcb.hw.text, el0("display").getAttribute("aria-label")], [15 + fcb.state().pedalModes[0], "Display " + MODE_SHOWN[fcb.state().pedalModes[0]]]);
// The "+" has a long bar (user's request): wider than its upright is thick, and as long as
// most of the upright's height.
{
  const bars = [...el0("display").innerHTML.matchAll(/<path d="M(\d+) (\d+) L(\d+) \2 L\3 (\d+) L\1 \4 Z"/g)].slice(0, 2)
    .map((m) => [Number(m[3]) - Number(m[1]), Number(m[4]) - Number(m[2])]);
  check("the sign's bar is 18 wide and its upright 22 tall", bars, [[18, 6], [6, 22]]);
}
// Footswitch LEDs as drawn: 1 lit, 2 dim (hw.leds), 0 dark.
const ledLevels = () => [...Array(10).keys()].map((i) => el0("led" + i).classList.contains("on") ? 1 : el0("led" + i).classList.contains("dim") ? 2 : 0);
const SHOW_MS = cfgMs("CONFIG_SHOW_MS"), SW_SHOW_MS = cfgMs("CONFIG_SWITCH_SHOW_MS");
fcb.down(2); wait(60);
check("a footswitch selects itself and shows its number, its LED lit", [fcb.state().cfgSel, fcb.hw.text, fcb.hw.display, fcb.state().edit, ledLevels()[2]], [2, 0, 3, 0, 1]);
fcb.up(2); wait(40);
// Then each setting in turn, CONFIG_SHOW_MS each, with its LED: the type (SELECT), the
// number (NUMBER), the on value (VALUE 1), the off value (VALUE 2), then the mode again.
{
  // The setting's LED is steady here (only the editor's flickers).
  const shown = [];
  for (let t = 0; t < SW_SHOW_MS * 2 + SHOW_MS * 5; t += 8) {
    wait(8);
    const leds = [1, 11, 7, 8, 9, 10].map((a) => fcb.hw.aux[a]).join("");
    const k = fcb.hw.text + ":" + (fcb.hw.text ? 0 : fcb.hw.display) + ":" + leds;
    if (shown[shown.length - 1] !== k) shown.push(k);
  }
  const m = (15 + fcb.state().pedalModes[2]) + ":0:110000";
  check("then its settings in turn, each with its steady LED: mode, type, number, on value, off value, then the number again (MIDI FUNCTION lit throughout, SWITCH for the mode)", shown,
        ["0:3:100000", m, "10:0:101000", "0:" + STOMP_CC[2] + ":100100", "0:127:100010", "0:0:100001", "0:3:100000", m]);
}
const CFG_FLASH_MS = Number(cfgText.match(/#define CONFIG_CHANGED_FLASH_MS\s+(\d+)/)[1]);
const statusOver = (ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(fcb.hw.status); } return [...seen].sort(); };
check("CONFIG LED steady before any change", statusOver(CFG_FLASH_MS * 3), [1]);
const mode3 = fcb.state().pedalModes[2];
press(2, 100);
check("pressed again it opens its editor on the mode: MIDI FUNCTION lit, the mode's footswitch (1-4) lit, the others dim",
      [fcb.state().edit, fcb.hw.aux[1], ledLevels()], [1, 1, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((i) => i === mode3 ? 1 : i < 4 ? 2 : 0)]);
check("the footswitches' labels name the modes", [0, 1, 2, 3, 4].map((i) => el0("mode" + i).textContent), ["Mode St", "Mode +St", "Mode Pu", "Mode -Pu", ""]);
press((mode3 + 1) % 4, 100);
check("a footswitch 1-4 chooses its mode", [fcb.state().pedalModes[2], fcb.hw.text], [(mode3 + 1) % 4, 15 + (mode3 + 1) % 4]);
check("after a change the CONFIG LED flashes", statusOver(CFG_FLASH_MS * 3), [0, 1]);
press(mode3, 100);
check("and back", fcb.state().pedalModes[2], mode3);
check("with the mode back as it was, the CONFIG LED is steady again", statusOver(CFG_FLASH_MS * 3), [1]);
press(DOWN, 100);
check("DOWN on the mode closes the editor", [fcb.state().edit, fcb.state().cfgPage, fcb.state().pedalModes[2]], [0, 0, mode3]);
// The MIDI channel is page 6, before the version page (user's request).
const TEXT_PROG = 12, TEXT_ABS = 14, TEXT_PICKUP = 19, TEXT_S1 = 20, TEXT_S2 = 21, TEXT_CL = 22, TEXT_RS = 23, TEXT_AS = 24, TEXT_ON = 25, TEXT_OFF = 26;   // HAL_TEXT_*, + 1
const BANK_LABEL = cfgMs("BANK_PC_LABEL_MS");
const toPage = (n) => { while (fcb.state().cfgPage < n) press(UP, 100); while (fcb.state().cfgPage > n) press(DOWN, 100); };
toPage(1);
check("one UP goes to page 2, which lights MIDI CHANNEL only", [fcb.state().cfgPage, pageLeds()], [1, [1, 0, 0, 0]]);
// Number entry, the same everywhere in config mode: the digits that can come next dim,
// the dot flashing while another digit can follow.
const dotsOver = (ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(fcb.hw.dots & 1); } return [...seen].sort(); };
check("the digits 1-9 can start a channel (dim), not 0", ledLevels(), [2, 2, 2, 2, 2, 2, 2, 2, 2, 0]);
check("the footswitches' labels are digits", [el0("mode0").textContent, el0("mode9").textContent], ["Digit 1", "Digit 0"]);
press(0, 100);
check("footswitch 1: channel 1, and 0-6 can follow (dim), the dot flashing",
      [fcb.state().midiChannel, fcb.hw.display, ledLevels(), dotsOver(600)], [1, 1, [2, 2, 2, 2, 2, 2, 0, 0, 0, 2], [0, 1]]);
press(6, 100);
check("then 6: channel 16, complete (the dot out)", [fcb.state().midiChannel, fcb.hw.display, dotsOver(600)], [16, 16, [0]]);
press(10, 100);
check("0 cannot start a channel", fcb.state().midiChannel, 16);
press(0, 100); press(UP, 100);
check("1 then UP [ENTER]: channel 1, still on page 2", [fcb.state().midiChannel, fcb.state().cfgPage, dotsOver(600)], [1, 1, [0]]);
press(0, 100); press(10, 100);
check("1 then 10: channel 10", [fcb.state().midiChannel, fcb.hw.display, fcb.hw.blank], [10, 10, 0]);
press(3, 100);
press(0, 100); press(DOWN, 100);
check("4, then 1 and DOWN [ESCAPE]: channel 4 again, still on page 2", [fcb.state().midiChannel, fcb.state().cfgPage], [4, 1]);
press(1, 100);
check("then switch 2 enters channel 2", fcb.state().midiChannel, 2);
check("footswitches send nothing in config mode", sent(), []);
chordTap();
check("UP + DOWN tapped in config mode saves and leaves at once", [fcb.state().mode, fcb.hw.status, JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0]], [0, 0, 2]);
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
// Pages 3 and 4: EXP A and EXP B, the same footswitches on both (user's layout): 6 CC,
// 8 pickup, 9 calibration; the page's EXP LED lit, the footswitch selected lit and the
// other two dim. A footswitch first shows its setting, pressed again it acts.
press(UP, 100); press(UP, 100);
const pk0 = fcb.state().pickup & 3;
const pkText = (b) => (b ? TEXT_PICKUP : TEXT_ABS), pkDots = (b) => (b ? 3 : 0);
check("two UPs go to page 3, EXP A: EXP 1 and footswitch 6 lit, 8 and 9 dim, pedal A's CC shown first",
      [fcb.state().cfgPage, pageLeds(), ledLevels(), fcb.hw.aux[4], fcb.hw.text, fcb.hw.display, fcb.hw.dots],
      [2, [0, 0, 1, 0], [0, 0, 0, 0, 0, 1, 0, 2, 2, 0], 0, 0, fcb.state().pedalCc[0], 0]);
check("each footswitch's label says what it does on this page", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((i) => el0("mode" + i).textContent),
      ["", "", "", "", "", "CC", "", "Pickup", "Calibrate", ""]);
press(8, 100);
check("footswitch 8 first shows pedal A's response", [fcb.state().pickup & 3, fcb.hw.text, ledLevels()[7]], [pk0, pkText(pk0 & 1), 1]);
press(8, 100);
check("footswitch 8 (selected) switches pedal A (\"P.U.\" / \"Ab\")", [fcb.state().pickup & 1, fcb.hw.text, fcb.hw.dots], [(pk0 & 1) ^ 1, pkText((pk0 & 1) ^ 1), pkDots((pk0 & 1) ^ 1)]);
check("drawn as \"P.U.\" or \"Ab\"", el0("display").getAttribute("aria-label"), (pk0 & 1) ? "Display Ab" : "Display PU");
press(8, 100);
check("and back", fcb.state().pickup & 3, pk0);
// Footswitch 9: the pedal's value, then "Lo" flashing, the heel with "Hi" flashing, the toe.
fcb.setRaw(255, 0); wait(100);
press(9, 100);
check("footswitch 9 first shows pedal A's value, taking nothing", [fcb.hw.display, ledLevels()[8], fcb.state().calStep, fcb.state().calArmed], [127, 1, 0, 0]);
fcb.setRaw(0, 0); wait(100);
check("following the pedal", fcb.hw.display, 0);
press(9, 100);
check("pressed again: \"Lo\" flashing", [fcb.hw.text, fcb.state().calStep, fcb.state().calArmed, pageLeds()], [TEXT_LO, 1, 1, [0, 0, 1, 0]]);
wait(260);
check("(blank half the time)", fcb.hw.blank, 7);
check("while it runs the footswitches have no labels", el0("mode8").textContent, "");
press(8, 100);
check("footswitch 8 does nothing now", [fcb.state().calStep, fcb.state().calArmed, fcb.state().pickup & 3], [1, 1, pk0]);
press(9, 100);
check("footswitch 9 stops it", [fcb.state().calStep, fcb.state().calArmed, fcb.state().cfgPage], [0, 0, 2]);
press(9, 100);
press(DOWN, 100);
check("DOWN [ESCAPE] on \"Lo\" stops it, still on page 3", [fcb.state().calStep, fcb.state().calArmed, fcb.state().cfgPage], [0, 0, 2]);
press(9, 100);
press(UP, 100);
check("UP [ENTER] takes the heel: \"Hi\" flashing", [fcb.state().calStep, fcb.state().calArmed, fcb.state().cfgPage], [2, 1, 2]);
press(DOWN, 100);
check("DOWN [ESCAPE] on \"Hi\" goes back to \"Lo\"", [fcb.hw.text, fcb.state().calStep], [TEXT_LO, 1]);
press(UP, 100);
press(UP, 100);
check("UP with the pedal still at the heel: \"Er\", the calibration unchanged", [fcb.hw.text, fcb.state().calStep, fcb.state().heel[0], fcb.state().toe[0]], [TEXT_ER, 3, 0, 255]);
check("drawn as \"ER\" (the large r)", el0("display").getAttribute("aria-label"), "Display ER");
wait(cfgMs("CAL_ERROR_SHOW_MS") + 100);
check("CAL_ERROR_SHOW_MS later, the pedal's value again", [fcb.state().calStep, fcb.hw.display, fcb.hw.blank], [0, 0, 0]);
press(9, 100); press(UP, 100);
fcb.setRaw(255, 255); wait(100);
press(UP, 100);
check("Lo, the heel, the toe: taken (heel 0, toe 255, the default range, so later checks hold), the value shown",
      [fcb.state().calStep, fcb.hw.display, fcb.state().heel[0], fcb.state().toe[0]], [0, 127, 0, 255]);
// Footswitch 6: first the CC, then (pressed again) its editor, NUMBER lit, typed; a
// pedal sets nothing (user's request).
const ccA0 = fcb.state().pedalCc[0];
press(6, 100);
check("footswitch 6 first shows pedal A's CC", [fcb.state().pcc & 1, fcb.hw.display, fcb.hw.aux[8], ledLevels()[5]], [0, ccA0, 0, 1]);
press(6, 100);
check("pressed again it opens pedal A's CC editor, NUMBER and EXP 1 lit, every digit dim",
      [fcb.state().pcc, fcb.hw.display, fcb.hw.aux[8], fcb.hw.aux[2], ledLevels(), fcb.state().pickup & 3], [1, ccA0, 1, 1, [2, 2, 2, 2, 2, 2, 2, 2, 2, 2], pk0]);
check("in the CC editor the labels are digits", [el0("mode0").textContent, el0("mode9").textContent], ["Digit 1", "Digit 0"]);
fcb.setRaw(0, 0); wait(100); fcb.setRaw(128, 128); wait(100);
check("moving a pedal leaves the CC alone", fcb.state().pedalCc[0], ccA0);
press(2, 100); press(2, 100);
check("footswitches 3, 3 type CC 33, and the panel's pedal label follows", [fcb.state().pedalCc[0], fcb.hw.display, String(el0("ccA").textContent)], [33, 33, "33"]);
press(DOWN, 100);
check("a short DOWN [ESCAPE] puts the old CC back, still on page 3", [fcb.state().pcc, fcb.state().pedalCc[0], fcb.state().cfgPage, fcb.hw.display, fcb.hw.aux[8]], [0, ccA0, 2, ccA0, 0]);
// Page 4, EXP B: the same footswitches for pedal B.
press(UP, 100);
check("UP goes to page 4, EXP B: EXP 2 and footswitch 6 lit, pedal B's CC", [fcb.state().cfgPage, pageLeds(), ledLevels()[5], fcb.hw.display], [3, [0, 0, 0, 1], 1, fcb.state().pedalCc[1]]);
press(8, 100);
check("footswitch 8 shows pedal B's response", fcb.hw.text, pkText(pk0 >> 1));
press(8, 100);
check("footswitch 8 switches pedal B", fcb.state().pickup & 3, pk0 ^ 2);
press(8, 100);
check("and back", fcb.state().pickup & 3, pk0);
fcb.setRaw(255, 0); wait(100);
press(9, 100);
check("footswitch 9 shows pedal B's value, EXP 2 lit", [fcb.hw.display, pageLeds()], [0, [0, 0, 0, 1]]);
// Both pedals back at the toe, where the SWITCH page below expects pedal B.
fcb.setRaw(255, 255); wait(100);
const ccB0 = fcb.state().pedalCc[1];
press(6, 100); press(6, 100); press(4, 100); press(4, 100); press(UP, 100);
check("footswitch 6 (twice), then 5, 5 and a short UP [ENTER]: pedal B on CC 55", [fcb.state().pcc & 1, fcb.state().pedalCc[1], fcb.state().cfgPage, fcb.hw.display], [0, 55, 3, 55]);
check("the panel's pedal B label follows", String(el0("ccB").textContent), "55");
// Type its CC back (footswitch 10 types 0, 1-5 are codes 0-4, 6-9 codes 6-9); 6 is
// still selected, so one press opens the editor. UP [ENTER] ends a number that could
// go on, then closes the editor.
press(6, 100);
String(ccB0).split("").map(Number).forEach((d) => press(d === 0 ? 10 : d <= 5 ? d - 1 : d, 100));
for (let n = 0; n < 2 && (fcb.state().pcc & 1); n++) press(UP, 100);
check("and typed back", [fcb.state().pcc & 1, fcb.state().pedalCc[1]], [0, ccB0]);
press(UP, 100);
wait(10);
check("page 5 lights SWITCH 1, the display shows \"S.1\"", [fcb.state().cfgPage, fcb.hw.aux[5], fcb.hw.aux[6], fcb.hw.text, fcb.hw.dots, el0("display").getAttribute("aria-label")], [4, 1, 0, TEXT_S1, 2, "Display S1"]);
press(3, 100);
check("switch 4 is a relay 1 trigger (its LED lit), DIRECT SELECT dark", [fcb.state().swMask[0], fcb.hw.leds[3], fcb.hw.aux[4]], [8, 1, 0]);
fcb.setRaw(255, 255); wait(100); fcb.setRaw(128, 255); wait(100);
check("moving pedal A sets relay 1's trigger value, shown, EXP 1 lit", [fcb.state().swThr[0] > 0, fcb.hw.display === fcb.state().swThr[0], fcb.hw.aux[2]], [true, true, 1]);
press(UP, 100);
check("page 6 shows \"S.2\"", [fcb.hw.text, fcb.hw.dots], [TEXT_S2, 2]);
check("page 6 lights SWITCH 2; the footswitches it does not follow, and EXP 1 / EXP 2 without a trigger, dim", [fcb.state().cfgPage, fcb.hw.aux[5], fcb.hw.aux[6], fcb.hw.leds[3], fcb.hw.aux[2], fcb.hw.aux[3]], [5, 0, 1, 2, 2, 2]);
check("drawn dim", [el0("led3").classList.contains("dim"), el0("ledExp1").classList.contains("dim")], [true, true]);
fcb.down(UP); wait(60); fcb.up(UP); wait(40);
// Page 7: the bank's program change, DIRECT SELECT lit, "Pr" and "--" (none) in turn.
check("UP goes to page 7: DIRECT SELECT lit, \"Pr\" first", [fcb.state().cfgPage, fcb.hw.aux[4], fcb.hw.aux[6], fcb.hw.text, fcb.hw.lamp, fcb.state().bankPc], [6, 1, 0, TEXT_PROG, 0, 0]);
wait(BANK_LABEL);
check("then \"--\" (none)", fcb.hw.text, TEXT_DASHES);
wait(BANK_LABEL);
check("then \"Pr\" again", fcb.hw.text, TEXT_PROG);
press(1, 100); press(4, 100);
check("footswitches 2 and 5 type program 25", [fcb.hw.display, fcb.hw.text, fcb.state().bankPc, sent()], [25, 0, 26, []]);
press(10, 100);
check("after a complete number, footswitch 10 shows \"--\", changing nothing yet", [fcb.hw.text, fcb.state().bankPc, fcb.state().pcMode & 2], [13, 26, 2]);
press(UP, 100);
check("then UP [ENTER]: none (\"--\"), still on page 7", [fcb.hw.text, fcb.state().bankPc, fcb.state().cfgPage], [13, 0, 6]);
// Page 8: every LED lit at once while "riban" scrolls, then the version from version.h,
// repeating.
const verText = readFileSync(new URL("../src/version.h", import.meta.url), "utf8");
// Major and minor from version.h, the build number from version_build.inc.
const verText2 = verText + readFileSync(new URL("../src/version_build.inc", import.meta.url), "utf8");
// As hal_display_hex is given them: major and minor in decimal (two BCD digits), the build in hex.
const VER_N = ["MAJOR", "MINOR", "BUILD"].map((k) => Number(verText2.match(new RegExp("#define FW_VERSION_" + k + " (0x[0-9A-Fa-f]+|[0-9]+)"))[1]));
const BCD = (v) => Math.floor(v / 10) * 16 + v % 10;
// Major and minor from 100 are shown as a number (hal_display); counted here as 0x100 + BCD.
const VER = VER_N.map((v, i) => i < 2 ? (v >= 100 ? 0x100 + BCD(v - 100) : BCD(v)) : v);
const verShown = () => fcb.hw.hex >= 0 ? fcb.hw.hex : fcb.hw.display >= 100 ? 0x100 + BCD(fcb.hw.display - 100) : -1;
fcb.down(UP); wait(60); fcb.up(UP); wait(40);
// Page 8, misc: SELECT lit, footswitches 1, 9 and 10 dim (all off), "CL".
check("UP goes to page 8, misc: SELECT lit, footswitches 1, 9 and 10 dim, \"CL\"", [fcb.state().cfgPage, fcb.hw.aux[7], ledLevels(), fcb.hw.text, fcb.state().misc & 7],
      [7, 1, [2, 0, 0, 0, 0, 0, 0, 0, 2, 2], TEXT_CL, 0]);
check("labelled", [el0("mode0").textContent, el0("mode8").textContent, el0("mode9").textContent], ["Clock display", "Running status", "Active sensing"]);
press(0, 100);
check("footswitch 1: the clock display on at once, its LED lit", [fcb.state().misc & 7, fcb.hw.text, el0("display").getAttribute("aria-label"), ledLevels()[0]], [1, TEXT_CL, "Display CL", 1]);
press(9, 100);
check("footswitch 9: running status on, \"rS\"", [fcb.state().misc & 7, fcb.hw.text, el0("display").getAttribute("aria-label"), ledLevels()[8]], [3, TEXT_RS, "Display RS", 1]);
press(10, 100);
check("footswitch 10: active sensing on, \"AS\"", [fcb.state().misc & 7, fcb.hw.text, el0("display").getAttribute("aria-label"), ledLevels()[9]], [7, TEXT_AS, "Display AS", 1]);
wait(600);
check("active sensing is not logged unless asked", sent(), []);
el0("showAS").checked = true;
wait(600);
const asLog = sent();
check("with \"Show active sensing\" ticked it is, about every ACTIVE_SENSE_MS", [asLog.length >= 1, asLog.every((x) => x === "fe")], [true, true]);
el0("showAS").checked = false;
press(0, 100); press(9, 100); press(10, 100);
check("each pressed again: all off, all dim", [fcb.state().misc & 7, ledLevels()[0], ledLevels()[8], ledLevels()[9]], [0, 2, 2, 2]);
check("\"A.S.\" with both dots", [fcb.hw.text, fcb.hw.dots], [TEXT_AS, 3]);
wait(SHOW_MS);
check("then its value, \"Of\"", [fcb.hw.text, fcb.hw.dots, el0("display").getAttribute("aria-label")], [TEXT_OFF, 0, "Display OF"]);
sent();
fcb.down(UP); wait(60); fcb.up(UP); wait(40);
check("UP goes to page 9: every LED lit at once (the lamp test), the scroll's lead-in dot", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [8, 3, 0, 7, 1]);
const LAMP_MS = cfgMs("LAMP_TEST_MS");
wait(LAMP_MS - 100);
check("for LAMP_TEST_MS", fcb.hw.lamp, 3);
wait(100);
check("then the normal LEDs", fcb.hw.lamp, 0);
// Page 8 opened LAMP_TEST_MS (plus at most 40 ms) ago; the lead-in and the text are
// eight frames.
const pageFrames = [SCROLL_TEXT.filter((x) => x <= fcb.hw.text && fcb.hw.text)].flat();
for (let t = 0; t < FRAME * 8 - 104 - LAMP_MS; t += 8) { wait(8); if (fcb.hw.text && pageFrames[pageFrames.length - 1] !== fcb.hw.text) pageFrames.push(fcb.hw.text); }
check("the scroll runs through every frame", pageFrames, SCROLL_TEXT);
// The trailing dot (two frames), then halfway into major.
wait(FRAME * 2 + 64 + BOOT_SHOW / 2);
check("major version with a trailing dot", [verShown(), fcb.hw.dots, fcb.hw.lamp], [VER[0], 1, 0]);
wait(1000);
check("then minor with leading and trailing dots", [verShown(), fcb.hw.dots], [VER[1], 5]);
wait(1000);
check("then build with a leading dot", [verShown(), fcb.hw.dots], [VER[2], 4]);
// The page draws it: lit segments of the two hex digits plus the leading dot.
const SEGS = [6, 2, 5, 5, 4, 5, 6, 3, 7, 6, 6, 5, 4, 5, 5, 4];
const litNow = () => (sandbox.document.getElementById("display").innerHTML.match(/var\(--seg-on\)/g) || []).length;
check("the display draws it (segments lit)", litNow(), SEGS[VER[2] >> 4] + SEGS[VER[2] & 15] + 1);
check("and sends nothing", sent(), []);
// Then a blank display before major again.
wait(1000);
check("then the display is blank (no dots)", [fcb.hw.hex, fcb.hw.blank, fcb.hw.dots, fcb.hw.lamp, litNow()], [-1, 7, 0, 0, 0]);
// From halfway into the blank to halfway into the lead-in's first frame.
wait(BOOT_SHOW / 2 + FRAME / 2);
check("then the lead-in: the leading dot alone, far right", [fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [0, 7, 1]);
wait(FRAME);
check("then the leading dot between the digits", [fcb.hw.text, fcb.hw.blank, fcb.hw.dots], [0, 7, 2]);
wait(FRAME);
check("then the scroll again", [fcb.hw.text, fcb.hw.dots], [SCROLL_TEXT[0], SCROLL_DOTS[0]]);
// Halfway into " r": the rest of the text and the trailing dot, then halfway into major.
wait(FRAME * 7.5 + BOOT_SHOW / 2);
check("then major again", [verShown(), fcb.hw.dots], [VER[0], 1]);
// It keeps repeating while the page is open.
// Over two more cycles every part (and the blank, -1) shows again.
// The drawn footswitch LEDs: at most one lit at a time, switches 1 to 10 in turn (user's
// request: lit as its frame starts). The panel LEDs: one at a time, up the row and back
// (SWITCH 1, SWITCH 2, SWITCH, SELECT, NUMBER, VALUE 1, VALUE 2, DIRECT SELECT, MIDI
// FUNCTION, MIDI CHAN).
const darkNow = () => [...Array(10).keys()].filter((i) => sandbox.document.getElementById("led" + i).classList.contains("on"));
const CHASE = ["ledSw1", "ledSw2", "ledSwitch", "ledSel", "ledNum", "ledVal1", "ledVal2", "ledDirect", "ledFunc", "ledChan"];
const chaseNow = () => CHASE.filter((id) => el0(id).classList.contains("on"));
const pageLamps = new Set(), pageParts = new Set(), darkSeq = [], chaseSeq = [];
let darkMax = 0, chaseCounts = new Set();
for (let t = 0; t < 14000; t += 8) {
  wait(8); pageLamps.add(fcb.hw.lamp); pageParts.add(verShown());
  const d = darkNow(); darkMax = Math.max(darkMax, d.length);
  if (d.length && darkSeq[darkSeq.length - 1] !== d[0]) darkSeq.push(d[0]);
  const c = chaseNow(); chaseCounts.add(c.length);
  if (c.length && chaseSeq[chaseSeq.length - 1] !== c[0]) chaseSeq.push(c[0]);
}
check("and keeps repeating", [[...pageParts].sort((x, y) => x - y), [...pageLamps]], [[-1, ...new Set(VER)].sort((x, y) => x - y), [0]]);
check("each step lights its footswitch LED, 1 to 10 in turn (one at a time)",
  [darkMax, new Set(darkSeq).size, darkSeq.every((v, i) => i === 0 || v === (darkSeq[i - 1] + 1) % 10)], [1, 10, true]);
{
  const upBack = CHASE.concat(CHASE.slice(1, -1).reverse());
  const at = upBack.indexOf(chaseSeq[0]);
  check("the panel LEDs light one at a time, up the row and back",
        [[...chaseCounts], chaseSeq.every((id, i) => id === upBack[(at + i) % upBack.length])], [[1], true]);
}
// Settings were changed on the earlier pages, so the drawn CONFIG LED flashes on page 7
// while the other LEDs stay lit.
const drawnOver = (id, ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 10) { wait(10); seen.add(sandbox.document.getElementById(id).classList.contains("on")); } return [...seen].sort(); };
check("with unsaved changes the CONFIG LED flashes on page 8", [fcb.hw.lamp, drawnOver("ledCfg", CFG_FLASH_MS * 3)], [0, [false, true]]);
// Footswitch 8 held: the minor version stays, LED 8 lit; released, it carries on.
fcb.down(8); wait(60);
check("holding footswitch 8 shows the minor version, its LED lit", [verShown(), fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
wait(BOOT_SHOW * 9);
check("and keeps it while held", [verShown(), fcb.hw.dots, darkNow()], [VER[1], 5, [7]]);
fcb.up(8); wait(BOOT_SHOW + 600);
check("released: it carries on to the build (its LED dark after its flash)", [verShown(), fcb.hw.dots, darkNow()], [VER[2], 4, []]);
fcb.down(10); wait(60);
check("holding footswitch 10 lights every LED and display segment", [fcb.hw.lamp, litNow(), CHASE.concat(["ledCfg", "ledExp1", "ledExp2"]).every((id) => el0(id).classList.contains("on")), darkNow().length],
      [1, 7 + 7 + 4 + 3, true, 10]);
fcb.up(10); wait(60);
check("let go, the normal LEDs", fcb.hw.lamp, 0);
fcb.down(0); wait(60);
check("footswitch 1 shows \" r\", its LED lit", [fcb.hw.text, fcb.hw.dots, darkNow()], [SCROLL_TEXT[0], SCROLL_DOTS[0], [0]]);
fcb.up(0); wait(FRAME + 100);
check("then \"ri\"", fcb.hw.text, SCROLL_TEXT[1]);
check("and nothing is sent", sent(), []);
// Pedal B sets the LED brightness here (user's request): the first reading is only a
// reference.
fcb.setRaw(0, 255); wait(60);
check("page 9 shows the saved brightness (full by default)", fcb.hw.level, 127);
fcb.setRaw(0, 0); wait(60);
check("pedal B at the heel: brightness 0", [fcb.hw.level, fcb.state().cfgPage], [0, 8]);
fcb.setRaw(0, 255); wait(60);
check("at the toe: full again", fcb.hw.level, 127);
press(UP, 100);
check("UP on page 9 stays on page 9 (no wrap)", fcb.state().cfgPage, 8);
press(DOWN, 100);
check("DOWN leaves page 9 for page 8: lamp test off, dots off, \"CL\", SELECT lit", [fcb.state().cfgPage, fcb.hw.lamp, fcb.hw.dots, fcb.hw.text, fcb.hw.aux[7]], [7, 0, 0, TEXT_CL, 1]);
press(DOWN, 100);
check("then page 7: \"Pr\", DIRECT SELECT lit", [fcb.state().cfgPage, fcb.hw.text, fcb.hw.aux[4]], [6, TEXT_PROG, 1]);
press(DOWN, 100);
check("then page 6", fcb.state().cfgPage, 5);
press(DOWN, 100);
check("then page 5", fcb.state().cfgPage, 4);
press(DOWN, 100);
check("then page 4, EXP B", [fcb.state().cfgPage, pageLeds()], [3, [0, 0, 0, 1]]);
press(DOWN, 100);
check("then page 3, EXP A, its CC", [fcb.state().cfgPage, fcb.hw.display, fcb.hw.leds[5], pageLeds()], [2, fcb.state().pedalCc[0], 1, [0, 0, 1, 0]]);
press(DOWN, 100);
check("then page 2, MIDI CHANNEL lit", [fcb.state().cfgPage, fcb.hw.display, pageLeds()], [1, 2, [1, 0, 0, 0]]);
press(DOWN, 100);
check("DOWN back to page 1", fcb.state().cfgPage, 0);
press(DOWN, 100);
check("DOWN on page 1 stays on page 1 (no wrap)", fcb.state().cfgPage, 0);
fcb.setRaw(0, 0); wait(100);
longKey(UP);
check("a long UP asks which bank to save to: the bank in use, page LEDs dark", [fcb.state().mode, fcb.state().save, fcb.hw.display, pageLeds()], [1, 1, 1, [0, 0, 0, 0]]);
check("its dot flashes", (() => { const a = fcb.hw.dots & 1; wait(250); return [a, fcb.hw.dots & 1].sort().join(); })(), "0,1");
check("the number and the dot alternate", [0, 1, 2, 3].map(() => { wait(125); const r = (fcb.hw.dots & 1) + ":" + fcb.hw.blank; wait(125); return r; })
      .every((r) => r === "1:7" || r === "0:0"), true);
check("the footswitches type a bank", [el0("mode0").textContent, el0("mode9").textContent], ["Bank digit 1", "Bank digit 0"]);
press(UP, 100);
check("a short UP then saves (bank 1) and leaves", [fcb.state().mode, fcb.hw.status, fcb.state().bank], [0, 0, 0]);
check("and the CONFIG LED stays dark in run mode", statusOver(CFG_FLASH_MS * 3), [0]);
// The save reached the editor's EEPROM, so going back in finds nothing unsaved: steady.
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
check("re-entering after the save: CONFIG LED steady (settings match the EEPROM)", [fcb.state().mode, statusOver(CFG_FLASH_MS * 3)], [1, [1]]);
longKey(DOWN);
check("and a cancel leaves with nothing changed", [fcb.state().mode, fcb.hw.status], [0, 0]);
// Switch 3's mode was put back, so saving puts it in that mode's resting state,
// which it is already in (on for inverted momentary, from power-on; off otherwise): no CC
// is sent. No program change either way.
check("with nothing sent and no program change", [sent(), fcb.state().program], [[], p]);
check("switch 3's LED after saving", fcb.hw.leds[2], DEF_MODE === 3 ? 1 : 0);
check("SWITCH 1 following switch 4 was stored with bank 1 (its record's bit 4)", JSON.parse(store["fcb1010-eeprom-v2"]).fs3[0] & 0x10, 0x10);
check("the settings record was stored", JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0], 2);
fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200);
toPage(1);
press(8, 100);
check("in config again, switch 8 enters channel 8 on page 6", fcb.state().midiChannel, 8);
longKey(DOWN);
check("a long DOWN cancels and leaves: channel 2 again", [fcb.state().mode, fcb.state().midiChannel], [0, 2]);
check("with no program change", [sent(), fcb.state().program], [[], p]);
// A long UP + DOWN in config mode saves and leaves too.
const longChord = () => { fcb.down(UP); fcb.down(DOWN); wait(1100); fcb.up(UP); fcb.up(DOWN); wait(200); };
longChord(); toPage(1); press(4, 100);
longChord();
check("a long UP + DOWN saves and leaves: channel 5 stored", [fcb.state().mode, fcb.hw.status, JSON.parse(store["fcb1010-eeprom-v2"]).cfg[0]], [0, 0, 5]);
check("without toggling DIRECT SELECT or changing the program", [sent(), fcb.hw.aux[4], fcb.state().program], [[], 0, p]);
longChord(); toPage(1); press(1, 100); longChord();
check("and back to channel 2", [fcb.state().mode, fcb.state().midiChannel], [0, 2]);
fcb.reset(); wait(400);
check("after a power cycle the channel is still 2", fcb.state().midiChannel, 2);
// Start-up lights the inverted momentary switches (their resting state), sending nothing.
check("after a power cycle only inverted momentary switches are lit, and only the identity sent (on channel 2)",
      [fcb.hw.leds.slice(), sent().map((x) => x.slice(0, 29))], [fcb.state().pedalModes.map((m) => (m === 3 ? 1 : 0)), ["f0 7e 01 06 02 7d 46 43 31 30"]]);
press(0, 100);
check("and switch 1 now sends on channel 2", sent(), MOM ? [cc(1, 0, ON), cc(1, 0, OFF)] : [cc(1, 0, 127)]);

console.log("editor: visual metronome");
// Turned on from page 8 (footswitch 1, already selected there), then saved.
longChord(); toPage(7); press(0, 100); longChord();
check("the misc page's footswitch 1 turns the metronome on", [fcb.state().mode, fcb.state().misc & 1], [0, 1]);
sent();
fcb.midiIn([0xfa, 0xf8]); wait(20);
check("MIDI Start, then a clock: \"+\" on beat 1, over the bank number", [fcb.hw.msign, el0("display").getAttribute("aria-label"), fcb.hw.display], [2, "Display +1", 1]);
fcb.midiIn(Array(12).fill(0xf8)); wait(20);
check("dark for the second half of the beat", fcb.hw.msign, 0);
fcb.midiIn(Array(12).fill(0xf8)); wait(20);
check("beat 2: \"-\"", [fcb.hw.msign, el0("display").getAttribute("aria-label")], [1, "Display -1"]);
fcb.midiIn([0xfc]); wait(20);
check("Stop: dark, and nothing is sent", [fcb.hw.msign, sent()], [0, []]);
longChord(); toPage(7); press(0, 100); longChord();
check("and off again", fcb.state().misc & 1, 0);

console.log("editor: relays and MIDI IN flash");
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

console.log("editor: expression pedal");
fcb.setRaw(200, 0); wait(100);
check("pedal A at 200 of 255 sends CC 7", sent(), ["b1 07 64"]);

console.log("editor: footswitch editor");
fcb.setRaw(0, 0); wait(100); sent();
const TEXT_CC = 10, TEXT_NOTE = 11;   // HAL_TEXT_CC / HAL_TEXT_NOTE + 1
const label = (i) => sandbox.document.getElementById("cc" + i).textContent;
const chHex = (fcb.state().midiChannel - 1).toString(16);
// MIDI FUNCTION, SELECT, NUMBER, VALUE 1, VALUE 2 (the editor's parameters in order).
// The parameter's LED: SWITCH (the pedal mode), SELECT, NUMBER, VALUE 1, VALUE 2.
// It flickers, so each is taken as lit if it lights within two flicker halves.
const editLeds = () => {
  const o = [0, 0, 0, 0, 0];
  for (let t = 0; t <= 2 * cfgMs("PARAM_FLASH_MS"); t += 2) {
    ms(2);
    [11, 7, 8, 9, 10].forEach((a, i) => { o[i] |= fcb.hw.aux[a]; });
  }
  return o;
};
longChord();
check("config mode, page 1", [fcb.state().mode, fcb.state().cfgPage], [1, 0]);
// A long press of footswitch 3 opens its editor on the pedal mode.
press(2, OVER_MS);
check("a long footswitch press opens its editor on the mode: SWITCH lit, and MIDI FUNCTION (page 1)",
      [fcb.state().edit, editLeds(), fcb.hw.aux[1]], [1, [1, 0, 0, 0, 0], 1]);
press(UP, 100);
check("UP: the type, SELECT lit, \"CC\" shown: footswitch 6 lit, 1 and 10 dim",
      [fcb.state().edit, editLeds(), fcb.hw.text, ledLevels()], [2, [0, 1, 0, 0, 0], TEXT_CC, [2, 0, 0, 0, 0, 1, 0, 0, 0, 2]]);
check("the footswitches' labels name the types", [0, 5, 9].map((i) => el0("mode" + i).textContent), ["Type Prog", "Type CC", "Type Note"]);
press(10, 100);
check("footswitch 10 makes it a note (\"nt\")", [fcb.hw.text, ledLevels()[9]], [TEXT_NOTE, 1]);
press(UP, 100);
check("UP: the number, NUMBER lit", [fcb.state().edit, editLeds()], [3, [0, 0, 1, 0, 0]]);
press(6, 100); press(10, 100);
check("footswitches 6 and 10 type 60", [fcb.hw.display, fcb.hw.text], [60, 0]);
press(UP, 100);
press(0, 100); press(10, 100); press(10, 100);
check("UP: the on value (VALUE 1 lit), 1 0 0 types 100", [fcb.state().edit, editLeds(), fcb.hw.display], [4, [0, 0, 0, 1, 0], 100]);
press(UP, 100);
press(1, 100); press(10, 100);
check("UP: the off value (VALUE 2 lit), 2 10 types 20", [fcb.state().edit, editLeds(), fcb.hw.display], [5, [0, 0, 0, 0, 1], 20]);
press(UP, OVER_MS);
check("UP (short or long) on the off value accepts: editor closed, page 1 showing its number, still in config mode",
      [fcb.state().edit, editLeds(), fcb.state().mode, fcb.hw.display], [0, [0, 0, 0, 0, 0], 1, 3]);
check("the footswitch label shows the new message", label(2), "Note 60");
press(UP, OVER_MS); press(UP, 100);
check("a long UP and a short one then save and leave", fcb.state().mode, 0);
check("footswitch 3's record was stored (note, 60, on 100, off 20)", JSON.parse(store["fcb1010-eeprom-v2"]).fs2, [1 | (fcb.state().pedalModes[2] << 2), 60, 100, 20]);
wait(20); sent();
press(2, 100); wait(20);
const notes = sent();
check("footswitch 3 now sends note 60 (velocity 100 on, note off velocity 20)",
      notes.length > 0 && notes.every((m) => (m === "9" + chHex + " 3c 64" || m === "8" + chHex + " 3c 14")) &&
      notes.some((m) => m === "9" + chHex + " 3c 64"), true);

console.log("editor: program change toggle");
longChord();
press(3, OVER_MS);                     // footswitch 4's editor
press(UP, 100);
press(0, 100);                         // type: program change (footswitch 1)
press(UP, 100);
check("UP skips NUMBER: a program change goes on to VALUE 1", [fcb.state().edit, editLeds()], [4, [0, 0, 0, 1, 0]]);
press(4, 100);                         // program sent on: 5 (50-59 could follow)
press(UP, 100);                        // UP [ENTER] ends the number there
press(UP, 100); press(6, 100);         // program sent off: 6
press(UP, 100);
check("VALUE 2 is the second program", [fcb.state().edit, editLeds(), fcb.state().msgs[3].on, fcb.state().msgs[3].off], [5, [0, 0, 0, 0, 1], 5, 6]);
press(UP, 100);
check("a short UP [ENTER] on VALUE 2 keeps it and closes the editor", [fcb.state().edit, fcb.state().mode, fcb.state().cfgPage], [0, 1, 0]);
check("footswitch 4's label shows both programs", label(3), "PC 5/6");
press(UP, OVER_MS); press(UP, 100);
wait(50); sent();
const pc = (n) => "c" + chHex + " " + hex(n);
const twoPresses = [[], []];
press(3, 100); wait(20); twoPresses[0] = sent();
press(3, 100); wait(20); twoPresses[1] = sent();
const wantPc = DEF_MODE === 2 ? [[pc(5), pc(6)], [pc(5), pc(6)]] :
               DEF_MODE === 3 ? [[pc(6), pc(5)], [pc(6), pc(5)]] : [[pc(5)], [pc(6)]];
check("it toggles between programs 5 and 6 (following the default pedal mode)", twoPresses, wantPc);

console.log("editor: configuration over MIDI (the page's configurator, NRPN)");
const conf = fcb.conf, NP = fcb.NP, NC = fcb.NC;
// Wait (in 50ms steps, up to n) until the configurator has no read or command in progress.
function idle(n, step = wait) { for (let t = 0; t < n && (conf.waiting || conf.after || t < 200); t += 50) step(50); }
const rec = () => JSON.parse(store["fcb1010-eeprom-v2"]);
wait(100); sent();
check("before any read the panel holds the editor's settings from page load (the defaults here)",
      [conf.known, conf.v[NP.NUM + 2], conf.v[NP.TYPE + 3], conf.v[NP.MODE + 4], typeof conf.v[NP.CHANNEL]], [false, STOMP_CC[2], 0, DEF_MODE, "number"]);
conf.ch = 1 + ((fcb.state().midiChannel) % 16);    // the wrong channel: SysEx reaches the pedal on any
fcb.confRead(true, []); idle(3000);
check("Read reaches the pedal on any channel and learns its channel", [conf.waiting, conf.ch, !!conf.v], ["", fcb.state().midiChannel, true]);
check("the dump has footswitch 3 (note 60, 100, 20) and 4 (programs 5 / 6)",
      [conf.v[NP.TYPE + 2], conf.v[NP.NUM + 2], conf.v[NP.ON + 2], conf.v[NP.OFF + 2], conf.v[NP.TYPE + 3], conf.v[NP.ON + 3], conf.v[NP.OFF + 3]],
      [1, 60, 100, 20, 2, 5, 6]);
check("and the channel, a mode and a stomp CC", [conf.v[NP.CHANNEL], conf.v[NP.MODE], conf.v[NP.NUM + 9]], [fcb.state().midiChannel, DEF_MODE, STOMP_CC[9]]);
const dumpMsgs = sent();
check("the dump went out as one SysEx message (65 bytes) with the pedal's channel, and nothing else",
      [dumpMsgs.length, dumpMsgs[0].split(" ").length, dumpMsgs[0].slice(0, 17)], [1, 65, "f0 7d 46 43 " + chHex.padStart(2, "0") + " 02"]);
fcb.confChanged(NP.NUM + 9, 99);
fcb.confChanged(NP.TRIGGER + 2, 70);
wait(20);
check("a set applies at once, unsaved", [fcb.state().msgs[9].num, fcb.state().swThr[2], rec().fs9 === undefined || rec().fs9[1] !== 99], [99, 70, true]);
fcb.confCommand(NC.SAVE, "saved"); idle(3000);
// The footswitch record's bits 6-7 hold part of the bank's pedal settings; the trigger is in the bank's record.
check("Save stores it and the configurator reads it back",
      [[rec().fs9[0] & 0x3F, ...rec().fs9.slice(1)], rec().bank[2] & 0x7F, conf.waiting, conf.v[NP.NUM + 9]], [[fcb.state().pedalModes[9] << 2, 99, 127, 0], 70, "", 99]);
fcb.confChanged(NP.NUM + 9, 98); wait(20);
fcb.confCommand(NC.RELOAD, "dropped"); idle(3000);
check("Discard goes back to the saved settings", [fcb.state().msgs[9].num, conf.v[NP.NUM + 9]], [99, 99]);
check("the dump has the bank (1) and the bank count", [conf.v[NP.BANK], conf.v[NP.BANKS] > 0], [1, true]);
// A never-saved bank's footswitch number: the note banks (config.h) play NOTE_BANK_SCALE from their first note.
const NOTE_SCALE = cfgText.match(/#define NOTE_BANK_SCALE\s+([0-9, ]+)/)[1].split(",").map(Number);
const bankNum = (b, i) => b === cfgNum("NOTE_BANK_1") ? cfgNum("NOTE_BANK_1_FIRST") + NOTE_SCALE[i] :
  b === cfgNum("NOTE_BANK_2") ? cfgNum("NOTE_BANK_2_FIRST") + NOTE_SCALE[i] : STOMP_CC[i];
fcb.confBank(3); idle(3000);
check("choosing bank 3 switches the pedal and reads that bank back",
      [fcb.state().bank, conf.v[NP.BANK], conf.known, conf.v[NP.NUM + 9], fcb.state().msgs[9].num], [2, 3, true, bankNum(3, 9), bankNum(3, 9)]);
fcb.confChanged(NP.NUM + 0, 88); wait(20);
fcb.confCommand(NC.SAVE, "saved"); idle(3000);
check("Store keeps bank 3's footswitches in its own records", [rec().b3fs0 && rec().b3fs0[1], rec().fs0 === undefined || rec().fs0[1] !== 88], [88, true]);
fcb.confBank(1); idle(3000);
check("and bank 1 is as it was", [fcb.state().bank, conf.v[NP.NUM + 9], conf.v[NP.NUM + 0]], [0, 99, STOMP_CC[0]]);
// Copy bank / Paste bank (user's request): bank 1's settings into bank 6, the channel left alone.
fcb.confChanged(NP.TRIGGER + 1, 33); fcb.confChanged(NP.PEDAL_CC + 1, 44); wait(20);
fire("cfCopy", "click");
check("Copy bank keeps the bank shown", [/^Copied bank 1\b/.test(el0("confStatus").textContent)], [true]);
fcb.confBank(6); idle(3000);
check("bank 6 is not yet like it", [fcb.state().bank, conf.v[NP.NUM + 9], conf.v[NP.PEDAL_CC + 1] === 44], [5, bankNum(6, 9), false]);
const chBefore = fcb.state().midiChannel;
fire("cfPaste", "click"); wait(160);
const pasteSays = el0("confStatus").textContent;
wait(600);
check("Paste bank sends it to bank 6: footswitches, relays and pedals, the channel unchanged",
      [fcb.state().bank, fcb.state().msgs[9].num, fcb.state().swThr[1], fcb.state().pedalCc[1], fcb.state().midiChannel, conf.v[NP.NUM + 9]],
      [5, 99, 33, 44, chBefore, 99]);
check("and says so", /^Pasted bank 1's settings into bank 6 \(\d+ settings\)/.test(pasteSays), true);
fire("cfPaste", "click"); wait(160);
check("pasting again changes nothing", /already match/.test(el0("confStatus").textContent), true);
wait(600);
fcb.confCommand(NC.RELOAD, "dropped"); idle(3000);
fcb.confBank(1); idle(3000);
fcb.confCommand(NC.RELOAD, "dropped"); idle(3000);
check("back on bank 1 with its saved settings", [fcb.state().bank, conf.v[NP.NUM + 9], fcb.state().swThr[1]], [0, 99, 0]);
check("the page is called the FCB1010 Editor", /<title>FCB1010 Editor<\/title>/.test(html) && /<h1>FCB1010 Editor<\/h1>/.test(html), true);
longChord();
fcb.confRead(false, []); wait(2000);
check("in config mode the pedal does not answer", [conf.waiting, fcb.state().mode], ["", 1]);
press(DOWN, OVER_MS);

console.log("editor: settings files (save to file, load from file)");
const el2 = el0;
wait(200);
fcb.confRead(false, []); idle(3000);
const file = JSON.parse(JSON.stringify(fcb.confBankFile(true)));
check("a saved file has the pedal's settings in readable form",
      [file.format, file.version, file.global.channel, file.footswitches[2], file.footswitches[3].type, file.relays.length],
      ["fcb1010-settings", 2, fcb.state().midiChannel, { mode: ["stomp", "stomp + momentary", "momentary", "inverted momentary"][DEF_MODE], type: "note", number: 60, on: 100, off: 20 }, "program", 2]);
check("its global section has the shared settings",
      file.global, { channel: fcb.state().midiChannel, brightness: 127, clockDisplay: false, runningStatus: false, activeSensing: false });
const oldCh = file.global.channel;
file.footswitches[1] = { mode: "momentary", type: "note", number: 77, on: 90, off: 10 };
file.relays[0] = { follows: [3, 7], pedalA: 0, pedalB: 64 };
file.global.channel = oldCh % 16 + 1;
check("loading it is accepted", fcb.confLoad(file), true);
wait(600);
const st = fcb.state();
check("the pedal took the file's settings, the channel last",
      [st.msgs[1], st.pedalModes[1], st.swMask[0], st.swThr[1], st.midiChannel, conf.ch, conf.unsaved],
      [{ type: 1, num: 77, on: 90, off: 10 }, 2, (1 << 2) | (1 << 6), 64, file.global.channel, file.global.channel, true]);
check("and reading it back gives the same file", fcb.confBankFile(true), file);
check("an invalid file is refused and changes nothing",
      [fcb.confLoad({ format: "fcb1010-settings", version: 1, channel: 1, footswitches: [], relays: [] }),
       fcb.confLoad({ format: "other" }), fcb.state().midiChannel], [false, false, file.global.channel]);
const bad = JSON.parse(JSON.stringify(file)); bad.footswitches[4].on = 200;
check("a value out of range is refused", fcb.confLoad(bad), false);
file.global.channel = oldCh; fcb.confLoad(file); wait(600);
check("loading a file with only the channel different sends just that", [fcb.state().midiChannel, conf.ch], [oldCh, oldCh]);
// The other shared settings (NRPN 98-101), from the file and from the Global controls.
file.global.brightness = 40; file.global.runningStatus = true; file.global.activeSensing = true;
fcb.confLoad(file); wait(600);
check("a file's brightness, running status and active sensing reach the pedal", [fcb.state().level, fcb.state().misc & 7, fcb.hw.level], [40, 6, 40]);
check("and read back the same", fcb.confBankFile(true).global, file.global);
check("the Global controls show them", [el2("cfLevel").value, el2("cfRunstat").value, el2("cfAsense").value, el2("cfMetro").value], [40, "1", "1", "0"]);
el2("cfLevel").value = "127"; fire("cfLevel", "change");
el2("cfRunstat").value = "0"; fire("cfRunstat", "change");
el2("cfAsense").value = "0"; fire("cfAsense", "change");
wait(200);
check("changing them sends them", [fcb.state().level, fcb.state().misc & 7], [127, 0]);
file.global = { channel: oldCh, brightness: 127, clockDisplay: false, runningStatus: false, activeSensing: false };
const v1 = JSON.parse(JSON.stringify(file)); v1.version = 1; v1.channel = oldCh; delete v1.global;
check("a version 1 file (the channel at the top) still loads", fcb.confLoad(v1), true);
wait(600);

// The bank's program change (configuration page 7; NRPN 92 / 93): null for none.
check("a file holds the bank's program change (none: null)", [file.bankProgram, fcb.state().bankPc], [null, 0]);
file.bankProgram = 42; fcb.confLoad(file); wait(600);
check("loading a program sets it, and reading back gives the same file", [fcb.state().bankPc, fcb.confBankFile(true).bankProgram, el2("cfBankPc").value], [43, 42, "42"]);
file.bankProgram = null; fcb.confLoad(file); wait(600);
check("null sets none", [fcb.state().bankPc, fcb.confBankFile(true).bankProgram, el2("cfBankPc").value], [0, null, "-1"]);
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
conf.known = false; fcb.confRead(false, []); idle(3000);
check("the pedal and port selectors are never disabled; the file and store buttons wait for a read",
      [el("cfTarget").disabled, el("cfOut").disabled, el("cfIn").disabled, el("cfRead").disabled],
      [undefined, undefined, undefined, undefined]);

console.log("editor: configurator through Web MIDI (a fake USB interface wired to the emulated pedal)");
// The interface's output feeds the pedal's MIDI IN at cable speed; the pedal's MIDI OUT reaches its input
// as Web MIDI delivers it: one complete message per event, status byte included.
const fakeIn = { id: "in1", name: "USB MIDI In", state: "connected", onmidimessage: null, open: () => Promise.resolve() };
const outSent = [];
let accessOpts = null;
const fakeOut = { id: "out1", name: "USB MIDI Out", state: "connected", open: () => Promise.resolve(), send: (b) => { outSent.push(Array.from(b)); fcb.wire(Array.from(b)); } };
sandbox.navigator = { requestMIDIAccess: (o) => (accessOpts = o, Promise.resolve({ inputs: new Map([["in1", fakeIn]]), outputs: new Map([["out1", fakeOut]]), onstatechange: null })) };
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
fcb.confRead(true, []); idle(3000, waitMidi);
check("SysEx access asked for, and the read is one SysEx request to any pedal",
      [accessOpts && accessOpts.sysex, outSent[0]], [true, [0xF0, 0x7D, 0x46, 0x43, 0x7F, 1, 0, 0xF7]]);
check("Find through the interface reads the pedal", [conf.waiting, conf.ch, !!conf.v, conf.v && conf.v[NP.NUM + 1]], ["", oldCh, true, 77]);
outSent.length = 0;
fcb.confChanged(NP.ON + 1, 55); waitMidi(50);
check("a change is one SysEx set", outSent, [[0xF0, 0x7D, 0x46, 0x43, 0x7F, 3, 41, 55, 0xF7]]);
fcb.confCommand(NC.SAVE, "stored"); idle(3000, waitMidi);
// The record's type byte carries the pedal mode in bits 2-3.
check("a change and Store reach the pedal", [fcb.state().msgs[1].on, rec().fs1, conf.v[NP.ON + 1]],
      [55, [1 | (fcb.state().pedalModes[1] << 2), 77, 55, 10], 55]);
{
  const f = fcb.confBankFile(true);
  f.footswitches[1].on = 56; f.footswitches[2].off = 7; f.relays[0].pedalA = 9;
  outSent.length = 0;
  fcb.confLoad(f); idle(3000, waitMidi);
  const sets = outSent.filter((m) => m[5] === 3);
  check("a file load is one SysEx set with only what differs, applied whole",
        [sets.length, sets[0] && sets[0].length, fcb.state().msgs[1].on, fcb.state().msgs[2].off, fcb.state().swThr[0]], [1, 7 + 2 * 3, 56, 7, 9]);
  fcb.confCommand(NC.RELOAD, "dropped"); idle(3000, waitMidi);
  // A version 3 file with another bank: written straight into that bank (one WRITE), then read back to
  // check it; the bank in use, unchanged, is not sent, and the pedal stays on its bank.
  const here = fcb.state().bank + 1, there = here === 2 ? 1 : 2;
  const two = { format: "fcb1010-settings", version: 3, bank: here, banks: [Object.assign({ bank: there }, fcb.confBankFile(false))] };
  delete two.banks[0].format; delete two.banks[0].version;
  two.banks.push(Object.assign({ bank: here }, fcb.confBankFile(false)));
  delete two.banks[1].format; delete two.banks[1].version;
  delete conf.banks[there];
  outSent.length = 0;
  fcb.confLoad(two);
  for (let t = 0; t < 30000 && (conf.job || conf.after || conf.waiting); t += 50) waitMidi(50);
  const cfgOut = outSent.filter((m) => m[1] === 0x7D);
  check("a whole-pedal load through the interface: one write of that bank, a read of it, then of the bank in use; no bank change",
        [cfgOut.length, cfgOut[0].slice(0, 7), cfgOut[0].length, cfgOut[0][55], cfgOut[1], cfgOut[2], fcb.state().bank, outSent.some((m) => m[5] === 3)],
        [3, [0xF0, 0x7D, 0x46, 0x43, 0x7F, 4, there], 56, 0xF7, [0xF0, 0x7D, 0x46, 0x43, 0x7F, 1, there, 0xF7], [0xF0, 0x7D, 0x46, 0x43, 0x7F, 1, 0, 0xF7], here - 1, false]);
  check("and the read back confirms it", [conf.banks[there][NP.NUM + 1], /^Stored bank \d+ from the file; the pedal stayed on bank \d+\./.test(el("confStatus").textContent)],
        [conf.v[NP.NUM + 1], true]);
}
fakeIn.onmidimessage = null;
const heardBefore = conf.heard;
fcb.confRead(false, []); idle(3000, waitMidi);
check("with nothing coming back, it says so", [conf.waiting, /nothing at all came in/.test(el("confStatus").textContent) || heardBefore > 0], ["", true]);
console.log("editor: configurator with a MIDI output only (no input, so no reads)");
conf.inp = null; conf.known = false;
const st0 = JSON.stringify(fcb.state().msgs);
fcb.confRead(false, []);
check("Read says it needs an input", [conf.waiting, /needs a MIDI input/.test(el("confStatus").textContent)], ["", true]);
const keptBanks = conf.banks; conf.banks = {};
fcb.confDownload();
conf.banks = keptBanks;
check("saving with only some banks read says the others are missing", /only \d+ of the pedal's \d+ banks/.test(el("confStatus").textContent), true);
const oneWay = fcb.confBankFile(true);
oneWay.footswitches[5] = { mode: "stomp", type: "cc", number: 33, on: 100, off: 1 };
oneWay.relays[1] = { follows: [10], pedalA: 12, pedalB: 0 };
check("a file loads with no read first", fcb.confLoad(oneWay), true);
waitMidi(800);
check("every setting was sent (no read, so nothing is assumed), and the pedal took them",
      [fcb.state().msgs[5], fcb.state().pedalModes[5], fcb.state().swMask[1], fcb.state().swThr[2], JSON.stringify(fcb.state().msgs.filter((m, i) => i !== 5)) === JSON.stringify(JSON.parse(st0).filter((m, i) => i !== 5))],
      [{ type: 0, num: 33, on: 100, off: 1 }, 0, 1 << 9, 12, true]);
check("the status says it cannot read back", /cannot read it back/.test(el("confStatus").textContent), true);
fcb.confChanged(NP.OFF + 5, 2); waitMidi(50);
fcb.confCommand(NC.SAVE, "Stored."); idle(3000, waitMidi);
check("a change and Store work without an input", [[rec().fs5[0] & 0x3F, ...rec().fs5.slice(1)], rec().bank[2] & 0x7F], [[fcb.state().pedalModes[5] << 2, 33, 100, 2], 12]);
check("saving a file without a read leaves out the firmware version", [fcb.confToFile().firmware, fcb.confBankFile(true).footswitches[5].off], [undefined, 2]);
{
  // A whole-pedal file with no input: every bank in it written (nothing is known), at a fixed pace.
  const b1 = Object.assign({ bank: 1 }, fcb.confBankFile(false)), b4 = Object.assign({ bank: 4 }, fcb.confBankFile(false));
  [b1, b4].forEach((e) => { delete e.format; delete e.version; });
  b4.footswitches[0] = { mode: "stomp", type: "cc", number: 45, on: 101, off: 2 };
  outSent.length = 0;
  check("with no input, a whole-pedal file loads", fcb.confLoad({ format: "fcb1010-settings", version: 3, bank: 1, banks: [b1, b4] }), true);
  for (let t = 0; t < 5000 && conf.job; t += 50) waitMidi(50);
  const w = outSent.filter((m) => m[5] === 4);
  check("each bank is one write, nothing is read, and the pedal stays on its bank",
        [w.map((m) => m[6]), outSent.filter((m) => m[5] === 1).length, rec().b4fs0.slice(1), fcb.state().bank, /cannot read it back/.test(el("confStatus").textContent)],
        [[1, 4], 0, [45, 101, 2], 0, true]);
}
conf.out = null;
check("with no output either, a load is refused", fcb.confLoad(oneWay), false);
fcb.confTarget("emu");
check("switching back to the editor works", [conf.target, el("cfTarget").disabled], ["emu", undefined]);

console.log("editor: expression pedal pickup");
// Pedal A to pickup over MIDI (NRPN 94), then a received CC A value it must reach. The
// panel's log keeps 200 lines, so this reads what came after a mark.
const PCC = cfgText.match(/#define PEDAL_CC_LIST\s+(\d+),\s*(\d+)/).slice(1).map(Number);
const pkShow = cfgNum("PEDAL_PICKUP_SHOW_MS");
const mark = () => fcb.hw.logLines[0];
const since = (m) => { const out = []; for (const l of fcb.hw.logLines) { if (l === m) break; out.push(l.raw); } return out.reverse(); };
const ccA = (v) => "b" + chHex + " " + hex(PCC[0]) + " " + hex(v);
fcb.setRaw(128, 128); wait(200);
fcb.confChanged(NP.PICKUP, 1); waitMidi(100);
check("pedal A is in pickup mode, the configurator holds it", [fcb.state().pickup & 1, conf.v[NP.PICKUP]], [1, 1]);
fcb.midiIn([0xb0 | (fcb.state().midiChannel - 1), PCC[0], 20]); wait(20);
check("a received CC A of 20 leaves pedal A (at 64) waiting", fcb.state().pickup & 4, 4);
const expSeen = (ms) => { const seen = new Set(); for (let t = 0; t < ms; t += 8) { wait(8); seen.add(fcb.hw.aux[2]); } return [...seen].sort(); };
check("its EXP 1 LED flashes", expSeen(1500), [0, 1]);
let m0 = mark();
fcb.setRaw(140, 128); wait(100);
check("moving it sends nothing and shows the distance with a + (above the value)", [since(m0), fcb.hw.sign, fcb.hw.display, el("display").getAttribute("aria-label")], [[], 1, 50, "Display +50"]);
wait(pkShow + 100);
check("the display goes back to the bank after " + pkShow + "ms", [fcb.hw.sign, fcb.hw.display], [0, fcb.state().bank + 1]);
fcb.midiIn([0xb0 | (fcb.state().midiChannel - 1), PCC[0], 100]); wait(20);
fcb.setRaw(120, 128); wait(100);
check("with 100 received, at 60 it shows -40", [since(m0), fcb.hw.sign, fcb.hw.display], [[], -1, 40]);
m0 = mark();
fcb.setRaw(230, 128); wait(100);
const pkSent = since(m0);
check("passing 100 picks it up: the value is sent and the LED goes dark", [pkSent.length > 0, pkSent.every((x) => x.startsWith(ccA(0).slice(0, 5))), fcb.state().pickup & 4, expSeen(500)],
      [true, true, 0, [0]]);
m0 = mark();
fcb.setRaw(200, 128); wait(100);
check("then every move is sent", since(m0).slice(-1), [ccA(fcb.state().sent[0])]);
fcb.confChanged(NP.PICKUP, 0); waitMidi(100);
check("back to immediate", fcb.state().pickup & 1, 0);
check("the settings file keeps the pedals' response and CC", fcb.confBankFile(true).pedals, [{ response: "immediate", cc: PCC[0] }, { response: "immediate", cc: PCC[1] }]);
const pkFile = fcb.confBankFile(true); pkFile.pedals[1] = { response: "pickup" };
fcb.confLoad(pkFile); waitMidi(600);
check("and loading it sets it", fcb.state().pickup & 2, 2);
pkFile.pedals[1] = { response: "slow" };
check("a bad response is refused", [fcb.confLoad(pkFile), /response must be/.test(el("confStatus").textContent)], [false, true]);
// The pedal's CC over MIDI (NRPN 96 / 97) and from a file; each bank has its own.
pkFile.pedals[1] = { response: "pickup", cc: 4 };
fcb.confLoad(pkFile); waitMidi(600);
check("a file's pedal CC sets the bank's CC, and the panel label shows it", [fcb.state().pedalCc[1], String(el0("ccB").textContent)], [4, "4"]);
pkFile.pedals[0] = { response: "immediate", cc: 200 };
check("a CC over 127 is refused", [fcb.confLoad(pkFile), /cc must be 0 to 127/.test(el("confStatus").textContent)], [false, true]);
fcb.confChanged(NP.PEDAL_CC + 1, PCC[1]); waitMidi(100);
check("a set over NRPN 97 puts it back", fcb.state().pedalCc[1], PCC[1]);

console.log("editor: reading every bank without changing bank, and reset to defaults");
{
  const BANKS = Number(cfgText.match(/#define BANK_COUNT\s+(\d+)/)[1]);
  const NOTE1 = Number(cfgText.match(/#define NOTE_BANK_1\s+(\d+)/)[1]);
  const other = NOTE1 > 1 ? NOTE1 : 2;
  const until = (cond, n) => { for (let t = 0; t < n && !cond(); t += 50) waitMidi(50); };
  const bank0 = fcb.state().bank;
  m0 = mark();
  fcb.confRead(false);
  until(() => !conf.waiting, 30000);
  const pcs = since(m0).filter((l) => /^C/.test(l));
  check("Read fetches every bank, and the pedal stays on its bank (no program change sent)",
        [Object.keys(conf.banks).length, fcb.state().bank, pcs.length, /all \d+ banks/.test(el("confStatus").textContent)], [BANKS, bank0, 0, true]);
  if (NOTE1 > 0) check("another bank's stored settings came back (the note bank's notes)", conf.banks[NOTE1][NP.TYPE], 1);
  fcb.confShowBank(other);
  check("the bank box shows another bank at once, without switching", [conf.view, fcb.state().bank, el("cfBank").value, conf.v[NP.BANK]],
        [other, bank0, String(other), bank0 + 1]);
  const fsKey = (b, n) => (b === 1 ? "" : "b" + b) + "fs" + n, bankKey = (b) => (b === 1 ? "" : "b" + b) + "bank";
  const on0 = fcb.state().msgs[0].on, onNew = conf.banks[other][NP.ON] === 5 ? 6 : 5;
  fcb.confChanged(NP.ON, onNew); waitMidi(100);
  check("a change to a bank not in use is stored into it, and the pedal stays on its bank",
        [rec()[fsKey(other, 0)][2], fcb.state().msgs[0].on, fcb.state().bank, conf.view, /^Stored in bank \d+ \(the pedal stays on bank \d+\)/.test(el("confStatus").textContent)],
        [onNew, on0, bank0, other, true]);
  fire("cfUseBank", "click"); until(() => !conf.waiting && !conf.after, 3000);
  check("Use on pedal switches the pedal to it", [fcb.state().bank, conf.v[NP.BANK], conf.view], [other - 1, other, other]);
  fcb.confBank(bank0 + 1); until(() => !conf.waiting && !conf.after, 3000);
  check("and back", fcb.state().bank, bank0);
  fcb.confShowBank(other);
  fcb.confCopy();
  fcb.confShowBank(bank0 + 1);
  fcb.confPaste(); waitMidi(600);
  check("Copy from a bank shown (not in use), Paste into the bank in use", [conf.view, fcb.state().msgs[0].type], [bank0 + 1, NOTE1 === other ? 1 : 0]);
  fcb.confCommand(NC.RELOAD, "dropped"); idle(3000, waitMidi);
  // And the other way: Paste into a bank not in use stores it there.
  fcb.confShowBank(bank0 + 1);
  fcb.confCopy();
  fcb.confShowBank(other);
  fcb.confPaste(); waitMidi(300);
  check("Paste into a bank shown, not in use, stores it there; the pedal stays on its bank",
        [rec()[fsKey(other, 0)][0] & 3, rec()[fsKey(other, 0)][1], fcb.state().bank, conf.banks[other][NP.TYPE], /^Pasted bank \d+'s settings into bank \d+ and stored it/.test(el("confStatus").textContent)],
        [fcb.state().msgs[0].type, fcb.state().msgs[0].num, bank0, fcb.state().msgs[0].type, true]);

  // Reset to defaults from the panel: every bank and the shared settings stored as defaults.
  const DEF_CH = Number(cfgText.match(/#define MIDI_CHANNEL\s+(\d+)/)[1]);
  fcb.confShowBank(bank0 + 1);
  fcb.confChanged(NP.ON + 2, 33); waitMidi(100);
  fcb.confCommand(NC.SAVE, "stored"); idle(3000, waitMidi);
  check("a stored change before the reset", rec().fs2[2], 33);
  fcb.confDefaults(true);
  until(() => !conf.after && !conf.waiting, 40000);
  check("Reset to defaults stores the defaults, and the panel finds the pedal and reads every bank again",
        [rec().fs2[2], fcb.state().msgs[2].on, fcb.state().midiChannel, conf.ch, Object.keys(conf.banks).length, conf.known],
        [127, 127, DEF_CH, DEF_CH, BANKS, true]);

  // A whole-pedal file (version 3): every bank; loading it writes and stores only the banks that differ.
  const whole = fcb.confToFile();
  const inUse = whole.bank, far = inUse === BANKS ? 1 : BANKS;
  fcb.confDownload();
  check("Save to file says it has every bank", /: all \d+ banks\.$/.test(el("confStatus").textContent), true);
  check("Save to file keeps every bank, which one is in use, and the shared settings",
        [whole.version, whole.banks.map((b) => b.bank).join(), inUse, whole.global.channel, whole.banks[far - 1].footswitches[0].number],
        [3, [...Array(BANKS)].map((_, i) => i + 1).join(), fcb.state().bank + 1, DEF_CH, conf.banks[far][NP.NUM]]);
  whole.banks[far - 1].footswitches[3] = { mode: "momentary", type: "cc", number: 88, on: 99, off: 3 };
  whole.banks[inUse - 1].relays[1].pedalB = 21;
  check("loading it is accepted, with the progress bar shown", [fcb.confLoad(whole), el("confProgress").hidden], [true, false]);
  m0 = mark();
  until(() => !conf.job && !conf.after && !conf.waiting, 60000);
  check("only the two changed banks are written and stored, the bar hidden again, and it says so",
        [el("confStatus").textContent, el("confProgress").hidden],
        ["Stored 2 banks (" + [far, inUse].sort((a, b) => a - b).join(", ") + ") from the file; the pedal stayed on bank " + inUse + ". (Read back from the pedal.)", true]);
  check("no bank change, so no program change was sent", since(m0).filter((l) => /^C/.test(l)).length, 0);
  check("both banks are stored, the pedal is on its bank, and the read back shows them",
        [[rec()[fsKey(far, 3)][0] & 0x3F, ...rec()[fsKey(far, 3)].slice(1)], rec()[bankKey(inUse)][3] & 0x7F, fcb.state().bank, fcb.state().swThr[3], conf.banks[far][NP.NUM + 3], conf.known],
        [[2 << 2, 88, 99, 3], 21, inUse - 1, 21, 88, true]);
  fcb.confLoad(whole);
  check("loading the same file again sends nothing", [conf.job, /matches the pedal/.test(el("confStatus").textContent)], [null, true]);
  const beyond = JSON.parse(JSON.stringify(whole)); beyond.banks[0].bank = BANKS + 1;
  const twice = JSON.parse(JSON.stringify(whole)); twice.banks[1].bank = twice.banks[0].bank;
  check("a bank beyond the pedal's, or one listed twice, is refused",
        [fcb.confLoad(beyond), /beyond the pedal's/.test(el("confStatus").textContent), fcb.confLoad(twice), /in the file twice/.test(el("confStatus").textContent)], [false, true, false, true]);
  const badBank = JSON.parse(JSON.stringify(whole)); badBank.banks[far - 1].footswitches[0].on = 300;
  check("a bad value names its bank", [fcb.confLoad(badBank), new RegExp("bank " + far + ", footswitch 1 on value").test(el("confStatus").textContent)], [false, true]);

  // Power-on with footswitch 9 held: "Ini" flashes; DOWN cancels, UP resets.
  fcb.confChanged(NP.ON + 2, 44); waitMidi(100);
  fcb.confCommand(NC.SAVE, "stored"); idle(3000, waitMidi);
  fcb.reset(); fcb.down(9); wait(400);
  check("footswitch 9 held at power-on: Ini", [fcb.hw.text, el0("display").getAttribute("aria-label")], [27, "Display Ini"]);
  const lit = new Set(); for (let t = 0; t < 600; t += 10) { wait(10); lit.add(fcb.hw.blank & 7); }
  check("it flashes", [...lit].sort(), [0, 7]);
  fcb.up(9); wait(300);
  check("letting go of footswitch 9 keeps the prompt", fcb.hw.text, 27);
  press(DOWN, 100);
  check("DOWN cancels: settings kept, the bank shown", [fcb.hw.text, fcb.hw.display, rec().fs2[2]], [0, 1, 44]);
  fcb.reset(); fcb.down(9); wait(400);
  press(UP, 100); wait(200);
  check("UP resets to defaults", [fcb.hw.text, rec().fs2[2], fcb.state().msgs[2].on], [0, 127, 127]);
}

console.log(fail ? "editor smoke test FAILED" : "editor smoke test passed");
process.exit(fail);
