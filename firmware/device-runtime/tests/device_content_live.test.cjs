// 文件说明：附件真实验收入口的离线控制面及原字节观察回归；合成租约/模型/S3夹具不代表真实平台验收。
'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {PassThrough, Writable} = require('node:stream');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const https = require('node:https');
const {execFileSync} = require('node:child_process');
const {createHash} = require('node:crypto');
const {PacketTap, ContentLedger, startObservationPorts} = require('./device_content_observation.cjs');
const {run, validateConfig, displayKeywordsMatch} = require('./device_content_live.cjs');

// config是明确离线形状，真实入口不接受依赖替换或跳过原文件/最终binary SHA核对。
const config = {binary: '/fixture/device_text', binarySha256: 'a'.repeat(64),
  apiHost: '127.0.0.1', apiPort: 12001, s3Host: '127.0.0.1', s3Port: 12002,
  mqttHost: '127.0.0.1', mqttPort: 12003, tlsPort: 12004,
  tlsKey: '/fixture/key', tlsCert: '/fixture/cert', tlsCa: '/fixture/ca', fixtures: [
    {type: 'file', path: '/fixture/word.txt', sha256: 'b'.repeat(64), keywords: ['附件固定标记']},
    {type: 'image', path: '/fixture/red.png', sha256: 'c'.repeat(64), keywords: ['红色', '圆形']},
  ]};
const env = {UR_DEVICE_CONTENT_MODEL_LIVE: '1', UR_DEVICE_PRODUCT: 'fixture', UR_DEVICE_NAME: 'device', UR_DEVICE_SECRET: 'synthetic'};

// packet编码合成MQTT帧供fragment/合包/边界回归，不触发设备认证或平台API。
function packet(topic, body, qos = 0) {
  const text = Buffer.from(JSON.stringify(body)), name = Buffer.from(topic);
  const head = Buffer.alloc(2); head.writeUInt16BE(name.length);
  const payload = Buffer.concat([head, name, ...(qos ? [Buffer.from([0, 7])] : []), text]);
  const remaining = []; let length = payload.length;
  do { const byte = length % 128; length = Math.floor(length / 128); remaining.push(byte | (length ? 128 : 0)); } while (length);
  return Buffer.concat([Buffer.from([0x30 | qos << 1, ...remaining]), payload]);
}
// establish投递同一次原连接/创建/选择；选中缺image/file必须失败，不能仅信CLI建立。
function establish(ledger, inputs = ['text', 'image', 'file']) {
  ledger.observe({direction: 'up', connect: true});
  ledger.observe({topic: ledger.topics.upSignal, body: {method: 'sessionCreate', msgToken: 'create'}});
  ledger.observe({topic: ledger.topics.downSignal, body: {method: 'sessionCreated', msgToken: 'create', code: 200,
    sessionId: 'private-session', data: {selected: {transport: 'none', inputModalities: inputs, outputModalities: ['text']}}}});
  ledger.observe({topic: ledger.topics.downSignal, body: {method: 'sessionEstablished', msgToken: 'event', sessionId: 'private-session', data: {state: 'established'}}});
}
// upload按真实公开字段形状制造离线元数据；合成SigV4仅观察器回归，不可用于真实S3。
function upload(ledger, item, mismatch = false, options = {}) {
  const mime = item.type === 'file' ? 'text/plain' : 'image/png';
  const request = {sessionId: ledger.session, requestId: 'request-' + item.type, name: 'fixture', mimeType: mime, size: 32, sha256: item.sha256};
  const signed = ledger.origin + '/oss/private/object-' + item.type + '?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=' + 'd'.repeat(64) + '&X-Amz-Credential=synthetic&X-Amz-Date=20261005T000000Z';
  const data = {contentRef: 'private-ref-' + item.type, type: item.type, mimeType: mime, status: 'uploading',
    uploadReceipt: 'private-receipt-' + item.type, uploadUrl: signed, expiresAt: Date.now() + 40000};
  ledger.metadata('prepare', request, {code: 200, data});
  if (options.prepareOnly) return data;
  ledger.put(new URL(signed).pathname + new URL(signed).search, 32, item.sha256, 200);
  ledger.metadata('complete', {sessionId: ledger.session, contentRef: data.contentRef,
    uploadReceipt: mismatch ? 'other-lease' : data.uploadReceipt}, {code: 200,
    data: {contentRef: data.contentRef, type: data.type, mimeType: mime, status: 'ready'}});
}
// submit要求ref/mime/token/input/response逐项关联，不能以任意成功ACK/终态凑通过。
function submit(ledger, item, override = {}) {
  const round = item.type, input = 'private-input-' + round, token = 'private-token-' + round, response = 'private-response-' + round;
  ledger.observe({topic: ledger.topics.upMessage, body: {method: 'inputSend', msgToken: token, sessionId: ledger.session,
    params: {inputId: input, outputModalities: ['text'], contents: [{type: item.type,
      contentRef: override.ref || ledger.current.ref, mimeType: ledger.current.mime}]}}});
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'requestAck', msgToken: override.token || token,
    sessionId: ledger.session, code: 200, data: {requestMethod: 'inputSend', result: {inputId: input, state: 'accepted'}}}});
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'respCreated', msgToken: 'event-created-' + round, sessionId: ledger.session,
    data: {respId: response, inputId: input}}});
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'respTextDelta', msgToken: 'event-text-' + round, sessionId: ledger.session,
    data: {respId: response, delta: item.keywords.join(' ')}}});
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'respDone', msgToken: 'event-done-' + round, sessionId: ledger.session,
    data: {respId: response, inputId: override.input || input, status: override.status || 'completed', audioState: 'none'}}});
}

