// 文件说明：显式S19正式后端验收；透明旁路核对原请求/逐轨事实/ACK/终态，并验证真实sender、解码与资源保持。
// 模块划分：有限MQTT取样、原计划关联、原UI操作及独立代理清理；不合成平台成功报文。
'use strict';
const assert = require('node:assert/strict');
const net = require('node:net');
const {TextDecoder} = require('node:util');
const {uniqueJSON} = require('../tests/tool_trace_projection.cjs');
const {browser, evaluated, run: ipcRun} = require('./ipc_live.cjs');

// planObserver仅保存单个在途计划的原标识；对外快照只含固定计数/枚举，不输出设备、会话、token或SDP。
function planObserver() {
  let current, revision = 0, watermark = 0, error = false;
  const records = [], wires = new Map(); // 保留八个计划的有限原字节，迟到DUP不错误归到新计划。
  const equal = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const integer = n => Number.isSafeInteger(n) && n >= 0;
  const id = s => typeof s === 'string' && s.length > 0 && s.length <= 128 && !/[\x00-\x1f\x7f]/.test(s);
  function begin(session, track, profile) {
    assert.ok(!error && (!current || current.completed) && records.length < 8, 'quality_observer_busy');
    assert.ok(id(session) && ['audio-1', 'video-1'].includes(track) && ['low', 'high', 'standard', 'audioOnly'].includes(profile));
    current = {session, track, profile, target: profile === 'audioOnly' && track === 'video-1' ? 'paused' : 'active',
      token: '', reportToken: '', request: false, pending: false, reported: false, acknowledged: false, applied: false, completed: false};
  }
  // observe只认可当前原腿和方向，重复帧必须原字节一致；错误被固定布尔锁存而不反射原文。
  function observe(direction, message) {
    if (!current || error || !message || message.sessionId !== current.session) return;
    const c = current, relevant = ['mediaUpdate', 'mediaUpdated', 'trackState', 'requestAck'].includes(message.method);
    if (!relevant) return;
    // 同一原腿后续可进行旧轨控制；它的code回执没有质量关联，不参与本计划的实态与ACK链。
    if (message.method === 'trackState' && Object.hasOwn(message, 'code') &&
        !Object.hasOwn(message.data ?? {}, 'requestToken') && !Object.hasOwn(message.data ?? {}, 'revision')) return;
    if (message.method === 'requestAck' && message.data?.requestMethod !== 'trackState') return;
    try {
      const data = message.data, params = message.params;
      const key = `${direction}:${message.method}:${message.code ?? 'event'}:${data?.state ?? ''}:${message.msgToken}`;
      const wire = JSON.stringify(message);
      if (wires.has(key)) {assert.equal(wires.get(key), wire); return;}
      assert.ok(wires.size < 128 && id(message.msgToken) && !Object.hasOwn(message, 'messageSeq'));
      wires.set(key, wire);
      if (direction === 'up' && message.method === 'mediaUpdate') {
        assert.ok(!c.request && !Object.hasOwn(message, 'code') && !data && !Object.hasOwn(message, 'eventSeq'));
        assert.ok(params?.operation === 'setQuality' && params.profile === c.profile &&
          equal(params.trackIds, [c.track]) && params.expectedRevision === revision);
        assert.deepEqual(Object.keys(params).sort(), ['expectedRevision', 'operation', 'profile', 'trackIds']);
        c.token = message.msgToken; c.request = true;
      } else if (direction === 'down' && message.method === 'mediaUpdated' && Object.hasOwn(message, 'code')) {
        assert.ok(c.request && message.msgToken === c.token && message.code === 200 && data?.state === 'pending' && !params);
        assert.ok(!Object.hasOwn(message, 'eventSeq') && data.revision === revision + 1 && data.mediaRevision === 1 &&
          Array.isArray(data.tracks) && data.tracks.length === 1 && data.tracks[0].trackId === c.track &&
          (!Object.hasOwn(data, 'requestToken') || data.requestToken === c.token));
        c.pending = true;
      } else if (direction === 'up' && message.method === 'trackState' && data?.requestToken === c.token) {
        assert.ok(c.pending && !Object.hasOwn(message, 'code') && !params && !Object.hasOwn(message, 'eventSeq'));
        assert.ok(data.trackId === c.track && data.state === c.target && data.revision === revision + 1 && data.mediaRevision === 1);
        assert.ok(!c.reported); c.reportToken = message.msgToken; c.reported = true;
      } else if (direction === 'down' && message.method === 'requestAck' && data?.requestMethod === 'trackState') {
        assert.ok(c.reported && message.msgToken === c.reportToken && message.code === 200 && !params && !Object.hasOwn(message, 'eventSeq'));
        assert.deepEqual(data.result, {trackId: c.track, state: c.target, mediaRevision: 1}); c.acknowledged = true;
      } else if (direction === 'down' && message.method === 'mediaUpdated' && !Object.hasOwn(message, 'code')) {
        assert.ok(c.reported && data?.requestToken === c.token && data.state === 'applied' && !params &&
          integer(message.eventSeq) && message.eventSeq > watermark && data.revision === revision + 1 && data.mediaRevision === 1);
        assert.ok(Array.isArray(data.tracks) && data.tracks.length === 1 && data.tracks[0].trackId === c.track && data.tracks[0].state === c.target);
        c.applied = true; watermark = message.eventSeq;
      } else throw Error('quality_phase_invalid');
      if (c.acknowledged && c.applied && !c.completed) {
        c.completed = true; revision++;
        records.push({profile: c.profile, track: c.track, revision, pending: true, actualReported: true, acknowledged: true, applied: true});
      }
    } catch {error = true;}
  }
  return {begin, observe, fail: () => {error = true;}, snapshot: () => ({error, completed: !!current?.completed,
    phase: {requested: !!current?.request, pending: !!current?.pending, reported: !!current?.reported,
      acknowledged: !!current?.acknowledged, applied: !!current?.applied},
    revision, records: records.map(r => ({...r}))})};
}

