// 文件说明：正式固定输出路径JS的受控DOM/Worklet回归；不是实际绘制、解码或硬件验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/file-lane.js'), 'utf8');

// fixture 显式模拟独立音频线程原票据和绘制调度，不在构造时自动证明空闲。
function fixture(lane = 'audioVideo', options = {}) {
  const events = [], commands = [], nodes = [], style = {};
  const context = {state: options.suspended ? 'suspended' : 'running', destination: {},
    audioWorklet: {async addModule() {}}, closed: false};
  // mount 显式模拟同一节点的连接与布局；无rAF只影响启用呈现，不能伪造关闭零布局。
  const mount = {isConnected: !options.disconnected, getClientRects: () =>
    options.badHiddenLayout || style.display !== 'none' ? [{}] : [],
    style: {setProperty(key, value, priority) {
    assert.equal(priority, 'important'); style[key] = value;
  }}};
  class Worklet {
    constructor() {
      this.disconnected = false;
      this.port = {onmessage: null, postMessage: value => {
        commands.push({...value});
        if (value.kind === 'fileAudioGate' && !options.noAudio)
          setImmediate(() => this.port.onmessage?.({data: {kind: 'fileAudioGated',
            generation: value.generation, ticket: value.ticket, enabled: value.enabled}}));
      }};
      nodes.push(this);
    }
    connect(value) { this.destination = value; }
    disconnect() { this.disconnected = true; }
  }
  const scope = {AudioWorkletNode: Worklet, performance,
    setTimeout: (fn, delay) => setTimeout(fn, (options.noAudio && delay === 1800) ||
      (options.noPaint && delay === 1500) ? 0 : delay), clearTimeout,
    requestAnimationFrame: fn => options.noPaint ? 0 : setTimeout(fn, 1),
    cancelAnimationFrame: clearTimeout, getComputedStyle: () => ({display: style.display})};
  vm.createContext(scope);
  vm.runInContext(source, scope);
  return {lane: new scope.UrFileLane(context, mount,
    value => events.push(JSON.parse(JSON.stringify(value))), lane), events, commands, nodes, context, style};
}
// gate 使用C++固定本地桥字段，不能添加云端session/媒体源或改变固定资源组。
function gate(lane, ticket = '1', enabled = false) {
  return {kind: 'fileLaneGate', lane, gateTicket: ticket, enabled};
}

test('构造未知，首次关闭实际块后才确认；音频/视频完整组独立', async () => {
  for (const name of ['audio', 'video', 'audioVideo']) {
    const f = fixture(name);
    assert.equal(f.lane.destination(), null);
    assert.equal(await f.lane.gate(gate(name, '1', true)), false);
    assert.equal(f.nodes.length, 0);
    assert.equal(await f.lane.gate(gate(name)), true);
    assert.equal(f.commands[0].enabled, false);
    assert.equal(f.style.display, 'none');
    assert.deepEqual(f.events, [{kind: 'fileLaneGated', lane: name, gateTicket: '1', enabled: false}]);
    assert.equal(await f.lane.gate(gate(name, '2', true)), true);
    assert.equal(f.commands[1].enabled, name !== 'video');
    assert.equal(f.style.display, name === 'audio' ? 'none' : 'block');
    assert.equal(await f.lane.gate(gate(name, '3')), true);
    assert.equal(f.nodes.length, 1);
    f.lane.release();
    assert.equal(f.context.closed, false);
    assert.equal(f.nodes[0].disconnected, true);
    assert.equal(f.lane.destination(), null);
  }
});

test('缺音频实际确认不能回关闭，启用缺真实绘制不能报告完整组成功', async () => {
  for (const options of [{noAudio: true}, {suspended: true}]) {
    const f = fixture('audioVideo', options);
    assert.equal(await f.lane.gate(gate('audioVideo')), false);
    assert.deepEqual(f.events, []);
    assert.equal(f.lane.destination(), null);
    f.lane.release();
  }
  const audio = fixture('audio', {noPaint: true});
  assert.equal(await audio.lane.gate(gate('audio')), true);
  audio.lane.release();
  const hidden = fixture('audioVideo', {noPaint: true});
  assert.equal(await hidden.lane.gate(gate('audioVideo')), true);
  assert.equal(await hidden.lane.gate(gate('audioVideo', '2', true)), false);
  assert.deepEqual(hidden.events, [{kind: 'fileLaneGated', lane: 'audioVideo', gateTicket: '1', enabled: false}]);
  assert.equal(hidden.lane.destination(), null);
  hidden.lane.release();
});

test('非法范围、重复/未来格式不能改换实际路径', async () => {
  const f = fixture();
  for (const value of [null, gate('audio'), gate('audioVideo', '0'), gate('audioVideo', '01'),
      gate('audioVideo', '18446744073709551616'), {...gate('audioVideo'), extra: 1},
      {...gate('audioVideo'), enabled: 1}]) assert.equal(await f.lane.gate(value), false);
  assert.equal(f.nodes.length, 0);
  assert.equal(await f.lane.gate(gate('audioVideo')), true);
  assert.equal(await f.lane.gate(gate('audioVideo')), false);
  assert.equal(f.events.length, 1);
  f.lane.release();
  assert.equal(await f.lane.gate(gate('audioVideo', '2', true)), false);
});

test('原操作在途不能覆盖，关闭取消等待而不造成功', async () => {
  const f = fixture('audioVideo', {noAudio: true});
  const original = f.lane.gate(gate('audioVideo'));
  assert.equal(await f.lane.gate(gate('audioVideo', '2')), false);
  await new Promise(resolve => setImmediate(resolve));
  f.lane.release();
  assert.equal(await original, false);
  assert.deepEqual(f.events, []);
  assert.equal(f.context.closed, false);
});

