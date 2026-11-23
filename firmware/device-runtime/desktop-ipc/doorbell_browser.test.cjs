// 文件说明：正式IPC页面的门铃按钮离线测试，核对原意图去重、事件状态和零媒体副作用。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 执行正式网页源码，采集/Peer调用一旦发生即计为越权，不使用真实网络或计时等待。
function fixture() {
  const elements = new Map(), sent = [];
  let captures = 0, peers = 0, timers = 0;
  const socket = {readyState: 1, bufferedAmount: 0, closes: 0,
    send(body) { sent.push(JSON.parse(body)); }, close() { this.readyState = 3; this.closes++; }};
  const getElement = id => {
    if (!elements.has(id)) elements.set(id, {textContent: '', disabled: false, onclick: null});
    return elements.get(id);
  };
  const context = vm.createContext({
    document: {getElementById: getElement},
    navigator: {mediaDevices: {getUserMedia() { captures++; throw Error('unexpected_capture'); }}},
    RTCPeerConnection: class { constructor() { peers++; throw Error('unexpected_peer'); } },
    WebSocket: {OPEN: 1}, window: {addEventListener() {}}, testSocket: socket,
    setTimeout() { timers++; throw Error('unexpected_auto_retry'); }, clearTimeout() {},
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'web/app.js'), 'utf8'), context);
  vm.runInContext('socket = testSocket;', context);
  return {sent, socket, elements, context,
    click() { getElement('doorbell').onclick(); },
    privacy(enabled) { getElement(enabled ? 'privacy-enable' : 'privacy-disable').onclick(); },
    async incoming(value) { context.value = value; await vm.runInContext('incoming(value)', context); },
    stop() { vm.runInContext('stop()', context); },
    facts() { return {captures, peers, timers}; },
  };
}

test('隐私出口独立默认关闭，原布尔报告在途去重且ACK不制造媒体副作用', async () => {
  const f=fixture();f.privacy(true);await f.incoming({kind:'ipcAvailable'});await f.incoming({kind:'doorbellAvailable'});
  f.privacy(false);assert.equal(f.sent.length,0);
  await f.incoming({kind:'privacyAvailable'});f.privacy(true);f.privacy(false);f.privacy(true);
  assert.deepEqual(f.sent,[{kind:'privacyReport',enabled:true}]);
  for(const id of ['privacy-enable','privacy-disable'])assert.equal(f.elements.get(id).disabled,true);
  await f.incoming({kind:'privacyState',state:'ack'});assert.match(f.elements.get('privacy-status').textContent,/旧媒体不会自动恢复/);
  f.privacy(false);assert.deepEqual(f.sent,[{kind:'privacyReport',enabled:true},{kind:'privacyReport',enabled:false}]);
  assert.deepEqual(f.facts(),{captures:0,peers:0,timers:0});
});

test('隐私拒绝与未知分别展示且不自动重试，断连迟到通知不能复活出口',async()=>{
  const f=fixture();await f.incoming({kind:'privacyAvailable'});f.privacy(true);
  await f.incoming({kind:'privacyState',state:'rejected'});assert.match(f.elements.get('privacy-status').textContent,/被拒绝/);
  assert.equal(f.sent.length,1);f.privacy(false);await f.incoming({kind:'privacyState',state:'unknown'});
  assert.match(f.elements.get('privacy-status').textContent,/结果未知/);assert.equal(f.sent.length,2);
  f.privacy(true);f.stop();const original=f.elements.get('privacy-status').textContent;
  await f.incoming({kind:'privacyAvailable'});await f.incoming({kind:'privacyState',state:'ack'});f.privacy(false);
  assert.equal(f.elements.get('privacy-status').textContent,original);assert.equal(f.sent.length,3);
  for(const id of ['privacy-enable','privacy-disable'])assert.equal(f.elements.get(id).disabled,true);
  assert.deepEqual(f.facts(),{captures:0,peers:0,timers:0});
});

test('隐私通知严格校验，属性出口不可用不能清空原IPC状态',async()=>{
  const f=fixture();await f.incoming({kind:'ipcAvailable'});
  for(const value of [{kind:'privacyAvailable',enabled:true},{kind:'privacyState',state:'stopped'},
    {kind:'privacyState',state:'ack',sessionId:'s'},{kind:'privacyState',state:null}])await assert.rejects(f.incoming(value));
  await f.incoming({kind:'privacyUnavailable'});f.privacy(true);
  assert.equal(vm.runInContext('available',f.context),true);assert.equal(vm.runInContext('closed',f.context),false);
  assert.equal(f.sent.length,0);assert.deepEqual(f.facts(),{captures:0,peers:0,timers:0});
});