// qualityButtonSelector只生成普通CSS；section与按钮序号必须来自原会话DOM读回，不使用CLI不支持的文本伪类。
function qualityButtonSelector(section, button) {
  assert.ok(Number.isInteger(section) && section >= 1 && section <= 3 && Number.isInteger(button) && button >= 1 && button <= 8);
  return `#views > section:nth-child(${section}) > button:nth-of-type(${button})`;
}

// frames仅旁路解析选定Topic的MQTT3.1.1 PUBLISH，非法UTF8、重复JSON键、长度和预算异常使验收失败，不改转发字节。
function frames(direction, topic, consume, fail) {
  let buffer = Buffer.alloc(0), disabled = false, count = 0;
  const stop = () => {if (!disabled) fail(); disabled = true; buffer = Buffer.alloc(0);};
  return chunk => {
    if (disabled) return;
    if (!Buffer.isBuffer(chunk) || buffer.length + chunk.length > 131072) {stop(); return;}
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length >= 2) {
      let size = 0, offset = 1, scale = 1, complete = false;
      for (; offset < Math.min(buffer.length, 5); offset++) {
        const byte = buffer[offset]; size += (byte & 127) * scale; scale *= 128;
        if (!(byte & 128)) {offset++; complete = true; break;}
      }
      if (!complete) {if (buffer.length >= 5) stop(); return;}
      if (size > 65536) {stop(); return;}
      if (buffer.length < offset + size) return;
      const flags = buffer[0], data = buffer.subarray(offset, offset + size); buffer = buffer.subarray(offset + size);
      if ((flags >> 4) !== 3) continue;
      if (data.length < 2) {stop(); return;}
      const length = data.readUInt16BE(0), qos = (flags >> 1) & 3;
      if (qos > 1 || length === 0 || 2 + length + (qos ? 2 : 0) > data.length) {stop(); return;}
      try {
        const decoder = new TextDecoder('utf-8', {fatal: true});
        if (decoder.decode(data.subarray(2, 2 + length)) !== topic) continue;
        if (++count > 256) {stop(); return;}
        consume(direction, uniqueJSON(decoder.decode(data.subarray(2 + length + (qos ? 2 : 0)))));
      } catch {stop(); return;}
    }
  };
}

