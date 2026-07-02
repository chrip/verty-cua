#!/usr/bin/env node
/* ═══════════════════════════════════════════════════════════════════════════
   record_session.mjs — deterministic scroll-session recorder (no deps).

   Drives headless Chromium via the DevTools Protocol, loads a page, scrolls it
   from top to bottom in fixed pixel steps, and writes one JPEG per step plus a
   ground_truth.json describing the exact scroll offset of every frame and the
   page's fixed-region bounding boxes.

   Requires only Node 22+ (built-in WebSocket + fetch) and `chromium` on PATH.

   Usage:
     node record_session.mjs [pageFile] [outDir]
       pageFile  HTML file to record   (default: ./scroll_page.html next to this)
       outDir    output directory      (default: ./session_page1)

   Tunables below: viewport size, scroll step, pauses, fps, JPEG quality.
   ═══════════════════════════════════════════════════════════════════════════ */

import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

// ── Config ──────────────────────────────────────────────────────────────────
const PAGE_FILE   = resolve(process.argv[2] || join(HERE, 'scroll_page.html'));
const OUT_DIR     = resolve(process.argv[3] || join(HERE, 'session_page1'));
const FRAMES_DIR  = join(OUT_DIR, 'frames');

const VIEWPORT_W  = 1280;   // exact frame width  (device px, scale factor 1)
const VIEWPORT_H  = 900;    // exact frame height
const AVG_STEP    = 12;     // average px/frame during a flick (varies via easing)
const FPS         = 30;     // intended playback fps (recorded into ground truth)
const JPEG_QUALITY = 92;    // 0..100
const SETTLE_MS   = 24;     // wait after each scroll for the page to repaint
const PORT        = 9240;

// Realistic scroll plan. Targets are fractions of maxScrollY; `pause` holds the
// position for N still frames (exercises the identical-frame pre-filter). Each
// scroll is an eased flick (slow→fast→slow). One step scrolls BACK UP, and the
// last lands at the very bottom, so the whole page is seen at least once.
const PAUSE_SHORT = 10;
const PAUSE_LONG  = 18;
const PLAN = [
  { pause: 12 },                          // rest at the top
  { to: 0.28 }, { pause: PAUSE_LONG },     // flick down, read
  { to: 0.58 }, { pause: PAUSE_LONG },     // flick down, read
  { to: 0.42 }, { pause: PAUSE_SHORT },    // ← scroll BACK UP (inverse), glance
  { to: 0.82 }, { pause: PAUSE_SHORT },    // flick down past where we were
  { to: 1.00 }, { pause: 14 },             // to the very bottom, rest
];

// ── CDP plumbing ──────────────────────────────────────────────────────────────
const chrome = spawn('chromium', [
  '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
  '--force-device-scale-factor=1', `--remote-debugging-port=${PORT}`,
  `--window-size=${VIEWPORT_W},${VIEWPORT_H}`, 'about:blank',
], { stdio: 'ignore', detached: true });

let target;
for (let i = 0; i < 60; i++) {
  await sleep(250);
  try {
    const list = await (await fetch(`http://127.0.0.1:${PORT}/json`)).json();
    target = list.find(t => t.type === 'page' && t.webSocketDebuggerUrl);
    if (target) break;
  } catch { /* not up yet */ }
}
if (!target) { console.error('Could not reach Chromium DevTools endpoint'); process.exit(1); }

const ws = new WebSocket(target.webSocketDebuggerUrl);
let msgId = 0;
const pending = new Map();
const call = (method, params = {}) => new Promise((res, rej) => {
  const id = ++msgId;
  pending.set(id, { res, rej });
  ws.send(JSON.stringify({ id, method, params }));
});
await new Promise(r => ws.addEventListener('open', r));
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    const { res, rej } = pending.get(m.id); pending.delete(m.id);
    m.error ? rej(new Error(m.error.message)) : res(m.result);
  }
});

const evaluate = async (expr) =>
  (await call('Runtime.evaluate', { expression: expr, returnByValue: true })).result.value;

