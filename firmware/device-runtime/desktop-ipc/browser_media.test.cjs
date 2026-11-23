// 文件说明：IPC网页驱动离线单测验证权限意图、异步关闭和独立Peer；不替代真实媒体E2E。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 提供可控采集和Peer，所有状态转换均来自正式网页源码。
function fixture(href) {
  const elements = new Map();
  const streams = [];
  const peers = [];
  const sent = [];
  let capture = null;
  let h264 = true;
  let opus = true;
  let playback = null;
  let quality = null; // 可控异步编码器，用于验证驱动完成之前没有成功事实。
  const captures = [];
  const element = () => ({children: [], append(...items) { this.children.push(...items); }, remove() {}, srcObject: null, textContent: '', plays: 0, pauses: 0,
    async play() { this.plays++; if (playback) await playback(); }, pause() { this.pauses++; }});
  // stream 记录实际停止调用，避免仅断言状态布尔值。
  const stream = (audio, video = true) => {
    const tracks = video ? [{kind: 'video', readyState: 'live', stops: 0,
      stop() { this.readyState = 'ended'; this.stops++; }}] : [];
    if (audio) tracks.push({kind: 'audio', readyState: 'live', stops: 0,
      stop() { this.readyState = 'ended'; this.stops++; }});
    const value = {getTracks() { return tracks; }, getVideoTracks() { return tracks.filter(t => t.kind === 'video'); }, getAudioTracks() { return tracks.filter(t => t.kind === 'audio'); }, addTrack(track) { tracks.push(track); }, removeTrack(track) { const index = tracks.indexOf(track); if (index >= 0) tracks.splice(index, 1); }};
    streams.push(value); return value;
  };
  // Peer 模拟完整候选已经收集，测试不使用真实网络或长计时器。
  class Peer {
    constructor() { this.iceGatheringState = 'complete'; this.transceivers = []; this.closed = false; peers.push(this); }
    addTransceiver(track, options) {
      const sender = {track, replacements: [], qualityCalls: [], parameters: {encodings: [{}]},
        getParameters() { return JSON.parse(JSON.stringify(this.parameters)); },
        async setParameters(value) { this.qualityCalls.push(value); if (quality) await quality(value); this.parameters = JSON.parse(JSON.stringify(value)); },
        async replaceTrack(value) { this.track = value; this.replacements.push(value); }};
      const result = {track, options, sender, setCodecPreferences(codecs) { this.codecs = codecs; }};
      this.transceivers.push(result); return result;
    }
    async createOffer() { return {type: 'offer', sdp: 'v=0\r\nfixture'}; }
    async setLocalDescription(description) { this.localDescription = description; }
    async setRemoteDescription(description) { this.remoteDescription = description; }
    close() { this.closed = true; }
    async getStats() { return new Map([['video', {type: 'outbound-rtp', kind: 'video', framesEncoded: 5, packetsSent: 10}]]); }
  }
  const testSocket = {readyState: 1, bufferedAmount: 0, send(body) { sent.push(JSON.parse(body)); }, close() { this.readyState = 3; }};
  const context = vm.createContext({
    document: {createElement: element, getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); }},
    navigator: {mediaDevices: {async getUserMedia(options) { captures.push(options); return capture ? capture(options) : stream(options.audio, options.video !== false); }}},
    RTCPeerConnection: Peer, RTCRtpSender: {getCapabilities(kind) { return {codecs: kind === 'audio' ? (opus ? [{mimeType: 'audio/opus'}] : []) : (h264 ? [{mimeType: 'video/H264', sdpFmtpLine: 'packetization-mode=1'}] : [])}; }},
    RTCRtpReceiver: {getCapabilities(kind) { return {codecs: kind === 'video' ? (h264 ? [{mimeType:'video/H264',sdpFmtpLine:'packetization-mode=1'}] : []) : (opus ? [{mimeType: 'audio/opus'}] : [])}; }},
    MediaStream: class { constructor(tracks) { this.tracks = tracks; } getTracks() { return this.tracks; } },
    WebSocket: {OPEN: 1}, window: {addEventListener() {}, ...(href ? {location: {href}} : {})}, URL, testSocket,
    setTimeout, clearTimeout,
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'web/app.js'), 'utf8'), context);
  vm.runInContext('socket = testSocket;', context);
  return {streams, peers, sent, context, stream, captures,
    capture(fn) { capture = fn; }, h264(value) { h264 = value; },
    opus(value) { opus = value; }, playback(fn) { playback = fn; },
    quality(fn) { quality = fn; },
    async incoming(value) { context.command = value; await vm.runInContext('incoming(command)', context); },
    async stats() { return JSON.parse(JSON.stringify(await vm.runInContext('ipcStats()', context))); },
    stop() { vm.runInContext('stop()', context); },
    // remote 只模拟指定接收transceiver的驱动事件；不伪装真实音频解码。
    remote(session) {
      context.targetSession = session;
      const view = vm.runInContext('views.get(targetSession)', context);
      const track = {kind: 'audio', readyState: 'live', stops: 0, stop() { this.readyState = 'ended'; this.stops++; }};
      view.peer.ontrack({track, transceiver: view.receiveTransceiver});
      return {track, sink: view.sink};
    },
    // remoteVideo 注入指定接收m-line事件；仅测试驱动隔离，不伪造实际H264解码。
    remoteVideo(session, wrong = false) {
      context.targetSession = session;
      const view = vm.runInContext('views.get(targetSession)', context);
      const track = {kind:'video',readyState:'live',stops:0,stop(){this.readyState='ended';this.stops++;}};
      view.peer.ontrack({track,transceiver:wrong ? {} : view.receiveVideoTransceiver});
      return {track,sink:view.remoteVideo,local:view.video};
    },
  };
}

// 固定PCM是显式软件诊断选项，不能改变普通页面的声学处理默认值。
// qualityReady 只模拟核心已建立投影；不会以测试Peer替代真实WebRTC验收。
async function qualityReady(f) {
  await f.incoming({kind:'ipcConfigure',sessionId:'viewer',audio:true,videoTrack:'video-1',audioTrack:'mic-1',releaseId:'1'});
  f.peers[0].connectionState='connected'; f.peers[0].onconnectionstatechange();
  await f.incoming({kind:'ipcActive',sessionId:'viewer'});
}

