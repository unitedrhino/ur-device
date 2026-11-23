// 文件说明：正式历史网页源的确定性离线回归；媒体/Peer为模拟端口，不作为实际解码证据。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// archive仅生成格式检查语料；真实容器解码由中央Chrome验收，不把此最小结构当可播录像。
function archive(options = {}) {
  const box = (type, ...parts) => {
    const body = Buffer.concat(parts), header = Buffer.alloc(8);
    header.writeUInt32BE(body.length + 8); header.write(type, 4);
    return Buffer.concat([header, body]);
  };
  const track = (kind, codec) => {
    const handler = Buffer.alloc(12); handler.write(kind, 8);
    const count = Buffer.alloc(8); count.writeUInt32BE(1, 4);
    const sample = codec === 'avc1' ? box(codec, Buffer.alloc(78), box('avcC', Buffer.from([1, 66, 224, 30, 255, 225, 0]))) :
      codec === 'Opus' ? box(codec, Buffer.alloc(28), box('dOps', Buffer.from([0, 2, 0, 0, 0, 0, 187, 128, 0, 0, 0]))) : box(codec, Buffer.alloc(28));
    return box('trak', box('mdia', box('hdlr', handler), box('minf', box('stbl', box('stsd', count, sample)))));
  };
  return Buffer.concat([
    box('ftyp', Buffer.from('isom0000')), box('moov', track('vide', options.videoCodec || 'avc1'), track('soun', options.audioCodec || 'Opus')),
  ]);
}

