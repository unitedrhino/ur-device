// 文件说明：显式IPC真实后端E2E；三端检查H264画面及可选Opus解码能量、独立停流，不以握手代替。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn, execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');

// browser 只操作本次生成的独立session，不访问共享浏览器或其他测试页面。
function browser(session, env, ...args) {
  return new Promise((resolve, reject) => execFile('agent-browser', ['--session', session, ...args],
    {env, timeout: args[0] === 'wait' ? 40000 : 30000, maxBuffer: 262144},
    (error, stdout) => error ? reject(Error(`browser_${args[0]}_failed`)) : resolve(stdout.trim())));
}
// evaluated 解包CLI输出，不在报告里保存SDP或桥认证令牌。
function evaluated(value) {
  let result = JSON.parse(value);
  while (typeof result === 'string') {
    try { result = JSON.parse(result); } catch { break; }
  }
  return result;
}
// admissionMode 只接受测试明确选定的两种平台策略；未知值不得静默自动接听。
function admissionMode(value) {
  if (value === undefined) return 'direct';
  assert.ok(value === 'direct' || value === 'accept', 'invalid_admission_mode');
  return value;
}
// listeningMode只接受显式0/1测试开关，避免误配置跳过监听验收。
function listeningMode(value) {
  assert.ok(value === undefined || value === '0' || value === '1', 'invalid_listening_mode');
  return value === '1';
}

// sourceControlResult 只接纳助手最后的明确受理标记，不把初始化日志或任意truthy值当成功。
function sourceControlResult(stdout) {
  const line = stdout.trim().split('\n').at(-1);
  const result = JSON.parse(line);
  assert.deepEqual(result, {accepted: true}, 'source_rpc_acceptance_required');
}

