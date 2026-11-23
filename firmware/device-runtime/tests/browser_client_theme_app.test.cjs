// 文件说明：真实app与主题模块的离线装配/退休回归；可控DOM不冒称正式浏览器或模型E2E。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const reception = require('./browser_client_tool_live.cjs'); // 复用正式只读observer，不能由测试吞掉业务消息。

// mounted运行原activate与原配对消息分派，麦克风授权挂起以隔离本轮主题端口，不重写业务函数。
async function mounted(withTheme = true, requireWindowReceiver = false) {
  const elements = new Map(), frames = new Map(), timers = new Map(), sent = [];
  const body = {style: {}, dataset: {}, getBoundingClientRect: () => ({width: 800, height: 600})};
  let sequence = 0, socket;
  // PairedSocket仅替代外部回环传输；正式app仍负责安装原onmessage/onclose。
  class PairedSocket {
    static OPEN = 1;
    constructor() {
      socket = this;
      this.readyState = 1;
      this.bufferedAmount = 0;
      Promise.resolve().then(() => this.onopen());
    }
    send(value) { sent.push(JSON.parse(value)); }
    close() { this.readyState = 3; }
  }
  const context = vm.createContext({
    document: {body, visibilityState: 'visible', getElementById(id) {
      if (!elements.has(id)) elements.set(id, {textContent: '', disabled: false, addEventListener() {}});
      return elements.get(id);
    }},
    window: {addEventListener() {}}, location: {host: 'localhost', search: ''}, URLSearchParams,
    fetch: async () => ({ok: true, json: async () => ({token: 'a'.repeat(64)})}),
    WebSocket: PairedSocket, navigator: {mediaDevices: {getUserMedia: () => new Promise(() => {})}},
    TextEncoder,
    setTimeout(fn, delay) { timers.set(++sequence, {fn, delay}); return sequence; },
    clearTimeout(id) { timers.delete(id); },
    requestAnimationFrame(fn) { frames.set(++sequence, fn); return sequence; },
    cancelAnimationFrame(id) { frames.delete(id); },
    getComputedStyle: element => ({backgroundColor: element.style.backgroundColor, color: element.style.color}),
  });
  // 原生Window方法需要正确receiver；此回归约束对应独立真实Chromium探针已复现的调用边界，不造生产异常。
  if (requireWindowReceiver) {
    context.receiverPorts = {setTimeout: context.setTimeout, clearTimeout: context.clearTimeout,
      requestAnimationFrame: context.requestAnimationFrame, cancelAnimationFrame: context.cancelAnimationFrame,
      getComputedStyle: context.getComputedStyle};
    vm.runInContext(`for (const name of Object.keys(receiverPorts)) {
      globalThis[name] = function(...args) {
        if (this !== globalThis) throw new TypeError('window_receiver_required');
        return receiverPorts[name](...args);
      };
    }`, context);
  }
  if (withTheme) vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/client-theme.js'), 'utf8'), context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8'), context);
  void context.activate();
  for (let turn = 0; turn < 8; ++turn) await Promise.resolve();
  assert.equal(typeof socket.onmessage, 'function');
  return {body, sent, frames, timers,
    incoming: value => context.dispatchLocalMessage(value),
    wire: value => socket.onmessage({data: JSON.stringify(value)}),
    close: () => socket.onclose(),
    terminated: () => vm.runInContext('terminated', context),
    metrics: () => JSON.parse(vm.runInContext('JSON.stringify(metrics)', context)),
    audio: () => vm.runInContext('worklet = {port: {postMessage() {}, close() {}}, disconnect() {}}', context),
    captureFact: value => context.recordCaptureState(value),
    // 只在测试指定的建会后时点安装正式observer，原incoming仍由生产app执行。
    installReceptionObserver: () => vm.runInContext('(' + reception.installObserver.toString() + ')(' +
      reception.projectMessageDiagnostics.toString() + ')', context),
    reception: () => JSON.parse(vm.runInContext('JSON.stringify(__urClientToolLive.messageDiagnostics)', context)),
    frame: () => { const fn = frames.values().next().value; assert.ok(fn); fn(); },
  };
}
// command含固定本地票据，不向网页传递平台session/resp/toolID/执行令牌。
const command = (themeTicket = '1', theme = 'dark') => ({kind: 'clientThemeSet', themeTicket, theme});

// 初始诊断由真实C++发送于observer安装前；这里直接走原app分派，不能借测试observer吞掉消息。
test('初始空端接收诊断经原incoming不抛异常且没有页面副作用', async () => {
  const page = await mounted();
  const before = page.metrics(), sent = structuredClone(page.sent);
  assert.doesNotThrow(() => page.incoming({kind: 'messageDiagnostic', received: 0,
    taken: 0, hostFiltered: 0, hostSent: 0, hostSendFailed: 0, overflow: false, records: []}));
  assert.deepEqual(page.metrics(), before);
  assert.deepEqual(page.sent, sent);
  assert.deepEqual(page.body.style, {});
  assert.deepEqual(page.body.dataset, {});
  assert.equal(page.terminated(), false);
  assert.equal(page.frames.size, 0);
  assert.equal(page.timers.size, 0);
});