// fixture运行正式源码，显式记录档案播放、硬件采集、发送轨和实际关闭调用。
function fixture(options = {}) {
  const sent = [], videos = [], peers = [], tracks = [], contexts = [], routes = [];
  let captures = 0;
  const track = kind => { const value = {kind, readyState: 'live', stops: 0, stop() { this.readyState = 'ended'; this.stops++; }}; tracks.push(value); return value; };
  const stream = values => ({getTracks: () => values, getVideoTracks: () => values.filter(t => t.kind === 'video'), getAudioTracks: () => values.filter(t => t.kind === 'audio')});
  class Video {
    constructor() { this.paused = true; this._time = 0; this.duration = options.duration ?? 30; this.readyState = 0; this.listeners = new Map(); this.callbacks = new Map(); this.serial = 0; this.plays = 0; this.pauses = 0; this.removed = false; videos.push(this); }
    // currentTime 模拟暂停seek仅呈现一次目标帧；错过这一帧的后注册监听不能凭空获得下一帧。
    get currentTime() { return this._time; }
    set currentTime(value) {
      const before=this._time;this._time=value;
      if(options.seekFrameOnly&&this.paused){this.seeking=true;this.readyState=1;
        setTimeout(()=>{
          if(options.oldSeekFrame){for(const [id,entry] of [...this.callbacks]){this.callbacks.delete(id);clearTimeout(entry.timer);entry.callback(0,{mediaTime:before});}}
          this.seeking=false;this.readyState=2;this.listeners.get('seeked')?.();
          if(options.holdSeekFrame)return;
          for(const [id,entry] of [...this.callbacks]){this.callbacks.delete(id);clearTimeout(entry.timer);entry.callback(0,{mediaTime:options.seekMediaTime??Math.min(value,this.duration-0.02)});}
        },0);
      }else this.listeners.get('seeked')?.();
    }
    canPlayType() { return options.container === false ? '' : 'probably'; }
    captureStream() { this.captures = (this.captures || 0) + 1; return stream(options.noTracks ? [] : [track('video'), track('audio')]); }
    requestVideoFrameCallback(callback) {
      const id = ++this.serial;
      const timer = options.seekFrameOnly&&this.paused ? null : setTimeout(() => { this.callbacks.delete(id); if (!this.paused) this.currentTime += 0.02; callback(0, {mediaTime: this.currentTime}); }, 0);
      this.callbacks.set(id, {timer,callback}); return id;
    }
    cancelVideoFrameCallback(id) { clearTimeout(this.callbacks.get(id)?.timer); this.callbacks.delete(id); }
    pause() { this.paused = true; this.pauses++; }
    async play() { this.plays++; this.paused = false; this.currentTime += 0.02; }
    load() { this.readyState = 2; }
    addEventListener(name, callback) { this.listeners.set(name, callback); }
    removeEventListener(name,callback) { if(this.listeners.get(name)===callback)this.listeners.delete(name); }
    removeAttribute() {} remove() { this.removed = true; }
  }
  class Context {
    constructor() { this.state = 'running'; this.closed = false; this.nodes = []; contexts.push(this); }
    async resume() {} async close() { this.closed = true; this.state = 'closed'; }
    createMediaElementSource() { const node = {connections: [], connect(target) { this.connections.push(target); }, disconnect(target) { this.connections = target ? this.connections.filter(v => v !== target) : []; }}; this.nodes.push(node); return node; }
    createMediaStreamDestination() { return {stream: stream([track('audio')])}; }
  }
  class Peer {
    constructor() { this.transceivers = []; this.connectionState = 'new'; this.iceGatheringState = 'complete'; this.closed = false; peers.push(this); }
    addTransceiver(kind, settings) { const value = {kind, settings, sender: {track: null, async replaceTrack(value) { this.track = value; }}, setCodecPreferences(codecs) { this.codecs = codecs; }}; this.transceivers.push(value); return value; }
    async createOffer() { return options.offer ? options.offer() : {type: 'offer', sdp: 'v=0\r\ns=fixture\r\n'}; }
    async setLocalDescription(value) { this.localDescription = value; }
    async setRemoteDescription() { this.connectionState = 'connected'; this.onconnectionstatechange?.(); }
    close() { this.closed = true; this.connectionState = 'closed'; }
  }
  const context = vm.createContext({module: {exports: {}}, document: {createElement: () => new Video()},
    navigator: {mediaDevices: {getUserMedia() { captures++; throw Error('must_not_capture'); }}},
    RTCRtpSender: {getCapabilities(kind) { return {codecs: kind === 'video' ? options.h264 === false ? [] : [{mimeType: 'video/H264'}] : options.opus === false ? [] : [{mimeType: 'audio/opus'}]}; }},
    RTCPeerConnection: Peer, AudioContext: Context, performance, setTimeout, clearTimeout, AbortController, Blob,
    URL: {createObjectURL: () => 'blob:fixed', revokeObjectURL() {}},
    async fetch(route, opts) { routes.push({route, opts}); let done = false; const data = options.archive || archive(options); return {ok: true, redirected: false, headers: {get: () => String(data.length)}, body: {getReader: () => ({async read() { if (done) return {done: true}; done = true; return {value: data, done: false}; }, async cancel() {}, releaseLock() {}})}}; },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'record-source.js'), 'utf8'), context);
  const Source = context.module.exports.UrRecordSource;
  const source = new Source({send: value => sent.push(JSON.parse(JSON.stringify(value))), token: 'a'.repeat(64), mount: {appendChild() {}}});
  const base = {sessionId: 's', playbackId: 'p'};
  const bind = {kind: 'recordBind', ...base, assetRoute: '/record-assets/archive.mp4', mime: 'video/mp4; codecs="avc1.42E01E,opus"', audio: true, durationMs: 30000, maxBytes: 1000};
  const command = (operation, generation, mediaRevision = 1, extra = {}) => ({kind: 'recordControl', ...base, operation, generation, mediaRevision, ...extra});
  return {source, sent, videos, peers, tracks, contexts, routes, base, bind, command, captures: () => captures,
    async load() { assert.equal(await source.incoming(bind), true); assert.equal(await source.incoming(command('load', '1')), true); },
    async peer(mediaRevision = 1) { assert.equal(await source.incoming({kind: 'recordPeerBegin', ...base, mediaRevision}), true); assert.equal(await source.incoming({kind: 'recordAnswer', ...base, mediaRevision, sdp: 'v=0\r\ns=answer\r\n'}), true); },
    async close() { if (source.binding) await source.incoming({kind: 'recordClose', ...base, releaseId: '1'}); },
  };
}
test('容器及H264/Opus实际能力缺失明确不支持，不声明摄像头代替', async () => {
  for (const config of [{container: false}, {h264: false}, {opus: false}]) {
    const f = fixture(config);
    assert.equal(await f.source.incoming({kind: 'recordProbe', mime: f.bind.mime, audio: true}), true);
    assert.equal(f.sent[0].supported, false); assert.equal(f.captures(), 0); assert.equal(f.routes.length, 0);
  }
});
test('固定路由及白名单字段拒绝外部URL和页面自报权限', async () => {
  const f = fixture();
  assert.equal(await f.source.incoming({...f.bind, assetRoute: 'https://elsewhere.invalid/asset.mp4'}), false);
  assert.equal(await f.source.incoming({...f.bind, assetRoute: '/record-assets/../x'}), false);
  assert.equal(await f.source.incoming({...f.bind, permission: true}), false);
  assert.equal(f.routes.length, 0); assert.equal(f.captures(), 0);
});
test('真实load替身完成但不play；独立音频只接档案目标而非扬声器', async () => {
  const f = fixture(); await f.load();
  assert.equal(f.source.media.plays, 0); assert.equal(f.source.state, 'loaded');
  assert.equal(f.sent.at(-1).state, 'loaded'); assert.equal(f.sent.at(-1).durationMs, 30000);
  assert.equal(f.contexts[0].nodes[0].connections.length, 0); assert.equal(f.captures(), 0);
  assert.equal(f.routes[0].route, '/record-assets/archive.mp4'); await f.close();
});
test('暂停档案无轨也可仅协商null sender；Connected不伪造playing', async () => {
  const f = fixture({noTracks: true}); await f.load(); await f.peer();
  assert.equal(f.source.media.plays, 0); assert.equal(f.source.media.captures || 0, 0);
  assert.equal(f.peers[0].transceivers.length, 2);
  for (const sender of f.peers[0].transceivers) { assert.equal(sender.sender.track, null); assert.equal(sender.settings.direction, 'sendonly'); }
  assert.equal(f.source.state, 'loaded'); assert.ok(f.sent.some(value => value.kind === 'recordConnected'));
  assert.equal(f.sent.filter(value => value.state === 'playing').length, 0); await f.close();
});
test('明确play后附真实档案轨，pause实际detach且不换Peer或偷开硬件', async () => {
  const f = fixture(); await f.load(); await f.peer();
  assert.equal(await f.source.incoming(f.command('play', '2')), true);
  assert.equal(f.source.state, 'playing'); assert.ok(f.peers[0].transceivers.every(value => value.sender.track?.readyState === 'live'));
  assert.equal(await f.source.incoming(f.command('pause', '3')), true);
  assert.equal(f.source.state, 'paused'); assert.ok(f.peers[0].transceivers.every(value => value.sender.track === null));
  assert.equal(f.peers.length, 1); assert.equal(f.captures(), 0); await f.close();
});
test('旧Peer实际关闭后seek换m；原paused restore不自动play或附轨', async () => {
  const f = fixture(); await f.load(); await f.peer();
  await f.source.incoming(f.command('play', '2')); await f.source.incoming(f.command('pause', '3'));
  const plays = f.source.media.plays;
  assert.equal(await f.source.incoming({kind: 'recordPeerClose', ...f.base, mediaRevision: 1}), true);
  assert.equal(f.peers[0].closed, true);
  assert.ok(f.sent.some(value => value.kind === 'recordPeerStopped' && value.mediaRevision === 1));
  assert.equal(await f.source.incoming(f.command('seek', '4', 2, {positionMs: 10000})), true);
  assert.equal(f.sent.at(-1).state, 'seeking'); assert.equal(f.sent.at(-1).positionMs, 10000);
  await f.peer(2);
  assert.equal(await f.source.incoming({kind: 'recordRestore', ...f.base, mediaRevision: 2, generation: '4', operation: 'paused'}), true);
  assert.equal(f.source.state, 'paused'); assert.equal(f.source.media.plays, plays);
  assert.ok(f.peers[1].transceivers.every(value => value.sender.track === null));
  assert.equal(f.captures(), 0); await f.close();
});
test('新m不可沿旧Peer定位；未知generation和旧answer拒绝', async () => {
  const f = fixture(); await f.load(); await f.peer();
  assert.equal(await f.source.incoming(f.command('seek', '2', 2, {positionMs: 10000})), false);
  assert.equal(f.source.media.currentTime, 0);
  assert.equal(await f.source.incoming(f.command('play', '01', 2)), false);
  assert.equal(await f.source.incoming({kind: 'recordAnswer', ...f.base, mediaRevision: 9, sdp: 'v=0\r\ns=old\r\n'}), false);
  await f.close();
});
test('实际duration不匹配失败，不用清单值伪造loaded', async () => {
  const f = fixture({duration: 29}); assert.equal(await f.source.incoming(f.bind), true);
  assert.equal(await f.source.incoming(f.command('load', '1')), false);
  assert.equal(f.sent.at(-1).code, 503); assert.equal(f.sent.at(-1).positionMs, undefined);
  assert.equal(f.source.state, ''); await f.close();
});
test('素材实际样本格式拒绝AAC/VP8/畸形盒，不被可播放MIME探针冒充H264+Opus', async () => {
  for (const config of [{audioCodec: 'mp4a'}, {videoCodec: 'vp08'}, {archive: Buffer.from('not_an_archive')}, {archive: archive().subarray(0, 35)}]) {
    const f = fixture(config); assert.equal(await f.source.incoming(f.bind), true);
    assert.equal(await f.source.incoming(f.command('load', '1')), false);
    assert.equal(f.sent.at(-1).code, 503); assert.equal(f.sent.filter(value => value.state === 'loaded').length, 0);
    assert.equal(f.source.media, null); assert.equal(f.captures(), 0); await f.close();
  }
});
test('旧offer协程未退出不能报告Stopped；结束后原offer不迟到污染新m', async () => {
  let finish;
  const f = fixture({offer: () => new Promise(resolve => { finish = resolve; })}); await f.load();
  const begin = f.source.incoming({kind: 'recordPeerBegin', ...f.base, mediaRevision: 1});
  await new Promise(resolve => setImmediate(resolve));
  const close = f.source.incoming({kind: 'recordPeerClose', ...f.base, mediaRevision: 1});
  assert.equal(f.peers[0].closed, true);
  assert.equal(f.sent.filter(value => value.kind === 'recordPeerStopped').length, 0);
  finish({type: 'offer', sdp: 'v=0\r\ns=late\r\n'}); await begin; await close;
  assert.equal(f.sent.filter(value => value.kind === 'recordPeerStopped').length, 1);
  assert.equal(f.sent.filter(value => value.kind === 'recordOffer').length, 0); await f.close();
});
test('整组close停止所有轨和音频context，再发布原release票据', async () => {
  const f = fixture(); await f.load(); await f.peer(); await f.source.incoming(f.command('play', '2'));
  await f.close();
  assert.equal(f.source.closed, true); assert.ok(f.peers.every(value => value.closed));
  assert.ok(f.tracks.every(value => value.readyState === 'ended')); assert.ok(f.contexts.every(value => value.closed));
  assert.deepEqual(f.sent.at(-1), {kind: 'recordClosed', ...f.base, releaseId: '1', success: true});
  assert.equal(await f.source.incoming(f.command('play', '3')), false);
});

