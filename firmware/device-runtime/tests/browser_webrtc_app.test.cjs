// 文件说明：在隔离VM验证正式浏览器脚本的WebRTC本地栅栏；不冒充真实ICE或硬件AEC验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');

// visionPage只提供原浏览器API的可控端口；能力预检不是真实摄像头或模型取帧验收。
function visionPage() {
  const value = page(), peers = [], calls = {camera: 0};
  value.state.track.readyState = 'live';
  value.context.UrAIVisionCamera = {cameraId: 'front', videoTrack: 'vision-1'};
  value.context.RTCRtpSender = {getCapabilities: () => ({codecs: [
    {mimeType: 'video/VP8'}, {mimeType: 'video/H264', clockRate: 90000}]})};
  value.context.navigator = {mediaDevices: {getUserMedia() { ++calls.camera; throw new Error('camera_forbidden'); }}};
  value.context.setInterval = () => 1; value.context.clearInterval = () => {};
  value.context.RTCPeerConnection = class {
    constructor() { this.transceivers = []; this.iceGatheringState = 'complete'; peers.push(this); }
    addTransceiver(track, options) {
      const entry = {kind: typeof track === 'string' ? track : track.kind,
        direction: options.direction, sender: {track: typeof track === 'string' ? null : track},
        setCodecPreferences(codecs) { this.codecs = codecs; }};
      this.transceivers.push(entry); return entry;
    }
    getSenders() { return this.transceivers.map(item => item.sender); }
    getTransceivers() { return this.transceivers; }
    createOffer() { return Promise.resolve({sdp: 'v=0\r\no=vision\r\n'}); }
    setLocalDescription(value) { this.localDescription = value; return Promise.resolve(); }
    setRemoteDescription() { return Promise.resolve(); }
    close() { this.closed = true; }
  };
  return {...value, peers, calls};
}

// visionCommand是原C++桥的固定paused配置，不携本人同意或可执行的摄像头发布许可。
function visionCommand(overrides = {}) {
  return {kind: 'webrtcConfigureVision', peerRevision: '1', cameraId: 'front',
    videoTrack: 'vision-1', codec: 'H264', state: 'paused', ...overrides};
}

test('视觉Peer仅建立原麦克风/扬声器/初始暂停H264三轨，不打开摄像头', async () => {
  const value = visionPage();
  assert.doesNotThrow(() => value.context.incoming(visionCommand()));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.peers.length, 1);
  const peer = value.peers[0];
  assert.deepEqual(peer.transceivers.map(item => [item.kind, item.direction]),
    [['audio', 'sendonly'], ['audio', 'recvonly'], ['video', 'sendonly']]);
  assert.equal(peer.transceivers[0].sender.track, value.state.track);
  assert.equal(value.state.track.enabled, false);
  assert.equal(peer.transceivers[2].sender.track, null);
  assert.deepEqual(Array.from(peer.transceivers[2].codecs, item => item.mimeType), ['video/H264']);
  assert.equal(value.calls.camera, 0);
  assert.equal(value.sent.filter(item => item.kind === 'webrtcOffer').length, 1);
  assert.equal(value.sent.some(item => item.kind === 'trackState' || item.kind === 'ready'), false);
});

test('视觉配置缺显式端口或H264、非法轨或未paused时拒绝且不退休原Peer', () => {
  for (const invalid of [{cameraId: 'other'}, {videoTrack: 'mic-1'}, {state: 'published'},
    {codec: 'vp8'}, {extra: true}, {cameraId: null}]) {
    const value = visionPage(), original = value.context.__testPeer;
    assert.throws(() => value.context.incoming(visionCommand(invalid)), /invalid_vision_config/);
    assert.equal(vm.runInContext('rtcPeer', value.context), original);
    assert.equal(value.peers.length, 0);
  }
  for (const unsupported of ['off', 'codec']) {
    const value = visionPage();
    if (unsupported === 'off') delete value.context.UrAIVisionCamera;
    else value.context.RTCRtpSender.getCapabilities = () => ({codecs: [{mimeType: 'video/VP8'}]});
    assert.throws(() => value.context.incoming(visionCommand()), /invalid_vision_config/);
    assert.equal(value.peers.length, 0);
  }
});

test('视觉配置重复和恢复不得创建第二Peer，停止后迟到offer不能发布或复活', async () => {
  const value = visionPage(); let finish;
  value.context.RTCPeerConnection.prototype.createOffer = () => new Promise(resolve => { finish = resolve; });
  value.context.incoming(visionCommand());
  assert.throws(() => value.context.incoming(visionCommand()), /invalid_vision_config/);
  assert.throws(() => value.context.incoming({kind: 'webrtcConfigure', peerRevision: '1'}), /rtc_command_fields/);
  vm.runInContext('rtcDisconnected=true;', value.context);
  assert.throws(() => value.context.incoming({kind: 'webrtcRecover', peerRevision: '2'}), /invalid_peer_recovery/);
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  finish({sdp: 'v=0\r\no=late\r\n'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.peers[0].closed, true);
  assert.equal(value.peers[0].transceivers[2].sender.track, null);
  assert.equal(value.sent.length, 0);
  assert.equal(value.calls.camera, 0);
  assert.equal(vm.runInContext('terminated', value.context), false);
});

test('视觉配置原输入已结束或已Connected时不借旧能力重新创建Peer', () => {
  for (const variant of ['ended', 'connected', 'extraInput']) {
    const value = visionPage();
    if (variant === 'ended') value.state.track.readyState = 'ended';
    if (variant === 'connected') vm.runInContext('rtcWasConnected=true;', value.context);
    if (variant === 'extraInput') value.context.__testStream.getAudioTracks = () => [value.state.track, value.state.track];
    assert.throws(() => value.context.incoming(visionCommand()), /invalid_vision_config/);
    assert.equal(value.peers.length, 0);
    assert.equal(value.calls.camera, 0);
  }
});

test('视觉候选创建部分失败精确关闭Peer，不停止原麦克风或摄像头取帧', () => {
  const value = visionPage();
  const original = value.context.RTCPeerConnection.prototype.addTransceiver;
  value.context.RTCPeerConnection.prototype.addTransceiver = function(track, options) {
    const entry = original.call(this, track, options);
    if (track === 'video') entry.sender.track = {kind: 'video'};
    return entry;
  };
  assert.throws(() => value.context.incoming(visionCommand()), /invalid_vision_sender/);
  assert.equal(value.peers[0].closed, true);
  assert.equal(vm.runInContext('rtcPeer', value.context), null);
  assert.equal(value.state.track.readyState, 'live');
  assert.equal(value.calls.camera, 0);
  assert.equal(value.sent.length, 0);
});

test('视觉协商期间宿主取消与迟到失败不会重新开放摄像头或关闭其它活组件', async () => {
  const value = visionPage(); let reject;
  value.context.RTCPeerConnection.prototype.createOffer = () => new Promise((_, fail) => { reject = fail; });
  value.context.incoming(visionCommand());
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  reject(new Error('fixed retired test failure'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.peers[0].closed, true);
  assert.equal(vm.runInContext('terminated', value.context), false);
  assert.throws(() => value.context.incoming(visionCommand()), /invalid_vision_config/);
  assert.equal(value.sent.length, 0);
});

test('原native明确camera设备映射可预置pausedPeer，但不能据映射启动采集', async () => {
  const value = visionPage();
  value.context.UrAIVisionCamera.deviceId = 'original-camera-device';
  assert.doesNotThrow(() => value.context.incoming(visionCommand()));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.peers[0].transceivers[2].sender.track, null);
  assert.equal(value.calls.camera, 0);
  assert.equal(value.sent.some(fact => fact.kind === 'trackState'), false);
});

// cameraControlCommand关联原Core控制代次，不把网页Peer或摄像头映射当作本人授权。
function cameraControlCommand(generation = '1', operation = 'publish', overrides = {}) {
  return {kind: 'webrtcVisionControl', generation, sessionId: 'original-vision-session',
    cameraId: 'front', trackId: 'vision-1', mediaRevision: '1', peerRevision: '1', operation, ...overrides};
}

// cameraPage通过正式incoming准备pausedPeer，再回放真实API端口的确定性完成边界；不是浏览器端云证明。
async function cameraPage(mapped = true) {
  const value = visionPage(), cameras = [], requests = [];
  if (mapped) value.context.UrAIVisionCamera.deviceId = 'original-camera-device';
  value.context.navigator.mediaDevices.getUserMedia = async options => {
    ++value.calls.camera; requests.push(options);
    const track = {kind: 'video', readyState: 'live', enabled: true, stopped: 0,
      getSettings: () => ({deviceId: 'original-camera-device'}),
      stop() { ++this.stopped; this.readyState = 'ended'; this.enabled = false; }};
    const camera = {track, getTracks: () => [track], getVideoTracks: () => [track], getAudioTracks: () => []};
    cameras.push(camera); return camera;
  };
  value.context.incoming(visionCommand());
  await new Promise(resolve => setImmediate(resolve));
  const peer = value.peers[0], sender = peer.transceivers[2].sender;
  sender.replaceTrack = async track => { sender.track = track; };
  peer.connectionState = 'connected'; peer.onconnectionstatechange();
  return {...value, cameras, requests, peer, sender};
}

test('AI摄像头publish无原物理映射只报明确失败，不打开默认摄像头', async () => {
  const value = await cameraPage(false);
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  const facts = value.sent.filter(fact => fact.kind === 'webrtcVisionActual');
  assert.equal(facts.length, 1);
  assert.equal(facts[0].state, 'failed'); assert.equal(facts[0].error, 'unsupported');
  assert.equal(value.calls.camera, 0); assert.equal(value.sender.track, null);
});

test('AI摄像头仅原选择取视频，实际replaceTrack完成并回读前不能报告active', async () => {
  const value = await cameraPage(); let complete;
  value.sender.replaceTrack = track => new Promise(resolve => {
    complete = () => { value.sender.track = track; resolve(); };
  });
  value.context.UrAIVisionCamera.deviceId = 'later-other-camera';
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.calls.camera, 1);
  assert.deepEqual(JSON.parse(JSON.stringify(value.requests)), [
    {audio: false, video: {deviceId: {exact: 'original-camera-device'}}}]);
  assert.equal(value.sent.some(fact => fact.kind === 'webrtcVisionActual'), false);
  complete(); await new Promise(resolve => setImmediate(resolve));
  const fact = value.sent.find(fact => fact.kind === 'webrtcVisionActual');
  const expected = {...cameraControlCommand(), kind: 'webrtcVisionActual', state: 'active', error: ''};
  delete expected.operation;
  assert.deepEqual(fact, expected);
});