test('MQTT每字节fragment及合包均保留原JSON/QoS1和中文', () => {
  const values = [], topic = '$thing/up/message/fixture/device';
  const first = packet(topic, {method: 'inputSend', msgToken: 'one', params: {text: '原中文'}}, 1);
  const tap = new PacketTap([topic], 'up', value => values.push(value));
  for (const byte of first) tap.push(Buffer.from([byte]));
  tap.push(Buffer.concat([first, first])); tap.finish();
  assert.equal(values.length, 3); assert.equal(values[0].body.params.text, '原中文');
});
test('QoS1同字节DUP只观察一次，换内容/未知原件DUP仍拒绝', () => {
  const values = [], topic = 'expected', original = packet(topic, {method: 'inputSend', msgToken: 'one'}, 1);
  const retransmit = Buffer.from(original); retransmit[0] |= 8;
  const tap = new PacketTap([topic], 'up', value => values.push(value));
  tap.push(original); tap.push(retransmit); tap.finish(); assert.equal(values.length, 1);
  const modified = packet(topic, {method: 'inputSend', msgToken: 'other'}, 1); modified[0] |= 8;
  assert.throws(() => tap.push(modified));
  assert.throws(() => new PacketTap([topic], 'up', () => {}).push(retransmit));
});
test('未选择Topic和原CONNECT密码不被观察回调消费', () => {
  const values = [], tap = new PacketTap(['expected'], 'up', value => values.push(value));
  const secret = Buffer.from('synthetic-password');
  tap.push(Buffer.concat([Buffer.from([0x10, secret.length]), secret, packet('other', {private: 'body'})]));
  tap.finish(); assert.deepEqual(values, [{direction: 'up', connect: true}]);
});
test('无关Topic的QoS2或二进制不污染选中Topic的严格观察', () => {
  const values = [], tap = new PacketTap(['expected'], 'up', value => values.push(value));
  const unrelated = packet('telemetry', {}, 2);
  unrelated[unrelated.length - 1] = 0xff;
  tap.push(unrelated);
  tap.push(packet('expected', {method: 'inputSend', msgToken: 'original'}, 1));
  tap.finish(); assert.equal(values.length, 1); assert.equal(values[0].topic, 'expected');
});
test('选中Topic的QoS1缺少有效原包标识不能被静默忽略', () => {
  const original = packet('expected', {}, 1);
  const start = original[1] < 128 ? 2 : 3;
  original.writeUInt16BE(0, start + 2 + Buffer.byteLength('expected'));
  assert.throws(() => new PacketTap(['expected'], 'up', () => {}).push(original),
    error => error.message === 'mqtt_observation_invalid');
});
for (const [name, bytes] of [
  ['四字节未终止', Buffer.from([0x30, 0xff, 0xff, 0xff, 0xff])],
  ['非最短RemainingLength', Buffer.from([0x30, 0x80, 0])],
  ['超包预算', Buffer.from([0x30, 0x81, 0x04])],
  ['QoS2', packet('expected', {}, 2)],
  ['空Topic', packet('', {})],
]) test('有界MQTT拒绝' + name + '且错误不含报文', () => {
  const tap = new PacketTap(['expected'], 'up', () => {}, {maxPacket: 512});
  assert.throws(() => tap.push(bytes), error => error.message === 'mqtt_observation_invalid');
});
test('半帧、累积预算、非法UTF8及回调异常均不能认作证据', () => {
  const partial = new PacketTap(['expected'], 'up', () => {}); partial.push(Buffer.from([0x30])); assert.throws(() => partial.finish());
  const budget = new PacketTap([], 'up', () => {}, {maxTotal: 2}); assert.throws(() => budget.push(Buffer.from([0xc0, 0, 0xc0, 0])));
  const invalid = packet('expected', {}); invalid[invalid.length - 1] = 0xff;
  assert.throws(() => new PacketTap(['expected'], 'up', () => {}).push(invalid));
  assert.throws(() => new PacketTap(['expected'], 'up', () => { throw Error('synthetic-private-secret'); }).push(packet('expected', {})),
    error => !error.message.includes('synthetic-private-secret'));
});
test('选中text-only子集或第二CONNECT不能冒充真实完整能力/同连接', () => {
  assert.throws(() => establish(new ContentLedger('fixture/device', 'https://127.0.0.1:12004'), ['text']));
  const ledger = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(ledger);
  assert.throws(() => ledger.observe({direction: 'up', connect: true}));
});
test('同session两附件全部关联；公共事实无token/ref/receipt/URL/正文', () => {
  const ledger = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(ledger);
  for (const item of config.fixtures) { ledger.begin(item.type, 32, item.sha256); upload(ledger, item); submit(ledger, item); }
  assert.ok(Object.values(ledger.facts()).every(Boolean));
  const report = JSON.stringify(ledger.facts()); for (const word of ['private-', 'synthetic', 'X-Amz', '附件固定标记']) assert.ok(!report.includes(word));
});

