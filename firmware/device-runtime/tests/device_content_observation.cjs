// 文件说明：真实附件验收的有界只读观察器；所有身份、租约、签名地址和正文只驻留内存，不进入报告。
'use strict';
const assert = require('node:assert/strict');
const net = require('node:net');
const http = require('node:http');
const https = require('node:https');
const fs = require('node:fs');
const {createHash} = require('node:crypto');
const {TextDecoder} = require('node:util');
const {performance} = require('node:perf_hooks');

// PacketTap观察原MQTT 3.1.1帧，不生成CONNECT、不重写原件；畸形、超预算或回调失败均关闭本次观察。
class PacketTap {
  constructor(topics, direction, onPacket, limits = {}) {
    this.topics = new Set(topics); this.direction = direction; this.onPacket = onPacket;
    this.maxPacket = limits.maxPacket || 65536; this.maxTotal = limits.maxTotal || 4194304;
    this.buffer = Buffer.alloc(0); this.total = 0; this.failed = false;
    this.qosPackets = new Map(); // 只留有界原QoS1摘要，不保存payload或认证字段。
  }
  // push接原socket分片，逐帧校验RemainingLength，不以分片或合并改变64KiB帧预算。
  push(bytes) {
    try {
      assert.ok(!this.failed && Buffer.isBuffer(bytes));
      this.total += bytes.length; assert.ok(this.total <= this.maxTotal);
      // 每轮最多保留一帧；先处理已有分片，避免一次合并多个合法帧造成伪超限。
      let offset = 0;
      while (offset < bytes.length) {
        const take = Math.min(bytes.length - offset, this.maxPacket + 5 - this.buffer.length);
        assert.ok(take > 0);
        this.buffer = Buffer.concat([this.buffer, bytes.subarray(offset, offset + take)]); offset += take;
        while (this.buffer.length >= 2) {
          let length = 0, multiplier = 1, count = 0, complete = false;
          for (; count < 4; ++count) {
            if (this.buffer.length < count + 2) break;
            const value = this.buffer[count + 1]; length += (value & 127) * multiplier;
            assert.ok(length <= this.maxPacket);
            if (!(value & 128)) { complete = true; ++count; break; }
            multiplier *= 128;
          }
          assert.ok(count < 4 || complete, 'invalid_remaining_length');
          if (!complete) break;
          // 非最短编码不能与原严格编解码生成的帧混用。
          assert.ok(count === 1 || length >= 128 ** (count - 1));
          const size = count + 1 + length;
          if (this.buffer.length < size) break;
          this.packet(this.buffer.subarray(0, size), count + 1);
          this.buffer = this.buffer.subarray(size);
        }
      }
    } catch (e) { this.failed = true; if (process.env.UR_DEVICE_CONTENT_DEBUG) console.error('[tap-push] ' + (e && e.message)); throw Error('mqtt_observation_invalid'); }
  }
  // packet只消费指定Topic的PUBLISH；CONNECT只统计次数，密码与其它报文从不解析或保留。
  packet(frame, start) {
    const type = frame[0] >> 4;
    if (type === 1) { this.onPacket({direction: this.direction, connect: true}); return; }
    if (type !== 3) return;
    // 转发路径包含无关遥测；先解析合法MQTT 3.1.1 Topic，再仅对选中Topic核验载荷与QoS。
    // 非法Topic不属于可忽略业务，选中Topic的QoS2也不能静默漏记后宣称证据完整。
    const qos = (frame[0] >> 1) & 3;
    assert.ok(frame.length >= start + 2);
    const size = frame.readUInt16BE(start); let position = start + 2;
    assert.ok(size > 0 && position + size <= frame.length);
    const decoder = new TextDecoder('utf-8', {fatal: true});
    let topic;
    try { topic = decoder.decode(frame.subarray(position, position + size)); } catch { return; }
    position += size;
    if (!this.topics.has(topic)) return;
    assert.ok(qos <= 1);
    let packetId;
    if (qos === 1) { assert.ok(position + 2 <= frame.length && frame.readUInt16BE(position) > 0); packetId = frame.readUInt16BE(position); position += 2; }
    if (qos === 1) {
      const key = topic + ':' + packetId;
      const digest = createHash('sha256').update(frame.subarray(start)).digest('hex');
      if (frame[0] & 8) {
        // 没有原件摘要的DUP不能被认证为合法重投；证据不完整须明确失败，不能静默跳过。
        const seen = this.qosPackets.get(key);
        assert.ok(seen !== undefined);
        assert.equal(seen, digest);
        return;
      }
      if (!this.qosPackets.has(key) && this.qosPackets.size >= 256) this.qosPackets.delete(this.qosPackets.keys().next().value);
      this.qosPackets.set(key, digest);
    }
    const body = JSON.parse(decoder.decode(frame.subarray(position)));
    assert.ok(body && typeof body === 'object' && !Array.isArray(body));
    this.onPacket({direction: this.direction, topic, body});
  }
  // finish不把断连后的半帧认作完整观测。
  finish() { assert.ok(!this.failed && this.buffer.length === 0, 'mqtt_observation_incomplete'); }
}

