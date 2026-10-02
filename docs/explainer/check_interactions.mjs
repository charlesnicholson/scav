#!/usr/bin/env node
// Drives the built explainer in headless Chrome with real mouse and keyboard input, and checks every
// hover, click, drag, slider, checkbox and filter box, once as loaded and once after the theme toggle.
// usage: node check_interactions.mjs [--page FILE] [--out DIR] [--only ID,ID] [--jobs N] [--no-theme] [--chrome BIN] [--budget SECS]
import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, dflt) => { const k = args.indexOf(name); return k >= 0 ? args[k + 1] : dflt; };
const PAGE = resolve(opt('--page', join(HERE, 'scav-explained.html')));
const OUT = resolve(opt('--out', join(tmpdir(), 'sx-interactions')));
const ONLY = (opt('--only', '') || '').split(',').filter(Boolean);
const THEME = !args.includes('--no-theme');
const CHROME = opt('--chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
const BUDGET = +opt('--budget', 900);                     // whole-run cap in seconds; Chrome dies with us
const VIEW = { w: 1440, h: 900 };
const JOBS = +opt('--jobs', 1);                         // > 1: shard the sections over that many browsers

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- Chrome and the DevTools protocol ------------------------------------------------------------
class CDP {
  constructor(ws) {
    this.ws = ws; this.seq = 0; this.wait = new Map(); this.subs = [];
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      if (msg.id) {
        const p = this.wait.get(msg.id); this.wait.delete(msg.id);
        if (msg.error) p.no(new Error(`${p.method}: ${msg.error.message}`)); else p.ok(msg.result);
      } else this.subs.forEach((f) => f(msg));
    };
  }
  send(method, params = {}) {
    const id = ++this.seq;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((ok, no) => this.wait.set(id, { ok, no, method }));
  }
  on(f) { this.subs.push(f); }
}

let chrome = null, profile = null;
function cleanup() {
  if (chrome && chrome.exitCode == null) { try { chrome.kill('SIGKILL'); } catch {} }
  if (profile) { try { rmSync(profile, { recursive: true, force: true }); } catch {} }
}
process.on('exit', cleanup);
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => { cleanup(); process.exit(130); });
setTimeout(() => { console.error(`budget of ${BUDGET}s spent; stopping`); cleanup(); process.exit(124); }, BUDGET * 1000).unref();

async function launch() {
  profile = mkdtempSync(join(tmpdir(), 'sx-chrome-'));
  chrome = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
    `--window-size=${VIEW.w},${VIEW.h}`, '--no-first-run', '--no-default-browser-check', '--hide-scrollbars',
    '--force-color-profile=srgb', '--force-prefers-reduced-motion', 'about:blank'], { stdio: 'ignore' });
  const portFile = join(profile, 'DevToolsActivePort');
  let port = '';
  for (let k = 0; k < 300 && !/^\d+$/.test(port); k++) {   // Chrome creates the file before it writes the port
    await sleep(50);
    if (existsSync(portFile)) port = readFileSync(portFile, 'utf8').split('\n')[0].trim();
  }
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const page = list.find((t) => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((ok, no) => { ws.onopen = ok; ws.onerror = no; });
  return new CDP(ws);
}

// ---- in-page side --------------------------------------------------------------------------------
// Runs before the page's scripts: records each listener's event types on its target, so the
// inventory sees exactly what the page bound. DOMDebugger cross-checks it after load.
function instrument() {
  const add = EventTarget.prototype.addEventListener;
  EventTarget.prototype.addEventListener = function (type, fn, o) {
    try { (this.__hxl || (this.__hxl = new Set())).add(type); } catch (e) { /* frozen targets */ }
    return add.call(this, type, fn, o);
  };
}

