// 桌面界面只持本地短期桥令牌，不读取设备三元组，不直接访问云端MQTT或模型API。
'use strict';
const element = id => document.getElementById(id);
const textOnlyDevice = globalThis.UrTextOnlyDevice === true; // 仅由C++同源资产装配，不从URL提升媒体能力。
let socket; // 唯一已配对回环连接，关闭后不自动重放命令。
let aiControlState = null; // 仅原生明确能力的实际控制恢复投影，观察不授予采集或重新联网。
let aiEndpointState = null; // 接续本人操作的闭集投影，不接受平台身份或自行推断媒体授权。
let aiEndpointSourceState = null; // 原源控制器闭集事实，不含原腿、候选或请求身份。
let context; // 与协议显式格式一致的AudioContext。
let stream; // 用户授权的麦克风，终态必须stop全部轨。
let rtcVisionConfiguration = null; // 原native明确启用的暂停视频轨快照；不包含摄像头采集或本人同意。
let rtcVisionControl = null; // 当前原Core控制；代次退休立即撤意图，异步完成不能复活旧发布。
let rtcVisionScope = null; // 首个合法Core控制固定本会话/轨/媒体代次，不从Peer connected补造。
let rtcVisionGeneration = 0n; // 本地原控制水位，原msgToken仍仅由Core账本关联。
let worklet; // 输入输出共享音频线程，方向状态彼此独立。
let captureCommand = null; // 当前原采集命令，旧音频线程回执不能盖过新启停意图。
let source; // 麦克风到处理器的连接。
let inputOwnership; // 仅专用宿主显式装配的原唯一采集门控，缺省设备行为不变。
let duplexHumanMedia; // 专用宿主的原Peer/Sender注册器；缺省未知，不能凭会话字符串创建采集。
const observeAudioFailure = globalThis.UrAudioFailureDiagnostics === true; // 仅原native明确开关，不从URL/业务消息授权。
let audioFailureCause = null; // 只留原音频线程的首个合法固定旁证，release后也不清除或重新采样。
const audioFailureCauses = new Set(['configurationInvalid', 'commandInvalid', 'unknownCommand',
  'captureStartInvalid', 'zeroPendingAck', 'captureSampleInvalid', 'captureBacklogFull',
  'playbackStartInvalid', 'playbackFrameInvalid', 'playbackQueueFull', 'playbackGateInvalid',
  'playbackGateConflict', 'playbackGatePending', 'ownershipGateInvalid', 'ownershipTicketConflict',
  'ownershipGatePending', 'ownershipOwnerConflict', 'ownershipRouteCapacity', 'unknown']); // 闭集不允许异常正文或媒体身份。
let handoffState; // 仅原C++七字段观察；用于按钮可用性，不是平台会话、租约或媒体权限。
let humanControlState; // 独立原核心控制观察，不把网络held或点击解释成实际停流。
const humanReceiveOnly = globalThis.UrHumanReceiveOnly === true; // 固定原生入口的文案角色，不启用媒体或授予接听。
const humanDialEnabled = humanReceiveOnly && globalThis.UrHumanVoiceEnabled === true &&
  globalThis.UrHumanEndpointSource === true && globalThis.UrHumanDialEnabled === true;
// humanDialEnabled仅由显式纯Human宿主装配；仍需原C++可用投影，不从按钮或来电取得许可。
const temporaryCalls = globalThis.UrTemporaryCalls === true && globalThis.UrHumanVoiceEnabled === true &&
  !humanReceiveOnly; // 仅native完整共享宿主显式装配普通临时通话，不从邀请或URL开启。
let temporaryResumeAllowed = false; // 本机恢复按钮资格；不是原完整组或麦克风实际许可。
const handoffButtons = {escalate: ['handoffEscalate', 'canEscalate'],
  accept: ['handoffAccept', 'canAccept'], reject: ['handoffReject', 'canReject'],
  cancel: ['handoffCancel', 'canCancel'], hangup: ['handoffHangup', 'canHangup']}; // 固定本地意图与按钮映射。
// recordHandoffState严格读取原宿主的状态投影，额外身份字段或终态后投影不得改变按钮。
function recordHandoffState(value) {
  const fields = ['kind', 'state', ...Object.values(handoffButtons).map(([, flag]) => flag)];
  if (terminated || globalThis.UrHumanVoiceEnabled !== true || !value ||
      value.kind !== 'handoffState' || Object.keys(value).length !== fields.length ||
      Object.keys(value).some(key => !fields.includes(key)) ||
      !Number.isInteger(value.state) || value.state < 0 || value.state > 10 ||
      Object.values(handoffButtons).some(([, flag]) => typeof value[flag] !== 'boolean'))
    throw new Error('invalid_handoff_state');
  handoffState = {...value};
  if (globalThis.UrHumanEndpointSource === true) element('humanInputStop').disabled = !value.canHangup;
  for (const [id, flag] of Object.values(handoffButtons)) element(id).disabled = !value[flag];
  const labels = ['AI会话', '绑定候选', '等待接听', '暂停AI资源', 'AI资源已暂停',
    '申请人工资源', '人工资源已取得，等待平台提交', '恢复AI资源', '人工通话',
    '移交结果待确认', '移交已关闭']; // 顺序与C++ AiHandoffState一致，实际取得不等于平台提交。
  const receiveLabels = ['等待语音来电', '准备语音会话', '语音来电处理中', '准备人工通话',
    '准备人工通话', '申请语音资源', '语音资源已取得，等待平台建立', '准备人工通话',
    '人工语音通话', '通话状态待确认', '人工语音通话已结束']; // 资源实际取得与平台建立仍分开。
  element('handoffStatus').textContent = (humanReceiveOnly ? receiveLabels : labels)[value.state];
}
// validHandoffTarget保持公共标识的UTF8字节边界；不接受控制符或被TextEncoder替换的孤立代理码位。
function validHandoffTarget(value) {
  return typeof value === 'string' && value.length > 0 && new TextEncoder().encode(value).length <= 128 &&
    [...value].every(character => {
      const code = character.codePointAt(0);
      return code >= 0x20 && code !== 0x7f && (code < 0xd800 || code > 0xdfff);
    });
}
// submitHandoffIntent只提交明确用户操作；不在页面构造session/focus、调用采集门控或推断操作成功。
function submitHandoffIntent(operation) {
  const button = handoffButtons[operation];
  if (terminated || globalThis.UrHumanVoiceEnabled !== true || !button ||
      !handoffState?.[button[1]]) return;
  const command = {kind: 'handoffControl', operation: (temporaryCalls || humanDialEnabled) && operation === 'escalate' ? 'dial' : operation};
  if (operation === 'escalate') {
    const targetKind = element('handoffTargetKind').value;
    const targetId = element('handoffTarget').value;
    if (!((temporaryCalls || humanDialEnabled) ? targetKind === 'device' : ['device', 'service'].includes(targetKind)) || !validHandoffTarget(targetId)) {
      element('handoffStatus').textContent = '请选择明确目标并填写有效标识。';
      return;
    }
    Object.assign(command, {targetKind, targetId});
    if ((temporaryCalls || humanDialEnabled) && element('handoffShareContext').checked === true) {
      element('handoffStatus').textContent = '此通话不分享AI上下文。';
      return;
    }
    if (element('handoffShareContext').checked === true) {
      const consentId = element('handoffConsentId').value;
      if (targetKind !== 'service' || !validHandoffTarget(consentId)) {
        element('handoffStatus').textContent = '分享须选择服务并填写本人已授权的引用。';
        return;
      }
      Object.assign(command, {shareContext: true, consentId});
    }
  }
  send(command); // 受理与资源状态必须等待C++后续事实，点击不伪造成功回执。
}
const ownershipBoot = globalThis.UrInputOwnershipBoot; // 只读宿主固定启动代次，不从URL或平台正文推导。
// recordHumanControlState 独立接纳固定HAL闭集，不改变原七字段或自动恢复未知控制。
function recordHumanControlState(value) {
  const fields = ['kind', 'canHold', 'canResume', 'held', 'controlState'];
  if (terminated || globalThis.UrHumanVoiceEnabled !== true || !value ||
      Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
      ['canHold', 'canResume', 'held'].some(key => typeof value[key] !== 'boolean') ||
      !['idle', 'pending', 'completed', 'rejected', 'unknown'].includes(value.controlState) ||
      (value.canHold && (value.held || value.canResume)) || (value.canResume && !value.held) ||
      (['pending', 'unknown'].includes(value.controlState) && (value.canHold || value.canResume)))
    throw new Error('invalid_human_control_state');
  humanControlState = {...value};
  element('handoffHold').disabled = !value.canHold;
  element('handoffResume').disabled = !value.canResume;
  const labels = {idle: '人工控制就绪', pending: '等待原控制完成', completed: '原控制已完成',
    rejected: '原控制被拒绝', unknown: '控制结果待确认，禁止自动重试'};
  element('humanControlStatus').textContent = labels[value.controlState] + (value.held ? '（实际已保持）' : '');
}
// recordHumanControlReceipt token只留32条内存，完成须原ACK和实际门控，不从UI推断。
function recordHumanControlReceipt(value) {
  const fields = ['kind', 'operation', 'controlToken', 'state', 'reason', 'revision', 'code',
    'actualApplied', 'actualHeld', 'receiptIndex'];
  if (terminated || globalThis.UrHumanVoiceEnabled !== true || !value ||
      Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
      !['hold', 'resume'].includes(value.operation) || !validHandoffTarget(value.controlToken) ||
      !['idle', 'pending', 'completed', 'rejected', 'unknown'].includes(value.state) ||
      !['', 'staleRevision', 'invalidState', 'rejected'].includes(value.reason) ||
      !Number.isSafeInteger(value.revision) || value.revision < 0 ||
      !Number.isInteger(value.code) || (value.code !== 0 && value.code !== 200 && (value.code < 400 || value.code > 599)) ||
      typeof value.actualApplied !== 'boolean' || typeof value.actualHeld !== 'boolean' ||
      !Number.isSafeInteger(value.receiptIndex) || value.receiptIndex <= metrics.humanControlReceiptIndex || value.receiptIndex > 512 ||
      (value.state === 'completed' && (value.code !== 200 || !value.actualApplied || value.actualHeld !== (value.operation === 'hold'))))
    throw new Error('invalid_human_control_receipt');
  metrics.humanControlReceiptIndex = value.receiptIndex;
  metrics.humanControlReceipts.push({...value});
  if (metrics.humanControlReceipts.length > 32) metrics.humanControlReceipts.shift();
}
// submitHumanControl 仅请求原HumanClient，不在页面执行焦点、Sender或播放器副作用。
function submitHumanControl(operation) {
  if (terminated || globalThis.UrHumanVoiceEnabled !== true ||
      !['hold', 'resume'].includes(operation) ||
      !humanControlState?.[operation === 'hold' ? 'canHold' : 'canResume']) return;
  send({kind: 'handoffControl', operation});
}
// captureSampleRate只读取专用宿主固定格式；旧独立UDP页继续16k，不从来电或URL偷换实际格式。
function captureSampleRate() {
  if (ownershipBoot === undefined) return 16000;
  const configured = globalThis.UrInputOwnershipSampleRate ?? 16000;
  if (configured !== 16000 && configured !== 48000) throw new Error('invalid_input_format');
  return configured;
}
// initializeHumanVoiceMedia只在专用共享宿主装配；借原stream/source/context，不自行申请另一路媒体。
async function initializeHumanVoiceMedia() {
  if (globalThis.UrHumanVoiceEnabled !== true) return;
  if (terminated || !inputOwnership || ownershipBoot === undefined ||
      captureSampleRate() !== 48000 || typeof globalThis.UrHumanVoiceMedia !== 'function' ||
      duplexHumanMedia || desktopFiles || desktopLive || useWebRTC)
    throw new Error('invalid_human_voice_assembly');
  duplexHumanMedia = new UrHumanVoiceMedia({stream, source, context, emit: send,
    emitDiagnostic: sendHumanStopDiagnostic}); // 默认不启用，只借同一socket的非release旁证出口。
  if (!await duplexHumanMedia.initialize(ownershipBoot)) throw new Error('invalid_human_voice_assembly');
  if (terminated) { duplexHumanMedia.close(); throw new Error('human_voice_closed'); }
}
let desktopFiles; // 仅C++显式全媒体资产提供文件宿主，不由浏览器选择平台能力。
let desktopLive; // 仅C++显式Live资产装配原同身份接收器；不访问云端或复用文件许可。
let clientTheme; // 独立审核主题端口，只接本地票据，不接平台执行令牌或身份。
const useWebRTC = typeof location !== 'undefined' &&
  new URLSearchParams(location.search).get('media') === 'webrtc'; // 模式必须与C++启动参数一致。
