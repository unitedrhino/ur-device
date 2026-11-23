// 文件说明：执行正式页面源端接续本人操作与闭集事实；纯DOM不代表MQTT或真实媒体已验收。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');
// page 只提供原同源DOM和已配对发送观察；没有网络、摄像头或构造平台身份的接口。
function page(enabled = true, human = false) {
  const elements = new Map(), sent = [];
  const context = vm.createContext({UrAIEndpointSource: enabled && !human,
    UrHumanEndpointSource: enabled && human, UrHumanVoiceEnabled: human, TextEncoder,
    document: {getElementById(id) {
      if (!elements.has(id)) elements.set(id, {value: '', addEventListener() {}});
      return elements.get(id);
    }}, window: {addEventListener() {}}});
  vm.runInContext(source, context);
  context.document.getElementById('endpointSourceTarget'); // 静态HTML已有输入框；最小DOM仍须显式建立。
  context.__send = value => {sent.push(JSON.parse(JSON.stringify(value))); return true;};
  vm.runInContext('send=globalThis.__send', context);
  return {elements, sent, receive: value => context.incoming(value),
    click: id => elements.get(id).onclick()};
}
// fact 模拟原C++只读投影，不能以投影本身授予采集、候选或会话权限。
function fact(state = 'idle', values = {}) {
  return {kind: 'endpointSourceState', state, canPrepare: state === 'idle',
    canCommit: state === 'ready', canAbort: state === 'preparing' || state === 'ready',
    pending: false, unknown: false, ...values};
}
test('源端本人prepare只发送目标愿望，ready后本人commit不携平台身份', () => {
  const p = page(); p.receive(fact());
  p.elements.get('endpointSourceTarget').value = 'device:p:target';
  p.click('endpointSourcePrepare');
  assert.deepEqual(p.sent, [{kind: 'endpointSourceControl', operation: 'prepare', target: 'device:p:target'}]);
  p.click('endpointSourceCommit'); assert.equal(p.sent.length, 1);
  p.receive(fact('ready')); p.click('endpointSourceCommit'); p.click('endpointSourceAbort');
  assert.deepEqual(p.sent.slice(1), [{kind: 'endpointSourceControl', operation: 'commit'},
    {kind: 'endpointSourceControl', operation: 'abort'}]);
});
test('在途和未知原结果禁止按钮，不把重投或等待超时作为自动重新发起', () => {
  const p = page();
  for (const value of [fact('pending', {canPrepare: false, pending: true}),
    fact('unknown', {canPrepare: false, unknown: true}), fact('committed')]) {
    p.receive(value);
    for (const id of ['endpointSourcePrepare', 'endpointSourceCommit', 'endpointSourceAbort']) p.click(id);
  }
  assert.deepEqual(p.sent, []);
});
test('未装配、额外身份、非法状态和伪造待确认按钮均拒绝', () => {
  assert.throws(() => page(false).receive(fact()));
  for (const value of [{...fact(), sessionId: 'alien'}, fact('untrusted'),
    {...fact(), canPrepare: 'true'}, {...fact(), pending: true},
    {...fact(), unknown: true}, {...fact(), canCommit: true}])
    assert.throws(() => page().receive(value));
});
test('目标输入沿UTF8字节上限及控制字符规则，非法目标不发送任何平台请求', () => {
  const p = page(); p.receive(fact());
  for (const target of ['', 'bad\nname', 'x'.repeat(129), '中'.repeat(43), '\ud800']) {
    p.elements.get('endpointSourceTarget').value = target;
    p.click('endpointSourcePrepare');
  }
  assert.deepEqual(p.sent, []);
});
// 人际源复用同一按钮合同，页面仍不持session/Focus/CAS；只停止本人输入，没有resume意图。
test('明确人际源装配复用接续控件且本人停止只发送停止愿望', () => {
  const p = page(true, true); p.receive(fact());
  assert.match(p.elements.get('endpointSourceStatus').textContent, /人与人/);
  p.elements.get('endpointSourceTarget').value = 'device:p:target';
  p.click('endpointSourcePrepare');
  p.receive({kind: 'handoffState', state: 8, canEscalate: false, canAccept: false,
    canReject: false, canCancel: false, canHangup: true});
  p.click('humanInputStop');
  p.receive(fact('ready', {canCommit: false, canAbort: true}));
  p.click('endpointSourceCommit'); p.click('endpointSourceAbort');
  assert.deepEqual(p.sent, [{kind: 'endpointSourceControl', operation: 'prepare', target: 'device:p:target'},
    {kind: 'humanInputControl', operation: 'stop'}, {kind: 'endpointSourceControl', operation: 'abort'}]);
});
