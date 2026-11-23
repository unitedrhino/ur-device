// 文件说明：仅本机验收使用的透明MQTT延迟桥；保持原鉴权及报文字节，不生成Execute、ACK或工具结果。
'use strict';
const assert = require('node:assert/strict');
const net = require('node:net');
const {createHash} = require('node:crypto');

// maxPacket 限定单个原MQTT帧及积累缓冲，畸形长度不能无限占用测试进程。
const maxPacket = 1048576;

// frameLength 只读取MQTT可变长度头；0表示尚不完整，畸形或超过预算时明确失败。
function frameLength(bytes) {
  if (bytes.length < 2) return 0;
  let size = 0, multiplier = 1;
  for (let index = 1; index <= 4; index++) {
    if (index >= bytes.length) return 0;
    const digit = bytes[index];
    size += (digit & 127) * multiplier;
    if (!(digit & 128)) {
      const total = index + 1 + size;
      assert.ok(total <= maxPacket, 'mqtt_frame_budget');
      return bytes.length >= total ? total : 0;
    }
    multiplier *= 128;
  }
  throw Error('mqtt_length_invalid');
}

// executeIdentity 只匹配原已鉴权设备下行Topic的审核工具；标识仅在验收进程内比较，不能写报告。
function executeIdentity(packet, topic) {
  if ((packet[0] >> 4) !== 3) return null;
  const qos = (packet[0] >> 1) & 3;
  assert.ok(qos <= 1, 'mqtt_qos_unsupported');
  let cursor = 1;
  while (packet[cursor++] & 128) {}
  assert.ok(cursor + 2 <= packet.length, 'mqtt_publish_header');
  const length = packet.readUInt16BE(cursor); cursor += 2;
  assert.ok(length > 0 && cursor + length + (qos ? 2 : 0) <= packet.length, 'mqtt_topic_invalid');
  const actual = packet.subarray(cursor, cursor + length).toString('utf8');
  cursor += length + (qos ? 2 : 0);
  if (actual !== topic || (packet[0] & 1)) return null;
  const payload = packet.subarray(cursor);
  let message;
  try {message = JSON.parse(payload.toString('utf8'));} catch {return null;}
  if (message?.method !== 'toolCallExecute' || message.data?.name !== 'set_local_display_theme') return null;
  const identity = {session:message.sessionId, response:message.data.respId,
    call:message.data.toolCallId, token:message.msgToken};
  assert.ok(Object.values(identity).every(value => typeof value === 'string' && value.length > 0 && value.length <= 128), 'mqtt_execute_identity');
  assert.ok(Number.isSafeInteger(message.messageSeq) && message.messageSeq > 0,'mqtt_execute_sequence');
  return {...identity, sequence:message.messageSeq,digest:createHash('sha256').update(payload).digest('hex')};
}

// createExecuteDelay 仅监听/连接IPv4回环；上行原样转发，下行第一次原Execute留在内存直到显式放行。
// 只有当前连接可放行，不把断线前授权重投新连接；关闭时回收所有socket且不投递被持有报文。
async function createExecuteDelay({targetPort, topic}) {
  assert.ok(Number.isSafeInteger(targetPort) && targetPort >= 1024 && targetPort <= 65535);
  assert.ok(typeof topic === 'string' && topic.startsWith('$thing/down/message/') &&
    topic.length <= 512 && !/[+#\x00\r\n]/.test(topic));
  const sockets = new Set();
  let held = null, current = null, failed = false, released = false, closing = false, everHeld = false;
  let closePromise;
  let duplicates = 0, forwarded = 0;
  const server = net.createServer(client => {
    if (closing || current) {failed = true;client.destroy();return;}
    const upstream = net.connect({host:'127.0.0.1',port:targetPort});
    current = client;sockets.add(client);sockets.add(upstream);
    let buffered = Buffer.alloc(0);
    const fail = () => {failed = true;client.destroy();upstream.destroy();};
    client.on('error',fail);upstream.on('error',fail);
    client.once('close',() => {sockets.delete(client);if(current === client) current = null;upstream.destroy();});
    upstream.once('close',() => {sockets.delete(upstream);client.destroy();});
    client.pipe(upstream); // CONNECT及设备身份只经原字节流，不读取或保存鉴权正文。
    client.on('drain',() => upstream.resume());
    upstream.on('data',chunk => {
      try {
        assert.ok(buffered.length + chunk.length <= maxPacket, 'mqtt_buffer_budget');
        buffered = Buffer.concat([buffered,chunk]);
        for (;;) {
          const size = frameLength(buffered);
          if (!size) break;
          const packet = Buffer.from(buffered.subarray(0,size));buffered = buffered.subarray(size);
          const identity = executeIdentity(packet,topic);
          if (identity && !held && !everHeld) {held = {packet,identity,client};everHeld = true;continue;}
          if (identity && held && !released && identity.token === held.identity.token) {
            assert.equal(identity.digest,held.identity.digest,'mqtt_duplicate_changed');
            assert.ok(++duplicates <= 8,'mqtt_duplicate_budget');continue;
          }
          forwarded++;
          if (!client.write(packet)) upstream.pause();
        }
      } catch {fail();} // 不输出原报文或自由异常；失败只能降低验收结论。
    });
  });
  await new Promise((resolve,reject) => {server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
  server.on('error',() => {failed = true;});
  const port = server.address().port;
  return {
    port,
    // identity 只提供原私有scope供调用者与当前控制目标比较，不能跨连接或构造新许可。
    identity:() => held ? {...held.identity} : null,
    // release 要求原会话/回复明确匹配且原连接存活；仅写一次保存的原MQTT帧。
    release(session,response) {
      assert.ok(!failed && !closing && !released && held && held.client === current &&
        !held.client.destroyed && held.identity.session === session && held.identity.response === response,'mqtt_release_scope');
      released = true;held.client.write(held.packet);return true;
    },
    // facts 不返回字节、标识、Topic或模型正文，仅固定布尔/有界计数。
    facts:() => ({held:everHeld,releasedOriginal:released,failed,duplicates,forwarded,connectionsClosed:sockets.size === 0}),
    async close() {
      if (closePromise) return closePromise;
      closing = true;
      const stopped = Array.from(sockets, socket => new Promise(resolve => {
        if (socket.closed) resolve(); else socket.once('close',resolve);
      }));
      const listenerClosed = new Promise(resolve => server.close(resolve));
      for (const socket of sockets) socket.destroy();
      closePromise = Promise.all([listenerClosed,...stopped]).then(() => {held = null;});
      return closePromise;
    },
  };
}
module.exports = {frameLength,executeIdentity,createExecuteDelay,maxPacket};