// 缺省禁用且IPC能力ACK不能启用门铃事件，事件出口有自己的SUBACK就绪事实。
test('默认禁用，IPC capability就绪不能替代事件出口授权', async () => {
  const f = fixture();
  assert.equal(f.elements.get('doorbell').disabled, true);
  f.click();
  await f.incoming({kind: 'ipcAvailable'});
  f.click();
  assert.equal(f.sent.length, 0);
  assert.equal(f.elements.get('doorbell').disabled, true);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 正式事件可在统一会话能力ACK前上报；新按压只发一个无参意图，Pending重复点击不换token。
test('独立事件出口就绪后按钮只发送无参原意图且Pending去重', async () => {
  const f = fixture();
  await f.incoming({kind: 'doorbellAvailable'});
  assert.equal(vm.runInContext('available', f.context), false);
  f.click(); f.click();
  assert.deepEqual(f.sent, [{kind: 'doorbellPress'}]);
  assert.equal(f.elements.get('doorbell').disabled, true);
  assert.match(f.elements.get('doorbell-status').textContent, /等待平台确认/);
  await f.incoming({kind: 'doorbellState', state: 'pending'});
  f.click();
  assert.equal(f.sent.length, 1);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 成功只说明物模型事件ACK，新同值独立按压仍产生新的本地意图；页面不选择平台token。
test('事件ACK不称媒体连接，随后新的按压仍发送独立意图', async () => {
  const f = fixture();
  await f.incoming({kind: 'doorbellAvailable'});
  f.click();
  await f.incoming({kind: 'doorbellState', state: 'ack'});
  assert.equal(f.elements.get('doorbell').disabled, false);
  assert.match(f.elements.get('doorbell-status').textContent, /不代表呼叫或媒体接通/);
  assert.equal(vm.runInContext('views.size', f.context), 0);
  f.click();
  assert.deepEqual(f.sent, [{kind: 'doorbellPress'}, {kind: 'doorbellPress'}]);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 超时/非成功ACK不自动再按门铃，只有用户再次主动点击才能产生另一个原意图。
test('unknown不自动重试或伪造新token，需要另一次主动按压', async () => {
  const f = fixture();
  await f.incoming({kind: 'doorbellAvailable'});
  f.click();
  await f.incoming({kind: 'doorbellState', state: 'unknown'});
  assert.equal(f.sent.length, 1);
  assert.equal(f.elements.get('doorbell').disabled, false);
  assert.match(f.elements.get('doorbell-status').textContent, /不能确认是否已触发呼叫/);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
  f.click();
  assert.equal(f.sent.length, 2);
});

// 桥背压不能悄悄丢一个按压后自动再创建业务；关闭页面并将原等待标记结果未确认。
test('本地桥背压关闭出口，不创建媒体或隐式重试', async () => {
  const f = fixture();
  await f.incoming({kind: 'doorbellAvailable'});
  f.socket.bufferedAmount = 65537;
  f.click();
  assert.equal(f.sent.length, 0);
  assert.equal(f.socket.closes, 1);
  assert.equal(f.elements.get('doorbell').disabled, true);
  assert.match(f.elements.get('doorbell-status').textContent, /不能确认是否已触发呼叫/);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 断连/关闭后迟到就绪或ACK不能恢复按钮；原状态不会重新打开摄像头或播放。
test('关闭后的迟到事件通知不能恢复门铃按钮', async () => {
  const f = fixture();
  await f.incoming({kind: 'doorbellAvailable'});
  f.click(); f.stop(); f.stop();
  const finalText = f.elements.get('doorbell-status').textContent;
  await f.incoming({kind: 'doorbellAvailable'});
  await f.incoming({kind: 'doorbellState', state: 'ack'});
  f.click();
  assert.equal(f.elements.get('doorbell').disabled, true);
  assert.equal(f.elements.get('doorbell-status').textContent, finalText);
  assert.equal(f.sent.length, 1);
  assert.equal(f.socket.closes, 1);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 本地事件通知也只允许固定字段和枚举，不能借页面直接执行统一信令或媒体操作。
test('非法门铃通知不接受额外授权字段或媒体状态枚举', async () => {
  const f = fixture();
  for (const value of [
    {kind: 'doorbellAvailable', eventID: 'camera'},
    {kind: 'doorbellState', state: 'connected'},
    {kind: 'doorbellState', state: 'ack', sessionId: 'call'},
    {kind: 'doorbellState', state: null},
  ]) await assert.rejects(f.incoming(value));
  assert.equal(f.sent.length, 0);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});

// 门铃未就绪通知只关闭按钮，不应把原IPC观看会话或统一能力状态清空。
test('门铃出口不可用不破坏独立IPC就绪状态', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcAvailable'});
  await f.incoming({kind: 'doorbellAvailable'});
  await f.incoming({kind: 'doorbellUnavailable'});
  f.click();
  assert.equal(vm.runInContext('available', f.context), true);
  assert.equal(vm.runInContext('closed', f.context), false);
  assert.equal(f.sent.length, 0);
  assert.equal(f.elements.get('doorbell').disabled, true);
  assert.deepEqual(f.facts(), {captures: 0, peers: 0, timers: 0});
});
