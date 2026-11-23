// 文件说明：在隔离DOM中验证正式无媒体页面装配，不把此夹具称为浏览器绘制或模型端到端验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const app = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');
const theme = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/client-theme.js'), 'utf8');

// page运行原正式脚本；媒体端口一旦被构造即失败，记录明确调用而不是只检查消息名称。
function page(textOnly = true, search = '') {
  const elements = new Map(), sent = [];
  let mediaCalls = 0;
  const context = vm.createContext({TextEncoder, URLSearchParams, setTimeout, clearTimeout,
    queueMicrotask, UrTextOnlyDevice: textOnly, location: {host: '127.0.0.1:18000', search},
    fetch: async () => ({ok: true, json: async () => ({token: 'a'.repeat(64)})}),
    navigator: {mediaDevices: {getUserMedia() {++mediaCalls; throw Error('media_must_not_open');}}},
    AudioContext: class {constructor() {++mediaCalls; throw Error('audio_must_not_open');}},
    RTCPeerConnection: class {constructor() {++mediaCalls; throw Error('peer_must_not_open');}},
    WebSocket: class {
      static OPEN = 1;
      constructor() {this.readyState = 1; this.bufferedAmount = 0; queueMicrotask(() => this.onopen?.());}
      send(body) {sent.push(JSON.parse(body));}
      close() {this.readyState = 3;}
    },
    document: {visibilityState: 'visible', body: {style: {}, dataset: {}, getBoundingClientRect: () => ({width: 100, height: 100})},
      getElementById(id) {
        if (!elements.has(id)) elements.set(id, {disabled: true, checked: true, value: '', textContent: '',
          addEventListener(name, action) {this[name] = action;}});
        return elements.get(id);
      }},
    window: {addEventListener() {}}, requestAnimationFrame: () => 1, cancelAnimationFrame() {},
    getComputedStyle: value => value.style,
  });
  vm.runInContext(theme, context);
  vm.runInContext(app, context);
  return {context, elements, sent, mediaCalls: () => mediaCalls,
    incoming: value => context.incoming(value), activate: () => context.activate()};
}

test('无媒体正式激活仅预检主题并连接，不申请麦克风、AudioContext或Peer', async () => {
  const p = page(); await p.activate();
  assert.equal(p.mediaCalls(), 0);
  assert.deepEqual(p.sent.map(value => value.kind), ['clientThemeReady', 'textReady']);
  assert.equal(vm.runInContext('terminated', p.context), false);
});
test('无媒体建立只开启文字及原回复取消，音频按钮保持关闭', async () => {
  const p = page(); await p.activate();
  p.incoming({kind: 'state', established: true, transport: 'none', text: '文字会话', responseId: 'reply-one'});
  for (const id of ['start', 'stop', 'textAudio']) assert.equal(p.elements.get(id).disabled, true);
  for (const id of ['textInput', 'sendText', 'cancel', 'end']) assert.equal(p.elements.get(id).disabled, false);
  assert.equal(p.elements.get('textAudio').checked, false);
  p.elements.get('cancel').click();
  assert.deepEqual(p.sent.at(-1), {kind: 'cancel', id: 'reply-one'});
  p.elements.get('textInput').value = '固定输入'; p.elements.get('sendText').click();
  assert.deepEqual(p.sent.at(-1), {kind: 'sendText', text: '固定输入', audio: false});
});
test('无媒体不能接纳音频模式冒充建立，终态迟到投影不能重开输入', async () => {
  const p = page(); await p.activate();
  assert.throws(() => p.incoming({kind: 'state', established: true, interactionMode: 'fullDuplex', text: '不可采用'}));
  p.context.release('关闭');
  p.incoming({kind: 'state', established: true, transport: 'none', text: '迟到', responseId: 'reply-one'});
  assert.equal(p.elements.get('sendText').disabled, true);
  assert.equal(p.mediaCalls(), 0);
});
test('默认音频页面不因新增无媒体入口静默绕过真实麦克风预检', async () => {
  const p = page(false); await p.activate();
  assert.equal(p.mediaCalls(), 1);
  assert.equal(p.sent.some(value => value.kind === 'textReady'), false);
  assert.equal(vm.runInContext('terminated', p.context), true);
});
test('URL不能把宿主固定None能力切换为WebRTC，也不能申请任何媒体', async () => {
  const p = page(true, '?media=webrtc'); await p.activate();
  assert.equal(p.mediaCalls(), 0);
  assert.equal(p.sent.length, 0);
  assert.equal(vm.runInContext('terminated', p.context), true);
});
