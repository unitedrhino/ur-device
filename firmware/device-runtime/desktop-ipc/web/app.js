// 文件说明：观看仅发布授权媒体，对讲独立收发音频；C++控制授权与会话，终态停止真实轨。
'use strict';
const views = new Map();
const invitations = new Map(); // 本地确认控件按会话去重，终态移除但不提前采集。
const retiringViews = new Map(); // 新共享宿主的原资源票据，晚到采集清理前不能回报Stopped。
let humanSession = ''; // 当前共享人际腿，与所有预览/对讲独立session。
let socket;
let closed = false;
let available = false;
let doorbellAvailable = false; // 原事件下行SUBACK就绪，与IPC capability和媒体状态独立。
let doorbellPending = false; // 原按压待确认时只禁止重复点击，不生成重试token。
let privacyAvailable = false; // 原属性下行SUBACK就绪，不借IPC或事件出口状态。
let privacyPending = false; // 单个原隐私意图在途时禁止发送相反报告。
// privacyButtons 只投影本地出口可用性，不切断或重新打开任何媒体轨。
function privacyButtons() {
  for (const id of ['privacy-enable', 'privacy-disable'])
    document.getElementById(id).disabled = closed || !privacyAvailable || privacyPending;
}
// privacyStatus 区分业务拒绝与超时未知，属性ACK绝不是实际停止票据。
function privacyStatus(state) {
  const texts = {
    pending: '隐私属性等待平台确认；不代表采集已经停止。',
    ack: '隐私属性已获平台确认；实际停流另行确认，旧媒体不会自动恢复。',
    rejected: '隐私属性上报已被拒绝；不能确认停流或关闭隐私。',
    unknown: '隐私属性结果未知；不能确认停流或关闭隐私。',
  };
  if (!Object.hasOwn(texts, state)) throw Error('invalid_privacy_state');
  privacyPending = state === 'pending';
  privacyButtons(); document.getElementById('privacy-status').textContent = texts[state];
}
// softwareFixedPcmMode 只允许本机页面显式选择固定PCM软件测试，普通设备保持声学处理默认开启。
// 该本地驱动选项不改变MQTT能力、授权或媒体状态，也不表示硬件AEC已验证。
function softwareFixedPcmMode() {
  if (!window.location) return false;
  const url = new URL(window.location.href);
  // fragment不发送到HTTP服务，严格保持原根资产白名单；旧query不会启用本地选项。
  return url.protocol === 'http:' && url.hostname === '127.0.0.1' && url.hash === '#softwareFixedPcm=1';
}
// captureAudioOptions 返回明确的采集处理参数；真实结果仍须由getSettings核验，不能把请求当事实。
function captureAudioOptions() {
  const enabled = !softwareFixedPcmMode();
  return {echoCancellation: enabled, noiseSuppression: enabled, autoGainControl: enabled};
}
// doorbellStatus 仅展示物模型事件结果，不以事件确认推断呼叫或媒体已建立。
function doorbellStatus(state) {
  const texts = {
    pending: '门铃事件等待平台确认；不是通话或媒体接通。',
    ack: '门铃事件已获平台确认；不代表呼叫或媒体接通。',
    unknown: '门铃事件未获成功确认，不能确认是否已触发呼叫。',
  };
  if (!Object.hasOwn(texts, state)) throw Error('invalid_doorbell_state');
  doorbellPending = state === 'pending';
  document.getElementById('doorbell').disabled = !doorbellAvailable || doorbellPending || closed;
  document.getElementById('doorbell-status').textContent = texts[state];
}
// status 仅写纯文本，错误或标识不能作为HTML执行。
function status(text) { document.getElementById('status').textContent = text; }
// send 对回环桥实施有界背压，不携带平台认证信息。
function send(value) {
  const body = JSON.stringify(value);
  if (closed || !socket || socket.readyState !== WebSocket.OPEN ||
      body.length > 65536 || socket.bufferedAmount > 65536) { stop(); return false; }
  socket.send(body); return true;
}
// release 只释放当前观看者，不清空其它观看者的源和Peer。
function release(session) {
  const invitation = invitations.get(session);
  if (invitation) { invitation.remove(); invitations.delete(session); }
  const view = views.get(session);
  if (!view || view.closed) return;
  if (view.releaseId) retiringViews.set(session, view);
  view.closed = true;
  // 自然恢复只属于原Peer的固定窗口；显式释放后旧定时器不能再操作任何腿。
  if (view.naturalTimer !== undefined) clearTimeout(view.naturalTimer);
  view.naturalTimer = undefined; view.naturalStarted = undefined;
  if (view.cancelGather) view.cancelGather();
  if (view.peer) view.peer.close();
  if (view.stream) for (const track of view.stream.getTracks()) track.stop();
  if (view.remoteTrack) view.remoteTrack.stop();
  if (view.remoteVideoTrack) view.remoteVideoTrack.stop();
  if (view.remoteVideo) { view.remoteVideo.pause(); view.remoteVideo.srcObject = null; }
  if (view.sink) { view.sink.pause(); view.sink.srcObject = null; }
  if (view.video) view.video.srcObject = null;
  if (view.element) view.element.remove();
  views.delete(session);
}
// stopped 必须等待原采集申请已完成并真正stop轨/close Peer，不能以本地Stop命令入队代替释放。
async function stopped(value, human) {
  if (!value.releaseId) { release(value.sessionId); return; } // 旧宿主不引入新本地回执。
  const view = views.get(value.sessionId) || retiringViews.get(value.sessionId);
  if (!view || view.releaseId !== value.releaseId || !!view.human !== human) return;
  release(value.sessionId);
  for (const pending of view.captureRequests) {
    try { const stream = await pending; for (const track of stream.getTracks()) track.stop(); }
    catch { /* 原申请失败没有采集资源，原Promise已实际结束。 */ }
  }
  if (retiringViews.get(value.sessionId) !== view) return;
  retiringViews.delete(value.sessionId);
  if (!closed) send({kind: human ? 'callStopped' : 'ipcStopped', sessionId: value.sessionId, releaseId: value.releaseId});
}
// stop 桥断线/页面关闭后停止所有媒体，不依赖MQTT或云端结束事件。
function stop() {
  if (closed) return;
  closed = true;
  privacyAvailable = false;
  if (privacyPending) privacyStatus('unknown');
  privacyButtons();
  doorbellAvailable = false;
  if (doorbellPending) doorbellStatus('unknown');
  document.getElementById('doorbell').disabled = true;
  for (const session of Array.from(views.keys())) release(session);
  for (const session of Array.from(invitations.keys())) release(session);
  if (socket) socket.close();
  status('设备已停止，所有本地媒体已释放。');
}
// viewItem 构造固定控件，不使用innerHTML拼接不可信会话标识。
function viewItem(session, talk, human = false) {
  const view = {closed: false, peer: null, stream: null, connected: false, active: false, talk, human, playing: false, speakerPaused: false, captureRequests: new Set()};
  view.element = document.createElement('section');
  const title = document.createElement('p'); title.textContent = `${human ? '人际通话' : talk ? '对讲' : '观看'}会话 ${session}`;
  view.video = document.createElement('video');
  view.video.autoplay = true; view.video.muted = true; view.video.playsInline = true;
  view.video.width = 320;
  view.video.hidden = talk; // 对讲不呈现麦克风采集，避免把本地上行当作会议下行。
  if (talk || human) { view.sink = document.createElement('audio'); view.sink.autoplay = false; view.sink.muted = false; }
  const end = document.createElement('button'); end.textContent = human ? '挂断此通话' : talk ? '结束此对讲' : '停止此观看者';
  end.onclick = () => { release(session); send({kind: human ? 'callHangup' : 'ipcEnd', sessionId: session}); };
  view.element.append(title, view.video, end);
  if (view.sink) view.element.append(view.sink);
  document.getElementById('views').append(view.element);
  views.set(session, view);
  return view;
}
// renderRemote 平台建立后才播放已授权接收轨；拒绝播放必须明确失败，不把ICE连通当已发声。
async function renderRemote(view) {
  if (view.closed || !view.active || !view.remoteTrack || view.playing || view.speakerPaused) return;
  if (view.rendering) return view.renderPromise; // 恢复须等待在途play结果，不能提前回报成功。
  view.rendering = true;
  view.renderPromise = (async () => {
    try {
      await view.sink.play();
      if (!closed && !view.closed && !view.speakerPaused) view.playing = true;
      else view.sink.pause(); // 迟到play不能越过暂停或终态；不再附加回执。
    } finally { view.rendering = false; }
  })();
  return view.renderPromise;
}
// renderRemoteVideo 只呈现正式接收轨，平台建立之前不播放，迟到播放不得越过终态。
async function renderRemoteVideo(view) {
  if (view.closed || !view.active || !view.remoteVideoTrack || view.remoteVideoPlaying) return;
  if (view.remoteVideoRendering) return view.remoteVideoPromise;
  view.remoteVideoRendering = true;
  view.remoteVideoPromise = (async () => {
    try {
      await view.remoteVideo.play();
      if (!closed && !view.closed) view.remoteVideoPlaying = true;
      else view.remoteVideo.pause();
    } finally { view.remoteVideoRendering = false; }
  })();
  return view.remoteVideoPromise;
}
// gathering 每个Peer独立收集全部ICE候选，关闭或超时清理监听和计时器。
async function gathering(view) {
  if (view.peer.iceGatheringState === 'complete') return;
  await new Promise((resolve, reject) => {
    const cleanup = () => { clearTimeout(timer); view.peer.removeEventListener('icegatheringstatechange', check); view.cancelGather = null; };
    const check = () => { if (view.peer.iceGatheringState === 'complete') { cleanup(); resolve(); } };
    const timer = setTimeout(() => { cleanup(); reject(Error('ice_timeout')); }, 8000);
    view.cancelGather = () => { cleanup(); reject(Error('peer_closed')); };
    view.peer.addEventListener('icegatheringstatechange', check); check();
  });
}
// configure 保留IPC及旧人际三轨；只有可信选中videoReceive才附加独立视频接收m-line。
async function configure(value) {
  const validTrack = name => typeof name === 'string' && name.length > 0 && name.length <= 128 && !/[\x00-\x1f\x7f]/.test(name);
  const human = value.human === true;
  const talk = !human && typeof value.speakerTrack === 'string' && value.speakerTrack !== '';
  if (value.releaseId !== undefined && (typeof value.releaseId !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value.releaseId))) throw Error('invalid_release');
  if (views.has(value.sessionId) || views.size >= 4 || typeof value.audio !== 'boolean' ||
      (value.audio && !validTrack(value.audioTrack)) ||
      (human && (!validTrack(value.speakerTrack) || !value.audio || !value.releaseId || !validTrack(value.cameraId) || [...views.values()].some(view => view.human))) ||
      (talk ? (!validTrack(value.speakerTrack) || !value.audio || value.videoTrack !== '' || value.audioTrack === value.speakerTrack || [...views.values()].some(view => view.talk))
            : !validTrack(value.videoTrack))) throw Error('invalid_peer');
  const view = viewItem(value.sessionId, talk, human);
  view.releaseId = value.releaseId; // 仅原C++租约，不从控制消息改绑。
  view.mediaRevision = 1; view.recovering = false; // 原端首次代次，重协商不改变采集意图。
  view.speakerTrack = talk || human ? value.speakerTrack : ''; // 原邀请绑定，不从轨控制临时扩权。
  view.capturePending = navigator.mediaDevices.getUserMedia({video: talk ? false : {width: 640, height: 480, frameRate: 15},
    audio: value.audio ? captureAudioOptions() : false});
  view.captureRequests.add(view.capturePending);
  void view.capturePending.then(() => view.captureRequests.delete(view.capturePending), () => view.captureRequests.delete(view.capturePending));
  const stream = await view.capturePending;
  if (closed || view.closed) { for (const track of stream.getTracks()) track.stop(); return; }
  view.stream = stream; view.video.srcObject = talk ? null : stream;
  if (stream.getVideoTracks().length !== (talk ? 0 : 1) || stream.getAudioTracks().length !== (value.audio ? 1 : 0)) throw Error('unexpected_capture');
  view.sources = new Map(); // 邀请轨到真实sender，不能从控制命令临时扩大采集权限。
  view.peer = new RTCPeerConnection({iceServers: []});
  for (const track of stream.getTracks()) {
    track.onended = () => { if (!view.closed) { release(value.sessionId); send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId}); } };
    const transceiver = view.peer.addTransceiver(track, {direction: human && track.kind === 'audio' ? 'sendrecv' : 'sendonly', streams: [stream]});
    if (human && track.kind === 'audio') view.receiveTransceiver = transceiver;
    const trackId = track.kind === 'video' ? value.videoTrack : value.audioTrack;
    if (typeof trackId === 'string' && trackId) view.sources.set(trackId, {kind: track.kind, track, sender: transceiver.sender, state: 'active'});
    if (track.kind === 'video') {
      const codecs = RTCRtpSender.getCapabilities('video').codecs.filter(codec =>
        codec.mimeType.toLowerCase() === 'video/h264' && (codec.sdpFmtpLine || '').includes('packetization-mode=1'));
      if (!codecs.length) throw Error('h264_unavailable');
      transceiver.setCodecPreferences(codecs);
    } else {
      const codecs = RTCRtpSender.getCapabilities('audio').codecs.filter(codec => codec.mimeType.toLowerCase() === 'audio/opus');
      if (!codecs.length) throw Error('opus_unavailable');
      transceiver.setCodecPreferences(codecs);
    }
  }
  if (!human) {
    // 本地档位按钮只提交意图，平台受理与驱动确认前不改变媒体状态。
    for (const [trackId, source] of view.sources) {
      for (const profile of ['low', 'standard', 'high', ...(source.kind === 'video' ? ['audioOnly'] : [])]) {
        const button = document.createElement('button');
        button.textContent = `${source.kind === 'video' ? '视频' : '音频'}档位 ${profile}`;
        button.onclick = () => {
          if (!closed && !view.closed && view.active)
            send({kind: 'ipcSetQuality', sessionId: value.sessionId, trackId, profile});
        };
        view.element.append(button);
      }
    }
  }
  if (talk || human) {
    if (!human) view.receiveTransceiver = view.peer.addTransceiver('audio', {direction: 'recvonly'});
    const codecs = RTCRtpReceiver.getCapabilities('audio').codecs.filter(codec => codec.mimeType.toLowerCase() === 'audio/opus');
    if (!codecs.length) throw Error('opus_unavailable');
    view.receiveTransceiver.setCodecPreferences(codecs);
    if (human && value.videoReceive === true) {
      view.receiveVideoTransceiver = view.peer.addTransceiver('video', {direction:'recvonly'});
      const videoCodecs = RTCRtpReceiver.getCapabilities('video').codecs.filter(codec =>
        codec.mimeType.toLowerCase() === 'video/h264' && (codec.sdpFmtpLine || '').includes('packetization-mode=1'));
      if (!videoCodecs.length) throw Error('h264_unavailable');
      view.receiveVideoTransceiver.setCodecPreferences(videoCodecs);
      view.remoteVideo = document.createElement('video');
      view.remoteVideo.autoplay = false; view.remoteVideo.muted = true; view.remoteVideo.playsInline = true;
      view.remoteVideo.width = 320;
      const label = document.createElement('p'); label.textContent = '对端画面（非本地预览）';
      view.element.append(label, view.remoteVideo);
    }
    view.peer.ontrack = event => {
      if (closed || view.closed) { event.track.stop(); return; }
      if (human && view.receiveVideoTransceiver && event.transceiver === view.receiveVideoTransceiver &&
          event.track.kind === 'video' && !view.remoteVideoTrack) {
        view.remoteVideoTrack = event.track;
        view.remoteVideo.srcObject = new MediaStream([event.track]);
        event.track.onended = () => { if (!view.closed) { release(value.sessionId); send({kind:'callFailure',sessionId:value.sessionId,releaseId:view.releaseId}); } };
        void renderRemoteVideo(view).catch(() => { if (!view.closed) { release(value.sessionId); send({kind:'callFailure',sessionId:value.sessionId,releaseId:view.releaseId}); } });
        return;
      }
      if (event.transceiver !== view.receiveTransceiver || event.track.kind !== 'audio' || view.remoteTrack) {
        event.track.stop(); release(value.sessionId); send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId}); return;
      }
      view.remoteTrack = event.track;
      event.track.enabled = !view.speakerPaused;
      view.sink.srcObject = new MediaStream([event.track]);
      event.track.onended = () => { if (!view.closed) { release(value.sessionId); send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId}); } };
      void renderRemote(view).catch(() => { if (!view.closed) { release(value.sessionId); send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId}); } });
    };
  }
  view.peer.onconnectionstatechange = () => {
    if (view.closed) return;
    // 已建立人际/会议只给原Peer五秒自然恢复，不换轨、不重开用户静音，也不重复初次握手。
    if ((human || talk) && view.active) {
      const fail = () => {
        if (view.closed) return;
        release(value.sessionId);
        send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId});
      };
      if (view.peer.connectionState === 'disconnected') {
        if (view.naturalStarted !== undefined) return;
        view.connected = false; view.naturalStarted = performance.now();
        const started = view.naturalStarted;
        send({kind: human ? 'callConnectivity' : 'ipcConnectivity', sessionId: value.sessionId, releaseId: view.releaseId, state: 'disconnected'});
        view.naturalTimer = setTimeout(() => {
          if (!view.closed && views.get(value.sessionId) === view && view.naturalStarted === started) fail();
        }, 5000);
        return;
      }
      if (view.peer.connectionState === 'connected' && view.naturalStarted !== undefined) {
        if (performance.now() < view.naturalStarted || performance.now() - view.naturalStarted >= 5000) { fail(); return; }
        clearTimeout(view.naturalTimer); view.naturalTimer = undefined; view.naturalStarted = undefined;
        view.connected = true;
        send({kind: human ? 'callConnectivity' : 'ipcConnectivity', sessionId: value.sessionId, releaseId: view.releaseId, state: 'connected'});
        return;
      }
      if (['failed', 'closed'].includes(view.peer.connectionState)) { fail(); return; }
    }
    if (view.peer.connectionState === 'connected' && !view.connected) {
      if (view.recovering && view.recoveryAnswer !== view.mediaRevision) return;
      view.connected = true; send(human ? {kind: 'callConnected', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcConnected', sessionId: value.sessionId, ...(view.mediaRevision > 1 ? {mediaRevision: view.mediaRevision} : {})});
    } else if (view.peer.connectionState === 'disconnected' && !talk && !human && view.active) {
      if (!view.recovering) {
        // 旧answer不授权自然重连；必须等待C++确认后的新代次answer才能重新报告connected。
        view.recovering = true; view.connected = false; view.active = false; view.recoveryAnswer = 0;
        send({kind: 'ipcDisconnected', sessionId: value.sessionId, mediaRevision: view.mediaRevision});
      }
    } else if (['failed', 'disconnected', 'closed'].includes(view.peer.connectionState) && !view.recovering) {
      release(value.sessionId); send(human ? {kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId} : {kind: 'ipcFailure', sessionId: value.sessionId});
    } else if (view.peer.connectionState === 'failed') {
      release(value.sessionId); send({kind: 'ipcFailure', sessionId: value.sessionId});
    }
  };
  await view.peer.setLocalDescription(await view.peer.createOffer());
  await gathering(view);
  if (!closed && !view.closed) send(human ? {kind: 'callOffer', sessionId: value.sessionId, releaseId: view.releaseId, sdp: view.peer.localDescription.sdp} : {kind: 'ipcOffer', sessionId: value.sessionId, sdp: view.peer.localDescription.sdp});
}
// controlTrack 分离接收播放与采集sender；实际驱动完成才回执，不把暂停输出等同于IM禁麦。
async function controlTrack(value) {
  const view = views.get(value.sessionId);
  const speaker = view && view.talk && value.trackId === view.speakerTrack;
  const source = view && view.sources && view.sources.get(value.trackId);
  if (!view || view.closed || !view.active ||
      (speaker ? !['pause', 'resume'].includes(value.operation) : (!source || !source.sender)) ||
      typeof value.token !== 'string' || !value.token || value.token.length > 128 ||
      !['publish', 'pause', 'resume', 'stop'].includes(value.operation) || view.trackBusy)
    throw Error('invalid_track');
  if (!speaker && source.state === 'stopped' && ['pause', 'resume'].includes(value.operation)) throw Error('stopped_track');
  view.trackBusy = true;
  try {
    if (source) source.qualityPaused = false; // 显式轨控制优先，质量恢复不能撤销用户的暂停。
    if (speaker) {
      view.speakerPaused = value.operation === 'pause';
      if (view.remoteTrack) view.remoteTrack.enabled = !view.speakerPaused;
      if (view.speakerPaused) { view.sink.pause(); view.playing = false; }
      else await renderRemote(view);
      if (!closed && !view.closed) send({kind: 'ipcTrackApplied', sessionId: value.sessionId, token: value.token,
        state: view.speakerPaused ? 'paused' : 'active', success: true});
      return;
    }
    if (value.operation === 'pause' || value.operation === 'stop') {
      await source.sender.replaceTrack(null);
      if (value.operation === 'stop') { source.track.onended = null; source.track.stop(); }
      else source.track.enabled = false;
      source.state = value.operation === 'stop' ? 'stopped' : 'paused';
    } else {
      if (source.track.readyState === 'ended') {
        const pending = navigator.mediaDevices.getUserMedia({video: source.kind === 'video' ? {width: 640, height: 480, frameRate: 15} : false,
          audio: source.kind === 'audio' ? (softwareFixedPcmMode() ? captureAudioOptions() : true) : false});
        view.captureRequests.add(pending); // 重发布也属于原资源组，晚到申请不能越过Stopped回执。
        void pending.then(() => view.captureRequests.delete(pending), () => view.captureRequests.delete(pending));
        const fresh = await pending;
        if (closed || view.closed) { for (const track of fresh.getTracks()) track.stop(); return; }
        const replacement = fresh.getTracks().find(track => track.kind === source.kind);
        if (!replacement) { for (const track of fresh.getTracks()) track.stop(); throw Error('missing_track'); }
        for (const track of fresh.getTracks()) if (track !== replacement) track.stop();
        view.stream.removeTrack(source.track); source.track = replacement; view.stream.addTrack(replacement);
        replacement.onended = () => { if (!view.closed) { release(value.sessionId); send({kind: 'ipcFailure', sessionId: value.sessionId}); } };
      }
      source.track.enabled = true;
      await source.sender.replaceTrack(source.track);
      source.state = 'active';
    }
    if (!closed && !view.closed) send({kind: 'ipcTrackApplied', sessionId: value.sessionId, token: value.token, state: source.state, success: true});
  } finally { view.trackBusy = false; }
}
// qualityTrack 实际调整原sender并读回参数；不重新采集，不将编码约束当实际码率或画质证明。
// audioOnly只暂停目标视频发送，后续档位只恢复由质量操作暂停的轨，不恢复用户手动暂停的轨。
async function qualityTrack(value) {
  const view = views.get(value.sessionId);
  const source = view && view.sources && view.sources.get(value.trackId);
  if (!view || view.closed || !view.active || view.human || view.trackBusy || !source ||
      !source.sender || typeof value.token !== 'string' || !value.token || value.token.length > 128 ||
      !['low', 'standard', 'high', 'audioOnly'].includes(value.profile) ||
      (view.releaseId && value.releaseId !== view.releaseId)) throw Error('invalid_quality');
  view.trackBusy = true;
  try {
    if (value.profile === 'audioOnly' && source.kind === 'video') {
      if (source.state === 'active') {
        await source.sender.replaceTrack(null);
        if (closed || view.closed) return;
        source.track.enabled = false;
        source.state = 'paused';
        source.qualityPaused = true;
      }
    } else {
      const bitrate = source.kind === 'video'
        ? {low: 150000, standard: 600000, high: 1200000}[value.profile]
        : {low: 16000, standard: 32000, high: 64000, audioOnly: 32000}[value.profile];
      const parameters = source.sender.getParameters();
      if (!parameters.encodings || parameters.encodings.length !== 1) throw Error('unsupported_encodings');
      parameters.encodings[0].maxBitrate = bitrate;
      if (source.kind === 'video') {
        parameters.encodings[0].scaleResolutionDownBy = value.profile === 'low' ? 2 : 1;
        parameters.encodings[0].maxFramerate = value.profile === 'low' ? 10 : 15;
      }
      await source.sender.setParameters(parameters);
      if (closed || view.closed) return;
      const actual = source.sender.getParameters().encodings;
      if (!actual || actual.length !== 1 || actual[0].maxBitrate !== bitrate ||
          (source.kind === 'video' && (actual[0].scaleResolutionDownBy !== parameters.encodings[0].scaleResolutionDownBy ||
            actual[0].maxFramerate !== parameters.encodings[0].maxFramerate))) throw Error('quality_not_applied');
      if (source.qualityPaused && source.state === 'paused') {
        await source.sender.replaceTrack(source.track);
        if (closed || view.closed) return;
        source.track.enabled = true;
        source.state = 'active';
        source.qualityPaused = false;
      }
    }
    if (!closed && !view.closed) send({kind: 'ipcQualityApplied', sessionId: value.sessionId,
      token: value.token, state: source.state, success: true});
  } finally { view.trackBusy = false; }
}
// incoming 只执行C++允许的固定指令，迟到answer不能复活已结束观看者。
// showParticipants 只接C++已消费的有限展示；非法旁路消息不得执行驱动、关闭Peer或发送云端意图。
function showParticipants(value) {
  const keys = (object, expected) => object && typeof object === 'object' && !Array.isArray(object) &&
    Object.keys(object).sort().join(',') === expected.slice().sort().join(',');
  const text = (value, limit) => typeof value === 'string' && value.length > 0 &&
    new TextEncoder().encode(value).length <= limit && !/[\x00-\x1f\x7f\ud800-\udfff]/u.test(value);
  if (!keys(value, ['kind', 'sessionId', 'eventSeq', 'participants']) ||
      !text(value.sessionId, 128) || !Number.isSafeInteger(value.eventSeq) || value.eventSeq < 1 ||
      !Array.isArray(value.participants) || value.participants.length > 32) return false;
  const view = views.get(value.sessionId);
  if (!view || view.closed || !view.talk || !view.active || value.eventSeq <= (view.rosterSequence || 0)) return false;
  const identities = new Set(), tracks = new Set();
  for (const member of value.participants) {
    if (!keys(member, ['participantId', 'tracks']) || !text(member.participantId, 128) ||
        identities.has(member.participantId) || !Array.isArray(member.tracks) || member.tracks.length > 8) return false;
    identities.add(member.participantId);
    for (const track of member.tracks) {
      if (!keys(track, ['trackId', 'kind', 'sourceId', 'direction', 'codec', 'state']) ||
          !text(track.trackId, 128) || tracks.has(track.trackId) || !text(track.sourceId, 128) ||
          !text(track.codec, 64) || !['audio', 'video', 'subtitle'].includes(track.kind) ||
          !['sendonly', 'recvonly', 'sendrecv'].includes(track.direction) ||
          !['requested', 'active', 'paused', 'stopped'].includes(track.state)) return false;
      tracks.add(track.trackId);
    }
  }
  view.participants = value.participants.map(member => ({participantId: member.participantId,
    tracks: member.tracks.map(track => ({...track}))})); // 不保留调用者可变对象别名。
  view.rosterSequence = value.eventSeq;
  if (!view.rosterElement) { view.rosterElement = document.createElement('p'); view.element.append(view.rosterElement); }
  view.rosterElement.textContent = `已接收成员（不代表完整会议名册或话权）：${[...identities].join('、')}`;
  return true;
}
// showSyncState 只消费C++已关联状态；查询按钮不接收平台token，不采集或恢复媒体。
function showSyncState(value) {
  const states=['ready','pending','succeeded','failed','unknown','cancelled','rejected','unavailable'];
  if (!value || Object.keys(value).length!==4 || value.kind!=='ipcSyncState' ||
      typeof value.sessionId!=='string' || !value.sessionId || value.sessionId.length>64 ||
      !states.includes(value.state) || !Number.isSafeInteger(value.eventSeq) || value.eventSeq<0) return false;
  const view=views.get(value.sessionId);
  if (closed || !view || view.closed || !view.active || !view.talk || view.human ||
      value.eventSeq<(view.syncSequence||0)) return false;
  if (value.state==='unavailable' && !view.syncButton) return false;
  if (!view.syncButton) {
    view.syncButton=document.createElement('button'); view.syncButton.textContent='查询完整成员名册';
    view.syncStatus=document.createElement('p'); view.element.append(view.syncButton,view.syncStatus);
    view.syncButton.onclick=()=>{
      if (closed || view.closed || !view.active || views.get(value.sessionId)!==view ||
          view.syncButton.disabled || view.syncState==='pending' || view.syncState==='unavailable') return;
      view.syncButton.disabled=true; view.syncState='pending';
      send({kind:'ipcSync',sessionId:value.sessionId});
    };
  }
  view.syncState=value.state; view.syncSequence=value.eventSeq;
  view.syncButton.disabled=['pending','unavailable'].includes(value.state);
  view.syncStatus.textContent=({ready:'可以查询完整成员名册。',pending:'正在等待成员名册查询结果。',
    succeeded:'共享控制器已安装完整成员名册。',failed:'成员名册查询失败；主对讲继续。',
    unknown:'成员名册查询未获有效结果；主对讲继续。',cancelled:'成员名册查询已取消。',
    rejected:'本次查询未受理；主对讲继续。',unavailable:'当前会话不支持成员名册查询。'})[value.state]+` 已消费名册水位：${value.eventSeq}。`;
  return true;
}
async function incoming(value) {
  if (value.kind === 'ipcSyncState') { showSyncState(value); return; }
  if (value.kind === 'ipcParticipants') { showParticipants(value); return; }
  if (value.kind === 'endpointStatus') {
    const fields=['kind','sessionId','handoffId','state','candidateSessionId','reason','operation','msgToken','revision','code','pending','unknown'];
    if (Object.keys(value).length !== fields.length || !Object.keys(value).every(key=>fields.includes(key)) ||
        !['idle','preparing','ready','committed','aborted','failed'].includes(value.state) ||
        !['','prepare','commit','abort'].includes(value.operation) ||
        !['','handoffTimeout','userCancelled','mediaFailed','permissionRevoked','sourceChanged'].includes(value.reason) ||
        !['sessionId','handoffId','candidateSessionId','msgToken'].every(key=>typeof value[key]==='string'&&value[key].length<=128&&!/[\x00-\x1f\x7f]/.test(value[key])) ||
        !Number.isSafeInteger(value.revision) || value.revision<0 ||
        !Number.isSafeInteger(value.code) || (value.code!==0&&(value.code<200||value.code>599)) ||
        typeof value.pending!=='boolean' || typeof value.unknown!=='boolean') throw Error('invalid_endpoint_status');
    window.__endpointReceipt=Object.freeze({...value}); // 只读验收投影；不能反向授予媒体或命令身份。
    document.getElementById('endpoint-status').textContent=`接续状态：${value.state}；应答 ${value.code}；${value.unknown?'结果未知':value.pending?'等待平台结果':'已收到平台事实'}。`;
    return;
  }
  if (value.kind === 'callState') {
    if (closed) return;
    if (Object.keys(value).length !== 3 || typeof value.sessionId !== 'string' || value.sessionId.length > 64 ||
        !['ringing', 'connected', 'available', 'unavailable', 'negotiating'].includes(value.state)) throw Error('invalid_call_state');
    document.getElementById('human-status').textContent = ({ringing: '人际来电：必须本地明确接听，不自动开启音视频。',
      connected: '平台会话已建立；实际媒体状态另行采集。', available: '人际通话入口就绪。',
      unavailable: '人际通话入口未就绪。', negotiating: '正在协商媒体，尚不能确认接通。'})[value.state];
    if (value.state === 'ringing') {
      if (!value.sessionId) throw Error('invalid_call_session');
      if (humanSession && humanSession !== value.sessionId) {
        // 旧确认可替换，但原真实媒体/晚到采集仍占租约，不能借新来电越过Stopped。
        if (views.has(humanSession) || retiringViews.has(humanSession)) throw Error('invalid_call_session');
        release(humanSession);
      }
      humanSession = value.sessionId;
      if (!invitations.has(humanSession)) {
        const originalSession = humanSession;
        const element = document.createElement('section');
        for (const [kind, text] of [['callAccept', '接听本次人际通话'], ['callReject', '拒绝本次人际通话']]) {
          const button = document.createElement('button'); button.textContent = text;
          button.onclick = () => { element.remove(); invitations.delete(originalSession); send({kind, sessionId: originalSession}); };
          element.append(button);
        }
        invitations.set(humanSession, element); document.getElementById('views').append(element);
      }
    } else if (['available', 'unavailable'].includes(value.state)) {
      if (humanSession) release(humanSession);
      humanSession = '';
    } else if (value.state === 'negotiating') {
      if (!value.sessionId) return; // 主叫尚未收到sessionCreated，不分配网页媒体身份。
      if (humanSession && humanSession !== value.sessionId) throw Error('invalid_call_session');
      humanSession=value.sessionId;
    } else if (value.state === 'connected') {
      const view = views.get(value.sessionId);
      if (view && view.human && view.connected && !view.closed) {
        view.active = true;
        try { await renderRemote(view); await renderRemoteVideo(view); }
        catch { release(value.sessionId); send({kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId}); }
      }
    }
    return;
  }
  if (value.kind === 'privacyAvailable' || value.kind === 'privacyUnavailable') {
    if (closed) return;
    if (Object.keys(value).length !== 1) throw Error('invalid_privacy_notice');
    privacyAvailable = value.kind === 'privacyAvailable'; privacyButtons();
    if (!privacyPending) document.getElementById('privacy-status').textContent = privacyAvailable
      ? '隐私属性出口就绪；上报后等待正式后端停流，关闭隐私不恢复旧媒体。'
      : '隐私属性出口未启用或未就绪。';
    return;
  }
  if (value.kind === 'privacyState') {
    if (closed) return;
    if (Object.keys(value).length !== 2) throw Error('invalid_privacy_notice');
    privacyStatus(value.state); return;
  }
  if (value.kind === 'doorbellAvailable' || value.kind === 'doorbellUnavailable') {
    if (closed) return;
    if (Object.keys(value).length !== 1) throw Error('invalid_doorbell_notice');
    doorbellAvailable = value.kind === 'doorbellAvailable';
    document.getElementById('doorbell').disabled = !doorbellAvailable || doorbellPending;
    if (!doorbellPending) document.getElementById('doorbell-status').textContent = doorbellAvailable
      ? '门铃事件出口就绪；按下按钮只上报事件，不启动音视频。' : '门铃事件出口未启用或未就绪。';
    return;
  }
  if (value.kind === 'doorbellState') {
    if (closed) return;
    if (Object.keys(value).length !== 2) throw Error('invalid_doorbell_notice');
    doorbellStatus(value.state); return;
  }
  if (value.kind === 'ipcAvailable') { available = true; status('设备在线，等待授权观看。'); return; }
  if (typeof value.sessionId !== 'string' || !value.sessionId || value.sessionId.length > 64) throw Error('invalid_session');
  if (value.kind === 'ipcStop' || value.kind === 'callStop') { await stopped(value, value.kind === 'callStop'); return; }
  if (value.kind === 'callConfigure') {
    const keys = Object.keys(value);
    if (keys.length !== (value.videoReceive === true ? 5 : 4) ||
        !keys.every(key => ['kind','sessionId','releaseId','cameraId','videoReceive'].includes(key)) ||
        (Object.hasOwn(value,'videoReceive') && value.videoReceive !== true) ||
        value.sessionId !== humanSession) throw Error('invalid_call_configure');
    try { await configure({...value, human: true, audio: true, audioTrack: 'mic-1', speakerTrack: 'speaker-1', videoTrack: 'video-1'}); }
    catch { release(value.sessionId); send({kind: 'callFailure', sessionId: value.sessionId, releaseId: value.releaseId}); }
    return;
  }
  if (value.kind === 'callAnswer') {
    const view = views.get(value.sessionId);
    if (Object.keys(value).length !== 4 || typeof value.sdp !== 'string') throw Error('invalid_call_answer');
    if (!view || view.closed || !view.human || view.releaseId !== value.releaseId) return;
    try { await view.peer.setRemoteDescription({type: 'answer', sdp: value.sdp}); }
    catch { release(value.sessionId); send({kind: 'callFailure', sessionId: value.sessionId, releaseId: view.releaseId}); }
    return;
  }
  if (value.kind === 'ipcTrack') {
    try { await controlTrack(value); }
    catch {
      send({kind: 'ipcTrackApplied', sessionId: value.sessionId, token: value.token, state: 'unknown', success: false});
      release(value.sessionId);
    }
    return;
  }
  if (value.kind === 'ipcQuality') {
    try { await qualityTrack(value); }
    catch {
      // 局部档位失败不停止主通话或其他观看者；仍以原token向核心报告失败。
      const view = views.get(value.sessionId);
      if (view && !view.closed && (!view.releaseId || view.releaseId === value.releaseId))
        send({kind: 'ipcQualityApplied', sessionId: value.sessionId, token: value.token, state: 'unknown', success: false});
    }
    return;
  }
  if (value.kind === 'ipcActive') {
    const view = views.get(value.sessionId);
    if (view && !view.closed && view.connected) {
      view.active = true;
      view.recovering = false;
      if (view.talk) {
        try { await renderRemote(view); }
        catch { release(value.sessionId); send({kind: 'ipcFailure', sessionId: value.sessionId}); }
      }
    }
    return;
  }
  if (value.kind === 'ipcInvite') {
    if (views.has(value.sessionId) || invitations.has(value.sessionId)) return;
    const element = document.createElement('button'); element.textContent = value.talk === true ? '接受本次对讲' : '接受本次观看';
    invitations.set(value.sessionId, element);
    element.onclick = () => { element.remove(); invitations.delete(value.sessionId); send({kind: 'ipcAccept', sessionId: value.sessionId}); };
    document.getElementById('views').append(element); return;
  }
  if (value.kind === 'ipcConfigure') {
    try { await configure(value); }
    catch { release(value.sessionId); send({kind: 'ipcFailure', sessionId: value.sessionId}); }
    return;
  }
  if (value.kind === 'ipcAnswer') {
    const view = views.get(value.sessionId);
    if (!view || view.closed || (value.releaseId !== undefined && view.releaseId !== value.releaseId)) return;
    if ((value.mediaRevision ?? 1) !== view.mediaRevision) return;
    try {
      await view.peer.setRemoteDescription({type: 'answer', sdp: value.sdp});
      if (!view.closed) view.recoveryAnswer = view.mediaRevision;
    }
    catch { release(value.sessionId); send({kind: 'ipcFailure', sessionId: value.sessionId}); }
    return;
  }
  if (value.kind === 'ipcRecover') {
    const view = views.get(value.sessionId);
    if (!view || view.closed || (value.releaseId !== undefined && view.releaseId !== value.releaseId)) return;
    if (view.human || view.talk || !view.recovering || !Number.isSafeInteger(value.mediaRevision) ||
        value.mediaRevision !== view.mediaRevision + 1 || Object.keys(value).length !== (value.releaseId === undefined ? 3 : 4)) throw Error('invalid_recovery');
    view.mediaRevision = value.mediaRevision;
    try {
      // ICE重启复用原sender和轨，不调用getUserMedia，也不改暂停／已停止状态。
      await view.peer.setLocalDescription(await view.peer.createOffer({iceRestart: true}));
      await gathering(view);
      if (!closed && !view.closed) send({kind:'ipcOffer',sessionId:value.sessionId,mediaRevision:view.mediaRevision,sdp:view.peer.localDescription.sdp});
    } catch { release(value.sessionId); send({kind:'ipcFailure',sessionId:value.sessionId}); }
    return;
  }
  throw Error('invalid_command');
}
// readiness 提供可审计的浏览器媒体事实，不将成功信令当作实际编码帧。
async function ipcStats() {
  const reports = [];
  for (const [sessionId, view] of views) {
    let packets = 0, frames = 0, audioSent = 0, audioReceived = 0, audioSamples = 0, audioEnergy = 0;
    if (view.peer) (await view.peer.getStats()).forEach(stat => {
      if (stat.type === 'outbound-rtp' && stat.kind === 'video') { packets += stat.packetsSent || 0; frames += stat.framesEncoded || 0; }
      if (stat.type === 'outbound-rtp' && stat.kind === 'audio') audioSent += stat.packetsSent || 0;
      if (stat.type === 'inbound-rtp' && stat.kind === 'audio') {
        audioReceived += stat.packetsReceived || 0;
        // RTC总采样含本地丢包补偿；未知补偿指标不作为真实解码证据。
        if (Number.isFinite(stat.totalSamplesReceived) && Number.isFinite(stat.concealedSamples))
          audioSamples += Math.max(0, stat.totalSamplesReceived - stat.concealedSamples);
        audioEnergy += stat.totalAudioEnergy || 0;
      }
    });
    reports.push({sessionId, packets, frames, connected: view.connected, active: view.active,
      talk: view.talk, human: view.human, playing: view.playing, audioSent, audioReceived, audioSamples, audioEnergy,
      liveTracks: view.stream ? view.stream.getTracks().filter(track => track.readyState === 'live').length : 0});
  }
  return reports;
}
// activate 用户主动配对后才连接设备平台；同源bootstrap令牌不进入地址栏或日志。
document.getElementById('activate').onclick = async () => {
  document.getElementById('activate').disabled = true;
  try {
    const bootstrap = await (await fetch('/bootstrap', {cache: 'no-store'})).json();
    socket = new WebSocket(`ws://${location.host}/bridge?token=${bootstrap.token}`);
    socket.onopen = () => send({kind: 'ready'});
    socket.onmessage = event => {
      try {
        if (typeof event.data !== 'string' || event.data.length > 65536) throw Error('invalid_message');
        void incoming(JSON.parse(event.data)).catch(stop);
      } catch { stop(); }
    };
    socket.onclose = stop; socket.onerror = stop;
  } catch { stop(); }
};
document.getElementById('shutdown').onclick = () => { send({kind: 'shutdown'}); stop(); };
// 新按压只交给C++生成原token；unknown以后必须用户另行按压，页面没有自动重试定时器。
document.getElementById('doorbell').disabled = true;
document.getElementById('doorbell').onclick = () => {
  if (closed || !doorbellAvailable || doorbellPending) return;
  doorbellStatus('pending');
  send({kind: 'doorbellPress'});
};
// 软件验证只报告原物模型意图，不能以按钮本地stop掩盖正式后端停流缺陷。
privacyButtons();
for (const [id, enabled] of [['privacy-enable', true], ['privacy-disable', false]]) {
  document.getElementById(id).onclick = () => {
    if (closed || !privacyAvailable || privacyPending) return;
    privacyStatus('pending'); send({kind: 'privacyReport', enabled});
  };
}
// localDeviceTarget 只提交明确的设备目标，平台身份、修订和token仍由C++生成并鉴权。
function localDeviceTarget(id) {
  const target=document.getElementById(id).value;
  if (typeof target!=='string'||target.length>121||!/^[A-Za-z0-9_.-]+:[A-Za-z0-9_.-]+$/.test(target)) return '';
  return target;
}
document.getElementById('call-dial').onclick=()=>{
  const target=localDeviceTarget('call-target');
  if (!closed&&target) send({kind:'callDial',target});
};
for (const [id,kind] of [['endpoint-prepare','endpointPrepare'],['endpoint-commit','endpointCommit'],['endpoint-abort','endpointAbort']]) {
  document.getElementById(id).onclick=()=>{
    if (closed||!humanSession) return;
    const target=kind==='endpointPrepare'?localDeviceTarget('endpoint-target'):'';
    if (kind==='endpointPrepare'&&!target) return;
    send({kind,sessionId:humanSession,...(target?{target:'device:'+target}:{})});
  };
}
window.addEventListener('pagehide', stop);