test('AI摄像头stop必须实际Sender为空且本次全部轨停止，不能停止原音频轨', async () => {
  const value = await cameraPage();
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, value.cameras[0].track);
  value.context.incoming(cameraControlCommand('2', 'stop'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, null);
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.equal(value.state.track.readyState, 'live');
  assert.equal(value.sent.filter(fact => fact.kind === 'webrtcVisionActual' && fact.generation === '2')[0].state, 'stopped');
});

test('AI摄像头stop抢占未完成getUM，迟到原流必须全停而不能补active', async () => {
  const value = await cameraPage(); let grant;
  const actualGet = value.context.navigator.mediaDevices.getUserMedia;
  value.context.navigator.mediaDevices.getUserMedia = options => new Promise(resolve => {
    grant = async () => resolve(await actualGet(options));
  });
  value.context.incoming(cameraControlCommand());
  value.context.incoming(cameraControlCommand('2', 'stop'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sent.some(fact => fact.kind === 'webrtcVisionActual' && fact.state === 'stopped'), false);
  await grant(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, null);
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.equal(value.sent.some(fact => fact.kind === 'webrtcVisionActual' && fact.state === 'active'), false);
  assert.equal(value.sent.find(fact => fact.kind === 'webrtcVisionActual' && fact.generation === '2').state, 'stopped');
});

test('AI摄像头Peer退休后迟到replaceTrack仍精确归零，禁止旧active回执', async () => {
  const value = await cameraPage(); let finish;
  value.sender.replaceTrack = track => track ? new Promise(resolve => {
    finish = () => { value.sender.track = track; resolve(); };
  }) : Promise.resolve(value.sender.track = null);
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  finish(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, null);
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.equal(value.peer.closed, true);
  assert.equal(value.sent.some(fact => fact.kind === 'webrtcVisionActual' && fact.state === 'active'), false);
});

test('AI摄像头取消仅撤原代次，非法或重复控制不取得另一camera和会话', async () => {
  const value = await cameraPage();
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  value.context.incoming(cameraControlCommand());
  assert.equal(value.calls.camera, 1);
  for (const changes of [{sessionId: 'other'}, {trackId: 'mic-1'}, {mediaRevision: '2'},
    {operation: 'resume'}, {generation: '0'}, {extra: true}])
    assert.throws(() => value.context.incoming(cameraControlCommand('2', 'publish', changes)), /invalid_vision_control/);
  const cancel = {...cameraControlCommand(), kind: 'webrtcVisionCancel'}; delete cancel.operation;
  value.context.incoming(cancel);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.equal(value.sender.track, null);
  assert.equal(value.sent.filter(fact => fact.kind === 'webrtcVisionActual' && fact.state === 'stopped').length, 0);
});

test('AI摄像头拒权或错误物理流均回固定失败并结束全部原轨', async () => {
  for (const variant of ['denied', 'wrongDevice', 'extraAudio', 'replaceRejected', 'readbackWrong']) {
    const value = await cameraPage(), actualGet = value.context.navigator.mediaDevices.getUserMedia;
    value.context.navigator.mediaDevices.getUserMedia = async options => {
      if (variant === 'denied') throw new Error('private browser permission text');
      const camera = await actualGet(options);
      if (variant === 'wrongDevice') camera.track.getSettings = () => ({deviceId: 'other-camera'});
      if (variant === 'extraAudio') {
        const audio = {kind: 'audio', readyState: 'live', stop() { this.readyState = 'ended'; }};
        camera.getTracks = () => [camera.track, audio]; camera.getAudioTracks = () => [audio];
      }
      return camera;
    };
    if (variant === 'replaceRejected') value.sender.replaceTrack = async () => { throw new Error('private replacement text'); };
    if (variant === 'readbackWrong') value.sender.replaceTrack = async () => {};
    value.context.incoming(cameraControlCommand());
    await new Promise(resolve => setImmediate(resolve));
    const fact = value.sent.find(item => item.kind === 'webrtcVisionActual');
    assert.equal(fact.state, 'failed'); assert.equal(fact.error, 'mediaFailed');
    assert.equal(value.sender.track, null);
    assert.ok(value.cameras.every(camera => camera.getTracks().every(track => track.readyState === 'ended')));
    assert.equal(value.state.track.readyState, 'live');
    assert.equal(JSON.stringify(fact).includes('private'), false);
    assert.throws(() => value.context.incoming(cameraControlCommand('2')), /invalid_vision_control/);
  }
});

test('AI摄像头Stop替轨失败不能假称stopped或重新publish', async () => {
  const value = await cameraPage();
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  value.sender.replaceTrack = async () => { throw new Error('private stop text'); };
  value.context.incoming(cameraControlCommand('2', 'stop'));
  await new Promise(resolve => setImmediate(resolve));
  const fact = value.sent.find(item => item.kind === 'webrtcVisionActual' && item.generation === '2');
  assert.equal(fact.state, 'failed'); assert.equal(fact.error, 'mediaFailed');
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.notEqual(value.sender.track, null);
  assert.throws(() => value.context.incoming(cameraControlCommand('3')), /invalid_vision_control/);
  assert.equal(value.calls.camera, 1);
});

test('AI摄像头取消未决getUM不提前释放，迟到流结束后仍需新的Stop实证', async () => {
  const value = await cameraPage(), actualGet = value.context.navigator.mediaDevices.getUserMedia;
  let grant;
  value.context.navigator.mediaDevices.getUserMedia = options => new Promise(resolve => {
    grant = async () => resolve(await actualGet(options));
  });
  value.context.incoming(cameraControlCommand());
  const cancel = {...cameraControlCommand(), kind: 'webrtcVisionCancel'}; delete cancel.operation;
  value.context.incoming(cancel);
  assert.throws(() => value.context.incoming(cameraControlCommand('2')), /invalid_vision_control/);
  await grant(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, null);
  assert.equal(value.cameras[0].track.readyState, 'ended');
  assert.equal(value.sent.some(item => item.kind === 'webrtcVisionActual'), false);
  assert.throws(() => value.context.incoming(cameraControlCommand('2')), /invalid_vision_control/);
  value.context.incoming(cameraControlCommand('2', 'stop'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sent.find(item => item.kind === 'webrtcVisionActual').state, 'stopped');
});

test('AI摄像头原track未实际ended即使Sender为空也不能报告stopped', async () => {
  const value = await cameraPage();
  value.context.incoming(cameraControlCommand());
  await new Promise(resolve => setImmediate(resolve));
  value.cameras[0].track.stop = () => {}; // 可控HAL违约，不能用调用次数代替资源状态。
  value.context.incoming(cameraControlCommand('2', 'stop'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sender.track, null);
  assert.equal(value.cameras[0].track.readyState, 'live');
  const fact = value.sent.find(item => item.kind === 'webrtcVisionActual' && item.generation === '2');
  assert.equal(fact.state, 'failed'); assert.equal(fact.error, 'mediaFailed');
});

// page提供原页面HAL；scripts缺省只加载原app，组合回归按同源资产顺序逐个执行正式脚本。
function page(focus = false, transport = 'webrtc', scripts = [source]) {
  const sent = [];
  const elements = new Map();
  const state = {now: 1000, voiced: true, track: {kind: 'audio', enabled: false}, gain: {value: 0}};
  const context = vm.createContext({
    UrWebRTCOutputFocus: focus,
    location: {search: `?media=${transport}`}, URLSearchParams, Float32Array, TextEncoder,
    performance: {now: () => state.now}, WebSocket: {OPEN: 1},
    document: {getElementById(id) {
      // 原按钮点击遵守disabled；不能直接调用监听器绕过产品界面的输入禁令。
      if (!elements.has(id)) {
        const listeners = new Map();
        elements.set(id, {textContent: '', disabled: true, value: '', checked: false,
          addEventListener(kind, callback) { listeners.set(kind, callback); },
          click() { if (!this.disabled) listeners.get('click')?.(); }});
      }
      return elements.get(id);
    }},
    window: {addEventListener() {}},
  });
  for (const script of scripts) vm.runInContext(script, context);
  context.__testSocket = {readyState: 1, bufferedAmount: 0, send(body) { sent.push(JSON.parse(body)); }, close() {}};
  context.__testPeer = {getSenders: () => [{track: state.track}], close() {}};
  context.__testStream = {getAudioTracks: () => [state.track], getTracks: () => [{stop() {}}]};
  context.__testGain = {gain: state.gain};
  context.__testAnalyser = {fftSize: 1024, getFloatTimeDomainData(samples) {
    samples.fill(state.voiced ? 0.02 : 0);
  }};
  vm.runInContext('socket=__testSocket;rtcPeer=__testPeer;stream=__testStream;rtcGain=__testGain;rtcAnalyser=__testAnalyser', context);
  return {context, state, sent, elements};
}

// sessionControls只从正式incoming和按钮点击验证UI权限，不直接操作核心或伪造已建立。
function sessionControls(value, fact) {
  value.context.incoming(fact);
  return Object.fromEntries(['start', 'stop', 'cancel', 'end', 'textInput', 'textAudio', 'sendText']
    .map(id => [id, !value.elements.get(id).disabled]));
}

test('WebRTC恢复事实仅开放停止取消结束，正式按钮不能启动或提交新文字', () => {
  const value = page();
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  assert.deepEqual(sessionControls(value, {kind: 'state', established: false, recovering: true,
    responseId: 'original-response', text: '恢复中'}),
    {start: false, stop: true, cancel: true, end: true, textInput: false, textAudio: false, sendText: false});
  value.elements.get('textInput').value = '公开测试输入';
  for (const id of ['start', 'sendText', 'stop', 'cancel', 'end']) value.elements.get(id).click();
  assert.deepEqual(value.sent, [{kind: 'stop'}, {kind: 'cancel', id: 'original-response'}, {kind: 'end'}]);
  assert.equal(value.state.track.enabled, false, 'UI控制权限不是采集或媒体恢复许可');
});

test('正常建立仍校验原mode，恢复及终态不会借用旧建立权限', () => {
  const value = page();
  for (const mode of ['halfDuplex', 'fullDuplex']) {
    assert.ok(Object.values(sessionControls(value, {kind: 'state', established: true,
      interactionMode: mode, text: '已建立'})).every(Boolean));
    assert.equal(vm.runInContext('metrics.interactionMode', value.context), mode);
    for (const fact of [{kind: 'state', established: false, text: '协商或释放'},
      {kind: 'state', established: false, recovering: false, text: '未建立'}]) {
      assert.ok(Object.values(sessionControls(value, fact)).every(enabled => !enabled));
      for (const id of ['start', 'stop', 'cancel', 'end', 'sendText']) value.elements.get(id).click();
    }
  }
  assert.equal(value.sent.length, 0);
  assert.throws(() => value.context.incoming({kind: 'state', established: true,
    interactionMode: 'realtime', text: '错误模式'}), /invalid_mode/);
  sessionControls(value, {kind: 'state', established: true, recovering: false,
    interactionMode: 'halfDuplex', text: '原合法建立'});
  value.elements.get('textInput').value = '公开测试输入';
  value.elements.get('start').click(); value.elements.get('sendText').click();
  assert.deepEqual(value.sent, [{kind: 'start'}, {kind: 'sendText', text: '公开测试输入', audio: false}]);
});

test('非法恢复标记或UDP不能取得恢复控制，拒绝不得改变原界面', () => {
  for (const transport of ['webrtc', 'udp']) {
    const value = page(false, transport);
    sessionControls(value, {kind: 'state', established: false, text: '原未建立'});
    const invalid = [null, 'true', 1, {}, []].map(recovering =>
      ({kind: 'state', established: false, recovering, text: '非法状态'}));
    invalid.push({kind: 'state', established: true, recovering: true,
      interactionMode: 'halfDuplex', text: '矛盾状态'},
      {kind: 'state', recovering: true, text: '缺建立字段'});
    if (transport === 'udp') invalid.push({kind: 'state', established: false, recovering: true, text: '错误传输'});
    for (const fact of invalid) {
      assert.throws(() => value.context.incoming(fact), /invalid_recovery_state/);
      assert.equal(value.elements.get('status').textContent, '原未建立');
      assert.ok(['start', 'stop', 'cancel', 'end', 'sendText'].every(id => value.elements.get(id).disabled));
    }
    assert.equal(value.sent.length, 0);
  }
});

test('释放后的迟到恢复或建立投影不能重新开放控制', () => {
  const value = page();
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'halfDuplex', text: '已建立'});
  value.context.release('原终态');
  for (const fact of [{kind: 'state', established: false, recovering: true, text: '迟到恢复'},
    {kind: 'state', established: true, interactionMode: 'halfDuplex', text: '迟到建立'}]) {
    assert.ok(Object.values(sessionControls(value, fact)).every(enabled => !enabled));
    assert.equal(value.elements.get('status').textContent, '原终态');
    for (const id of ['start', 'stop', 'cancel', 'end', 'sendText']) value.elements.get(id).click();
  }
  assert.equal(value.sent.length, 0);
});

test('native仅原WebRTC暂断或已确认控制恢复投影非established，不能伪造业务建立', () => {
  const main = fs.readFileSync(path.join(__dirname, '../adapters/desktop/browser_main.cpp'), 'utf8');
  // UDP也可明确声明sessionSync；只有核心正处Recovering且原控制屏障成立，才允许同类停止控件。
  const fact = main.match(/state == VoiceState::Recovering\s*&&\s*\(use_webrtc \|\| runtime->voice\(\)\.control_recovering\(\)\)\s*\? R"\((\{"kind":"state"[^\n]+)\)"/);
  assert.ok(fact, '正式宿主须显式区分恢复与协商，不能伪造established');
  const value = page();
  assert.deepEqual(sessionControls(value, JSON.parse(fact[1])),
    {start: false, stop: true, cancel: true, end: true, textInput: false, textAudio: false, sendText: false});
});

// output固定测试节点只记录实际线程命令；必须另显式提交原音频块回执，不能预先确认。
function output(pageValue) {
  const commands = [];
  const {context, state} = pageValue;
  context.__output = {port: {postMessage: value => commands.push(value), close() {}}, disconnect() {}};
  context.__presented = {fftSize: 128, getFloatTimeDomainData: samples => samples.fill(state.voiced ? .02 : 0)};
  vm.runInContext('rtcOutputNode=__output;rtcPresentedAnalyser=__presented;', context);
  const actual = (ticket, enabled) =>
    context.rtcOutputActual(context.__output, {kind: 'callAudioGated', peerEpoch: '0', ticket, enabled});
  return {commands, actual};
}

test('WebRTC原末端实态门控与采集及回复意图分开，Gain不能绕过focus', () => {
  const value = page(true), gate = output(value);
  value.context.incoming({kind: 'captureStart', peerRevision: '1', id: 'original-input'});
  value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  assert.equal(value.sent.length, 0);
  gate.actual('1', false);
  assert.equal(value.state.track.enabled, true);
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.incoming({kind: 'webrtcVolume', id: 'original-volume', volume: 40});
  value.context.rtcObserve(); value.context.rtcObserve();
  assert.equal(vm.runInContext('metrics.lastPlayableConsumedResponseId', value.context), '');
  value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: true});
  gate.actual('1', false); // 原迟到false不能关闭新在途门，也不能误报新已取得。
  assert.equal(vm.runInContext('rtcGateKnown', value.context), false);
  gate.actual('2', true);
  value.context.rtcObserve(); value.context.rtcObserve();
  assert.equal(vm.runInContext('metrics.lastPlayableConsumedResponseId', value.context), 'original-response');
  assert.equal(value.state.track.enabled, true);
  const count = value.sent.length;
  gate.actual('2', true);
  assert.equal(value.sent.length, count, '原重复实态不新增ACK或期限');
});
test('WebRTC输出开启只能由真实原票据确认，额外或矛盾字段拒绝', () => {
  const value = page(true), gate = output(value);
  value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  assert.deepEqual(JSON.parse(JSON.stringify(gate.commands)), [
    {kind: 'callAudioMask', peerEpoch: '0', ticket: '1', enabled: false}]);
  assert.throws(() => value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: true}),
    /invalid_rtc_output_gate/);
  assert.equal(value.sent.length, 0);
  gate.actual('1', false);
});
// rtcObserve按分析器采样窗口观察原已解码流；未许可输出的有声不能混成已呈现或首音。
test('WebRTC门前解码有声计数独立于未确认门控及门后首音判据', () => {
  const value = page(true), gate = output(value);
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  value.context.rtcObserve(); value.context.rtcObserve();
  assert.equal(vm.runInContext('metrics.decodedNonSilent', value.context), 2);
  assert.equal(vm.runInContext('metrics.consumedNonSilent', value.context), 0);
  assert.equal(vm.runInContext('metrics.playableResponses', value.context), 0);
  assert.equal(vm.runInContext('metrics.lastPlayableConsumedResponseId', value.context), '');
  gate.actual('1', false);
  value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: true});
  value.context.rtcObserve(); value.context.rtcObserve();
  assert.equal(vm.runInContext('metrics.decodedNonSilent', value.context), 4);
  assert.equal(vm.runInContext('metrics.consumedNonSilent', value.context), 0);
  gate.actual('2', true);
  value.context.rtcObserve(); value.context.rtcObserve();
  assert.equal(vm.runInContext('metrics.decodedNonSilent', value.context), 6);
  assert.equal(vm.runInContext('metrics.consumedNonSilent', value.context), 2);
  assert.equal(vm.runInContext('metrics.playableResponses', value.context), 1);
  assert.equal(vm.runInContext('metrics.lastPlayableConsumedResponseId', value.context), 'original-response');
});
// 仅核实际展示函数的单位，合成数值不是网络、音频帧或硬件播放证据。
test('WebRTC展示分析器窗口而非音频线程实际帧', () => {
  const {context} = page();
  vm.runInContext('metrics.rtcSampleWindows=3;metrics.decodedNonSilent=2;metrics.consumedNonSilent=1;showMetrics();', context);
  const text = context.document.getElementById('metrics').textContent;
  assert.match(text, /WebRTC采样 3 窗口/);
  assert.match(text, /解码非静音 2 窗口/);
  assert.match(text, /门后有声观察 1 窗口/);
  assert.doesNotMatch(text, /帧|音频线程消费|硬件播放/);
});
test('UDP展示保留原Worklet音频帧及消费单位', () => {
  const {context} = page(false, 'udp');
  vm.runInContext('metrics.captured=1;metrics.decodedNonSilent=2;metrics.consumed=3;metrics.consumedNonSilent=1;showMetrics();', context);
  assert.equal(context.document.getElementById('metrics').textContent,
    '采集 1 帧，解码非静音 2 帧，音频线程消费 3 帧（有声 1 帧）');
});
test('WebRTC节目预检失败或双组装拒绝时不能发ready', async () => {
  for (const mode of [{live: true, liveReady: false}, {file: true, live: true}]) {
    const value = await activatedPage(mode);
    assert.equal(vm.runInContext('terminated', value.context), true);
    assert.equal(value.sent.filter(message => message.kind === 'ready').length, 0);
  }
});
test('原WebRTC配置异步失败在退休后不能关闭仍存活的节目宿主', async () => {
  const {context, sent} = page();
  let rejectOffer;
  context.__testPeer.createOffer = () => new Promise((_, reject) => { rejectOffer = reject; });
  context.__testPeer.setLocalDescription = async () => {};
  context.incoming({kind: 'webrtcConfigure', peerRevision: '1'});
  context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  rejectOffer(new Error('synthetic retired original'));
  await Promise.resolve(); await Promise.resolve();
  assert.equal(vm.runInContext('terminated', context), false);
  assert.equal(sent.length, 0, '原已退休Peer不能送迟到offer或整宿主失败');
});
test('WebRTC腿退休后原实际末端仍须完成独立false票据', () => {
  const value = page(true), gate = output(value);
  value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  gate.actual('1', false);
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: false});
  assert.equal(vm.runInContext('rtcGateKnown', value.context), false);
  gate.actual('2', false);
  assert.equal(value.sent.filter(message => message.kind === 'outputGated').length, 2);
  assert.equal(vm.runInContext('terminated', value.context), false);
});