// ContentLedger关联正式session/HTTP租约/typed input/回复，公共facts只能输出固定布尔和数值。
class ContentLedger {
  constructor(identity, origin) {
    this.topics = {upSignal: '$thing/up/signaling/' + identity, downSignal: '$thing/down/signaling/' + identity,
      upMessage: '$thing/up/message/' + identity, downMessage: '$thing/down/message/' + identity};
    this.origin = origin; this.connects = 0; this.session = ''; this.createdToken = '';
    this.established = false; this.failed = false; this.current = null; this.count = 0;
    this.events = new Map(); // 原事件重投按method/token及完整内容摘要核验；不同内容不得借同token吞掉。
  }
  // fail保存固定失败事实，不保留底层异常或敏感参数。
  fail(reason) { this.failed = true; if (process.env.UR_DEVICE_CONTENT_DEBUG && reason) console.error(`[ledger-fail] ${reason}`); }
  // begin固定本轮实际原件哈希和长度，上一轮必须完成，不能借旧引用证明新上传。
  begin(type, size, hash) {
    assert.ok(this.established && !this.failed && this.count < 2);
    assert.ok(!this.current || this.current.done);
    assert.ok(['image', 'file'].includes(type) && Number.isSafeInteger(size) && size > 0);
    assert.match(hash, /^[a-f0-9]{64}$/);
    this.current = {type, size, hash, prepared: false, put: false, ready: false,
      submitted: false, accepted: false, done: false, receipt: '', ref: '', url: '', input: '', token: '', response: ''};
    ++this.count;
  }
  // observe按已鉴权Topic和原请求关联选中能力；其它业务Topic不是本验收的证据。
  observe(packet) {
    if (packet.connect) { assert.equal(packet.direction, 'up'); assert.equal(++this.connects, 1); return; }
    const value = packet.body;
    assert.ok(typeof value.method === 'string' && typeof value.msgToken === 'string' && value.msgToken.length <= 128);
    if (['sessionCreate', 'sessionCreated', 'sessionEstablished', 'inputSend', 'respCreated', 'respDone'].includes(value.method) ||
        (value.method === 'requestAck' && value.data?.requestMethod === 'inputSend')) {
      const key = packet.topic + ':' + value.method + ':' + value.msgToken;
      const digest = createHash('sha256').update(JSON.stringify(value)).digest('hex');
      if (this.events.has(key)) { assert.equal(this.events.get(key), digest); return; }
      assert.ok(this.events.size < 64); this.events.set(key, digest);
    }
    if (packet.topic === this.topics.upSignal && value.method === 'sessionCreate') {
      if (process.env.UR_DEVICE_CONTENT_DEBUG) this.lastCreateSent = {tokenLen: value.msgToken.length};
      assert.equal(this.createdToken, ''); this.createdToken = value.msgToken; return;
    }
    if (packet.topic === this.topics.downSignal && value.method === 'sessionCreated') {
      if (process.env.UR_DEVICE_CONTENT_DEBUG) this.lastCreated = {code: value.code, msgTokenMatch: value.msgToken === this.createdToken};
      assert.equal(value.msgToken, this.createdToken); assert.equal(value.code, 200);
      assert.equal(this.session, ''); assert.ok(typeof value.sessionId === 'string' && value.sessionId.length <= 128);
      const selected = value.data.selected;
      assert.equal(selected.transport, 'none');
      assert.deepEqual([...selected.inputModalities].sort(), ['file', 'image', 'text']);
      assert.deepEqual(selected.outputModalities, ['text']); this.session = value.sessionId; return;
    }
    if (packet.topic === this.topics.downSignal && value.method === 'sessionEstablished') {
      assert.equal(value.sessionId, this.session); assert.equal(value.data.state, 'established');
      assert.equal(this.established, false); this.established = true; return;
    }
    if (!this.current) return;
    const item = this.current;
    if (packet.topic === this.topics.upMessage && value.method === 'inputSend') {
      assert.equal(value.sessionId, this.session); assert.ok(item.ready && !item.submitted);
      assert.deepEqual(value.params.outputModalities, ['text']); assert.equal(value.params.contents.length, 1);
      assert.deepEqual(value.params.contents[0], {type: item.type, contentRef: item.ref, mimeType: item.mime});
      assert.ok(typeof value.params.inputId === 'string' && value.params.inputId.length <= 128);
      item.token = value.msgToken; item.input = value.params.inputId; item.submitted = true;
      item.inputSentAt = performance.now();
    } else if (packet.topic === this.topics.downMessage && value.method === 'requestAck' && value.data.requestMethod === 'inputSend') {
      assert.equal(value.sessionId, this.session); assert.equal(value.msgToken, item.token);
      assert.ok(item.submitted && !item.accepted); assert.equal(value.code, 200);
      assert.equal(value.data.result.inputId, item.input); assert.equal(value.data.result.state, 'accepted'); item.accepted = true;
    } else if (packet.topic === this.topics.downMessage && value.method === 'respCreated') {
      assert.ok(item.submitted && !item.response); assert.equal(value.sessionId, this.session);
      assert.equal(value.data.inputId, item.input); assert.ok(typeof value.data.respId === 'string' && value.data.respId.length <= 128);
      item.response = value.data.respId;
    } else if (packet.topic === this.topics.downMessage && ['respTextDelta', 'respTextDone'].includes(value.method)) {
      // 首网络文本以原response关联，不用无轮次的TUI管道迟到换行/上一轮正文推断TTFT。
      if (value.sessionId !== this.session || value.data.respId !== item.response) return;
      const text = value.method === 'respTextDelta' ? value.data.delta : value.data.text;
      assert.ok(typeof text === 'string' && text.length <= 65536);
      if (text.trim() && item.firstNetworkTextAt === undefined) item.firstNetworkTextAt = performance.now();
    } else if (packet.topic === this.topics.downMessage && value.method === 'respDone') {
      if (process.env.UR_DEVICE_CONTENT_DEBUG) this.lastRespDone = {status: value.data?.status, audioState: value.data?.audioState}; // 调试证据仅状态码，先于断言记录。
      assert.ok(item.accepted && item.response && !item.done); assert.equal(value.sessionId, this.session);
      assert.equal(value.data.respId, item.response); assert.equal(value.data.inputId, item.input);
      assert.equal(value.data.status, 'completed'); assert.equal(value.data.audioState, 'none'); item.done = true;
    }
  }
  // metadata仅核验已完成原HTTP交互；返回值不写回或伪造，错误应答不能变成prepared/ready事实。
  metadata(endpoint, request, response) {
    const item = this.current; assert.ok(item && !this.failed);
    assert.equal(request.sessionId, this.session); assert.equal(response.code, 200);
    const data = response.data;
    if (endpoint === 'prepare') {
      assert.ok(!item.prepared); assert.equal(request.size, item.size); assert.equal(request.sha256, item.hash);
      assert.ok(typeof request.requestId === 'string' && request.requestId.length <= 64);
      assert.equal(data.type, item.type); assert.equal(data.mimeType, request.mimeType); assert.equal(data.status, 'uploading');
      assert.ok(typeof data.contentRef === 'string' && data.contentRef.length <= 64);
      assert.ok(typeof data.uploadReceipt === 'string' && data.uploadReceipt.length <= 4096);
      assert.ok(Number.isSafeInteger(data.expiresAt) && data.expiresAt > Date.now());
      const signed = new URL(data.uploadUrl);
      assert.equal(signed.origin, this.origin); assert.ok(signed.pathname.startsWith('/oss/'));
      assert.equal(signed.searchParams.get('X-Amz-Algorithm'), 'AWS4-HMAC-SHA256');
      assert.match(signed.searchParams.get('X-Amz-Signature') || '', /^[a-f0-9]{64}$/);
      assert.ok(signed.searchParams.has('X-Amz-Credential') && signed.searchParams.has('X-Amz-Date'));
      item.ref = data.contentRef; item.receipt = data.uploadReceipt; item.mime = data.mimeType;
      item.url = signed.pathname + signed.search; item.prepared = true;
      item.preparedAt = performance.now();
    } else {
      assert.equal(endpoint, 'complete'); assert.ok(item.prepared && item.put && !item.ready);
      assert.equal(request.contentRef, item.ref); assert.equal(request.uploadReceipt, item.receipt);
      assert.deepEqual(data, {contentRef: item.ref, type: item.type, mimeType: item.mime, status: 'ready'}); item.ready = true;
      item.readyAt = performance.now();
    }
  }
  // put检查原签名URL对应的真实网络字节；网络成功不替代Complete授权或模型消费。
  put(url, size, hash, status) {
    const item = this.current; assert.ok(item && item.prepared && !item.put);
    assert.equal(url, item.url); assert.equal(size, item.size); assert.equal(hash, item.hash);
    // 仅记录与原请求及原字节关联的合法状态码；拒绝响应仍不得成为上传成功事实。
    assert.ok(Number.isInteger(status) && status >= 100 && status <= 599);
    item.observedPutHTTPStatus = status;
    assert.ok(status >= 200 && status < 300); item.put = true; item.putAt = performance.now();
  }
  // facts不泄漏内部关联值、路径、签名Query或TUI正文。
  facts() {
    const item = this.current;
    const facts = {oneOriginalConnection: this.connects === 1, selectedImageAndFile: this.established,
      prepareVerified: !!item?.prepared, signedPutBytesVerified: !!item?.put, completeVerified: !!item?.ready,
      typedInputVerified: !!item?.submitted, originalInputAckVerified: !!item?.accepted,
      originalReplyCompleted: !!item?.done};
    if (item?.observedPutHTTPStatus !== undefined) facts.observedPutHTTPStatus = item.observedPutHTTPStatus;
    if (process.env.UR_DEVICE_CONTENT_DEBUG) { // 调试时输出信令方法名集合（不含token/正文），定位会话建立缺口。
      facts.signalingMethods = [...this.events.keys()].map(key => key.split(':')[1]);
      facts.createdTokenSeen = this.createdToken !== '';
      facts.lastCreateSent = this.lastCreateSent || null;
      facts.lastCreated = this.lastCreated || null;
      facts.lastRespDone = this.lastRespDone;
    }
    return facts;
  }
}

