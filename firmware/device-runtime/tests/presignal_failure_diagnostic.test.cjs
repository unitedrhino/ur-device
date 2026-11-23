// 文件说明：宿主UDP预信令观察的格式/隐私反例，不运行模型或媒体，也不替代真实UDP探针。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { presignalDiagnostics } = require('./presignal_failure_diagnostic.cjs');
// line沿C++固定六字段和顺序生成合成观察，字段不携带标识或凭据。
const line = (observation = 1, received = 169, evicted = 153, retained = 16, complete = true) =>
  `host_udp_presignal={"kind":"udpPresignalDiagnostic","observation":${observation},"received":${received},"evicted":${evicted},"retained":${retained},"complete":${complete}}\n`;
test('default off and non-buffer cannot invent observations', () => {
  assert.equal(presignalDiagnostics(Buffer.from('normal\n')), null);
  assert.equal(presignalDiagnostics(line()), null);
});
test('exact complete selected reply tuple and monotonic snapshots are immutable', () => {
  const records = presignalDiagnostics(Buffer.from(line() + line(2, 0, 0, 0)));
  assert.deepEqual(records, [
    { kind: 'udpPresignalDiagnostic', observation: 1, received: 169, evicted: 153, retained: 16, complete: true },
    { kind: 'udpPresignalDiagnostic', observation: 2, received: 0, evicted: 0, retained: 0, complete: true },
  ]);
  assert.ok(Object.isFrozen(records) && records.every(Object.isFrozen));
});
test('overflow stays incomplete rather than a fabricated zero-receive success', () => {
  assert.equal(presignalDiagnostics(Buffer.from(line(1, 0, 0, 1, false)))[0].complete, false);
  assert.equal(presignalDiagnostics(Buffer.from(line(1, 0xffffffff, 0xffffffff, 16, false)))[0].received, 0xffffffff);
  assert.equal(presignalDiagnostics(Buffer.from(line(1, 0, 0, 1, true))), null);
});
test('illegal numbers, forged completeness, duplicate observation and free fields are rejected', () => {
  const values = [line(0), line(-1), line(1.5), line(0x100000000), line(1, 169, 170), line(1, 169, 153, 17),
    line(1, 168), line() + line(), line(2) + line(1), line().replace('169', '1e3'),
    line().replace('"complete":true', '"complete":true,"nonce":"forbidden"'), line().replace('true', '"true"')];
  for (const value of values) assert.equal(presignalDiagnostics(Buffer.from(value)), null);
});
test('invalid UTF8 and bounded output cannot become sanitized success', () => {
  assert.equal(presignalDiagnostics(Buffer.concat([Buffer.from(line()), Buffer.from([0xff])])), null);
  assert.equal(presignalDiagnostics(Buffer.alloc(8 * 1024 * 1024 + 1)), null);
  assert.equal(presignalDiagnostics(Buffer.from('host_udp_presignal=' + 'x'.repeat(513))), null);
});