test('签名PUT被拒时保留原HTTP状态，但不能认证原字节上传完成或泄漏签名',()=>{
  const ledger=new ContentLedger('fixture/device','https://127.0.0.1:12004');establish(ledger);
  const item=config.fixtures[0];ledger.begin(item.type,32,item.sha256);upload(ledger,item,false,{prepareOnly:true});
  assert.throws(()=>ledger.put(ledger.current.url,32,item.sha256,403));
  const facts=ledger.facts();assert.equal(facts.observedPutHTTPStatus,403);assert.equal(facts.signedPutBytesVerified,false);
  const report=JSON.stringify(facts);for(const word of ['private-','synthetic','X-Amz','receipt'])assert.ok(!report.includes(word));
});
test('ACK/respCreated/respDone原同token同内容重投不重开轮次，异内容拒绝', () => {
  const ledger = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(ledger);
  const item = config.fixtures[0]; ledger.begin(item.type, 32, item.sha256); upload(ledger, item); submit(ledger, item);
  const ack = {topic: ledger.topics.downMessage, body: {method: 'requestAck', msgToken: 'private-token-file',
    sessionId: ledger.session, code: 200, data: {requestMethod: 'inputSend', result: {inputId: 'private-input-file', state: 'accepted'}}}};
  ledger.observe(ack);
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'respCreated', msgToken: 'event-created-file',
    sessionId: ledger.session, data: {respId: 'private-response-file', inputId: 'private-input-file'}}});
  ledger.observe({topic: ledger.topics.downMessage, body: {method: 'respDone', msgToken: 'event-done-file', sessionId: ledger.session,
    data: {respId: 'private-response-file', inputId: 'private-input-file', status: 'completed', audioState: 'none'}}});
  assert.ok(ledger.current.done); assert.equal(ledger.count, 1);
  assert.throws(() => ledger.observe({...ack, body: {...ack.body, code: 403}}));
});
for (const mode of ['receipt', 'ref', 'token', 'input', 'status']) test('真实关联边界拒绝错' + mode, () => {
  const ledger = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(ledger);
  ledger.begin('file', 32, config.fixtures[0].sha256);
  if (mode === 'receipt') assert.throws(() => upload(ledger, config.fixtures[0], true));
  else { upload(ledger, config.fixtures[0]); assert.throws(() => submit(ledger, config.fixtures[0], {[mode]: mode === 'status' ? 'failed' : 'different'})); }
});
test('缺PUT、坏Hash、非签名URL及旧Complete均在各自实际关联条件拒绝', () => {
  const ledger = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(ledger);
  ledger.begin('file', 32, config.fixtures[0].sha256); upload(ledger, config.fixtures[0]); submit(ledger, config.fixtures[0]);
  ledger.begin('image', 32, config.fixtures[1].sha256);
  assert.throws(() => ledger.metadata('complete', {sessionId: ledger.session}, {code: 200}));
  const imageGrant = upload(ledger, config.fixtures[1], false, {prepareOnly: true});
  assert.throws(() => ledger.put(ledger.current.url, 32, 'f'.repeat(64), 200));
  assert.equal(ledger.current.put, false); // 不是靠重复PUT前置条件制造假的hash拒绝。
  assert.throws(() => ledger.metadata('complete', {sessionId: ledger.session, contentRef: imageGrant.contentRef,
    uploadReceipt: imageGrant.uploadReceipt}, {code: 200, data: {contentRef: imageGrant.contentRef,
    type: 'image', mimeType: 'image/png', status: 'ready'}}));
  assert.equal(ledger.current.ready, false);
  const fresh = new ContentLedger('fixture/device', 'https://127.0.0.1:12004'); establish(fresh); fresh.begin('file', 32, config.fixtures[0].sha256);
  assert.throws(() => fresh.metadata('prepare', {sessionId: fresh.session, size: 32, sha256: config.fixtures[0].sha256, requestId: 'r', mimeType: 'text/plain'},
    {code: 200, data: {type: 'file', mimeType: 'text/plain', status: 'uploading', contentRef: 'ref', uploadReceipt: 'receipt', expiresAt: Date.now() + 10000, uploadUrl: 'http://untrusted.invalid/file'}}));
});
test('配置仅允许完整image/file两个语料与固定回环、最终产物摘要', () => {
  assert.equal(validateConfig(config, false).fixtures.length, 2);
  for (const candidate of [{...config, mqttHost: 'remote.invalid'}, {...config, binarySha256: ''},
    {...config, fixtures: [config.fixtures[0]]}, {...config, extra: true}, {...config, tlsPort: config.apiPort}])
    assert.throws(() => validateConfig(candidate, false));
});

