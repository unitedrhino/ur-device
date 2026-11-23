// 文件说明：显式历史真实后端E2E与固定素材生成入口；仅正式APP接口和独立实际Peer，不注入媒体成功。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {spawn, execFile} = require('node:child_process');
const {randomUUID, createHash} = require('node:crypto');
const {promisify} = require('node:util');
const {browser, evaluated, attachListeningAudio} = require('./ipc_live.cjs');

// fixturePlan固定30s H264/Opus及五位实际视频秒标记，不使用实时fake camera或麦克风。
function fixturePlan(output, cwd = process.cwd()) {
  const root = path.resolve(cwd, '.temp'), file = path.resolve(output || '');
  assert.ok(file.startsWith(`${root}${path.sep}`) && file.endsWith('.mp4'), 'task_temp_mp4_required');
  const markers = ["drawbox=x=0:y=0:w=320:h=64:color=black:t=fill"];
  for (let bit = 0; bit < 5; bit++) markers.push(`drawbox=x=${16 + bit * 40}:y=16:w=24:h=24:color=white:t=fill:enable='gte(mod(floor(t)/${2 ** bit},2),1)'`);
  return {file, ffmpeg: ['-hide_banner', '-loglevel', 'error', '-nostdin', '-n', '-f', 'lavfi', '-i',
    'color=c=0x1828a0:s=320x240:r=15:d=30', '-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=48000:duration=30',
    '-vf', markers.join(','), '-c:v', 'libx264', '-profile:v', 'baseline', '-pix_fmt', 'yuv420p', '-preset', 'veryfast',
    '-tune', 'zerolatency', '-g', '15', '-c:a', 'libopus', '-b:a', '64k', '-ac', '1', '-ar', '48000', '-t', '30',
    '-movflags', '+faststart', file], probe: ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', file]};
}
// generateFixture只输出本worktree临时路径、不覆盖现有档案；实际执行必须由中央资源门禁调用。
async function generateFixture(output) {
  const plan = fixturePlan(output), root = path.resolve('.temp');
  fs.mkdirSync(root, {recursive: true});
  const realRoot = fs.realpathSync(root);
  let ancestor = path.dirname(plan.file);
  while (!fs.existsSync(ancestor)) ancestor = path.dirname(ancestor);
  const realAncestor = fs.realpathSync(ancestor);
  assert.ok(realAncestor === realRoot || realAncestor.startsWith(`${realRoot}${path.sep}`), 'no_symlink_escape');
  fs.mkdirSync(path.dirname(plan.file), {recursive: true});
  assert.ok(fs.realpathSync(path.dirname(plan.file)).startsWith(`${fs.realpathSync(root)}${path.sep}`), 'no_symlink_escape');
  assert.equal(fs.existsSync(plan.file), false, 'fixture_already_exists');
  const exec = promisify(execFile);
  await exec('ffmpeg', plan.ffmpeg, {timeout: 60000, maxBuffer: 65536});
  const probe = JSON.parse((await exec('ffprobe', plan.probe, {timeout: 10000, maxBuffer: 65536})).stdout);
  const video = probe.streams.filter(value => value.codec_type === 'video'), audio = probe.streams.filter(value => value.codec_type === 'audio');
  assert.ok(video.length === 1 && video[0].codec_name === 'h264' && audio.length === 1 && audio[0].codec_name === 'opus', 'actual_h264_opus_archive_required');
  assert.ok(Number(audio[0].sample_rate) === 48000 && audio[0].channels === 1 && Math.round(Number(probe.format.duration) * 1000) === 30000, 'actual_fixed_duration_required');
  const data = fs.readFileSync(plan.file);
  assert.ok(data.length > 0 && data.length <= 8388608, 'bridge_archive_budget');
  return {durationMs: 30000, videoCodec: 'H264', audioCodec: 'opus', bytes: data.length, sha256: createHash('sha256').update(data).digest('hex')};
}
// validateConfiguration在任何进程／网络副作用前确认本机隔离、固定完整档案与注入授权。
function validateConfiguration(env, isFile = file => fs.statSync(file).isFile()) {
  assert.equal(env.UR_RECORD_LIVE, '1', 'explicit_record_live_required');
  const url = new URL(env.UR_RECORD_BASE_URL);
  assert.ok(url.protocol === 'http:' && url.hostname === '127.0.0.1' && !url.username && !url.password &&
    url.pathname === '/' && !url.search && !url.hash, 'isolated_backend_required');
  const port = Number(env.UR_RECORD_DEVICE_PORT), start = Number(env.UR_RECORD_START_TIME_MS);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535, 'local_port_required');
  assert.ok(Number.isSafeInteger(start) && start >= 0 && start + 30000 <= Number.MAX_SAFE_INTEGER, 'absolute_catalog_time_required');
  for (const key of ['UR_RECORD_DEVICE_BIN', 'UR_RECORD_FILE']) assert.ok(env[key] && isFile(env[key]), 'device_and_archive_files_required');
  for (const key of ['UR_RECORD_APP_TOKEN', 'UR_RECORD_TENANT', 'UR_RECORD_APP_ID', 'UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME',
    'UR_DEVICE_SECRET', 'UR_RECORD_CAMERA_ID', 'UR_RECORD_ID', 'UR_RECORD_VERSION', 'UR_RECORD_CONTENT_REF'])
    assert.ok(typeof env[key] === 'string' && env[key].length > 0 && !/[\x00-\x1f\x7f]/.test(env[key]), 'explicit_scope_required');
  assert.ok(env.UR_RECORD_ENABLED === '1' && env.UR_RECORD_AUDIO === '1' && env.UR_RECORD_DURATION_MS === '30000', 'full_audio_archive_required');
  assert.ok(path.isAbsolute(env.UR_RECORD_FILE) && Number.isSafeInteger(Number(env.UR_RECORD_MAX_BYTES)) && Number(env.UR_RECORD_MAX_BYTES) >= 1 &&
    Number(env.UR_RECORD_MAX_BYTES) <= 8388608 && Number.isSafeInteger(Number(env.UR_RECORD_LEASE_MS)) &&
    Number(env.UR_RECORD_LEASE_MS) >= 1000 && Number(env.UR_RECORD_LEASE_MS) <= 600000, 'finite_archive_configuration_required');
  return {port, start};
}
// openCommand仅构造原用户意图，版本/ref/clip授信由目录与Creator决定，不能自报用户。
function openCommand(env, token) {
  return {msgToken: token, method: 'interaction.open', target: {productID: env.UR_DEVICE_PRODUCT, deviceName: env.UR_DEVICE_NAME},
    params: {purpose: 'recordPlayback', cameraId: env.UR_RECORD_CAMERA_ID, recordingId: env.UR_RECORD_ID, clip: {startMs: 0, endMs: 30000}}};
}
// playbackCommand明确携当前m和实际前置revision；seek只新增positionMs，不在APP端自行加m。
function playbackCommand(token, session, playback, revision, media, operation, position) {
  assert.ok(['play', 'pause', 'seek', 'stop'].includes(operation) && Number.isSafeInteger(revision) && revision >= 0 &&
    Number.isSafeInteger(media) && media >= 1, 'original_playback_revision_required');
  const argumentsValue = {operation, expectedRevision: revision, playbackId: playback, mediaRevision: media};
  if (operation === 'seek') { assert.ok(Number.isSafeInteger(position) && position >= 0 && position <= 30000); argumentsValue.positionMs = position; }
  else assert.equal(position, undefined, 'position_is_seek_only');
  return {msgToken: token, method: 'interaction.control', sessionId: session, params: {command: 'playbackControl', arguments: argumentsValue}};
}
// controlFactReady 判断APP已收到的原播放器事实，不从视频或用户目标位置推断驱动状态。
function controlFactReady(fact, playback, media, operation) {
  const allowed = {play: ['loaded', 'paused', 'playing'], pause: ['playing', 'paused'],
    seek: ['loaded', 'paused', 'playing'], stop: ['loaded', 'paused', 'playing', 'seeking']};
  return !!(fact && fact.playbackId === playback && fact.mediaRevision === media &&
    Number.isSafeInteger(fact.revision) && fact.revision > 0 && allowed[operation]?.includes(fact.state));
}
// mediaPass只接受真正解码PCM、H264像素及帧内秒标记，不以SDP/Connected/目标位置冒实际播放。
function mediaPass(value, minimumSecond = 0, maximumSecond = 29) {
  return !!(value && value.connected && value.frames >= 2 && value.pixels > 0 && value.width === 320 &&
    value.audioSamples > 1920 && value.peak > 0.001 && value.run >= 2 &&
    Number.isInteger(value.markerSecond) && value.markerSecond >= minimumSecond && value.markerSecond <= maximumSecond);
}
// pausePass从排空后的稳定窗口证明源时钟和实际RTP/解码停流，不依靠页面静音。
function pausePass(before, after, sourceBefore, sourceAfter) {
  return !!(before && after && sourceBefore && sourceAfter && before.frames === after.frames && before.packets === after.packets &&
    before.audioPackets === after.audioPackets && sourceBefore.state === 'paused' && sourceAfter.state === 'paused' &&
    Math.abs(sourceAfter.positionMs - sourceBefore.positionMs) <= 80 && sourceBefore.frames === sourceAfter.frames);
}
// waitFor原固定预算不因轮询刷新；报错不含第三方正文、身份或素材路径。
async function waitFor(predicate, timeout = 20000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) { if (await predicate()) return; await new Promise(resolve => setTimeout(resolve, 100)); }
  throw Error('record_fact_timeout');
}

