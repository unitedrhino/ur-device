// 文件说明：固定工具trace安全投影的纯离线回归；包含实际go-zero包装、脱敏、重复字段、分片与失败隔离，不调用模型/资源。
'use strict';
const {test} = require('node:test'), assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {limits, keys, enums, project, streams, attach} = require('./tool_trace_projection.cjs');

// fixture 固定15字段生产合同；测试文本不含真实身份、正文或凭据。
function fixture(changes = {}) {return {marker: 'ai_tool_trace', traceSeq: 1, stage: 'boundTools',
  known: true, truncated: false, inviteMediaPlaybackPresent: true, controlOutputFocusPresent: true,
  toolsCount: 2, accumulatedCalls: 0, parsedCount: 0, toolFragments: 0, mergeErrors: 0,
  toolChoice: 'absent', finishReason: 'absent', end: 'absent', ...changes};}
// json 包装实际go-zero JSON content并附自由metadata，metadata只用于确认不会进入输出。
function json(value = fixture(), metadata = {}) {return JSON.stringify({'@timestamp': '2026-10-03T00:00:00.123Z',
  level: 'info', caller: '/private/source.go:9', trace: 'private-id', content: JSON.stringify(value), ...metadata});}
// plain 构造go-zero既有时间戳/TAB级别/TAB正文及自由尾部，输出只允许正文的固定schema。
function plain(value = fixture(), level = 'info') {return '2026-10-03T00:00:00.123Z\t' + level + '\t' +
  JSON.stringify(value) + '\tcaller=/private\theader=must-not-escape';}