// Installs window.__hx: inventory, hit points, before/after state. Elements are keyed by section,
// figure and child-index path, so a redraw that rebuilds the same structure resolves to the new node.
function pageLib() {
  const HOVER = ['mousemove', 'mouseenter', 'mouseover', 'pointerenter', 'pointerover'];
  const CLICK = ['click', 'dblclick'];
  const DRAG = ['pointerdown', 'mousedown'];
  const H = window.__hx = { reg: new Map(), tags: new Map() };
  const tipEl = () => document.getElementById('sx-tip');

  function rootOf(el) { return el.closest('[data-fig]') || el.closest('section.sx'); }
  function pathOf(el, root) {
    const p = [];
    for (let n = el; n && n !== root; n = n.parentNode) p.unshift([].indexOf.call(n.parentNode.children, n));
    return p.join('.');
  }
  function byPath(root, path) {
    let n = root;
    for (const k of path ? path.split('.') : []) n = n && n.children[+k];
    return n || null;
  }
  function describe(el) {
    if (!el || !el.tagName) return 'nothing';
    const tag = el.tagName.toLowerCase(), cls = (el.getAttribute('class') || '').trim();
    let text = (el.textContent || '').trim().replace(/\s+/g, ' ');
    if (tag === 'input') text = el.type + (el.type === 'range' ? ` ${el.min}..${el.max}` : '');
    if (tag === 'svg' || tag === 'g' || tag === 'figure' || tag === 'div') text = text.slice(0, 18);
    const data = [].filter.call(el.attributes, (a) => a.name.startsWith('data-') && a.name !== 'data-fig')
      .map((a) => `${a.name.slice(5)}=${a.value}`).join(' ');
    return `${tag}${cls ? '.' + cls.split(/\s+/).join('.') : ''}${data ? '[' + data + ']' : ''}${text ? ' "' + text.slice(0, 36) + '"' : ''}`;
  }
  H.describe = describe;
  const sig = (el) => `${el.tagName}|${el.getAttribute('class') || ''}|${el.__hxl ? [...el.__hxl].sort().join(',') : ''}`;
  H.sig = (key) => H.tags.get(key);
  // An element, or an ancestor inside its section, that answers the pointer itself.
  const live = (t) => { for (let n = t; n && n.tagName !== 'SECTION'; n = n.parentNode) if (n.__hxl && kinds(n).length) return true; return false; };
  function kinds(el) {
    const tag = el.tagName.toLowerCase(), t = el.__hxl ? [...el.__hxl] : [];
    if (tag === 'input') return el.type === 'range' ? ['range'] : el.type === 'checkbox' ? ['check'] : ['text'];
    if (tag === 'button') return ['click'];
    const k = [];
    if (t.some((x) => HOVER.includes(x))) k.push('hover');
    if (t.some((x) => CLICK.includes(x))) k.push('click');
    if (t.some((x) => DRAG.includes(x))) k.push('drag');
    return k;
  }
  H.sections = () => [].map.call(document.querySelectorAll('section.sx'), (s) => s.id);
  H.scrollTo = (id) => { document.getElementById(id).scrollIntoView({ block: 'start', behavior: 'instant' }); return scrollY; };
  H.figCounts = () => {
    const o = {};
    document.querySelectorAll('[data-fig]').forEach((h) => { o[h.closest('section').id + '/' + h.getAttribute('data-fig')] = h.querySelectorAll('figure.sx-fig').length; });
    return o;
  };
  H.failNotes = () => [].map.call(document.querySelectorAll('section.sx p.note'), (p) => p.textContent).filter((t) => /failed to load/.test(t));
  H.listenerCount = () => {
    const o = {};
    document.querySelectorAll('section.sx').forEach((s) => { let n = 0; s.querySelectorAll('*').forEach((el) => { if (el.__hxl) n += el.__hxl.size; }); o[s.id] = n; });
    return o;
  };
  // Every interactive element in a section, in document order.
  H.discover = (secId) => {
    const sec = document.getElementById(secId), out = [];
    sec.querySelectorAll('*').forEach((el) => {
      const tag = el.tagName.toLowerCase();
      if (!el.__hxl && tag !== 'button' && tag !== 'input') return;
      const k = kinds(el);
      if (!k.length) return;
      const root = rootOf(el), fig = root.getAttribute('data-fig') || '(section)';
      const key = `${secId}|${fig}|${pathOf(el, root)}`;
      H.reg.set(key, el); H.tags.set(key, sig(el));
      const r = el.getBoundingClientRect();
      out.push({ key, sig: sig(el), fig, desc: describe(el), kinds: k, types: el.__hxl ? [...el.__hxl].sort() : [], size: [Math.round(r.width), Math.round(r.height)] });
    });
    return out;
  };
  H.resolve = (key) => {
    let el = H.reg.get(key);
    if (el && el.isConnected) return el;
    const [secId, fig, path] = key.split('|');
    const sec = document.getElementById(secId);
    const root = fig === '(section)' ? sec : sec.querySelector(`[data-fig="${fig}"]`);
    el = root && byPath(root, path);
    if (!el || sig(el) !== H.tags.get(key)) return null;
    H.reg.set(key, el);
    return el;
  };
  // Screen points inside the element's own hit geometry (fill or stroke per its pointer-events),
  // clipped to the viewport. Thin strokes also get points along the centreline.
  function samples(el) {
    const r0 = el.getBoundingClientRect(), pts = [];
    let pad = 0;
    if (el instanceof SVGGeometryElement) { const m = el.getScreenCTM(); pad = (parseFloat(getComputedStyle(el).strokeWidth) || 0) * Math.hypot(m.a, m.b) / 2; }
    const r = { left: r0.left - pad, right: r0.right + pad, top: r0.top - pad, bottom: r0.bottom + pad };
    const x0 = Math.max(r.left + 1, 1), x1 = Math.min(r.right - 1, innerWidth - 2);
    const y0 = Math.max(r.top + 1, 1), y1 = Math.min(r.bottom - 1, innerHeight - 2);
    if (x1 < x0 || y1 < y0) return { pts, mode: 'offscreen' };
    const N = 13;
    const grid = [];
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) grid.push([x0 + (x1 - x0) * (i + 0.5) / N, y0 + (y1 - y0) * (j + 0.5) / N]);
    if (!(el instanceof SVGGeometryElement)) return { pts: grid, mode: 'box' };
    const cs = getComputedStyle(el), pe = cs.pointerEvents;
    if (pe === 'none') return { pts, mode: 'pointer-events:none' };
    const hasFill = cs.fill !== 'none', hasStroke = cs.stroke !== 'none';
    const useFill = ['fill', 'visibleFill', 'all', 'visible', 'bounding-box'].includes(pe) || (['auto', 'visiblePainted', 'painted'].includes(pe) && hasFill);
    const useStroke = ['stroke', 'visibleStroke', 'all', 'visible'].includes(pe) || (['auto', 'visiblePainted', 'painted'].includes(pe) && hasStroke);
    const ctm = el.getScreenCTM(), inv = ctm.inverse();
    const inside = ([x, y]) => {
      const p = new DOMPoint(x, y).matrixTransform(inv);
      return (useFill && el.isPointInFill(p)) || (useStroke && el.isPointInStroke(p));
    };
    grid.forEach((p) => { if (inside(p)) pts.push(p); });
    if (useStroke && typeof el.getTotalLength === 'function') {
      const L = el.getTotalLength();
      for (let k = 0; k <= 48; k++) {
        const q = el.getPointAtLength(L * k / 48), s = new DOMPoint(q.x, q.y).matrixTransform(ctm);
        if (s.x > 1 && s.y > 1 && s.x < innerWidth - 2 && s.y < innerHeight - 2) pts.push([s.x, s.y]);
      }
    }
    return { pts, mode: (useFill ? 'fill' : '') + (useStroke ? 'stroke' : ''), inside };
  }
  // Scrolls the element to the middle of the viewport and hit-tests its own points: the share that
  // reach it, what covers the rest, and a reachable point near the middle of the reachable ones.
  H.point = (key) => {
    const el = H.resolve(key);
    if (!el) return { gone: true };
    el.scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'instant' });
    const { pts, mode, inside } = samples(el);
    const hit = [], blockers = {};
    let livecover = 0;
    pts.forEach((p) => {
      const x = Math.round(p[0]), y = Math.round(p[1]), t = document.elementFromPoint(x, y);
      if (t && (t === el || el.contains(t))) hit.push([x, y]);
      else if (t && live(t)) livecover++;
      else { const d = describe(t); blockers[d] = (blockers[d] || 0) + 1; }
    });
    const r = el.getBoundingClientRect();
    let best = null;
    if (hit.length) {
      const cx = hit.reduce((a, p) => a + p[0], 0) / hit.length, cy = hit.reduce((a, p) => a + p[1], 0) / hit.length;
      best = hit.reduce((a, p) => (Math.hypot(p[0] - cx, p[1] - cy) < Math.hypot(a[0] - cx, a[1] - cy) ? p : a), hit[0]);
    }
    // Small targets: points 2 px inside each edge midpoint, where they lie in the element.
    let edges = null;
    if (Math.min(r.width, r.height) < 24 && r.width > 4 && r.height > 4) {
      const cand = [[r.left + 2, (r.top + r.bottom) / 2], [r.right - 2, (r.top + r.bottom) / 2], [(r.left + r.right) / 2, r.top + 2], [(r.left + r.right) / 2, r.bottom - 2]]
        .filter((p) => !inside || inside(p));
      edges = { n: cand.length, hit: 0, blockers: [] };
      cand.forEach(([x, y]) => {
        const t = document.elementFromPoint(x, y);
        if (t && (t === el || el.contains(t) || live(t))) edges.hit++; else edges.blockers.push(describe(t));
      });
    }
    return { x: best && best[0], y: best && best[1], n: pts.length - livecover, hits: hit.length, livecover, mode, blockers, edges,
      rect: [r.left, r.top, r.width, r.height] };
  };
  H.tip = () => {
    const t = tipEl(), vis = !!t && getComputedStyle(t).display !== 'none' && t.offsetWidth > 0;
    const r = t ? t.getBoundingClientRect() : null;
    return { vis, text: vis ? t.textContent.replace(/\s+/g, ' ').trim() : '',
      onscreen: !vis || (r.left >= 0 && r.top >= 0 && r.right <= innerWidth && r.bottom <= innerHeight) };
  };
  H.state = (key) => {
    const el = H.resolve(key);
    if (!el) return { gone: true, tip: H.tip() };
    const r = el.getBoundingClientRect(), st = { tip: H.tip(), cx: r.left + r.width / 2, cy: r.top + r.height / 2,
      w: r.width, h: r.height, text: (el.textContent || '').trim().slice(0, 60), on: el.classList.contains('on') };
    if (el.tagName === 'INPUT') { st.value = el.value; st.checked = el.checked; }
    const p = el.parentNode;
    if (el.tagName === 'BUTTON' && p) {
      const sib = [].filter.call(p.children, (b) => b.tagName === 'BUTTON');
      st.group = sib.map((b) => b.classList.contains('on') ? 1 : 0);
      st.index = sib.indexOf(el);
      const rng = p.querySelector('input[type=range]');
      if (rng) { st.range = +rng.value; st.rmin = +rng.min; st.rmax = +rng.max; st.readout = (p.querySelector('.readout') || {}).textContent; }
    }
    return st;
  };
  // Counts DOM changes in the element's section from now until H.changes().
  H.watch = (key) => {
    if (H.mo) H.mo.disconnect();
    H.muts = 0;
    const sec = document.getElementById(key.split('|')[0]);
    H.mo = new MutationObserver((l) => { H.muts += l.length; });
    H.mo.observe(sec, { subtree: true, childList: true, attributes: true, characterData: true });
    return H.state(key);
  };
  H.changes = () => { if (H.mo) H.muts += H.mo.takeRecords().length; return H.muts; };
  H.range = (key) => {
    const el = H.resolve(key);
    el.scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'instant' });
    const r = el.getBoundingClientRect(), lo = +el.min, hi = +el.max, v = +el.value, T = 16;
    const at = (val) => r.left + T / 2 + (hi > lo ? (val - lo) / (hi - lo) : 0) * (r.width - T);
    const t = document.elementFromPoint(at(v), r.top + r.height / 2);
    return { lo, hi, v, step: el.step, y: r.top + r.height / 2, x: at(v), xlo: at(lo), xhi: at(hi), w: r.width,
      ok: t === el, blocker: t === el ? '' : describe(t) };
  };
  // Every value of a range, synthetically: catches an onStep that throws partway through.
  H.sweep = (key, cap) => {
    const el = H.resolve(key), lo = +el.min, hi = +el.max, st = +el.step || 1, v0 = el.value;
    const n = Math.round((hi - lo) / st), k = Math.min(n, cap), done = [];
    for (let i = 0; i <= k; i++) {
      el.value = lo + Math.round(i * n / k) * st;
      el.dispatchEvent(new Event('input', { bubbles: true }));
      done.push(el.value);
    }
    el.value = v0; el.dispatchEvent(new Event('input', { bubbles: true }));
    return done.length;
  };
  H.frame = () => new Promise((ok) => requestAnimationFrame(() => setTimeout(() => ok(1), 0)));
  H.mark = (x, y) => {
    let m = document.getElementById('__hx-mark');
    if (!m) { m = document.createElement('div'); m.id = '__hx-mark'; document.body.appendChild(m); }
    Object.assign(m.style, { position: 'fixed', left: (x - 7) + 'px', top: (y - 7) + 'px', width: '14px', height: '14px',
      border: '2px solid red', borderRadius: '50%', pointerEvents: 'none', zIndex: 99, display: 'block' });
  };
  H.unmark = () => { const m = document.getElementById('__hx-mark'); if (m) m.style.display = 'none'; };
  H.figRect = (key) => {
    const el = H.resolve(key), f = el && (el.closest('figure') || el.closest('section'));
    if (!f) return null;
    const r = f.getBoundingClientRect();
    return [r.left, r.top, r.width, r.height];
  };
}

