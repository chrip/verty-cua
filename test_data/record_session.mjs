#!/usr/bin/env node
/* ═══════════════════════════════════════════════════════════════════════════
   record_session.mjs — deterministic session recorder (no deps).

   Drives headless Chromium via the DevTools Protocol and writes one JPEG per
   captured frame plus a ground_truth.json describing exactly what happened
   (per-frame scroll offset, page id, hover/click/navigation events, and each
   page's fixed-region bounding boxes).

   Requires only Node 22+ (built-in WebSocket + fetch) and `chromium` on PATH.

   Usage:
     node record_session.mjs [--plan single|multipage] [pageFile] [outDir]

   Plans:
     single     (default) scroll one page top→bottom with eased flicks, reading
                pauses and one scroll-back-up.  Defaults: scroll_page.html →
                session_page1/.  Ground-truth schema: single-page (benchmark).
     multipage  visit page 1, scroll a little, hover a few links, click a link,
                land on page 2 and stay there for a few seconds.  Defaults:
                scroll_page.html → session_multipage/.
     revisit    page 1 → scroll → click to page 2 → scroll → HISTORY BACK to
                page 1 → scroll further (Milestone-4 fixture: recurring page).
                Defaults: scroll_page.html → session_revisit/.

   Multi-page ground-truth schema (multipage + revisit): pages[] are UNIQUE
   pages (keyed by file); visits[] is the chronological visit list; frames
   carry {page, scrollY} with page = unique page id (null during navigation
   transitions); events[] and navigations[] describe hovers/clicks/backs.

   Tunables below: viewport size, scroll step, pauses, fps, JPEG quality.
   ═══════════════════════════════════════════════════════════════════════════ */

import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

// ── CLI ─────────────────────────────────────────────────────────────────────
const argv = process.argv.slice(2);
let PLAN_NAME = 'single';
const planIdx = argv.indexOf('--plan');
if (planIdx >= 0) { PLAN_NAME = argv[planIdx + 1]; argv.splice(planIdx, 2); }
const PLANS = ['single', 'multipage', 'revisit', 'hostile',
               'noscroll', 'revisit_noscroll', 'cookie', 'jumps', 'fling',
               'manyjumps'];
if (!PLANS.includes(PLAN_NAME)) {
  console.error(`unknown plan: ${PLAN_NAME} (use ${PLANS.join('|')})`);
  process.exit(1);
}
const DEFAULT_OUT  = { single: 'session_page1', multipage: 'session_multipage',
                       revisit: 'session_revisit', hostile: 'session_hostile',
                       noscroll: 'session_noscroll',
                       revisit_noscroll: 'session_revisit_noscroll',
                       cookie: 'session_cookie', jumps: 'session_jumps',
                       fling: 'session_fling',
                       manyjumps: 'session_manyjumps' };
const DEFAULT_PAGE = { hostile: 'scroll_page_hostile.html',
                       cookie:  'scroll_page_cookie.html' };
const PAGE_FILE = resolve(argv[0] || join(HERE, DEFAULT_PAGE[PLAN_NAME] || 'scroll_page.html'));
const OUT_DIR   = resolve(argv[1] || join(HERE, DEFAULT_OUT[PLAN_NAME]));
const FRAMES_DIR = join(OUT_DIR, 'frames');

// ── Config ──────────────────────────────────────────────────────────────────
const VIEWPORT_W   = 1280;  // exact frame width  (device px, scale factor 1)
const VIEWPORT_H   = 900;   // exact frame height
const AVG_STEP     = 12;    // average px/frame during a flick (varies via easing)
const FPS          = 30;    // intended playback fps (recorded into ground truth)
const JPEG_QUALITY = 92;    // 0..100
const SETTLE_MS    = 24;    // wait after each scroll/mouse step for a repaint
const PORT         = 9240;

// Single-page plan: fractions of maxScrollY; `pause` holds N still frames.
// One step scrolls BACK UP, the last lands at the very bottom (full coverage).
const SINGLE_PLAN = [
  { pause: 12 },
  { to: 0.28 }, { pause: 18 },
  { to: 0.58 }, { pause: 18 },
  { to: 0.42 }, { pause: 10 },   // ← inverse scroll
  { to: 0.82 }, { pause: 10 },
  { to: 1.00 }, { pause: 14 },
];