test('WebRTC输入在C++确认前关闭，定向停播立即静音', () => {
  const {context, state, sent} = page();
  context.incoming({kind: 'captureStart', peerRevision: '1', id: 'input-1'});
  assert.equal(state.track.enabled, true);
  context.incoming({kind: 'captureStop', peerRevision: '1', id: 'input-1'});
  assert.equal(state.track.enabled, false);
  context.incoming({kind: 'webrtcVolume', id: 'tool-1', volume: 40});
  assert.deepEqual(sent, [{kind: 'webrtcVolumeApplied', id: 'tool-1', volume: 40}]);
  context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'reply-1', epoch: '1'});
  assert.equal(state.gain.value, 0.4);
  context.incoming({kind: 'playbackStop', peerRevision: '1', id: 'reply-1', epoch: '1'});
  assert.equal(state.gain.value, 0);
  assert.throws(() => context.incoming({kind: 'webrtcAnswer', peerRevision: '1', sdp: 'invalid'}), /invalid_answer/);
  assert.throws(() => context.incoming({kind: 'webrtcVolume', id: 'tool-2', volume: 101}), /invalid_volume/);
});

test('WebRTC远端音轨显式启动静音解码管线，结束时不遗留独立播放', () => {
  const {context} = page();
  const sink = {muted: false, paused: false, srcObject: null,
    play() { return Promise.resolve(); }, pause() { this.paused = true; }};
  context.document.createElement = () => sink;
  context.MediaStream = class { constructor(tracks) { this.tracks = tracks; } };
  const graph = {connect() {}, disconnect() {}};
  const gain = {gain: {value: 0}, connect() {}};
  context.__testAudioContext = {destination: graph,
    createMediaStreamSource() { return graph; },
    createAnalyser() { return {fftSize: 0, connect() {}}; },
    createGain() { return gain; }, close() { return Promise.resolve(); }};
  vm.runInContext('context=__testAudioContext;rtcAnalyser=null;rtcGain=null;', context);
  context.rtcAttachRemote({track: {kind: 'audio'}});
  assert.equal(sink.muted, true);
  assert.equal(sink.autoplay, true);
  assert.equal(sink.srcObject.tracks.length, 1);
  assert.equal(gain.gain.value, 0, '未收到回复开始时不能直接发声');
  context.release('done');
  assert.equal(sink.paused, true);
  assert.equal(sink.srcObject, null);
});