// 短暂媒体失联应保留原采集意图和Peer，等待C++确认后重协商；不能直接结束或重新申请驱动。
test('预览短断网保留原资源并报告原媒体代次', async () => {
  const f=fixture(); await qualityReady(f);
  const peer=f.peers[0]; peer.connectionState='disconnected'; peer.onconnectionstatechange();
  assert.equal(peer.closed,false);
  assert.equal(f.streams[0].getTracks().every(track=>track.readyState==='live'),true);
  assert.deepEqual(f.sent.at(-1),{kind:'ipcDisconnected',sessionId:'viewer',mediaRevision:1});
  assert.equal(f.captures.length,1); f.stop();
});

// 网络自行恢复可能先于原失联ACK；旧answer不得授权旧代次connected越过新SDP协商。
test('失联ACK之前旧Peer自然重连不抢报connected', async () => {
  const f=fixture(); await qualityReady(f);
  await f.incoming({kind:'ipcAnswer',sessionId:'viewer',sdp:'v=0\r\noriginal',releaseId:'1'});
  const peer=f.peers[0];
  peer.connectionState='disconnected'; peer.onconnectionstatechange();
  const count=f.sent.length;
  peer.connectionState='connected'; peer.onconnectionstatechange();
  assert.equal(f.sent.length,count);
  assert.equal(f.sent.at(-1).kind,'ipcDisconnected');
  assert.equal(peer.closed,false); f.stop();
});

// 重协商不重新getUserMedia、replaceTrack或开启已经暂停/停止的来源，旧answer不能作用新代次。
test('ICE重协商保留暂停来源且拒绝旧代次应答', async () => {
  const f=fixture(); await qualityReady(f);
  await f.incoming({kind:'ipcTrack',sessionId:'viewer',trackId:'video-1',token:'pause',operation:'pause',releaseId:'1'});
  const peer=f.peers[0], sender=peer.transceivers[0].sender;
  assert.equal(sender.track,null);
  peer.connectionState='disconnected'; peer.onconnectionstatechange();
  const replacements=sender.replacements.length;
  await f.incoming({kind:'ipcRecover',sessionId:'viewer',mediaRevision:2,releaseId:'1'});
  assert.equal(f.captures.length,1); assert.equal(f.peers.length,1);
  assert.equal(sender.track,null); assert.equal(sender.replacements.length,replacements);
  assert.equal(f.sent.at(-1).kind,'ipcOffer'); assert.equal(f.sent.at(-1).mediaRevision,2);
  await f.incoming({kind:'ipcAnswer',sessionId:'viewer',sdp:'v=0\r\nold',releaseId:'1'});
  assert.equal(peer.remoteDescription,undefined);
  peer.connectionState='connected'; peer.onconnectionstatechange();
  assert.notEqual(f.sent.at(-1).kind,'ipcConnected');
  await f.incoming({kind:'ipcAnswer',sessionId:'viewer',mediaRevision:2,sdp:'v=0\r\nnew',releaseId:'1'});
  peer.onconnectionstatechange();
  assert.deepEqual(f.sent.at(-1),{kind:'ipcConnected',sessionId:'viewer',mediaRevision:2});
  assert.equal(sender.track,null); f.stop();
});

// 暂停之外的停止意图同样保留；旧资源票据和跳代命令均不创建新采集。
test('ICE重协商不恢复已停止来源或接受跳代', async () => {
  const f=fixture(); await qualityReady(f);
  await f.incoming({kind:'ipcTrack',sessionId:'viewer',trackId:'video-1',token:'stop',operation:'stop',releaseId:'1'});
  const peer=f.peers[0], sender=peer.transceivers[0].sender;
  peer.connectionState='disconnected'; peer.onconnectionstatechange();
  await f.incoming({kind:'ipcRecover',sessionId:'viewer',mediaRevision:2,releaseId:'old'});
  await assert.rejects(f.incoming({kind:'ipcRecover',sessionId:'viewer',mediaRevision:3,releaseId:'1'}),/invalid_recovery/);
  await f.incoming({kind:'ipcRecover',sessionId:'viewer',mediaRevision:2,releaseId:'1'});
  assert.equal(sender.track,null);
  assert.equal(f.streams[0].getVideoTracks()[0].readyState,'ended');
  assert.equal(f.captures.length,1); assert.equal(f.peers.length,1); f.stop();
});

test('质量回执等待实际编码器完成并读回原sender参数', async () => {
  const f=fixture(); await qualityReady(f); let finish;
  f.quality(() => new Promise(resolve => {finish=resolve;}));
  const pending=f.incoming({kind:'ipcQuality',sessionId:'viewer',trackId:'video-1',token:'quality-1',profile:'low',releaseId:'1'});
  assert.equal(f.sent.filter(item=>item.kind==='ipcQualityApplied').length,0);
  assert.equal(f.peers[0].transceivers[0].sender.qualityCalls.length,1);
  finish(); await pending;
  assert.deepEqual(f.sent.at(-1),{kind:'ipcQualityApplied',sessionId:'viewer',token:'quality-1',state:'active',success:true});
  assert.equal(f.peers[0].transceivers[0].sender.getParameters().encodings[0].maxBitrate,150000);
  assert.equal(f.captures.length,1); assert.equal(f.peers[0].closed,false); f.stop();
});

test('质量驱动失败与旧租约回调均不停止主媒体', async () => {
  const f=fixture(); await qualityReady(f);
  f.quality(async () => {throw Error('encoder_failed');});
  await f.incoming({kind:'ipcQuality',sessionId:'viewer',trackId:'video-1',token:'quality-failed',profile:'high',releaseId:'1'});
  assert.equal(f.sent.at(-1).success,false); assert.equal(f.peers[0].closed,false);
  const before=f.sent.length;
  await f.incoming({kind:'ipcQuality',sessionId:'viewer',trackId:'video-1',token:'old',profile:'low',releaseId:'2'});
  assert.equal(f.sent.length,before); assert.equal(f.peers[0].closed,false); f.stop();
});

