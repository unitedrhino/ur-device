// 文件说明：启动只读旁证合同测试；只核原返回/拒绝/消息和有界脱敏，不以模拟结果替代真实门控。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const {installStartupObservation} = require('./voice_focus_startup_observation.cjs');

// fixture 提供原Promise和消息端口可控事实；旁证不得换引用、吞拒绝或再执行控制。
function fixture() {
  let clock = 10, host;
  const calls = [];
  const result = Promise.resolve('original-result');
  class Messages {
    constructor(port) { this.port = port; port.onmessage = event => calls.push(event); }
    post(value) { calls.push(value); return 'post-return'; }
    expect(...args) { this.post(args[0]); return result; }
  }
  class Host {
    constructor() { this.lanes = new Map(); this.context = {state: 'running'};
      this.life = {signal: {aborted: false}}; }
    makeAudio(lane) { lane.node = {port: {}}; lane.audio = new Messages(lane.node.port); return 42; }
    gate(value) { calls.push(value); return true; }
    presented(signal) { calls.push(signal); return result; }
  }
  const scope = {UrLiveMessages: Messages, UrLiveHost: Host,
    performance: {now: () => clock}, document: {visibilityState: 'visible'}};
  installStartupObservation(scope, () => host);
  host = new Host();
  const lane = {name: 'audio'}; host.lanes.set('audio', lane);
  assert.equal(host.makeAudio(lane), 42);
  return {scope, host, lane, calls, result, clock: value => { clock = value; }};
}
test('original promise, command, receiver event and gate return remain identical', async () => {
  const f = fixture(), value = {kind: 'liveAudioMask', ticket: '1', enabled: false};
  assert.equal(f.lane.audio.expect(value), f.result);
  assert.equal(f.host.presented(f.host.life.signal), f.result);
  assert.equal(f.host.gate({lane: 'audio'}), true);
  const original = {data: {kind: 'liveAudioMasked', ticket: '1', enabled: false}};
  f.lane.node.port.onmessage(original);
  assert.equal(f.calls.at(-1), original);
  assert.equal(f.calls[0], value);
  await f.result;
  assert.ok(f.scope.focusStartupObservation.some(row => row.phase === 'maskActual'));
  assert.ok(f.scope.focusStartupObservation.some(row => row.phase === 'presentationSettled'));
});
test('unknown messages and failure text never reach diagnostics', async () => {
  const f = fixture();
  f.lane.node.port.onmessage({data: {kind: 'private', secret: 'must-not-output'}});
  assert.equal(JSON.stringify(f.scope.focusStartupObservation).includes('must-not-output'), false);
  const g = fixture(), original = Promise.reject(new Error('private-secret-error'));
  // 覆盖原方法先于安装，确保测试的失败来自原Promise，而非旁证人为改异常。
  const Original = class { makeAudio() {} gate() {} presented() { return original; } };
  const scope = {...g.scope, UrLiveHost: Original, focusStartupObservation: undefined};
  installStartupObservation(scope, () => ({context: {state: 'running'}}));
  const actual = new Original().presented();
  assert.equal(actual, original);
  await assert.rejects(actual, /private-secret-error/);
  assert.equal(scope.focusStartupObservation.at(-1).failure, 'other');
  assert.equal(JSON.stringify(scope.focusStartupObservation).includes('private-secret-error'), false);
});
test('bounded original observations never evict early failure evidence', () => {
  const f = fixture();
  for (let i = 0; i < 150; ++i) f.host.gate({lane: 'unknown-secret'});
  assert.equal(f.scope.focusStartupObservation.length, 96);
  assert.equal(f.scope.focusStartupObservation[0].phase, 'nodeCreated');
  assert.equal(JSON.stringify(f.scope.focusStartupObservation).includes('unknown-secret'), false);
});
test('monotonic elapsed, visibility and cancellation are fixed projections only', () => {
  const f = fixture(); f.clock(1510); f.scope.document.visibilityState = 'hidden';
  f.host.life.signal.aborted = true; f.host.gate({lane: 'video'});
  assert.deepEqual(f.scope.focusStartupObservation.at(-1), {phase: 'gateAccepted', lane: 'video',
    success: true, elapsedMs: 1500, visibility: 'hidden', context: 'running', aborted: true, failure: null,
    hasFocus: false, actualFrames: null, rafCancelled: false, rafTimestampMs: null, executionElapsedMs: null});
  f.scope.document.visibilityState = 'secret-visibility'; f.clock(1); f.host.gate({});
  assert.equal(f.scope.focusStartupObservation.at(-1).visibility, 'unknown');
  assert.equal(f.scope.focusStartupObservation.at(-1).elapsedMs, null);
});