test('WebRTC回复仅在当前代次连续有声后可播放，尾部静音再报告排空', () => {
  const {context, state, sent} = page();
  context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'reply-1', epoch: '9'});
  context.rtcObserve();
  context.rtcObserve();
  const metrics = JSON.parse(vm.runInContext('JSON.stringify(metrics)', context));
  assert.equal(metrics.lastPlayableConsumedResponseId, 'reply-1');
  assert.throws(() => context.incoming({kind: 'webrtcFinish', peerRevision: '1', epoch: '8'}), /invalid_finish/);
  context.incoming({kind: 'webrtcFinish', peerRevision: '1', epoch: '9'});
  state.voiced = false;
  state.now = 1600;
  for (let index = 0; index < 10; index++) context.rtcObserve();
  assert.deepEqual(sent, [{kind: 'webrtcDrained', peerRevision: '1', epoch: '9'}]);
  context.rtcObserve();
  assert.equal(sent.length, 1);
});

test('WebRTC无解码图尾部排空必须携带当前Peer代次且仅报告一次', () => {
  for (const revision of ['1', '2']) {
    const {context, state, sent} = page();
    // 只模拟当前HAL代次及缺失解码图；固定尾部等待不冒认实际解码或有声播放。
    context.__testRevision = revision;
    vm.runInContext('rtcPeerRevision=BigInt(__testRevision);rtcAnalyser=null;', context);
    context.incoming({kind: 'playbackStart', peerRevision: revision, id: 'reply-current', epoch: '9'});
    context.incoming({kind: 'webrtcFinish', peerRevision: revision, epoch: '9'});
    state.now = 1999;
    context.rtcObserve();
    assert.deepEqual(sent, []);
    state.now = 2000;
    context.rtcObserve();
    assert.deepEqual(sent, [{kind: 'webrtcDrained', peerRevision: revision, epoch: '9'}]);
    state.now = 3000;
    context.rtcObserve();
    assert.equal(sent.length, 1);
    assert.equal(vm.runInContext('metrics.lastPlayableConsumedResponseId', context), '');
  }
});