test('质量操作纯音频暂停视频且不越过显式暂停恢复媒体', async () => {
  const f=fixture(); await qualityReady(f);
  const command={kind:'ipcQuality',sessionId:'viewer',trackId:'video-1',token:'quality-1',profile:'audioOnly',releaseId:'1'};
  await f.incoming(command);
  const sender=f.peers[0].transceivers[0].sender;
  assert.equal(sender.track,null); assert.equal(f.sent.at(-1).state,'paused');
  await f.incoming({...command,token:'quality-2',profile:'standard'});
  assert.equal(sender.track.kind,'video'); assert.equal(f.sent.at(-1).state,'active');
  await f.incoming({kind:'ipcTrack',sessionId:'viewer',trackId:'video-1',token:'pause',operation:'pause',releaseId:'1'});
  await f.incoming({...command,token:'quality-3',profile:'high'});
  assert.equal(sender.track,null); assert.equal(f.sent.at(-1).state,'paused'); f.stop();
});

test('关闭途中完成编码调整不产生成功回执或复活轨', async () => {
  const f=fixture(); await qualityReady(f); let finish;
  f.quality(() => new Promise(resolve => {finish=resolve;}));
  const pending=f.incoming({kind:'ipcQuality',sessionId:'viewer',trackId:'video-1',token:'late',profile:'high',releaseId:'1'});
  await f.incoming({kind:'ipcStop',sessionId:'viewer',releaseId:'1'});
  finish(); await pending;
  assert.equal(f.sent.filter(item=>item.kind==='ipcQualityApplied').length,0);
  assert.ok(f.streams[0].getTracks().every(track=>track.readyState==='ended')); f.stop();
});

test('普通IPC采集保留三项声学处理默认开启', async () => {
  const f = fixture('http://127.0.0.1:19002/');
  await f.incoming({kind:'ipcConfigure',sessionId:'talk',audio:true,videoTrack:'',audioTrack:'mic-1',speakerTrack:'speaker-1',releaseId:'1'});
  assert.deepEqual(JSON.parse(JSON.stringify(f.captures[0].audio)), {echoCancellation:true,noiseSuppression:true,autoGainControl:true});
  f.stop();
});

test('显式本机固定PCM模式关闭三项处理，不把采集设置冒充AI AEC能力', async () => {
  const f = fixture('http://127.0.0.1:19002/#softwareFixedPcm=1');
  await f.incoming({kind:'ipcConfigure',sessionId:'talk',audio:true,videoTrack:'',audioTrack:'mic-1',speakerTrack:'speaker-1',releaseId:'1'});
  assert.deepEqual(JSON.parse(JSON.stringify(f.captures[0].audio)), {echoCancellation:false,noiseSuppression:false,autoGainControl:false});
  assert.equal(f.sent.some(item=>Object.hasOwn(item,'aecMode')),false);
  f.stop();
});

test('固定PCM轨停止后重发布仍使用原显式采集策略', async () => {
  const f = fixture('http://127.0.0.1:19002/#softwareFixedPcm=1');
  await f.incoming({kind:'ipcConfigure',sessionId:'talk',audio:true,videoTrack:'',audioTrack:'mic-1',speakerTrack:'speaker-1',releaseId:'1'});
  f.peers[0].connectionState='connected';f.peers[0].onconnectionstatechange();
  await f.incoming({kind:'ipcActive',sessionId:'talk'});
  await f.incoming({kind:'ipcTrack',sessionId:'talk',trackId:'mic-1',token:'stop-original',operation:'stop'});
  await f.incoming({kind:'ipcTrack',sessionId:'talk',trackId:'mic-1',token:'publish-original',operation:'publish'});
  assert.equal(f.captures.length,2);
  assert.deepEqual(JSON.parse(JSON.stringify(f.captures[1].audio)), {echoCancellation:false,noiseSuppression:false,autoGainControl:false});
  f.stop();
});

test('非回环页面及重复选项不得隐式启用固定PCM模式', async () => {
  for (const href of ['https://example.invalid/#softwareFixedPcm=1','http://127.0.0.1:19002/#softwareFixedPcm=1&softwareFixedPcm=1',
    'http://127.0.0.1:19002/?softwareFixedPcm=1']) {
    const f=fixture(href);
    await f.incoming({kind:'ipcConfigure',sessionId:'talk',audio:true,videoTrack:'',audioTrack:'mic-1',speakerTrack:'speaker-1',releaseId:'1'});
    assert.deepEqual(JSON.parse(JSON.stringify(f.captures[0].audio)), {echoCancellation:true,noiseSuppression:true,autoGainControl:true});f.stop();
  }
});

// humanReady 仅模拟正式C++的本地邀请状态；用户接听请求自身不启动采集。
async function humanReady(f, sessionId = 'source-leg') {
  await f.incoming({kind: 'callState', state: 'ringing', sessionId});
  f.context.targetSession = sessionId;
  vm.runInContext('invitations.get(targetSession).children[0].onclick()', f.context);
}

// 平台选中的接收能力才创建第三条m-line，不能将本地预览当作对端视频。
test('显式人际视频接收建立独立recvonly轨，旧三轨不增加权限', async () => {
  const f = fixture(); await humanReady(f);
  await f.incoming({kind:'callConfigure',sessionId:'source-leg',releaseId:'1',cameraId:'entry',videoReceive:true});
  assert.equal(f.peers[0].transceivers.length,3);
  assert.equal(f.peers[0].transceivers[2].options.direction,'recvonly');
  assert.equal(f.peers[0].transceivers[2].track,'video');
  f.stop();
});

// 接收画面和本地预览必须分离；媒体连通不等同于平台建立，终态必须实际停止接收轨。
test('人际远程视频只在正式建立后播放且停止释放接收资源', async () => {
  const f=fixture(); await humanReady(f);
  await f.incoming({kind:'callConfigure',sessionId:'source-leg',releaseId:'1',cameraId:'entry',videoReceive:true});
  const remote=f.remoteVideo('source-leg'); assert.notEqual(remote.sink,remote.local);
  assert.equal(remote.sink.plays,0);
  f.peers[0].connectionState='connected'; f.peers[0].onconnectionstatechange();
  await f.incoming({kind:'callState',state:'connected',sessionId:'source-leg'});
  assert.equal(remote.sink.plays,1);
  await f.incoming({kind:'callStop',sessionId:'source-leg',releaseId:'1'});
  assert.equal(remote.track.readyState,'ended'); assert.equal(remote.sink.srcObject,null);
  assert.equal(f.peers[0].closed,true); f.stop();
});