let rtcPeer; // 仅桌面驱动持有Peer，云端设备身份仍在C++。
let rtcEndpointSender = null; // AI候选只绑定本次明确创建的单audio Sender，不按null轨猜归属。
let endpointOwnershipSource = null; // 原麦克风探针独立于远端解码source，回复清理不得断开采集所有权端口。
let rtcPeerRevision = 1n, rtcWasConnected = false, rtcDisconnected = false;
let rtcCancelGather = null; // 至多一个原Peer候选等待器；退休立即取消，不挂到八秒以后。
let rtcSink; // 静音HTML播放管线保持远端音轨解码；实际输出仅由WebAudio增益控制。
let rtcGain; // 当前回复本地增益，取消时立即归零。
const rtcOutputFocus = globalThis.UrWebRTCOutputFocus === true; // 仅固定原生宿主开启，不从URL或来电授予焦点。
let rtcOutputNode, rtcPresentedAnalyser; // 原48k末端实际Worklet及门后采样，独立于网络解码观察。
let rtcGatePending = null, rtcGateTicket = 0n;
let rtcGateKnown = false, rtcGateEnabled = false; // 原实际处理块完成才更新，不把目标或Gain=0当idle。
let rtcAnalyser; // 软件音频输出采样事实，不等于物理扬声器验收。
let rtcTimer; // 检测远端实际有声与回复尾部静音的有界调度。
let rtcVolume = 100; // 与C++本地动作驱动同步的目标增益。
let rtcFinishAt = 0; // 仅当前回复等待排空；0表示无完成请求。
let rtcSilence = 0; // 完成请求后的连续静音观察数。
let rtcLastVoicedAt = 0; // 迟到RTP不能被较早静音观察误判已排空。