test('WebRTC无解码图退休后的旧观察不能补排空或吞后继完成', () => {
  const {context, state, sent} = page();
  vm.runInContext('rtcAnalyser=null;', context);
  context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'old-reply', epoch: '9'});
  context.incoming({kind: 'webrtcFinish', peerRevision: '1', epoch: '9'});
  const oldObserve = context.rtcObserve; // 保存原调度回调，停腿后仍可能迟到被调用。
  context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  state.now = 2000;
  oldObserve();
  assert.deepEqual(sent, []);
  vm.runInContext('rtcPeer=__testPeer;rtcPeerRevision=2n;rtcAnalyser=null;', context);
  context.incoming({kind: 'playbackStart', peerRevision: '2', id: 'new-reply', epoch: '10'});
  context.incoming({kind: 'webrtcFinish', peerRevision: '1', epoch: '9'});
  state.now = 3000;
  oldObserve();
  assert.deepEqual(sent, [], '原代次迟到完成不能替后继排空');
  context.incoming({kind: 'webrtcFinish', peerRevision: '2', epoch: '10'});
  state.now = 4000;
  context.rtcObserve();
  assert.deepEqual(sent, [{kind: 'webrtcDrained', peerRevision: '2', epoch: '10'}]);
});

test('WebRTC无解码图宿主终态禁止旧尾部观察补排空', () => {
  const {context, state, sent} = page();
  vm.runInContext('rtcAnalyser=null;', context);
  context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'old-reply', epoch: '9'});
  context.incoming({kind: 'webrtcFinish', peerRevision: '1', epoch: '9'});
  const oldObserve = context.rtcObserve;
  context.release('测试宿主终态');
  state.now = 2000;
  oldObserve();
  assert.deepEqual(sent, []);
});

test('原WebRTC腿停止不得截断子节目和共享宿主释放事实', () => {
  const {context} = page();
  const effects = {peer: 0, files: 0, live: 0, socket: 0, context: 0};
  context.__testPeer.close = () => ++effects.peer;
  context.__testSocket.close = () => ++effects.socket;
  context.__files = {close: () => ++effects.files};
  context.__live = {close: () => ++effects.live};
  context.__audio = {close: () => ++effects.context};
  vm.runInContext('desktopFiles=__files;desktopLive=__live;context=__audio;', context);
  context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  assert.equal(effects.peer, 1);
  assert.deepEqual(effects, {peer: 1, files: 0, live: 0, socket: 0, context: 0});
  assert.equal(vm.runInContext('terminated', context), false);
});

// activatedPage运行原activate入口；合成HAL不产生网络/模型，也不把工厂存在冒认授权或节目收到。
async function activatedPage({file = false, live = false, liveReady = true, visionPreflight} = {}) {
  const value = page(), {context, state, sent} = value;
  const effects = {file: 0, live: 0, liveActivated: 0};
  state.track.getSettings = () => ({echoCancellation: true});
  context.fetch = async () => ({ok: true, json: async () => ({token: '0'.repeat(64)})});
  context.setTimeout = setTimeout; context.clearTimeout = clearTimeout;
  context.setInterval = () => 1; context.clearInterval = () => {};
  context.WebSocket = class {
    static OPEN = 1;
    constructor() { this.readyState = 1; this.bufferedAmount = 0; queueMicrotask(() => this.onopen()); }
    send(body) { sent.push(JSON.parse(body)); } close() {}
  };
  context.navigator = {mediaDevices: {getUserMedia: async () => context.__testStream}};
  if (visionPreflight) {
    // 本人视觉授权未发生；预检只能读取已有浏览器许可/设备清单，不许请求摄像头取流。
    context.UrAIVisionCamera = {cameraId: 'front', videoTrack: 'vision-1', deviceId: 'explicit-camera'};
    context.RTCRtpSender = {getCapabilities: () => ({codecs: [{mimeType: visionPreflight === 'noH264' ? 'video/VP8' : 'video/H264'}]})};
    context.navigator.permissions = {query: async () => {
      if (visionPreflight === 'permissionRejected') throw new Error('private permission text');
      return {state: ['denied', 'prompt'].includes(visionPreflight) ? visionPreflight : 'granted'};
    }};
    context.navigator.mediaDevices.enumerateDevices = async () => {
      if (visionPreflight === 'enumerationFailed') throw new Error('private device text');
      const device = {kind: visionPreflight === 'audioOnly' ? 'audioinput' : 'videoinput',
        deviceId: visionPreflight === 'hidden' ? '' : visionPreflight === 'wrongDevice' ? 'other' : 'explicit-camera'};
      return Array.from({length: visionPreflight === 'duplicate' ? 2 : visionPreflight === 'excessive' ? 129 : 1}, () => device);
    };
    if (visionPreflight === 'missingPermission') delete context.navigator.permissions;
    if (visionPreflight === 'missingEnumerator') delete context.navigator.mediaDevices.enumerateDevices;
    context.navigator.mediaDevices.getUserMedia = async options => {
      assert.equal(options.video, false, '正式预检不得隐式打开camera'); return context.__testStream;
    };
  }
  context.AudioContext = class {
    constructor() { this.sampleRate = 48000; }
    async resume() {} async close() {}
  };
  context.RTCPeerConnection = class {
    addTrack() {} getSenders() { return [{track: state.track}]; } close() {}
  };
  if (visionPreflight) context.RTCPeerConnection.prototype.addTransceiver = () => {};
  context.document.body = {append() {}};
  context.document.createElement = () => ({setAttribute() {}});
  if (file) context.UrDesktopFiles = class {
    constructor(audio, emit, token) {
      ++effects.file; assert.equal(audio.sampleRate, 48000); assert.equal(typeof emit, 'function');
      assert.equal(token, '0'.repeat(64), '必须传入原activate局部配对凭据，不能借未定义外层auth');
    }
    close() {}
  };
  if (live) context.UrLiveHost = class {
    constructor() { ++effects.live; }
    async activate() { ++effects.liveActivated; return liveReady; } async close() {}
  };
  await context.activate();
  return {...value, effects};
}

test('正式activate在原audio ready之前完成H264/本人浏览器许可/原摄像头枚举预检但不取帧', async () => {
  const value = await activatedPage({visionPreflight: 'ready'});
  const index = value.sent.findIndex(item => item.kind === 'webrtcVisionReady');
  assert.ok(index >= 0);
  assert.deepEqual(value.sent[index], {kind: 'webrtcVisionReady', cameraId: 'front', videoTrack: 'vision-1',
    deviceId: 'explicit-camera', state: 'ready'});
  assert.ok(value.sent.findIndex(item => item.kind === 'ready') > index);
});

test('摄像头许可未授/隐藏或错设备/无H264/枚举失败均不可广告视觉，普通audio ready不变', async () => {
  for (const visionPreflight of ['denied', 'prompt', 'hidden', 'wrongDevice', 'audioOnly', 'noH264',
    'enumerationFailed', 'permissionRejected', 'missingPermission', 'missingEnumerator', 'duplicate', 'excessive']) {
    const value = await activatedPage({visionPreflight});
    const fact = value.sent.find(item => item.kind === 'webrtcVisionReady');
    assert.ok(fact, visionPreflight); assert.equal(fact.state, 'unsupported');
    const audio = value.sent.find(item => item.kind === 'ready');
    assert.deepEqual(audio, {kind: 'ready', sampleRate: 48000, frameMs: 20, aec: true});
    assert.equal(JSON.stringify(fact).includes('private'), false);
  }
});

