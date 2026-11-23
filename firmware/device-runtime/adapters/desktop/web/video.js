// 文件说明：视频模拟设备仅通过已配对回环桥提供WebRTC驱动，不访问MQTT或设备密钥。
'use strict';
const item = id => document.getElementById(id);
let socket;
let peer;
let capture;
let state = 'idle';
let closed = false;
let generation = 0;
let reports = {audioPackets: 0, videoPackets: 0, videoFrames: 0};
let trackPending = false;
// 本页只持原Peer局部票据及实际观察，不持平台身份或自动恢复许可。
let peerEpoch = '0', callSession = '', controlPending = false, actualHeld = false;
let callAudio, callAudioNode, remoteAudioSource, outputPending, holdPending;
let outputTicket = 0n, holdTicket = 0n, outputEnabled = false;
const controlReceipts = []; // 最多24项原固定ACK/实态投影，不保存平台正文。
// 转接与撤候选是独立账本；仅保留24个原token，候选ID与目标不进入只读投影。
const transferReceipts = [];
let latestTransfer = null, transferIntent = '', transferOverflow = false;
let audioZeroBlocks = 0, audioNonSilentBlocks = 0, holdEffects = 0;

// transferBusy 未知不自动重试；成功转接也仍等待原sessionEnded释放旧腿。
function transferBusy() {
  return !!transferIntent || (latestTransfer &&
    (['pending','transferring','transferred','unknown'].includes(latestTransfer.state) ||
     ['pending','unknown'].includes(latestTransfer.cancelState)));
}