test('暂停seek目标帧在ready观察期间已呈现仍须记录原实际帧而非等不存在的下一帧', async () => {
  const f=fixture({seekFrameOnly:true});await f.load();await f.peer();
  await f.source.incoming({kind:'recordPeerClose',...f.base,mediaRevision:1});
  assert.equal(await f.source.incoming(f.command('seek','2',2,{positionMs:10000})),true);
  assert.equal(f.sent.at(-1).state,'seeking');assert.equal(f.sent.at(-1).positionMs,10000);
  assert.equal(f.source.media.plays,0);assert.equal(f.captures(),0);await f.close();
});

test('原seeked前旧呈现回调不能成功，seeked后记录真实帧内时间而非请求毫秒', async () => {
  const f=fixture({seekFrameOnly:true,oldSeekFrame:true,seekMediaTime:9.98});await f.load();await f.peer();
  await f.source.incoming({kind:'recordPeerClose',...f.base,mediaRevision:1});
  assert.equal(await f.source.incoming(f.command('seek','2',2,{positionMs:10001})),true);
  assert.equal(f.sent.at(-1).positionMs,9980);assert.equal(f.sent.at(-1).state,'seeking');
  assert.equal(f.sent.filter(value=>value.kind==='recordFact'&&value.mediaRevision===2&&value.code===200).length,1);
  assert.equal(f.source.waiters.size,0);assert.equal(f.source.media.callbacks.size,0);await f.close();
});