// recoverActiveReply经过原activate、当前Peer断连和退休；模拟HAL不替代实际ICE/模型验证。
async function recoverActiveReply() {
  const value = await activatedPage();
  const peer = vm.runInContext('rtcPeer', value.context);
  value.context.RTCPeerConnection.prototype.createOffer = async () => ({sdp: 'v=0\r\no=recovery\r\n'});
  value.context.RTCPeerConnection.prototype.setLocalDescription = async function(description) {
    this.localDescription = description; this.iceGatheringState = 'complete';
  };
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'halfDuplex',
    responseId: 'original-response', text: '原回复'});
  peer.connectionState = 'connected'; peer.onconnectionstatechange();
  peer.connectionState = 'disconnected'; peer.onconnectionstatechange();
  value.context.incoming({kind: 'webrtcRecover', peerRevision: '2'});
  await new Promise(resolve => setImmediate(resolve));
  sessionControls(value, {kind: 'state', established: false, recovering: true,
    responseId: 'original-response', text: '恢复中'});
  return {...value, oldPeer: peer};
}

test('真实Peer退休后恢复按钮仍须取消核心原回复，而不是已清播放目标', async () => {
  const value = await recoverActiveReply();
  assert.equal(vm.runInContext('responseId', value.context), '', '旧播放目标必须退休且不能被控制投影复活');
  value.elements.get('cancel').click();
  assert.deepEqual(value.sent.filter(message => message.kind === 'cancel'),
    [{kind: 'cancel', id: 'original-response'}]);
  assert.equal(value.state.track.enabled, false);
});

test('恢复后新核心回复替换控制目标，旧Peer回调和旧终态不得取消后继', async () => {
  const value = await recoverActiveReply();
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'fullDuplex',
    responseId: 'next-response', text: '已恢复'});
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.incoming({kind: 'responseCompleted', id: 'original-response', audioState: 'sent',
    networkPackets: 0, decodedFrames: 0, nonSilentFrames: 0});
  value.elements.get('cancel').click();
  assert.deepEqual(value.sent.filter(message => message.kind === 'cancel'),
    [{kind: 'cancel', id: 'next-response'}]);
  assert.equal(vm.runInContext('responseId', value.context), '');
});

test('原核心目标清除或会话终态后不能借陈旧播放和迟到投影取消', async () => {
  const value = await recoverActiveReply();
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'halfDuplex',
    responseId: '', text: '原回复已结束'});
  value.context.incoming({kind: 'playbackStart', peerRevision: '2', id: 'finished-response', epoch: '2'});
  value.elements.get('cancel').click();
  sessionControls(value, {kind: 'state', established: false, responseId: 'finished-response', text: '终态'});
  value.elements.get('cancel').click();
  value.context.release('已释放');
  sessionControls(value, {kind: 'state', established: false, recovering: true,
    responseId: 'original-response', text: '迟到'});
  value.elements.get('cancel').click();
  assert.deepEqual(value.sent.filter(message => message.kind === 'cancel'), []);
});

test('本地取消目标拒绝非法类型控制字符和超长UTF8，拒绝不替换原目标', async () => {
  const value = await recoverActiveReply();
  value.context.TextEncoder = TextEncoder;
  for (const responseId of [null, 1, {}, 'bad\nresponse', '\ud800', '字'.repeat(43)]) {
    assert.throws(() => sessionControls(value, {kind: 'state', established: false, recovering: true,
      responseId, text: '非法目标'}), /invalid_control_response/);
  }
  for (const extra of [{id: 'other'}, {sessionId: 'other'}, {peerRevision: '2'}])
    assert.throws(() => sessionControls(value, {kind: 'state', established: false, recovering: true,
      responseId: 'other-response', text: '伪造额外字段', ...extra}), /invalid_control_response/);
  value.elements.get('cancel').click();
  assert.deepEqual(value.sent.filter(message => message.kind === 'cancel'),
    [{kind: 'cancel', id: 'original-response'}]);
});

test('缺少核心控制投影不借播放目标授权，UDP仍沿原路径取消', () => {
  const value = page();
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'playing', epoch: '1'});
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'halfDuplex',
    responseId: 'core-response', text: '原核心目标'});
  sessionControls(value, {kind: 'state', established: true, interactionMode: 'halfDuplex', text: '旧展示投影'});
  value.elements.get('cancel').click();
  assert.deepEqual(value.sent, []);
  const udp = page(false, 'udp');
  vm.runInContext("responseId='original-udp-response';", udp.context);
  sessionControls(udp, {kind: 'state', established: true, interactionMode: 'halfDuplex', text: 'UDP'});
  udp.elements.get('cancel').click();
  assert.deepEqual(udp.sent, [{kind: 'cancel', id: 'original-udp-response'}]);
});

test('原控制ID允许转义字符和合法UTF8边界但不复活播放或采集', async () => {
  const value = await recoverActiveReply();
  for (const id of ['core-"\\target', '字'.repeat(42) + 'ab', '回复😀']) {
    sessionControls(value, {kind: 'state', established: false, recovering: true, responseId: id, text: '恢复'});
    value.elements.get('cancel').click();
    assert.deepEqual(value.sent.at(-1), {kind: 'cancel', id});
    assert.equal(vm.runInContext('responseId', value.context), '');
    assert.equal(value.state.track.enabled, false);
  }
});

test('native取消授权核原Core目标运行态，终态水位和投影与Peer播放分离', () => {
  // 本项是宿主接线源码合同，不冒称真实C++执行；中央构建与核心取消单测另行验证。
  const main = fs.readFileSync(path.join(__dirname, '../adapters/desktop/browser_main.cpp'), 'utf8');
  const dispatch = main.match(/else if \(request.kind == "cancel"\) \{([\s\S]*?)\n            \}/);
  assert.ok(dispatch);
  assert.match(dispatch[1], /!request.id.empty\(\) && request.id == runtime->response_control_target\(\)/);
  assert.doesNotMatch(dispatch[1], /request.id == response\)/, '播放目标不能绕过原核心内容控制资格');
  assert.match(main, /std::string\(runtime->response_control_target\(\)\)/);
  assert.match(main, /if \(state != previous \|\| control_response != previous_control_response\)/);
  assert.match(main, /browser_response_control_state\(message, control_response\)/);
});
test('原WebRTC activate必须装配文件驱动，不能early-return广告空能力', async () => {
  const value = await activatedPage({file: true});
  assert.equal(vm.runInContext('terminated', value.context), false);
  assert.equal(value.effects.file, 1);
  assert.equal(value.sent.filter(message => message.kind === 'ready').length, 1);
});
test('原WebRTC activate必须完成实时节目预检以后才报告ready', async () => {
  const value = await activatedPage({live: true});
  assert.equal(vm.runInContext('terminated', value.context), false);
  assert.equal(value.effects.liveActivated, 1);
  assert.equal(value.sent.filter(message => message.kind === 'ready').length, 1);
});

// 临时ICE断连仅报告原Peer事实；平台ACK以前不能换Peer或退出共享宿主。
test('WebRTC暂断保持同宿主并关闭旧AI意图等待核心恢复命令', async () => {
  const value = await activatedPage({live: true});
  const peer = vm.runInContext('rtcPeer', value.context);
  value.state.track.enabled = true;
  peer.connectionState = 'connected'; peer.onconnectionstatechange();
  peer.connectionState = 'disconnected'; peer.onconnectionstatechange();
  assert.equal(vm.runInContext('terminated', value.context), false);
  assert.equal(value.state.track.enabled, false);
  assert.equal(vm.runInContext('rtcPeer', value.context), peer);
  assert.deepEqual(value.sent.filter(message => message.kind.startsWith('webrtc')), [
    {kind: 'webrtcConnected', peerRevision: '1'},
    {kind: 'webrtcDisconnected', peerRevision: '1'}]);
  assert.equal(value.effects.liveActivated, 1);
});