// startObservationPorts只启动本任务回环TCP/TLS转发；原认证、签名和body逐字节交给正式后端，不自产令牌。
async function startObservationPorts(config, ledger) {
  const sockets = new Set(); let connection = false, closed = false;
  const own = socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); };
  const mqtt = net.createServer(client => {
    own(client);
    if (connection || closed) { ledger.fail("second_client_or_closed"); client.destroy(); return; }
    connection = true;
    const upstream = net.connect(config.mqttPort, config.mqttHost); own(upstream);
    const tap = direction => new PacketTap(Object.values(ledger.topics), direction, value => ledger.observe(value));
    const up = tap('up'), down = tap('down');
    const relay = (source, destination, observer) => {
      source.on('data', bytes => {
        try { observer.push(bytes); }
        catch { ledger.fail("tap_parse"); source.destroy(); destination.destroy(); return; }
        if (!destination.write(bytes)) source.pause();
      });
      destination.on('drain', () => source.resume());
      source.once('end', () => { try { observer.finish(); } catch { ledger.fail("finish_on_end"); } destination.end(); });
      source.on('error', () => { if (!closed) ledger.fail("socket_error_" + (source === upstream ? "upstream" : "client")); destination.destroy(); });
      source.once('close', () => {
        if (!closed) {
          try { observer.finish(); } catch { ledger.fail(); }
          if (ledger.count !== 2 || !ledger.current?.done) ledger.fail();
        }
        destination.destroy();
      });
    };
    relay(client, upstream, up); relay(upstream, client, down);
  });
  const handler = async (request, response) => {
    let outgoing;
    try {
      assert.equal(request.method, 'POST' === request.method ? 'POST' : 'PUT');
      const endpoint = request.url === '/api/v1/things/device/edge/content/prepare' ? 'prepare'
        : request.url === '/api/v1/things/device/edge/content/complete' ? 'complete' : null;
      const put = request.method === 'PUT' && request.url.startsWith('/oss/');
      assert.ok((endpoint && request.method === 'POST') || put);
      if (put) assert.ok(!request.headers.authorization && !request.headers.cookie, 'credential_not_isolated');
      if (endpoint) {
        const parts = []; let size = 0;
        for await (const chunk of request) { size += chunk.length; assert.ok(size <= 8192); parts.push(chunk); }
        const body = Buffer.concat(parts), parsed = JSON.parse(body.toString('utf8'));
        outgoing = http.request({host: config.apiHost, port: config.apiPort, method: 'POST', path: request.url,
          headers: {...request.headers, host: config.apiHost + ':' + config.apiPort}}, incoming => {
          const output = []; let count = 0;
          incoming.on('data', chunk => { count += chunk.length; if (count > 16384) incoming.destroy(); else output.push(chunk); });
          incoming.on('end', () => {
            try {
              const bytes = Buffer.concat(output); assert.ok(incoming.statusCode >= 200 && incoming.statusCode < 300);
              ledger.metadata(endpoint, parsed, JSON.parse(bytes.toString('utf8')));
              response.writeHead(incoming.statusCode, incoming.headers); response.end(bytes);
            } catch { ledger.fail(); response.writeHead(502); response.end(); }
          });
          incoming.on('error', () => { ledger.fail(); response.destroy(); });
        });
        outgoing.end(body);
      } else {
        let size = 0; const digest = createHash('sha256'); let ended = false;
        outgoing = http.request({host: config.s3Host, port: config.s3Port, method: 'PUT',
          // 固定前缀只移除/oss，不decode/re-encode path或query；Host恢复原SDK签名的真实S3端点。
          path: request.url.slice(4), headers: {...request.headers, host: config.s3Host + ':' + config.s3Port}}, incoming => {
          try { assert.ok(ended); ledger.put(request.url, size, digest.digest('hex'), incoming.statusCode); }
          catch { ledger.fail(); incoming.resume(); response.writeHead(502); response.end(); return; }
          response.writeHead(incoming.statusCode, incoming.headers); incoming.pipe(response);
          incoming.on('error', () => { ledger.fail(); response.destroy(); });
        });
        request.on('data', chunk => {
          size += chunk.length;
          if (size > 16777216) { ledger.fail(); request.destroy(); outgoing.destroy(); return; }
          digest.update(chunk); if (!outgoing.write(chunk)) request.pause();
        });
        outgoing.on('drain', () => request.resume());
        request.once('end', () => { ended = true; outgoing.end(); });
      }
      outgoing.setTimeout(30000, () => outgoing.destroy());
      outgoing.on('socket', own);
      outgoing.on('error', () => { ledger.fail(); response.destroy(); });
      request.on('error', () => { ledger.fail(); outgoing.destroy(); });
    } catch { ledger.fail(); if (outgoing) outgoing.destroy(); response.writeHead(502); response.end(); }
  };
  const tls = https.createServer({key: fs.readFileSync(config.tlsKey), cert: fs.readFileSync(config.tlsCert)}, handler);
  tls.on('connection', own); tls.on('clientError', (_, socket) => { ledger.fail(); socket.destroy(); });
  // close观察真实socket/server关闭结果，只关闭本函数创建资源，不清理外部平台/S3或进程。
  const close = async () => {
    closed = true;
    // server.close回调可能早于最后client socket的close通知；必须等原socket事实，不能只因destroy已受理返回成功。
    const owned = [...sockets];
    const drained = Promise.all(owned.map(socket => new Promise(resolve => {
      socket.once('close', resolve); socket.destroy();
    })));
    let timer;
    const socketClosed = await Promise.race([drained.then(() => true),
      new Promise(resolve => { timer = setTimeout(() => resolve(false), 1000); })]);
    clearTimeout(timer);
    const shut = server => new Promise(resolve => server.listening ? server.close(error => resolve(!error)) : resolve(true));
    const results = await Promise.all([shut(mqtt), shut(tls)]);
    return socketClosed && results.every(Boolean) && sockets.size === 0;
  };
  const listen = (server, port) => new Promise((resolve, reject) => {
    server.once('error', reject); server.listen(port, '127.0.0.1', resolve);
  });
  try {
    await listen(mqtt, 0); await listen(tls, config.tlsPort);
    return {mqttPort: mqtt.address().port, close};
  } catch { await close(); throw Error('content_observation_ports_unavailable'); }
}
module.exports = {PacketTap, ContentLedger, startObservationPorts};