// relay只连接明确回环Broker；失败或关闭只销毁本代理创建的socket，不触碰其他客户端或Broker。
async function relay(env, observer) {
  assert.equal(env.UR_MQTT_HOST, '127.0.0.1');
  const port = Number(env.UR_MQTT_PORT); assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  assert.ok(env.UR_DEVICE_PRODUCT && env.UR_DEVICE_NAME);
  const sockets = new Set();
  const server = net.createServer(source => {
    const target = net.connect(port, '127.0.0.1'); sockets.add(source); sockets.add(target);
    source.on('data', frames('up', `$thing/up/signaling/${env.UR_DEVICE_PRODUCT}/${env.UR_DEVICE_NAME}`, observer.observe, observer.fail));
    target.on('data', frames('down', `$thing/down/signaling/${env.UR_DEVICE_PRODUCT}/${env.UR_DEVICE_NAME}`, observer.observe, observer.fail));
    const failed = () => {observer.fail(); source.destroy(); target.destroy();};
    source.on('error', failed); target.on('error', failed);
    source.on('close', () => {sockets.delete(source); target.destroy();});
    target.on('close', () => {sockets.delete(target); source.destroy();});
    source.pipe(target); target.pipe(source);
  });
  await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
  return {port: server.address().port, close: () => new Promise((resolve, reject) => {
    for (const socket of sockets) socket.destroy(); server.close(error => error ? reject(error) : resolve());
  })};
}