// 原socket解析与catch/release必须实际执行，合法或畸形诊断均只隔离，其他未知业务消息仍严格拒绝。
test('原配对socket的端接收诊断不触发release，畸形诊断也不污染主会话', async () => {
  const page = await mounted();
  const before = page.metrics(), sent = structuredClone(page.sent);
  for (const diagnostic of [
    {kind: 'messageDiagnostic', received: 0, taken: 0, hostFiltered: 0,
      hostSent: 0, hostSendFailed: 0, overflow: false, records: []},
    {kind: 'messageDiagnostic'},
    {kind: 'messageDiagnostic', received: 'invalid', records: 'private-body',
      sessionId: 'private-id', text: 'private-text', state: 'established'},
  ]) {
    page.wire(diagnostic);
    assert.equal(page.terminated(), false);
    assert.deepEqual(page.metrics(), before);
    assert.deepEqual(page.sent, sent);
    assert.deepEqual(page.body.style, {});
    assert.deepEqual(page.body.dataset, {});
  }
  assert.throws(() => page.incoming({kind: 'unknown_business_message'}), /unknown_message/);
  page.wire({kind: 'unknown_business_message'});
  assert.equal(page.terminated(), true);
});

// 诊断必须仍经过incoming，建会后正式observer可看到固定副本；畸形消息只产生null，不变业务。
test('真实app接受初始诊断后仍允许正式observer观察，未知诊断保持null', async () => {
  const page = await mounted();
  const before = page.metrics(), sent = structuredClone(page.sent);
  page.wire({kind: 'messageDiagnostic', received: 0, taken: 0, hostFiltered: 0,
    hostSent: 0, hostSendFailed: 0, overflow: false, records: []});
  assert.equal(page.terminated(), false);
  page.installReceptionObserver();
  page.wire({kind: 'messageDiagnostic', received: 1, taken: 0, hostFiltered: 0,
    hostSent: 0, hostSendFailed: 0, overflow: false, records: [], body: 'private-text'});
  assert.equal(page.reception().received, 1);
  assert.ok(!JSON.stringify(page.reception()).includes('private-text'));
  page.wire({kind: 'messageDiagnostic', records: 'private-text'});
  assert.equal(page.reception(), null);
  assert.deepEqual(page.metrics(), before);
  assert.deepEqual(page.sent, sent);
  assert.equal(page.terminated(), false);
  page.close();
  page.incoming({kind: 'messageDiagnostic', text: 'private-text'});
  assert.equal(page.terminated(), true, '诊断不能恢复已结束的页面');
});

test('采集事实只接原音频线程当前命令，停止入队或迟到旧启动不能冒已关闭', async () => {
  const page = await mounted();
  page.audio();
  assert.equal(page.metrics().captureOpen, null);
  assert.throws(() => page.captureFact({kind: 'captureState', id: 'original', enabled: false}));
  assert.throws(() => page.captureFact({kind: 'captureState', id: '', enabled: true}));
  page.captureFact({kind: 'captureState', id: '', enabled: false});
  assert.equal(page.metrics().captureOpen, false, '仅音频线程初始化回报可确认尚未开始输入');
  page.incoming({kind: 'captureStart', id: 'original'});
  page.captureFact({kind: 'captureState', id: 'original', enabled: true});
  assert.equal(page.metrics().captureOpen, true);
  page.incoming({kind: 'captureStop', id: 'original'});
  assert.equal(page.metrics().captureOpen, null, '停止命令入队仍不是音频线程实态');
  page.captureFact({kind: 'captureState', id: '', enabled: false});
  assert.equal(page.metrics().captureOpen, null, '初始化事实不能代替原输入停止');
  page.captureFact({kind: 'captureState', id: 'original', enabled: true});
  assert.equal(page.metrics().captureOpen, null);
  page.captureFact({kind: 'captureState', id: 'original', enabled: false});
  assert.equal(page.metrics().captureOpen, false);
  assert.throws(() => page.captureFact({kind: 'captureState', id: 'other', enabled: false, extra: true}));
  page.close();
  assert.equal(page.metrics().captureOpen, null);
  assert.throws(() => page.captureFact({kind: 'captureState', id: 'original', enabled: false}));
});

