// 文件说明：正式页面只读控制恢复事实；模拟DOM不冒充真实MQTT或HAL验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');
// page 只提供原页面初始化需要的DOM；任何隐式网络或采集调用都会使测试失败。
function page(enabled = true) {
  const elements = new Map();
  const context = vm.createContext({UrAIControlRecovery: enabled, TextEncoder,
    document: {getElementById(id) {
      if (!elements.has(id)) elements.set(id, {addEventListener() {}});
      return elements.get(id);
    }},
    window: {addEventListener() {}}});
  vm.runInContext(source, context);
  return {context, elements, receive: value => context.incoming(value),
    state: () => JSON.parse(vm.runInContext('JSON.stringify(aiControlState)', context))};
}
// fact 保留完整固定原生投影，不包括会话ID、地址或凭据。
function fact(state = 'ready', endpointEpoch = 1, messageWatermark = 0) {
  return {kind: 'controlRecovery', state, endpointEpoch, messageWatermark, originalSessionPreserved: true};
}
test('原生显式能力才消费固定恢复事实，不自动开麦或重连', () => {
  const p = page();
  p.receive(fact());
  assert.deepEqual(p.state(), fact());
  for (const state of ['disconnected', 'connecting', 'synchronizing', 'ready', 'inactive']) {
    p.receive(fact(state, 2, 9));
    assert.deepEqual(p.state(), fact(state, 2, 9));
  }
  assert.throws(() => page(false).receive(fact()));
});
test('额外身份字段、非安全整数与未知状态拒绝且不替换上一合法投影', () => {
  const p = page(); p.receive(fact());
  for (const value of [{...fact(), sessionId: 'private'}, {...fact(), password: 'private'},
    {...fact(), state: 'resumed'}, {...fact(), endpointEpoch: 0},
    {...fact(), endpointEpoch: Number.MAX_SAFE_INTEGER + 1}, {...fact(), endpointEpoch: '1'},
    {...fact(), messageWatermark: -1}, {...fact(), messageWatermark: 1.5},
    {...fact(), messageWatermark: null}, {...fact(), originalSessionPreserved: 'true'}]) {
    assert.throws(() => p.receive(value));
    assert.deepEqual(p.state(), fact());
  }
});
test('公开端代次与MSG水位不回退，晚到旧事实不能覆盖新连接状态', () => {
  const p = page(); p.receive(fact('ready', 2, 9));
  assert.throws(() => p.receive(fact('ready', 1, 9)));
  assert.throws(() => p.receive(fact('ready', 2, 8)));
  assert.deepEqual(p.state(), fact('ready', 2, 9));
});
test('UDP控制断网只有原生实际恢复投影可保留用户停止和结束按钮', () => {
  const p = page(), state = {kind: 'state', established: false, recovering: true, text: '恢复中'};
  assert.throws(() => p.receive(state));
  p.receive(fact('disconnected'));
  p.receive(state);
  assert.equal(p.elements.get('start').disabled, true);
  assert.equal(p.elements.get('stop').disabled, false);
  assert.equal(p.elements.get('end').disabled, false);
  const old = page(false);
  assert.throws(() => old.receive(state));
});
module.exports = {page, fact};
