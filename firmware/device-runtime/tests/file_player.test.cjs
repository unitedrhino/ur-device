// 正式文件组件的控制/下载/清理回归：DOM和音频桥受控模拟，不代表实际浏览器解码E2E。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/file-player.js'), 'utf8');

// paintFixture 仅检验显示栅栏的调度和原等待隔离；不创建媒体或伪造实际解码帧。
function paintFixture(started = 0) {
  let now = started, next = 1; // 原单调窗口与本地唯一句柄。
  const frames = new Map(), timers = new Map();
  const scope = {performance: {now: () => now},
    setTimeout: (callback, delay) => { const id = next++; timers.set(id, {callback, deadline: now + delay}); return id; },
    clearTimeout: id => timers.delete(id),
    requestAnimationFrame: callback => { const id = next++; frames.set(id, callback); return id; },
    cancelAnimationFrame: id => frames.delete(id)};
  vm.createContext(scope);
  vm.runInContext(source, scope);
  const player = new scope.UrFilePlayer({}, {}, () => {}, 'a'.repeat(64));
  return {player, frames, timers, at(value) { now = value; },
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

test('播放器显示栅栏必须完成两周期，单帧超时和关闭清理原等待', async () => {
  const valid = paintFixture();
  const completed = valid.player.paint();
  valid.at(16); valid.frame(); valid.at(32); valid.frame();
  await completed;
  assert.equal(valid.player.waiters.size, 0);
  assert.equal(valid.timers.size, 0);
  for (const closing of [false, true]) {
    const f = paintFixture();
    const rejected = assert.rejects(f.player.paint(), {message: closing ? 'closed' : 'paintTimeout'});
    f.at(16); f.frame();
    if (closing) { f.player.closing = true; for (const cancel of f.player.waiters) cancel(); }
    else { f.at(1500); f.timeout(); }
    await rejected;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
    assert.equal(f.player.waiters.size, 0);
  }
});

test('播放器第二帧抢在滞后timer之前仍执行单调1500ms截止', async () => {
  for (const [started, first, second] of [[0, 1000, 1600], [0, 16, 1500],
      [100, 90, 100], [0, 1000, 500], [0, 16, NaN], [0, 16, Infinity], [NaN, 16, 32]]) {
    const f = paintFixture(started);
    const rejected = assert.rejects(f.player.paint(), {message: 'paintTimeout'});
    f.at(first); f.frame();
    if (f.frames.size) { f.at(second); f.frame(); }
    await rejected;
    assert.equal(f.frames.size, 0);
    assert.equal(f.timers.size, 0);
    assert.equal(f.player.waiters.size, 0);
  }
});

test('播放器已取消的旧绘制回调不能影响下一次原等待或登记新帧', async () => {
  for (const second of [false, true]) {
    const f = paintFixture();
    const rejected = assert.rejects(f.player.paint(), {message: 'closed'});
    if (second) { f.at(16); f.frame(); }
    const old = f.frames.values().next().value;
    for (const cancel of f.player.waiters) cancel();
    await rejected;
    const next = f.player.paint();
    const currentCancel = f.player.waiters.values().next().value;
    old(32); // 旧第一回调不能登记新帧；旧第二回调不能重复完成或移除后继等待。
    assert.equal(f.frames.size, 1);
    assert.equal(f.player.waiters.size, 1);
    assert.equal(f.player.waiters.values().next().value, currentCancel);
    f.at(48); f.frame(); f.at(64); f.frame();
    await next;
    assert.equal(f.player.waiters.size, 0);
    assert.equal(f.timers.size, 0);
  }
});

// fixture 每例独立借用上下文和媒体；记录资源动作，不能悄悄close宿主音频。
function fixture(options = {}) {
  const events = [], requests = [], nodes = [], media = [], urls = [];
  let cancelled = 0;
  class Element {
    constructor() {
      this.paused = true; this.ended = false; this.hidden = true; this.readyState = 0;
      this.duration = 5; this.playbackRate = 1; this._time = 0; this.seeking = false;
      this.listeners = new Map(); this.frames = new Map(); this.nextFrame = 0; this.removed = false;
    }
    get currentTime() { return this._time; }
    set currentTime(value) { this._time = value; this.seeking = !!options.longSeek; }
    load() { this.readyState = this.src ? 2 : 0; }
    async play() { this.paused = false; this._time += 0.01; }
    pause() { this.paused = true; }
    addEventListener(kind, fn) { this.listeners.set(kind, fn); }
    removeAttribute(name) { delete this[name]; }
    remove() { this.removed = true; }
    requestVideoFrameCallback(fn) {
      const id = ++this.nextFrame;
      this.frames.set(id, setTimeout(() => { this.frames.delete(id); fn(0, {}); }, 5));
      return id;
    }
    cancelVideoFrameCallback(id) { clearTimeout(this.frames.get(id)); this.frames.delete(id); }
  }
  class Worklet {
    constructor() {
      this.disconnected = false;
      this.port = {onmessage: null, postMessage: value => {
        if (!['fileAudioGate', 'fileAudioClose'].includes(value.kind)) return;
        setImmediate(() => {
          this.port.onmessage?.({data: {kind: value.kind === 'fileAudioClose' ? 'fileAudioClosed' : 'fileAudioGated', generation: value.generation,
            ticket: value.ticket, enabled: value.enabled}});
          if (value.enabled && !options.noSamples) this.port.onmessage?.({data: {
            kind: 'fileAudioMeasured', generation: value.generation, ticket: 1, samples: 256, energy: 64}});
        });
      }};
      nodes.push(this);
    }
    connect(destination) { this.destination = destination; }
    disconnect() { this.disconnected = true; }
  }
  class FileURL extends URL {
    static createObjectURL() { urls.push('created'); return 'blob:fixture'; }
    static revokeObjectURL() { urls.push('revoked'); }
  }
  const context = {state: options.suspended ? 'suspended' : 'running', destination: {},
    closed: false, audioWorklet: {async addModule() {}},
    createMediaElementSource() { const node = {connect() {}, disconnect() { this.disconnected = true; }}; nodes.push(node); return node; },
    close() { this.closed = true; }};
  const mount = {appendChild() {}};
  const scope = {URL: FileURL, Blob, Uint8Array, AbortController, performance,
    // 不绘制夹具只使原绘制截止立即触发，不用真实1.5秒等待掩盖音频路径的错误依赖。
    setTimeout: (fn, delay) => setTimeout(fn, options.blockPaint && delay === 1500 ? 0 : delay), clearTimeout,
    requestAnimationFrame: fn => options.blockPaint ? 0 : setTimeout(fn, 1),
    cancelAnimationFrame: handle => { if (!options.blockPaint) clearTimeout(handle); },
    AudioWorkletNode: Worklet, document: {createElement() { const e = new Element(); media.push(e); return e; }},
    fetch: async (url, config) => {
      requests.push({url, config});
      if (options.hungFetch) await new Promise((resolve, reject) => {
        config.signal.addEventListener('abort', () => reject(new Error('aborted')), {once: true});
      });
      let index = 0;
      const chunks = options.chunks || [new Uint8Array([1, 2, 3])];
      return {ok: true, redirected: !!options.redirected,
        headers: {get: key => key === 'Content-Type' ? (options.mime || 'audio/wav') : options.length ?? null},
        body: {getReader: () => ({async read() { return index < chunks.length ? {done: false, value: chunks[index++]} : {done: true}; },
          async cancel() { ++cancelled; }, releaseLock() {}})}};
    }};
  vm.createContext(scope);
  vm.runInContext(source, scope);
  const player = new scope.UrFilePlayer(context, mount, value => events.push(JSON.parse(JSON.stringify(value))),
    'a'.repeat(64), options.destination);
  return {player, context, events, requests, nodes, media, urls, cancelled: () => cancelled};
}
// bind 保持与C++驱动本地契约一致，不含设备身份或内容URL。
function bind(extra = {}) {
  return {kind: 'fileBind', sessionId: 'child', playbackId: 'p', mime: 'audio/wav', audio: true,
    origin: '', maxBytes: 1024, ...extra};
}
// control 参数只来自宿主固定操作，不把本地桥报文当云端MQTT示例。
function control(operation, generation, extra = {}) {
  return {kind: 'fileControl', sessionId: 'child', playbackId: 'p', generation: String(generation), operation, ...extra};
}
// load 准备实际状态回归，不隐含播放或授权整组输出。
async function load(f, mime = 'audio/wav', audio = true) {
  assert.equal(await f.player.initialize(bind({mime, audio})), true);
  const url = mime.startsWith('video/') ? '/media/file.mp4' : '/media/file.wav';
  assert.equal(await f.player.control(control('load', 1, {url, validForMs: 1000})), true);
}
// close 确保测试产生的所有定时器、节点与独占媒体清理，不遗留模拟循环。
async function close(f) {
  return f.player.close({kind: 'fileClose', sessionId: 'child', playbackId: 'p', generation: f.player.generation});
}

test('初始化不得启动宿主上下文或申请麦克风，绑定只用一次', async () => {
  const suspended = fixture({suspended: true});
  assert.equal(await suspended.player.initialize(bind()), false);
  assert.equal(suspended.media.length, 0);
  const f = fixture();
  assert.equal(await f.player.initialize(bind()), true);
  assert.equal(await f.player.initialize(bind()), false);
  assert.equal(f.events[0].kind, 'fileReady');
  assert.equal(f.media[0].paused, true);
  assert.equal(await close(f), true);
  assert.equal(f.context.closed, false);
});

test('显式固定路径输出节点不回退直通宿主扬声器', async () => {
  const destination = {};
  const f = fixture({destination});
  assert.equal(await f.player.initialize(bind()), true);
  assert.equal(f.nodes.find(node => node.port).destination, destination);
  assert.notEqual(destination, f.context.destination);
  await close(f);
});

// outputGate 只构造本地原票据，不授予模型URL或业务资源。
function outputGate(enabled, ticket = '1', extra = {}) {
  return {kind: 'fileOutputGate', sessionId: 'child', playbackId: 'p', gateTicket: ticket, enabled, ...extra};
}

test('整组门控等待音频与绘制且保留原节目，显式恢复不新建资源', async () => {
  const f = fixture({mime: 'video/mp4'});
  await load(f, 'video/mp4');
  assert.equal(await f.player.control(control('play', 2)), true);
  const media = f.player.media;
  const busy = f.player.outputGate(outputGate(false));
  assert.equal(f.player.outputBusy, true);
  assert.equal(f.events.some(v => v.kind === 'fileOutputGated'), false);
  assert.equal(await busy, true);
  assert.equal(media.hidden, true);
  assert.equal(media.paused, false);
  assert.equal(f.player.state, 'playing');
  assert.equal(await f.player.control(control('play', 3)), false);
  assert.equal(await f.player.outputGate(outputGate(true, '2')), true);
  assert.equal(f.player.media, media);
  assert.equal(media.hidden, false);
  assert.deepEqual(f.events.filter(v => v.kind === 'fileOutputGated').map(v => v.enabled), [false, true]);
  assert.equal(f.context.closed, false);
  await close(f);
});

test('门控不自动恢复暂停意图，旧票据和畸形输入不改变当前路径', async () => {
  const f = fixture();
  await load(f);
  await f.player.control(control('play', 2));
  await f.player.control(control('pause', 3));
  assert.equal(await f.player.outputGate(outputGate(false)), true);
  assert.equal(await f.player.outputGate(outputGate(true, '2')), true);
  assert.equal(f.player.state, 'paused');
  assert.equal(f.player.media.paused, true);
  assert.equal(f.player.media.hidden, true);
  for (const input of [outputGate(false, '2'), outputGate(false, '0'), outputGate(false, '01'),
      outputGate(false, '3\n'), outputGate(false, '18446744073709551616'),
      outputGate(false, '3', {sessionId: 'other'}), outputGate(false, '3', {unknown: true})])
    assert.equal(await f.player.outputGate(input), false);
  assert.equal(f.player.outputEnabled, true);
  await close(f);
});

test('上下文未知或关闭中不报告门控成功且不关闭借用上下文', async () => {
  const f = fixture();
  await load(f);
  f.context.state = 'suspended';
  assert.equal(await f.player.outputGate(outputGate(false)), false);
  assert.equal(f.events.some(v => v.kind === 'fileOutputGated'), false);
  assert.equal(f.player.waiters.size, 0);
  assert.equal(f.context.closed, false);
  f.player.closing = true;
  f.player.release();
});
test('加载固定文件不自动播放，Bearer只送同源测试资产', async () => {
  const f = fixture();
  await load(f);
  assert.equal(f.events.at(-1).state, 'loaded');
  assert.equal(f.events.at(-1).positionMs, 0);
  assert.equal(f.events.at(-1).audioSamples, 0);
  assert.equal(f.media[0].paused, true);
  assert.equal(f.media[0].hidden, true);
  assert.equal(f.requests[0].config.redirect, 'error');
  assert.equal(f.requests[0].config.credentials, 'omit');
  assert.equal(f.requests[0].config.headers.Authorization, 'Bearer ' + 'a'.repeat(64));
  await close(f);
});
test('远端固定授权源不泄露配对令牌，源外地址在fetch前拒绝', async () => {
  const f = fixture();
  assert.equal(await f.player.initialize(bind({origin: 'https://media.example.com'})), true);
  assert.equal(await f.player.control(control('load', 1, {url: 'https://media.example.com/file.wav', validForMs: 1000})), true);
  assert.equal(Object.keys(f.requests[0].config.headers).length, 0);
  await close(f);
  const denied = fixture();
  await denied.player.initialize(bind({origin: 'https://media.example.com'}));
  assert.equal(await denied.player.control(control('load', 1, {url: 'https://media.example.com.evil.test/file.wav', validForMs: 1000})), false);
  assert.equal(denied.requests.length, 0);
  assert.equal(denied.events.at(-1).code, 502);
  assert.equal(JSON.stringify(denied.events).includes('evil.test'), false);
  await close(denied);
});
test('实际流大小超限即取消，不只信任Content-Length', async () => {
  const f = fixture({length: '1', chunks: [new Uint8Array(600), new Uint8Array(600)]});
  await f.player.initialize(bind());
  assert.equal(await f.player.control(control('load', 1, {url: '/media/file.wav', validForMs: 1000})), false);
  assert.equal(f.cancelled(), 1);
  assert.equal(f.requests[0].config.signal.aborted, true);
  assert.equal(f.urls.length, 0);
  assert.equal(f.events.at(-1).code, 502);
  await close(f);
});
test('重定向或MIME不匹配不产生loaded或Blob', async () => {
  for (const options of [{redirected: true}, {mime: 'text/html'}, {length: '9007199254740992'}]) {
    const f = fixture(options);
    await f.player.initialize(bind());
    assert.equal(await f.player.control(control('load', 1, {url: '/media/file.wav', validForMs: 1000})), false);
    assert.equal(f.urls.length, 0);
    assert.equal(f.events.at(-1).code, 502);
    await close(f);
  }
});
test('播放等待音频线程采样，暂停与关闭不销毁借用的AI上下文', async () => {
  const f = fixture();
  await load(f);
  assert.equal(await f.player.control(control('play', 2)), true);
  assert.equal(f.events.at(-1).state, 'playing');
  assert.equal(f.events.at(-1).audioSamples, 256);
  assert.equal(f.player.energy, 64);
  assert.equal(await f.player.control(control('pause', 3)), true);
  assert.equal(f.events.at(-1).state, 'paused');
  assert.equal(f.media[0].paused, true);
  assert.equal(await close(f), true);
  assert.equal(f.events.at(-1).kind, 'fileClosed');
  assert.equal(f.media[0].removed, true);
  assert.ok(f.nodes.every(n => n.disconnected));
  assert.deepEqual(f.urls, ['created', 'revoked']);
  assert.equal(f.context.closed, false);
});
test('视频无声模式仍等待真实帧回调，显示暂停后隐藏', async () => {
  const f = fixture({mime: 'video/mp4'});
  await load(f, 'video/mp4', false);
  assert.equal(f.media[0].muted, true);
  assert.equal(await f.player.control(control('play', 2)), true);
  assert.equal(f.events.at(-1).audioSamples, 0);
  assert.ok(f.events.at(-1).videoFrames > 0);
  assert.equal(f.media[0].hidden, false);
  assert.equal(await f.player.control(control('pause', 3)), true);
  assert.equal(f.media[0].hidden, true);
  await close(f);
});
test('长seek被stop打断时旧协程不能恢复状态或覆盖新应答', async () => {
  const f = fixture({longSeek: true});
  await load(f);
  const seek = f.player.control(control('seek', 2, {positionMs: 1000}));
  for (let count = 0; count < 20 && f.player.state !== 'seeking'; ++count)
    await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(f.player.state, 'seeking');
  assert.equal(await f.player.control(control('stop', 3)), true);
  assert.equal(await seek, false);
  assert.equal(f.player.state, 'stopped');
  assert.equal(f.events.at(-1).state, 'stopped');
  assert.equal(f.events.at(-1).generation, '3');
  assert.equal(f.player.busy, false);
  await close(f);
});
test('迟到控制和跨实例关闭不能操纵当前媒体，uint64保持字符串', async () => {
  const f = fixture();
  await load(f);
  assert.equal(await f.player.control(control('play', '18446744073709551615')), true);
  const before = f.events.length;
  assert.equal(await f.player.control(control('stop', 2)), false);
  assert.equal(f.events.length, before);
  assert.equal(f.media[0].paused, false);
  assert.equal(await f.player.close({kind: 'fileClose', sessionId: 'other', playbackId: 'p', generation: f.player.generation}), false);
  assert.equal(f.events.at(-1).generation, '18446744073709551615');
  await close(f);
});
test('关闭在途播放取消等待，不拼接迟到采样为playing', async () => {
  const f = fixture({noSamples: true});
  await load(f);
  const playing = f.player.control(control('play', 2));
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(f.events.some(v => v.state === 'playing'), false);
  assert.equal(await close(f), true);
  assert.equal(await playing, false);
  f.player.audio({kind: 'fileAudioMeasured', generation: '2', ticket: 2, samples: 512, energy: 128});
  assert.equal(f.events.at(-1).kind, 'fileClosed');
  assert.equal(f.events.some(v => v.state === 'playing'), false);
  assert.equal(f.player.waiters.size, 0);
  assert.equal(f.context.closed, false);
});
test('关闭在途下载中止网络，不创建迟到Blob或加载应答', async () => {
  const f = fixture({hungFetch: true});
  await f.player.initialize(bind());
  const loading = f.player.control(control('load', 1, {url: '/media/file.wav', validForMs: 1000}));
  assert.equal(f.requests.length, 1);
  assert.equal(await close(f), true);
  assert.equal(await loading, false);
  assert.equal(f.requests[0].config.signal.aborted, true);
  assert.equal(f.urls.length, 0);
  assert.equal(f.events.at(-1).kind, 'fileClosed');
  assert.equal(f.events.some(v => v.state === 'loaded'), false);
});
test('纯音频门控不依赖不存在的显示路径，视频仍须实际绘制确认', async () => {
  for (const video of [false, true]) {
    const f = fixture({blockPaint: true, mime: video ? 'video/mp4' : 'audio/wav'});
    try {
      await load(f, video ? 'video/mp4' : 'audio/wav');
      assert.equal(await f.player.control(control('play', 2)), true);
      const confirmed = await f.player.outputGate({kind: 'fileOutputGate', sessionId: 'child',
        playbackId: 'p', gateTicket: '1', enabled: false});
      assert.equal(confirmed, !video);
      assert.equal(f.events.some(v => v.kind === 'fileOutputGated'), !video);
      assert.equal(f.player.outputGateFailure, video ? 'paint' : '');
      assert.equal(f.context.closed, false);
    } finally { await close(f); }
  }
});

test('自然结束是独立事件，实际失败不输出原始媒体错误', async () => {
  const ended = fixture();
  await load(ended);
  await ended.player.control(control('play', 2));
  ended.media[0]._time = ended.media[0].duration;
  ended.media[0].ended = true;
  await ended.player.naturalEnd();
  assert.equal(ended.events.at(-1).state, 'ended');
  assert.equal(ended.events.at(-1).spontaneous, true);
  assert.equal(ended.events.at(-1).positionMs, 5000);
  await close(ended);
  const failed = fixture();
  await load(failed);
  failed.media[0].error = {message: 'signed-url-and-private-content'};
  failed.player.mediaFailure();
  assert.equal(failed.events.at(-1).state, 'failed');
  assert.equal(failed.events.at(-1).spontaneous, true);
  assert.equal(JSON.stringify(failed.events).includes('private-content'), false);
  await close(failed);
});
test('微小流块也受结构数量限制，不用字节预算掩盖大量对象内存', async () => {
  const f = fixture({chunks: Array.from({length: 8193}, () => new Uint8Array([1]))});
  await f.player.initialize(bind({maxBytes: 100000}));
  try {
    assert.equal(await f.player.control(control('load', 1, {url: '/media/file.wav', validForMs: 1000})), false);
    assert.equal(f.urls.length, 0);
    assert.equal(f.cancelled(), 1);
  } finally { await close(f); }
});

test('暂停或丢失音频节点不能伪造已静音及整组实际释放', async () => {
  for (const state of ['suspended', 'closed', 'missingWorklet', 'audioFailed']) {
    const f = fixture();
    await load(f);
    await f.player.control(control('play', 2));
    if (state === 'missingWorklet') f.player.worklet = null;
    else if (state === 'audioFailed') f.player.audioFailed = true;
    else f.context.state = state;
    assert.equal(await f.player.control(control('pause', 3)), false, state);
    assert.equal(f.events.at(-1).code, 502, state);
    assert.equal(await close(f), false, state);
    assert.equal(f.events.some(value => value.kind === 'fileClosed'), false, state);
    assert.equal(f.player.media, null, state);
    assert.equal(f.player.worklet, null, state);
    assert.equal(f.context.closed, false, state);
  }
});

test('直接关闭暂停上下文必须保留未知释放，不修改宿主上下文', async () => {
  const f = fixture({mime: 'video/mp4'});
  await load(f, 'video/mp4');
  await f.player.control(control('play', 2));
  f.context.state = 'suspended';
  assert.equal(await close(f), false);
  assert.equal(f.events.some(value => value.kind === 'fileClosed'), false);
  assert.equal(f.player.media, null);
  assert.equal(f.player.waiters.size, 0);
  assert.equal(f.player.gates.size, 0);
  assert.equal(f.context.state, 'suspended');
  assert.equal(f.context.closed, false);
});