// ── Recorder ────────────────────────────────────────────────────────────────
try {
  await call('Page.enable');
  await call('Runtime.enable');

  // Pin an exact viewport independent of window chrome.
  await call('Emulation.setDeviceMetricsOverride', {
    width: VIEWPORT_W, height: VIEWPORT_H, deviceScaleFactor: 1, mobile: false,
  });

  await call('Page.navigate', { url: 'file://' + PAGE_FILE });
  // Wait for load + our ground-truth object to be populated.
  for (let i = 0; i < 80; i++) {
    await sleep(100);
    if (await evaluate('document.readyState === "complete" && !!window.GROUND_TRUTH')) break;
  }
  await evaluate('window.scrollTo(0, 0)');
  await sleep(100);

  const gt = await evaluate('window.GROUND_TRUTH');
  const maxScrollY = gt.page.maxScrollY;
  console.log(`page: ${PAGE_FILE}`);
  console.log(`viewport ${VIEWPORT_W}×${VIEWPORT_H} | pageHeight ${gt.page.pageHeight}px | maxScrollY ${maxScrollY}px`);
  console.log(`fixed regions:`, gt.fixed);

  // Fresh output directory.
  rmSync(FRAMES_DIR, { recursive: true, force: true });
  mkdirSync(FRAMES_DIR, { recursive: true });

  const frames = [];
  let n = 0;
  const capture = async (scrollY) => {
    const { data } = await call('Page.captureScreenshot', { format: 'jpeg', quality: JPEG_QUALITY });
    const file = `frame_${String(n).padStart(5, '0')}.jpg`;
    writeFileSync(join(FRAMES_DIR, file), Buffer.from(data, 'base64'));
    frames.push({ index: n, file, scrollY });
    n++;
  };

  // ── Execute the scroll plan ─────────────────────────────────────────────────
  // Eased flicks (slow→fast→slow) between keyframes, reading pauses at each, and
  // one scroll-back-up before finishing at the bottom.
  const easeInOut = t => (t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);

  const resolvedPlan = [];
  let curY = 0;
  await evaluate('window.scrollTo(0, 0)'); await sleep(SETTLE_MS);

  for (const step of PLAN) {
    if (step.pause) {                                 // hold position (reading pause)
      for (let i = 0; i < step.pause; i++) await capture(curY);
      resolvedPlan.push({ pause: step.pause, atPx: curY });
      continue;
    }
    const targetY = Math.min(maxScrollY, Math.max(0, Math.round(step.to * maxScrollY)));
    const dist = targetY - curY;
    resolvedPlan.push({ to: step.to, toPx: targetY, dir: dist >= 0 ? 'down' : 'up' });
    if (dist === 0) continue;
    const steps = Math.max(2, Math.round(Math.abs(dist) / AVG_STEP));
    let lastY = curY;
    for (let i = 1; i <= steps; i++) {
      const y = Math.round(curY + dist * easeInOut(i / steps));
      if (y === lastY) continue;                      // skip duplicate positions mid-flick
      const actual = await evaluate(`window.scrollTo(0, ${y}); Math.round(window.scrollY)`);
      await sleep(SETTLE_MS);
      await capture(actual);
      lastY = y;
    }
    curY = targetY;
  }

  // ── Trajectory / coverage stats ─────────────────────────────────────────────
  const ys = frames.map(f => f.scrollY);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const upSegments = resolvedPlan.filter(p => p.dir === 'up').length;
  const sorted = [...new Set(ys)].sort((a, b) => a - b);
  let maxGap = 0;
  for (let i = 1; i < sorted.length; i++) maxGap = Math.max(maxGap, sorted[i] - sorted[i - 1]);
  const reachedBottom = maxY >= maxScrollY;

  const meta = {
    page: PAGE_FILE,
    recordedAt: new Date().toISOString(),
    viewport: { width: VIEWPORT_W, height: VIEWPORT_H, deviceScaleFactor: 1 },
    avgStep: AVG_STEP,
    fps: FPS,
    jpegQuality: JPEG_QUALITY,
    frameCount: frames.length,
    pageHeight: gt.page.pageHeight,
    maxScrollY,
    scrollRange: { min: minY, max: maxY },
    reachedBottom,
    upSegments,
    maxScrollGap: maxGap,   // largest jump between consecutive covered offsets
    plan: resolvedPlan,
    // The reconstructed-screenshot ground truth for the stitching algorithm:
    fixed: gt.fixed,
    regions: gt.regions,
    frames,
  };
  writeFileSync(join(OUT_DIR, 'ground_truth.json'), JSON.stringify(meta, null, 2));

  console.log(`\n✔ ${frames.length} frames → ${FRAMES_DIR}`);
  console.log(`✔ ground truth → ${join(OUT_DIR, 'ground_truth.json')}`);
  console.log(`  trajectory: scrollRange [${minY}, ${maxY}]px, ${upSegments} up-segment(s), `
            + `reachedBottom=${reachedBottom}, maxGap=${maxGap}px (< content window ⇒ full coverage)`);
} catch (e) {
  console.error('record failed:', e.message);
  process.exitCode = 1;
} finally {
  // Ask Chromium to quit itself over CDP. Signal-based kill is unreliable here:
  // the snap browser re-parents outside our process group, so process.kill()
  // gets EPERM. Browser.close shuts it down cleanly with no signals.
  try { await Promise.race([call('Browser.close'), sleep(2000)]); } catch { /* socket dropped */ }
  try { ws.close(); } catch {}
  try { process.kill(-chrome.pid, 'SIGKILL'); } catch { /* best-effort fallback */ }
}