test('首次关闭隐藏视频路径以实际零布局证明，不依赖隐藏节点产生rAF', async () => {
  for (const name of ['video', 'audioVideo']) {
    const f = fixture(name, {noPaint: true});
    assert.equal(await f.lane.gate(gate(name)), true);
    assert.deepEqual(f.events, [{kind: 'fileLaneGated', lane: name, gateTicket: '1', enabled: false}]);
    assert.equal(f.commands[0].enabled, false);
    assert.equal(f.style.display, 'none');
    assert.notEqual(f.lane.destination(), null);
    f.lane.release();
  }
});

test('关闭仍须本路径实际连接且零布局，未连接或隐藏样式未隔离不能回空闲', async () => {
  for (const options of [{disconnected: true}, {badHiddenLayout: true}]) {
    const f = fixture('audioVideo', options);
    assert.equal(await f.lane.gate(gate('audioVideo')), false);
    assert.deepEqual(f.events, []);
    assert.equal(f.lane.destination(), null);
    f.lane.release();
  }
});

// paintFixture 独立控制单调时钟及两类任务队列，不把DOM调度模拟称为实际视频解码。
function paintFixture(started = 0) {
  let now = started, next = 1; // 原单调窗口与本地唯一句柄。
  const frames = new Map(), timers = new Map(), style = {display: 'block'};
  const scope = {performance: {now: () => now},
    setTimeout: (callback, delay) => { const id = next++; timers.set(id, {callback, deadline: now + delay}); return id; },
    clearTimeout: id => timers.delete(id),
    requestAnimationFrame: callback => { const id = next++; frames.set(id, callback); return id; },
    cancelAnimationFrame: id => frames.delete(id), getComputedStyle: () => ({display: style.display})};
  vm.createContext(scope);
  vm.runInContext(source, scope);
  // mount 用确定布局核关闭证明，不用可控帧队列推断节点已隔离。
  const mount = {isConnected: true, getClientRects: () => style.display === 'none' ? [] : [{}],
    style: {setProperty: (key, value) => { style[key] = value; }}};
  const lane = new scope.UrFileLane({state: 'running'}, mount, () => {}, 'video');
  return {lane, frames, timers, style, mount, at(value) { now = value; },
    frame() {
      assert.equal(frames.size, 1);
      const [id, callback] = frames.entries().next().value;
      frames.delete(id); callback(now);
    },
    timeout() {
      assert.equal(timers.size, 1);
      const [id, timer] = timers.entries().next().value;
      assert.ok(now >= timer.deadline);
      timers.delete(id); timer.callback();
    }};
}

test('显示门控两周期正常完成；单帧超时、关闭或样式错误均拒绝', async () => {
  const valid = paintFixture();
  const completed = valid.lane.paint(true);
  valid.at(16); valid.frame(); valid.at(32); valid.frame();
  await completed;
  assert.equal(valid.timers.size, 0);
  assert.equal(valid.lane.cancelPaint, null);
  for (const scenario of ['timeout', 'closed', 'style']) {
    const f = paintFixture();
    const rejected = assert.rejects(f.lane.paint(true), {message: 'unknown'});
    f.at(16); f.frame();
    if (scenario === 'timeout') { f.at(1500); f.timeout(); }
    else if (scenario === 'closed') f.lane.release();
    else { f.style.display = 'none'; f.at(32); f.frame(); }
    await rejected;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
  }
});

test('显示门控第二帧抢在滞后timer之前也不能超1500ms或越过无效时钟', async () => {
  for (const [started, first, second] of [[0, 1000, 1600], [0, 16, 1500],
      [100, 90, 100], [0, 1000, 500], [0, 16, NaN], [0, 16, Infinity], [NaN, 16, 32]]) {
    const f = paintFixture(started);
    const rejected = assert.rejects(f.lane.paint(true), {message: 'unknown'});
    f.at(first); f.frame();
    if (f.frames.size) { f.at(second); f.frame(); }
    await rejected;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
  }
});

test('已取消的显示旧回调不得登记新帧或清掉下一次等待', async () => {
  for (const second of [false, true]) {
    const f = paintFixture();
    const rejected = assert.rejects(f.lane.paint(true), {message: 'unknown'});
    if (second) { f.at(16); f.frame(); }
    const old = f.frames.values().next().value;
    f.lane.cancelPaint();
    await rejected;
    const next = f.lane.paint(true);
    const currentCancel = f.lane.cancelPaint;
    old(32); // 迟到的旧第一或第二回调，不能登记新帧或清掉后继取消器。
    assert.equal(f.frames.size, 1);
    assert.equal(f.lane.cancelPaint, currentCancel);
    f.at(48); f.frame(); f.at(64); f.frame();
    await next;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
  }
});

test('关闭零布局确认取消本路径原绘制，迟到旧回调不能成功或污染后续启用', async () => {
  for (const second of [false, true]) {
    const f = paintFixture();
    const rejected = assert.rejects(f.lane.paint(true), {message: 'unknown'});
    if (second) { f.at(16); f.frame(); }
    const old = f.frames.values().next().value;
    f.style.display = 'none';
    await f.lane.paint(false);
    await rejected;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
    assert.equal(f.lane.cancelPaint, null);
    f.style.display = 'block';
    const next = f.lane.paint(true), currentCancel = f.lane.cancelPaint;
    old(32);
    assert.equal(f.frames.size, 1);
    assert.equal(f.lane.cancelPaint, currentCancel);
    f.at(48); f.frame(); f.at(64); f.frame();
    await next;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
  }
});