// run只通过正式原APP端点与源进程测试，失败保留脱敏阶段，不直发MQTT或调用收费模型。
async function run() {
  const env = process.env, {port, start} = validateConfiguration(env);
  const browserEnv = {...env, AGENT_BROWSER_ARGS: [env.AGENT_BROWSER_ARGS || '--no-sandbox', '--mute-audio',
    '--autoplay-policy=no-user-gesture-required'].join(',')};
  const sourceBrowser = `task143-record-source-${randomUUID()}`, appBrowser = `task143-record-app-${randomUUID()}`;
  const sessions = [sourceBrowser, appBrowser], eventCache = new Map(), evidence = [];
  const server = http.createServer((_, response) => {
    response.writeHead(200, {'content-type': 'text/html', 'cache-control': 'no-store'});
    response.end('<!doctype html><html><body><video id="remote" autoplay playsinline muted></video></body></html>');
  });
  let child, completion, timer, exited = false, endpoint = '', session = '', playback = '', stage = 'configuration';
  let media = 1, actual = null, established = 0, releaseReceipt = null;
  // api允许正式目录／原interaction接口，不把未实现路由默认为Mock或成功。
  async function api(route, body = {}) {
    const response = await fetch(`${env.UR_RECORD_BASE_URL}/api/v1/im/interaction/${route}`, {method: 'POST',
      headers: {'content-type': 'application/json', token: env.UR_RECORD_APP_TOKEN, 'tenant-code': env.UR_RECORD_TENANT, 'app-id': env.UR_RECORD_APP_ID},
      body: JSON.stringify(body), signal: AbortSignal.timeout(15000)});
    const result = await response.json();
    const pending = route === 'commands' && response.status === 202 && result.code === 202 &&
      result.data?.accepted === true && typeof result.data?.msgToken === 'string';
    assert.ok((response.status === 200 && result.code === 200) || pending, 'formal_record_api_rejected'); return result.data;
  }
  // receive先保存对应原会话事实再ACK；只当前媒体代次可更新APP Actual。
  function applyEvent(event) {
    if (event.sessionId !== session) return;
    if (event.method === 'interaction.state' && event.data?.state === 'established' && event.data.mediaRevision === media) established = media;
    if (event.method === 'interaction.controlState' && event.data?.command === 'playbackState') {
      const fact = event.data.result;
      if (fact?.playbackId === playback && fact.released === true && Object.keys(fact).length === 3 &&
          Number.isSafeInteger(fact.endEventSeq) && fact.endEventSeq > 0) releaseReceipt = fact;
      else if (fact?.playbackId === playback && fact.mediaRevision === media && Number.isSafeInteger(fact.revision)) actual = fact;
    }
  }
  async function receive() {
    const data = await api('events', {endpointId: endpoint, limit: 32});
    assert.ok(Array.isArray(data.list) && data.list.length <= 32, 'bounded_original_events_required');
    for (const delivery of data.list) {
      const event = delivery.event; assert.ok(event && typeof event.msgToken === 'string', 'original_event_required');
      eventCache.set(`${event.method}:${event.msgToken}`, event);
      if (eventCache.size > 128) eventCache.delete(eventCache.keys().next().value);
      applyEvent(event);
    }
    if (data.list.length) await api('events/ack', {endpointId: endpoint, deliveryIds: data.list.map(value => value.deliveryId)});
  }
  // command将HTTP受理与原token最终结果分开，重试不得更换token。
  async function command(commandValue) {
    const accepted = await api('commands', {endpointId: endpoint, command: commandValue});
    assert.ok(accepted.accepted === true && accepted.msgToken === commandValue.msgToken, 'original_command_acceptance_required');
    let result;
    await waitFor(async () => { await receive(); result = eventCache.get(`interaction.result:${commandValue.msgToken}`); return !!result; });
    assert.equal(result.code, 200, 'original_operation_result_required'); return result;
  }
  // sourceFacts只读真实档案元素／Peer及核心投影，不把平台目标值写入测量对象。
  async function sourceFacts() {
    return evaluated(await browser(sourceBrowser, browserEnv, 'eval', `(()=>{const s=recordSource;return JSON.stringify({state:s.state,positionMs:s.decodedPosition,
      frames:s.frames,playing:!!s.media&&!s.media.paused,peerConnected:s.peer?.connection.connectionState==='connected',
      retiring:s.retiring.size,closed:s.closed,peerAbsent:!s.peer,audioAbsent:!s.audioContext,mediaAbsent:!s.media,
      core:recordHostState});})()`));
  }
  // rememberSource保留当前实际对象供旧代次及最终释放核验，不按旧token重造新对象绕过栅栏。
  async function rememberSource() {
    await browser(sourceBrowser, browserEnv, 'eval', `(()=>{const s=recordSource,p=s.peer;if(!p)throw Error('source_peer_required');
      (window.__recordSourcePeers||=[]).push({peer:p,tracks:[...p.tracks,...(p.stream?.getTracks()||[]),...(p.audio?.stream.getTracks()||[])]});
      window.__recordSourceContext ||= s.audioContext;window.__recordSourceElement ||= s.media;return true;})()`);
  }
  // viewerMetrics来自真实inbound解码、canvas五位秒标记与非静音PCM分析器。
  async function viewerMetrics() {
    return evaluated(await browser(appBrowser, browserEnv, 'eval', `(async()=>{let frames=0,packets=0,audioPackets=0,audioSamples=0;
      if(window.__recordPeer)(await __recordPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video'){frames+=s.framesDecoded||0;packets+=s.packetsReceived||0;}
        if(s.type==='inbound-rtp'&&s.kind==='audio'){audioPackets+=s.packetsReceived||0;audioSamples+=s.totalSamplesReceived||0;}});
      const video=document.getElementById('remote');let pixels=0,markerSecond=-1;
      if(video.videoWidth===320&&video.readyState>=2){const canvas=document.createElement('canvas');canvas.width=320;canvas.height=240;
        const ctx=canvas.getContext('2d');ctx.drawImage(video,0,0);const bytes=ctx.getImageData(0,0,320,240).data;markerSecond=0;
        for(let bit=0;bit<5;bit++){const p=(28*320+28+bit*40)*4;if(bytes[p]+bytes[p+1]+bytes[p+2]>400)markerSecond+=2**bit;}
        for(let i=0;i<bytes.length;i+=4)pixels+=bytes[i]+bytes[i+1]+bytes[i+2];}
      return JSON.stringify({connected:window.__recordPeer?.connectionState==='connected',frames,packets,audioPackets,audioSamples,pixels,
        markerSecond,width:video.videoWidth,peak:window.__ipcAudio?.peak||0,run:window.__ipcAudio?.maxRun||0});})()`));
  }
  // closeViewer清空原代次解码缓冲并实际关闭Peer/所有接收轨/音频图，旧对象保留供最终释放核对。
  async function closeViewer() {
    return evaluated(await browser(appBrowser, browserEnv, 'eval', `(async()=>{const peer=window.__recordPeer,audio=window.__ipcAudio;
      if(audio)await audio.stop();if(peer){const tracks=peer.getReceivers().map(r=>r.track);tracks.forEach(t=>t.stop());peer.close();
        (window.__recordRetired||=[]).push({peer,tracks,audio});}const video=document.getElementById('remote');video.pause();video.srcObject=null;
      window.__recordPeer=null;window.__ipcAudio=null;return JSON.stringify({peerClosed:!peer||peer.connectionState==='closed',
        tracksEnded:!peer||peer.getReceivers().every(r=>r.track.readyState==='ended'),audioClosed:!audio||audio.context.state==='closed'});})()`));
  }
  // negotiate原观看端只recvonly，不用getUserMedia；当前m双端实际报告后再等待平台Established。
  async function negotiate() {
    const offer = evaluated(await browser(appBrowser, browserEnv, 'eval', `(async()=>{const peer=window.__recordPeer=new RTCPeerConnection({iceServers:[]});
      peer.ontrack=e=>{if(e.track.kind==='video'){const sink=document.getElementById('remote');sink.srcObject=new MediaStream([e.track]);void sink.play();}
        else (${attachListeningAudio.toString()})(e.track);};
      for(const kind of ['video','audio']){const t=peer.addTransceiver(kind,{direction:'recvonly'});t.setCodecPreferences(RTCRtpReceiver.getCapabilities(kind).codecs.filter(c=>
        c.mimeType.toLowerCase()===(kind==='video'?'video/h264':'audio/opus')&&(kind!=='video'||(c.sdpFmtpLine||'').includes('packetization-mode=1'))));}
      await peer.setLocalDescription(await peer.createOffer());if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
        const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};peer.addEventListener('icegatheringstatechange',check);check();});
      return JSON.stringify(peer.localDescription.sdp);})()`));
    const answer = await command({msgToken: randomUUID(), method: 'interaction.mediaOffer', sessionId: session,
      params: {mediaRevision: media, webrtc: {iceMode: 'complete', sdp: offer}}});
    assert.ok(answer.data?.mediaRevision === media && answer.data.webrtc?.iceMode === 'complete' && answer.data.webrtc.sdp?.startsWith('v=0\r\n'), 'current_media_answer_required');
    await browser(appBrowser, browserEnv, 'eval', `(async()=>{await __recordPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answer.data.webrtc.sdp)}});return true;})()`);
    await browser(appBrowser, browserEnv, 'wait', '--fn', '__recordPeer.connectionState === "connected"');
    await command({msgToken: randomUUID(), method: 'interaction.mediaState', sessionId: session, params: {mediaRevision: media, state: 'connected'}});
    await waitFor(async () => { await receive(); return established === media && (await sourceFacts()).core?.phase === 'active'; }, 15000);
  }
  // control只读取APP已收到的原Actual前置，不用请求目标位置当实际revision。
  async function control(operation, position) {
    // 寻位回复只说明seeking；持续拉取当前APP事件直到驱动事实到达，不能拿解码画面替代水位。
    await waitFor(async () => { await receive(); return controlFactReady(actual, playback, media, operation); });
    // 只记录已收到的状态与水位，不泄露身份、令牌或SDP；用于定位实际播流与APP事实到达的时差。
    console.error(JSON.stringify({diagnostic: 'record_control_precondition', operation,
      actualState: actual.state, actualRevision: actual.revision, mediaRevision: media}));
    const result = await command(playbackCommand(randomUUID(), session, playback, actual.revision, media, operation, position));
    const fact = result.data?.result;
    assert.ok(fact && fact.playbackId === playback && Number.isSafeInteger(fact.revision), 'actual_control_result_required');
    actual = fact; return fact;
  }
  // playing证据必须来自新Peer真实标记与PCM，不借上一代次计数通过。
  async function playing(minimum, maximum) {
    let value;
    await waitFor(async () => { value = await viewerMetrics(); return mediaPass(value, minimum, maximum); });
    const source = await sourceFacts(); assert.ok(source.state === 'playing' && source.playing && source.frames > 0);
    evidence.push({mediaRevision: media, videoFrames: value.frames, decodedSamples: value.audioSamples, peak: value.peak, markerSecond: value.markerSecond, sourcePositionMs: source.positionMs});
  }
  try {
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
    child = spawn(env.UR_RECORD_DEVICE_BIN, [String(port)], {env, stdio: ['ignore', 'pipe', 'pipe']});
    child.stderr.on('data', () => {});
    completion = new Promise(resolve => child.once('close', () => { exited = true; resolve(); }));
    timer = setTimeout(() => child.kill('SIGTERM'), 240000);
    await new Promise((resolve, reject) => {const timeout = setTimeout(() => reject(Error('record_device_start_timeout')), 10000);
      child.once('error', () => {clearTimeout(timeout); reject(Error('record_spawn_failed'));});
      child.stdout.on('data', bytes => {if(bytes.toString().includes(`http://127.0.0.1:${port}/`)){clearTimeout(timeout);resolve();}});});
    stage = 'device_online'; await browser(sourceBrowser, browserEnv, 'open', `http://127.0.0.1:${port}/`);
    await browser(sourceBrowser, browserEnv, 'wait', '--fn', 'document.getElementById("record-status").textContent.includes("历史能力已确认")');
    await browser(appBrowser, browserEnv, 'open', `http://127.0.0.1:${server.address().port}/`);
    endpoint = (await api('endpoints/register')).endpointId; assert.ok(endpoint);
    stage = 'formal_catalog_query';
    const catalog = await api('recordings/query', {endpointId: endpoint, target: {productID: env.UR_DEVICE_PRODUCT, deviceName: env.UR_DEVICE_NAME},
      cameraId: env.UR_RECORD_CAMERA_ID, startTimeMs: start, endTimeMs: start + 30000, limit: 16});
    const item = catalog.list?.find(value => value.recordingId === env.UR_RECORD_ID);
    assert.ok(item && item.cameraId === env.UR_RECORD_CAMERA_ID && item.recordingVersion === env.UR_RECORD_VERSION && item.available === true &&
      item.durationMs === 30000 && item.videoCodec === 'H264' && item.audioCodec === 'opus', 'actual_authorized_catalog_required');
    stage = 'independent_history_open'; const opened = await command(openCommand(env, randomUUID()));
    session = opened.sessionId; playback = opened.data?.playbackId;
    assert.ok(session && opened.data?.sessionId === session && playback, 'original_history_binding_required');
    // 同一events批次可能先含open结果再含loaded；原未知session事件保留后按新绑定投影，不丢事实。
    for (const event of eventCache.values()) applyEvent(event);
    stage = 'actual_load_without_play';
    await waitFor(async () => { await receive(); const source = await sourceFacts(); return actual?.state === 'loaded' && source.state === 'loaded' && !source.playing; });
    await negotiate(); assert.equal((await sourceFacts()).playing, false, 'no_play_before_explicit_control');
    stage = 'actual_initial_play'; await control('play'); await playing(0, 6); await rememberSource();
    stage = 'playing_seek_new_peer';
    const seeking = await control('seek', 10000); assert.ok(seeking.state === 'seeking' && seeking.mediaRevision === media + 1);
    assert.deepEqual(await closeViewer(), {peerClosed: true, tracksEnded: true, audioClosed: true});
    media++; actual = seeking; established = 0; await negotiate(); await playing(10, 15); await rememberSource();
    stage = 'actual_pause_stability'; await control('pause');
    await browser(appBrowser, browserEnv, 'wait', '1500');
    const paused = await viewerMetrics(), sourcePaused = await sourceFacts();
    await browser(appBrowser, browserEnv, 'wait', '750');
    assert.ok(pausePass(paused, await viewerMetrics(), sourcePaused, await sourceFacts()), 'actual_pause_must_stop_read_and_rtp');
    stage = 'paused_seek_new_peer';
    const pausedSeek = await control('seek', 20000); assert.ok(pausedSeek.state === 'seeking' && pausedSeek.mediaRevision === media + 1);
    assert.deepEqual(await closeViewer(), {peerClosed: true, tracksEnded: true, audioClosed: true});
    media++; actual = pausedSeek; established = 0; await negotiate();
    await waitFor(async () => { await receive(); return actual?.state === 'paused' && (await sourceFacts()).state === 'paused'; });
    assert.equal((await sourceFacts()).playing, false, 'seek_cannot_resume_paused');
    const beforeExplicitPlay = await viewerMetrics(); await browser(appBrowser, browserEnv, 'wait', '750');
    const stillPaused = await viewerMetrics(); assert.equal(stillPaused.packets, beforeExplicitPlay.packets); assert.equal(stillPaused.audioPackets, beforeExplicitPlay.audioPackets);
    stage = 'explicit_resume_actual_media'; await control('play'); await playing(20, 25); await rememberSource();
    stage = 'actual_natural_eof';
    await waitFor(async () => { await receive(); return (await sourceFacts()).state === 'ended'; }, 20000);
    stage = 'original_terminal_full_release';
    await waitFor(async () => { await receive(); const source = await sourceFacts(); return source.closed && source.peerAbsent && source.audioAbsent && source.mediaAbsent &&
      source.retiring === 0 && source.core?.released === true && source.core.releaseSubmitted === true && releaseReceipt !== null; });
    assert.deepEqual(await closeViewer(), {peerClosed: true, tracksEnded: true, audioClosed: true});
    const allViewerClosed = evaluated(await browser(appBrowser, browserEnv, 'eval', '__recordRetired.length===3 && __recordRetired.every(r=>r.peer.connectionState==="closed"&&r.tracks.every(t=>t.readyState==="ended")&&(!r.audio||r.audio.context.state==="closed"))'));
    assert.equal(allViewerClosed, true, 'all_media_generations_released');
    const allSourceClosed = evaluated(await browser(sourceBrowser, browserEnv, 'eval', `(async()=>{await Promise.all(__recordSourcePeers.map(r=>r.peer.pending));
      return JSON.stringify({peers:__recordSourcePeers.length===3&&__recordSourcePeers.every(r=>r.peer.closed&&r.peer.connection.connectionState==='closed'),
        tracks:__recordSourcePeers.every(r=>r.tracks.every(t=>t.readyState==='ended')),audio:__recordSourceContext.state==='closed',
        element:__recordSourceElement.paused&&!document.contains(__recordSourceElement)&&__recordSourceElement.getAttribute('src')===null});})()`));
    assert.deepEqual(allSourceClosed, {peers: true, tracks: true, audio: true, element: true}, 'actual_source_objects_all_released');
    await browser(sourceBrowser, browserEnv, 'click', '#record-close');
    const exit = await Promise.race([completion.then(() => true), new Promise(resolve => setTimeout(() => resolve(false), 12000))]);
    assert.equal(exit, true, 'source_process_exit_required');
    return {ok: true, actualHistoryMediaVerified: true, backendReleaseReceiptVerified: true, hardwareVerified: false,
      scenarios: ['play', 'playingSeek', 'pause', 'pausedSeek', 'resume', 'naturalEOF', 'release'], evidence};
  } catch {
    return {ok: false, stage, actualHistoryMediaVerified: false, hardwareVerified: false, evidence};
  } finally {
    clearTimeout(timer);
    if (session && endpoint) { try { await command({msgToken: randomUUID(), method: 'interaction.end', sessionId: session, params: {reason: 'userStopped'}}); } catch {} }
    try { await closeViewer(); } catch {}
    for (const name of sessions) { try { await browser(name, browserEnv, 'close'); } catch {} }
    if (child && !exited) { child.kill('SIGTERM'); await completion; }
    await new Promise(resolve => server.close(resolve));
  }
}

module.exports = {fixturePlan, generateFixture, validateConfiguration, openCommand, playbackCommand, controlFactReady, mediaPass, pausePass, run};
if (require.main === module) {
  const operation = process.argv[2] === '--fixture' ? generateFixture(process.env.UR_RECORD_FIXTURE_OUTPUT) : run();
  operation.then(result => {process.stdout.write(`${JSON.stringify(result)}\n`); if (result.ok === false) process.exitCode = 1;})
    .catch(() => {process.stderr.write('record_live_configuration_or_fixture_failed\n'); process.exitCode = 1;});
}
