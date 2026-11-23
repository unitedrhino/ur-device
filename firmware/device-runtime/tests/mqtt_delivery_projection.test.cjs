// 文件说明：MQTT投递固定日志投影的纯离线回归；不启动Broker、设备、模型或业务环境。
'use strict';
const {test} = require('node:test'), assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {limits, keys, enums, project, streams, attach} = require('./mqtt_delivery_projection.cjs');

// fixture 只使用生产固定12字段与合成安全计数，不含真实目标、会话、正文或凭据。
function fixture(changes = {}) {return {marker: 'UR_MQTT_DELIVERY_TRACE', stage: 'brokerAccepted',
  method: 'toolCallExecute', messageSeq: 1, messageSeqKnown: true, payloadBytes: 128, wireBytes: 0,
  targetMatched: true, socketWrites: 0, pubackReads: 0, pubackCompleted: 0, truncated: false, ...changes};}
// json 按真实go-zero默认logger包装，caller取prettyCaller末两段格式。
function json(value = fixture(), metadata = {}) {return JSON.stringify({'@timestamp': '2026-10-03T00:00:00.123Z',
  level: 'info', caller: 'hook/delivery_trace.go:77', content: JSON.stringify(value), ...metadata});}
// plain 按真实go-zero TAB正文及caller独立槽包装，不将正文内嵌caller当来源。
function plain(value = fixture()) {return '2026-10-03T00:00:00.123Z\tinfo\t' + JSON.stringify(value) +
  '\tcaller=hook/delivery_trace.go:77';}

test('真实go-zero两种来源包装投影固定12字段，裸正文不能作为日志来源', () => {
  assert.deepEqual(project(json()), fixture());
  assert.deepEqual(project(plain()), fixture());
  assert.equal(project(JSON.stringify(fixture())), null);
});