// 非选中轨、重复轨和终态迟到轨不能借浏览器回调扩展授权。
test('未经视频接收授权及错误m-line必须拒绝并释放', async () => {
  for (const selected of [false,true]) {
    const f=fixture(); await humanReady(f);
    await f.incoming({kind:'callConfigure',sessionId:'source-leg',releaseId:'1',cameraId:'entry',...(selected?{videoReceive:true}:{})});
    const remote=f.remoteVideo('source-leg',true);
    assert.equal(remote.track.readyState,'ended'); assert.equal(f.peers[0].closed,true);
    assert.equal(f.sent.at(-1).kind,'callFailure'); f.stop();
  }
});

test('重复远程视频与终态迟到轨均实际停止，不覆盖原轨', async () => {
  const f=fixture(); await humanReady(f);
  await f.incoming({kind:'callConfigure',sessionId:'source-leg',releaseId:'1',cameraId:'entry',videoReceive:true});
  const first=f.remoteVideo('source-leg'),peer=f.peers[0];
  const duplicate=f.remoteVideo('source-leg');
  assert.equal(first.track.readyState,'ended'); assert.equal(duplicate.track.readyState,'ended');
  const late={kind:'video',stops:0,stop(){this.stops++;}};
  peer.ontrack({track:late,transceiver:{}}); assert.equal(late.stops,1); f.stop();
});

test('视频接收开关拒绝非布尔值及额外扩权字段', async () => {
  for (const value of [false,'true',null,1]) {
    const f=fixture(); await humanReady(f);
    await assert.rejects(f.incoming({kind:'callConfigure',sessionId:'source-leg',releaseId:'1',cameraId:'entry',videoReceive:value}),/invalid_call_configure/);
    assert.equal(f.captures.length,0); f.stop();
  }
});

// main投影的实际分支表达式决定是否发出本地callState，直接提取正式CPP而非复制一份宿主算法。
test('共享宿主同轮结束再邀请即使枚举相同，也必须投影新的原session', () => {
  const source = fs.readFileSync(path.join(__dirname,'main.cpp'),'utf8');
  const matched = /if\s*\((human\.state\(\)[^{;]*?)\)\s*\{\s*human_previous\s*=\s*human\.state\(\)/.exec(source);
  assert.ok(matched,'actual_human_projection_branch_required');
  const expression = matched[1];
  const context = vm.createContext({human:{state:()=>7,session_id:()=> 'new-leg'},
    human_previous:7,human_previous_session:'old-leg'});
  assert.equal(vm.runInContext(expression,context),true,'different_original_leg_must_emit_state');
});

// 旧振铃没有采集资源，源Ended与新Invite同轮时只替换确认控件，不能开启媒体或关闭宿主。
test('未接听旧人际确认可由已验证新腿替换，同枚举新邀请不关闭共享宿主', async () => {
  const f = fixture();
  await f.incoming({kind:'callState',state:'ringing',sessionId:'old-leg'});
  await f.incoming({kind:'callState',state:'ringing',sessionId:'new-leg'});
  assert.equal(vm.runInContext('humanSession',f.context),'new-leg');
  assert.equal(vm.runInContext('invitations.has("old-leg")',f.context),false);
  assert.equal(vm.runInContext('invitations.has("new-leg")',f.context),true);
  assert.equal(f.captures.length,0); assert.equal(f.sent.length,0);
  f.stop();
});

// 实际旧媒体还存在时新邀请不能清其资源票据或开第二组麦克风，也不能停止其他预览。
test('共享新腿不能覆盖未实际停止旧媒体，独立观看者不受影响', async () => {
  const f = fixture();
  await f.incoming({kind:'ipcConfigure',sessionId:'viewer',audio:false,videoTrack:'video-1',releaseId:'1'});
  await humanReady(f,'old-leg');
  await f.incoming({kind:'callConfigure',sessionId:'old-leg',releaseId:'2',cameraId:'entry'});
  await assert.rejects(f.incoming({kind:'callState',state:'ringing',sessionId:'new-leg'}));
  assert.equal(f.captures.length,2); assert.equal(f.peers[0].closed,false); assert.equal(f.peers[1].closed,false);
  assert.equal(vm.runInContext('humanSession',f.context),'old-leg');
  f.stop();
});

// 原Peer已移除但原getUserMedia还在途时，retiring也必须拒绝新绑定；真正Stopped后才可替换控件。
test('共享旧腿晚到采集仍占原身份，新振铃不能越过retiring停止围栏', async () => {
  const f = fixture(); let finish;
  f.capture(() => new Promise(resolve => {finish=resolve;}));
  await humanReady(f,'old-leg');
  const starting = f.incoming({kind:'callConfigure',sessionId:'old-leg',releaseId:'1',cameraId:'entry'});
  const stopping = f.incoming({kind:'callStop',sessionId:'old-leg',releaseId:'1'});
  await assert.rejects(f.incoming({kind:'callState',state:'ringing',sessionId:'new-leg'}));
  assert.equal(f.captures.length,1);
  assert.equal(f.sent.filter(item=>item.kind==='callStopped').length,0);
  const stream=f.stream(true);finish(stream);
  await starting;await stopping;
  assert.ok(stream.getTracks().every(track=>track.readyState==='ended'));
  await f.incoming({kind:'callState',state:'ringing',sessionId:'new-leg'});
  assert.equal(vm.runInContext('humanSession',f.context),'new-leg');
  assert.equal(f.captures.length,1);
  f.stop();
});

// 门铃源必须再次明确接听；直到C++确认接听ACK并configure才允许真实采集申请。
test('共享人际来电本地明确接听与双mline布局，不由门铃或邀请开启媒体', async () => {
  const f = fixture();
  await humanReady(f);
  assert.deepEqual(f.sent, [{kind: 'callAccept', sessionId: 'source-leg'}]);
  assert.equal(f.captures.length, 0);
  await f.incoming({kind: 'callConfigure', sessionId: 'source-leg', releaseId: '1', cameraId: 'entry'});
  assert.equal(f.captures.length, 1);
  assert.equal(f.peers[0].transceivers.length, 2);
  assert.deepEqual(f.peers[0].transceivers.map(item => item.options.direction), ['sendonly', 'sendrecv']);
  assert.equal(f.sent.at(-1).kind, 'callOffer');
  assert.equal(f.sent.at(-1).releaseId, '1');
  assert.equal((await f.stats())[0].active, false);
  f.stop();
});

// 原异步getUserMedia未完成时Stop不能提前ACK；晚到轨必须先stop，再交原票据。
test('共享人际实际Stopped等待晚到采集，原票据只释放原腿', async () => {
  const f = fixture(); let finish;
  f.capture(() => new Promise(resolve => { finish = resolve; }));
  await humanReady(f);
  const starting = f.incoming({kind: 'callConfigure', sessionId: 'source-leg', releaseId: '1', cameraId: 'entry'});
  await f.incoming({kind: 'callStop', sessionId: 'source-leg', releaseId: '2'});
  assert.equal(f.sent.filter(item => item.kind === 'callStopped').length, 0);
  const stopping = f.incoming({kind: 'callStop', sessionId: 'source-leg', releaseId: '1'});
  assert.equal(f.sent.filter(item => item.kind === 'callStopped').length, 0);
  const stream = f.stream(true); finish(stream);
  await starting; await stopping;
  assert.ok(stream.getTracks().every(track => track.readyState === 'ended'));
  assert.deepEqual(f.sent.at(-1), {kind: 'callStopped', sessionId: 'source-leg', releaseId: '1'});
  assert.equal(f.peers.length, 0);
  f.stop();
});

// 只源video上行和单audio双向；原平台建立后才播放接收音频，不能干扰独立观看者。
test('共享人际音频实际连通与平台建立分开，挂断不停止其它观看者', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'viewer', audio: false, videoTrack: 'video-1', releaseId: '1'});
  await humanReady(f);
  await f.incoming({kind: 'callConfigure', sessionId: 'source-leg', releaseId: '2', cameraId: 'entry'});
  await f.incoming({kind: 'callAnswer', sessionId: 'source-leg', releaseId: 'wrong', sdp: 'v=0\r\nwrong'});
  assert.equal(f.peers[1].remoteDescription, undefined);
  await f.incoming({kind: 'callAnswer', sessionId: 'source-leg', releaseId: '2', sdp: 'v=0\r\nanswer'});
  f.peers[1].connectionState = 'connected'; f.peers[1].onconnectionstatechange();
  const remote = f.remote('source-leg');
  assert.equal(remote.sink.plays, 0);
  await f.incoming({kind: 'callState', state: 'connected', sessionId: 'source-leg'});
  assert.equal(remote.sink.plays, 1);
  assert.ok((await f.stats()).find(item => item.human).playing);
  await f.incoming({kind: 'callStop', sessionId: 'source-leg', releaseId: '2'});
  assert.equal(f.peers[1].closed, true); assert.equal(remote.track.readyState, 'ended');
  assert.equal(f.peers[0].closed, false);
  assert.equal(f.streams[0].getTracks()[0].readyState, 'live');
  assert.deepEqual(f.sent.at(-1), {kind: 'callStopped', sessionId: 'source-leg', releaseId: '2'});
  f.stop();
});