// Multi-page plan: visit page 1, scroll a little, hover links (real :hover CSS
// lands in the video), click the CTA → page 2, park the mouse, stay a while.
const MULTIPAGE_PLAN = [
  { pause: 15 },                                                  // arrive
  { to: 0.12 },                                                   // scroll a little
  { pause: 12 },                                                  // read
  { hover: '#site-header nav a:nth-of-type(2)',     frames: 10 }, // header link
  { hover: '#left-sidebar nav a:nth-of-type(4)',    frames: 10 }, // sidebar link
  { hover: '#scroll-content a.next-article',        frames: 8 },  // the CTA
  { click: '#scroll-content a.next-article' },                    // → page 2
  { park:  { x: 30, y: 870 } },                                   // mouse off the links
  { pause: 75 },                                                  // stay ~2.5 s
];

// No-scroll plan: just look at the page. The result must be a single
// viewport-sized screenshot — no scroll events, no fixed elements claimed
// (nothing scrolled, so nothing is knowable about the chrome).
const NOSCROLL_PLAN = [{ pause: 90 }];

// Large-jump plan: instant scrollTo leaps (Page-Down / anchor-link style) —
// each jump lands in ONE frame. Exercises scroll offsets close to the
// viewport height, far beyond smooth-flick step sizes.
// Jumps are recoverable only while enough content is visible in BOTH frames:
// the hard limit is dy < viewport − header − footer (≈772px here), and
// periodic content (image placeholders, text lines) erodes reliability as the
// overlap shrinks. ~400px leaps (≈half the content window) are the measured
// reliable envelope — see the M7 notes in ROADMAP.md.
const JUMPS_PLAN = [
  { pause: 10 },
  { jump: 0.17 }, { pause: 10 },   // ≈400px leaps
  { jump: 0.34 }, { pause: 10 },
  { jump: 0.51 }, { pause: 10 },
  { jump: 0.68 }, { pause: 10 },
  { jump: 0.85 }, { pause: 10 },
  { jump: 1.00 }, { pause: 12 },
];

// Fling plan: see the whole document, then LEAP across it in a single frame —
// back to the top, then forward to the bottom again. Each leap is 1647-2353px:
// beyond MAX_SCROLL, and beyond the PHYSICAL limit of frame-to-frame matching
// (viewport − header − footer ≈ 772px here), so the frames either side of a
// leap share NO pixels and no frame-pair method can measure it. To a classifier
// that sees only two frames, a leap is indistinguishable from a navigation.
//
// Both leaps land in territory the session has ALREADY SEEN, which is what
// makes them recoverable: the page's own stitched canvas is the evidence, and
// re-localizing against it is intra-page re-localization by CONTENT (the
// scrollbar path cannot be exercised in the lab — CDP screenshots contain no
// scrollbar, verified across all fixtures; rw4_portal is its only test).
//
// The session never leaves the page, so the required OUTCOME is: exactly one
// page, ZERO navigation events, and each leap reported as one large scroll
// event. PAGE_CHANGE transitions are expected here and are not a failure —
// see PAGECHANGE_EXPECTED in test_stitch_session.py.
const FLING_PLAN = [
  { pause: 10 },
  { to: 1.00 },   { pause: 12 },   // see the whole document
  { jump: 0.00 }, { pause: 12 },   // leap back to the top      (−2353px)
  { to: 0.30 },   { pause: 10 },   // scroll down again (content-tracked)
  { jump: 1.00 }, { pause: 14 },   // leap back to the bottom   (+1647px)
];

// Many-visits plan: see the whole document once, then LEAP between its two ends
// ten times. Every leap is beyond the shared-pixel window, so each one splits the
// visit and is re-localized as a CONTINUATION — the result is ELEVEN visits on a
// SINGLE page, which is what stresses the cost of folding a visit into the page
// canvas. The correctness bar is the same as --plan fling (one page, no
// navigations, the leaps present as large scroll events); what this fixture adds
// is scale in the visit count, where a per-visit full rebuild is quadratic.
const MANYJUMPS_PLAN = [
  { pause: 8 },
  { to: 1.00 }, { pause: 8 },      // see the whole document (leaps need seen territory)
  { jump: 0.00 }, { pause: 5 },
  { jump: 1.00 }, { pause: 5 },
  { jump: 0.00 }, { pause: 5 },
  { jump: 1.00 }, { pause: 5 },
  { jump: 0.00 }, { pause: 5 },
  { jump: 1.00 }, { pause: 5 },
  { jump: 0.00 }, { pause: 5 },
  { jump: 1.00 }, { pause: 5 },
  { jump: 0.00 }, { pause: 5 },
  { jump: 1.00 }, { pause: 8 },
];

