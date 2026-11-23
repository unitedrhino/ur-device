// 文件说明：媒体分支脱敏投影回归，使用原双pipe模拟器，不申请真实模型或产生业务副作用。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {keys, enums, limits, project, attach} = require('./media_branch_projection.cjs');
// fact只构造生产十三字段的合成事实，不保存业务标识、恢复引用或上游正文。
function fact(extra = {}) { return {marker: 'ai_media_branch', callSeq: 1, tool: 'recover_media_playback_operation',
  branch: 'resolvedOriginal', target: 'parent', referencePresent: true, referenceValid: true,
  referenceSameOrigin: true, dispatched: true, upstreamObserved: true, upstreamRPCError: false,
  upstreamIsError: false, outcome: 'returned', ...extra}; }
// wrapped使用真实go-zero content包装；private字段故意在外层确认不会被输出。
function wrapped(value = fact()) { return JSON.stringify({content: JSON.stringify(value), caller: 'private-source', token: 'private-token'}); }
// pipes用两个独立事件流验证实际旁路接线，不消费或终止原进程。
function pipes() { return {stdout: new EventEmitter(), stderr: new EventEmitter()}; }
test('原三种日志包装都只输出固定十三字段，不泄漏自由metadata', () => {
  for (const value of [JSON.stringify(fact()), wrapped(), '2026-10-03T00:00:00Z\tinfo\t' + JSON.stringify(fact()) + '\tcaller=private-source']) {
    assert.deepEqual(project(value), fact());
    assert.deepEqual(Object.keys(project(value)), keys);
    assert.ok(!JSON.stringify(project(value)).includes('private'));
  }
});
test('声明枚举和原有界序号都接受，但不等同媒体执行成功', () => {
  for (const [key, values] of Object.entries(enums)) for (const value of values)
    assert.equal(project(wrapped(fact({[key]: value})))[key], value);
  for (const callSeq of [1, 1024]) assert.equal(project(wrapped(fact({callSeq}))).callSeq, callSeq);
  assert.ok(Object.isFrozen(keys)); assert.ok(Object.isFrozen(enums)); assert.ok(Object.isFrozen(limits));
});
test('旧工具trace以及多余私有字段、缺字段、错误类型与自由枚举均拒绝', () => {
  for (const key of keys) { const value = fact(); delete value[key]; assert.equal(project(wrapped(value)), null); }
  for (const extra of [{sessionId: 'private'}, {recoveryRef: 'private'}, {url: 'private'}, {error: 'private'},
    {marker: 'ai_tool_trace'}, {callSeq: 0}, {callSeq: 1025}, {callSeq: 0.5}, {callSeq: '1'},
    {branch: 'private'}, {tool: 'external_tool'}, {target: 'child-id'}, {outcome: 'completed'}])
    assert.equal(project(wrapped(fact(extra))), null);
  for (const key of keys.slice(5, 12)) for (const value of [1, 'true', null, [], {}])
    assert.equal(project(wrapped(fact({[key]: value}))), null);
});
test('重复字段与转义别名、超长、自由plain prefix和深层metadata均拒绝', () => {
  const original = JSON.stringify(fact());
  for (const raw of [original.replace('{', '{"callSeq":2,'), original.replace('{', '{"\\u006darker":"other",'),
    'private ' + original, 'x'.repeat(limits.lineBytes + 1), '{"content":"x","content":' + JSON.stringify(original) + '}'])
    assert.equal(project(raw), null);
  assert.equal(project(JSON.stringify({content: original, nested: JSON.parse('['.repeat(33) + '0' + ']'.repeat(33))})), null);
});
test('默认和非精确开关不订阅原双pipe，不借父环境或修改原输入', () => {
  for (const value of [undefined, '0', '', true, 1, 'true']) {
    const child = pipes(), observer = attach(child, () => assert.fail('disabled'), {UR_AI_TOOL_TRACE: value});
    assert.equal(child.stdout.listenerCount('data'), 0); assert.equal(child.stderr.listenerCount('data'), 0);
    assert.equal(observer.statistics().enabled, false); observer.close();
  }
});
test('双流独立跨UTF8分片恢复，实际end才提交末尾，不能拼接另一流', () => {
  const child = pipes(), values = [], observer = attach(child, value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  const raw = Buffer.from(JSON.stringify({content: JSON.stringify(fact()), private: '中文😀'}));
  child.stdout.emit('data', Buffer.from('private-prefix'));
  for (const byte of raw) child.stderr.emit('data', Buffer.from([byte]));
  assert.equal(values.length, 0); child.stderr.emit('end'); child.stdout.emit('data', Buffer.from('\n'));
  assert.equal(values.length, 1); assert.equal(values[0].stream, 'stderr'); assert.deepEqual(values[0].facts, fact());
  assert.equal(observer.statistics().ended.stderr, true); assert.equal(observer.statistics().ended.stdout, false);
  child.stdout.emit('end'); assert.equal(observer.statistics().ended.stdout, true);
});
test('超长丢至实际换行而不保留尾部，原大buffer和后继合法行不变', () => {
  const child = pipes(), values = [], observer = attach(child, value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  const raw = Buffer.from('x'.repeat(limits.lineBytes + 1) + wrapped() + '\n' + wrapped() + '\n');
  const before = Buffer.from(raw); child.stdout.emit('data', raw);
  assert.deepEqual(raw, before); assert.equal(values.length, 1);
  assert.equal(observer.statistics().oversizedLines, 1); assert.equal(observer.statistics().truncated, true);
});
test('总预算及丢弃明确，不把缺少后段诊断当没有恢复调用', () => {
  const child = pipes(), values = [], observer = attach(child, value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  child.stdout.emit('data', Buffer.from((wrapped() + '\n').repeat(limits.records + 1)));
  assert.equal(values.length, limits.records); assert.equal(observer.statistics().discarded, 1);
  assert.equal(observer.statistics().truncated, true);
});
test('sink同步或异步失败仅记缺失，未知chunk不执行转换，原流继续', async () => {
  const child = pipes(); let calls = 0;
  const observer = attach(child, () => { if (++calls === 1) throw Error('private'); return Promise.reject(Error('private')); }, {UR_AI_TOOL_TRACE: '1'});
  const raw = Buffer.from(wrapped() + '\n');
  assert.doesNotThrow(() => child.stdout.emit('data', raw)); child.stderr.emit('data', raw);
  child.stdout.emit('data', {toString() { assert.fail('conversion'); }});
  assert.equal(observer.statistics().pendingSinks, 1); await Promise.resolve();
  assert.equal(observer.statistics().sinkFailures, 2); assert.equal(observer.statistics().pendingSinks, 0);
  assert.equal(observer.statistics().invalidChunks, 1); assert.ok(!JSON.stringify(observer.statistics()).includes('private'));
});
test('sink重入关闭及重复end不重放事实，终态后不复活订阅', () => {
  const child = pipes(), values = [];
  const observer = attach(child, value => { values.push(value); observer.close(); }, {UR_AI_TOOL_TRACE: '1'});
  child.stdout.emit('data', Buffer.from((wrapped() + '\n').repeat(2)));
  child.stdout.emit('end'); child.stderr.emit('end'); observer.close();
  child.stderr.emit('data', Buffer.from(wrapped() + '\n'));
  assert.equal(values.length, 1); assert.equal(observer.statistics().accepted, 1);
});
