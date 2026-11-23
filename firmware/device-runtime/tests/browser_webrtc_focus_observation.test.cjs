// 文件说明：通过原浏览器 app 的门控与回执函数验证只读观察器；本地 VM 不冒充真实 Chrome、后端或 Worklet 执行。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {installWebRTCFocusObservation} = require('./browser_webrtc_focus_observation.cjs');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');

// fixture 装配原 app 与局部模拟驱动；原 gate/receipt 验证仍由生产函数执行，不手工填观察结果。
function fixture(enabled) {
  if (arguments.length === 0) enabled = true; // 缺省测试装配开启，显式undefined仍用于关闭边界。
  const sent = [], commands = [], elements = new Map();
  let accepted = true;
  const scope = vm.createContext({UrWebRTCOutputFocus: true,
    location: {search: '?media=webrtc'}, URLSearchParams, Float32Array, TextEncoder,
    performance: {now: () => 100}, WebSocket: {OPEN: 1},
    document: {getElementById(id) {
      if (!elements.has(id)) elements.set(id, {textContent: '', disabled: true, addEventListener() {}});
      return elements.get(id);
    }}, window: {addEventListener() {}}});
  vm.runInContext(source, scope);
  scope.__socket = {readyState: 1, bufferedAmount: 0, send(value) {sent.push(JSON.parse(value));}};
  scope.__node = {port: {postMessage(value) {commands.push(value);}}};
  scope.__accepted = () => accepted;
  vm.runInContext('socket=__socket;rtcOutputNode=__node;const fixtureSend=send;send=function(value){return __accepted()?fixtureSend(value):false;};', scope);
  scope.__install = enabled;
  vm.runInContext(`(${installWebRTCFocusObservation.toString()})(__install)`, scope);
  return {scope, sent, commands, accept(value) {accepted = value;},
    facts() {return JSON.parse(vm.runInContext('JSON.stringify(__urWebRTCFocusObservation.read())', scope));},
    request(ticket, value) {scope.incoming({kind: 'playbackGate', ticket, enabled: value});},
    actual(ticket, value) {scope.rtcOutputActual(scope.__node, {kind: 'callAudioGated', peerEpoch: '0', ticket, enabled: value});},
    receipt(index = 1, operation = 'acquire', code = 200) {
      const states = {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'};
      scope.incoming({kind: 'focusReceipt', state: code === 200 ? states[operation] : 'denied',
        operation, code, revision: index, receiptIndex: index});
    }};
}

test('缺省关闭及非 true 不读取原页面、不安装或修改函数', () => {
  for (const flag of [undefined, false, 0, 1, '1', 'true']) {
    assert.equal(installWebRTCFocusObservation(flag), false);
    const f = fixture(flag);
    assert.equal(vm.runInContext('typeof __urWebRTCFocusObservation', f.scope), 'undefined');
  }
});
test('原请求不等于ACK，完整false/true经原函数与本地send后才有观察', () => {
  const f = fixture();
  f.request('1', false);
  assert.deepEqual(f.facts().events, [{kind: 'gateRequested', enabled: false}]);
  f.actual('1', false);
  assert.equal(f.facts().initialFalseSubmitted, true);
  f.request('2', true); f.actual('2', true);
  assert.equal(f.facts().actualTrueSubmitted, true);
  assert.equal(f.facts().nativeAcquiredObserved, false, '本地提交不能伪造C++受理或MQTT结果');
  f.receipt();
  assert.equal(f.facts().nativeAcquiredObserved, true);
  assert.equal(f.sent.length, 2);
  assert.equal(f.commands.length, 2);
  assert.equal(f.facts().unknown, false);
  assert.equal(Object.hasOwn(f.facts(), 'passed'), false, '组件旁证不能标真实模型通过');
});
test('重复及旧票据不新增ACK观察，裸send不能冒充Worklet回执', () => {
  const f = fixture(); f.request('1', false); f.actual('1', false);
  const count = f.facts().events.length;
  f.actual('1', false);
  f.scope.send({kind: 'outputGated', ticket: '1', enabled: false});
  assert.equal(f.facts().events.length, count);
  f.request('2', true); f.actual('1', false);
  assert.equal(f.facts().actualTrueSubmitted, false);
  f.actual('2', true); assert.equal(f.facts().actualTrueSubmitted, true);
});
test('原send拒绝仍原样返回false，不把留存目标当实际提交', () => {
  const f = fixture(); f.accept(false); f.request('1', false);
  // 原app发送失败会release；只在本例隔离收尾资源，不修改门控函数或send结果。
  vm.runInContext('release=function() {terminated=true;};', f.scope);
  f.actual('1', false);
  assert.equal(f.sent.length, 0);
  assert.equal(f.facts().initialFalseSubmitted, false);
  assert.deepEqual(f.facts().events.at(-1), {kind: 'workletAck', enabled: false, localSubmitted: false});
});
test('原native拒绝保留denied，重复水位仍按原函数失败，不升级success', () => {
  const f = fixture(); f.receipt(1, 'acquire', 409);
  assert.equal(f.facts().nativeAcquiredObserved, false);
  assert.equal(f.facts().events[0].state, 'denied');
  assert.throws(() => f.receipt(1), /invalid_focus_receipt/);
  assert.equal(f.facts().unknown, true);
  assert.equal(f.facts().nativeAcquiredObserved, false);
});
test('无关自由正文不记录，snapshot无别名，stop恢复原函数且不驱动资源', () => {
  const f = fixture();
  f.scope.incoming({kind: 'messageDiagnostic', body: 'PRIVATE_TOKEN_ADDRESS'});
  assert.deepEqual(f.facts().events, []);
  f.request('1', false); f.actual('1', false);
  vm.runInContext('__urWebRTCFocusObservation.read().events.length=0;', f.scope);
  assert.equal(f.facts().events.length, 2);
  vm.runInContext('__urWebRTCFocusObservation.stop();', f.scope);
  f.request('2', true); f.actual('2', true);
  assert.equal(f.facts().events.length, 2);
  assert.equal(f.facts().stopped, true);
  assert.equal(JSON.stringify(f.facts()).includes('PRIVATE'), false);
  assert.equal(JSON.stringify(f.facts()).includes('ticket'), false);
});
test('共享64事件预算不丢早期失败，不因读取或stop重置', () => {
  const f = fixture();
  for (let i = 1; i <= 40; ++i) {f.request(String(i), i % 2 === 0); f.actual(String(i), i % 2 === 0);}
  assert.equal(f.facts().events.length, 64);
  assert.equal(f.facts().truncated, true);
  assert.equal(f.facts().events[0].enabled, false);
  assert.equal(f.sent.length, 40, '满额不得改变原业务发送');
  vm.runInContext('__urWebRTCFocusObservation.stop();', f.scope);
  assert.equal(f.facts().truncated, true);
});
test('额外回执字段及非原节点不被观察为ACK，原拒绝路径不变', () => {
  const f = fixture(); f.request('1', false);
  f.scope.rtcOutputActual({}, {kind: 'callAudioGated', peerEpoch: '0', ticket: '1', enabled: false});
  assert.equal(f.facts().events.length, 1);
  vm.runInContext('release=function() {terminated=true;};', f.scope);
  f.scope.rtcOutputActual(f.scope.__node,
    {kind: 'callAudioGated', peerEpoch: '0', ticket: '1', enabled: false, private: 'PRIVATE_SECRET'});
  assert.equal(f.facts().initialFalseSubmitted, false);
  assert.equal(f.facts().events.length, 1);
  assert.equal(JSON.stringify(f.facts()).includes('PRIVATE'), false);
  assert.equal(vm.runInContext('terminated', f.scope), true);
});
test('stop不覆盖后继观察器且重复安装不再次包裹业务函数', () => {
  const f = fixture();
  assert.equal(vm.runInContext(`(${installWebRTCFocusObservation.toString()})(true)`, f.scope), false);
  vm.runInContext('const laterIncoming=incoming;incoming=function(...args){return laterIncoming.apply(this,args);};const laterWrapper=incoming;__urWebRTCFocusObservation.stop();', f.scope);
  assert.equal(vm.runInContext('incoming===laterWrapper', f.scope), true);
  f.request('1', false); f.actual('1', false);
  assert.deepEqual(f.facts().events, []);
  assert.equal(f.sent.length, 1);
});