// 共享纯音频IPC原票据同样要等实际轨结束，旧session或旧releaseId不能释放新Peer。
test('共享IPC实际Stopped与旧票据隔离', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, audioTrack: 'mic-1', speakerTrack: 'speaker-1', videoTrack: '', releaseId: '1'});
  await f.incoming({kind: 'ipcStop', sessionId: 'talk', releaseId: '9'});
  assert.equal(f.peers[0].closed, false);
  await f.incoming({kind: 'ipcStop', sessionId: 'talk', releaseId: '1'});
  assert.equal(f.streams[0].getTracks()[0].readyState, 'ended');
  assert.deepEqual(f.sent.at(-1), {kind: 'ipcStopped', sessionId: 'talk', releaseId: '1'});
  await f.incoming({kind: 'ipcConfigure', sessionId: 'next', audio: true, audioTrack: 'mic-1', speakerTrack: 'speaker-1', videoTrack: '', releaseId: '2'});
  await f.incoming({kind: 'ipcStop', sessionId: 'talk', releaseId: '1'});
  assert.equal(f.peers[1].closed, false);
  f.stop();
});

// 停止后重发布也是原在途采集，不允许actualStopped越过晚到重发布申请。
test('共享IPC重发布晚到采集也属于原停止围栏', async () => {
  const f = fixture(); let finish;
  await f.incoming({kind: 'ipcConfigure', sessionId: 'viewer', audio: false, videoTrack: 'video-1', releaseId: '1'});
  f.peers[0].connectionState = 'connected'; f.peers[0].onconnectionstatechange();
  await f.incoming({kind: 'ipcActive', sessionId: 'viewer'});
  await f.incoming({kind: 'ipcTrack', sessionId: 'viewer', token: 'stop-track', trackId: 'video-1', operation: 'stop'});
  f.capture(() => new Promise(resolve => { finish = resolve; }));
  const publishing = f.incoming({kind: 'ipcTrack', sessionId: 'viewer', token: 'publish-track', trackId: 'video-1', operation: 'publish'});
  const stopping = f.incoming({kind: 'ipcStop', sessionId: 'viewer', releaseId: '1'});
  assert.equal(f.sent.filter(item => item.kind === 'ipcStopped').length, 0);
  const fresh = f.stream(false); finish(fresh);
  await publishing; await stopping;
  assert.equal(fresh.getTracks()[0].readyState, 'ended');
  assert.deepEqual(f.sent.at(-1), {kind: 'ipcStopped', sessionId: 'viewer', releaseId: '1'});
  assert.equal(f.sent.filter(item => item.token === 'publish-track' && item.success).length, 0);
  f.stop();
});