// Multi-page WITHOUT scrolling: page-change and REVISIT detection must work
// independently of both scrolling and fixed elements.
const REVISIT_NOSCROLL_PLAN = [
  { pause: 20 },
  { click: '#scroll-content a.next-article' },   // → page 2
  { pause: 20 },
  { back: true },                                 // history back → page 1
  { pause: 20 },
];

// Cookie-banner plan (borderline case): a large DOM mutation on the SAME page.
const COOKIE_PLAN = [
  { pause: 35 },                        // banner visible
  { clickLocal: '#cookie-accept' },     // DOM mutation — banner disappears
  { pause: 30 },
  { to: 0.35 }, { pause: 12 },          // then scroll a bit
  { to: 0.75 }, { pause: 12 },
];

// Revisit plan (Milestone-4 fixture): the session returns to a page it has
// already seen — via HISTORY BACK — and then scrolls further, extending the
// coverage its first visit left behind.
const REVISIT_PLAN = [
  { pause: 12 },                                                  // arrive page 1
  { to: 0.25 }, { pause: 10 },                                    // scroll (~590px)
  { click: '#scroll-content a.next-article' },                    // → page 2
  { pause: 12 },
  { to: 0.15 }, { pause: 10 },                                    // scroll page 2 a bit
  { back: true },                                                 // history back → page 1
  { pause: 12 },
  { to: 0.45 }, { pause: 10 },                                    // scroll deeper than visit 1
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

// evaluate that survives a navigation (execution context destroyed → null).
const tryEvaluate = async (expr) => {
  try { return await evaluate(expr); } catch { return null; }
};

// ── Shared recorder helpers ───────────────────────────────────────────────────
const frames = [];
let frameNo = 0;
const capture = async (extra) => {
  const { data } = await call('Page.captureScreenshot', { format: 'jpeg', quality: JPEG_QUALITY });
  const file = `frame_${String(frameNo).padStart(5, '0')}.jpg`;
  writeFileSync(join(FRAMES_DIR, file), Buffer.from(data, 'base64'));
  frames.push({ index: frameNo, file, ...extra });
  return frameNo++;
};

const easeInOut = t => (t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);

// Load a page and wait until its ground-truth instrumentation is ready.
async function loadPage(url) {
  await call('Page.navigate', { url });
  for (let i = 0; i < 80; i++) {
    await sleep(100);
    if (await tryEvaluate('document.readyState === "complete" && !!window.GROUND_TRUTH')) break;
  }
  await evaluate('window.scrollTo(0, 0)');
  await sleep(100);
  return evaluate('window.GROUND_TRUTH');
}

// Eased scroll from curY to targetY, capturing one frame per step.
// `frameMeta(scrollY)` supplies the per-frame ground-truth fields.
async function scrollTo(curY, targetY, frameMeta) {
  const dist = targetY - curY;
  if (dist === 0) return curY;
  const steps = Math.max(2, Math.round(Math.abs(dist) / AVG_STEP));
  let lastY = curY;
  for (let i = 1; i <= steps; i++) {
    const y = Math.round(curY + dist * easeInOut(i / steps));
    if (y === lastY) continue;
    const actual = await evaluate(`window.scrollTo(0, ${y}); Math.round(window.scrollY)`);
    await sleep(SETTLE_MS);
    await capture(frameMeta(actual));
    lastY = y;
  }
  return targetY;
}

// ── Mouse (viewport coordinates) ──────────────────────────────────────────────
const mouse = { x: 10, y: 10 };
async function moveMouse(x, y, steps = 3) {
  for (let i = 1; i <= steps; i++) {
    const mx = Math.round(mouse.x + (x - mouse.x) * (i / steps));
    const my = Math.round(mouse.y + (y - mouse.y) * (i / steps));
    await call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: mx, y: my });
    await sleep(SETTLE_MS);
  }
  mouse.x = x; mouse.y = y;
}

async function elementCenter(selector) {
  const c = await evaluate(`(() => {
    const el = document.querySelector(${JSON.stringify(selector)});
    if (!el) return null;
    const r = el.getBoundingClientRect();
    return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };
  })()`);
  if (!c) throw new Error(`element not found or not visible: ${selector}`);
  if (c.y < 0 || c.y > VIEWPORT_H || c.x < 0 || c.x > VIEWPORT_W)
    throw new Error(`element outside viewport: ${selector} at (${c.x},${c.y})`);
  return c;
}