test('seek末尾只有真实最后帧才能成功且不把位置补造为duration', async () => {
  const f=fixture({seekFrameOnly:true});await f.load();await f.peer();
  await f.source.incoming({kind:'recordPeerClose',...f.base,mediaRevision:1});
  assert.equal(await f.source.incoming(f.command('seek','2',2,{positionMs:30000})),true);
  assert.equal(f.sent.at(-1).positionMs,29980);assert.equal(f.source.media.plays,0);await f.close();
});

test('seek无帧超时取消回调和事件监听，不发布伪造成功或污染下一m', async () => {
  const f=fixture({seekFrameOnly:true,holdSeekFrame:true});await f.load();await f.peer();
  await f.source.incoming({kind:'recordPeerClose',...f.base,mediaRevision:1});
  assert.equal(await f.source.incoming(f.command('seek','2',2,{positionMs:30000})),false);
  assert.equal(f.sent.at(-1).code,503);assert.equal(f.sent.at(-1).positionMs,undefined);
  assert.equal(f.source.media.callbacks.size,0);assert.equal(f.source.waiters.size,0);assert.equal(f.source.frameHandle,null);
  assert.equal(f.source.media.listeners.has('seeked'),false);await f.close();
});

test('seek中的整组close立即取消原等待，迟到旧回调不能恢复位置或回执', async () => {
  const f=fixture({seekFrameOnly:true,holdSeekFrame:true});await f.load();await f.peer();
  await f.source.incoming({kind:'recordPeerClose',...f.base,mediaRevision:1});
  const operation=f.source.incoming(f.command('seek','2',2,{positionMs:10000}));
  await new Promise(resolve=>setImmediate(resolve));
  const media=f.source.media,late=[...media.callbacks.values()][0]?.callback;
  await f.close();assert.equal(await operation,false);assert.equal(f.source.closed,true);
  const count=f.sent.length;late?.(0,{mediaTime:10});assert.equal(f.sent.length,count);
  assert.equal(f.source.decodedPosition,0);assert.equal(media.callbacks.size,0);assert.equal(f.source.waiters.size,0);
  assert.deepEqual(f.sent.at(-1),{kind:'recordClosed',...f.base,releaseId:'1',success:true});
});