// 上游只有音频接收权限，异常视频轨必须立即停止而非静默播放。
test('共享门铃源拒绝反向视频轨与非法本地命令', async () => {
  const f = fixture();
  await assert.rejects(f.incoming({kind: 'callConfigure', sessionId: 'source-leg', releaseId: '1', cameraId: 'entry'}));
  assert.equal(f.captures.length, 0);
  await humanReady(f);
  await f.incoming({kind: 'callConfigure', sessionId: 'source-leg', releaseId: '1', cameraId: 'entry'});
  const track = {kind: 'video', stops: 0, stop() { this.stops++; }};
  f.peers[0].ontrack({track, transceiver: f.peers[0].transceivers[0]});
  assert.equal(track.stops, 1); assert.equal(f.peers[0].closed, true);
  assert.equal(f.sent.at(-1).kind, 'callFailure');
  await f.incoming({kind: 'callStop', sessionId: 'source-leg', releaseId: '1'});
  assert.equal(f.sent.at(-1).kind, 'callStopped');
  f.stop();
});

// RTC总采样包含丢包补偿，只有减去concealedSamples才能作为实际媒体解码计数。
test('音频解码计数排除补偿采样，缺失指标不伪造解码事实', async () => {
  const f = fixture();
  await f.incoming({kind:'ipcConfigure',sessionId:'view',audio:true,videoTrack:'video-1',audioTrack:'mic-1'});
  for (const [totalSamplesReceived,concealedSamples,expected] of [[16000,4000,12000],[1000,2000,0],[20000,undefined,0]]) {
    f.peers[0].getStats = async () => new Map([['audio',{type:'inbound-rtp',kind:'audio',totalSamplesReceived,concealedSamples}]]);
    assert.equal((await f.stats())[0].audioSamples,expected);
  }
  f.stop();
});

// 源控制等待sender实际替换，停止后重新发布只重采集当前源，不关闭其它观看者。
test('源轨暂停恢复与停止重发布按真实驱动完成回执', async () => {
  const f = fixture();
  for (const sessionId of ['a', 'b']) {
    await f.incoming({kind: 'ipcConfigure', sessionId, audio: false, videoTrack: 'video-1'});
    f.peers[f.peers.length - 1].connectionState = 'connected';
    f.peers[f.peers.length - 1].onconnectionstatechange();
    await f.incoming({kind: 'ipcActive', sessionId});
  }
  const sender = f.peers[0].transceivers[0].sender;
  for (const [operation, state] of [['pause', 'paused'], ['resume', 'active'], ['stop', 'stopped'], ['publish', 'active']]) {
    await f.incoming({kind: 'ipcTrack', sessionId: 'a', token: operation, trackId: 'video-1', operation});
    assert.deepEqual(f.sent.at(-1), {kind: 'ipcTrackApplied', sessionId: 'a', token: operation, state, success: true});
    assert.equal(f.peers[1].closed, false);
    assert.equal(f.streams[1].getTracks()[0].stops, 0);
    if (state !== 'active') assert.equal(sender.track, null);
    else assert.equal(sender.track.readyState, 'live');
  }
  assert.equal(f.streams.length, 3);
  f.stop();
  assert.equal(sender.track.readyState, 'ended');
});

// 控制未授权音轨不能开启新麦克风，错误只释放当前观看者。
test('未授权源轨控制回报失败且不采集音频', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: false, videoTrack: 'video-1'});
  await f.incoming({kind: 'ipcTrack', sessionId: 'a', token: 'bad', trackId: 'audio-1', operation: 'publish'});
  assert.equal(f.sent.at(-1).success, false);
  assert.equal(f.streams.length, 1);
  assert.equal(f.streams[0].getTracks()[0].readyState, 'ended');
});

// replaceTrack失败不回报成功；迟到驱动完成也不能恢复已结束会话。
test('源轨驱动失败与关闭后迟到完成不能伪报成功', async () => {
  for (const late of [false, true]) {
    const f = fixture();
    await f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: false, videoTrack: 'video-1'});
    f.peers[0].connectionState = 'connected'; f.peers[0].onconnectionstatechange();
    await f.incoming({kind: 'ipcActive', sessionId: 'a'});
    let resolve;
    f.peers[0].transceivers[0].sender.replaceTrack = late ? () => new Promise(done => { resolve = done; }) : async () => { throw Error('device_failure'); };
    const control = f.incoming({kind: 'ipcTrack', sessionId: 'a', token: 'test', trackId: 'video-1', operation: 'pause'});
    if (late) { f.stop(); resolve(); }
    await control;
    assert.equal(f.sent.filter(message => message.kind === 'ipcTrackApplied' && message.success).length, 0);
    if (!late) assert.equal(f.sent.at(-1).success, false);
    assert.equal(f.streams[0].getTracks()[0].readyState, 'ended');
  }
});

// 多观看者只发送视频，关闭一端不会停止其它端的Peer或源。
test('独立观看者使用sendonly，默认不采集麦克风', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: false, videoTrack: 'video-1'});
  await f.incoming({kind: 'ipcConfigure', sessionId: 'b', audio: false, videoTrack: 'video-1'});
  assert.equal(f.streams.length, 2);
  for (const peer of f.peers) {
    assert.equal(peer.transceivers.length, 1);
    assert.equal(peer.transceivers[0].options.direction, 'sendonly');
    assert.equal(peer.transceivers[0].track.kind, 'video');
  }
  await f.incoming({kind: 'ipcAnswer', sessionId: 'a', sdp: 'v=0\r\nanswer'});
  f.peers[0].connectionState = 'connected';
  f.peers[0].onconnectionstatechange(); f.peers[0].onconnectionstatechange();
  assert.equal(f.sent.filter(value => value.kind === 'ipcConnected').length, 1);
  assert.equal((await f.stats())[0].active, false);
  await f.incoming({kind: 'ipcActive', sessionId: 'a'});
  assert.equal((await f.stats())[0].active, true);
  await f.incoming({kind: 'ipcStop', sessionId: 'a'});
  assert.equal(f.peers[0].closed, true);
  assert.equal(f.streams[0].getTracks()[0].stops, 1);
  assert.equal(f.peers[1].closed, false);
  assert.equal(f.streams[1].getTracks()[0].stops, 0);
  await f.incoming({kind: 'ipcAnswer', sessionId: 'a', sdp: 'v=0\r\nlate'});
  assert.equal(f.peers.length, 2);
  const stats = await f.stats();
  assert.equal(stats.length, 1); assert.equal(stats[0].sessionId, 'b');
  f.stop();
  assert.equal(f.streams[1].getTracks()[0].stops, 1);
});