// 新Peer即使原物理focus仍为true也不继承采集/回复；旧state回调不能作用新代次。
test('WebRTC原恢复命令只换Peer且旧回调不能关闭后继Peer', async () => {
  const value = await activatedPage({live: true});
  const peer = vm.runInContext('rtcPeer', value.context), oldState = peer.onconnectionstatechange;
  peer.createOffer = async () => ({sdp: 'v=0\r\no=old\r\n'});
  value.context.RTCPeerConnection.prototype.createOffer = async () => ({sdp: 'v=0\r\no=new\r\n'});
  value.context.RTCPeerConnection.prototype.setLocalDescription = async function(offer) {
    this.localDescription = offer; this.iceGatheringState = 'complete';
  };
  peer.connectionState = 'connected'; oldState();
  peer.connectionState = 'disconnected'; oldState();
  value.context.incoming({kind: 'webrtcRecover', peerRevision: '2'});
  await new Promise(resolve => setImmediate(resolve));
  assert.notEqual(vm.runInContext('rtcPeer', value.context), peer);
  assert.equal(value.state.track.enabled, false);
  assert.equal(vm.runInContext('responseId', value.context), '');
  peer.connectionState = 'failed'; oldState();
  assert.equal(vm.runInContext('terminated', value.context), false);
  assert.equal(value.effects.liveActivated, 1);
  assert.deepEqual(value.sent.filter(message => message.kind === 'webrtcOffer'), [
    {kind: 'webrtcOffer', peerRevision: '2', sdp: 'v=0\r\no=new\r\n'}]);
});

// 原在途false与Volume是物理事实；恢复不能重建节点或清掉已交付票据。
test('WebRTC恢复保留原末端票据和音量但不继承新回复发声意图', async () => {
  const value = await activatedPage();
  // 使用原可信focus页面，复用激活HAL但不把测试选项当真实平台授权。
  const focused = page(true), gate = output(focused);
  focused.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  focused.context.incoming({kind: 'webrtcVolume', id: 'volume', volume: 40});
  focused.context.RTCPeerConnection = value.context.RTCPeerConnection;
  focused.context.setInterval = () => 1; focused.context.clearInterval = () => {};
  focused.context.RTCPeerConnection.prototype.createOffer = async () => ({sdp: 'v=0\r\no=recovery\r\n'});
  focused.context.RTCPeerConnection.prototype.setLocalDescription = async function(description) {
    this.localDescription = description; this.iceGatheringState = 'complete';
  };
  vm.runInContext('rtcDisconnected=true;', focused.context);
  focused.context.incoming({kind: 'webrtcRecover', peerRevision: '2'});
  gate.actual('1', false);
  assert.equal(vm.runInContext('rtcVolume', focused.context), 40);
  assert.equal(vm.runInContext('rtcGateKnown', focused.context), true);
  assert.equal(vm.runInContext('responseId', focused.context), '');
  assert.equal(focused.state.track.enabled, false);
  await new Promise(resolve => setImmediate(resolve));
  focused.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'old', epoch: '9'});
  focused.context.incoming({kind: 'captureStart', peerRevision: '1', id: 'old-input'});
  focused.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  assert.equal(vm.runInContext('responseId', focused.context), '');
  assert.equal(focused.state.track.enabled, false);
  assert.equal(vm.runInContext('terminated', focused.context), false);
});
test('WebRTC代次字段非法未来或未暂断的恢复不得改变当前Peer', () => {
  const value = page(), original = value.context.__testPeer;
  for (const revision of [undefined, 1, '0', '01', '18446744073709551616', '3']) {
    assert.throws(() => value.context.incoming({kind: 'captureStart', peerRevision: revision, id: 'i'}), /invalid_peer_revision/);
  }
  assert.throws(() => value.context.incoming({kind: 'webrtcRecover', peerRevision: '2'}), /invalid_peer_recovery/);
  assert.equal(vm.runInContext('rtcPeer', value.context), original);
  assert.equal(value.state.track.enabled, false);
});
// 退休立即注销原ICE监听和计时器；旧候选微任务不发送Offer也不退出仍活节目。
test('WebRTC关闭原Peer立即取消原ICE等待并丢弃迟到候选', async () => {
  const value = page(), peer = value.context.__testPeer;
  let listener, timer, removed = 0, canceled = 0;
  peer.createOffer = async () => ({sdp: 'v=0\r\no=old\r\n'});
  peer.setLocalDescription = async function(description) { this.localDescription = description; };
  peer.addEventListener = (_, callback) => { listener = callback; };
  peer.removeEventListener = (_, callback) => { if (callback === listener) ++removed; };
  value.context.setTimeout = callback => { timer = callback; return 1; };
  value.context.clearTimeout = () => ++canceled;
  value.context.incoming({kind: 'webrtcConfigure', peerRevision: '1'});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(typeof listener, 'function');
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  assert.equal(removed, 1); assert.equal(canceled, 1);
  peer.iceGatheringState = 'complete'; listener(); timer();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(value.sent.length, 0);
  assert.equal(vm.runInContext('terminated', value.context), false);
});
// 真实失败不是暂断，不把永久错误无限转成Peer恢复循环。
test('WebRTC当前Peer永久failed仍结束宿主', async () => {
  const value = await activatedPage();
  const peer = vm.runInContext('rtcPeer', value.context);
  peer.connectionState = 'failed'; peer.onconnectionstatechange();
  assert.equal(vm.runInContext('terminated', value.context), true);
});

// remoteGraph仅提供浏览器图构造HAL；被测函数仍为正式rtcAttachRemote，构造不模拟网络连通。
function remoteGraph(value) {
  const {context} = value;
  const graph = {connect() {}, disconnect() {}};
  const gain = {gain: {value: 0}, connect() {}, disconnect() {}};
  const sink = {muted: false, srcObject: null, play: async () => {}, pause() {}};
  context.document.createElement = () => sink;
  context.MediaStream = class { constructor(tracks) { this.tracks = tracks; } };
  context.__remoteAudio = {destination: graph, createMediaStreamSource: () => graph,
    createAnalyser: () => ({fftSize: 1024, connect() {}, disconnect() {}}),
    createGain: () => gain, close: async () => {}};
  vm.runInContext('context=__remoteAudio;rtcAnalyser=null;rtcGain=null;', context);
  return {gain, graph, sink};
}
test('原回复开始先到远端track后到时须保留当前Peer的既有回复和音量', () => {
  const value = page(), graph = remoteGraph(value);
  value.context.incoming({kind: 'webrtcVolume', id: 'original-volume', volume: 47});
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.rtcAttachRemote({track: {kind: 'audio'}});
  assert.equal(graph.gain.gain.value, .47, '原代码总置0会丢掉已接纳的当前回复意图');
  value.context.incoming({kind: 'playbackStop', peerRevision: '1', id: 'original-response', epoch: '1'});
  assert.equal(graph.gain.gain.value, 0);
});
test('原回复已取消而track后到时不能复活旧回复的增益', () => {
  const value = page(), graph = remoteGraph(value);
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.incoming({kind: 'playbackStop', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.rtcAttachRemote({track: {kind: 'audio'}});
  assert.equal(graph.gain.gain.value, 0);
});

test('原Peer退休后的迟到track不得创建新解码图或恢复取消意图', () => {
  const value = page(), graph = remoteGraph(value), originalPeer = value.context.__testPeer;
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.incoming({kind: 'webrtcStop', peerRevision: '1'});
  value.context.rtcAttachRemote({track: {kind: 'audio'}}, originalPeer);
  assert.equal(graph.sink.srcObject, null);
  assert.equal(vm.runInContext('rtcGain', value.context), null);
  assert.equal(vm.runInContext('terminated', value.context), false);
});
test('当前track恢复Gain不能绕过原末端真实音频块false和在途焦点', () => {
  const value = page(true), graph = remoteGraph(value);
  const audioThread = vm.createContext({sampleRate: 48000, Float32Array,
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage() {}}; } },
    registerProcessor(_, Processor) { audioThread.Processor = Processor; }});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/call-worklet.js'), 'utf8'), audioThread);
  const processor = new audioThread.Processor(), node = {port: {close() {}}, disconnect() {}};
  node.port.postMessage = command => processor.port.onmessage({data: command});
  processor.port.postMessage = fact => value.context.rtcOutputActual(node, fact);
  value.context.__actualOutput = node;
  vm.runInContext('rtcOutputNode=__actualOutput;', value.context);
  value.context.incoming({kind: 'playbackStart', peerRevision: '1', id: 'original-response', epoch: '1'});
  value.context.rtcAttachRemote({track: {kind: 'audio'}});
  assert.equal(graph.gain.gain.value, 1, '恢复原回复意图不等于输出许可');
  const input = new Float32Array(128).fill(.02), actual = new Float32Array(128);
  processor.process([[input]], [[actual]]);
  assert.equal(actual.every(sample => sample === 0), true, '未确认任何focus时实际DSP保持零块');
  value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
  assert.equal(value.sent.length, 0, '命令受理不能伪造原实际块ACK');
  processor.process([[input]], [[actual]]);
  assert.equal(actual.every(sample => sample === 0), true);
  assert.equal(value.sent.filter(fact => fact.kind === 'outputGated').length, 1);
  value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: true});
  assert.equal(value.sent.filter(fact => fact.kind === 'outputGated').length, 1);
  processor.process([[input]], [[actual]]);
  assert.equal(actual.some(sample => sample !== 0), true);
  assert.equal(value.sent.filter(fact => fact.kind === 'outputGated').length, 2);
  value.context.incoming({kind: 'playbackGate', ticket: '3', enabled: false});
  processor.process([[input]], [[actual]]);
  assert.equal(actual.every(sample => sample === 0), true, '原回复仍存在也不能绕过后继释放');
});