// status只写纯文本，平台或浏览器错误不能作为HTML执行。
function status(text) { item('status').textContent = text; }
// buttons以C++确认的状态为准，不能把本地点击当成云端接通。
function buttons() {
  item('activate').disabled = state !== 'idle';
  item('target').disabled = state !== 'available';
  item('dial').disabled = state !== 'available';
  item('accept').disabled = state !== 'ringing';
  item('reject').disabled = state !== 'ringing';
  item('hangup').disabled = !['negotiating', 'connected'].includes(state);
  item('mute').disabled = state !== 'connected' || trackPending;
  item('camera').disabled = state !== 'connected' || trackPending;
  const busy = !!transferBusy();
  item('hold').disabled = state !== 'connected' || actualHeld || controlPending || !!holdPending || !!outputPending || busy;
  item('resume').disabled = state !== 'connected' || !actualHeld || controlPending || !!holdPending || !!outputPending || busy;
  item('cancel').disabled = !['inviting', 'negotiating', 'connected'].includes(state) || controlPending || busy;
  item('transfer').disabled = state !== 'connected' || !peer || actualHeld || controlPending ||
    !!holdPending || !!outputPending || trackPending || busy;
  item('transferTarget').disabled = item('transfer').disabled;
  item('cancelTransfer').disabled = state !== 'connected' || !peer || !!transferIntent || actualHeld ||
    controlPending || !!holdPending || !!outputPending || trackPending ||
    !latestTransfer || latestTransfer.state !== 'transferring' || latestTransfer.code !== 200 ||
    !latestTransfer.candidatePresent || !!latestTransfer.cancelToken;
}
// send限制桥消息大小和积压，失败即终止本地媒体。
function send(value) {
  const text = JSON.stringify(value);
  if (closed || !socket || socket.readyState !== WebSocket.OPEN ||
      text.length > 65536 || socket.bufferedAmount > 65536) {
    stop('本地连接不可用，媒体已停止。'); return false;
  }
  socket.send(text); return true;
}
// releasePeer无论协商是否完成均停止真实音视频轨，避免挂断后继续采集。
function releasePeer() {
  ++generation;
  trackPending = false;
  latestTransfer = null; transferIntent = '';
  holdPending = null;
  if (outputPending) {
    clearTimeout(outputPending.timeout);
    callAudioNode?.port.postMessage({kind:'callAudioAbort',ticket:outputPending.gateTicket});
    outputPending = null;
  }
  outputEnabled = false;
  if (remoteAudioSource) { remoteAudioSource.disconnect(); remoteAudioSource = null; }
  item('remoteAudio').pause();
  item('local').pause(); item('remote').pause();
  item('local').style.visibility = item('remote').style.visibility = 'hidden';
  item('mute').textContent = '静音';
  item('camera').textContent = '关摄像头';
  if (peer) { peer.close(); peer = null; }
  if (capture) { for (const track of capture.getTracks()) track.stop(); capture = null; }
  item('local').srcObject = null;
  item('remote').srcObject = null;
  item('remoteAudio').srcObject = null;
}
// stop只有致命桥故障或页面关闭才断开C++；正常通话结束仍可接新来电。
function stop(reason) {
  if (closed) return;
  closed = true; releasePeer();
  if (callAudioNode) callAudioNode.port.postMessage({kind:'callAudioClose'});
  if (callAudio) void callAudio.close();
  if (socket) socket.close();
  state = 'ended'; buttons(); status(reason);
}
// mediaStats分离WebRTC收包、实际解码视频帧，不把SDP成功当成可播放画面。
async function mediaStats() {
  if (!peer) return reports;
  const stats = await peer.getStats();
  let audioPackets = 0, videoPackets = 0, videoFrames = 0;
  stats.forEach(value => {
    if (value.type !== 'inbound-rtp') return;
    if (value.kind === 'audio') audioPackets += value.packetsReceived || 0;
    if (value.kind === 'video') {
      videoPackets += value.packetsReceived || 0;
      videoFrames += value.framesDecoded || 0;
    }
  });
  reports = {audioPackets, videoPackets, videoFrames};
  item('metrics').textContent = `音频收包 ${audioPackets}，视频收包 ${videoPackets}，解码画面 ${videoFrames}`;
  return reports;
}
// controlMetrics 为正式验收提供有界固定事实，不从UI按钮或业务枚举推断实际媒体完成。
function controlMetrics() {
  return {peerEpoch, sessionId:callSession, actualHeld, outputEnabled, holdEffects,
    audioZeroBlocks, audioNonSilentBlocks, receipts:controlReceipts.map(value => ({...value})),
    transferReceipts:transferReceipts.map(value=>({...value})), transferOverflow};
}
// callAudioReady 用户激活时安装唯一末端节点，首块静音，原ready不代表实际静音已确认。
async function callAudioReady() {
  callAudio = new AudioContext({sampleRate:48000});
  await callAudio.resume();
  if (callAudio.sampleRate !== 48000 || callAudio.state !== 'running') throw Error('audio_unavailable');
  await callAudio.audioWorklet.addModule('/call-worklet.js');
  callAudioNode = new AudioWorkletNode(callAudio, 'ur-call-audio', {numberOfInputs:1,
    numberOfOutputs:1, outputChannelCount:[2]});
  callAudioNode.port.onmessage = event => {
    const value = event.data;
    if (value?.kind === 'callAudioStats') {
      if (Number.isSafeInteger(value.zeroBlocks) && Number.isSafeInteger(value.nonSilentBlocks) &&
          value.zeroBlocks >= audioZeroBlocks && value.nonSilentBlocks >= audioNonSilentBlocks &&
          value.zeroBlocks <= 1e9 && value.nonSilentBlocks <= 1e9) {
        audioZeroBlocks = value.zeroBlocks; audioNonSilentBlocks = value.nonSilentBlocks;
      }
      return;
    }
    if (value?.kind === 'callAudioFailed') { failOutput(); return; }
    if (value?.kind !== 'callAudioGated' || !outputPending ||
        value.ticket !== outputPending.gateTicket || value.peerEpoch !== outputPending.peerEpoch ||
        value.enabled !== outputPending.enabled) return;
    outputPending.audioApplied = true; completeOutput();
  };
  callAudioNode.connect(callAudio.destination);
}
// paintBoundary 只在两帧真实显示提交后确认，背景暂停或空调度不伪造display完成。
function paintBoundary() {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(Error('display_timeout')), 1500);
    requestAnimationFrame(() => requestAnimationFrame(() => { clearTimeout(timeout); resolve(); }));
  });
}
// fixedCounter 验证局部uint64十进制，禁止浮点/前导零或溢出票据。
function fixedCounter(value, zero = false) {
  return typeof value === 'string' && (zero ? /^(0|[1-9][0-9]{0,19})$/ : /^[1-9][0-9]{0,19}$/).test(value) &&
    BigInt(value) <= 18446744073709551615n;
}
// completeOutput 音频原块与原画面提交齐全才回执；不是WebSocket命令排队成功。
function completeOutput() {
  if (closed || !outputPending || !outputPending.audioApplied || !outputPending.displayApplied) return;
  const value = outputPending;
  clearTimeout(value.timeout); outputPending = null; outputEnabled = value.enabled;
  send({kind:'callOutputApplied',sessionId:value.sessionId,peerEpoch:value.peerEpoch,
    gateTicket:value.gateTicket,enabled:value.enabled,applied:true});
  // 本地音频/显示等待已经完成，必须同步解除按钮等待；平台不会为这次局部回执再发送callState。
  buttons();
}
// failOutput 失败收敛零输出但不给成功回执；只有原C++票据能接收该固定未知结果。
function failOutput() {
  const value = outputPending;
  outputEnabled = false;
  // 上行恢复失败也可能发生在音频回执之后，仍撤销原末端票据，不能等C++断桥才静音。
  if (outputTicket > 0n) callAudioNode?.port.postMessage({kind:'callAudioAbort',ticket:outputTicket.toString()});
  item('local').pause(); item('remote').pause();
  item('local').style.visibility = item('remote').style.visibility = 'hidden';
  if (!value) return;
  clearTimeout(value.timeout); outputPending = null;
  send({kind:'callOutputApplied',sessionId:value.sessionId,peerEpoch:value.peerEpoch,
    gateTicket:value.gateTicket,enabled:value.enabled,applied:false});
}
// applyOutput 只执行原当前Peer门控，实际音频块与display提交未齐不能打开许可计数。
async function applyOutput(value) {
  if (!callAudioNode || value.peerEpoch !== peerEpoch ||
      (value.sessionId && value.sessionId !== callSession) || outputPending) throw Error('output_scope');
  outputPending = {...value,audioApplied:false,displayApplied:false};
  buttons();
  const current = outputPending;
  current.timeout = setTimeout(failOutput, 1500);
  callAudioNode.port.postMessage({kind:'callAudioMask',ticket:value.gateTicket,
    peerEpoch:value.peerEpoch,enabled:value.enabled});
  for (const id of ['local','remote']) {
    const video = item(id); video.muted = true;
    video.style.visibility = value.enabled ? 'visible' : 'hidden';
    if (value.enabled && video.srcObject) await video.play(); else video.pause();
  }
  await paintBoundary();
  if (outputPending !== current || closed || peerEpoch !== value.peerEpoch) return;
  current.displayApplied = true; completeOutput();
}
// applyHold 只对原两个Sender停止/恢复原捕获轨，不重建Peer/SDP/摄像头或改变手动静音意图。
async function applyHold(value) {
  if (!peer || !capture || value.peerEpoch !== peerEpoch || value.sessionId !== callSession || holdPending)
    throw Error('hold_scope');
  const current = {value,peer,generation}; holdPending = current;
  buttons();
  const tracks = capture.getTracks();
  const senders = peer.getSenders();
  if (tracks.length !== 2 || senders.length !== 2) throw Error('sender_count');
  // 每个Sender保留原Track绑定；held时track为空不能用当前枚举猜恢复目标。
  for (const sender of senders) {
    if (!sender.originalCallTrack) sender.originalCallTrack = sender.track;
    if (!tracks.includes(sender.originalCallTrack) || sender.originalCallTrack.readyState !== 'live')
      throw Error('original_track_missing');
  }
  await Promise.all(senders.map(sender => sender.replaceTrack(value.held ? null : sender.originalCallTrack)));
  if (closed || holdPending !== current || peer !== current.peer || generation !== current.generation) return;
  if (senders.some(sender => sender.track !== (value.held ? null : sender.originalCallTrack)))
    throw Error('sender_not_applied');
  holdPending = null; actualHeld = value.held;
  holdEffects = Math.min(1e9, holdEffects + 1);
  send({...value,kind:'callHoldApplied',applied:true}); buttons();
}
// gatherComplete只发送包含全部候选的SDP，过时通话的迟到offer直接丢弃。
async function gatherComplete(current) {
  if (peer.iceGatheringState !== 'complete') {
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(Error('ice_timeout')), 8000);
      const check = () => {
        if (peer?.iceGatheringState !== 'complete') return;
        clearTimeout(timeout); peer.removeEventListener('icegatheringstatechange', check); resolve();
      };
      peer.addEventListener('icegatheringstatechange', check); check();
    });
  }
  if (current !== generation || !peer?.localDescription) return;
  send({kind: 'callOffer', sdp: peer.localDescription.sdp});
}
// configure真正创建摄像头/麦克风轨与Peer，设备接听ACK前不会采集。
async function configure(value) {
  if (peer) throw Error('call_already_started');
  const current = ++generation;
  if (value.peerEpoch) peerEpoch = value.peerEpoch;
  capture = await navigator.mediaDevices.getUserMedia({audio: {
    echoCancellation: true, noiseSuppression: true
  }, video: {width: 640, height: 480, frameRate: 15}});
  if (current !== generation || closed) {
    for (const track of capture.getTracks()) track.stop(); capture = null; return;
  }
  if (capture.getAudioTracks().length !== 1 || capture.getVideoTracks().length !== 1)
    throw Error('media_tracks_missing');
  item('local').srcObject = capture;
  item('local').pause(); item('local').style.visibility = 'hidden';
  peer = new RTCPeerConnection({iceServers: []});
  for (const track of capture.getTracks()) {
    track.onended = () => {
      if (current === generation && !closed) stop('采集设备已断开。');
    };
    peer.addTrack(track, capture);
  }
  peer.ontrack = event => {
    if (current !== generation || closed) return;
    if (event.track.kind === 'video') {
      item('remote').srcObject = new MediaStream([event.track]);
      item('remote').muted = true;
      item('remote').style.visibility = outputEnabled ? 'visible' : 'hidden';
      if (outputEnabled) void item('remote').play().catch(error => {
        // 拒接或挂断会主动清空srcObject，过期play承诺失败不代表新会话故障。
        if (current !== generation || error?.name === 'AbortError') return;
        stop('远端视频播放失败。');
      });
    } else {
      if (!callAudio || !callAudioNode || remoteAudioSource) { stop('音频末端不可用。'); return; }
      remoteAudioSource = callAudio.createMediaStreamSource(new MediaStream([event.track]));
      remoteAudioSource.connect(callAudioNode);
      // Chromium远端WebRTC轨需要原生媒体元素驱动解码；该元素始终静音，出声仍仅由原worklet许可控制。
      const audio = item('remoteAudio');
      audio.muted = true; audio.srcObject = new MediaStream([event.track]);
      void audio.play().catch(error => {
        // 原通话释放或换代引起的迟到play失败不能关闭后继Peer。
        if (current !== generation || closed || error?.name === 'AbortError') return;
        stop('远端音频解码启动失败。');
      });
    }
  };
  peer.onconnectionstatechange = () => {
    if (current !== generation) return;
    if (peer.connectionState === 'connected') send({kind: 'callConnected'});
    else if (['failed', 'disconnected'].includes(peer.connectionState))
      stop('音视频连接中断。');
  };
  const offer = await peer.createOffer();
  await peer.setLocalDescription(offer);
  await gatherComplete(current);
}
// incoming仅执行C++已校验的媒体命令，页面不生成云端信令。
function incoming(value) {
  if (!value || typeof value.kind !== 'string') throw Error('invalid_command');
  if (value.kind === 'callState') {
    if (!['connecting', 'available', 'inviting', 'ringing', 'negotiating', 'connected'].includes(value.state))
      throw Error('invalid_state');
    if (typeof value.sessionId === 'string' && value.sessionId) callSession = value.sessionId;
    state = value.state; buttons();
    status(value.state === 'ringing' ? '收到来电，请接听或拒接。' :
      value.state === 'connected' ? '音视频通话已接通。' :
      value.state === 'available' ? '等待来电。' :
      value.state === 'inviting' ? '正在呼叫目标设备。' :
      value.state === 'connecting' ? '正在连接平台。' : '正在协商媒体。');
    return;
  }
  if (value.kind === 'callConfigure') {
    if (Object.keys(value).length !== 2 || !fixedCounter(value.peerEpoch)) throw Error('invalid_configure');
    void configure(value).catch(() => stop('音视频设备初始化失败。')); return;
  }
  if (value.kind === 'callOutputGate') {
    if (closed || Object.keys(value).length !== 5 || typeof value.sessionId !== 'string' ||
        !fixedCounter(value.peerEpoch,true) || !fixedCounter(value.gateTicket) ||
        typeof value.enabled !== 'boolean' || BigInt(value.gateTicket) <= outputTicket)
      throw Error('invalid_output_gate');
    outputTicket = BigInt(value.gateTicket);
    void applyOutput(value).catch(failOutput); return;
  }
  if (value.kind === 'callHoldGate') {
    if (closed || Object.keys(value).length !== 7 || typeof value.sessionId !== 'string' ||
        !fixedCounter(value.peerEpoch) || !fixedCounter(value.ticket) || typeof value.held !== 'boolean' ||
        typeof value.controlToken !== 'string' || !value.controlToken ||
        !Number.isSafeInteger(value.revision) || value.revision < 1 || BigInt(value.ticket) <= holdTicket)
      throw Error('invalid_hold_gate');
    holdTicket = BigInt(value.ticket);
    void applyHold(value).catch(() => {
      if (closed || peerEpoch !== value.peerEpoch) return;
      holdPending = null; failOutput();
      send({...value,kind:'callHoldApplied',applied:false});
    }); return;
  }
  if (value.kind === 'callControlReceipt') {
    if (closed || Object.keys(value).length !== 10 || value.sessionId !== callSession ||
        typeof value.sessionId !== 'string' || value.sessionId.length > 64 ||
        !['cancel','hold','resume'].includes(value.operation) ||
        !['pending','completed','rejected','unknown'].includes(value.state) ||
        !Number.isSafeInteger(value.code) || !Number.isSafeInteger(value.revision) ||
        typeof value.actualHeld !== 'boolean' || typeof value.actualApplied !== 'boolean' ||
        !['','staleRevision','invalidState','rejected'].includes(value.reason) ||
        typeof value.controlToken !== 'string' || !value.controlToken || value.controlToken.length > 128)
      throw Error('invalid_control_receipt');
    if (controlReceipts.length < 24) controlReceipts.push({...value});
    controlPending = value.state === 'pending' || value.state === 'unknown'; buttons();
    if (value.state === 'rejected') status('平台拒绝该控制，原通话继续；已接通请使用挂断。');
    return;
  }
  if (value.kind === 'callControlRejected') { controlPending = false; buttons(); return; }
  if (value.kind === 'callTransferReceipt') {
    // 固定13字段与原腿隔离；ACK和事件分别存在，不能把200当实际完成或新腿媒体授权。
    const text = (field,limit,empty=false) => typeof value[field] === 'string' &&
      (empty || !!value[field]) && value[field].length <= limit && !/[\x00-\x1f\x7f]/.test(value[field]);
    const code = field => Number.isSafeInteger(value[field]) &&
      (value[field]===0 || value[field]===200 || (value[field]>=400 && value[field]<=599));
    const rejectReasons = ['','staleRevision','invalidState','rejected'];
    if (Object.keys(value).length !== 13 || !text('sessionId',64) || !text('controlToken',128) ||
        !text('cancelToken',128,true) || !code('code') || !code('cancelCode') ||
        !Number.isSafeInteger(value.ackRevision) || value.ackRevision < 0 ||
        !Number.isSafeInteger(value.resultRevision) || value.resultRevision < 0 ||
        !['pending','transferring','transferred','failed','cancelled','rejected','unknown'].includes(value.state) ||
        !['idle','pending','completed','rejected','unknown'].includes(value.cancelState) ||
        typeof value.candidatePresent !== 'boolean' || !rejectReasons.includes(value.cancelReason) ||
        ![...rejectReasons,'candidateCancelled','transferRejected','transferTimeout','transferMediaFailed',
          'transferUnavailable','transferRevisionChanged','transferPermissionRevoked'].includes(value.reason))
      throw Error('invalid_transfer_receipt');
    if (closed || state !== 'connected' || !peer || value.sessionId !== callSession) return;
    const index = transferReceipts.findIndex(record=>record.controlToken===value.controlToken && record.sessionId===value.sessionId);
    if (index >= 0) transferReceipts[index] = {...value};
    else if (transferReceipts.length < 24) transferReceipts.push({...value});
    else transferOverflow = true;
    // 已知旧操作的迟到结果只补账本，不能覆盖下一原操作的按钮等待。
    if (!latestTransfer || latestTransfer.controlToken === value.controlToken || index < 0) {
      latestTransfer = {...value};
      if (transferIntent === 'transfer' || (transferIntent === 'cancelTransfer' && value.cancelToken)) transferIntent = '';
      buttons();
      status(value.state === 'transferred' ? '转接已完成，等待平台结束原通话。' :
        value.state === 'unknown' || value.cancelState === 'unknown' ? '控制结果未知，不自动重试；原通话仍由平台确认。' :
        value.state === 'failed' || value.state === 'rejected' || value.cancelState === 'rejected' ? '平台拒绝或转接失败，原通话继续。' :
        value.state === 'cancelled' ? '候选已撤销，原通话继续。' : '转接处理中，原通话继续。');
    }
    return;
  }
  if (value.kind === 'callTransferRejected') {
    if (Object.keys(value).length !== 2 || !['transfer','cancelTransfer'].includes(value.operation))
      throw Error('invalid_transfer_rejected');
    if (transferIntent === value.operation) transferIntent = '';
    buttons(); return;
  }
  if (value.kind === 'callAnswer') {
    if (Object.keys(value).length !== 2 || typeof value.sdp !== 'string' ||
        value.sdp.length > 49152 || !value.sdp.startsWith('v=0\r\n') || !peer)
      throw Error('invalid_answer');
    void peer.setRemoteDescription({type: 'answer', sdp: value.sdp})
      .catch(() => stop('媒体协商失败。')); return;
  }
  if (value.kind === 'callStop') { releasePeer(); return; }
  if (value.kind === 'callTrackAck') {
    if (Object.keys(value).length !== 3 ||
        !['mic-1', 'video-1'].includes(value.trackId) ||
        !['active', 'paused'].includes(value.state) || !trackPending)
      throw Error('invalid_track_ack');
    trackPending = false; buttons(); return;
  }
  if (value.kind === 'ended' || value.kind === 'error') {
    stop(value.kind === 'error' ? '设备会话失败。' : '设备进程已结束。'); return;
  }
  throw Error('unknown_command');
}
// activate显式获取用户权限并验证H264编码能力，完成后立即停止试探采集。
async function activate() {
  if (state !== 'idle') return;
  state = 'connecting'; buttons();
  try {
    const checked = await navigator.mediaDevices.getUserMedia({audio: true, video: true});
    const supported = RTCRtpSender.getCapabilities('video')?.codecs.some(codec =>
      codec.mimeType.toLowerCase() === 'video/h264' &&
      /profile-level-id=(?:42|4d)/i.test(codec.sdpFmtpLine || ''));
    for (const track of checked.getTracks()) track.stop();
    if (!supported) throw Error('h264_unavailable');
    await callAudioReady();
    const response = await fetch('/bootstrap', {cache: 'no-store'});
    if (!response.ok) throw Error('pairing_failed');
    const auth = await response.json();
    if (typeof auth.token !== 'string' || !/^[a-f0-9]{64}$/.test(auth.token))
      throw Error('invalid_token');
    socket = new WebSocket(`ws://${location.host}/bridge?token=${auth.token}`);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(Error('pairing_timeout')), 5000);
      socket.onopen = () => { clearTimeout(timeout); resolve(); };
      socket.onerror = () => { clearTimeout(timeout); reject(Error('pairing_failed')); };
    });
    socket.onclose = () => stop('设备连接已断开。');
    socket.onerror = () => stop('设备连接故障。');
    socket.onmessage = event => {
      try {
        if (typeof event.data !== 'string' || event.data.length > 65536)
          throw Error('message_limit');
        incoming(JSON.parse(event.data));
      } catch { stop('本地媒体报文非法。'); }
    };
    if (!send({kind: 'ready'})) return;
    status('等待云端确认设备音视频能力。');
    setInterval(() => { void mediaStats().catch(() => stop('媒体统计失败。')); }, 250);
  } catch { stop('音视频设备或H264编码不可用。'); }
}
item('activate').addEventListener('click', activate);
item('dial').addEventListener('click', () => {
  if (state !== 'available') return;
  const target = item('target').value.trim();
  if (target.length < 3 || target.length > 128 || !/^[^:/+#\x00-\x1f\x7f]+:[^/+#\x00-\x1f\x7f]+$/.test(target)) {
    status('目标设备格式应为 productID:deviceName。'); return;
  }
  // 先锁定本地按钮，避免云端状态回报到达前双击产生第二次会话创建。
  state = 'inviting'; buttons(); status('正在呼叫目标设备。');
  send({kind: 'callDial', target});
});
item('accept').addEventListener('click', () => send({kind: 'callAccept'}));
item('reject').addEventListener('click', () => send({kind: 'callReject'}));
item('hangup').addEventListener('click', () => send({kind: 'callHangup'}));
// controlIntent 用户只提交固定操作，原角色/版本/token均由C++核心与平台审核。
for (const operation of ['hold','resume','cancel']) item(operation).addEventListener('click', () => {
  if (item(operation).disabled || closed) return;
  controlPending = true; buttons(); send({kind:'callControl',operation});
});
// 转接目标只是用户意图；候选腿、原版本及两个原token始终由C++核心维护。
item('transfer').addEventListener('click', () => {
  if (item('transfer').disabled || closed) return;
  const target = item('transferTarget').value;
  if (typeof target !== 'string' || target.length < 3 || target.length > 128 ||
      !/^[^:/+#\x00-\x1f\x7f]+:[^/+#\x00-\x1f\x7f]+$/.test(target)) {
    status('转接设备格式应为 productID:deviceName。'); return;
  }
  transferIntent = 'transfer'; buttons(); send({kind:'callControl',operation:'transfer',target});
});
item('cancelTransfer').addEventListener('click', () => {
  if (item('cancelTransfer').disabled || closed) return;
  transferIntent = 'cancelTransfer'; buttons(); send({kind:'callControl',operation:'cancelTransfer'});
});
// toggleTrack先改变真实驱动轨再报告平台；取消后不能由远端重新打开本地摄像头/麦克风。
function toggleTrack(kind) {
  if (state !== 'connected' || trackPending || !capture) return;
  const track = kind === 'audio' ? capture.getAudioTracks()[0] : capture.getVideoTracks()[0];
  if (!track || track.readyState !== 'live') { stop('本地媒体轨不可用。'); return; }
  track.enabled = !track.enabled;
  const paused = !track.enabled;
  item(kind === 'audio' ? 'mute' : 'camera').textContent = kind === 'audio' ?
    (paused ? '取消静音' : '静音') : (paused ? '开摄像头' : '关摄像头');
  trackPending = true; buttons();
  send({kind:'callTrack', trackId:kind === 'audio' ? 'mic-1' : 'video-1',
    state:paused ? 'paused' : 'active'});
}
item('mute').addEventListener('click', () => toggleTrack('audio'));
item('camera').addEventListener('click', () => toggleTrack('video'));
window.addEventListener('pagehide', () => stop('页面已关闭。'));
