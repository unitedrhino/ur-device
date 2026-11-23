// 文件说明：使用原生产paint的可控rAF/时钟回归；只验证旁证，不以虚拟绘制替代浏览器验收。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const {installFileStartupObservation} = require('./file_startup_observation.cjs');

// fixture 保留启用时1500ms计时与两rAF成功条件，关闭独立核零布局，观察代码不能自己推进帧。
function fixture() {
  let now = 100, sequence = 0;
  const frames = new Map(), timers = new Map(), cancelled = [], budgets = [];
  const scope = {performance: {now: () => now}, document: {visibilityState: 'visible'},
    setTimeout(fn, budget) { budgets.push(budget); timers.set(++sequence, fn); return sequence; },
    clearTimeout(id) { timers.delete(id); },
    requestAnimationFrame(fn) { frames.set(++sequence, fn); return sequence; },
    cancelAnimationFrame(id) { cancelled.push(id); frames.delete(id); },
    getComputedStyle: mount => ({display: mount.display})};
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/file-lane.js'), 'utf8'), scope);
  installFileStartupObservation(scope);
  const lane = new scope.UrFileLane({state: 'running'}, {display: 'block', isConnected: true,
    getClientRects() { return this.display === 'none' ? [] : [{}]; }}, () => {}, 'video');
  return {scope, lane, frames, timers, cancelled, budgets,
    clock: value => { now = value; },
    frame() { const [id, fn] = frames.entries().next().value; frames.delete(id); fn.call(scope, now); },
    expire() { const [id, fn] = timers.entries().next().value; timers.delete(id); fn(); }};
}
test('two actual frames retain original success and precise visible timing', async () => {
  const f = fixture(), original = f.lane.paint(true);
  assert.deepEqual(f.budgets, [1500]);
  assert.equal(f.scope.fileStartupObservation.some(row => row.phase === 'paintSettled'), false);
  f.clock(120); f.frame(); f.clock(145); f.frame(); await original;
  const result = f.scope.fileStartupObservation.at(-1);
  assert.equal(result.phase, 'paintSettled'); assert.equal(result.success, true);
  assert.equal(result.actualFrames, 2); assert.equal(result.elapsedMs, 45);
  assert.equal(result.computedDisplay, 'block'); assert.equal(f.timers.size, 0);
});
test('hidden page with no actual frame remains original timeout, not inferred paint success', async () => {
  const f = fixture(); f.scope.document.visibilityState = 'hidden';
  const actual = f.lane.paint(true);
  f.clock(1600); f.expire(); await assert.rejects(actual, /unknown/);
  const row = f.scope.fileStartupObservation.at(-1);
  assert.equal(row.actualFrames, 0); assert.equal(row.elapsedMs, 1500);
  assert.equal(row.visibility, 'hidden'); assert.equal(row.paintCancelled, false);
  assert.equal(row.success, false); assert.equal(f.frames.size, 0);
});
test('real two-frame style mismatch stays distinguishable from timeout', async () => {
  const f = fixture(); f.lane.mount.display = 'none';
  const actual = f.lane.paint(true); f.frame(); f.frame();
  await assert.rejects(actual, /unknown/);
  const row = f.scope.fileStartupObservation.at(-1);
  assert.equal(row.actualFrames, 2); assert.equal(row.elapsedMs, 0);
  assert.equal(row.computedDisplay, 'none'); assert.equal(row.paintCancelled, false);
});
test('original scope release cancellation is reported separately from a missing rAF', async () => {
  const f = fixture(), actual = f.lane.paint(true);
  f.lane.closing = true; f.lane.cancelPaint(); await assert.rejects(actual, /unknown/);
  const row = f.scope.fileStartupObservation.at(-1);
  assert.equal(row.paintCancelled, true); assert.equal(row.closing, true);
  assert.equal(f.frames.size, 0); assert.equal(f.timers.size, 0);
});
test('unrelated page animations retain original callback and handle', () => {
  const f = fixture(), callback = () => 1, id = f.scope.requestAnimationFrame(callback);
  assert.equal(f.frames.get(id), callback);
  f.scope.cancelAnimationFrame(id); assert.deepEqual(f.cancelled, [id]);
  assert.deepEqual(f.scope.fileStartupObservation, []);
});
test('capacity and private DOM strings never replace original timer or enter records', async () => {
  const f = fixture(); f.lane.mount.display = 'secret-style';
  f.scope.document.visibilityState = 'secret-visibility';
  for (let i = 0; i < 20; ++i) {
    const actual = f.lane.paint(true); f.frame(); f.frame(); await assert.rejects(actual, /unknown/);
  }
  assert.equal(f.scope.fileStartupObservation.length, 96);
  assert.equal(f.scope.fileStartupObservation[0].phase, 'paintBegin');
  assert.equal(JSON.stringify(f.scope.fileStartupObservation).includes('secret-'), false);
  assert.ok(f.budgets.every(value => value === 1500));
  assert.equal(installFileStartupObservation(f.scope), false);
});
test('original resolved false is not upgraded into successful initialization', async () => {
  const f = fixture(); f.lane.closing = true;
  const result = f.lane.initialize();
  assert.equal(await result, false);
  const row = f.scope.fileStartupObservation.at(-1);
  assert.equal(row.phase, 'initializeSettled'); assert.equal(row.success, false);
});
test('关闭零布局旁证如实记录零呈现帧，不增加timer或模拟rAF', async () => {
  const f = fixture(); f.lane.mount.display = 'none';
  await f.lane.paint(false);
  const row = f.scope.fileStartupObservation.at(-1);
  assert.equal(row.phase, 'paintSettled'); assert.equal(row.success, true);
  assert.equal(row.actualFrames, 0); assert.equal(row.computedDisplay, 'none');
  assert.equal(f.frames.size, 0); assert.deepEqual(f.budgets, []);
});