// nativeFocusAssets只解释正式native固定注入，不自行设置输出焦点；其它媒体资产不在该边界模拟。
function nativeFocusAssets(transport, focus, diagnostic) {
  const main = fs.readFileSync(path.join(__dirname, '../adapters/desktop/browser_main.cpp'), 'utf8');
  assert.ok(main.includes('const bool use_webrtc = media_setting == "webrtc";'));
  assert.ok(main.includes('const bool use_focus = focus_setting == "voice" || use_files || use_live;'));
  assert.ok(main.includes('const bool use_files = focus_setting == "media";'));
  assert.ok(main.includes('const bool use_live = focus_setting == "live";'));
  const assets = {'/app.js': source,
    '/index.html': fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/index.html'), 'utf8'),
    '/client-theme.js': fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/client-theme.js'), 'utf8')};
  const conditions = [
    [/if \(env\("UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS"\) == "1"\) \{([\s\S]*?)\n    \}/, diagnostic === '1'],
    [/if \(use_webrtc && \(use_focus \|\| endpoint_listen\)\) \{([\s\S]*?)\n    \}/, transport === 'webrtc' && ['voice', 'media', 'live'].includes(focus)]];
  for (const [expression, enabled] of conditions) {
    const block = main.match(expression); assert.ok(block);
    if (!enabled) continue;
    const insertion = block[1].match(/assets\.at\("(\/index\.html|\/app\.js)"\)\s*\.insert\((marker|0),\s*("(?:[^"\\]|\\.)*")\);/);
    assert.ok(insertion, '只允许正式固定资产与字面量');
    const [, asset, position, literal] = insertion;
    const offset = position === '0' ? 0 : assets[asset].indexOf('  <script src="/app.js"');
    assert.ok(offset >= 0);
    assets[asset] = assets[asset].slice(0, offset) + JSON.parse(literal) + assets[asset].slice(offset);
  }
  return assets;
}

// nativeFocusAssembly按原self策略加载同源脚本，以独立真实Worklet源码确认实际音频处理块。
async function nativeFocusAssembly(transport, focus, diagnostic) {
  const assets = nativeFocusAssets(transport, focus, diagnostic), scripts = [];
  for (const script of assets['/index.html'].matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/g)) {
    const url = script[1].match(/\bsrc="([^"]+)"/);
    if (url) { assert.ok(Object.hasOwn(assets, url[1])); scripts.push(assets[url[1]]); }
    // 原CSP不准inline；该加载策略模型不冒充Chrome运行。
  }
  const value = page(false, transport, scripts), {context} = value;
  const toAudio = [], toPage = [], modules = [];
  let processor, node, Processor;
  const workletContext = vm.createContext({sampleRate: 48000,
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage: data => toPage.push(data)}; } },
    registerProcessor(name, klass) { assert.equal(name, 'ur-call-audio'); Processor = klass; }});
  const graph = {connect() {}, disconnect() {}};
  context.__nativeAudio = {destination: graph,
    audioWorklet: {async addModule(url) {
      modules.push(url); assert.equal(url, '/call-worklet.js');
      vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/call-worklet.js'), 'utf8'), workletContext);
    }}, createAnalyser: () => ({...graph, fftSize: 0}), close: async () => {}};
  context.AudioWorkletNode = class {
    constructor(audio, name) {
      assert.equal(audio, context.__nativeAudio); assert.equal(name, 'ur-call-audio');
      processor = new Processor(); node = this;
      this.port = {postMessage: data => toAudio.push(data), close() {}};
    }
    connect() {} disconnect() {}
  };
  vm.runInContext('context=__nativeAudio;', context);
  await context.initializeRTCOutput();
  // 两个原端口异步交付；命令入队和回执入队都不等于页面已确认。
  const flush = async () => {
    await new Promise(resolve => setImmediate(resolve));
    while (toAudio.length) processor.port.onmessage({data: toAudio.shift()});
    while (toPage.length) node.port.onmessage({data: toPage.shift()});
  };
  const render = (empty = false) => {
    const output = new Float32Array(128);
    if (processor) processor.process([[new Float32Array(128).fill(.25)]], [empty ? [] : [output]]);
    return output;
  };
  return {...value, assets, modules, flush, render,
    hasNode: () => !!node,
    diagnostic: () => JSON.parse(vm.runInContext('JSON.stringify(UrReadAudioFailureDiagnostic())', context))};
}

test('native输出焦点CSP业务组合：voice/media/live装配原Worklet，实际块后才ACK', async () => {
  for (const focus of ['voice', 'media', 'live']) {
    const value = await nativeFocusAssembly('webrtc', focus, '');
    assert.doesNotThrow(() => value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false}),
      '旧inline被原CSP挡住会跳过节点并拒绝playbackGate');
    assert.equal(value.hasNode(), true); assert.deepEqual(value.modules, ['/call-worklet.js']);
    assert.equal(value.sent.length, 0); await value.flush();
    value.render(true); await value.flush(); assert.equal(value.sent.length, 0, '空输出不能确认');
    assert.ok(value.render().every(sample => sample === 0));
    assert.equal(value.sent.length, 0, '真实块回执入队后还必须原页面收件'); await value.flush();
    assert.deepEqual(value.sent, [{kind: 'outputGated', ticket: '1', enabled: false}]);
    value.context.incoming({kind: 'playbackGate', ticket: '2', enabled: true}); await value.flush();
    assert.ok(value.render().every(sample => sample === .25)); await value.flush();
    assert.deepEqual(value.sent.at(-1), {kind: 'outputGated', ticket: '2', enabled: true});
    value.render(); await value.flush(); assert.equal(value.sent.length, 2, '重复真实块不重发门控ACK');
    assert.throws(() => value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false}), /invalid_rtc_output_gate/);
    value.context.release('固定测试收尾');
  }
});

test('native输出焦点CSP业务组合：原env条件/defaultoff与诊断共开均保留', async () => {
  for (const [transport, focus] of [['udp', 'voice'], ['udp', 'media'], ['udp', 'live'], ['webrtc', ''], ['', '']]) {
    const value = await nativeFocusAssembly(transport, focus, '');
    assert.equal(value.assets['/app.js'], source); assert.equal(value.hasNode(), false);
    assert.deepEqual(value.modules, []); assert.equal(value.diagnostic(), null);
  }
  for (const diagnostic of ['', '0', 'true', '01', '1 ', '1']) {
    const value = await nativeFocusAssembly('webrtc', 'voice', diagnostic);
    assert.equal(value.hasNode(), true);
    assert.deepEqual(value.diagnostic(), diagnostic === '1' ? {known: false, cause: 'unknown'} : null);
    assert.equal(value.assets['/index.html'], nativeFocusAssets('', '', '')['/index.html']);
    assert.equal(value.assets['/app.js'], "'use strict';globalThis.UrWebRTCOutputFocus=true;\n" +
      (diagnostic === '1' ? "'use strict';globalThis.UrAudioFailureDiagnostics=true;\n" : '') + source);
    assert.throws(() => page(false, 'webrtc', [value.assets['/app.js'] + '\nnativeFocusUndeclaredWrite = 1;']),
      /nativeFocusUndeclaredWrite/, '单开或共开前缀均不得丢失原strict语义');
    value.context.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
    await value.flush(); value.render(); await value.flush();
    assert.deepEqual(value.sent, [{kind: 'outputGated', ticket: '1', enabled: false}]);
    value.context.release('固定测试收尾');
  }
});