// fixture仅模拟本次原child，不执行模型；每个上传阶段仍经同一ledger关联，CLI alone不能过验收。
function fixture(mode = 'normal') {
  let ledger, child, released = false, portsReleased = false, round = 0;
  const startPorts = async (_, value) => { ledger = value; return {mqttPort: 12005, close: async () => { portsReleased = true; return true; }}; };
  const launch = () => {
    child = new EventEmitter(); child.stdout = new PassThrough(); child.stderr = new PassThrough();
    const emit = value => child.stdout.write(JSON.stringify(value) + '\n');
    const close = code => { if (released) return; released = true; child.stdout.end(); child.stderr.end(); child.emit('close', code); };
    child.kill = () => { queueMicrotask(() => close(143)); return true; };
    child.stdin = new Writable({write(bytes, _, complete) {
      try {
        const line = bytes.toString('utf8').trim();
        if (line === '/quit') { emit({sessionEnded: true, completedRounds: 2}); complete(); queueMicrotask(() => close(mode === 'badExit' ? 1 : 0)); return; }
        const item = config.fixtures[round]; assert.equal(line, '/' + item.type + ' ' + item.path); ++round;
        if (mode !== 'cliOnly') { upload(ledger, item, mode === 'wrongReceipt'); submit(ledger, item); }
        emit({inputAccepted: true, round});
        const original = item.keywords.join(' '); // 原效果字符必须保留，夹具仅模拟现有显示清理的空白。
        const text = Buffer.from(mode === 'wrongEffect' ? '公开无关文本' : mode === 'displaySpacing'
          ? [...original].join(' \t\r\n') : original);
        child.stderr.write(text.subarray(0, 2)); child.stderr.write(text.subarray(2));
        emit({replyCompleted: true, round}); if (mode === 'duplicateDone') emit({replyCompleted: true, round});
        complete();
      } catch { complete(Error('fixed_fixture_rejected')); }
    }});
    queueMicrotask(() => { establish(ledger); emit({sessionEstablished: true, transport: 'none'}); });
    return child;
  };
  return {startPorts, spawn: launch, config, timeoutMs: 120, facts: () => ({released, portsReleased, round})};
}
test('未显式允许收费或MQTT原启TLS时不建端口、不降级', async () => {
  let starts = 0;
  for (const candidate of [{...env, UR_DEVICE_CONTENT_MODEL_LIVE: '0'}, {...env, UR_MQTT_CA: '/actual/ca'}])
    await assert.rejects(run(candidate, {config, startPorts: () => { ++starts; }}));
  assert.equal(starts, 0);
});
test('同一原child两轮须三阶段、typed与实际效果同时成立且报告脱敏', async () => {
  const deps = fixture(); const value = await run(env, deps);
  assert.equal(value.evidence, 'offline-control-fixture'); assert.equal(value.rounds.length, 2);
  assert.equal(value.ownedProcessReleased, true); assert.equal(value.ownedPortsReleased, true);
  assert.ok(value.rounds.every(item => item.keywordMatched && item.originalReplyCompleted && item.signedPutBytesVerified));
  for (const word of ['private-', 'fixture/device', 'X-Amz', ...config.fixtures.flatMap(item => item.keywords)])
    assert.ok(!JSON.stringify(value).includes(word));
  assert.deepEqual(deps.facts(), {released: true, portsReleased: true, round: 2});
});
test('显示清理插入ASCII空白不应误判原附件口令缺失，仍须原两轮完成',async()=>{
  const deps=fixture('displaySpacing');const value=await run(env,deps);
  assert.equal(value.rounds.length,2);assert.ok(value.rounds.every(item=>item.keywordMatched&&item.originalReplyCompleted));
  assert.deepEqual(deps.facts(),{released:true,portsReleased:true,round:2});
});
test('显示空白归一不能吞错字、缺字、乱序、标点、大小写或Unicode伪装',()=>{
  const words=['TASK143ATTACHMENTCYPRESS271828'];
  assert.equal(displayKeywordsMatch('TASK143 ATTACHMENT\tCYPRESS\r\n271828',words),true);
  for(const text of ['TASK143ATTACHMENTCYPRESS271829','TASK143ATTACHMENTCYPRESS27182',
    'ATTACHMENTTASK143CYPRESS271828','TASK143_ATTACHMENT_CYPRESS_271828',
    'task143ATTACHMENTCYPRESS271828','TASK143\u00a0ATTACHMENTCYPRESS271828',
    'TASK143\u200bATTACHMENTCYPRESS271828'])assert.equal(displayKeywordsMatch(text,words),false);
});
for (const mode of ['cliOnly', 'wrongReceipt', 'wrongEffect', 'duplicateDone', 'badExit'])
  test('拒绝' + mode + '且保留固定失败阶段和原资源释放事实', async () => {
    const deps = fixture(mode);
    await assert.rejects(run(env, deps), error => {
      assert.equal(error.facts.failed, true); assert.equal(error.facts.ownedProcessReleased, true);
      assert.equal(error.facts.ownedPortsReleased, true); assert.equal(error.facts.evidence, 'offline-control-fixture');
      assert.ok(!JSON.stringify(error.facts).includes('private-')); return true;
    });
    assert.equal(deps.facts().released, true); assert.equal(deps.facts().portsReleased, true);
  });