test('所有生产method与stage枚举接受，ACK早于socket计数不被推断为业务送达', () => {
  for (const [key, values] of Object.entries(enums)) for (const value of values)
    assert.equal(project(json(fixture({[key]: value})))[key], value);
  const early = fixture({stage: 'pubackCompleted', socketWrites: 0, pubackReads: 1, pubackCompleted: 1});
  assert.deepEqual(project(json(early)), early);
  assert.ok(Object.isFrozen(limits)); assert.ok(Object.isFrozen(keys));
  for (const values of Object.values(enums)) assert.ok(Object.isFrozen(values));
});
test('明确真实来源槽，错误caller/级别/裸JSON/内嵌来源/递归content不能冒充诊断', () => {
  for (const metadata of [{caller: 'other.go:77'}, {caller: 'delivery_trace.go:77'},
    {caller: 'hook/delivery_trace.go:0'}, {caller: 'hook/delivery_trace.go:77 private'},
    {caller: null}, {level: 'error'}, {level: 'info private'}, {'@timestamp': 'free'}, {'@timestamp': {}}])
    assert.equal(project(json(fixture(), metadata)), null);
  for (const line of [JSON.stringify(fixture()), JSON.stringify({content: JSON.stringify(fixture())}),
    json(JSON.parse(json())), 'private ' + plain(), plain().replace('caller=', 'notCaller='),
    plain() + '\tcaller=hook/delivery_trace.go:78', plain().replace('\tinfo\t', '\terror\t'),
    plain().replace('toolCallExecute', 'toolCallExecute caller=hook/delivery_trace.go:77')]) assert.equal(project(line), null);
  assert.deepEqual(project(plain().replace('\tinfo\t', '\t\u001b[34m  info  \u001b[0m\t')), fixture());
  assert.deepEqual(project('2026/10/03 00:00:00 ' + plain()), fixture());
});
test('固定12key重建，不泄漏外层正文身份及错误，缺失或额外内层字段整条拒绝', () => {
  const projected = project(json(fixture(), {identity: 'private-identity', headers: {authorization: 'private-key'},
    body: 'private-body', error: 'private-error', ignored: ['中文😀']}));
  assert.deepEqual(Object.keys(projected), keys); assert.deepEqual(projected, fixture());
  assert.ok(!JSON.stringify(projected).includes('private'));
  for (const key of keys) {const item = fixture(); delete item[key]; assert.equal(project(json(item)), null);}
  for (const key of ['sessionId', 'deviceId', 'userId', 'msgToken', 'payload', 'topic', 'error', 'headers'])
    assert.equal(project(json(fixture({[key]: 'private'}))), null);
});
test('重复key和转义别名在正文及logger包装任意深度都拒绝', () => {
  const raw = JSON.stringify(fixture());
  for (const prefix of ['"marker":"UR_MQTT_DELIVERY_TRACE",', '"\\u006darker":"UR_MQTT_DELIVERY_TRACE",',
    '"messageSeq":1,"messageSeq":2,']) assert.equal(project(json().replace(JSON.stringify(raw), JSON.stringify(raw.replace('{', '{' + prefix)))), null);
  const content = JSON.stringify(raw);
  assert.equal(project('{"@timestamp":"2026-10-03T00:00:00.123Z","level":"info","caller":"other.go:1","caller":"hook/delivery_trace.go:77","content":' + content + '}'), null);
  assert.equal(project(json().replace('"level":"info"', '"level":"info","\\u006cevel":"info"')), null);
  assert.equal(project(json(fixture(), {ignored: JSON.parse('{"x":1}')}).replace('"x":1', '"x":1,"x":2')), null);
});
test('安全序号已知/未知及计数上下界精确，unsafe/小数/类型转换一概拒绝', () => {
  for (const value of [1, Number.MAX_SAFE_INTEGER]) assert.equal(project(json(fixture({messageSeq: value}))).messageSeq, value);
  assert.deepEqual(project(json(fixture({messageSeq: 0, messageSeqKnown: false}))), fixture({messageSeq: 0, messageSeqKnown: false}));
  for (const change of [{messageSeq: 0}, {messageSeq: 1, messageSeqKnown: false}, {messageSeq: Number.MAX_SAFE_INTEGER + 1},
    {messageSeq: -1}, {messageSeq: '1'}, {messageSeq: 0.5}, {messageSeq: null}]) assert.equal(project(json(fixture(change))), null);
  for (const [key, maximum] of Object.entries({payloadBytes: 65536, wireBytes: 131072, socketWrites: 256, pubackReads: 256, pubackCompleted: 256})) {
    for (const value of [0, maximum]) assert.equal(project(json(fixture({stage: 'socketWritten', [key]: value})))[key], value);
    for (const value of [-1, maximum + 1, 0.5, '1', null, {}, [], true, Infinity, NaN])
      assert.equal(project(json(fixture({stage: 'socketWritten', [key]: value}))), null);
  }
  for (const key of ['messageSeqKnown', 'targetMatched', 'truncated']) for (const value of [1, 'true', null, {}])
    assert.equal(project(json(fixture({[key]: value}))), null);
  for (const key of ['stage', 'method', 'marker']) for (const value of ['private', '', 1, null, {}])
    assert.equal(project(json(fixture({[key]: value}))), null);
  assert.equal(project(json(fixture({wireBytes: 1}))), null);
});
test('字节16KiB/深度32有界，超限和未成对surrogate含转义均不接纳', () => {
  const minimum = Buffer.byteLength(json(fixture(), {padding: ''}));
  const room = limits.lineBytes - minimum;
  const line = json(fixture(), {padding: '中'.repeat(Math.floor(room / 3)) + 'x'.repeat(room % 3)});
  assert.equal(Buffer.byteLength(line), limits.lineBytes); assert.deepEqual(project(line), fixture());
  assert.equal(project(line + ' '), null);
  assert.equal(project(json(fixture(), {ignored: JSON.parse('['.repeat(33) + '0' + ']'.repeat(33))})), null);
  for (const value of ['\ud800', '\udc00', 'x\ud800x']) assert.equal(project(json(fixture(), {ignored: value})), null);
  assert.equal(project(json() + '\ud800'), null);
});
test('实际child环境缺省关闭不借父开关，关闭不读chunk/调用转换或增加监听', () => {
  const prior = process.env.UR_MQTT_DELIVERY_TRACE;
  try {
    process.env.UR_MQTT_DELIVERY_TRACE = '1';
    for (const env of [undefined, null, {}, {UR_MQTT_DELIVERY_TRACE: ''}, {UR_MQTT_DELIVERY_TRACE: '0'},
      {UR_MQTT_DELIVERY_TRACE: 'true'}, {UR_MQTT_DELIVERY_TRACE: '01'}, {UR_MQTT_DELIVERY_TRACE: 1}]) {
      const observer = streams(() => assert.fail('defaultoff'), env);
      observer.stdout({toString() {assert.fail('coercion');}}); observer.stderr(json() + '\n');
      observer.end('stdout'); assert.equal(observer.statistics().acceptedRecords, 0);
      assert.equal(observer.statistics().streams.stdout.totalBytes, 0); assert.equal(observer.statistics().enabled, false);
    }
  } finally {if (prior === undefined) delete process.env.UR_MQTT_DELIVERY_TRACE; else process.env.UR_MQTT_DELIVERY_TRACE = prior;}
});
test('独立双流逐字节UTF8分片还原，尾行只在真实end而不在close收录', () => {
  const values = [], observer = streams(value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  const out = Buffer.from(json(fixture(), {ignored: '中文😀'})), err = Buffer.from(plain());
  for (const byte of out) observer.stdout(Buffer.from([byte]));
  observer.stderr(err); assert.equal(values.length, 0); observer.end('stdout');
  assert.equal(values.length, 1); assert.equal(values[0].stream, 'stdout');
  observer.close(); observer.end('stderr'); assert.equal(values.length, 1);
  assert.equal(observer.statistics().streams.stderr.ended, false);
  assert.ok(!JSON.stringify(values).includes('中文'));
});
test('大chunk按4096诊断分片而不修改原Buffer，合法surrogate对跨内部字符串分片保留', () => {
  const values = [], observer = streams(value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  const body = json(fixture(), {ignored: 'x'.repeat(650) + '😀'.repeat(200)});
  const raw = Buffer.from((body + '\n').repeat(30)), before = Buffer.from(raw);
  observer.stdout(raw); observer.stderr((body + '\n').repeat(30));
  assert.equal(values.length, 60); assert.deepEqual(raw, before);
  assert.equal(observer.statistics().streams.stdout.totalBytes, raw.length);
  assert.equal(observer.statistics().streams.stderr.totalBytes, raw.length);
});
test('非法UTF8/截断尾字节/BOM/孤立surrogate不能被替换后冒充事实，下一真实行恢复', () => {
  const values = [], observer = streams(value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  const valid = Buffer.from(json()), at = valid.indexOf(Buffer.from('UR_MQTT'));
  observer.stdout(Buffer.concat([valid.subarray(0, at), Buffer.from([0xff]), valid.subarray(at), Buffer.from('\n')]));
  observer.stdout(Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), valid, Buffer.from('\n')]));
  observer.stderr('\ud800'); observer.stderr('\n');
  observer.stdout(valid); observer.stdout(Buffer.from([0xf0])); observer.end('stdout');
  assert.equal(values.length, 0); observer.stderr(json() + '\n'); assert.equal(values.length, 1);
  assert.equal(observer.statistics().truncated, true); assert.ok(observer.statistics().streams.stdout.invalidChunks >= 2);
});
test('16KiB超长整行丢弃至真实换行，不把尾部及另一流拼出合法记录', () => {
  const values = [], observer = streams(value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  observer.stdout('x'.repeat(limits.lineBytes + 1)); observer.stdout(json() + '\n');
  assert.equal(values.length, 0); observer.stderr(json() + '\n'); observer.stdout(json() + '\n');
  assert.equal(values.length, 2); assert.equal(observer.statistics().streams.stdout.oversizedLines, 1);
  assert.equal(observer.statistics().truncated, true);
});
test('两流合计256记录预算显式截断，额外合法记录只记缺失不制造原业务无消息', () => {
  const values = [], observer = streams(value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  observer.stdout((json() + '\n').repeat(limits.records)); observer.stderr(json() + '\n');
  assert.equal(values.length, limits.records); assert.equal(observer.statistics().discardedRecords, 1);
  assert.equal(observer.statistics().acceptedRecords, limits.records); assert.equal(observer.statistics().truncated, true);
});
test('sink同步/异步失败隔离且有界统计，不输出自由error，不等待原业务流', async () => {
  let calls = 0; const observer = streams(() => {if (++calls === 1) throw Error('private-error');
    return calls === 2 ? Promise.reject(Error('private-error')) : Promise.resolve();}, {UR_MQTT_DELIVERY_TRACE: '1'});
  const raw = Buffer.from((json() + '\n').repeat(3)), before = Buffer.from(raw);
  assert.doesNotThrow(() => observer.stdout(raw)); assert.deepEqual(raw, before);
  assert.equal(observer.statistics().pendingSinks, 2); await Promise.resolve();
  assert.equal(observer.statistics().sinkFailures, 2); assert.equal(observer.statistics().emittedRecords, 1);
  assert.equal(observer.statistics().pendingSinks, 0); assert.ok(!JSON.stringify(observer.statistics()).includes('private'));
});
test('未知chunk不转换，end/close幂等及sink重入close不会重复投影尾行', () => {
  const values = [], observer = streams(value => {values.push(value); observer.close();}, {UR_MQTT_DELIVERY_TRACE: '1'});
  observer.stderr({toString() {assert.fail('coercion');}}); observer.stderr('\n');
  observer.stdout((json() + '\n').repeat(2)); observer.close(); observer.end('stdout'); observer.end('unknown');
  assert.equal(values.length, 1); assert.equal(observer.statistics().streams.stderr.invalidChunks, 1);
  assert.equal(observer.statistics().invalidStreams, 1);
});

// childFixture 保留原转发/退出监听与原chunk，任何控制pipe或child方法都令测试失败。
function childFixture() {
  const child = new EventEmitter(), original = {stdout: [], stderr: [], exitCodes: []};
  child.spawnargs = ['synthetic-backend']; child.exitCode = null;
  child.kill = () => assert.fail('must-not-kill');
  for (const name of ['stdout', 'stderr']) {
    child[name] = new EventEmitter(); child[name].on('data', bytes => original[name].push(bytes));
    child[name].pause = child[name].resume = child[name].destroy = () => assert.fail('must-not-change-pipe');
  }
  child.on('close', code => original.exitCodes.push(code));
  return {child, original};
}
test('attach关闭不注册监听，显式实际child开关才启用且不消费父开关', () => {
  for (const env of [undefined, null, {}, {UR_MQTT_DELIVERY_TRACE: '0'}, {UR_MQTT_DELIVERY_TRACE: true}]) {
    const {child, original} = childFixture();
    const observer = attach(child, () => assert.fail('disabled'), env);
    const bytes = Buffer.from(json() + '\n'); child.stdout.emit('data', bytes);
    assert.equal(observer.statistics().enabled, false); assert.equal(child.stdout.listenerCount('data'), 1);
    assert.equal(child.stdout.listenerCount('end'), 0); assert.equal(original.stdout[0], bytes);
  }
});
test('attach仅真实两pipe的end收尾，child.close不假排空且原对象字节/退出状态不变', () => {
  const {child, original} = childFixture(), values = [], args = child.spawnargs;
  const observer = attach(child, value => values.push(value), {UR_MQTT_DELIVERY_TRACE: '1'});
  const out = Buffer.from(json()), err = Buffer.from(plain());
  child.stdout.emit('data', out); child.stderr.emit('data', err); child.exitCode = 7; child.emit('close', 7);
  assert.equal(values.length, 0); assert.equal(child.listenerCount('close'), 1);
  child.stderr.emit('end'); child.stdout.emit('end'); child.stdout.emit('end');
  assert.equal(values.length, 2); assert.equal(observer.statistics().acceptedRecords, 2);
  assert.equal(child.spawnargs, args); assert.equal(original.stdout[0], out); assert.equal(original.stderr[0], err);
  assert.deepEqual(original.exitCodes, [7]); assert.equal(child.exitCode, 7);
});
