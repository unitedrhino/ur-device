// 文件说明：原首因闭集严格消费者的形状/组合/UTF8/重复反例，不连接设备或平台。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { scopeFailureCategory } = require('./scope_failure_diagnostic.cjs');
// line 只产生固定协议诊断，不持实际设备会话或自由错误。
function line(layer = 'focusEngine', predicate = 'leaseExpired') {
  return `host_scope_failure={"kind":"scopeFailureDiagnostic","origin":"candidateGate","layer":"${layer}","predicate":"${predicate}","observation":1}\n`;
}
test('原poll首因保留固定层级与谓词', () => {
  assert.deepEqual(scopeFailureCategory(Buffer.from(line())), { kind: 'scopeFailureDiagnostic',
    origin: 'candidateGate', layer: 'focusEngine', predicate: 'leaseExpired', observation: 1 });
});
test('所有原HAL与driver固定组合可独立读取', () => {
  for (const [layer, predicate] of [['candidateHAL', 'clockInvalid'], ['authority', 'permitInvalid'],
    ['duplexDriver', 'inputUnconfirmed'], ['candidate', 'bindingMissing']])
    assert.equal(scopeFailureCategory(Buffer.from(line(layer, predicate))).predicate, predicate);
});
test('未开启与未知混合组合不推断首因', () => {
  assert.equal(scopeFailureCategory(Buffer.from('host_failure={}\n')), null);
  assert.equal(scopeFailureCategory(Buffer.from(line('authority', 'leaseExpired'))), null);
  assert.equal(scopeFailureCategory(Buffer.from(line('unknown', 'unknown'))), null);
});
test('重复首因、额外敏感字段与转义拒绝', () => {
  for (const value of [line() + line(), line().replace('"observation":1', '"observation":1,"sessionId":"private"'),
    line().replace('"observation":1', '"observation":1,"observation":1'),
    line().replace('leaseExpired', 'lease\\u0045xpired'), line().replace('1}', '2}')])
    assert.equal(scopeFailureCategory(Buffer.from(value)), null);
});
test('非UTF8及非Buffer或超预算不接受', () => {
  assert.equal(scopeFailureCategory(Buffer.concat([Buffer.from(line()), Buffer.from([0xff])])), null);
  assert.equal(scopeFailureCategory(line()), null);
  assert.equal(scopeFailureCategory(Buffer.alloc(8 * 1024 * 1024 + 1)), null);
});