// ---- the driver ----------------------------------------------------------------------------------
const NEUTRAL = [124, VIEW.h - 8];                       // empty foot of the contents rail: no listeners
const BAD_TEXT = /undefined|\bNaN\b|\bnull\b|\bInfinity\b|\[object/;
const EXPECT = {
  hover: 'tooltip with sensible text, or a highlight; tooltip hides on leave',
  click: 'the drawing or the control changes',
  drag: 'the dragged mark follows the pointer; the drawing updates',
  range: 'the value follows the thumb; the drawing updates; every value draws',
  check: 'checks and unchecks; the drawing updates both ways',
  text: 'typing filters; clearing restores',
};

let cdp, errs = [];
const ev = async (expr, awaitPromise = false) => {
  const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise });
  if (r.exceptionDetails) throw new Error(`evaluate ${expr.slice(0, 60)}: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
  return r.result.value;
};
const call = (fn, ...a) => ev(`__hx.${fn}(${a.map((x) => JSON.stringify(x)).join(',')})`);
const frame = () => ev('__hx.frame()', true);
const mouse = (type, x, y, pressed = false) => cdp.send('Input.dispatchMouseEvent', {
  type, x, y, button: type === 'mouseMoved' && !pressed ? 'none' : 'left', buttons: pressed ? 1 : 0, clickCount: type === 'mouseMoved' ? 0 : 1 });
async function moveTo(x, y) { await mouse('mouseMoved', x, y); await frame(); }
async function clickAt(x, y) {
  await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, true); await mouse('mouseReleased', x, y); await frame();
}
async function press(k, code, vk) {
  await cdp.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: k, code, windowsVirtualKeyCode: vk });
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: vk });
}
const errSince = (n) => errs.slice(n);
const pct = (a, b) => (b ? Math.round(100 * a / b) : 0);
function blockerText(p) {
  return Object.entries(p.blockers || {}).sort((a, b) => b[1] - a[1]).slice(0, 2).map(([d, n]) => `${d} ×${n}`).join('; ');
}

async function testHover(key, p) {
  await moveTo(...NEUTRAL);
  const s0 = await call('watch', key);
  await moveTo(p.x, p.y);
  const m1 = await call('changes'), s1 = await call('state', key);
  await moveTo(...NEUTRAL);
  const m2 = (await call('changes')) - m1, s2 = await call('state', key);
  const bad = [], note = [];
  if (!s1.tip.vis && !m1) bad.push('no tooltip and no highlight');
  if (s1.tip.vis && (!s1.tip.text || BAD_TEXT.test(s1.tip.text))) bad.push(`tooltip text "${s1.tip.text.slice(0, 60)}"`);
  if (s1.tip.vis && !s1.tip.onscreen) bad.push('tooltip runs off the viewport');
  if (s2.tip.vis) bad.push('tooltip stays after the pointer leaves');
  if (m1 && !m2 && !s1.tip.vis) note.push('highlight kept after leave');
  const got = s1.tip.vis ? `tip "${s1.tip.text.slice(0, 70)}"` : `${m1} changes`;
  return { bad, note, got: got + (s0.tip.vis ? ' (tip was already up)' : '') };
}

async function testClick(key, p) {
  await moveTo(p.x, p.y);
  const s0 = await call('watch', key);
  await mouse('mousePressed', p.x, p.y, true); await mouse('mouseReleased', p.x, p.y); await frame();
  const m = await call('changes'), s1 = await call('state', key), bad = [], note = [];
  let got = `${m} changes`;
  if (s1.gone) { got += ', node replaced'; }
  if (s0.group && s0.group.length > 1 && s0.group.some(Boolean)) {   // a choice
    const on = s1.group || [];
    got = `on = [${on.join('')}]`;
    const sum = on.reduce((a, b) => a + b, 0), wasOn = s0.group[s0.index] === 1;   // clicking the lit one may toggle it off
    if (!((on[s1.index] === 1 && sum === 1) || (wasOn && sum === 0))) bad.push('choice did not move its "on" state to the clicked button');
    if (!m) bad.push('nothing redrew');
  } else if (s0.range != null && /^(◀|▶)$/.test(s0.text)) {           // stepper prev / next
    const atBound = s0.text === '◀' ? s0.range <= s0.rmin : s0.range >= s0.rmax;
    got = `step ${s0.range} → ${s1.range} (${s1.readout})`;
    if (s1.range === s0.range && !atBound) bad.push('step did not move');
  } else if (s0.range != null && /^(Play|Pause)$/.test(s0.text)) {     // stepper play
    got = `"${s0.text}" → "${s1.text}"`;
    if (s0.text === 'Play') {
      if (s1.text !== 'Pause') bad.push('Play did not start');
      let moved = false;
      for (let k = 0; k < 30 && !moved; k++) { await sleep(100); moved = (await call('state', key)).range !== s1.range; }
      if (!moved && s1.text === 'Pause') bad.push('playing did not advance the step');
      await clickAt(p.x, p.y);
      const s2 = await call('state', key);
      got += ` → "${s2.text}"${moved ? ', advanced' : ''}`;
      if (s2.text !== 'Play') bad.push('second click did not pause');
    }
  } else if (!m && !s1.gone) bad.push('click changed nothing');
  return { bad, note, got };
}

async function testDrag(key, p) {
  const dirs = [[48, 0], [0, 40], [-48, 0], [0, -40]];
  let res = null;
  for (const [dx, dy] of dirs) {
    await moveTo(p.x, p.y);
    const s0 = await call('watch', key);
    await mouse('mousePressed', p.x, p.y, true); await frame();
    for (let k = 1; k <= 6; k++) { await mouse('mouseMoved', p.x + dx * k / 6, p.y + dy * k / 6, true); await frame(); }
    const mid = await call('state', key), mMid = await call('changes');
    await mouse('mouseReleased', p.x + dx, p.y + dy); await frame();
    const m = await call('changes');
    const fx = mid.gone ? null : mid.cx - s0.cx, fy = mid.gone ? null : mid.cy - s0.cy;
    res = { dx, dy, fx, fy, mMid, m };
    if (m) break;
    p = await call('point', key);
    if (!p || p.x == null) break;
  }
  const bad = [], note = [];
  const along = res.dx ? res.fx : res.fy, want = res.dx || res.dy;
  let got = `pointer (${res.dx},${res.dy}) → mark (${res.fx == null ? '?' : Math.round(res.fx)},${res.fy == null ? '?' : Math.round(res.fy)}), ${res.mMid} changes mid-drag, ${res.m} total`;
  if (!res.m) bad.push('drag changed nothing in any direction');
  else if (!res.mMid) bad.push('nothing moved until release');
  if (along != null && Math.abs(along) > 2 && Math.abs(along - want) > Math.max(6, 0.35 * Math.abs(want))) note.push('mark does not track the pointer 1:1');
  await moveTo(...NEUTRAL);
  return { bad, note, got };
}

async function testRange(key) {
  const r = await call('range', key), bad = [], note = [];
  if (!r.ok) return { bad: [`thumb covered by ${r.blocker}`], note, got: 'blocked' };
  const s0 = await call('watch', key);
  const to = (r.v - r.lo) / ((r.hi - r.lo) || 1) < 0.5 ? r.xlo + 0.75 * (r.xhi - r.xlo) : r.xlo + 0.25 * (r.xhi - r.xlo);
  await mouse('mouseMoved', r.x, r.y); await mouse('mousePressed', r.x, r.y, true);
  for (let k = 1; k <= 6; k++) { await mouse('mouseMoved', r.x + (to - r.x) * k / 6, r.y, true); await frame(); }
  await mouse('mouseReleased', to, r.y); await frame();
  const m = await call('changes'), s1 = await call('state', key);
  const lo = r.lo + 0.25 * (r.hi - r.lo), hi = r.lo + 0.75 * (r.hi - r.lo), want = to > r.x ? hi : lo;
  const tol = Math.max(+r.step || 1, (r.hi - r.lo) / 12);
  let got = `${r.v} → ${s1.value} (aimed ~${+want.toFixed(2)}), ${m} changes`;
  if (r.hi === r.lo) { note.push('single value'); return { bad, note, got }; }
  if (+s1.value === r.v) bad.push('value did not move');
  else if (Math.abs(+s1.value - want) > tol) bad.push('value does not follow the thumb');
  if (+s1.value !== r.v && m <= 1) bad.push('nothing redrew');
  const before = errs.length, n = await call('sweep', key, 150);
  await frame();
  got += `; swept ${n} values`;
  if (errs.length > before) bad.push('a value throws');
  return { bad, note, got };
}

async function testCheck(key, p) {
  const s0 = await call('watch', key);
  await clickAt(p.x, p.y);
  const m1 = await call('changes'), s1 = await call('state', key);
  await clickAt(p.x, p.y);
  const m2 = (await call('changes')) - m1, s2 = await call('state', key), bad = [];
  if (s1.checked === s0.checked) bad.push('click did not toggle');
  if (s2.checked !== s0.checked) bad.push('second click did not toggle back');
  if (!m1 || !m2) bad.push(`drawing did not update (${m1}, ${m2} changes)`);
  return { bad, note: [], got: `${s0.checked} → ${s1.checked} → ${s2.checked}; ${m1} / ${m2} changes` };
}

async function testText(key, p) {
  await call('watch', key);
  await clickAt(p.x, p.y);
  await cdp.send('Input.insertText', { text: 'port' }); await frame();
  const m1 = await call('changes');
  for (let k = 0; k < 4; k++) await press('Backspace', 'Backspace', 8);
  await frame();
  const m2 = (await call('changes')) - m1, s = await call('state', key), bad = [];
  if (!m1) bad.push('typing changed nothing');
  if (!m2) bad.push('clearing changed nothing');
  if (s.value) bad.push(`box not cleared: "${s.value}"`);
  return { bad, note: [], got: `${m1} changes typing, ${m2} clearing` };
}

const rows = [];
let shots = 0;
async function one(run, sec, it, kind) {
  const before = errs.length;
  let p = null, out;
  try {
    if (kind !== 'range') {
      p = await call('point', it.key);
      if (p.gone) return;                                   // removed by an earlier action; its successor is tested
      if (p.x == null) {
        const why = p.livecover && !p.hits && !Object.keys(p.blockers).length ? `covered entirely by other interactive marks`
          : p.mode === 'offscreen' || !(p.n + p.livecover) ? `no visible hit area (${p.mode}, ${it.size.join('×')} px)` : `covered: ${blockerText(p)}`;
        out = { bad: [`unreachable: ${why}`], note: [], got: 'no point reaches it' };
      }
    }
    if (!out) out = await ({ hover: testHover, click: testClick, drag: testDrag, range: testRange, check: testCheck, text: testText })[kind](it.key, p);
  } catch (e) {
    out = { bad: [`harness: ${e.message}`], note: [], got: 'error' };
  }
  const ex = errSince(before);
  if (ex.length) out.bad.push(`exception: ${ex[0].slice(0, 160)}`);
  if (p && p.n && p.hits / p.n < 0.6 && kind !== 'range') out.note.push(`only ${pct(p.hits, p.n)}% of it is reachable; rest under ${blockerText(p)}`);
  if (p && p.edges && p.edges.n && p.edges.hit < p.edges.n) out.note.push(`edges ${p.edges.hit}/${p.edges.n} reach it (${[...new Set(p.edges.blockers)].join('; ')})`);
  const row = { run, sec, fig: it.fig, el: it.desc, key: it.key, action: kind, expected: EXPECT[kind], actual: out.got,
    pass: !out.bad.length, why: out.bad.join('; '), notes: out.note.join('; ') };
  if (!row.pass) {
    if (p && p.x != null) await call('mark', p.x, p.y);
    const png = await cdp.send('Page.captureScreenshot', { format: 'png' });
    await call('unmark');
    row.shot = `fail-${++shots}.png`;
    writeFileSync(join(OUT, row.shot), Buffer.from(png.data, 'base64'));
  }
  rows.push(row);
  process.stdout.write(row.pass ? '.' : 'F');
}

// Per section: every hover first (they leave state alone), then clicks, drags and controls in
// document order. Clicks redraw, so the section is inventoried again afterwards and anything new
// (another chart's marks, say) is tested too.
async function audit(run, sections) {
  for (const sec of sections) {
    const t0 = Date.now();
    await call('scrollTo', sec); await frame();
    const seen = new Set();
    for (let pass = 0; pass < 3; pass++) {
      const items = (await call('discover', sec)).filter((it) => !seen.has(it.key + '#' + it.sig));
      if (!items.length) break;
      items.forEach((it) => seen.add(it.key + '#' + it.sig));
      for (const it of items) if (it.kinds.includes('hover')) await one(run, sec, it, 'hover');
      for (const it of items) for (const kind of it.kinds) if (kind !== 'hover') await one(run, sec, it, kind);
    }
    await moveTo(...NEUTRAL);
    process.stdout.write(` ${sec} ${((Date.now() - t0) / 1000).toFixed(1)}s\n`);
  }
}

// DevTools' own listener list against the instrumented one: the inventory misses nothing.
async function inventoryCheck() {
  const { result } = await cdp.send('Runtime.evaluate', { expression: 'document' });
  const { listeners } = await cdp.send('DOMDebugger.getEventListeners', { objectId: result.objectId, depth: -1, pierce: true });
  const pairs = new Set(listeners.filter((l) => l.backendNodeId).map((l) => `${l.backendNodeId}:${l.type}`));
  const mine = await ev(`(() => { let n = 0; [document, ...document.querySelectorAll('*')].forEach((e) => { if (e.__hxl) n += e.__hxl.size; }); return n; })()`);
  return { devtools: pairs.size, instrumented: mine, match: pairs.size === mine };
}

function report(meta) {
  const esc = (s) => String(s ?? '').replace(/\|/g, '\\|').replace(/\n/g, ' ');
  const runs = [...new Set(rows.map((r) => r.run))], secs = [...new Set(rows.map((r) => r.sec))];
  const L = ['# Explainer interaction audit', '', `page: \`${PAGE}\`  `, `viewport ${VIEW.w}×${VIEW.h}, Chrome headless, real CDP mouse and keyboard input`, ''];
  L.push(`inventory: DevTools lists ${meta.inventory.devtools} (node, type) listener pairs, the instrumentation ${meta.inventory.instrumented}${meta.inventory.match ? ' (match)' : ' (MISMATCH)'}`, '');
  if (meta.toggle) L.push(`theme toggle: ${meta.toggle.theme}; figures per placeholder ${meta.toggle.sameFigs ? 'unchanged' : 'CHANGED'}; listeners per section ${meta.toggle.sameListeners ? 'unchanged' : 'CHANGED ' + JSON.stringify(meta.toggle.diff)}; ${meta.toggle.errors.length} exceptions`, '');
  if (meta.initErrors.length || meta.failNotes.length) L.push(`init: ${meta.initErrors.length} exceptions, ${meta.failNotes.length} failed figures: ${esc([...meta.initErrors, ...meta.failNotes].join('; '))}`, '');
  L.push('| section | ' + runs.map((r) => `${r}: tested | pass | fail`).join(' | ') + ' |', '|---|' + runs.map(() => '---:|---:|---:').join('|') + '|');
  for (const s of secs) L.push(`| ${s} | ` + runs.map((r) => { const x = rows.filter((w) => w.run === r && w.sec === s); return `${x.length} | ${x.filter((w) => w.pass).length} | ${x.filter((w) => !w.pass).length}`; }).join(' | ') + ' |');
  const byAct = {};
  rows.forEach((r) => { const a = (byAct[r.action] ||= { n: 0, f: 0 }); a.n++; if (!r.pass) a.f++; });
  L.push('', 'by action: ' + Object.entries(byAct).map(([a, v]) => `${a} ${v.n} (${v.f} fail)`).join(', '), '');
  const head = '| run | section | figure | element | action | expected | actual | result |';
  const sep = '|---|---|---|---|---|---|---|---|';
  const line = (r) => `| ${r.run} | ${r.sec} | ${esc(r.fig)} | ${esc(r.el)} | ${r.action} | ${esc(r.expected)} | ${esc(r.actual)} | ${r.pass ? 'pass' : `**FAIL**: ${esc(r.why)} ([shot](${r.shot}))`}${r.notes ? ` · ${esc(r.notes)}` : ''} |`;
  const fails = rows.filter((r) => !r.pass);
  L.push(`## Failures (${fails.length})`, '', head, sep, ...fails.map(line), '');
  const noted = rows.filter((r) => r.pass && r.notes);
  L.push(`## Passed with notes (${noted.length})`, '', head, sep, ...noted.map(line), '');
  L.push(`## Every interaction (${rows.length})`, '', head, sep, ...rows.map(line), '');
  writeFileSync(join(OUT, 'report.md'), L.join('\n'));
  writeFileSync(join(OUT, 'results.json'), JSON.stringify({ meta, rows }, null, 1));
  return fails.length;
}