// presentationFixture 运行原生产presented和1500ms等待；只手交浏览器回调，不生成媒体或提前结算。
function presentationFixture() {
  let now = 100, sequence = 0, host, originalResult;
  const frames = new Map(), timers = new Map(), cancelled = [], budgets = [];
  const scope = {AbortController, performance: {now: () => now},
    document: {visibilityState: 'visible', hasFocus() { return true; }},
    setTimeout(fn, budget) { budgets.push(budget); timers.set(++sequence, fn); return sequence; },
    clearTimeout(id) { timers.delete(id); },
    requestAnimationFrame(fn) { assert.equal(this, scope); frames.set(++sequence, fn); return sequence; },
    cancelAnimationFrame(id) { assert.equal(this, scope); cancelled.push(id); frames.delete(id); }};
  for (const name of ['live-player.js', 'live-host.js']) vm.runInNewContext(fs.readFileSync(
    path.join(__dirname, '../adapters/desktop/web', name), 'utf8'), scope);
  const original = scope.UrLiveHost.prototype.presented;
  scope.UrLiveHost.prototype.presented = function(...args) {
    originalResult = original.apply(this, args); return originalResult;
  };
  installStartupObservation(scope, () => host);
  host = new scope.UrLiveHost(() => {}, {}, scope); host.context = {state: 'running'};
  return {scope, host, frames, timers, cancelled, budgets, original: () => originalResult,
    clock(value) { now = value; },
    frame(timestamp = now) { const [id, fn] = frames.entries().next().value; frames.delete(id); fn.call(scope, timestamp); },
    expire() { const [id, fn] = timers.entries().next().value; timers.delete(id); fn(); }};
}
test('原两次rAF保持Promise和1500ms预算，分别记录首帧与第二帧', async () => {
  const f = presentationFixture(), actual = f.host.presented(f.host.life.signal);
  assert.equal(actual, f.original()); assert.deepEqual(f.budgets, [1500]);
  assert.equal(f.scope.focusStartupObservation.at(-1).phase, 'rafScheduled');
  f.clock(120); f.frame();
  assert.equal(f.scope.focusStartupObservation.filter(row => row.phase === 'rafActual')[0].actualFrames, 1);
  assert.equal(f.scope.focusStartupObservation.some(row => row.phase === 'presentationSettled'), false);
  f.clock(140); f.frame(); await actual;
  const row = f.scope.focusStartupObservation.at(-1);
  assert.equal(row.phase, 'presentationSettled'); assert.equal(row.actualFrames, 2);
  assert.equal(row.success, true); assert.equal(row.hasFocus, true); assert.equal(row.elapsedMs, 40);
  assert.equal(f.frames.size, 0); assert.equal(f.timers.size, 0);
});
test('原首帧或第二帧缺失保留超时拒绝并取消同一原句柄', async () => {
  for (const firstFrame of [false, true]) {
    const f = presentationFixture(), actual = f.host.presented(f.host.life.signal);
    if (firstFrame) { f.clock(120); f.frame(); }
    const originalHandle = f.frames.keys().next().value;
    f.clock(1600); f.expire(); await assert.rejects(actual, /live_actual_timeout/);
    const row = f.scope.focusStartupObservation.at(-1);
    assert.equal(row.actualFrames, firstFrame ? 1 : 0); assert.equal(row.success, false);
    assert.equal(row.rafCancelled, true); assert.equal(row.failure, 'live_actual_timeout');
    assert.deepEqual(f.cancelled, [originalHandle]); assert.equal(f.frames.size, 0);
    assert.equal(f.scope.focusStartupObservation.filter(row => row.phase === 'rafCancelled').length, 1);
  }
});
test('原close信号仍取消在途帧且不伪报第二帧或呈现成功', async () => {
  const f = presentationFixture(), actual = f.host.presented(f.host.life.signal);
  f.frame(); const originalHandle = f.frames.keys().next().value;
  f.host.closing = true; f.host.life.abort(); await assert.rejects(actual, /live_cancelled/);
  const row = f.scope.focusStartupObservation.at(-1);
  assert.equal(row.failure, 'live_cancelled'); assert.equal(row.aborted, true);
  assert.equal(row.actualFrames, 1); assert.equal(row.rafCancelled, true);
  // 原presented在abort回调和finally各取消一次同句柄；旁证不能去重或替换原调用。
  assert.deepEqual(f.cancelled, [originalHandle, originalHandle]); assert.equal(f.timers.size, 0);
});
test('无关动画保持原callback接收者、参数、返回与取消句柄，不进入媒体旁证', () => {
  const f = presentationFixture(), receiver = {}, args = [120, {private: 'not-observed'}];
  const callback = function(...values) { assert.equal(this, receiver); assert.deepEqual(values, args); return 42; };
  const id = f.scope.requestAnimationFrame(callback);
  assert.equal(f.frames.get(id), callback); assert.equal(f.frames.get(id).apply(receiver, args), 42);
  f.scope.cancelAnimationFrame(id); assert.deepEqual(f.cancelled, [id]);
  assert.deepEqual(f.scope.focusStartupObservation, []);
});
test('关联原rAF也保持callback接收者、参数、返回值以及原取消返回', () => {
  let callback, commandReceiver;
  const receiver = {}, args = [120, {private: 'must-not-output'}];
  const originalCallback = function(...values) {
    assert.equal(this, receiver); assert.deepEqual(values, args); return 42;
  };
  class Host {
    makeAudio() {} gate() {}
    presented() { assert.equal(this, commandReceiver); return scope.requestAnimationFrame(originalCallback); }
  }
  class Messages { post() {} expect() {} }
  const scope = {UrLiveHost: Host, UrLiveMessages: Messages, performance: {now: () => 1},
    document: {hasFocus: () => true},
    requestAnimationFrame(value) { assert.equal(this, scope); callback = value; return 7; },
    cancelAnimationFrame(id) { assert.equal(this, scope); assert.equal(id, 7); return 'cancel-return'; }};
  installStartupObservation(scope); commandReceiver = new Host();
  assert.equal(commandReceiver.presented(), 7);
  assert.equal(callback.apply(receiver, args), 42);
  assert.equal(scope.cancelAnimationFrame(7), 'cancel-return');
  assert.equal(JSON.stringify(scope.focusStartupObservation).includes('must-not-output'), false);
});
test('focus读取失败及容量满都不打断原帧、Promise或取消', async () => {
  const f = presentationFixture(); f.scope.document.hasFocus = () => { throw Error('private-focus'); };
  let actual = f.host.presented(f.host.life.signal); f.frame(); f.frame(); await actual;
  f.scope.document.hasFocus = () => true;
  for (let index = 0; index < 30; index++) {
    actual = f.host.presented(f.host.life.signal); f.frame(); f.frame(); await actual;
  }
  assert.equal(f.scope.focusStartupObservation.length, 96);
  assert.equal(JSON.stringify(f.scope.focusStartupObservation).includes('private-focus'), false);
  actual = f.host.presented(f.host.life.signal); const id = f.frames.keys().next().value;
  f.expire(); await assert.rejects(actual, /live_actual_timeout/);
  assert.equal(f.cancelled.at(-1), id); assert.equal(f.frames.size, 0); assert.equal(f.timers.size, 0);
  assert.ok(f.budgets.every(value => value === 1500));
});
test('reinstall never duplicates wrappers or original side effects', () => {
  const f = fixture(); assert.equal(installStartupObservation(f.scope), false);
  f.host.gate({lane: 'audio'}); assert.equal(f.calls.length, 1);
});
test('fixed rejection category and original thrown object stay independent', () => {
  const failure = new Error('live_port_closed'), original = {kind: 'liveAudioMask', ticket: '1'};
  class Messages { post() { throw failure; } expect() {} }
  class Host { makeAudio() {} presented() {} gate() { return false; } }
  const scope = {UrLiveHost: Host, UrLiveMessages: Messages,
    performance: {now: () => 1}, document: {visibilityState: 'visible'}};
  installStartupObservation(scope);
  assert.throws(() => new Messages().post(original), error => error === failure);
  assert.equal(scope.focusStartupObservation.at(-1).failure, 'live_port_closed');
  assert.equal(new Host().gate({lane: 'audio'}), false);
  assert.equal(scope.focusStartupObservation.at(-1).success, false);
});
test('serialized browser installer has no undeclared module dependencies', () => {
  const f = fixture();
  // 使用独立prototype避免重新包裹原fixture；页面收到的是函数源码，不依赖Node require。
  class Host { makeAudio() {} presented() { return f.result; } gate() { return true; } }
  class Messages { post() {} expect() { return f.result; } }
  const scope = {...f.scope, UrLiveHost: Host, UrLiveMessages: Messages,
    focusStartupObservation: undefined};
  const install = new Function('return (' + installStartupObservation.toString() + ');')();
  assert.equal(install(scope), true);
  assert.equal(new Host().gate({lane: 'audioVideo'}), true);
  assert.equal(scope.focusStartupObservation.at(-1).lane, 'audioVideo');
});