// verifyQuality通过原页面真实按钮提交八次逐轨意图；实际原sender和两观看者的解码均独立读回，不拿终态替代媒体。
async function verifyQuality({deviceBrowser, env, viewers, metrics}, observer) {
  assert.equal(viewers.length, 3); const viewer = viewers[1], other = viewers[2], session = JSON.stringify(viewer.sessionId);
  assert.ok(viewer.listening && other.listening);
  await browser(deviceBrowser, env, 'eval', `(()=>{const v=views.get(${session});window.__qualityOriginal={peer:v.peer,stream:v.stream};return true;})()`);
  const evidence = [];
  for (const [track, profile] of [['audio-1','low'],['audio-1','high'],['audio-1','standard'],
    ['video-1','low'],['video-1','high'],['video-1','standard'],['video-1','audioOnly'],['video-1','standard']]) {
    observer.begin(viewer.sessionId, track, profile);
    const before = await metrics(viewer), otherBefore = await metrics(other);
    const label = `${track === 'video-1' ? '视频' : '音频'}档位 ${profile}`;
    const index = evaluated(await browser(deviceBrowser, env, 'eval', `(()=>{const v=views.get(${session});
      const n=Array.from(document.getElementById('views').children).indexOf(v.element)+1;
      const button=Array.from(v.element.querySelectorAll('button')).find(b=>b.textContent===${JSON.stringify(label)});
      if(!button||n<1)throw Error('quality_button_absent');button.scrollIntoView({block:'center'});
      return JSON.stringify({section:n,button:Array.from(v.element.querySelectorAll(':scope > button')).indexOf(button)+1});})()`));
    await browser(deviceBrowser, env, 'snapshot', '-i');
    await browser(deviceBrowser, env, 'click', qualityButtonSelector(index.section, index.button));
    const until = Date.now() + 12000;
    while (Date.now() < until && !observer.snapshot().completed && !observer.snapshot().error) await new Promise(r => setTimeout(r, 50));
    assert.ok(observer.snapshot().completed && !observer.snapshot().error, 'quality_original_plan_not_completed');
    const actual = evaluated(await browser(deviceBrowser, env, 'eval', `(()=>{const v=views.get(${session}),s=v.sources.get(${JSON.stringify(track)}),e=s.sender.getParameters().encodings;
      return JSON.stringify({samePeer:v.peer===__qualityOriginal.peer,sameStream:v.stream===__qualityOriginal.stream,connected:v.peer.connectionState==='connected',
        state:s.state,sourceLive:s.track.readyState==='live',sourceEnabled:s.track.enabled,senderBound:s.sender.track===s.track,encodingCount:e?.length||0,
        maxBitrate:e?.[0]?.maxBitrate||0,scale:e?.[0]?.scaleResolutionDownBy||0,maxFramerate:e?.[0]?.maxFramerate||0});})()`));
    assert.ok(actual.samePeer && actual.sameStream && actual.connected && actual.sourceLive);
    if (profile === 'audioOnly') {
      assert.ok(actual.state === 'paused' && !actual.sourceEnabled && !actual.senderBound);
      await browser(viewer.browser, env, 'wait', '500'); const drained = await metrics(viewer);
      await browser(viewer.browser, env, 'wait', '500'); const stopped = await metrics(viewer);
      assert.ok(stopped.packets === drained.packets && stopped.frames === drained.frames && stopped.audioSamples > drained.audioSamples);
    } else {
      const bitrate = track === 'video-1' ? {low:150000,high:1200000,standard:600000}[profile] : {low:16000,high:64000,standard:32000}[profile];
      assert.ok(actual.state === 'active' && actual.sourceEnabled && actual.senderBound && actual.encodingCount === 1 && actual.maxBitrate === bitrate);
      if (track === 'video-1') assert.ok(actual.scale === (profile === 'low' ? 2 : 1) && actual.maxFramerate === (profile === 'low' ? 10 : 15));
      await browser(viewer.browser, env, 'wait', '--fn', `(async()=>{let a=0,v=0;(await __ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio')a+=s.totalSamplesReceived||0;
        if(s.type==='inbound-rtp'&&s.kind==='video')v+=s.framesDecoded||0;});return a>${before.audioSamples}+960&&v>${before.frames};})()`);
    }
    const after = await metrics(viewer), otherAfter = await metrics(other);
    assert.ok(after.audioPeak > 0.001 && otherAfter.audioSamples > otherBefore.audioSamples && otherAfter.frames > otherBefore.frames);
    evidence.push({track,profile,revision:observer.snapshot().revision,samePeer:true,sameCapture:true,actualSender:actual,
      decodedFrames:after.frames,decodedAudioSamples:after.audioSamples,otherContinued:true});
  }
  return evidence;
}

// run显式启用后才创建代理与设备；任何失败原ipcRun仍走清理，环境变量和代理在finally精确恢复。
async function run() {
  assert.equal(process.env.UR_IPC_QUALITY_LIVE, '1', 'explicit_quality_live_required');
  const observer = planObserver(), proxy = await relay(process.env, observer), original = process.env.UR_MQTT_PORT;
  let quality;
  process.env.UR_MQTT_PORT = String(proxy.port);
  try {
    await ipcRun({onEstablished: async context => {quality = await verifyQuality(context, observer);}});
    assert.ok(quality?.length === 8 && !observer.snapshot().error && observer.snapshot().records.length === 8, 'quality_full_chain_required');
    console.log(JSON.stringify({diagnostic:'ipcQuality',passed:true,plans:observer.snapshot().records,evidence:quality,hardwareVerified:false}));
  } catch {
    console.log(JSON.stringify({diagnostic:'ipcQuality',passed:false,observation:observer.snapshot(),hardwareVerified:false}));
    throw Error('ipc_quality_live_failed');
  } finally {process.env.UR_MQTT_PORT = original; await proxy.close();}
}
module.exports = {planObserver,frames,relay,qualityButtonSelector,verifyQuality,run};
if (require.main === module) run().catch(() => {console.error('ipc_quality_live_failed'); process.exitCode = 1;});