// 采集授权尚未返回时收到关闭，晚到流必须真正停止，不产生Peer或offer。
test('等待采集期间关闭，晚到资源不得复活', async () => {
  const f = fixture();
  let complete;
  f.capture(() => new Promise(resolve => { complete = resolve; }));
  const pending = f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: false, videoTrack: 'video-1'});
  await f.incoming({kind: 'ipcStop', sessionId: 'a'});
  complete(f.stream(false)); await pending;
  assert.equal(f.streams[0].getTracks()[0].stops, 1);
  assert.equal(f.peers.length, 0); assert.equal(f.sent.length, 0);
});

// 不支持H264应停流并明确报告失败，不静默切换其它视频格式。
test('缺少H264拒绝且真实停止部分采集', async () => {
  const f = fixture(); f.h264(false);
  await f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: false, videoTrack: 'video-1'});
  assert.equal(f.streams[0].getTracks()[0].stops, 1);
  assert.equal(f.peers[0].closed, true);
  assert.deepEqual(f.sent, [{kind: 'ipcFailure', sessionId: 'a'}]);
});

// 监听音频仅在明确授权时发布；桥故障立即关闭所有真实轨。
test('显式监听音频与桥关闭释放', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'a', audio: true, videoTrack: 'video-1', audioTrack: 'audio-1'});
  assert.equal(f.peers[0].transceivers.length, 2);
  for (const item of f.peers[0].transceivers) assert.equal(item.options.direction, 'sendonly');
  f.stop(); f.stop();
  assert.ok(f.streams[0].getTracks().every(track => track.stops === 1));
});

// 重投邀请不能生成重复确认按钮；没有开始媒体的邀请也需要终态清理。
test('邀请去重与尚未采集时的终态', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcInvite', sessionId: 'a'});
  await f.incoming({kind: 'ipcInvite', sessionId: 'a'});
  assert.equal(vm.runInContext('invitations.size', f.context), 1);
  assert.equal(f.streams.length, 0);
  await f.incoming({kind: 'ipcStop', sessionId: 'a'});
  assert.equal(vm.runInContext('invitations.size', f.context), 0);
  f.stop();
});

// 对讲授权只打开麦克风，实际接收轨在平台建立后才播放，独立终止不影响观看。
test('独立对讲音频方向、建立前不播放及精确清理', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'view', audio: false, videoTrack: 'video-1'});
  await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
  assert.equal(f.captures[1].video, false);
  assert.equal(f.captures[1].audio.echoCancellation, true);
  assert.equal(f.streams[1].getTracks().length, 1);
  assert.equal(f.streams[1].getTracks()[0].kind, 'audio');
  const peer = f.peers[1];
  assert.deepEqual(peer.transceivers.map(t => t.options.direction), ['sendonly', 'recvonly']);
  assert.ok(peer.transceivers.every(t => t.codecs[0].mimeType === 'audio/opus'));
  const remote = f.remote('talk');
  assert.equal(remote.sink.plays, 0);
  peer.connectionState = 'connected'; peer.onconnectionstatechange();
  assert.equal(remote.sink.plays, 0);
  await f.incoming({kind: 'ipcActive', sessionId: 'talk'});
  assert.equal(remote.sink.plays, 1);
  assert.equal((await f.stats()).find(s => s.sessionId === 'talk').playing, true);
  await f.incoming({kind: 'ipcStop', sessionId: 'talk'});
  assert.equal(remote.track.stops, 1); assert.equal(remote.sink.pauses, 1);
  assert.equal(remote.sink.srcObject, null); assert.equal(peer.closed, true);
  assert.equal(f.streams[1].getTracks()[0].stops, 1);
  assert.equal(f.peers[0].closed, false); assert.equal(f.streams[0].getTracks()[0].stops, 0);
  f.stop();
});