// Reference render for perceptual scoring: the browser's own full-page
// screenshot, captured AFTER the session so it cannot disturb the frames, and
// with the fixed chrome HIDDEN — captureBeyondViewport renders position:fixed
// elements once at their viewport position (a footer would land mid-image),
// which is meaningless in document coordinates. Hiding fixed elements does not
// change layout, so fullpage.png is the pure scrolling document — exactly what
// the stitch interior is scored against.
//
// Captured after the session, this is the page in its SETTLED state: banners
// dismissed, late content loaded. That makes it the reference for whether the
// stitch reproduces the settled page rather than a transient early appearance.
async function captureSettledReference(pageHeight) {
  await evaluate('window.scrollTo(0, 0)');
  await evaluate(`document.querySelectorAll(
      '[data-region="header"],[data-region="footer"],[data-region="left"],' +
      '[data-region="right"],#back-to-top,#cookie-banner')
    .forEach(el => el.style.display = 'none')`);
  await sleep(200);
  const { data } = await call('Page.captureScreenshot', {
    format: 'png',
    clip: { x: 0, y: 0, width: VIEWPORT_W, height: pageHeight, scale: 1 },
    captureBeyondViewport: true,
  });
  writeFileSync(join(OUT_DIR, 'fullpage.png'), Buffer.from(data, 'base64'));
  console.log(`✔ content reference (chrome hidden, settled) → ${join(OUT_DIR, 'fullpage.png')} (${VIEWPORT_W}×${pageHeight})`);
}

// ── Plan: single (schema unchanged — the stitch benchmark consumes it) ───────
async function runSingle(plan) {
  const gt = await loadPage('file://' + PAGE_FILE);
  const maxScrollY = gt.page.maxScrollY;
  console.log(`page: ${PAGE_FILE}`);
  console.log(`viewport ${VIEWPORT_W}×${VIEWPORT_H} | pageHeight ${gt.page.pageHeight}px | maxScrollY ${maxScrollY}px`);
  console.log('fixed regions:', gt.fixed);

  const resolvedPlan = [];
  let curY = 0;
  await evaluate('window.scrollTo(0, 0)'); await sleep(SETTLE_MS);

  for (const step of plan) {
    if (step.pause) {
      for (let i = 0; i < step.pause; i++) await capture({ scrollY: curY });
      resolvedPlan.push({ pause: step.pause, atPx: curY });
      continue;
    }
    if (step.jump !== undefined) {
      const targetY = Math.min(maxScrollY, Math.round(step.jump * maxScrollY));
      const actual = await evaluate(`window.scrollTo(0, ${targetY}); Math.round(window.scrollY)`);
      await sleep(SETTLE_MS);
      await capture({ scrollY: actual });
      resolvedPlan.push({ jump: step.jump, toPx: targetY });
      curY = targetY;
      continue;
    }
    const targetY = Math.min(maxScrollY, Math.max(0, Math.round(step.to * maxScrollY)));
    resolvedPlan.push({ to: step.to, toPx: targetY, dir: targetY >= curY ? 'down' : 'up' });
    curY = await scrollTo(curY, targetY, scrollY => ({ scrollY }));
  }

  const ys = frames.map(f => f.scrollY);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const sorted = [...new Set(ys)].sort((a, b) => a - b);
  let maxGap = 0;
  for (let i = 1; i < sorted.length; i++) maxGap = Math.max(maxGap, sorted[i] - sorted[i - 1]);

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
    reachedBottom: maxY >= maxScrollY,
    upSegments: resolvedPlan.filter(p => p.dir === 'up').length,
    maxScrollGap: maxGap,
    plan: resolvedPlan,
    fixed: gt.fixed,
    regions: gt.regions,
    dynamicRegions: gt.dynamicRegions || [],
    frames,
  };
  writeFileSync(join(OUT_DIR, 'ground_truth.json'), JSON.stringify(meta, null, 2));
  console.log(`\n✔ ${frames.length} frames → ${FRAMES_DIR}`);
  console.log(`✔ ground truth → ${join(OUT_DIR, 'ground_truth.json')}`);
  console.log(`  trajectory: scrollRange [${minY}, ${maxY}]px, reachedBottom=${maxY >= maxScrollY}, maxGap=${maxGap}px`);

  if (PLAN_NAME === 'hostile') await captureSettledReference(gt.page.pageHeight);
}