async function main() {
  rmSync(OUT, { recursive: true, force: true });
  mkdirSync(OUT, { recursive: true });
  cdp = await launch();
  cdp.on((m) => {
    if (m.method === 'Runtime.exceptionThrown') {
      const d = m.params.exceptionDetails;
      errs.push(`${(d.exception?.description || d.text || '').split('\n').slice(0, 2).join(' ')} @${d.lineNumber}:${d.columnNumber}`);
    } else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      errs.push('console.error: ' + m.params.args.map((a) => a.value ?? a.description ?? '').join(' ').split('\n').slice(0, 2).join(' '));
    } else if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') errs.push('log: ' + m.params.entry.text);
  });
  for (const d of ['Page', 'Runtime', 'Log', 'DOM']) await cdp.send(`${d}.enable`);
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: VIEW.w, height: VIEW.h, deviceScaleFactor: 1, mobile: false });
  await cdp.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'light' }] });
  await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: `(${instrument})()` });
  const loaded = new Promise((ok) => cdp.on((m) => { if (m.method === 'Page.loadEventFired') ok(); }));
  await cdp.send('Page.navigate', { url: pathToFileURL(PAGE).href });
  await loaded;
  await ev(`(${pageLib})()`);
  const all = await call('sections');
  const sections = ONLY.length ? all.filter((s) => ONLY.includes(s)) : all;
  for (const s of all) { await call('scrollTo', s); await frame(); }  // each section inits as it nears the viewport
  await frame();
  const meta = { page: PAGE, initErrors: errs.slice(), failNotes: await call('failNotes'), inventory: await inventoryCheck() };
  const figs0 = await call('figCounts'), lis0 = await call('listenerCount');
  console.log(`inventory ${JSON.stringify(meta.inventory)}; init exceptions ${meta.initErrors.length}`);
  await audit('loaded', sections);
  if (THEME) {
    await moveTo(...NEUTRAL);
    const e0 = errs.length;
    const btn = await ev(`(() => { const r = document.getElementById('theme-btn').getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    await clickAt(...btn); await frame(); await sleep(150); await frame();
    const figs1 = await call('figCounts'), lis1 = await call('listenerCount');
    const diff = Object.fromEntries(Object.keys(lis0).filter((k) => lis0[k] !== lis1[k]).map((k) => [k, [lis0[k], lis1[k]]]));
    meta.toggle = { theme: await ev(`document.documentElement.getAttribute('data-theme')`), sameFigs: JSON.stringify(figs0) === JSON.stringify(figs1),
      sameListeners: !Object.keys(diff).length, diff, errors: errs.slice(e0), failNotes: await call('failNotes') };
    console.log(`theme toggle ${JSON.stringify(meta.toggle)}`);
    await audit('after theme toggle', sections);
  }
  const nf = report(meta) + (meta.initErrors.length ? 1 : 0) + (meta.toggle && (!meta.toggle.sameFigs || !meta.toggle.sameListeners || meta.toggle.errors.length || meta.toggle.theme !== 'dark') ? 1 : 0);
  console.log(`${rows.length} interactions, ${rows.filter((r) => !r.pass).length} failures; report ${join(OUT, 'report.md')}`);
  cleanup();
  process.exit(nf ? 1 : 0);
}

// Shards the sections round-robin over child runs, each with its own Chrome, and merges their results.
async function sharded() {
  const ids = [...readFileSync(PAGE, 'utf8').matchAll(/<section class="sx" id="([^"]+)"/g)].map((m) => m[1]).filter((s) => !ONLY.length || ONLY.includes(s));
  rmSync(OUT, { recursive: true, force: true });
  mkdirSync(OUT, { recursive: true });
  const shards = Array.from({ length: Math.min(JOBS, ids.length) }, (_, k) => ids.filter((_, i) => i % JOBS === k));
  const pass = args.filter((a, i) => !['--jobs', '--out', '--only'].includes(a) && !['--jobs', '--out', '--only'].includes(args[i - 1]));
  const codes = await Promise.all(shards.map((sh, k) => new Promise((ok) => {
    const c = spawn(process.execPath, [fileURLToPath(import.meta.url), ...pass, '--only', sh.join(','), '--out', join(OUT, `shard-${k}`)], { stdio: ['ignore', 'pipe', 'inherit'] });
    let buf = '';
    c.stdout.on('data', (d) => { buf += d; const ls = buf.split('\n'); buf = ls.pop(); ls.forEach((l) => console.log(`[${k}] ${l}`)); });
    c.on('exit', (code) => ok(code));
  })));
  const parts = shards.map((_, k) => JSON.parse(readFileSync(join(OUT, `shard-${k}`, 'results.json'), 'utf8')));
  parts.forEach((p, k) => p.rows.forEach((r) => { if (r.shot) r.shot = `shard-${k}/${r.shot}`; }));
  const order = (r) => ids.indexOf(r.sec) + (r.run === 'loaded' ? 0 : 1000);
  parts.flatMap((p) => p.rows).map((r, i) => [r, i]).sort((a, b) => order(a[0]) - order(b[0]) || a[1] - b[1]).forEach(([r]) => rows.push(r));
  const m = parts.map((p) => p.meta), meta = { page: PAGE, initErrors: m.flatMap((x) => x.initErrors), failNotes: m.flatMap((x) => x.failNotes), inventory: m[0].inventory };
  if (m[0].toggle) meta.toggle = { theme: m.every((x) => x.toggle.theme === 'dark') ? 'dark' : 'NOT dark', sameFigs: m.every((x) => x.toggle.sameFigs),
    sameListeners: m.every((x) => x.toggle.sameListeners), diff: Object.assign({}, ...m.map((x) => x.toggle.diff)), errors: m.flatMap((x) => x.toggle.errors), failNotes: m.flatMap((x) => x.toggle.failNotes) };
  const nf = report(meta);
  console.log(`${rows.length} interactions, ${nf} failures over ${shards.length} browsers; report ${join(OUT, 'report.md')}`);
  process.exit(codes.some(Boolean) ? 1 : 0);
}

(JOBS > 1 ? sharded() : main()).catch((e) => { console.error(e); cleanup(); process.exit(2); });