// sourceControlRPC 通过正式后端RPC助手受理，命令由stdin传递，不使用shell或直接MQTT注入。
function sourceControlRPC(binary, env, request) {
  return new Promise((resolve, reject) => {
    const child = execFile(binary, [], {env, timeout: 15000, maxBuffer: 65536}, (error, stdout, stderr) => {
      if (error) return reject(Error(sourceControlFailure(stderr)));
      try { sourceControlResult(stdout); resolve(); } catch { reject(Error('source_control_rpc_invalid_result')); }
    });
    child.stdin.on('error', () => {}); // 子进程先失败时仍由退出回调返回错误，不留下未处理EPIPE。
    child.stdin.end(JSON.stringify(request));
  });
}
// sourceControlFailure 只提取助手明确定义的RPC枚举，任意子进程正文不得进入报告。
function sourceControlFailure(stderr) {
  const match = /^ipc_source_control_failed_rpc_(0|[1-9]|1[0-6])\s*$/.exec(stderr.trim());
  return match ? `source_control_rpc_rejected_${match[1]}` : 'source_control_rpc_rejected';
}
// attachListeningAudio在观看浏览器运行；静音媒体元素激活Chromium解码，零增益图只观察PCM。
// 函数不捕获模块环境，可注入真实页面并以可控驱动独立验证生命周期。
function attachListeningAudio(track) {
  const sink = document.createElement('audio'); // 负责启动远端解码，不直接向扬声器发声。
  sink.muted = true; sink.autoplay = true; sink.playsInline = true;
  sink.srcObject = new MediaStream([track]);
  const playing = sink.play(); // 先激活媒体元素，再接入WebAudio，避免收包但不解码。
  const context = new AudioContext({sampleRate:48000}); // 接收端解码观察时钟，不冒充设备输入配置。
  const analyser = context.createAnalyser(); analyser.fftSize = 512;
  const source = context.createMediaStreamSource(new MediaStream([track])); // 保留强引用直到轨释放。
  const gain = context.createGain(); gain.gain.value = 0;
  source.connect(analyser); analyser.connect(gain); gain.connect(context.destination);
  const state = window.__ipcAudio = {context,analyser,source,gain,sink,peak:0,run:0,maxRun:0,closed:false,failed:false};
  const samples = new Float32Array(512); // 固定采样缓冲，不随网络包无限增长。
  state.timer = setInterval(() => {
    analyser.getFloatTimeDomainData(samples);
    let sum = 0; // RMS仅来自解码PCM，不能由RTP数量或设备状态推断。
    for (const value of samples) sum += value * value;
    const rms = Math.sqrt(sum / samples.length);
    state.peak = Math.max(state.peak,rms); state.run = rms > 0.001 ? state.run + 1 : 0;
    state.maxRun = Math.max(state.maxRun,state.run);
  },50);
  // stop精确回收当前接收轨的分析资源；重复终态不重复释放或产生第二条音路。
  state.stop = () => {
    if (state.closed) return state.closing;
    state.closed = true; clearInterval(state.timer);
    source.disconnect(); gain.disconnect(); sink.pause(); sink.srcObject = null; sink.remove();
    state.closing = context.close();
    return state.closing;
  };
  track.onended = state.stop;
  playing.catch(() => {state.failed = true; state.stop();});
  context.resume().catch(() => {state.failed = true; state.stop();});
}
// confirmAdmission 在设备确认场景先验证没有对应采集或Peer，再点击当前真实邀请控件。
// execute为当前设备浏览器的命令适配，返回CLI原始结果；不模拟云端sessionAccept。
async function confirmAdmission(mode, sessionId, execute, purpose = 'ipcLiveView') {
  assert.ok(mode === 'direct' || mode === 'accept', 'invalid_admission_mode');
  assert.ok(purpose === 'ipcLiveView' || purpose === 'ipcTalk', 'invalid_consent_purpose');
  assert.ok(typeof sessionId === 'string' && sessionId.length > 0 && sessionId.length <= 64, 'invalid_session');
  if (mode === 'direct') return;
  const session = JSON.stringify(sessionId);
  try { await execute('wait', '--fn', `invitations.has(${session})`); }
  catch { throw Error('local_invitation_not_received'); }
  const captured = evaluated(await execute('eval', `(()=>{const button=invitations.get(${session});const bounds=button?.getBoundingClientRect();return JSON.stringify({pending:!!button,captured:views.has(${session}),buttonTop:bounds?.top,buttonBottom:bounds?.bottom,viewportHeight:innerHeight});})()`));
  assert.equal(captured.pending, true, 'local_invitation_required');
  assert.equal(captured.captured, false, 'capture_before_consent');
  // 多路预览使新按钮位于视口外；仅滚动对应控件，不用脚本click或伪造确认消息。
  await execute('eval', `(()=>{invitations.get(${session}).scrollIntoView({block:'center'});return true;})()`);
  await execute('snapshot', '-i');
  await execute('find', 'role', 'button', 'click', '--name', purpose === 'ipcTalk' ? '接受本次对讲' : '接受本次观看');
  // 点击命令成功不等于控件事件执行，必须观察对应邀请实际被移除。
  await execute('wait', '--fn', `!invitations.has(${session})`);
  return captured;
}
// safetyTerminal 只识别原观看腿的正式主动安全终态，不把HTTP受理或客户端主动挂断当成撤权事实。
function safetyTerminal(event, sessionId, cause) {
  return ['privacyEnabled', 'permissionRevoked'].includes(cause) && !!event &&
    event.method === 'interaction.state' && event.sessionId === sessionId &&
    typeof event.msgToken === 'string' && event.msgToken.length > 0 &&
    !Object.hasOwn(event, 'code') && Number.isSafeInteger(event.eventSeq) && event.eventSeq > 0 &&
    event.data?.state === 'ended' && event.data.reason === cause;
}
// run 只允许显式启动和已授权本机服务；凭据由环境注入，断言失败也不输出原响应。
// options.onEstablished仅供显式中央验收追加实测，收到所有真实建连事实后执行，不生成平台应答。
// options.safety.trigger只触发正式DMS操作；实际停止与原持久终态须由本验收器独立读取。
async function run(options = {}) {
  assert.equal(process.env.UR_IPC_LIVE, '1', 'explicit_live_switch_required');
  if (options.safety !== undefined) {
    assert.deepEqual(Object.keys(options.safety).sort(), options.safety.restore === undefined
      ? ['cause', 'trigger'] : ['cause', 'restore', 'trigger'], 'invalid_safety_probe');
    assert.ok(['privacyEnabled', 'permissionRevoked'].includes(options.safety.cause), 'invalid_safety_cause');
    assert.equal(typeof options.safety.trigger, 'function', 'invalid_safety_trigger');
    if (options.safety.restore !== undefined) assert.equal(typeof options.safety.restore, 'function', 'invalid_safety_restore');
  }
  const admission = admissionMode(process.env.UR_IPC_ADMISSION);
  const listening = listeningMode(process.env.UR_IPC_LISTEN_AUDIO);
  const trackControls = listeningMode(process.env.UR_IPC_TRACK_CONTROL); // 显式轨验收不得因配置错误退回只建连。
  const sourceControls = listeningMode(process.env.UR_IPC_SOURCE_CONTROL);
  const sourceBinary = process.env.UR_IPC_SOURCE_CONTROL_BIN;
  if (sourceControls) {
    assert.ok(listening, 'source_audio_scenario_required');
    assert.ok(sourceBinary && fs.statSync(sourceBinary).isFile(), 'source_rpc_helper_required');
  }
  assert.ok(!trackControls || listening,'track_audio_scenario_required');
  const binary = process.env.UR_IPC_DEVICE_BIN;
  const base = process.env.UR_IPC_BASE_URL;
  const token = process.env.UR_IPC_APP_TOKEN;
  const port = Number(process.env.UR_IPC_DEVICE_PORT);
  assert.ok(binary && fs.statSync(binary).isFile(), 'device_binary_required');
  assert.equal(new URL(base).hostname, '127.0.0.1', 'local_service_required');
  assert.equal(new URL(base).protocol, 'http:', 'local_http_required');
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535, 'local_port_required');
  assert.ok(token && process.env.UR_DEVICE_PRODUCT && process.env.UR_DEVICE_NAME && process.env.UR_DEVICE_SECRET,
    'injected_credentials_required');
  if (listening) {
    assert.equal(process.env.UR_IPC_MICROPHONE, '1', 'explicit_microphone_required');
    assert.ok(process.env.UR_IPC_AUDIO_FIXTURE && fs.statSync(process.env.UR_IPC_AUDIO_FIXTURE).isFile(), 'audio_fixture_required');
  }
  // api 保留成功业务数据用于关联，不将第三方错误正文反射到日志。
  async function api(route, body) {
    const response = await fetch(`${base}/api/v1/im/interaction/${route}`, {
      method: 'POST', headers: {'content-type': 'application/json', 'app-id': process.env.UR_IPC_APP_ID || '100',
        'tenant-code': process.env.UR_IPC_TENANT || 'platform', token},
      body: JSON.stringify(body), signal: AbortSignal.timeout(20000)});
    const result = await response.json();
    if (response.status !== 200 || result.code !== 200) throw Error(`api_${route.replaceAll('/', '_')}_rejected`);
    return result.data;
  }
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required',
    ...(listening ? [`--use-file-for-fake-audio-capture=${process.env.UR_IPC_AUDIO_FIXTURE}`] : [])].join(',')};
  const deviceBrowser = `ipc-device-${randomUUID()}`;
  const sessions = [deviceBrowser];
  const viewers = [];
  const server = http.createServer((_, response) => {
    response.writeHead(200, {'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store'});
    response.end('<!doctype html><html><body><video id="remote" autoplay playsinline muted></video></body></html>');
  });
  let child;
  let completion;
  let childExited = false;
  let childExitCode = null; // 真实close退出码，SIGTERM受理或Peer停止不能替代正常设备退出。
  let outcome; // 所有清理屏障完成后才打印最终结果，不能提前输出媒体PASS。
  const cleanupFailures = []; // 仅保留固定资源分类，不输出会话或底层错误。
  let deadline;
  let stage = 'start';
  let currentViewer; // 仅用于失败时的脱敏本地状态，不输出身份或SDP。
  const acceptFacts = {accepted: 0, rejected: 0}; // 只计数C++固定诊断，不保留进程自由日志。
  let consentFacts; // 授权点击前的纯布局和采集布尔量，不包含会话标识。
  // pollResult 先处理/确认每条实际投递，只关联本端原token；终态迟到不能覆盖当前请求结果。
  async function pollResult(viewer, msgToken) {
    const end = Date.now() + 20000;
    while (Date.now() < end) {
      const received = await api('events', {endpointId: viewer.endpointId, limit: 32});
      let found;
      for (const delivery of received.list || []) {
        const event = delivery.event;
        assert.ok(event && typeof event === 'object' && typeof delivery.deliveryId === 'string');
        if (event.method === 'interaction.state') viewer.state = event.data?.state;
        if (event.method === 'interaction.result' && event.msgToken === msgToken) found = event;
      }
      if (received.list?.length) await api('events/ack', {endpointId: viewer.endpointId,
        deliveryIds: received.list.map(value => value.deliveryId)});
      if (found) { assert.equal(found.code, 200, 'operation_result_rejected'); return found; }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw Error('operation_result_timeout');
  }
  // command 每个独立操作生成新token，HTTP accepted与最终结果分别校验。
  async function command(viewer, method, params, sessionId) {
    const msgToken = randomUUID();
    const payload = {msgToken, method, params};
    if (sessionId) payload.sessionId = sessionId;
    else payload.target = {productID: process.env.UR_DEVICE_PRODUCT, deviceName: process.env.UR_DEVICE_NAME};
    const result = await api('commands', {endpointId: viewer.endpointId, command: payload});
    assert.equal(result.accepted, true); assert.equal(result.msgToken, msgToken);
    return pollResult(viewer, msgToken);
  }
  // metrics 必须从浏览器的实际inbound解码及已呈现像素读取，不由MQTT状态推断。
  async function metrics(viewer) {
    return evaluated(await browser(viewer.browser, env, 'eval', `(async()=>{
      let frames=0,packets=0,audioPackets=0,audioSamples=0; const peer=window.__ipcPeer;
      if(peer)(await peer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video'){frames+=s.framesDecoded||0;packets+=s.packetsReceived||0;}
        if(s.type==='inbound-rtp'&&s.kind==='audio'){audioPackets+=s.packetsReceived||0;audioSamples+=s.totalSamplesReceived||0;}});
      const video=document.getElementById('remote');let pixels=0;
      if(video.videoWidth&&video.readyState>=2){const canvas=document.createElement('canvas');canvas.width=16;canvas.height=16;
        const context=canvas.getContext('2d');context.drawImage(video,0,0,16,16);const bytes=context.getImageData(0,0,16,16).data;
        for(let index=0;index<bytes.length;index+=4)pixels+=bytes[index]+bytes[index+1]+bytes[index+2];}
      const audio=window.__ipcAudio;return JSON.stringify({frames,packets,pixels,width:video.videoWidth,connected:peer?.connectionState==='connected',
        audioPackets,audioSamples,audioPeak:audio?.peak||0,audibleRun:audio?.maxRun||0,audioContext:audio?.context.state||'absent'});
    })()`));
  }
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    child = spawn(binary, [String(port)], {env: process.env, stdio: ['ignore', 'pipe', 'pipe']});
    let diagnostic = '';
    child.stdout.on('data', bytes => {
      diagnostic = (diagnostic + bytes.toString()).slice(-4096);
      const lines = diagnostic.split('\n'); diagnostic = lines.pop();
      for (const line of lines) {
        if (line === 'ipc_local_accept_ok') acceptFacts.accepted++;
        if (line === 'ipc_local_accept_rejected') acceptFacts.rejected++;
      }
    });
    // 不把进程自由日志转发到报告；所有失败只保留本测试固定阶段名。
    child.stderr.on('data', () => {});
    completion = new Promise(resolve => child.once('close', code => { childExited = true; childExitCode = code; resolve(); }));
    deadline = setTimeout(() => child.kill('SIGTERM'), 180000);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timer); reject(Error('device_spawn_failed')); });
      child.stdout.on('data', bytes => {
        if (bytes.toString().includes(`http://127.0.0.1:${port}/`)) { clearTimeout(timer); resolve(); }
      });
    });
    stage = 'device_online';
    await browser(deviceBrowser, env, 'open', `http://127.0.0.1:${port}/`);
    await browser(deviceBrowser, env, 'snapshot', '-i');
    await browser(deviceBrowser, env, 'find', 'role', 'button', 'click', '--name', '连接平台');
    await browser(deviceBrowser, env, 'wait', '--fn', 'available === true && closed === false');
    if (options.onReady !== undefined) {
      stage = 'device_property_baseline';
      assert.equal(typeof options.onReady, 'function', 'invalid_ready_probe');
      await options.onReady({deviceBrowser, env}); // 物模型基线经原C++上报建立，不能用SQL绕过准入。
    }
    // 提前创建全部应用Peer，避免浏览器启动占用邀请有效期。
    for (let index = 0; index < 3; ++index) {
      const endpoint = await api('endpoints/register', {});
      assert.ok(endpoint.endpointId);
      const viewer = {endpointId: endpoint.endpointId, browser: `ipc-viewer-${randomUUID()}`, listening: listening && index > 0};
      viewers.push(viewer); sessions.push(viewer.browser);
      await browser(viewer.browser, env, 'open', `http://127.0.0.1:${server.address().port}/`);
      viewer.offer = evaluated(await browser(viewer.browser, env, 'eval', `(async()=>{
        const peer=new RTCPeerConnection({iceServers:[]});window.__ipcPeer=peer;
        peer.ontrack=e=>{if(e.track.kind==='video'){document.getElementById('remote').srcObject=new MediaStream([e.track]);return;}
          (${attachListeningAudio.toString()})(e.track);};
        const track=peer.addTransceiver('video',{direction:'recvonly'});
        const codecs=RTCRtpReceiver.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/h264'&&(c.sdpFmtpLine||'').includes('packetization-mode=1'));
        if(!codecs.length)throw Error('h264_required');track.setCodecPreferences(codecs);
        if(${viewer.listening}){const audio=peer.addTransceiver('audio',{direction:'recvonly'});
          const opus=RTCRtpReceiver.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()==='audio/opus');
          if(!opus.length)throw Error('opus_required');audio.setCodecPreferences(opus);}
        await peer.setLocalDescription(await peer.createOffer());
        if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
          const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
          peer.addEventListener('icegatheringstatechange',check);check();});
        return JSON.stringify(peer.localDescription.sdp);
      })()`));
      assert.ok(typeof viewer.offer === 'string' && viewer.offer.startsWith('v=0\r\n'));
    }
    for (const viewer of viewers) {
      currentViewer = viewer;
      stage = 'open';
      const opened = await command(viewer, 'interaction.open', {purpose: 'ipcLiveView', cameraId: 'front',
        ...(viewer.listening ? {listenAudio:true} : {})});
      viewer.sessionId = opened.sessionId;
      assert.ok(viewer.sessionId && opened.data?.sessionId === viewer.sessionId);
      stage = admission === 'accept' ? 'local_consent' : 'open';
      consentFacts = await confirmAdmission(admission, viewer.sessionId, (...args) => browser(deviceBrowser, env, ...args));
      stage = 'device_media_start';
      await browser(deviceBrowser, env, 'wait', '--fn', `!!views.get(${JSON.stringify(viewer.sessionId)})?.peer?.localDescription`);
      stage = 'negotiate';
      const offered = await command(viewer, 'interaction.mediaOffer', {mediaRevision: 1,
        webrtc: {iceMode: 'complete', sdp: viewer.offer}}, viewer.sessionId);
      assert.equal(offered.data?.mediaRevision, 1); assert.equal(offered.data?.webrtc?.iceMode, 'complete');
      const answer = offered.data.webrtc.sdp;
      assert.ok(typeof answer === 'string' && answer.startsWith('v=0\r\n'));
      await browser(viewer.browser, env, 'eval', `(async()=>{await window.__ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answer)}});return true;})()`);
      await browser(viewer.browser, env, 'wait', '--fn', 'window.__ipcPeer.connectionState === "connected"');
      await command(viewer, 'interaction.mediaState', {mediaRevision: 1, state: 'connected'}, viewer.sessionId);
      stage = 'both_sides_established';
      const establishedDeadline = Date.now() + 15000;
      while (viewer.state !== 'established' && Date.now() < establishedDeadline) {
        const received = await api('events', {endpointId: viewer.endpointId, limit: 32});
        for (const delivery of received.list || []) {
          if (delivery.event?.method === 'interaction.state' && delivery.event.sessionId === viewer.sessionId)
            viewer.state = delivery.event.data?.state;
        }
        if (received.list?.length) await api('events/ack', {endpointId: viewer.endpointId, deliveryIds: received.list.map(value => value.deliveryId)});
        if (viewer.state !== 'established') await new Promise(resolve => setTimeout(resolve, 100));
      }
      assert.equal(viewer.state, 'established', 'application_established_required');
      await browser(deviceBrowser, env, 'wait', '--fn', `views.get(${JSON.stringify(viewer.sessionId)})?.active === true`);
      stage = 'actual_decoded_video';
      await browser(viewer.browser, env, 'wait', '--fn', `(async()=>{let frames=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>=2&&document.getElementById('remote').videoWidth>0;})()`);
      viewer.before = await metrics(viewer);
      assert.ok(viewer.before.frames >= 2 && viewer.before.packets > 0 && viewer.before.pixels > 0);
      stage = 'actual_decoded_audio';
      if (viewer.listening) {
        await browser(viewer.browser, env, 'wait', '--fn', 'window.__ipcAudio?.maxRun >= 2');
        viewer.before = await metrics(viewer);
        assert.ok(viewer.before.audioPackets > 0 && viewer.before.audioSamples > 0 && viewer.before.audioPeak > 0.001 && viewer.before.audibleRun >= 2);
      } else {
        assert.equal(viewer.before.audioPackets, 0);
        const noAudio = evaluated(await browser(viewer.browser, env, 'eval', 'JSON.stringify(!window.__ipcAudio && !window.__ipcPeer.getReceivers().some(r=>r.track?.kind==="audio"))'));
        assert.equal(noAudio,true,'video_only_viewer_must_not_receive_audio');
      }
      const capturedAudio = evaluated(await browser(deviceBrowser, env, 'eval',
        `JSON.stringify(views.get(${JSON.stringify(viewer.sessionId)}).stream.getAudioTracks().length)`));
      assert.equal(capturedAudio,viewer.listening ? 1 : 0,'device_capture_must_match_explicit_invitation');
    }
    if (options.onEstablished !== undefined) {
      assert.equal(typeof options.onEstablished, 'function', 'invalid_established_probe');
      stage = 'quality_actual_backend';
      await options.onEstablished({deviceBrowser, env, viewers, metrics, command});
    }
    if (trackControls) {
      const viewer = viewers[1], other = viewers[2]; // 控制一个观看者，另一个必须继续实际解码。
      currentViewer = viewer;
      // control等待持久原结果，而不是仅判断HTTP accepted。
      async function control(trackId,operation,state) {
        const result = await command(viewer,'interaction.trackControl',{trackId,operation,mediaRevision:1},viewer.sessionId);
        assert.equal(result.data.trackId,trackId);assert.equal(result.data.state,state);assert.equal(result.data.mediaRevision,1);
      }
      stage = 'pause_video_subscription';
      await control('video-1','pause','paused');
      await browser(viewer.browser,env,'wait','500'); // 排空真实网络与解码在途帧，不能把旧帧误认为新转发。
      const paused = await metrics(viewer), otherBefore = await metrics(other);
      await browser(viewer.browser,env,'wait','500');
      const pausedEnd = await metrics(viewer), otherAfter = await metrics(other);
      assert.equal(pausedEnd.packets,paused.packets,'paused_video_must_not_receive_new_rtp');
      assert.equal(pausedEnd.frames,paused.frames,'paused_video_must_not_decode_new_frames');
      assert.ok(pausedEnd.audioSamples>paused.audioSamples,'video_pause_must_keep_audio');
      assert.ok(otherAfter.frames>otherBefore.frames && otherAfter.audioSamples>otherBefore.audioSamples,'other_viewer_must_continue');
      stage = 'resume_video_subscription';
      await control('video-1','resume','active');
      await browser(viewer.browser,env,'wait','--fn',`(async()=>{let frames=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>${pausedEnd.frames}+2;})()`);
      stage = 'unsubscribe_audio';
      await control('audio-1','unsubscribe','stopped');
      await browser(viewer.browser,env,'wait','500');
      const silent = await metrics(viewer);
      await browser(viewer.browser,env,'wait','500');
      const silentEnd = await metrics(viewer);
      assert.equal(silentEnd.audioPackets,silent.audioPackets,'unsubscribed_audio_must_not_receive_new_rtp');
      assert.ok(silentEnd.frames>silent.frames,'audio_unsubscribe_must_keep_video');
      stage = 'resubscribe_audio';
      await browser(viewer.browser,env,'eval','(()=>{window.__ipcAudio.maxRun=0;window.__ipcAudio.run=0;window.__ipcAudio.peak=0;return true;})()');
      await control('audio-1','subscribe','active');
      await browser(viewer.browser,env,'wait','--fn','window.__ipcAudio.maxRun>=2');
      const resumed = await metrics(viewer);
      assert.ok(resumed.audioPackets>silentEnd.audioPackets && resumed.audioPeak>0.001,'resubscription_requires_actual_audible_decode');
    }
    stage = 'independent_hangup';
    if (sourceControls) {
      const viewer = viewers[1], other = viewers[2]; // 平台源控制不能结束其他观看腿或新增未授权音轨。
      currentViewer = viewer;
      const sourceEnv = {...process.env, UR_IPC_TENANT: process.env.UR_IPC_TENANT || 'platform'};
      async function source(trackId, operation, state) {
        const request = {sessionId: viewer.sessionId, trackId, operation, mediaRevision: 1, msgToken: randomUUID()};
        await sourceControlRPC(sourceBinary, sourceEnv, request);
        await browser(deviceBrowser, env, 'wait', '--fn',
          `views.get(${JSON.stringify(viewer.sessionId)})?.sources.get(${JSON.stringify(trackId)})?.state === ${JSON.stringify(state)}`);
        const actual = evaluated(await browser(deviceBrowser, env, 'eval', `(()=>{const item=views.get(${JSON.stringify(viewer.sessionId)}).sources.get(${JSON.stringify(trackId)});
          return JSON.stringify({state:item.state,bound:item.sender.track===item.track,ended:item.track.readyState==='ended'});})()`));
        assert.equal(actual.state, state, 'source_driver_actual_state');
        assert.equal(actual.bound, state === 'active', 'source_sender_binding');
        assert.equal(actual.ended, state === 'stopped', 'source_real_track_release');
        await sourceControlRPC(sourceBinary, sourceEnv, request); // 重试原命令，不用新token伪装幂等。
      }
      stage = 'source_unauthorized_audio';
      await assert.rejects(sourceControlRPC(sourceBinary, sourceEnv,
        {sessionId:viewers[0].sessionId,trackId:'audio-1',operation:'publish',mediaRevision:1,msgToken:randomUUID()}));
      stage = 'source_pause_video';
      await source('video-1','pause','paused');
      await browser(viewer.browser,env,'wait','500'); // 有界排空在途媒体，再统计真正停流窗口。
      const paused = await metrics(viewer), otherBefore = await metrics(other);
      await browser(viewer.browser,env,'wait','500');
      const pausedEnd = await metrics(viewer), otherAfter = await metrics(other);
      assert.equal(pausedEnd.packets,paused.packets,'source_paused_video_must_stop_rtp');
      assert.equal(pausedEnd.frames,paused.frames,'source_paused_video_must_stop_decoding');
      assert.ok(pausedEnd.audioSamples>paused.audioSamples,'source_video_pause_must_keep_audio');
      assert.ok(otherAfter.frames>otherBefore.frames && otherAfter.audioSamples>otherBefore.audioSamples,'source_control_other_viewer_must_continue');
      stage = 'source_resume_video';
      await source('video-1','resume','active');
      await browser(viewer.browser,env,'wait','--fn',`(async()=>{let frames=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>${pausedEnd.frames}+2;})()`);
      stage = 'source_pause_audio';
      await source('audio-1','pause','paused');
      await browser(viewer.browser,env,'wait','500');
      const audioPaused = await metrics(viewer);
      await browser(viewer.browser,env,'wait','500');
      const audioPausedEnd = await metrics(viewer);
      assert.equal(audioPausedEnd.audioPackets,audioPaused.audioPackets,'source_paused_audio_must_stop_rtp');
      assert.ok(audioPausedEnd.frames>audioPaused.frames,'source_audio_pause_must_keep_video');
      stage = 'source_resume_audio';
      await source('audio-1','resume','active');
      await browser(viewer.browser,env,'wait','--fn',`(async()=>{let samples=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio')samples+=s.totalSamplesReceived||0;});return samples>${audioPausedEnd.audioSamples}+960;})()`);
      stage = 'source_stop_audio';
      await source('audio-1','stop','stopped');
      await browser(viewer.browser,env,'wait','500');
      const silent = await metrics(viewer), remainingBefore = await metrics(other);
      await browser(viewer.browser,env,'wait','500');
      const silentEnd = await metrics(viewer), remainingAfter = await metrics(other);
      assert.equal(silentEnd.audioPackets,silent.audioPackets,'source_stopped_audio_must_stop_rtp');
      assert.ok(silentEnd.frames>silent.frames,'source_audio_stop_must_keep_video');
      assert.ok(remainingAfter.audioSamples>remainingBefore.audioSamples,'source_audio_stop_other_viewer_must_continue');
      await browser(viewer.browser,env,'eval','(()=>{window.__ipcAudio.maxRun=0;window.__ipcAudio.run=0;window.__ipcAudio.peak=0;return true;})()');
      stage = 'source_publish_audio';
      await source('audio-1','publish','active');
      await browser(viewer.browser,env,'wait','--fn','window.__ipcAudio.maxRun>=2');
      const freshAudio = await metrics(viewer);
      assert.ok(freshAudio.audioSamples>silentEnd.audioSamples && freshAudio.audioPeak>0.001,'source_publish_requires_fresh_non_silent_decoding');
      stage = 'source_stop_video';
      await source('video-1','stop','stopped');
      await browser(viewer.browser,env,'wait','500');
      const stopped = await metrics(viewer);
      await browser(viewer.browser,env,'wait','500');
      const stoppedEnd = await metrics(viewer);
      assert.equal(stoppedEnd.packets,stopped.packets,'source_stopped_video_must_stop_rtp');
      assert.ok(stoppedEnd.audioSamples>stopped.audioSamples,'source_video_stop_must_keep_audio');
      stage = 'source_publish_video';
      await source('video-1','publish','active');
      await browser(viewer.browser,env,'wait','--fn',`(async()=>{let frames=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>${stoppedEnd.frames}+2;})()`);
      assert.ok((await metrics(viewer)).pixels>0,'source_publish_video_requires_rendered_pixels');
    }
    await browser(deviceBrowser, env, 'eval', `(()=>{window.__ipcAllTracks=Array.from(views.values()).flatMap(view=>view.stream.getTracks());
      window.__ipcAllPeers=Array.from(views.values()).map(view=>view.peer);
      window.__ipcFirstTracks=views.get(${JSON.stringify(viewers[0].sessionId)}).stream.getTracks();return true;})()`);
    if (options.safety) {
      // trigger返回值不作为成功证据，也不主动发送interaction.end掩盖设备仍采集的问题。
      stage = 'dms_camera_safety_trigger';
      await options.safety.trigger();
      stage = 'device_camera_safety_actual_stop';
      await browser(deviceBrowser, env, 'wait', '--fn',
        'views.size === 0 && invitations.size === 0 && window.__ipcAllTracks.length >= 3 && ' +
        'window.__ipcAllTracks.every(track=>track.readyState === "ended") && ' +
        'window.__ipcAllPeers.length === 3 && window.__ipcAllPeers.every(peer=>peer.connectionState === "closed")');
      stage = 'application_camera_safety_terminal';
      for (const viewer of viewers) {
        const deadlineAt = Date.now() + 20000; // 固定原等待预算，不因非终态或重投续期。
        let terminal = false;
        while (!terminal && Date.now() < deadlineAt) {
          const received = await api('events', {endpointId: viewer.endpointId, limit:32});
          for (const delivery of received.list || [])
            if (safetyTerminal(delivery.event, viewer.sessionId, options.safety.cause)) terminal = true;
          if (received.list?.length) await api('events/ack', {endpointId:viewer.endpointId,
            deliveryIds:received.list.map(value=>value.deliveryId)});
          if (!terminal) await new Promise(resolve=>setTimeout(resolve,100));
        }
        assert.equal(terminal,true,'original_application_safety_terminal_required');
      }
      if (options.safety.restore) await options.safety.restore();
    } else {
      await command(viewers[0], 'interaction.end', {reason: 'userStopped'}, viewers[0].sessionId);
      await browser(deviceBrowser, env, 'wait', '--fn', `!views.has(${JSON.stringify(viewers[0].sessionId)}) && views.size === 2 && window.__ipcFirstTracks.every(track=>track.readyState === 'ended')`);
      // 独立停流后其它两端的实际解码计数必须继续增长。
      for (const viewer of viewers.slice(1)) {
        await browser(viewer.browser, env, 'wait', '--fn', `(async()=>{let frames=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>${viewer.before.frames + 2};})()`);
        if (viewer.listening) {
          await browser(viewer.browser, env, 'wait', '--fn', `(async()=>{let samples=0;(await window.__ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio')samples+=s.totalSamplesReceived||0;});return samples>${viewer.before.audioSamples}+960;})()`);
          assert.ok((await metrics(viewer)).audioPeak > 0.001);
        }
      }
      stage = 'release_all';
      for (const viewer of viewers.slice(1)) await command(viewer, 'interaction.end', {reason: 'userStopped'}, viewer.sessionId);
    }
    await browser(deviceBrowser, env, 'wait', '--fn', 'views.size === 0 && invitations.size === 0 && window.__ipcAllTracks.every(track=>track.readyState === "ended")');
    for (const viewer of viewers) {
      const released = evaluated(await browser(viewer.browser, env, 'eval', `(async()=>{const audio=window.__ipcAudio;
        if(audio)await audio.stop();window.__ipcPeer.close();return JSON.stringify({audioClosed:!audio||audio.context.state==='closed',
          sinkReleased:!audio||audio.sink.srcObject===null,peerClosed:window.__ipcPeer.connectionState==='closed'});})()`));
      assert.deepEqual(released,{audioClosed:true,sinkReleased:true,peerClosed:true},'viewer_media_resources_must_close');
    }
    if (admission === 'accept') {
      assert.equal(acceptFacts.accepted, 3, 'three_local_confirmations_required');
      assert.equal(acceptFacts.rejected, 0, 'local_confirmation_rejected');
    }
    outcome = {result: 'pass', admission, viewers: 3, decodedVideoFrames: viewers.map(viewer => viewer.before.frames),
      renderedPixelSums: viewers.map(viewer => viewer.before.pixels), localConfirmations: acceptFacts.accepted,
      listening: viewers.map(viewer => viewer.listening), decodedAudioSamples: viewers.map(viewer => viewer.before.audioSamples),
      decodedAudioPeaks: viewers.map(viewer => viewer.before.audioPeak),
      trackControlsVerified:trackControls,
      sourceControlsVerified:sourceControls,
      independentStop: !options.safety, safetyStopVerified: options.safety?.cause || null,
      allDeviceTracksReleased: true,allViewerMediaReleased:true};
  } catch (error) {
    // 仅读取固定布尔量和数量，定位缺邀请、桥关闭或采集失败，不转储浏览器源码和会话标识。
    let deviceFacts;
    let audioFacts; // 固定解码数和能量，不输出SDP、设备身份或自由日志。
    try {
      const session = JSON.stringify(currentViewer?.sessionId || '');
      deviceFacts = evaluated(await browser(deviceBrowser, env, 'eval',
        `(async()=>{const view=views.get(${session});let audioPackets=0,sourceEnergy=0;
          if(view?.peer)(await view.peer.getStats()).forEach(s=>{if(s.type==='outbound-rtp'&&s.kind==='audio')audioPackets+=s.packetsSent||0;
            if(s.type==='media-source'&&s.kind==='audio')sourceEnergy+=s.totalAudioEnergy||0;});
          return JSON.stringify({closed,available,views:views.size,invitations:invitations.size,pending:invitations.has(${session}),captured:!!view,
            audioTracks:view?.stream?.getAudioTracks().length||0,audioPackets,sourceEnergy});})()`));
      if (currentViewer) audioFacts = await metrics(currentViewer);
    } catch {}
    // 自有固定错误和断言类型可以记录，避免assert原始SDP/令牌差异进入日志。
    outcome = {result: 'fail', stage, deviceFacts, audioFacts, acceptFacts, consentFacts, reason:
      /^[a-z_]+$/.test(error.message) ? error.message : error instanceof assert.AssertionError ? 'assertion_failed' : 'execution_failed'};
    process.exitCode = 1;
  } finally {
    clearTimeout(deadline);
    // 设备和正式后端仍在线时恢复本任务原物模型值；失败不能被已打印的媒体PASS掩盖。
    if (options.safety?.restore) try { await options.safety.restore(); } catch { cleanupFailures.push('privacy_restore'); }
    for (const viewer of viewers) {
      if (viewer.sessionId) {
        try { await command(viewer, 'interaction.end', {reason: 'userStopped'}, viewer.sessionId); } catch {}
      }
    }
    for (const session of sessions) {
      try { await browser(session, env, 'close'); } catch { cleanupFailures.push('named_browser'); }
    }
    // 正常页面关闭应驱动C++退出0；超时仍精确回收，但不能将被迫终止认作正常通过。
    const {bounded, stopChild} = require('./conference_member_live.cjs');
    if (completion && !childExited) try { await bounded(completion, 3000, 'device_close_timeout'); } catch {}
    await stopChild(child, completion, () => childExited).catch(() => cleanupFailures.push('original_child'));
    if (child && (!childExited || childExitCode !== 0)) cleanupFailures.push('child_exit');
    if (server.listening) await bounded(new Promise((resolve, reject) => server.close(error => error ? reject(error) : resolve())),
      5000, 'listener_close_timeout').catch(() => cleanupFailures.push('local_listener'));
  }
  const cleanup = {childClosed: childExited, childExitCode, browsersClosed: !cleanupFailures.includes('named_browser'),
    listenerClosed: !server.listening, failures: cleanupFailures};
  if (cleanupFailures.length) { outcome = {...outcome, result: 'fail', stage: 'cleanup'}; process.exitCode = 1; }
  outcome.cleanup = cleanup;
  console.log(JSON.stringify(outcome)); return outcome;
}
// 导入暴露纯助手与显式run入口，供离线用例和中央验收复用，不隐式启动网络、浏览器或设备进程。
module.exports = {admissionMode, confirmAdmission, listeningMode, attachListeningAudio, sourceControlResult, sourceControlRPC, sourceControlFailure, safetyTerminal, browser, evaluated, run};
if (require.main === module) {
  run().catch(() => { console.log(JSON.stringify({result: 'fail', stage: 'setup', reason: 'invalid_live_configuration'})); process.exitCode = 1; });
}