// ── Multi-page session runner (plans: multipage, revisit) ────────────────────
async function runSession(plan) {
  const pages = [];                    // UNIQUE pages, keyed by file
  const pageIndexByFile = new Map();
  const visits = [];                   // chronological {page, firstFrame, lastFrame, arrivedBy}
  const events = [];
  const navigations = [];

  // Register the currently loaded page; returns its unique index + ground truth.
  const registerPage = async () => {
    const gt = await evaluate('window.GROUND_TRUTH');
    const file = await evaluate('decodeURIComponent(location.pathname)');
    if (!pageIndexByFile.has(file)) {
      pageIndexByFile.set(file, pages.length);
      pages.push({ file, pageHeight: gt.page.pageHeight, maxScrollY: gt.page.maxScrollY,
                   fixed: gt.fixed, regions: gt.regions });
    }
    return { idx: pageIndexByFile.get(file), gt };
  };

  let gt = await loadPage('file://' + PAGE_FILE);
  let pageIdx = (await registerPage()).idx;
  let curY = 0;
  visits.push({ page: pageIdx, firstFrame: 0, lastFrame: -1, arrivedBy: 'initial' });
  console.log(`page ${pageIdx}: ${PAGE_FILE} (pageHeight ${gt.page.pageHeight}px)`);

  // Capture through a navigation until the new page is stable, then register
  // it. Transition frames (state unreadable / still loading) get page:null —
  // the window the segmenter must skip. Stable frames are patched to the new
  // page id once it is known.
  const navigate = async (kind, triggerFrame) => {
    const fromPage = pageIdx;
    visits[visits.length - 1].lastFrame = frameNo - 1;

    let stable = 0, firstStableFrame = -1;
    const stableIdx = [];
    for (let i = 0; i < 200 && stable < 3; i++) {
      await sleep(SETTLE_MS);
      const st = await tryEvaluate(
        '(document.readyState === "complete" && window.GROUND_TRUTH) ? {ready: true, scrollY: Math.round(window.scrollY)} : null');
      if (st && st.ready) {
        if (firstStableFrame < 0) firstStableFrame = frameNo;
        stable++;
        stableIdx.push(frameNo);
        await capture({ page: -1, scrollY: st.scrollY });   // patched below
      } else {
        stable = 0;
        firstStableFrame = -1;
        stableIdx.length = 0;
        await capture({ page: null, scrollY: null });
      }
    }

    const reg = await registerPage();
    pageIdx = reg.idx;
    gt = reg.gt;
    for (const i of stableIdx) frames[i].page = pageIdx;
    curY = await evaluate('Math.round(window.scrollY)');

    navigations.push({ fromPage, toPage: pageIdx, kind, triggerFrame, firstStableFrame });
    visits.push({ page: pageIdx, firstFrame: firstStableFrame, lastFrame: -1, arrivedBy: kind });
    events.push({ type: 'pageLoaded', page: pageIdx, frame: firstStableFrame });
    console.log(`${kind} @frame ${triggerFrame} → page ${pageIdx} `
              + `(scrollY ${curY}), stable @frame ${firstStableFrame}`);
  };

  for (const step of plan) {
    if (step.pause) {
      for (let i = 0; i < step.pause; i++) await capture({ page: pageIdx, scrollY: curY });
      continue;
    }
    if (step.to !== undefined) {
      const targetY = Math.min(gt.page.maxScrollY, Math.max(0, Math.round(step.to * gt.page.maxScrollY)));
      curY = await scrollTo(curY, targetY, scrollY => ({ page: pageIdx, scrollY }));
      continue;
    }
    if (step.hover) {
      const c = await elementCenter(step.hover);
      await moveMouse(c.x, c.y);
      const start = frameNo;
      for (let i = 0; i < step.frames; i++) { await sleep(SETTLE_MS); await capture({ page: pageIdx, scrollY: curY }); }
      events.push({ type: 'hover', selector: step.hover, frameStart: start, frameEnd: frameNo - 1 });
      console.log(`hover ${step.hover} → frames ${start}..${frameNo - 1}`);
      continue;
    }
    if (step.park) {
      await moveMouse(step.park.x, step.park.y);
      continue;
    }
    if (step.clickLocal) {
      const c = await elementCenter(step.clickLocal);
      await moveMouse(c.x, c.y);
      const triggerFrame = frameNo;
      await call('Input.dispatchMouseEvent', { type: 'mousePressed',  x: c.x, y: c.y, button: 'left', clickCount: 1 });
      await call('Input.dispatchMouseEvent', { type: 'mouseReleased', x: c.x, y: c.y, button: 'left', clickCount: 1 });
      events.push({ type: 'mutation', selector: step.clickLocal, frame: triggerFrame });
      console.log(`mutation ${step.clickLocal} @frame ${triggerFrame} (same page)`);
      await sleep(SETTLE_MS * 3);   // let the mutation render
      continue;
    }
    if (step.click) {
      const c = await elementCenter(step.click);
      await moveMouse(c.x, c.y);
      const triggerFrame = frameNo;
      await call('Input.dispatchMouseEvent', { type: 'mousePressed',  x: c.x, y: c.y, button: 'left', clickCount: 1 });
      await call('Input.dispatchMouseEvent', { type: 'mouseReleased', x: c.x, y: c.y, button: 'left', clickCount: 1 });
      events.push({ type: 'click', selector: step.click, frame: triggerFrame });
      await navigate('click', triggerFrame);
      continue;
    }
    if (step.back) {
      const triggerFrame = frameNo;
      const h = await call('Page.getNavigationHistory');
      if (h.currentIndex <= 0) throw new Error('no history entry to go back to');
      events.push({ type: 'back', frame: triggerFrame });
      await call('Page.navigateToHistoryEntry', { entryId: h.entries[h.currentIndex - 1].id });
      await navigate('back', triggerFrame);
      continue;
    }
  }
  visits[visits.length - 1].lastFrame = frameNo - 1;

  // The cookie page SETTLES during the session — the banner is dismissed and the
  // real footer takes its place. A settled full-page reference therefore makes
  // the question "does the stitch show the settled page, or a transient early
  // appearance frozen in?" measurable by the existing ghost metric.
  if (PLAN_NAME === 'cookie' && pages.length === 1)
    await captureSettledReference(pages[0].pageHeight);

  const meta = {
    plan: PLAN_NAME,
    recordedAt: new Date().toISOString(),
    viewport: { width: VIEWPORT_W, height: VIEWPORT_H, deviceScaleFactor: 1 },
    avgStep: AVG_STEP,
    fps: FPS,
    jpegQuality: JPEG_QUALITY,
    frameCount: frames.length,
    pages,
    visits,
    events,
    navigations,
    frames,
  };
  writeFileSync(join(OUT_DIR, 'ground_truth.json'), JSON.stringify(meta, null, 2));
  console.log(`\n✔ ${frames.length} frames → ${FRAMES_DIR}`);
  console.log(`✔ ground truth → ${join(OUT_DIR, 'ground_truth.json')}`);
  console.log(`  unique pages: ${pages.length}, visits: ${visits.length}, `
            + `navigations: ${navigations.length}, `
            + `transition frames: ${frames.filter(f => f.page === null).length}`);
}