// rtcOutputActual只接本节点原实际音频线程回执；固定票据不参与平台授权或回复代次。
function rtcOutputActual(node, value) {
  if (terminated || node !== rtcOutputNode) return;
  if (value?.kind === 'callAudioStats') return; // 原节点饱和统计不作为本次门控完成事实。
  if (value?.kind !== 'callAudioGated' || Object.keys(value).length !== 4 ||
      value.peerEpoch !== '0' || typeof value.enabled !== 'boolean' || typeof value.ticket !== 'string' ||
      !/^[1-9][0-9]{0,19}$/.test(value.ticket) || BigInt(value.ticket) > 18446744073709551615n)
    return release('WebRTC输出实态无效，已释放设备资源。');
  if (BigInt(value.ticket) < rtcGateTicket) return; // 旧原票据不破坏后继实际门控。
  if (!rtcGatePending) {
    if (BigInt(value.ticket) === rtcGateTicket && rtcGateKnown && value.enabled === rtcGateEnabled) return;
    return release('WebRTC输出实态无效，已释放设备资源。');
  }
  if (value.ticket !== rtcGatePending.ticket || value.enabled !== rtcGatePending.enabled)
    return release('WebRTC输出实态无效，已释放设备资源。');
  rtcGateKnown = true; rtcGateEnabled = value.enabled; rtcGatePending = null;
  if (!send({kind: 'outputGated', ticket: value.ticket, enabled: value.enabled}))
    release('WebRTC输出确认失败，已释放设备资源。');
}
// rtcOutputGate只排既有playbackGate局部票据，实际确认由音频块完成；不触碰麦克风或音量。
function rtcOutputGate(value) {
  if (!rtcOutputFocus || !rtcOutputNode || Object.keys(value).length !== 3 ||
      typeof value.enabled !== 'boolean' || typeof value.ticket !== 'string' ||
      !/^[1-9][0-9]{0,19}$/.test(value.ticket) ||
      BigInt(value.ticket) > 18446744073709551615n || BigInt(value.ticket) <= rtcGateTicket ||
      rtcGatePending)
    throw new Error('invalid_rtc_output_gate');
  rtcGateTicket = BigInt(value.ticket);
  rtcGateKnown = false;
  rtcGatePending = {ticket: value.ticket, enabled: value.enabled};
  rtcOutputNode.port.postMessage({kind: 'callAudioMask', peerEpoch: '0',
    ticket: value.ticket, enabled: value.enabled});
}
// initializeRTCOutput保持同一个原Context，无远端输入也须由非空实际输出块确认首次false。
async function initializeRTCOutput() {
  if (!rtcOutputFocus) return;
  await context.audioWorklet.addModule('/call-worklet.js');
  if (terminated) throw new Error('rtc_output_closed');
  const node = new AudioWorkletNode(context, 'ur-call-audio', {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
    channelCount: 1, channelCountMode: 'explicit'
  });
  rtcOutputNode = node;
  rtcPresentedAnalyser = context.createAnalyser();
  rtcPresentedAnalyser.fftSize = 1024;
  node.port.onmessage = event => rtcOutputActual(node, event.data);
  node.onprocessorerror = () => release('WebRTC输出驱动失败，已释放设备资源。');
  node.connect(rtcPresentedAnalyser);
  rtcPresentedAnalyser.connect(context.destination);
}
// rtcStopVoice只退休原AI Peer与解码图；保留WS、节目、Context和末端以完成独立release。
function rtcStopVoice() {
  rtcCancelVision(); // 先关本次摄像头轨，再退休Peer；晚GUM或替轨承诺须沿原对象清理。
  if (rtcCancelGather) { rtcCancelGather(); rtcCancelGather = null; }
  if (rtcTimer) { clearInterval(rtcTimer); rtcTimer = null; }
  if (rtcPeer) {
    rtcPeer.ontrack = null; rtcPeer.onconnectionstatechange = null;
    for (const sender of rtcPeer.getSenders()) if (sender.track) sender.track.enabled = false;
    rtcPeer.close(); rtcPeer = null;
  }
  rtcEndpointSender = null;
  rtcClearReply();
}
// rtcVisionStopTracks仅结束本publish取得的流；不碰原麦克风、预览或其它人际会话。
function rtcVisionStopTracks(record) {
  if (record?.camera) for (const track of record.camera.getTracks()) track.stop();
}
// rtcVisionDetach在原替轨承诺完成后清理原Sender；不借Peer关闭推断实际null或新会话许可。
async function rtcVisionDetach(record) {
  try {
    rtcVisionStopTracks(record);
    const ended = () => !record?.camera || record.camera.getTracks().every(track => track.readyState === 'ended');
    if (!record?.sender || record.sender.track === null) return ended();
    if (!record.track || record.sender.track !== record.track) return false;
    await record.sender.replaceTrack(null);
    return record.sender.track === null && ended();
  }
  catch { return false; }
}
// rtcVisionLive只读原Peer/唯一Sender和当前代次；旧异步任务不能绑定新Peer。
function rtcVisionLive(record) {
  return record.valid && record === rtcVisionControl && !terminated && !rtcDisconnected &&
    rtcPeer === record.peer && rtcPeerRevision.toString() === record.control.peerRevision &&
    record.peer.connectionState === 'connected' && record.peer.getSenders().includes(record.sender);
}
// rtcVisionActual只报告原实际完成的本地事实；调用受理、getUM授权与queued命令都不构成active/stopped。
function rtcVisionActual(record, state, error = '') {
  if (!rtcVisionLive(record) || record.reported) return;
  if (state === 'active' && (!record.track || record.track.readyState !== 'live' ||
      record.sender.track !== record.track)) { state = 'failed'; error = 'mediaFailed'; }
  if (state === 'stopped' && record.sender.track !== null) { state = 'failed'; error = 'mediaFailed'; }
  record.failed = state === 'failed'; // 失败不证明本次资源释放；再次publish必须先完成独立Stop。
  record.reported = true;
  const fact = {...record.control, kind: 'webrtcVisionActual', state, error};
  delete fact.operation;
  send(fact); // 失败不是摄像头已停止，更不据此结束原AI语音或发云端trackState。
}
// rtcCancelVision撤当前意图并同步stop已取得的全部轨；未知在途承诺没有被包装成Stopped。
function rtcCancelVision(generation) {
  const record = rtcVisionControl;
  if (!record || (generation !== undefined && generation !== record.control.generation)) return;
  record.valid = false;
  rtcVisionStopTracks(record);
  // 原work会等GUM/replaceTrack完成并再次精确null，先行null不能掩盖晚到true。
  void rtcVisionDetach(record);
}
// rtcVisionPublish只取原物理相机；实际轨、deviceId、Sender替轨承诺和scope须共同成立。
async function rtcVisionPublish(record) {
  if (!rtcVisionConfiguration.deviceId || typeof navigator.mediaDevices?.getUserMedia !== 'function') {
    rtcVisionActual(record, 'failed', 'unsupported'); return;
  }
  try {
    const camera = await navigator.mediaDevices.getUserMedia({audio: false,
      video: {deviceId: {exact: rtcVisionConfiguration.deviceId}}});
    record.camera = camera; // 从此所有退出路径都按原返回流回收，不能只停一个自选track。
    if (!rtcVisionLive(record)) { await rtcVisionDetach(record); return; }
    const tracks = camera.getVideoTracks();
    if (tracks.length !== 1 || camera.getAudioTracks().length !== 0 || camera.getTracks().length !== 1 ||
        tracks[0].kind !== 'video' || tracks[0].readyState !== 'live' ||
        tracks[0].getSettings().deviceId !== rtcVisionConfiguration.deviceId)
      throw new Error('invalid_camera_source');
    record.track = tracks[0];
    await record.sender.replaceTrack(record.track);
    if (!rtcVisionLive(record)) { await rtcVisionDetach(record); return; }
    if (record.sender.track !== record.track || record.track.readyState !== 'live')
      throw new Error('invalid_camera_sender');
    rtcVisionActual(record, 'active');
  } catch {
    await rtcVisionDetach(record);
    rtcVisionActual(record, 'failed', 'mediaFailed'); // 不记录浏览器错误文本或请求设备身份。
  }
}
// rtcVisionIncoming只处理已鉴权CPP的固定本地命令；原token/权限仍由Core检验，网页不能签发同意。
function rtcVisionIncoming(value) {
  const cancelling = value.kind === 'webrtcVisionCancel';
  const fields = ['kind', 'generation', 'sessionId', 'cameraId', 'trackId', 'mediaRevision', 'peerRevision',
    ...(cancelling ? [] : ['operation'])];
  const positive = text => typeof text === 'string' && /^[1-9][0-9]{0,19}$/.test(text) &&
    BigInt(text) <= 18446744073709551615n;
  if (terminated || !rtcVisionConfiguration || !rtcPeer || !rtcWasConnected || rtcDisconnected ||
      Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
      !positive(value.generation) || !positive(value.mediaRevision) ||
      BigInt(value.mediaRevision) > 9007199254740991n || !validHandoffTarget(value.sessionId) ||
      new TextEncoder().encode(value.sessionId).length > 64 ||
      value.cameraId !== rtcVisionConfiguration.cameraId || value.trackId !== rtcVisionConfiguration.videoTrack ||
      (!cancelling && !['publish', 'stop'].includes(value.operation)))
    throw new Error('invalid_vision_control');
  const scope = {sessionId: value.sessionId, cameraId: value.cameraId, trackId: value.trackId,
    mediaRevision: value.mediaRevision, peerRevision: value.peerRevision};
  if (rtcVisionScope && Object.keys(scope).some(key => scope[key] !== rtcVisionScope[key]))
    throw new Error('invalid_vision_control');
  if (cancelling) { rtcCancelVision(value.generation); return; }
  const generation = BigInt(value.generation), previous = rtcVisionControl;
  if (generation < rtcVisionGeneration) return; // 退休控制不再次采集、不重发旧ACK。
  if (generation === rtcVisionGeneration) {
    if (!previous || fields.some(key => value[key] !== previous.control[key])) throw new Error('invalid_vision_control');
    return;
  }
  if (value.operation === 'publish' && previous && (previous.pending || previous.failed || previous.sender.track !== null ||
      (!previous.valid && previous.control.operation === 'publish'))) throw new Error('invalid_vision_control');
  // 只有原视频transceiver提供的Sender可接摄像头；不借音频/Human Sender推导画面发布。
  const sender = rtcPeer.getTransceivers?.().find(item => item === rtcVisionConfiguration.transceiver)?.sender;
  if (!sender || typeof sender.replaceTrack !== 'function') throw new Error('invalid_vision_control');
  if (previous && value.operation === 'stop') rtcCancelVision();
  const record = {control: Object.freeze({...value}), peer: rtcPeer, sender,
    valid: true, reported: false, pending: true, camera: null, track: null, work: null};
  rtcVisionScope = Object.freeze(scope); rtcVisionGeneration = generation; rtcVisionControl = record;
  record.work = value.operation === 'publish' ? rtcVisionPublish(record) : (async () => {
    if (previous?.work) await previous.work; // 等原GUM/替轨实退，不在未知时假报Stopped。
    if (previous && !await rtcVisionDetach(previous)) { rtcVisionActual(record, 'failed', 'mediaFailed'); return; }
    rtcVisionActual(record, 'stopped');
  })();
  // 异步拒绝同样结束原等待并回固定失败；派生Promise不留未处理拒绝，也不冒称Stopped。
  record.work = record.work.then(() => { record.pending = false; }, () => {
    record.pending = false;
    rtcVisionActual(record, 'failed', 'mediaFailed');
  });
}
// rtcClearReply断开旧远端解码图；共享末端Worklet无排队PCM，不清原物理门票据或节目。
function rtcClearReply() {
  if (rtcGain) rtcGain.gain.value = 0; // 先禁旧意图再断图，原末端票据不提供新回复播放意图。
  if (rtcSink) { rtcSink.pause(); rtcSink.srcObject = null; rtcSink = null; }
  if (source) { source.disconnect(); source = null; }
  rtcGain?.disconnect?.(); rtcAnalyser?.disconnect?.();
  rtcGain = rtcAnalyser = null;
  responseId = playbackEpoch = '';
  rtcFinishAt = rtcSilence = 0;
  rtcLastVoicedAt = 0;
  currentReplyConsecutiveConsumedNonSilent = 0;
  currentReplyPlayable = false;
}
// rtcCreatePeer仅重建AI网络腿；同一已授权stream/context不重新采集或声明ready。
function rtcCreatePeer(revision, vision = null) {
  const peer = new RTCPeerConnection({iceServers: []});
  rtcPeer = peer; rtcPeerRevision = revision;
  rtcWasConnected = rtcDisconnected = false;
  const track = stream.getAudioTracks()[0];
  const candidate = globalThis.UrAIEndpointListen === true;
  if (!candidate) track.enabled = false;
  if (vision) {
    // 三轨按原平台方向创建；视频Sender为空，不从预览或同意引用自动取得物理摄像头。
    try {
      peer.addTransceiver(track, {direction: 'sendonly', streams: [stream]});
      peer.addTransceiver('audio', {direction: 'recvonly'});
      const video = peer.addTransceiver('video', {direction: 'sendonly'});
      if (video.sender.track !== null || typeof video.setCodecPreferences !== 'function')
        throw new Error('invalid_vision_sender');
      video.setCodecPreferences(vision.codecs);
      rtcVisionConfiguration = Object.freeze({...vision, transceiver: video}); // 锁原实际transceiver，不按track.kind猜Sender。
    } catch (error) {
      peer.close(); rtcPeer = null; // 只回收本候选；不停止共享麦克风或另建摄像头Source。
      throw error;
    }
  } else if (candidate) {
    if (ownershipBoot === undefined || !inputOwnership || rtcEndpointSender)
      throw new Error('invalid_endpoint_sender');
    rtcEndpointSender = peer.addTransceiver('audio', {direction: 'sendrecv'}).sender;
    if (!rtcEndpointSender || rtcEndpointSender.track !== null)
      throw new Error('invalid_endpoint_sender');
  } else peer.addTrack(track, stream);
  peer.ontrack = event => {
    if (!terminated && rtcPeer === peer && rtcPeerRevision === revision && !rtcDisconnected)
      rtcAttachRemote(event, peer);
  };
  peer.onconnectionstatechange = () => {
    if (terminated || rtcPeer !== peer || rtcPeerRevision !== revision) return;
    if (peer.connectionState === 'connected' && !rtcDisconnected && !rtcWasConnected) {
      rtcWasConnected = true;
      send({kind: 'webrtcConnected', peerRevision: revision.toString()});
    } else if (peer.connectionState === 'disconnected' && rtcWasConnected && !rtcDisconnected) {
      rtcDisconnected = true;
      for (const sender of peer.getSenders()) if (sender.track) sender.track.enabled = false;
      rtcClearReply();
      send({kind: 'webrtcDisconnected', peerRevision: revision.toString()});
    } else if (peer.connectionState === 'failed')
      release('WebRTC媒体连接失败，已释放设备资源。');
  };
  rtcTimer = setInterval(rtcObserve, 20);
}
// rtcPreflightVision仅确认原浏览器已有camera许可、明确设备及H264协商能力；不隐式申请画面或本人AI授权。
async function rtcPreflightVision() {
  const native = globalThis.UrAIVisionCamera;
  if (native === undefined) return; // 默认关闭，原音频不读取视觉API也不新增ready消息。
  const valid = (value, limit) => typeof value === 'string' && value.length > 0 &&
    new TextEncoder().encode(value).byteLength <= limit && !/[\u0000-\u001f\u007f\ud800-\udfff]/u.test(value);
  if (!native || typeof native !== 'object' || Array.isArray(native) || Object.keys(native).length !== 3 ||
      Object.keys(native).some(key => !['cameraId', 'videoTrack', 'deviceId'].includes(key)) ||
      !valid(native.cameraId, 64) || !valid(native.videoTrack, 64) || !valid(native.deviceId, 256) ||
      ['mic-1', 'speaker-1'].includes(native.videoTrack)) throw new Error('invalid_vision_preflight');
  const original = Object.freeze({...native}); // 异步期间原native配置不能被改成另一物理相机。
  let supported = false;
  try {
    const formats = globalThis.RTCRtpSender?.getCapabilities?.('video');
    if (typeof globalThis.RTCPeerConnection?.prototype?.addTransceiver === 'function' &&
        formats?.codecs?.some(codec => typeof codec.mimeType === 'string' && codec.mimeType.toLowerCase() === 'video/h264') &&
        typeof navigator.permissions?.query === 'function' && typeof navigator.mediaDevices?.enumerateDevices === 'function') {
      const permission = await navigator.permissions.query({name: 'camera'});
      if (permission.state === 'granted') {
        const devices = await navigator.mediaDevices.enumerateDevices();
        supported = Array.isArray(devices) && devices.length <= 128 &&
          devices.filter(device => device.kind === 'videoinput' && device.deviceId === original.deviceId).length === 1;
      }
    }
  } catch { supported = false; } // 未授权、隐藏设备或API不可证均拒能力，不回显浏览器错误。
  if (!terminated) send({kind: 'webrtcVisionReady', ...original, state: supported ? 'ready' : 'unsupported'});
}
// rtcVisionConfigurationFor只接受原native端口选择及浏览器实际H264能力；不调用任何采集API或返回发布许可。
function rtcVisionConfigurationFor(value) {
  const fields = ['kind', 'peerRevision', 'cameraId', 'videoTrack', 'codec', 'state'];
  const native = globalThis.UrAIVisionCamera;
  const validId = (id, limit = 64) => typeof id === 'string' && id.length > 0 &&
    new TextEncoder().encode(id).byteLength <= limit && !/[\u0000-\u001f\u007f\ud800-\udfff]/u.test(id);
  const audio = stream?.getAudioTracks();
  if (terminated || rtcVisionConfiguration || !rtcPeer || rtcDisconnected || rtcWasConnected ||
      !audio || audio.length !== 1 || audio[0].kind !== 'audio' || audio[0].readyState !== 'live' ||
      !native || typeof native !== 'object' || Array.isArray(native) ||
      ![2, 3].includes(Object.keys(native).length) ||
      Object.keys(native).some(key => !['cameraId', 'videoTrack', 'deviceId'].includes(key)) ||
      (Object.hasOwn(native, 'deviceId') && !validId(native.deviceId, 256)) ||
      !Object.hasOwn(native, 'cameraId') || !Object.hasOwn(native, 'videoTrack') ||
      Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
      !validId(value.cameraId) || !validId(value.videoTrack) ||
      value.cameraId !== native.cameraId || value.videoTrack !== native.videoTrack ||
      ['mic-1', 'speaker-1'].includes(value.videoTrack) || value.codec !== 'H264' || value.state !== 'paused' ||
      typeof globalThis.RTCRtpSender?.getCapabilities !== 'function' ||
      typeof globalThis.RTCPeerConnection?.prototype?.addTransceiver !== 'function')
    throw new Error('invalid_vision_config');
  const capabilities = RTCRtpSender.getCapabilities('video');
  const codecs = capabilities?.codecs?.filter(codec =>
    typeof codec.mimeType === 'string' && codec.mimeType.toLowerCase() === 'video/h264');
  if (!codecs?.length) throw new Error('invalid_vision_config');
  return Object.freeze({cameraId: value.cameraId, videoTrack: value.videoTrack, codecs,
    deviceId: native.deviceId}); // 物理选择按原native快照固定；缺失只允许paused建连，不可默认开相机。
}
// initializeProgramOutputs复用原file/live组件，两传输共用；类型存在不等于平台授权或实际idle。
async function initializeProgramOutputs(bootstrapToken) {
  if (typeof globalThis.UrDesktopFiles === 'function')
    desktopFiles = new UrDesktopFiles(context, send, bootstrapToken,
      () => release('文件媒体驱动失败，已释放设备资源。'));
  if (typeof globalThis.UrLiveHost === 'function') {
    if (desktopFiles) throw new Error('unsupported_live_assembly');
    const output = document.createElement('section');
    output.id = 'liveOutput'; output.setAttribute('aria-label', '实时节目画面');
    document.body.append(output);
    desktopLive = new UrLiveHost(send, output);
    if (!await desktopLive.activate()) throw new Error('unsupported_live_driver');
    if (terminated) { await desktopLive.close(); return; }
  }
}
let responseId = ''; // 仅当前Peer播放目标；退休时清空，不作为模型回复控制授权。
let controlResponseId = ''; // 已配对C++状态投影的模型回复控制目标，独立于Peer播放/增益。
let controlProjectionAvailable = false; // 原宿主一旦提供核心投影，就不得在退休后退回旧播放目标。
let playbackEpoch = ''; // 当前浏览器播放代次，不与会话ID混用。
let currentReplyConsumed = 0; // 本回复Worklet已实际消费的帧，网络到达不能替代。
let currentReplyConsumedNonSilent = 0; // 本回复Worklet实际消费的有声帧，不跨回复累计。
let currentReplyConsecutiveConsumedNonSilent = 0; // Worklet消费顺序中的连续有声帧，静音会切断首音判定。
let currentReplyConsecutiveNonSilent = 0; // 当前回复连续非静音解码帧，不能跨取消或下一轮累计。
let currentReplyPlayable = false; // 本回复是否达到连续两帧非静音的设备侧可播放门槛。
let completedResponseId = ''; // 最近完成内容的身份，用于区别插话前后两轮。
let consumedResponseId = ''; // 最近真正由Worklet消费的回复，不从网络终态推断。
let pendingPlayback = new Map(); // 当前代次已送音频线程、尚未确认消费的票据及其有声事实。
let terminated = false; // 清理只执行一次，不用重连掩盖失败。
const nonSilentFramePower = 128 * 128; // 与C++ UdpAudio 的PCM16 RMS阈值保持一致。
const metrics = {captured: 0, captureOpen: null, decodedNonSilent: 0, playableResponses: 0,
  consumed: 0, consumedNonSilent: 0, lastVoicedConsumedResponseId: '',
  lastVoicedConsumedResponseFrames: 0, lastPlayableResponseId: '',
  lastPlayableConsumedResponseId: '', completedResponses: 0,
  successfulActions: 0, lastActualVolume: null, interactionMode: null,
  uplinkWhilePlaying: 0, cancelledResponses: 0, interruptedPlaybacks: 0,
  textInputResult: null, lastAudioState: null, lastNetworkPackets: 0,
  lastDecodedFrames: 0, lastNonSilentFrames: 0, lastMissingPackets: 0,
  lastObservedComplete: false, rtcSampleWindows: 0, rtcPeakPower: 0,
  lastMediaFinalResponseId: '', focusReceiptIndex: 0, focusReceipts: [],
  clientToolReceiptIndex: 0, clientToolReceipts: [], humanControlReceiptIndex: 0,
  humanControlReceipts: [], lastReplyTiming: null}; // 有界原收件ACK只保留内存，不保存工具参数/结果正文。

// forwardInputOwnershipActual仅在原Gate首次接纳真实线程票据后投影同scope停止意图；指标仍等待线程停采事实。
function forwardInputOwnershipActual(value) {
  if (value.kind === 'inputOwnershipGated' && value.enabled === false && captureCommand &&
      captureCommand.sessionId === value.sessionId && captureCommand.routeGeneration === value.routeGeneration)
    captureCommand = {...captureCommand, enabled: false};
  return send(value);
}