test('观察端口启动失败固定报告且不启动设备；观察原child失败也需清理', async () => {
  let launched = false;
  await assert.rejects(run(env, {config, startPorts: async () => { throw Error('synthetic-secret'); }, spawn: () => { launched = true; }}),
    error => error.facts.phase === 'observationStartup' && !JSON.stringify(error.facts).includes('synthetic-secret'));
  assert.equal(launched, false);
  const deps = fixture(); await assert.rejects(run(env, {...deps, onSpawn: () => { throw Error('synthetic-secret'); }}));
  assert.equal(deps.facts().released, true); assert.equal(deps.facts().portsReleased, true);
});

// listen仅为离线网络转发回归分配回环端口；全部新socket/server在本用例关闭，不调用业务/OSS/模型。
async function listen(server) {
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  return server.address().port;
}
// requestTLS启用实际CA校验，不以rejectUnauthorized=false兜底，返回值只供本地断言。
function requestTLS(port, ca, route, method, bytes, headers = {}) {
  return new Promise(resolve => {
    const request = https.request({host: '127.0.0.1', port, ca, path: route, method,
      headers: {...headers, 'content-length': bytes.length}}, response => {
      const parts = []; response.on('data', value => parts.push(value));
      response.once('end', () => resolve({status: response.statusCode, body: Buffer.concat(parts)}));
      response.once('error', () => resolve({closed: true}));
    });
    request.on('error', () => resolve({closed: true})); request.setTimeout(3000, () => request.destroy()); request.end(bytes);
  });
}
test('真实回环TLS/TCP转发仅验证原字节与故障清理，不冒称SDK或设备鉴权', async t => {
  const base = path.resolve(__dirname, '../../../.temp');
  const temp = fs.mkdtempSync(path.join(base, 'device-content-observation-')); fs.chmodSync(temp, 0o700);
  const key = path.join(temp, 'key.pem'), cert = path.join(temp, 'cert.pem');
  execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=IP:127.0.0.1', '-keyout', key, '-out', cert], {stdio: 'ignore', timeout: 10000});
  fs.chmodSync(key, 0o600); fs.chmodSync(cert, 0o600);
  t.after(() => fs.rmSync(temp, {recursive: true})); // 精确本用例mkdtemp，不回收其它任务文件。
  for (const mode of ['transparent', 'authorization', 'cookie', 'requestLimit', 'responseLimit', 'upstreamError', 'putAbort', 'mqttAbort']) {
    await t.test(mode, async () => {
      let apiSeen = false, putSeen = false, brokerBytes = Buffer.alloc(0); const sockets = new Set();
      const keep = socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); };
      const api = http.createServer(async (request, response) => {
        const parts = []; for await (const chunk of request) parts.push(chunk);
        const bytes = Buffer.concat(parts); apiSeen = true;
        assert.equal(request.headers.authorization, 'Basic synthetic');
        if (mode === 'responseLimit') { response.end('x'.repeat(20000)); return; }
        if (mode === 'upstreamError') { response.writeHead(503); response.end('synthetic-private-upstream'); return; }
        assert.deepEqual(bytes, metadataBytes);
        response.setHeader('content-type', 'application/json'); response.end(JSON.stringify({code: 200, data: prepared}));
      });
      const s3 = http.createServer(async (request, response) => {
        try {
          putSeen = true; const parts = []; for await (const chunk of request) parts.push(chunk);
          assert.equal(request.headers.host, '127.0.0.1:' + s3Port);
          assert.equal(request.url, signedPath.slice(4)); assert.ok(!request.headers.authorization && !request.headers.cookie);
          assert.deepEqual(Buffer.concat(parts), sourceBytes); response.end();
        } catch { response.destroy(); }
      });
      const broker = net.createServer(socket => { keep(socket); socket.on('data', bytes => { brokerBytes = Buffer.concat([brokerBytes, bytes]); socket.write(bytes); }); });
      api.on('connection', keep); s3.on('connection', keep);
      const apiPort = await listen(api), s3Port = await listen(s3), mqttPort = await listen(broker);
      const reservation = net.createServer(), tlsPort = await listen(reservation);
      await new Promise(resolve => reservation.close(resolve));
      const origin = 'https://127.0.0.1:' + tlsPort, ledger = new ContentLedger('fixture/device', origin);
      establish(ledger); const sourceBytes = Buffer.from('synthetic-original-private-bytes');
      const digest = createHash('sha256').update(sourceBytes).digest('hex'); ledger.begin('file', sourceBytes.length, digest);
      const signedPath = '/oss/private/synthetic%20path?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=' + 'e'.repeat(64) + '&X-Amz-Credential=synthetic%2Fdate&X-Amz-Date=20261005T000000Z';
      const prepared = {contentRef: 'private-ref', type: 'file', mimeType: 'text/plain', status: 'uploading',
        uploadReceipt: 'private-receipt', uploadUrl: origin + signedPath, expiresAt: Date.now() + 30000};
      const metadataBytes = Buffer.from(JSON.stringify({sessionId: ledger.session, requestId: 'private-request', name: 'fixture.txt',
        mimeType: 'text/plain', size: sourceBytes.length, sha256: digest}));
      let ports, portsReleased = false;
      try {
        ports = await startObservationPorts({...config, apiPort, s3Port, mqttPort, tlsPort, tlsKey: key, tlsCert: cert}, ledger);
        const ca = fs.readFileSync(cert);
        if (mode === 'mqttAbort') {
          const socket = net.connect(ports.mqttPort, '127.0.0.1'); await new Promise(resolve => socket.once('connect', resolve));
          socket.write(Buffer.from([0x30])); await new Promise(resolve => setTimeout(resolve, 20)); socket.destroy();
          await new Promise(resolve => setTimeout(resolve, 30)); assert.equal(ledger.failed, true);
        } else if (mode === 'putAbort') {
          ledger.metadata('prepare', JSON.parse(metadataBytes), {code: 200, data: prepared});
          const request = https.request({host: '127.0.0.1', port: tlsPort, ca, path: signedPath, method: 'PUT',
            headers: {'content-length': sourceBytes.length}}, response => response.resume());
          request.on('error', () => {}); request.write(sourceBytes.subarray(0, 2));
          await new Promise(resolve => setTimeout(resolve, 20)); request.destroy();
          await new Promise(resolve => setTimeout(resolve, 30)); assert.equal(ledger.failed, true); assert.equal(ledger.current.put, false);
        } else if (mode === 'authorization' || mode === 'cookie') {
          ledger.metadata('prepare', JSON.parse(metadataBytes), {code: 200, data: prepared});
          const value = await requestTLS(tlsPort, ca, signedPath, 'PUT', sourceBytes, {[mode]: 'synthetic-private'});
          assert.equal(value.status, 502); assert.equal(putSeen, false); assert.equal(ledger.failed, true);
        } else {
          const value = await requestTLS(tlsPort, ca, '/api/v1/things/device/edge/content/prepare', 'POST',
            mode === 'requestLimit' ? Buffer.from('x'.repeat(9000)) : metadataBytes, {authorization: 'Basic synthetic'});
          if (mode === 'transparent') {
            assert.equal(value.status, 200); assert.deepEqual(JSON.parse(value.body).data, prepared);
            assert.equal(apiSeen, true); assert.equal(ledger.current.prepared, true);
            const put = await requestTLS(tlsPort, ca, signedPath, 'PUT', sourceBytes);
            assert.equal(put.status, 200); assert.equal(putSeen, true); assert.equal(ledger.current.put, true);
            const bytes = packet(ledger.topics.upSignal, {method: 'capabilityReport', msgToken: 'transparent'});
            const socket = net.connect(ports.mqttPort, '127.0.0.1'); const echoed = [];
            await new Promise((resolve, reject) => {
              socket.once('connect', () => { for (const byte of bytes) socket.write(Buffer.from([byte])); });
              socket.on('data', chunk => { echoed.push(chunk); if (Buffer.concat(echoed).length >= bytes.length) resolve(); }); socket.on('error', reject);
            });
            assert.deepEqual(brokerBytes, bytes); assert.deepEqual(Buffer.concat(echoed), bytes);
            // 本回归没有完整模型轮次；关闭原socket应被记作未完成而非端云成功。
            socket.destroy();
          } else {
            assert.ok(value.status === 502 || value.closed); assert.equal(ledger.failed, true); assert.equal(ledger.current.prepared, false);
            if (mode === 'requestLimit') assert.equal(apiSeen, false);
          }
        }
      } finally {
        if (ports) portsReleased = await ports.close();
        const drained = Promise.all([...sockets].map(socket => new Promise(resolve => { socket.once('close', resolve); socket.destroy(); })));
        for (const server of [api, s3, broker]) await new Promise(resolve => server.close(resolve));
        await drained;
      }
      assert.equal(portsReleased, true);
      assert.equal(sockets.size, 0);
    });
  }
});