// timingFixture 只模拟原PerformanceObserver接收者和队列，不创建长任务、计时器或真实浏览器。
function timingFixture(options = {}) {
  let observer, callback, reads = 0, disconnects = 0, observes = 0, constructions = 0;
  let records = [];
  const order = []; // 只核原observe/takeRecords/disconnect调用顺序，不记录自由API正文。
  class Host { makeAudio() {} presented() { return options.result ?? 'original'; } gate() { return true; } }
  class Messages { post() {} expect() {} }
  class Observer {
    static get supportedEntryTypes() {
      if (options.supportedError) throw Error('private-supported');
      return options.unsupported ? ['mark'] : ['longtask'];
    }
    constructor(value) {
      constructions++; if (options.constructError) throw Error('private-constructor');
      callback = value; observer = this;
    }
    observe(value) {
      assert.equal(this, observer); observes++;
      order.push('observe');
      assert.deepEqual(value, {type: 'longtask', buffered: true});
      if (options.observeError) throw Error('private-observe');
    }
    takeRecords() {
      assert.equal(this, observer); reads++;
      order.push('takeRecords');
      if (options.takeError) throw Error('private-take');
      const result = records; records = []; return result;
    }
    disconnect() {
      assert.equal(this, observer); disconnects++;
      order.push('disconnect');
      if (options.disconnectError) throw Error('private-disconnect');
    }
  }
  const scope = {UrLiveHost: Host, UrLiveMessages: Messages, performance: {now: () => 100},
    document: {hasFocus: () => true}, PerformanceObserver: options.missing ? undefined : Observer};
  installStartupObservation(scope);
  return {scope, order, setRecords(value) { records = value; },
    counts: () => ({reads, disconnects, observes, constructions}),
    emit(entries) {
      const list = {getEntries() { assert.equal(this, list); return entries; }};
      callback.call(observer, list, observer);
    },
    callbackError() { callback.call(observer, {getEntries() { throw Error('private-callback'); }}); }};
}
// longEntry 的自由name/attribution仅用于验证不投影；原事实只取类型与两项数值。
function longEntry(startTime = 125, duration = 60) {
  return {entryType: 'longtask', startTime, duration, name: 'private-name',
    attribution: [{containerSrc: 'private-url'}]};
}
test('LongTask一次注册同origin，固定relative数值且原API receiver不变', () => {
  const f = timingFixture(); f.emit([longEntry(125.5, 60.25)]);
  assert.deepEqual(f.scope.focusStartupTimingFacts, {supported: true,
    entries: [{startOffsetMs: 25.5, durationMs: 60.25}], truncated: false});
  assert.deepEqual(f.counts(), {reads: 0, disconnects: 0, observes: 1, constructions: 1});
  assert.equal(installStartupObservation(f.scope), false); assert.equal(f.counts().observes, 1);
  assert.equal(new f.scope.UrLiveHost().presented(), 'original');
  f.scope.stopFocusStartupTimingObservation();
  assert.equal(JSON.stringify(f.scope.focusStartupTimingFacts).includes('private-'), false);
});
test('LongTask忽略旧origin/未知类型及非有限负数，不读取自由字段', () => {
  const f = timingFixture(), protectedEntry = longEntry(100, 0);
  for (const field of ['name', 'attribution']) Object.defineProperty(protectedEntry, field,
    {get() { throw Error('private-field'); }});
  f.emit([longEntry(99, 60), {...longEntry(), entryType: 'mark'}, longEntry(Infinity, 60),
    longEntry(NaN, 60), longEntry(110, -1), longEntry(110, Infinity), longEntry('110', 60),
    longEntry(110, '60'), protectedEntry]);
  assert.deepEqual(f.scope.focusStartupTimingFacts.entries, [{startOffsetMs: 0, durationMs: 0}]);
  assert.equal(f.scope.focusStartupTimingFacts.supported, true); f.scope.stopFocusStartupTimingObservation();
});
test('LongTask最多32条，溢出不淘汰旧记录且明确truncated', () => {
  const f = timingFixture(); f.emit(Array.from({length: 40}, (_, index) => longEntry(100 + index, 60)));
  assert.equal(f.scope.focusStartupTimingFacts.entries.length, 32);
  assert.deepEqual(f.scope.focusStartupTimingFacts.entries[0], {startOffsetMs: 0, durationMs: 60});
  assert.equal(f.scope.focusStartupTimingFacts.truncated, true); f.scope.stopFocusStartupTimingObservation();
});
test('显式stop先takeRecords后disconnect且幂等，停后迟到callback不能新增事实', () => {
  const f = timingFixture(); f.emit([longEntry()]); f.setRecords([longEntry(200, 70)]);
  const facts = f.scope.focusStartupTimingFacts;
  assert.equal(f.scope.stopFocusStartupTimingObservation(), facts);
  assert.equal(f.scope.stopFocusStartupTimingObservation(), facts);
  assert.deepEqual(facts.entries, [{startOffsetMs: 25, durationMs: 60}, {startOffsetMs: 100, durationMs: 70}]);
  f.emit([longEntry(300, 80)]); assert.equal(facts.entries.length, 2);
  assert.deepEqual(f.counts(), {reads: 1, disconnects: 1, observes: 1, constructions: 1});
  assert.deepEqual(f.order, ['observe', 'takeRecords', 'disconnect']);
});
test('LongTask缺API/不支持/能力getter/构造/observe失败均不影响控制或暴露异常', () => {
  for (const options of [{missing: true}, {unsupported: true}, {supportedError: true},
    {constructError: true}, {observeError: true}]) {
    const f = timingFixture(options);
    assert.deepEqual(f.scope.focusStartupTimingFacts, {supported: false, entries: [], truncated: false});
    assert.equal(new f.scope.UrLiveHost().gate({lane: 'audio'}), true);
    assert.doesNotThrow(() => f.scope.stopFocusStartupTimingObservation());
    assert.doesNotThrow(() => f.scope.stopFocusStartupTimingObservation());
    assert.equal(JSON.stringify(f.scope.focusStartupTimingFacts).includes('private-'), false);
    if (options.observeError) assert.equal(f.counts().disconnects, 1);
  }
});
test('LongTask callback及条目读取异常被隔离，未知采集不伪报可靠supported', () => {
  const f = timingFixture(); assert.doesNotThrow(() => f.callbackError());
  assert.equal(f.scope.focusStartupTimingFacts.supported, false);
  const g = timingFixture(), broken = {get entryType() { throw Error('private-entry'); }};
  assert.doesNotThrow(() => g.emit([broken])); assert.equal(g.scope.focusStartupTimingFacts.supported, false);
  f.scope.stopFocusStartupTimingObservation(); g.scope.stopFocusStartupTimingObservation();
});
test('LongTask takeRecords或disconnect异常仍一次停止，不改变原Promise结果', () => {
  for (const options of [{takeError: true}, {disconnectError: true}]) {
    const f = timingFixture(options);
    assert.doesNotThrow(() => f.scope.stopFocusStartupTimingObservation());
    assert.doesNotThrow(() => f.scope.stopFocusStartupTimingObservation());
    assert.equal(f.scope.focusStartupTimingFacts.supported, false);
    assert.equal(f.counts().reads, 1); assert.equal(f.counts().disconnects, 1);
    assert.equal(new f.scope.UrLiveHost().presented(), 'original');
  }
});
test('原rAF timestamp与执行时差独立投影，不换原帧参数或原1500ms预算', async () => {
  const f = presentationFixture(), actual = f.host.presented(f.host.life.signal);
  f.clock(130); f.frame(110.25); f.clock(145); f.frame(140.5); await actual;
  const rows = f.scope.focusStartupObservation.filter(row => row.phase === 'rafActual');
  assert.deepEqual(rows.map(row => [row.rafTimestampMs, row.executionElapsedMs]), [[10.25, 30], [40.5, 45]]);
  assert.deepEqual(f.budgets, [1500]); assert.equal(actual, f.original());
  assert.deepEqual(f.scope.focusStartupTimingFacts, {supported: false, entries: [], truncated: false});
  f.scope.stopFocusStartupTimingObservation();
});
test('LongTask读取故障或原拒绝并存时仍返回同原Promise与原异常对象', async () => {
  const error = new Error('original-private-error'), original = Promise.reject(error);
  const f = timingFixture({result: original}); f.callbackError();
  const actual = new f.scope.UrLiveHost().presented();
  assert.equal(actual, original); await assert.rejects(actual, value => value === error);
  f.scope.stopFocusStartupTimingObservation();
  assert.equal(JSON.stringify(f.scope.focusStartupTimingFacts).includes('original-private-error'), false);
});
test('LongTask批次与数值超限只作不完整或过滤，不隐式转换自由对象', () => {
  let coercions = 0; // 自由对象转换不是合法数值读取，不能靠异常降supported掩盖副作用。
  const f = timingFixture(), original = longEntry({valueOf() {
    coercions++; assert.fail('diagnostic must not coerce private objects');
  }}, 60);
  f.emit([original, longEntry(Number.MAX_SAFE_INTEGER + 1000, 60), longEntry(110, Number.MAX_SAFE_INTEGER + 1)]);
  assert.deepEqual(f.scope.focusStartupTimingFacts.entries, []);
  assert.equal(coercions, 0);
  assert.equal(f.scope.focusStartupTimingFacts.supported, true);
  f.emit(Array.from({length: 300}, () => ({entryType: 'mark'})));
  assert.equal(f.scope.focusStartupTimingFacts.truncated, true);
  f.scope.stopFocusStartupTimingObservation();
  const g = timingFixture(); assert.doesNotThrow(() => g.emit(null));
  assert.equal(g.scope.focusStartupTimingFacts.supported, false); g.scope.stopFocusStartupTimingObservation();
});