// recordCaptureState 只观察音频线程已应用的当前原命令，不发送协议或据此自动重启采集。
function recordCaptureState(value) {
  const scoped = inputOwnership && Object.keys(value).length === 5;
  if (terminated || (!scoped && Object.keys(value).length !== 3) ||
      value.kind !== 'captureState' || typeof value.id !== 'string' ||
      value.id.length > 128 || typeof value.enabled !== 'boolean')
    throw new Error('invalid_capture_fact');
  // 空标识只用于音频线程有效初始化的零输入事实，绝不确认有原输入的停止命令。
  if (!captureCommand) {
    if (value.id !== '' || value.enabled) throw new Error('invalid_capture_fact');
    metrics.captureOpen = false;
    return;
  }
  if (!value.id) return;
  if (value.id !== captureCommand.id || value.enabled !== captureCommand.enabled) return;
  if (scoped && (value.sessionId !== captureCommand.sessionId ||
      value.routeGeneration !== captureCommand.routeGeneration)) return;
  metrics.captureOpen = value.enabled;
}

// captureAckMain 仅原故障开关下计数，关闭时没有额外消息或计时。
const captureAckMain = observeAudioFailure ? {received: 0, filterAccepted: 0, filterRejected: 0,
  sendAccepted: 0, sendRejected: 0, sendException: 0, ackPosted: 0, ackRejected: 0,
  ackException: 0, socketUnavailable: 0, socketBackpressure: 0, overflow: false} : null;
let captureAckThread = null; // 只保存一次原线程失败快照，不保存关联或PCM。
let captureAckThreadRejected = false;

// noteCaptureAckMain 不求错误正文且只接受固定键；异常不能改变原send或release。
function noteCaptureAckMain(field) {
  if (!captureAckMain) return;
  try {
    if (!Object.hasOwn(captureAckMain, field) || typeof captureAckMain[field] !== 'number') return;
    if (captureAckMain[field] >= 1000000) captureAckMain.overflow = true;
    else ++captureAckMain[field];
  } catch { try { captureAckMain.overflow = true; } catch { /* 旁证异常不改变原路径。 */ } }
}

// captureAckSocketFacts 旁观原发送前状态；不改变原连接、容量或发送次数。
function captureAckSocketFacts() {
  if (!captureAckMain) return;
  try {
    if (!socket || socket.readyState !== WebSocket.OPEN) noteCaptureAckMain('socketUnavailable');
    if (socket && socket.bufferedAmount > 65536) noteCaptureAckMain('socketBackpressure');
  } catch { try { captureAckMain.overflow = true; } catch { /* 旁证异常不改变原路径。 */ } }
}

// recordCaptureAckThread 只接原端口严格数据属性闭集；未知键或溢出不能作为完整旁证。
function recordCaptureAckThread(value) {
  if (!captureAckMain || captureAckThread || captureAckThreadRejected) return;
  try {
    const fields = Object.getOwnPropertyDescriptors(value);
    const names = ['kind', 'generated', 'applied', 'oldInputRejected', 'ownerRejected',
      'highWater', 'pending', 'overflow', 'clock', 'maximumAgeMilliseconds', 'oldestPendingAgeMilliseconds'];
    if (Reflect.ownKeys(fields).length !== names.length || names.some(k => !Object.hasOwn(fields[k] || {}, 'value')))
      throw Error('invalid_capture_ack_diagnostic');
    const v = Object.fromEntries(names.map(k => [k, fields[k].value]));
    if (v.kind !== 'captureAckDiagnostic' || !['valid', 'unavailable', 'invalid'].includes(v.clock) ||
        typeof v.overflow !== 'boolean' ||
        ['generated', 'applied', 'oldInputRejected', 'ownerRejected'].some(k => !Number.isInteger(v[k]) || v[k] < 0 || v[k] > 1000000) ||
        ['highWater', 'pending'].some(k => !Number.isInteger(v[k]) || v[k] < 0 || v[k] > 16) ||
        (v.maximumAgeMilliseconds !== null && (!Number.isFinite(v.maximumAgeMilliseconds) || v.maximumAgeMilliseconds < 0)) ||
        (v.oldestPendingAgeMilliseconds !== null && (!Number.isFinite(v.oldestPendingAgeMilliseconds) || v.oldestPendingAgeMilliseconds < 0)) ||
        (v.pending === 0 && v.oldestPendingAgeMilliseconds !== null) ||
        (v.clock !== 'valid' && (v.maximumAgeMilliseconds !== null || v.oldestPendingAgeMilliseconds !== null))) throw Error('invalid_capture_ack_diagnostic');
    captureAckThread = v;
  } catch { captureAckThreadRejected = true; }
}

// readCaptureAckDiagnostic 返回当前有界副本；线程缺事实明确为null，不触发请求或另一个消费者。
function readCaptureAckDiagnostic() {
  if (!captureAckMain) return null;
  return {main: {...captureAckMain}, thread: captureAckThread ? {...captureAckThread} : null,
    threadRejected: captureAckThreadRejected};
}
globalThis.UrReadCaptureAckDiagnostic = readCaptureAckDiagnostic;

// receiveAudioThread保持单个原onmessage；输入票据沿专用端口，播放事实继续原验证与计数。
function receiveAudioThread(value) {
  if (observeAudioFailure && value?.kind === 'audioFailureDiagnostic') {
    recordAudioFailureDiagnostic(value); // 独立旁证不转发协议，也不能触发/取消原关闭策略。
  } else if (observeAudioFailure && value?.kind === 'captureAckDiagnostic') {
    recordCaptureAckThread(value);
  } else if (inputOwnership && ['inputOwnershipIdle', 'inputOwnershipGated'].includes(value?.kind)) {
    inputOwnership.handleActual(value); // 旧票据由原端口隔离，不能关闭已经取得输入的另一腿。
  } else if (value?.kind === 'captureState') {
    try { recordCaptureState(value); }
    catch { release('浏览器采集实态无效，已释放媒体。'); }
  } else if (value?.kind === 'captured') {
    noteCaptureAckMain('received');
    if (inputOwnership && !inputOwnership.forwardCapture(value)) { noteCaptureAckMain('filterRejected'); return; }
    noteCaptureAckMain('filterAccepted');
    captureAckSocketFacts();
    let captureSent;
    try { captureSent = send(value); }
    catch (error) { noteCaptureAckMain('sendException'); throw error; }
    noteCaptureAckMain(captureSent ? 'sendAccepted' : 'sendRejected');
    if (captureSent) {
      ++metrics.captured;
      if (responseId && currentReplyConsumed > 0) ++metrics.uplinkWhilePlaying;
      showMetrics();
      const ack = {kind: 'captureAck', id: value.id};
      if (inputOwnership) {
        // 仅复制这条原帧的关联；不得给积压帧贴当前会话或Human所有权。
        try {
          const accepted = inputOwnership.forwardControl({...ack, sessionId: value.sessionId,
            routeGeneration: value.routeGeneration});
          noteCaptureAckMain(accepted ? 'ackPosted' : 'ackRejected');
        } catch (error) { noteCaptureAckMain('ackException'); throw error; }
      } else {
        try { worklet.port.postMessage(ack); noteCaptureAckMain('ackPosted'); }
        catch (error) { noteCaptureAckMain('ackException'); throw error; }
      }
    }
  } else if (value?.kind === 'played') {
    if (typeof value.nonSilent !== 'boolean') return release('浏览器音频消费事实无效，已释放媒体。');
    if (send({kind: 'played', epoch: value.epoch, ticket: value.ticket})) recordPlaybackConsumption(value);
  } else if (value?.kind === 'playbackGated') {
    send({kind: 'outputGated', ticket: value.ticket, enabled: value.enabled});
  } else release('浏览器音频驱动失败，已释放麦克风。');
}

// recordAudioFailureDiagnostic只接原端口严格两字段literal；访问器/额外字段不得读取或污染首因。
function recordAudioFailureDiagnostic(value) {
  if (!observeAudioFailure || audioFailureCause !== null) return;
  try {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return;
    const fields = Object.getOwnPropertyDescriptors(value);
    if (Reflect.ownKeys(fields).length !== 2 || !Object.hasOwn(fields.kind || {}, 'value') ||
        !Object.hasOwn(fields.cause || {}, 'value') || fields.kind.value !== 'audioFailureDiagnostic' ||
        !audioFailureCauses.has(fields.cause.value)) return;
    audioFailureCause = fields.cause.value;
  } catch { /* 旁证不可用保持unknown，不能覆盖原audioFailed或造成额外release。 */ }
}

// readAudioFailureDiagnostic仅返回已锁存固定事实副本；unknown表示未取得可分类首因，不证明没有故障。
function readAudioFailureDiagnostic() {
  return observeAudioFailure ? {known: audioFailureCause !== null && audioFailureCause !== 'unknown',
    cause: audioFailureCause ?? 'unknown'} : null;
}
globalThis.UrReadAudioFailureDiagnostic = readAudioFailureDiagnostic; // 私有观察器可在release后读取，不申请源或消费端口。