test('正式activate独立预检，原分派应用真实DOM后仍须原帧读回', async () => {
  const page = await mounted();
  assert.deepEqual(page.sent, [{kind: 'clientThemeReady', schemaRevision: 1}]);
  page.wire(command());
  assert.equal(page.body.dataset.clientTheme, 'dark');
  assert.equal(page.body.style.backgroundColor, 'rgb(15, 23, 42)');
  assert.equal(page.sent.length, 1);
  page.frame();
  assert.deepEqual(page.sent[1], {kind: 'clientThemeActual', themeTicket: '1', status: 'succeeded', theme: 'dark'});
  assert.equal(page.frames.size, 0);
  assert.equal(page.timers.size, 0);
});

test('正式主题装配保持Window receiver，原配对命令不会错误关闭且清理同原帧和timer', async () => {
  const page = await mounted(true, true);
  page.wire(command());
  assert.equal(page.terminated(), false);
  assert.equal(page.body.dataset.clientTheme, 'dark');
  assert.equal(page.frames.size, 1);assert.equal(page.timers.size, 1);
  page.frame();
  assert.deepEqual(page.sent[1], {kind: 'clientThemeActual', themeTicket: '1', status: 'succeeded', theme: 'dark'});
  assert.equal(page.frames.size, 0);assert.equal(page.timers.size, 0);
  page.wire(command('2', 'light'));page.close();
  assert.equal(page.frames.size, 0);assert.equal(page.timers.size, 0);
  assert.equal(page.terminated(), true);assert.equal(page.sent.length, 2);
});

test('固定原Submit ACK仅内存计数，不改变主题/回复/播放，重复与非法关联不计', async () => {
  const page = await mounted();
  const receipt = {kind: 'clientToolSubmitted', sessionId: 'session', responseId: 'reply', toolCallId: 'call',
    requestMethod: 'toolCallSubmit', continuation: 'unknown', code: 200, receiptIndex: 1};
  page.incoming(receipt);
  assert.equal(page.metrics().clientToolReceipts[0].continuation, 'unknown');
  assert.equal(page.metrics().completedResponses, 0);
  assert.equal(page.metrics().consumed, 0);
  assert.deepEqual(page.body.style, {});
  for (const change of [{receiptIndex: 1}, {requestMethod: 'audioStart'}, {executionToken: 'synthetic'},
    {responseId: ''}, {toolCallId: null}, {sessionId: ''}, {code: 403, continuation: 'accepted'}, {receiptIndex: 513}])
    assert.throws(() => page.incoming({...receipt, receiptIndex: 2, ...change}), /invalid_client_tool_receipt/);
  assert.equal(page.metrics().clientToolReceiptIndex, 1);
  for (let index = 2; index <= 35; ++index) page.incoming({...receipt, receiptIndex: index});
  assert.equal(page.metrics().clientToolReceipts.length, 32);
  assert.equal(page.metrics().clientToolReceiptIndex, 35);
  page.close();
  assert.throws(() => page.incoming({...receipt, receiptIndex: 36}), /invalid_client_tool_receipt/);
  assert.equal(page.metrics().clientToolReceiptIndex, 35);
});

test('原关闭挂点退休等待，迟到帧和新票据不能补成功或改变DOM', async () => {
  const page = await mounted();
  page.wire(command());
  const late = page.frames.values().next().value;
  page.close();
  late();
  assert.equal(page.terminated(), true);
  assert.equal(page.sent.length, 1);
  assert.equal(page.frames.size, 0);
  assert.equal(page.timers.size, 0);
  assert.throws(() => page.incoming(command('2', 'light')), /invalid_client_theme/);
  assert.equal(page.body.dataset.clientTheme, 'dark');
});

test('未装配主题模块不广告，展示事件与自由参数不能自授执行能力', async () => {
  const page = await mounted(false);
  assert.deepEqual(page.sent, []);
  assert.throws(() => page.incoming(command()), /invalid_client_theme/);
  assert.deepEqual(page.body.style, {});
  const ready = await mounted();
  ready.incoming({kind: 'toolStatus', id: 'call', name: 'set_local_display_theme', state: 'started'});
  assert.deepEqual(ready.body.style, {});
  ready.wire({...command(), script: 'arbitrary'});
  assert.equal(ready.terminated(), true);
  assert.deepEqual(ready.body.style, {});
  assert.equal(ready.sent.length, 1);
});

test('正式资产显式加载主题模块再装配app，不改变旧媒体资产内容', () => {
  const html = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/index.html'), 'utf8');
  assert.ok(html.indexOf('/client-theme.js') < html.indexOf('/app.js'));
  assert.match(html, /<script src="\/client-theme\.js" defer><\/script>/);
  const embedded = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web_assets.hpp.in'), 'utf8');
  assert.match(embedded, /client-theme\.js/);
  assert.match(embedded, /@UR_CLIENT_THEME@/);
});