// ── Run ───────────────────────────────────────────────────────────────────────
try {
  await call('Page.enable');
  await call('Runtime.enable');
  await call('Emulation.setDeviceMetricsOverride', {
    width: VIEWPORT_W, height: VIEWPORT_H, deviceScaleFactor: 1, mobile: false,
  });

  rmSync(FRAMES_DIR, { recursive: true, force: true });
  mkdirSync(FRAMES_DIR, { recursive: true });

  if      (PLAN_NAME === 'multipage')        await runSession(MULTIPAGE_PLAN);
  else if (PLAN_NAME === 'revisit')          await runSession(REVISIT_PLAN);
  else if (PLAN_NAME === 'revisit_noscroll') await runSession(REVISIT_NOSCROLL_PLAN);
  else if (PLAN_NAME === 'cookie')           await runSession(COOKIE_PLAN);
  else if (PLAN_NAME === 'noscroll')         await runSingle(NOSCROLL_PLAN);
  else if (PLAN_NAME === 'jumps')            await runSingle(JUMPS_PLAN);
  else if (PLAN_NAME === 'fling')            await runSingle(FLING_PLAN);
  else if (PLAN_NAME === 'manyjumps')        await runSingle(MANYJUMPS_PLAN);
  else                                       await runSingle(SINGLE_PLAN);   // single + hostile
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