test('纯payload及真实go-zero JSON/plain投影精确15key，不泄漏外层metadata/ID/header', () => {
  const expected = fixture();
  for (const line of [JSON.stringify(expected), json(expected, {headers: {authorization: 'must-not-escape'},
    ignored: [{url: 'must-not-escape'}]}), plain(), plain(expected, '\u001b[31m  error  \u001b[0m'),
    '2026/10/03 00:00:00 ' + plain()]) {
    const value = project(line); assert.deepEqual(value, expected); assert.deepEqual(Object.keys(value), keys);
    assert.ok(!JSON.stringify(value).includes('private')); assert.ok(!JSON.stringify(value).includes('must-not-escape'));
  }
});
test('所有声明stage/choice/finish/end枚举与数值边界接受，但值不推导业务成功', () => {
  for (const [key, values] of Object.entries(enums)) for (const value of values)
    assert.equal(project(json(fixture({[key]: value})))[key], value);
  const boundaries = {traceSeq: [1, 256], toolsCount: [0, 128], accumulatedCalls: [0, 128],
    parsedCount: [0, 128], toolFragments: [0, 65536], mergeErrors: [0, 65536]};
  for (const [key, values] of Object.entries(boundaries)) for (const value of values)
    assert.equal(project(json(fixture({[key]: value})))[key], value);
  assert.deepEqual(project(json(fixture({known: false, truncated: true, toolsCount: 0,
    inviteMediaPlaybackPresent: false, controlOutputFocusPresent: false}))), fixture({known: false,
    truncated: true, toolsCount: 0, inviteMediaPlaybackPresent: false, controlOutputFocusPresent: false}));
});
test('导出白名单只读，字节边界精确16KiB而不按字符宽放日志', () => {
  assert.ok(Object.isFrozen(keys)); assert.ok(Object.isFrozen(limits));
  for (const values of Object.values(enums)) {assert.ok(Object.isFrozen(values)); assert.throws(() => values.push('free'));}
  const value = JSON.parse(json()), minimum = Buffer.byteLength(JSON.stringify({...value, padding: ''}));
  const size = limits.lineBytes - minimum, count = Math.floor(size / 3);
  const line = JSON.stringify({...value, padding: '中'.repeat(count) + 'x'.repeat(size - count * 3)});
  assert.equal(Buffer.byteLength(line), limits.lineBytes); assert.deepEqual(project(line), fixture());
  assert.equal(project(line + ' '), null);
});
test('内层任意额外/缺失/多项私有字段整体拒绝，不选择性省略后冒充标准trace', () => {
  for (const key of keys) {const value = fixture(); delete value[key]; assert.equal(project(json(value)), null);}
  for (const change of [{id: 'private'}, {headers: {}}, {body: 'private'}, {sessionId: 'private'},
    {name: 'things_iot_mcp_invite_media_playback'}, {id: 1, url: 'private', error: 'private'},
    {extra: true}, {'__proto__': null, free: true}]) assert.equal(project(json(fixture(change))), null);
  assert.equal(project(json(JSON.parse('{"__proto__":"private"}'))), null);
});
test('重复JSON key及转义别名拒绝，包括内层重复、外层content覆盖与metadata重复', () => {
  const raw = JSON.stringify(fixture());
  for (const payload of [raw.replace('{', '{"marker":"other",'), raw.replace('{', '{"traceSeq":2,'),
    raw.replace('{', '{"\\u006darker":"ai_tool_trace",'), raw.replace('{', '{"known":true,"known":false,')]) {
    assert.equal(project(payload), null); assert.equal(project(JSON.stringify({content: payload})), null);
  }
  const body = JSON.stringify(raw);
  assert.equal(project('{"content":"private","content":' + body + '}'), null);
  assert.equal(project('{"content":' + body + ',"metadata":{"id":1,"id":2}}'), null);
  assert.deepEqual(project(json(fixture(), {message: 'quoted : { "id": "not-an-object" }'})), fixture());
});
test('类型/负数/非finite/unsafeint/枚举自由值严格拒绝，不执行自由对象转换', () => {
  for (const key of ['traceSeq', 'toolsCount', 'accumulatedCalls', 'parsedCount', 'toolFragments', 'mergeErrors'])
    for (const value of [-1, 0.5, Number.MAX_SAFE_INTEGER + 1, 65537, '1', null, true, {}, [], Infinity, NaN])
      assert.equal(project(json(fixture({[key]: value}))), null);
  for (const key of ['known', 'truncated', 'inviteMediaPlaybackPresent', 'controlOutputFocusPresent'])
    for (const value of ['true', 1, null, {}]) assert.equal(project(json(fixture({[key]: value}))), null);
  for (const key of ['stage', 'toolChoice', 'finishReason', 'end'])
    for (const value of ['private', '', 1, null, {}, []]) assert.equal(project(json(fixture({[key]: value}))), null);
  assert.equal(project(JSON.stringify(fixture()).replace('"traceSeq":1', '"traceSeq":1e999')), null);
  assert.equal(project(json(fixture({marker: 'other'}))), null);
});
test('自由plain prefix/非法level/正文ANSI/异常正文/超长及深层wrapper不能成为trace', () => {
  for (const line of ['private ' + plain(), plain(fixture(), 'private'), 'not-json', '{}', '[]',
    plain().replace('\t{"marker"', '\t\u001b[31m{"marker"'), json().replace('ai_tool_trace', 'other'),
    'x'.repeat(limits.lineBytes + 1), JSON.stringify({content: fixture()})]) assert.equal(project(line), null);
  assert.equal(project(JSON.stringify({content: JSON.stringify(fixture()), metadata: JSON.parse('['.repeat(33) + '0' + ']'.repeat(33))})), null);
});
test('缺省/非法optin完全关闭；只精确UR_AI_TOOL_TRACE=1启用，关闭仍不改原输入', () => {
  for (const value of [undefined, '', '0', 'true', '01', 1, true]) {
    let calls = 0; const observer = streams(() => calls++, {UR_AI_TOOL_TRACE: value}), bytes = Buffer.from(json() + '\n');
    const before = Buffer.from(bytes); observer.stdout(bytes); observer.stderr(bytes); observer.close();
    assert.deepEqual(bytes, before); assert.equal(calls, 0); assert.equal(observer.statistics().enabled, false);
    assert.equal(observer.statistics().streams.stdout.totalBytes, 0);
  }
  const values = [], observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout(json() + '\n'); assert.equal(values.length, 1); assert.equal(observer.statistics().enabled, true);
});
test('两流独立且UTF8跨chunk可还原，另一流自由尾部不拼成合法trace', () => {
  const values = [], observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  const bytes = Buffer.from(json(fixture({stage: 'httpRequest', toolChoice: 'required'}), {caller: '私密中文😀'}));
  observer.stdout('private-prefix');
  for (const byte of bytes) observer.stderr(Buffer.from([byte]));
  observer.stdout('\n'); assert.equal(values.length, 0); observer.end('stderr');
  assert.equal(values.length, 1); assert.equal(values[0].stream, 'stderr');
  assert.equal(values[0].facts.toolChoice, 'required'); assert.ok(!JSON.stringify(values).includes('私密'));
  observer.close(); assert.equal(observer.statistics().streams.stderr.ended, true);
});
test('4096解码chunk只是诊断内存界限，不截断原大Buffer/字符串或合法多行记录', () => {
  const raw = Buffer.from((json() + '\n').repeat(40)), before = Buffer.from(raw), values = [];
  const observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout(raw); observer.stderr(raw.toString()); assert.equal(values.length, 80);
  assert.deepEqual(raw, before); assert.equal(observer.statistics().streams.stdout.totalBytes, raw.length);
  assert.equal(observer.statistics().projectionFailures, 0); observer.close();
});
test('16KiB超长整行丢至真实换行，不把尾部/第二流/后续合法行错误提升', () => {
  const values = [], observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout(Buffer.from('x'.repeat(limits.lineBytes + 1))); observer.stdout(json() + '\n');
  assert.equal(values.length, 0); observer.stderr(json() + '\n'); observer.stdout(json() + '\n'); observer.close();
  assert.equal(values.length, 2); const stats = observer.statistics();
  assert.equal(stats.streams.stdout.oversizedLines, 1); assert.equal(stats.streams.stdout.lineCount, 2);
  assert.equal(stats.truncated, true); assert.equal(stats.streams.stderr.oversizedLines, 0);
});
test('两流共用2048总cap及显式丢弃统计，不把后段缺记录当成模型没有工具', () => {
  const values = [], observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout((json() + '\n').repeat(limits.records)); assert.equal(values.length, limits.records);
  assert.equal(observer.statistics().truncated, false);
  observer.stderr(json(fixture({stage: 'parsedTools', parsedCount: 1})) + '\n'); observer.stdout(json() + '\n');
  const stats = observer.statistics(); assert.equal(stats.truncated, true); assert.equal(stats.discardedRecords, 2);
  assert.equal(stats.acceptedRecords, limits.records); assert.equal(stats.emittedRecords, limits.records);
});
test('sink异常隔离，下一条诊断仍可收录，原业务buffer不变且缺失明确', () => {
  let calls = 0; const values = [], observer = streams(value => {if (++calls === 1) throw Error('private'); values.push(value);}, {UR_AI_TOOL_TRACE: '1'});
  const bytes = Buffer.from(json() + '\n'), before = Buffer.from(bytes);
  assert.doesNotThrow(() => observer.stdout(bytes)); observer.stderr(bytes); assert.deepEqual(bytes, before);
  assert.equal(values.length, 1); const stats = observer.statistics(); assert.equal(stats.sinkFailures, 1);
  assert.equal(stats.emittedRecords, 1); assert.equal(stats.acceptedRecords, 2); assert.equal(stats.truncated, true);
  assert.ok(!JSON.stringify(stats).includes('private'));
  assert.doesNotThrow(() => streams(null, {UR_AI_TOOL_TRACE: '1'}).stdout(bytes));
});
test('异步sink拒绝不成为未处理异常，pending/完成/失败均明确且不等待原流', async () => {
  let calls = 0; const observer = streams(() => ++calls === 1 ? Promise.reject(Error('private')) : Promise.resolve(), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout(json() + '\n'); observer.stderr(json() + '\n'); observer.close();
  assert.equal(observer.statistics().pendingSinks, 2); assert.equal(observer.statistics().emittedRecords, 0);
  await Promise.resolve(); const stats = observer.statistics(); assert.equal(stats.pendingSinks, 0);
  assert.equal(stats.emittedRecords, 1); assert.equal(stats.sinkFailures, 1); assert.equal(stats.truncated, true);
  assert.ok(!JSON.stringify(stats).includes('private'));
});
test('sink同步关闭只停止诊断且不重入重复投影，原多行Buffer保持原样', () => {
  const values = [], observer = streams(value => {values.push(value); observer.close();}, {UR_AI_TOOL_TRACE: '1'});
  const bytes = Buffer.from((json() + '\n').repeat(2)), before = Buffer.from(bytes);
  observer.stdout(bytes); assert.equal(values.length, 1); assert.deepEqual(bytes, before);
  assert.equal(observer.statistics().streams.stdout.ended, true); assert.equal(observer.statistics().acceptedRecords, 1);
});
test('未知chunk不调用valueOf/toString，关闭/重复end幂等且结束后不再接收事实', () => {
  const values = [], observer = streams(value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  observer.stdout({toString() {assert.fail('must-not-coerce');}, valueOf() {assert.fail('must-not-coerce');}});
  observer.stderr(json()); assert.equal(values.length, 0); observer.end('unknown'); observer.close(); observer.close();
  assert.equal(values.length, 1); observer.stdout(json() + '\n'); observer.stderr(json() + '\n');
  assert.equal(values.length, 1); const stats = observer.statistics(); assert.equal(stats.invalidStreams, 1);
  assert.equal(stats.streams.stdout.invalidChunks, 1); assert.equal(stats.streams.stdout.ended, true);
  assert.equal(stats.streams.stderr.ended, true); assert.equal(stats.truncated, true);
});

// childFixture 用纯事件模拟已有child及双pipe，原业务监听保留原chunk对象，不创建实际进程。
function childFixture() {
  const child = new EventEmitter(), original = {stdout: [], stderr: [], exitCodes: []};
  child.spawnargs = ['synthetic-backend']; child.exitCode = null;
  child.kill = () => assert.fail('诊断不得杀child');
  for (const name of ['stdout', 'stderr']) {
    child[name] = new EventEmitter(); child[name].on('data', bytes => original[name].push(bytes));
    child[name].pause = child[name].resume = child[name].destroy = () => assert.fail('诊断不得改变原pipe');
  }
  child.on('close', code => original.exitCodes.push(code));
  return {child, original};
}
test('attach只用实际child env；父开关开启不能启用缺省/非法child开关，关闭不加业务监听', () => {
  const prior = process.env.UR_AI_TOOL_TRACE;
  try {
    process.env.UR_AI_TOOL_TRACE = '1';
    for (const env of [undefined, null, {}, {UR_AI_TOOL_TRACE: '0'}, {UR_AI_TOOL_TRACE: 'true'},
      {UR_AI_TOOL_TRACE: '01'}, {UR_AI_TOOL_TRACE: 1}, {UR_AI_TOOL_TRACE: true}]) {
      const {child, original} = childFixture(), bytes = Buffer.from(json() + '\n');
      const observer = attach(child, () => assert.fail('关闭不得emit'), env);
      assert.equal(observer.statistics().enabled, false);
      for (const name of ['stdout', 'stderr']) {
        assert.equal(child[name].listenerCount('data'), 1); assert.equal(child[name].listenerCount('end'), 0);
        child[name].emit('data', bytes); assert.equal(original[name][0], bytes);
      }
      child.emit('close', 7); assert.deepEqual(original.exitCodes, [7]);
      assert.equal(child.listenerCount('close'), 1); assert.equal(observer.statistics().acceptedRecords, 0);
    }
    process.env.UR_AI_TOOL_TRACE = '0';
    const {child} = childFixture(), values = [], observer = attach(child, value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
    child.stdout.emit('data', Buffer.from(json() + '\n'));
    assert.equal(observer.statistics().enabled, true); assert.equal(values.length, 1);
  } finally {
    if (prior === undefined) delete process.env.UR_AI_TOOL_TRACE; else process.env.UR_AI_TOOL_TRACE = prior;
  }
});
test('attach独立双流UTF8分片和真实end排空末行；child close不伪end，原字节/参数/退出码不变', () => {
  const {child, original} = childFixture(), values = [], spawnargs = child.spawnargs;
  const observer = attach(child, value => values.push(value), {UR_AI_TOOL_TRACE: '1'});
  const out = Buffer.from(json(fixture({stage: 'boundTools'}), {ignored: '中文'}));
  const err = Buffer.from(json(fixture({stage: 'streamResult', end: 'eof'})));
  for (let index = 0; index < out.length; index++) {
    const chunk = out.subarray(index, index + 1); child.stdout.emit('data', chunk);
    assert.equal(original.stdout.at(-1), chunk);
  }
  child.stderr.emit('data', err.subarray(0, 17)); child.stderr.emit('data', err.subarray(17));
  child.exitCode = 9; child.emit('close', 9);
  assert.equal(values.length, 0); assert.equal(child.listenerCount('close'), 1);
  for (const name of ['stdout', 'stderr']) assert.equal(observer.statistics().streams[name].ended, false);
  child.stderr.emit('end'); assert.equal(values.length, 1); assert.equal(values[0].stream, 'stderr');
  assert.equal(observer.statistics().streams.stdout.ended, false); child.stdout.emit('end');
  assert.equal(values.length, 2); assert.equal(values[1].stream, 'stdout');
  child.stdout.emit('end'); child.stderr.emit('end'); child.stdout.emit('data', out);
  assert.equal(values.length, 2); assert.equal(child.spawnargs, spawnargs); assert.equal(child.exitCode, 9);
  assert.deepEqual(original.exitCodes, [9]); assert.deepEqual(Buffer.concat(original.stderr), err);
  const stats = observer.statistics(); assert.equal(stats.acceptedRecords, 2); assert.equal(stats.emittedRecords, 2);
  assert.equal(stats.streams.stdout.totalBytes, out.length); assert.equal(stats.streams.stderr.totalBytes, err.length);
  assert.equal(stats.streams.stdout.ended, true); assert.equal(stats.streams.stderr.ended, true);
});
test('attach sink同步/异步失败仅统计取证缺失，不改变业务chunk或原child退出', async () => {
  const {child, original} = childFixture(), bytes = Buffer.from(json() + '\n'); let calls = 0;
  const observer = attach(child, () => {
    if (++calls === 1) throw Error('private');
    return Promise.reject(Error('private'));
  }, {UR_AI_TOOL_TRACE: '1'});
  assert.doesNotThrow(() => child.stdout.emit('data', bytes)); child.stderr.emit('data', bytes);
  child.stdout.emit('end'); child.stderr.emit('end'); child.emit('close', 3);
  assert.equal(observer.statistics().pendingSinks, 1); await Promise.resolve();
  const stats = observer.statistics(); assert.equal(stats.sinkFailures, 2); assert.equal(stats.pendingSinks, 0);
  assert.equal(stats.truncated, true); assert.equal(stats.acceptedRecords, 2); assert.equal(stats.emittedRecords, 0);
  assert.equal(original.stdout[0], bytes); assert.equal(original.stderr[0], bytes);
  assert.deepEqual(original.exitCodes, [3]); assert.ok(!JSON.stringify(stats).includes('private'));
});