// recordFocusReceipt 只保留原操作回执的32条观察，历史revision不更新当前授权或触发播放。
function recordFocusReceipt(value) {
  const fields = ['kind', 'state', 'revision', 'operation', 'code', 'receiptIndex'];
  const states = {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'};
  if (Object.keys(value).length !== fields.length ||
      Object.keys(value).some(key => !fields.includes(key)) ||
      typeof value.operation !== 'string' || typeof value.state !== 'string' ||
      !Object.hasOwn(states, value.operation) ||
      !Number.isSafeInteger(value.revision) || value.revision < 0 ||
      !Number.isSafeInteger(value.receiptIndex) || value.receiptIndex <= metrics.focusReceiptIndex ||
      !Number.isSafeInteger(value.code) ||
      !((value.code === 200 && value.state === states[value.operation]) ||
        (value.code >= 400 && value.code <= 599 && value.state === 'denied')))
    throw new Error('invalid_focus_receipt');
  metrics.focusReceipts.push({state: value.state, revision: value.revision,
    operation: value.operation, code: value.code, receiptIndex: value.receiptIndex});
  if (metrics.focusReceipts.length > 32) metrics.focusReceipts.shift();
  metrics.focusReceiptIndex = value.receiptIndex;
}

// validMediaCounts保证云端终态快照与本地停播最终计数使用同一有界口径。
function validMediaCounts(value) {
  return Number.isSafeInteger(value.networkPackets) && value.networkPackets >= 0 &&
    Number.isSafeInteger(value.decodedFrames) && value.decodedFrames >= 0 &&
    Number.isSafeInteger(value.nonSilentFrames) && value.nonSilentFrames >= 0 &&
    value.nonSilentFrames <= value.decodedFrames && value.decodedFrames <= value.networkPackets;
}
// validFinalMedia只接受设备停播时冻结的完整性事实；终态快照不能伪造该结论。
function validFinalMedia(value) {
  return validMediaCounts(value) && Number.isSafeInteger(value.missingPackets) &&
    value.missingPackets >= 0 && typeof value.observedComplete === 'boolean' &&
    (!value.observedComplete || (value.missingPackets === 0 &&
      value.decodedFrames === value.networkPackets));
}

// showMetrics区分RTC分析器观察窗口和UDP原音频线程消费帧，两者均不称作硬件播放。
function showMetrics() {
  element('metrics').textContent = useWebRTC
    ? `WebRTC采样 ${metrics.rtcSampleWindows} 窗口，解码非静音 ${metrics.decodedNonSilent} 窗口，门后有声观察 ${metrics.consumedNonSilent} 窗口`
    : `采集 ${metrics.captured} 帧，解码非静音 ${metrics.decodedNonSilent} 帧，音频线程消费 ${metrics.consumed} 帧（有声 ${metrics.consumedNonSilent} 帧）`;
}

// recordPlaybackConsumption只在当前代次、已交付票据的Worklet确认后记数；迟到/重复回调不伪造有声消费。
function recordPlaybackConsumption(value) {
  if (!responseId || value.epoch !== playbackEpoch ||
      value.ticket !== pendingPlayback.keys().next().value) return;
  const nonSilent = pendingPlayback.get(value.ticket);
  pendingPlayback.delete(value.ticket);
  ++metrics.consumed;
  ++currentReplyConsumed;
  consumedResponseId = responseId;
  if (nonSilent && value.nonSilent === true) {
    ++metrics.consumedNonSilent;
    ++currentReplyConsumedNonSilent;
    ++currentReplyConsecutiveConsumedNonSilent;
    metrics.lastVoicedConsumedResponseId = responseId;
    metrics.lastVoicedConsumedResponseFrames = currentReplyConsumedNonSilent;
    if (currentReplyConsecutiveConsumedNonSilent >= 2)
      metrics.lastPlayableConsumedResponseId = responseId;
  } else currentReplyConsecutiveConsumedNonSilent = 0;
  showMetrics();
}

// status使用textContent避免把模型文本或设备消息当HTML执行。
function status(text) { element('status').textContent = text; }
// release释放真实浏览器资源，不能只关闭界面按钮而保留麦克风。
function release(reason) {
  if (terminated) return;
  terminated = true;
  controlResponseId = ''; // 终态后旧本地投影不能恢复控制目标。
  handoffState = undefined;
  humanControlState = undefined;
  metrics.humanControlReceipts = []; // 原控制token不在关闭后的页面继续留存。
  element('handoffHold').disabled = element('handoffResume').disabled = true;
  for (const [id] of Object.values(handoffButtons)) element(id).disabled = true;
  captureCommand = null;
  metrics.captureOpen = null; // 关闭意图不伪造音频线程已确认的停止状态。
  clientTheme?.close(); // 先退休原渲染票据；页面关闭后迟到命令不能产生新副作用。
  void inputOwnership?.close(); // 原Sender迟到取得仍由适配精确补偿；关闭意图不伪造实际释放。
  duplexHumanMedia?.close(); // 只关闭原人工Peer／输出图，共享输入源仍由本宿主随后释放。
  desktopFiles?.close();
  void desktopLive?.close(); // 实际退出由宿主自身收口，不把关页面等同节目Released或focusRelease。
  pendingPlayback.clear();
  for (const id of ['activate', 'start', 'stop', 'cancel', 'end', 'textInput', 'textAudio', 'sendText']) element(id).disabled = true;
  if (stream) for (const track of stream.getTracks()) track.stop();
  if (rtcTimer) clearInterval(rtcTimer);
  if (rtcSink) { rtcSink.pause(); rtcSink.srcObject = null; rtcSink = null; }
  if (rtcPeer) rtcPeer.close();
  if (rtcOutputNode) {
    rtcOutputNode.port.postMessage({kind: 'callAudioClose'});
    rtcOutputNode.port.close(); rtcOutputNode.disconnect(); rtcOutputNode = null;
  }
  rtcPresentedAnalyser?.disconnect?.(); rtcPresentedAnalyser = null;
  if (source) source.disconnect();
  endpointOwnershipSource?.disconnect(); endpointOwnershipSource = null;
  if (worklet) { worklet.port.close(); worklet.disconnect(); }
  if (context) void context.close();
  if (socket) socket.close();
  status(reason);
}
// rtcObserve采样已经通过浏览器解码的音频；尾部需在完成事件后持续静音才通知C++。
function rtcObserve() {
  if (terminated || !useWebRTC || !responseId) return;
  if (!rtcAnalyser) {
    if (rtcFinishAt && performance.now() - rtcFinishAt >= 1000) {
      const epoch = playbackEpoch;
      rtcFinishAt = 0;
      send({kind: 'webrtcDrained', peerRevision: rtcPeerRevision.toString(), epoch});
    }
    return;
  }
  const samples = new Float32Array(rtcAnalyser.fftSize);
  rtcAnalyser.getFloatTimeDomainData(samples);
  const power = samples.reduce((sum, sample) => sum + sample * sample, 0) / samples.length;
  ++metrics.rtcSampleWindows;
  metrics.rtcPeakPower = Math.max(metrics.rtcPeakPower, power);
  const decodedVoiced = power >= (128 / 32768) ** 2;
  let voiced = decodedVoiced;
  if (rtcOutputFocus) {
    voiced = false;
    if (rtcGateKnown && rtcGateEnabled && rtcPresentedAnalyser) {
      const actual = new Float32Array(rtcPresentedAnalyser.fftSize);
      rtcPresentedAnalyser.getFloatTimeDomainData(actual);
      voiced = actual.reduce((sum, sample) => sum + sample * sample, 0) / actual.length >= (128 / 32768) ** 2;
    }
  }
  // 解码尾部独立于呈现门控；关闭输出不能借门前有声计为实际可播放。
  if (decodedVoiced) {
    ++metrics.decodedNonSilent; // 门前解码有声采样窗口，不受输出许可影响，也不冒充实际音频帧。
    rtcLastVoicedAt = performance.now(); rtcSilence = 0;
  }
  else if (rtcFinishAt) ++rtcSilence;
  ++metrics.consumed;
  ++currentReplyConsumed;
  if (voiced) {
    ++metrics.consumedNonSilent;
    ++currentReplyConsecutiveConsumedNonSilent;
    metrics.lastVoicedConsumedResponseId = responseId;
    if (currentReplyConsecutiveConsumedNonSilent >= 2) {
      if (!currentReplyPlayable) {
        currentReplyPlayable = true;
        ++metrics.playableResponses;
      }
      metrics.lastPlayableResponseId = responseId;
      metrics.lastPlayableConsumedResponseId = responseId;
    }
  } else {
    currentReplyConsecutiveConsumedNonSilent = 0;
  }
  if (rtcFinishAt && performance.now() - rtcFinishAt >= 500 &&
      performance.now() - rtcLastVoicedAt >= 200 && rtcSilence >= 10) {
    const epoch = playbackEpoch;
    rtcFinishAt = 0;
    rtcSilence = 0;
    send({kind: 'webrtcDrained', peerRevision: rtcPeerRevision.toString(), epoch});
  }
  showMetrics();
}
// rtcAttachRemote只把已协商音轨接入受控播放图；静音媒体元素激活浏览器解码但不直接发声。
function rtcAttachRemote(event, originalPeer = rtcPeer) {
  // 只恢复本原Peer当前回复意图；取消后为空、原Peer退休后的track不能重建播放图。
  if (terminated || rtcDisconnected || !originalPeer || rtcPeer !== originalPeer ||
      event.track.kind !== 'audio' || rtcAnalyser) return;
  // Chromium在仅连接WebAudio图时可能收RTP却不拉取远端解码；静音media元素负责激活解码，
  // 可听播放仍仅经过下方增益节点，停播/取消不会泄漏第二条不受控音路。
  rtcSink = document.createElement('audio');
  rtcSink.muted = true;
  rtcSink.autoplay = true;
  rtcSink.playsInline = true;
  rtcSink.srcObject = new MediaStream([event.track]);
  const sink = rtcSink;
  void sink.play().catch(() => {
    if (rtcSink === sink && !terminated) release('WebRTC远端音轨无法启动，已释放设备资源。');
  });
  source = context.createMediaStreamSource(new MediaStream([event.track]));
  rtcAnalyser = context.createAnalyser();
  rtcAnalyser.fftSize = 1024;
  rtcGain = context.createGain();
  rtcGain.gain.value = responseId ? rtcVolume / 100 : 0;
  source.connect(rtcAnalyser);
  rtcAnalyser.connect(rtcGain);
  rtcGain.connect(rtcOutputFocus ? rtcOutputNode : context.destination);
}
// rtcConfigure由C++会话创建触发，候选收集完成后才把SDP交回设备协议核心。
async function rtcConfigure() {
  if (!rtcPeer || rtcPeer.localDescription) throw new Error('rtc_already_configured');
  const original = rtcPeer;
  const offer = await original.createOffer();
  if (terminated || rtcPeer !== original) return;
  await original.setLocalDescription(offer);
  if (terminated || rtcPeer !== original) return;
  if (original.iceGatheringState !== 'complete') {
    await new Promise((resolve, reject) => {
      let settled = false;
      const finish = error => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        original.removeEventListener('icegatheringstatechange', check);
        if (rtcCancelGather === cancel) rtcCancelGather = null;
        if (error) reject(error); else resolve();
      };
      const cancel = () => finish();
      const timer = setTimeout(() => {
        finish(terminated || rtcPeer !== original ? null : new Error('ice_timeout'));
      }, 8000);
      const check = () => {
        if (!terminated && rtcPeer === original && original.iceGatheringState !== 'complete') return;
        finish();
      };
      rtcCancelGather = cancel;
      original.addEventListener('icegatheringstatechange', check);
      check();
    });
  }
  if (!terminated && rtcPeer === original && !rtcDisconnected)
    send({kind: 'webrtcOffer', peerRevision: rtcPeerRevision.toString(), sdp: original.localDescription.sdp});
}
// rtcIncoming只接收C++已关联会话后的固定媒体命令，不接受网页直发云端信令。
function rtcIncoming(value) {
  if (value.kind === 'playbackGate') { rtcOutputGate(value); return; }
  // 音量属于同物理speaker原请求；其它网络腿命令必须带正整数字符串代次。
  if (value.kind !== 'webrtcVolume') {
    if (typeof value.peerRevision !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value.peerRevision) ||
        BigInt(value.peerRevision) > 18446744073709551615n) throw new Error('invalid_peer_revision');
    const revision = BigInt(value.peerRevision);
    if (revision < rtcPeerRevision) return; // 已消费旧命令，不能关闭或开启后继Peer。
    if (value.kind === 'webrtcRecover') {
      if (rtcVisionConfiguration || Object.keys(value).length !== 2 || !rtcPeer || !rtcDisconnected ||
          revision !== rtcPeerRevision + 1n) throw new Error('invalid_peer_recovery');
      rtcStopVoice();
      rtcCreatePeer(revision);
      const peer = rtcPeer;
      void rtcConfigure().catch(() => {
        if (!terminated && rtcPeer === peer) release('WebRTC恢复候选协商失败，已释放设备媒体。');
      });
      return;
    }
    if (revision !== rtcPeerRevision) throw new Error('invalid_peer_revision');
  }
  if (value.kind === 'webrtcVisionControl' || value.kind === 'webrtcVisionCancel') {
    rtcVisionIncoming(value); return;
  }
  if (value.kind === 'webrtcConfigureVision') {
    const selected = rtcVisionConfigurationFor(value); // 完整预检成功以前不退休原音频Peer。
    rtcStopVoice();
    rtcVisionConfiguration = selected;
    rtcCreatePeer(BigInt(value.peerRevision), selected);
    const original = rtcPeer;
    void rtcConfigure().catch(() => {
      if (!terminated && rtcPeer === original) release('WebRTC视觉候选协商失败，已释放设备媒体。');
    });
    return; // 这里只安装paused三轨；没有trackState/publish或ready事实。
  }
  if (value.kind === 'webrtcStop') {
    if (Object.keys(value).length !== 2) throw new Error('rtc_command_fields');
    rtcStopVoice(); return;
  }
  if (!rtcPeer) throw new Error('rtc_not_ready');
  if (value.kind === 'webrtcConfigure') {
    if (rtcVisionConfiguration || Object.keys(value).length !== 2 || rtcDisconnected) throw new Error('rtc_command_fields');
    const original = rtcPeer;
    void rtcConfigure().catch(() => {
      if (!terminated && rtcPeer === original) release('WebRTC候选协商失败，已释放设备媒体。');
    });
  } else if (value.kind === 'webrtcAnswer') {
    if (Object.keys(value).length !== 3 || rtcDisconnected || typeof value.sdp !== 'string' || value.sdp.length > 49152 ||
        !value.sdp.startsWith('v=0\r\n')) throw new Error('invalid_answer');
    const original = rtcPeer;
    void original.setRemoteDescription({type: 'answer', sdp: value.sdp})
      .catch(() => {
        if (!terminated && rtcPeer === original) release('WebRTC应答失败，已释放设备媒体。');
      });
  } else if (value.kind === 'webrtcVolume') {
    if (typeof value.id !== 'string' || !value.id || value.id.length > 128 ||
        Object.keys(value).length !== 3 || !Number.isInteger(value.volume) ||
        value.volume < 0 || value.volume > 100)
      throw new Error('invalid_volume');
    rtcVolume = value.volume;
    if (rtcGain && responseId) rtcGain.gain.value = rtcVolume / 100;
    if (!send({kind: 'webrtcVolumeApplied', id: value.id, volume: rtcVolume}))
      throw new Error('volume_ack_failed');
  } else if (value.kind === 'captureStart' || value.kind === 'captureStop') {
    const candidate = globalThis.UrAIEndpointListen === true;
    if (Object.keys(value).length !== (candidate ? 5 : 3) || rtcDisconnected || typeof value.id !== 'string' || !value.id || value.id.length > 128)
      throw new Error('invalid_capture');
    if (candidate) {
      if (!inputOwnership) throw new Error('invalid_capture_owner');
      inputOwnership.forwardSenderControl(value); // 撤权在途的旧启停意图丢弃，不能越过实际所有权屏障。
      return; // 原完整组与新本机输入命令缺一不能开Sender，不重走默认直接enabled路径。
    }
    const sender = rtcPeer.getSenders().find(item => item.track?.kind === 'audio');
    if (!sender?.track) throw new Error('missing_audio_sender');
    sender.track.enabled = value.kind === 'captureStart';
  } else if (value.kind === 'playbackStart' || value.kind === 'playbackStop') {
    if (Object.keys(value).length !== 4 || rtcDisconnected || typeof value.id !== 'string' || !value.id ||
        typeof value.epoch !== 'string' || !/^[1-9][0-9]*$/.test(value.epoch))
      throw new Error('invalid_playback');
    if (value.kind === 'playbackStart') {
      responseId = value.id;
      playbackEpoch = value.epoch;
      rtcFinishAt = rtcSilence = 0;
      currentReplyConsecutiveConsumedNonSilent = 0;
      currentReplyPlayable = false;
      metrics.lastPlayableResponseId = metrics.lastPlayableConsumedResponseId = '';
      if (rtcGain) rtcGain.gain.value = rtcVolume / 100;
    } else if (responseId === value.id && playbackEpoch === value.epoch) {
      if (rtcGain) rtcGain.gain.value = 0;
      responseId = playbackEpoch = '';
      rtcFinishAt = rtcSilence = 0;
    }
  } else if (value.kind === 'webrtcFinish') {
    if (Object.keys(value).length !== 3 || rtcDisconnected || !responseId || value.epoch !== playbackEpoch || rtcFinishAt)
      throw new Error('invalid_finish');
    rtcFinishAt = performance.now();
    rtcSilence = 0;
  } else throw new Error('unknown_rtc_command');
}
// send限制每个消息及发送积压；失败立即停止音频，不无限缓存或忽略控制丢失。
function send(value) {
  const body = JSON.stringify(value);
  if (terminated || !socket || socket.readyState !== WebSocket.OPEN ||
      body.length > 65536 || socket.bufferedAmount > 65536) {
    release('本地连接不可用或发送积压；请重新启动设备进程。');
    return false;
  }
  socket.send(body);
  return true;
}
// sendHumanStopDiagnostic只发送本媒体已锁存的六字段旁证；预留原Stopped空间，失败不release/重试/改消息队列。
function sendHumanStopDiagnostic(value) {
  try {
    // 原release先置terminated再关闭本媒体；只允许仍open的原scope旁证，不改变普通send的终止检查。
    if (duplexHumanMedia?.observeStop !== true || !socket ||
        socket.readyState !== WebSocket.OPEN || !value || typeof value !== 'object' ||
        Array.isArray(value)) return false;
    const fields = Object.getOwnPropertyDescriptors(value); // 旁证不执行getter或反射自由正文。
    const keys = ['kind', 'sessionId', 'routeGeneration', 'peerEpoch', 'known', 'cause'];
    if (Reflect.ownKeys(fields).length !== keys.length ||
        !keys.every(key => Object.hasOwn(fields[key] || {}, 'value'))) return false;
    const fact = Object.fromEntries(keys.map(key => [key, fields[key].value]));
    const bound = duplexHumanMedia.scope; // 只关联本次原绑定，不能从新的页面输入推断scope。
    const causes = ['processorError', 'startOutputUnavailable', 'startException', 'answerException',
      'remoteTrackInvalid', 'remoteTrackSetupFailed', 'remoteSinkRejected', 'peerFailed',
      'peerDisconnected', 'explicitClose'];
    if (fact.kind !== 'humanVoiceStopDiagnostic' || !bound ||
        ['sessionId', 'routeGeneration', 'peerEpoch'].some(key =>
          typeof fact[key] !== 'string' || fact[key] !== bound[key]) ||
        typeof fact.known !== 'boolean' ||
        (fact.known ? !causes.includes(fact.cause) : fact.cause !== 'unknown')) return false;
    const body = JSON.stringify(fact), stopped = JSON.stringify({kind: 'humanVoiceStopped',
      sessionId: fact.sessionId, routeGeneration: fact.routeGeneration, peerEpoch: fact.peerEpoch});
    const bytes = new TextEncoder().encode(body).byteLength; // WebSocket积压按UTF8字节计，包含可转义原会话标识。
    const reserve = bytes + new TextEncoder().encode(stopped).byteLength;
    if (bytes > 1024 || !Number.isFinite(socket.bufferedAmount) || socket.bufferedAmount < 0 ||
        socket.bufferedAmount > 65536 - reserve) return false;
    socket.send(body);
    return true;
  } catch { return false; } // 断链/发送异常仅缺旁证；原业务send的错误策略不改变。
}
// validOptionalResponseId复核可选C++内容回复ID；缺省兼容旧消息，非空按128 UTF8字节/无控制字符边界验证。
function validOptionalResponseId(value) {
  if (!Object.hasOwn(value, 'responseId')) return true;
  const id = value.responseId; // 只校验本消息原标识，不更新播放ID或从当前回复推断关联。
  return typeof id === 'string' && id.length > 0 &&
    !/[\u0000-\u001f\u007f\ud800-\udfff]/u.test(id) &&
    typeof TextEncoder === 'function' && new TextEncoder().encode(id).byteLength <= 128;
}
// incoming只转交驱动消息或展示C++事件；未知消息明确终止，避免界面与设备状态分歧。
function incoming(value) {
  if (!value || typeof value.kind !== 'string') throw new Error('invalid_message');
  if (value.kind === 'temporaryCallState') {
    const fields = ['kind', 'canResumeAI', 'restoring'];
    if (!temporaryCalls || terminated || Object.keys(value).length !== fields.length ||
        Object.keys(value).some(key => !fields.includes(key)) ||
        typeof value.canResumeAI !== 'boolean' || typeof value.restoring !== 'boolean' ||
        (value.canResumeAI && value.restoring)) throw new Error('invalid_temporary_call_state');
    temporaryResumeAllowed = value.canResumeAI;
    element('temporaryResumeAI').disabled = !temporaryResumeAllowed;
    element('temporaryStatus').textContent = value.restoring ? '等待原AI资源实际恢复与新输入确认。' :
      value.canResumeAI ? '通话资源已释放，请手动恢复AI。' : '临时通话仍未满足AI恢复条件。';
    return;
  }
  if (value.kind === 'endpointSourceState') {
    const fields = ['kind', 'state', 'canPrepare', 'canCommit', 'canAbort', 'pending', 'unknown'];
    if (terminated || (globalThis.UrAIEndpointSource !== true && globalThis.UrHumanEndpointSource !== true) || !value ||
        Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
        !['idle', 'pending', 'preparing', 'ready', 'committed', 'aborted', 'failed', 'unknown', 'rejected'].includes(value.state) ||
        fields.slice(2).some(key => typeof value[key] !== 'boolean') ||
        ((value.pending || value.unknown) && (value.canPrepare || value.canCommit || value.canAbort)) ||
        (value.canCommit && value.state !== 'ready')) throw new Error('invalid_endpoint_source_state');
    aiEndpointSourceState = {...value};
    const labels = {idle: '原AI会话已建立', pending: '等待平台受理', preparing: '等待目标端本人接听和媒体就绪',
      ready: '目标端已就绪，可确认移交', committed: '平台已提交移交，实际停流仍以原设备事实为准',
      aborted: '本次接续已撤销', failed: '本次接续失败', unknown: '接续结果待确认，不重复发起', rejected: '平台拒绝本次请求'};
    element('endpointSourceStatus').textContent = globalThis.UrHumanEndpointSource === true && value.state === 'idle' ?
      '等待原人与人通话及实际资源允许接续' : labels[value.state];
    for (const [id, flag] of [['endpointSourcePrepare', 'canPrepare'], ['endpointSourceCommit', 'canCommit'],
      ['endpointSourceAbort', 'canAbort']]) element(id).disabled = !value[flag];
    return;
  }
  if (value.kind === 'endpointState') {
    const fields = ['kind', 'phase', 'canAccept', 'canReject'];
    if (terminated || globalThis.UrAIEndpointListen !== true ||
        Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
        !Number.isInteger(value.phase) || value.phase < 0 || value.phase > 9 ||
        typeof value.canAccept !== 'boolean' || typeof value.canReject !== 'boolean' ||
        value.canAccept !== (value.phase === 2) || value.canReject !== (value.phase === 2))
      throw new Error('invalid_endpoint_state');
    aiEndpointState = {...value}; // 阶段只影响本人按钮，采集仍由原native完整组与新输入控制。
    element('endpointAccept').disabled = !value.canAccept;
    element('endpointReject').disabled = !value.canReject;
    const labels = ['未启用', '等待AI接续邀请', '请本人接听或拒绝', '等待接听确认', '准备候选媒体',
      '等待平台双端提交', '等待完整音频资源', 'AI接续会话已建立', '正在停止并释放', '接续失败'];
    element('endpointStatus').textContent = labels[value.phase];
    return;
  }
  if (value.kind === 'controlRecovery') {
    const fields = ['kind', 'state', 'endpointEpoch', 'messageWatermark', 'originalSessionPreserved'];
    if (terminated || globalThis.UrAIControlRecovery !== true ||
        Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
        !['disconnected', 'connecting', 'synchronizing', 'ready', 'inactive'].includes(value.state) ||
        !Number.isSafeInteger(value.endpointEpoch) || value.endpointEpoch < 1 ||
        !Number.isSafeInteger(value.messageWatermark) || value.messageWatermark < 0 ||
        typeof value.originalSessionPreserved !== 'boolean' ||
        (aiControlState && (value.endpointEpoch < aiControlState.endpointEpoch ||
          value.messageWatermark < aiControlState.messageWatermark)))
      throw new Error('invalid_control_recovery');
    aiControlState = {...value}; // 真实状态另由原HAL执行；不能从ready事实推断麦克风已恢复。
    return;
  }
  if (value.kind === 'inputOwnershipBind') {
    const fields = ['kind', 'sessionId', 'routeGeneration', 'routeKind'];
    if (!inputOwnership || terminated || Object.keys(value).length !== fields.length ||
        Object.keys(value).some(key => !fields.includes(key)) ||
        !['udp', 'humanSender', 'aiSender'].includes(value.routeKind) ||
        (value.routeKind === 'aiSender' && (globalThis.UrAIEndpointListen !== true || !useWebRTC ||
          !rtcPeer || !rtcWasConnected || rtcDisconnected))) throw new Error('invalid_input_binding');
    const binding = {sessionId: value.sessionId, routeGeneration: value.routeGeneration};
    const media = value.routeKind === 'humanSender' ?
      duplexHumanMedia?.inputBinding(binding.sessionId, binding.routeGeneration) :
      (value.routeKind === 'aiSender' ? {peer: rtcPeer, sender: rtcEndpointSender} : undefined);
    if (!inputOwnership.bindRoute(binding, value.routeKind, media)) throw new Error('invalid_input_binding');
    return; // 本地关联不是资源许可，不能发取得ACK或开始采集。
  }
  if (value.kind === 'inputOwnershipGate') {
    if (!inputOwnership || terminated || !inputOwnership.request(value)) throw new Error('invalid_input_gate');
    return; // 真实完成只来自原Worklet/Sender，受理不发成功事实。
  }
  // 只读宿主诊断不进入任何业务状态/驱动/展示；畸形内容也隔离，固定投影由可选observer校验。
  if (value.kind === 'messageDiagnostic') return;
  if (value.kind === 'clientThemeSet') {
    if (terminated || !clientTheme?.handle(value)) throw new Error('invalid_client_theme');
    return;
  }
  if (value.kind === 'clientToolSubmitted') {
    const fields = ['kind', 'sessionId', 'responseId', 'toolCallId', 'requestMethod', 'continuation', 'code', 'receiptIndex'];
    if (terminated || Object.keys(value).length !== fields.length || Object.keys(value).some(key => !fields.includes(key)) ||
        !validOptionalResponseId(value) || typeof value.sessionId !== 'string' || !value.sessionId ||
        value.sessionId.length > 64 || /[\u0000-\u001f\u007f]/u.test(value.sessionId) ||
        !validOptionalResponseId({responseId: value.toolCallId}) || !value.responseId ||
        value.requestMethod !== 'toolCallSubmit' || !['accepted', 'unknown'].includes(value.continuation) ||
        !Number.isInteger(value.code) || !(value.code === 200 || (value.code >= 400 && value.code <= 599)) ||
        (value.code !== 200 && value.continuation !== 'unknown') ||
        !Number.isSafeInteger(value.receiptIndex) || value.receiptIndex <= metrics.clientToolReceiptIndex || value.receiptIndex > 512)
      throw new Error('invalid_client_tool_receipt');
    metrics.clientToolReceipts.push({sessionId: value.sessionId, responseId: value.responseId,
      toolCallId: value.toolCallId, requestMethod: value.requestMethod, continuation: value.continuation,
      code: value.code, receiptIndex: value.receiptIndex});
    if (metrics.clientToolReceipts.length > 32) metrics.clientToolReceipts.shift();
    metrics.clientToolReceiptIndex = value.receiptIndex;
    return; // ACK只证明端结果受理；不改变回复计数、播放、主题或展示工具终态。
  }
  if (value.kind === 'focusReceipt') {
    recordFocusReceipt(value);
    return;
  }
  if (useWebRTC && ['webrtcConfigure', 'webrtcConfigureVision', 'webrtcVisionControl', 'webrtcVisionCancel',
    'webrtcRecover', 'webrtcAnswer', 'webrtcStop', 'webrtcVolume',
    'webrtcFinish', 'captureStart', 'captureStop', 'playbackStart', 'playbackStop', 'playbackGate'].includes(value.kind)) {
    rtcIncoming(value);
    return;
  }
  if (['captureStart', 'captureStop', 'playbackStart', 'playbackStop', 'playbackFrame', 'playbackGate'].includes(value.kind)) {
    if (useWebRTC) throw new Error('wrong_audio_gate');
    if (!worklet) throw new Error('media_not_ready');
    if (value.kind === 'captureStart' || value.kind === 'captureStop') {
      if (typeof value.id !== 'string' || !value.id || value.id.length > 128)
        throw new Error('invalid_capture_command');
      if (inputOwnership && !inputOwnership.forwardControl(value)) return;
      captureCommand = {id: value.id, enabled: value.kind === 'captureStart',
        ...(inputOwnership ? {sessionId: value.sessionId, routeGeneration: value.routeGeneration} : {})};
      metrics.captureOpen = null;
      if (inputOwnership) return; // 原端口已交付唯一线程，不重复post或从逻辑命令推断实态。
    }
    if (value.kind === 'playbackStart') {
      responseId = value.id;
      playbackEpoch = value.epoch;
      pendingPlayback.clear();
      currentReplyConsumed = 0;
      currentReplyConsumedNonSilent = 0;
      currentReplyConsecutiveConsumedNonSilent = 0;
      currentReplyConsecutiveNonSilent = 0;
      currentReplyPlayable = false;
      // 每次播放都重置验收身份；即使上游错误地复用回复ID，也不能沿用旧代次的首音或最终媒体事实。
      metrics.lastPlayableResponseId = '';
      metrics.lastPlayableConsumedResponseId = '';
      metrics.lastVoicedConsumedResponseId = '';
      metrics.lastVoicedConsumedResponseFrames = 0;
      metrics.lastMediaFinalResponseId = '';
      metrics.lastNetworkPackets = 0;
      metrics.lastDecodedFrames = 0;
      metrics.lastNonSilentFrames = 0;
      metrics.lastMissingPackets = 0;
      metrics.lastObservedComplete = false;
    }
    if (value.kind === 'playbackStop' && responseId === value.id) {
      responseId = playbackEpoch = '';
      pendingPlayback.clear();
      currentReplyConsumed = 0;
      currentReplyConsumedNonSilent = 0;
      currentReplyConsecutiveConsumedNonSilent = 0;
      currentReplyConsecutiveNonSilent = 0;
      currentReplyPlayable = false;
    }
    if (value.kind === 'playbackFrame' && responseId && value.epoch === playbackEpoch) {
      if (!Number.isSafeInteger(value.ticket) || value.ticket < 1 || value.ticket > 4294967295 ||
          !Array.isArray(value.samples) || value.samples.length !== captureSampleRate() * 20 / 1000 ||
          !value.samples.every(sample => Number.isInteger(sample) && sample >= -32768 && sample <= 32767) ||
          pendingPlayback.size >= 32 || pendingPlayback.has(value.ticket))
        throw new Error('invalid_playback_frame');
      const power = value.samples.reduce((sum, sample) => sum + sample * sample, 0);
      const nonSilent = value.samples.length > 0 &&
        power / value.samples.length >= nonSilentFramePower;
      pendingPlayback.set(value.ticket, nonSilent);
      if (nonSilent) {
        ++metrics.decodedNonSilent;
        ++currentReplyConsecutiveNonSilent;
        if (!currentReplyPlayable && currentReplyConsecutiveNonSilent >= 2) {
          currentReplyPlayable = true;
          ++metrics.playableResponses;
          metrics.lastPlayableResponseId = responseId;
        }
      } else currentReplyConsecutiveNonSilent = 0;
      showMetrics();
    }
    worklet.port.postMessage(value);
  } else if (value.kind === 'handoffState') {
    recordHandoffState(value);
  } else if (value.kind === 'humanControlState') {
    recordHumanControlState(value);
  } else if (value.kind === 'humanControlReceipt') {
    recordHumanControlReceipt(value);
  } else if (value.kind === 'state') {
    const enabled = value.established === true;
    // 旧WebRTC媒体恢复照旧；UDP/None只有原生已核原会话的控制恢复投影才能保留停止/结束入口。
    const recovering = value.recovering === true;
    const controlRecovering = globalThis.UrAIControlRecovery === true &&
      aiControlState?.originalSessionPreserved === true &&
      ['disconnected', 'connecting', 'synchronizing'].includes(aiControlState.state);
    if ((Object.hasOwn(value, 'recovering') && typeof value.recovering !== 'boolean') ||
        (recovering && ((!useWebRTC && !controlRecovering) || value.established !== false)))
      throw new Error('invalid_recovery_state');
    if (textOnlyDevice && ((recovering && !controlRecovering) || (enabled
        ? value.transport !== 'none' || Object.hasOwn(value, 'interactionMode')
        : Object.hasOwn(value, 'transport') || Object.hasOwn(value, 'interactionMode'))))
      throw new Error('invalid_text_only_state');
    if (!textOnlyDevice && enabled && !['halfDuplex', 'fullDuplex'].includes(value.interactionMode))
      throw new Error('invalid_mode');
    if (Object.hasOwn(value, 'responseId') &&
        (Object.keys(value).some(key =>
          !(textOnlyDevice ? ['kind', 'established', 'transport', 'text', 'responseId']
            : ['kind', 'established', 'recovering', 'interactionMode', 'text', 'responseId']).includes(key)) ||
          typeof value.responseId !== 'string' ||
          (value.responseId !== '' && !validOptionalResponseId(value))))
      throw new Error('invalid_control_response');
    if (terminated) return; // 原释放后的迟到投影不能重开按钮或覆盖终态文案。
    // 缺字段只兼容旧状态展示，不借播放ID补授权；未建立且非恢复必须退休旧控制槽。
    if (Object.hasOwn(value, 'responseId')) controlProjectionAvailable = true;
    controlResponseId = (enabled || recovering) && Object.hasOwn(value, 'responseId')
      ? value.responseId : '';
    status(String(value.text));
    if (enabled && !textOnlyDevice) metrics.interactionMode = value.interactionMode;
    // 暂断允许停止、定向取消及结束，不能开始新输入；真正恢复仍等待核心连通ACK屏障。
    for (const id of ['start', 'textInput', 'textAudio', 'sendText']) element(id).disabled = !enabled;
    for (const id of ['stop', 'cancel', 'end']) element(id).disabled = !(enabled || recovering);
    if (textOnlyDevice) {
      // None会话只有文字与原回复控制，永不把模式投影当成媒体就绪。
      for (const id of ['start', 'stop', 'textAudio']) element(id).disabled = true;
      element('textAudio').checked = false;
    }
  } else if (value.kind === 'text') {
    if (!validOptionalResponseId(value)) throw new Error('invalid_text_response_id');
    const view = element('transcript');
    view.textContent = (view.textContent + String(value.text)).slice(-16384);
  } else if (value.kind === 'toolStatus') {
    if (!validOptionalResponseId(value) || typeof value.id !== 'string' || !value.id || value.id.length > 128 ||
        !['started', 'succeeded', 'failed', 'unknown'].includes(value.state) ||
        (value.state === 'started'
          ? typeof value.name !== 'string' || !value.name || value.name.length > 128
          : value.name !== undefined))
      throw new Error('invalid_tool_status');
    const labels = {succeeded: '工具已返回结果', failed: '工具调用失败', unknown: '工具结果待确认'};
    const line = value.state === 'started'
      ? `\n正在调用工具：${value.name}\n`
      : `\n${labels[value.state]}（${value.id}）\n`;
    const view = element('transcript');
    view.textContent = (view.textContent + line).slice(-16384);
  } else if (value.kind === 'playbackInterrupted') {
    if (!Number.isSafeInteger(value.count) || value.count < metrics.interruptedPlaybacks)
      throw new Error('invalid_interruption_fact');
    metrics.interruptedPlaybacks = value.count;
  } else if (value.kind === 'responseCompleted') {
    if (typeof value.id !== 'string' || !value.id || value.id.length > 128 ||
        !['none', 'sent', 'cancelled', 'failed'].includes(value.audioState) ||
        !validMediaCounts(value))
      throw new Error('invalid_response_fact');
    completedResponseId = value.id;
    metrics.lastAudioState = value.audioState;
    // MQTT终态可能迟于本地停播；此时不让较早采样的媒体计数覆盖最终事实。
    if (responseId === value.id && metrics.lastMediaFinalResponseId !== value.id) {
      metrics.lastNetworkPackets = value.networkPackets;
      metrics.lastDecodedFrames = value.decodedFrames;
      metrics.lastNonSilentFrames = value.nonSilentFrames;
    }
    ++metrics.completedResponses;
  } else if (value.kind === 'responseMediaFinal') {
    if (typeof value.id !== 'string' || !value.id || value.id.length > 128 ||
        value.id !== responseId || !validFinalMedia(value))
      throw new Error('invalid_media_final');
    metrics.lastMediaFinalResponseId = value.id;
    metrics.lastNetworkPackets = value.networkPackets;
    metrics.lastDecodedFrames = value.decodedFrames;
    metrics.lastNonSilentFrames = value.nonSilentFrames;
    metrics.lastMissingPackets = value.missingPackets;
    metrics.lastObservedComplete = value.observedComplete;
  } else if (value.kind === 'replyTiming') {
    // 原生同一单调时钟仅作只读事实；允许合法重叠，不用页面时钟填补null或变更测量起点。
    const fields = ['kind', 'responseId', 'lastVoiceSentMs', 'firstTextMs', 'firstNetworkMs', 'firstPlayableMs'];
    if (Object.keys(value).length !== fields.length || fields.some(key => !Object.hasOwn(value, key)) ||
        typeof value.responseId !== 'string' || !value.responseId || value.responseId.length > 128 ||
        /[\u0000-\u001f]/.test(value.responseId) ||
        fields.slice(2).some(key => value[key] !== null && (!Number.isSafeInteger(value[key]) || value[key] < 0)) ||
        (value.firstNetworkMs !== null && value.firstPlayableMs !== null && value.firstPlayableMs < value.firstNetworkMs))
      throw new Error('invalid_reply_timing');
    // 停播与MQTT终态不保证同序，只认本机当前或刚完成原回复；迟到旧事实不能覆盖下一轮。
    if (responseId ? value.responseId === responseId
      : [completedResponseId, metrics.lastMediaFinalResponseId].includes(value.responseId))
      metrics.lastReplyTiming = {...value};
  } else if (value.kind === 'responseCancelled') {
    ++metrics.cancelledResponses;
  } else if (value.kind === 'textStatus') {
    const labels = {accepted: '云端已受理文字输入，等待模型回复。',
      busy: '云端会话正忙，请先结束当前输入或回复后重试。',
      unsupported: '云端未协商文字输入能力。', rejected: '云端拒绝了文字输入。'};
    if (!Object.hasOwn(labels, value.result)) throw new Error('invalid_text_status');
    metrics.textInputResult = value.result;
    status(labels[value.result]);
  } else if (value.kind === 'textSubmitted') {
    const labels = {submitted: '文字已提交到设备协议状态机，等待云端确认。',
      busy: '当前会话仍在采集或回复，请稍后重试。',
      unsupported: '本次会话未协商文字输入能力。', rejected: '文字输入未被本地设备状态机接纳。'};
    if (!Object.hasOwn(labels, value.result)) throw new Error('invalid_text_submission');
    status(labels[value.result]);
  } else if (value.kind === 'actionCompleted') {
    if (!Number.isInteger(value.code) || !Number.isInteger(value.volume) || value.volume < 0 || value.volume > 100)
      throw new Error('invalid_action_fact');
    if (value.code === 200) ++metrics.successfulActions;
    metrics.lastActualVolume = value.volume;
  } else if (value.kind === 'ended') release('会话已结束，麦克风已释放。');
  else if (value.kind === 'error') release('设备链路失败，麦克风已释放。');
  else throw new Error('unknown_message');
}
// dispatchLocalMessage 只分派固定Live命令，原AI/文件继续原handler；Live实际事实不得作为下行播放指令。
function dispatchLocalMessage(value) {
  if (['humanVoiceOutputInit', 'humanVoiceOutputBind', 'humanVoiceStart',
      'humanVoiceAnswer', 'humanVoiceOutputGate', 'humanVoiceStop'].includes(value?.kind)) {
    if (terminated || globalThis.UrHumanVoiceEnabled !== true || !duplexHumanMedia)
      throw new Error('invalid_human_voice_command');
    // SDP等命令异步执行；受理不回ACK，真实事实仍由原媒体节点沿emit回到C++。
    void duplexHumanMedia.handle(value).then(accepted => {
      if (!accepted && !terminated) release('人工语音驱动拒绝命令，已释放设备资源。');
    }, () => release('人工语音驱动失败，已释放设备资源。'));
  } else if (desktopLive && ['liveLaneGate', 'liveBegin', 'liveAnswer', 'liveTrackControl',
      'liveClose', 'liveVolume'].includes(value?.kind)) {
    if (!desktopLive.handle(value)) throw new Error('invalid_live_command');
  } else if (!desktopFiles?.handle(value)) incoming(value);
}
// activate必须由用户手势触发；实际AEC只来自MediaStreamTrack.getSettings。
async function activate() {
  element('activate').disabled = true;
  let phase = '本地配对'; // 仅输出固定阶段，不回显可能含令牌的浏览器错误原文。
  try {
    if (textOnlyDevice && (useWebRTC || ownershipBoot !== undefined ||
        globalThis.UrHumanVoiceEnabled === true || globalThis.UrWebRTCOutputFocus === true))
      throw new Error('invalid_text_only_assembly');
    if (ownershipBoot !== undefined && (useWebRTC ||
        typeof globalThis.UrInputOwnershipGate !== 'function' || typeof ownershipBoot !== 'string' ||
        !/^[1-9][0-9]{0,15}$/.test(ownershipBoot) || BigInt(ownershipBoot) > 9007199254740991n))
      throw new Error('invalid_input_assembly');
    const reply = await fetch('/bootstrap', {credentials: 'same-origin', cache: 'no-store'});
    if (!reply.ok) throw new Error('pairing_failed');
    const auth = await reply.json();
    if (typeof auth.token !== 'string' || !/^[a-f0-9]{64}$/.test(auth.token)) throw new Error('invalid_token');
    socket = new WebSocket(`ws://${location.host}/bridge?token=${auth.token}`);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('pairing_timeout')), 5000);
      socket.onopen = () => { clearTimeout(timeout); resolve(); };
      socket.onerror = () => { clearTimeout(timeout); reject(new Error('pairing_failed')); };
    });
    socket.onclose = () => release('本机连接已关闭，麦克风已释放。');
    socket.onerror = () => release('本机连接故障，麦克风已释放。');
    socket.onmessage = event => {
      try {
        if (typeof event.data !== 'string' || event.data.length > 65536) throw new Error('message_limit');
        const value = JSON.parse(event.data);
        dispatchLocalMessage(value);
      } catch { release('本地媒体报文非法，已停止采集与播放。'); }
    };
    if (typeof globalThis.UrClientThemeDisplay === 'function') {
      // Window原生方法不能以platform对象为receiver；箭头适配保留原页面调用上下文，不更改主题期限或绘制判据。
      clientTheme = new UrClientThemeDisplay({element: document.body,
        emit: send, setTimer: (callback, delay) => setTimeout(callback, delay),
        clearTimer: timer => clearTimeout(timer),
        scheduleFrame: callback => requestAnimationFrame(callback),
        cancelFrame: frame => cancelAnimationFrame(frame),
        readStyle: element => getComputedStyle(element),
        isVisible: () => document.visibilityState === 'visible' &&
          document.body.getBoundingClientRect().width > 0 && document.body.getBoundingClientRect().height > 0,
      });
      if (!clientTheme.ready()) throw new Error('client_theme_not_ready');
    }
    if (textOnlyDevice) {
      if (!clientTheme || terminated || !send({kind: 'textReady'}))
        throw new Error('client_theme_not_ready');
      element('media').textContent = '纯文字设备：无麦克风、扬声器或媒体轨。';
      status('文字设备已就绪，等待平台协商。');
      return;
    }
    phase = '麦克风授权';
    stream = await navigator.mediaDevices.getUserMedia({audio: {
      channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true
    }, video: false});
    // 权限弹窗期间连接可能已关闭；迟到授权不得重新开启已结束会话的麦克风。
    if (terminated) { for (const track of stream.getTracks()) track.stop(); return; }
    if (useWebRTC) {
      phase = 'WebRTC音频端点';
      context = new AudioContext({sampleRate: 48000});
      if (context.sampleRate !== 48000) throw new Error('unsupported_sample_rate');
      await context.resume();
      await initializeRTCOutput();
      if (globalThis.UrAIEndpointListen === true) {
        // 候选输入沿原处理线程保留实际false探针，网络Peer不借ready或Focus自行开麦。
        if (ownershipBoot === undefined || !rtcOutputNode || typeof globalThis.UrInputOwnershipGate !== 'function')
          throw new Error('invalid_endpoint_input');
        await context.audioWorklet.addModule('/audio-worklet.js');
        if (terminated) return;
        worklet = new AudioWorkletNode(context, 'ur-device-audio', {
          numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
          channelCount: 1, channelCountMode: 'explicit',
          processorOptions: {sampleRate: 48000, frameMs: 20,
            inputOwnership: true, ownershipBootGeneration: ownershipBoot}
        });
        worklet.port.onmessage = event => receiveAudioThread(event.data);
        endpointOwnershipSource = context.createMediaStreamSource(stream);
        inputOwnership = new UrInputOwnershipGate({stream, source: endpointOwnershipSource, worklet,
          bootGeneration: ownershipBoot,
          senders: () => rtcEndpointSender && rtcPeer?.getSenders().includes(rtcEndpointSender) ? [rtcEndpointSender] : [],
          emit: send});
        endpointOwnershipSource.connect(worklet); worklet.connect(context.destination);
        if (!inputOwnership.live()) throw new Error('invalid_endpoint_input');
      }
      await initializeProgramOutputs(auth.token);
      if (terminated) return;
      await rtcPreflightVision();
      if (terminated) return;
      const track = stream.getAudioTracks()[0];
      track.onended = () => release('麦克风不可用，已结束本地连接。');
      rtcCreatePeer(1n);
      const aec = track.getSettings().echoCancellation === true;
      element('media').textContent = `WebRTC Opus 48 kHz / 20 ms；浏览器 AEC：${aec ? '已启用' : '未确认，仅允许半双工'}`;
      send({kind: 'ready', sampleRate: 48000, frameMs: 20, aec});
      status('WebRTC媒体驱动已就绪，等待平台协商。');
      return;
    }
    phase = '音频上下文';
    const captureRate = captureSampleRate();
    context = new AudioContext({sampleRate: captureRate});
    if (context.sampleRate !== captureRate) throw new Error('unsupported_sample_rate');
    phase = '音频处理器加载';
    await context.audioWorklet.addModule('/audio-worklet.js');
    if (terminated) return;
    worklet = new AudioWorkletNode(context, 'ur-device-audio', {
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
      channelCount: 1, channelCountMode: 'explicit',
      processorOptions: {sampleRate: captureRate, frameMs: 20, observeCaptureState: true,
        ...(observeAudioFailure ? {observeFailure: true} : {}),
        ...(ownershipBoot !== undefined ? {inputOwnership: true, ownershipBootGeneration: ownershipBoot} : {})}
    });
    worklet.port.onmessage = event => receiveAudioThread(event.data);
    source = context.createMediaStreamSource(stream);
    if (ownershipBoot !== undefined) {
      inputOwnership = new UrInputOwnershipGate({stream, source, worklet, bootGeneration: ownershipBoot,
        senders: () => duplexHumanMedia ? duplexHumanMedia.actualSenders() : [], emit: forwardInputOwnershipActual});
    }
    source.connect(worklet);
    worklet.connect(context.destination);
    phase = '音频上下文启动';
    await context.resume();
    if (terminated) return;
    // 原AudioContext此前可能合法suspended；活源资格必须在原resume完成后核验。
    if (inputOwnership && !inputOwnership.live()) throw new Error('invalid_input_assembly');
    await initializeHumanVoiceMedia(); // 原context已运行后才允许静音输出探针，准备不授予采集权限。
    if (terminated) return;
    await initializeProgramOutputs(auth.token);
    if (terminated) return;
    const track = stream.getAudioTracks()[0];
    const aec = track.getSettings().echoCancellation === true;
    track.onended = () => release('麦克风不可用，已结束本地连接。');
    element('media').textContent = `${captureRate / 1000} kHz / 单声道 / 20 ms；浏览器 AEC：${aec ? '已启用' : '未确认，仅允许半双工'}`;
    send({kind: 'ready', sampleRate: captureRate, frameMs: 20, aec});
    status('音频设备已就绪，等待平台协商。');
  } catch (error) {
    const known = ['NotAllowedError', 'NotFoundError', 'NotSupportedError', 'SecurityError', 'AbortError'];
    const category = known.includes(error?.name) ? error.name : '初始化失败';
    release(`${phase}失败（${category}）；请检查浏览器权限及本机设备进程。`);
  }
}
element('activate').addEventListener('click', activate);
if (globalThis.UrAIEndpointSource === true || globalThis.UrHumanEndpointSource === true) {
  element('endpointSourceControls').hidden = false;
  if (globalThis.UrHumanEndpointSource === true)
    element('endpointSourceStatus').textContent = '等待原人与人通话及实际资源允许接续';
  if (globalThis.UrHumanEndpointSource === true) {
    element('humanInputStop').hidden = false;
    element('humanInputStop').onclick = () => {
      if (!terminated && handoffState?.canHangup)
        send({kind: 'humanInputControl', operation: 'stop'}); // 只停本人输入，不携scope、许可或恢复意图。
    };
  }
  for (const [id, operation, flag] of [['endpointSourcePrepare', 'prepare', 'canPrepare'],
    ['endpointSourceCommit', 'commit', 'canCommit'], ['endpointSourceAbort', 'abort', 'canAbort']])
    element(id).onclick = () => {
      if (terminated || !aiEndpointSourceState?.[flag]) return;
      const intent = {kind: 'endpointSourceControl', operation};
      if (operation === 'prepare') {
        const target = element('endpointSourceTarget').value;
        if (!validHandoffTarget(target)) { element('endpointSourceStatus').textContent = '请填写合法的已授权目标端标识。'; return; }
        intent.target = target;
      }
      send(intent); // 原腿、候选、修订、token及MQTT编码只能由C++原控制器负责。
    };
}
if (globalThis.UrAIEndpointListen === true) {
  element('endpointControls').hidden = false;
  for (const [id, flag] of [['endpointAccept', 'canAccept'], ['endpointReject', 'canReject']])
    element(id).addEventListener('click', () => {
      if (!terminated && aiEndpointState?.[flag]) send({kind: id}); // 本人明确意图，不携自由会话/截止。
    });
}
if (textOnlyDevice) {
  element('activate').textContent = '连接文字设备';
  element('textAudio').checked = false;
}
element('handoffControls').hidden = globalThis.UrHumanVoiceEnabled !== true;
element('handoffOutbound').hidden = humanReceiveOnly && !humanDialEnabled;
element('handoffCancel').hidden = humanReceiveOnly && !humanDialEnabled;
element('handoffHold').addEventListener('click', () => submitHumanControl('hold'));
element('handoffResume').addEventListener('click', () => submitHumanControl('resume'));
element('temporaryResumeAI').hidden = !temporaryCalls;
element('temporaryResumeAI').disabled = true;
if (temporaryCalls) element('handoffEscalate').textContent = '临时人际语音通话';
if (humanDialEnabled) element('handoffEscalate').textContent = '发起人与人语音通话';
element('temporaryResumeAI').addEventListener('click', () => {
  if (terminated || !temporaryCalls || !temporaryResumeAllowed) return;
  temporaryResumeAllowed = false; // 原请求在途不重复点击；结果及实际资源仍等native投影。
  element('temporaryResumeAI').disabled = true;
  send({kind: 'start'}); // 新输入由C++生成ID/token，页面不能复用旧关联或直接操作Sender。
});
for (const [operation, [id]] of Object.entries(handoffButtons)) {
  element(id).disabled = true;
  element(id).addEventListener('click', () => submitHandoffIntent(operation));
}
for (const id of ['start', 'stop', 'end']) element(id).addEventListener('click', () => send({kind: id}));
element('sendText').addEventListener('click', () => {
  const text = element('textInput').value;
  if (!text.trim()) { status('请输入文字后再发送。'); return; }
  if (send({kind: 'sendText', text, audio: !textOnlyDevice && element('textAudio').checked})) {
    element('textInput').value = '';
    status('正在提交文字输入。');
  }
});
element('cancel').addEventListener('click', () => {
  // 新宿主统一使用核心目标；仅旧UDP宿主从未提供投影时保留本地播放取消，最终仍由C++复核。
  const target = controlProjectionAvailable || useWebRTC || textOnlyDevice ? controlResponseId : responseId;
  if (target) send({kind: 'cancel', id: target});
});
window.addEventListener('pagehide', () => release('页面已关闭。'));