test('对讲不隐式开视频、双输出与非法轨计划拒绝且不采集', async () => {
  for (const fields of [{videoTrack: 'video-1'}, {audio: false}, {audioTrack: 'speaker-1'}, {speakerTrack: 'bad\n'}]) {
    const f = fixture();
    await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1', ...fields});
    assert.equal(f.captures.length, 0);
    assert.deepEqual(f.sent, [{kind: 'ipcFailure', sessionId: 'talk'}]);
  }
  const f = fixture();
  for (const sessionId of ['first', 'second'])
    await f.incoming({kind: 'ipcConfigure', sessionId, audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
  assert.equal(f.streams.length, 1); assert.equal(f.peers[0].closed, false);
  assert.equal(f.sent.at(-1).kind, 'ipcFailure');
  f.stop();
});

test('播放拒绝及缺少Opus不伪报成功，晚到麦克风实际停止', async () => {
  const configuration = {kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'};
  const denied = fixture();
  denied.playback(async () => { throw Error('autoplay_denied'); });
  await denied.incoming(configuration);
  const remote = denied.remote('talk');
  denied.peers[0].connectionState = 'connected'; denied.peers[0].onconnectionstatechange();
  await denied.incoming({kind: 'ipcActive', sessionId: 'talk'});
  assert.equal(remote.track.stops, 1); assert.equal(denied.peers[0].closed, true);
  assert.equal(denied.sent.at(-1).kind, 'ipcFailure');
  const missing = fixture(); missing.opus(false);
  await missing.incoming(configuration);
  assert.equal(missing.streams[0].getTracks()[0].stops, 1);
  assert.equal(missing.sent.at(-1).kind, 'ipcFailure');
  const late = fixture(); let complete;
  late.capture(() => new Promise(resolve => { complete = resolve; }));
  const pending = late.incoming(configuration);
  await late.incoming({kind: 'ipcStop', sessionId: 'talk'});
  complete(late.stream(true, false)); await pending;
  assert.equal(late.streams[0].getTracks()[0].stops, 1);
  assert.equal(late.peers.length, 0); assert.equal(late.sent.length, 0);
});

// 真实媒体验证入口只能调用本次回环测试服务，不接受远程源、凭据URL或借用生产环境。
test('真实对讲媒体测试限定回环目标', () => {
  const {localEndpoint} = require('./talk_media_live.cjs');
  assert.equal(localEndpoint('http://127.0.0.1:12345'), 'http://127.0.0.1:12345');
  for (const target of ['https://127.0.0.1:12345', 'http://localhost:12345', 'http://example.com:12345',
    'http://127.0.0.1:0', 'http://127.0.0.1', 'http://user:secret@127.0.0.1:12345',
    'http://127.0.0.1:12345/source', 'http://127.0.0.1:12345?token=x', 'http://127.0.0.1:12345#x'])
    assert.throws(() => localEndpoint(target));
});

// 扬声器是接收端口，不能替换麦克风sender或重建Peer；重复建立事件不能解除暂停。
test('对讲扬声器独立暂停恢复，不停止麦克风及其他观看者', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'view', audio: false, videoTrack: 'video-1'});
  await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
  const peer = f.peers[1], sender = peer.transceivers[0].sender;
  const remote = f.remote('talk');
  peer.connectionState = 'connected'; peer.onconnectionstatechange();
  await f.incoming({kind: 'ipcActive', sessionId: 'talk'});
  await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'pause', trackId: 'speaker-1', operation: 'pause'});
  assert.deepEqual(f.sent.at(-1), {kind: 'ipcTrackApplied', sessionId: 'talk', token: 'pause', state: 'paused', success: true});
  assert.equal(remote.sink.pauses, 1);
  assert.equal(remote.track.enabled, false);
  assert.equal((await f.stats()).find(s => s.sessionId === 'talk').playing, false);
  await f.incoming({kind: 'ipcActive', sessionId: 'talk'});
  assert.equal(remote.sink.plays, 1);
  await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'resume', trackId: 'speaker-1', operation: 'resume'});
  assert.deepEqual(f.sent.at(-1), {kind: 'ipcTrackApplied', sessionId: 'talk', token: 'resume', state: 'active', success: true});
  assert.equal(remote.sink.plays, 2);
  assert.equal(remote.track.enabled, true);
  assert.equal(sender.replacements.length, 0);
  assert.equal(f.streams[1].getTracks()[0].stops, 0);
  assert.equal(remote.track.stops, 0);
  assert.equal(f.captures.length, 2);
  assert.equal(f.peers[0].closed, false);
  assert.equal(peer.closed, false);
  f.stop();
});

// 缺少实际接收轨时暂停仍应生效，晚到音频不能自动播放；恢复才开始播放。
test('对讲扬声器暂停屏障覆盖晚到接收轨', async () => {
  const f = fixture();
  await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
  f.peers[0].connectionState = 'connected'; f.peers[0].onconnectionstatechange();
  await f.incoming({kind: 'ipcActive', sessionId: 'talk'});
  await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'pause', trackId: 'speaker-1', operation: 'pause'});
  assert.equal(f.sent.at(-1).success, true);
  const remote = f.remote('talk');
  await Promise.resolve();
  assert.equal(remote.sink.plays, 0);
  assert.equal(remote.track.enabled, false);
  await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'resume', trackId: 'speaker-1', operation: 'resume'});
  assert.equal(remote.sink.plays, 1);
  assert.equal(f.sent.at(-1).state, 'active');
  f.stop();
});

// 恢复拒绝或挂断后的迟到play完成必须释放本腿，不能发送成功回执或复活输出。
test('对讲扬声器恢复失败及迟到播放完成不伪报成功', async () => {
  for (const late of [false, true]) {
    const f = fixture();
    await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
    const remote = f.remote('talk');
    f.peers[0].connectionState = 'connected'; f.peers[0].onconnectionstatechange();
    await f.incoming({kind: 'ipcActive', sessionId: 'talk'});
    await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'pause', trackId: 'speaker-1', operation: 'pause'});
    let complete;
    f.playback(late ? () => new Promise(resolve => { complete = resolve; }) : async () => { throw Error('playback_denied'); });
    const pending = f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'resume', trackId: 'speaker-1', operation: 'resume'});
    if (late) { await Promise.resolve(); await f.incoming({kind: 'ipcStop', sessionId: 'talk'}); complete(); }
    await pending;
    assert.equal(f.sent.filter(s => s.token === 'resume' && s.success).length, 0);
    if (!late) assert.equal(f.sent.at(-1).success, false);
    assert.equal(remote.track.stops, 1);
    assert.equal(remote.sink.srcObject, null);
    assert.equal(f.peers[0].closed, true);
  }
});

// 首次play尚未结束时暂停再恢复，恢复回执必须等待同一播放结果，不能因rendering布尔值提前成功。
test('对讲首次播放与暂停恢复交错时等待实际播放结果', async () => {
  for (const reject of [false, true]) {
    const f = fixture();
    await f.incoming({kind: 'ipcConfigure', sessionId: 'talk', audio: true, videoTrack: '', audioTrack: 'mic-1', speakerTrack: 'speaker-1'});
    f.remote('talk');
    f.peers[0].connectionState = 'connected'; f.peers[0].onconnectionstatechange();
    let complete, fail;
    f.playback(() => new Promise((resolve, rejection) => { complete = resolve; fail = rejection; }));
    const active = f.incoming({kind: 'ipcActive', sessionId: 'talk'});
    await f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'pause', trackId: 'speaker-1', operation: 'pause'});
    const resume = f.incoming({kind: 'ipcTrack', sessionId: 'talk', token: 'resume', trackId: 'speaker-1', operation: 'resume'});
    await Promise.resolve();
    assert.equal(f.sent.filter(s => s.token === 'resume').length, 0);
    if (reject) fail(Error('playback_denied')); else complete();
    await Promise.all([active, resume]);
    assert.equal(f.sent.filter(s => s.token === 'resume' && s.success).length, reject ? 0 : 1);
    f.stop();
  }
});
