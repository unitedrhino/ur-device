// 文件说明：S18同用户双预览接续的正式后端/C++验收；真实解码和设备轨生命周期不能由信令成功代替。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {browser, evaluated, confirmAdmission, admissionMode} = require('./ipc_live.cjs');
const {bounded, stopChild} = require('./conference_member_live.cjs');

// configuration 拒绝隐式真实执行、外部服务和模糊操作；凭据只读取环境且不进入结果。
function configuration(env, stat = fs.statSync) {
  assert.equal(env.UR_IPC_ENDPOINT_HANDOFF_LIVE, '1', 'explicit_handoff_required');
  assert.equal(env.UR_IPC_LIVE, '1', 'explicit_ipc_required');
  const operation = env.UR_IPC_ENDPOINT_HANDOFF_OPERATION || 'commit';
  assert.ok(['commit', 'abort', 'timeout'].includes(operation), 'invalid_handoff_operation');
  const base = new URL(env.UR_IPC_BASE_URL);
  assert.equal(base.hostname, '127.0.0.1', 'local_service_required');
  assert.equal(base.protocol, 'http:', 'local_http_required');
  assert.equal(base.pathname, '/'); assert.equal(base.search, ''); assert.equal(base.hash, '');
  assert.equal(base.username, ''); assert.equal(base.password, '');
  const port = Number(env.UR_IPC_DEVICE_PORT);
  assert.ok(Number.isSafeInteger(port) && port >= 1024 && port <= 65535, 'local_port_required');
  for (const field of ['UR_IPC_APP_TOKEN', 'UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET'])
    assert.ok(typeof env[field] === 'string' && env[field].length > 0, 'injected_credentials_required');
  assert.ok(stat(env.UR_IPC_DEVICE_BIN).isFile(), 'device_binary_required');
  return {operation, base: base.origin, port, admission: admissionMode(env.UR_IPC_ADMISSION)};
}

// handoffState 精确关联原会话/事务；独立接续revision和会话eventSeq绝不混用。
function handoffState(event, source, handoff, state, candidate) {
  if (!event || event.method !== 'endpointState' || event.sessionId !== source ||
    event.data?.handoffId !== handoff || event.data?.state !== state ||
    !Number.isSafeInteger(event.data.revision) || event.data.revision <= 0) return false;
  const reply = event.code === 200 && !Object.hasOwn(event, 'eventSeq');
  const unsolicited = !Object.hasOwn(event, 'code') && Number.isSafeInteger(event.eventSeq) && event.eventSeq > 0;
  if (!reply && !unsolicited) return false;
  if (state === 'ready' || state === 'committed') return event.data.candidateSessionId === candidate;
  if (Object.hasOwn(event.data, 'candidateSessionId')) return false;
  if (state === 'aborted') return event.data.reason === 'userCancelled';
  if (state === 'failed') return event.data.reason === 'handoffTimeout' && unsolicited;
  return state === 'preparing' && !Object.hasOwn(event.data, 'reason');
}

// decodedProgress 要求新解码与非黑像素，排除握手、静帧和仅RTP计数伪通过。
function decodedProgress(before, after) {
  return Number.isSafeInteger(before?.frames) && Number.isSafeInteger(after?.frames) &&
    after.frames >= before.frames + 2 && after.packets > before.packets && after.pixels > 0 && after.connected === true;
}

// deviceTransferFacts 核对原对象：保留腿不重建且轨存活，结束腿Peer关闭且原采集轨全部ended。
function deviceTransferFacts(facts) {
  return !!facts && facts.stoppedPresent === false && facts.stoppedPeer === 'closed' &&
    facts.stoppedTracks > 0 && facts.stoppedEnded === true && facts.retainedPresent === true &&
    facts.sameRetainedPeer === true && facts.sameRetainedStream === true && facts.retainedActive === true &&
    facts.retainedConnected === true && facts.retainedTracks > 0 && facts.retainedLive === true;
}

// run 仅本任务本机隔离服务；两个应用端和一个原C++设备进程，不注入MQTT或Owner就绪状态。
async function run(environment = process.env) {
  const config = configuration(environment);
  const env = {...environment, AGENT_BROWSER_ARGS: [environment.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required'].join(',')};
  const deviceBrowser = `handoff-device-${randomUUID()}`;
  const sessions = new Set(); // 只清理确实由此运行尝试打开的具名会话，绝不close --all。
  const viewers = [];
  const cleanupFailures = [];
  const server = http.createServer((_, response) => {
    response.writeHead(200, {'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store'});
    response.end('<!doctype html><html><body><video id="remote" autoplay playsinline muted></video></body></html>');
  });
  let child, completion, deadline;
  let childExited = false, childExitCode = null, stage = 'setup', outcome;
  // api 仅走正式鉴权HTTP；失败只返回固定分类，不打印身份、SDP或自由服务端正文。
  async function api(route, body) {
    const response = await fetch(`${config.base}/api/v1/im/interaction/${route}`, {method:'POST',
      headers:{'content-type':'application/json','app-id':environment.UR_IPC_APP_ID || '100',
        'tenant-code':environment.UR_IPC_TENANT || 'platform',token:environment.UR_IPC_APP_TOKEN},
      body:JSON.stringify(body),signal:AbortSignal.timeout(20000)});
    const value = await response.json();
    if (response.status !== 200 || value.code !== 200) throw Error(`api_${route.replaceAll('/','_')}_rejected`);
    return value.data;
  }
  // receive 确认真实outbox并保留已投递原事件，等待ready不能遗失先到的异步事实。
  async function receive(viewer) {
    const value = await api('events',{endpointId:viewer.endpointId,limit:32});
    const deliveries = value.list || [];
    for (const delivery of deliveries) {
      assert.equal(typeof delivery.deliveryId,'string');
      const event = delivery.event;
      assert.ok(event && typeof event.msgToken === 'string');
      viewer.events.push(event);
      if (event.method === 'interaction.state' && event.sessionId === viewer.sessionId) viewer.state = event.data?.state;
    }
    assert.ok(viewer.events.length <= 256,'bounded_event_history_required');
    if (deliveries.length) await api('events/ack',{endpointId:viewer.endpointId,deliveryIds:deliveries.map(row=>row.deliveryId)});
  }
  // until 使用单调总截止短轮询；协议固定30秒不会被重发prepare或长浏览器wait延长。
  async function until(probe, duration = 15000, reason = 'state_timeout') {
    const end = performance.now() + duration;
    do { const value = await probe(); if (value) return value; await new Promise(resolve=>setTimeout(resolve,100)); }
    while (performance.now() < end);
    throw Error(reason);
  }
  // command 关联原token及指定返回method；HTTP受理与持久结果分别校验，重放不再次执行副作用。
  async function command(viewer, method, params, sessionId, original) {
    const payload = original || {msgToken:randomUUID(),method,params,...(sessionId ? {sessionId} :
      {target:{productID:environment.UR_DEVICE_PRODUCT,deviceName:environment.UR_DEVICE_NAME}})};
    const accepted = await api('commands',{endpointId:viewer.endpointId,command:payload});
    assert.equal(accepted.accepted,true); assert.equal(accepted.msgToken,payload.msgToken);
    const resultMethod = method === 'endpointControl' ? 'endpointState' : 'interaction.result';
    const event = await until(async()=>{
      await receive(viewer);
      return viewer.events.find(value=>value.method === resultMethod && value.msgToken === payload.msgToken && Object.hasOwn(value,'code'));
    },20000,'original_result_timeout');
    assert.equal(event.code,200,'original_operation_rejected');
    return {event,payload,replay:accepted.replay === true};
  }
  // metrics 来自当前浏览器实际H264解码与画面采样，不用业务状态构造帧计数。
  async function metrics(viewer) {
    return evaluated(await browser(viewer.browser,env,'eval',`(async()=>{let frames=0,packets=0;
      const peer=window.__ipcPeer;(await peer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video'){frames+=s.framesDecoded||0;packets+=s.packetsReceived||0;}});
      const video=document.getElementById('remote');let pixels=0;if(video.videoWidth&&video.readyState>=2){const canvas=document.createElement('canvas');
        canvas.width=16;canvas.height=16;const context=canvas.getContext('2d');context.drawImage(video,0,0,16,16);
        const bytes=context.getImageData(0,0,16,16).data;for(let i=0;i<bytes.length;i+=4)pixels+=bytes[i]+bytes[i+1]+bytes[i+2];}
      return JSON.stringify({frames,packets,pixels,connected:peer.connectionState==='connected'});})()`));
  }
  // establish 原应用明确open并实际协商，accept策略仍必须点击原设备的本地确认控件。
  async function establish(viewer, handoff) {
    const opened = await command(viewer,'interaction.open',{purpose:'ipcLiveView',cameraId:environment.UR_IPC_CAMERA_ID || 'front',...(handoff ? {handoff} : {})});
    viewer.sessionId = opened.event.sessionId;
    assert.equal(opened.event.data?.sessionId,viewer.sessionId);
    await confirmAdmission(config.admission,viewer.sessionId,(...args)=>browser(deviceBrowser,env,...args));
    await browser(deviceBrowser,env,'wait','--fn',`!!views.get(${JSON.stringify(viewer.sessionId)})?.peer?.localDescription`);
    const offered = await command(viewer,'interaction.mediaOffer',{mediaRevision:1,webrtc:{iceMode:'complete',sdp:viewer.offer}},viewer.sessionId);
    assert.equal(offered.event.data?.mediaRevision,1);
    await browser(viewer.browser,env,'eval',`(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(offered.event.data.webrtc.sdp)}});return true;})()`);
    await browser(viewer.browser,env,'wait','--fn','__ipcPeer.connectionState === "connected"');
    await command(viewer,'interaction.mediaState',{mediaRevision:1,state:'connected'},viewer.sessionId);
    await until(async()=>{await receive(viewer);return viewer.state==='established';},15000,'application_established_required');
    await browser(deviceBrowser,env,'wait','--fn',`views.get(${JSON.stringify(viewer.sessionId)})?.active === true`);
    const decoded = await until(async()=>{const value=await metrics(viewer);return value.frames>=2&&value.packets>0&&value.pixels>0&&value.connected&&value;},15000,'actual_decode_required');
    return {opened,decoded};
  }
  try {
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    // 先启动两个真实接收Peer，避免浏览器冷启动占用prepare固定30秒。
    for (let index=0;index<2;index++) {
      const endpoint = await api('endpoints/register',{});
      const viewer = {endpointId:endpoint.endpointId,browser:`handoff-viewer-${randomUUID()}`,events:[]};
      assert.equal(typeof viewer.endpointId,'string'); viewers.push(viewer); sessions.add(viewer.browser);
      await browser(viewer.browser,env,'open',`http://127.0.0.1:${server.address().port}/`);
      viewer.offer = evaluated(await browser(viewer.browser,env,'eval',`(async()=>{const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});
        peer.ontrack=e=>document.getElementById('remote').srcObject=new MediaStream([e.track]);
        const track=peer.addTransceiver('video',{direction:'recvonly'});
        const codecs=RTCRtpReceiver.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/h264'&&(c.sdpFmtpLine||'').includes('packetization-mode=1'));
        if(!codecs.length)throw Error('h264_required');track.setCodecPreferences(codecs);await peer.setLocalDescription(await peer.createOffer());
        if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
          const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
          peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);})()`));
    }
    child = spawn(environment.UR_IPC_DEVICE_BIN,[String(config.port)],{env:environment,stdio:['ignore','pipe','pipe']});
    child.stderr.on('data',()=>{});
    completion = new Promise(resolve=>child.once('close',code=>{childExited=true;childExitCode=code;resolve();}));
    deadline = setTimeout(()=>child.kill('SIGTERM'),180000);
    await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('device_start_timeout')),10000);
      child.once('error',()=>{clearTimeout(timer);reject(Error('device_spawn_failed'));});
      child.stdout.on('data',bytes=>{if(bytes.toString().includes(`http://127.0.0.1:${config.port}/`)){clearTimeout(timer);resolve();}});});
    sessions.add(deviceBrowser);
    await browser(deviceBrowser,env,'open',`http://127.0.0.1:${config.port}/`);
    await browser(deviceBrowser,env,'snapshot','-i');
    await browser(deviceBrowser,env,'find','role','button','click','--name','连接平台');
    await browser(deviceBrowser,env,'wait','--fn','available === true && closed === false');
    const [source,target]=viewers;
    stage='source_decode';
    const original = await establish(source);
    const handoffId=randomUUID();
    stage='prepare_retains_source';
    const prepared=await command(source,'endpointControl',{operation:'prepare',expectedRevision:0,handoffId,target:{kind:'endpoint',id:target.endpointId}},source.sessionId);
    assert.ok(handoffState(prepared.event,source.sessionId,handoffId,'preparing'));
    await until(async()=>decodedProgress(original.decoded,await metrics(source)),5000,'prepare_must_retain_source');
    // 目标必须先收到平台事实，再经目标端明确open确认；不是prepare自动分配媒体。
    await until(async()=>{await receive(target);return target.events.some(event=>handoffState(event,source.sessionId,handoffId,'preparing'));},5000,'target_prepare_required');
    const noCandidate=evaluated(await browser(deviceBrowser,env,'eval','JSON.stringify(views.size===1)'));
    assert.equal(noCandidate,true,'prepare_must_not_create_candidate');
    stage='target_explicit_open_and_decode';
    const candidate = await establish(target,{sourceSessionId:source.sessionId,handoffId});
    const oldBefore=await metrics(source);
    await until(async()=>decodedProgress(oldBefore,await metrics(source)),5000,'source_must_remain_before_commit');
    stage='server_actual_ready';
    const ready=await until(async()=>{await receive(source);return source.events.find(event=>handoffState(event,source.sessionId,handoffId,'ready',target.sessionId));},10000,'actual_owner_ready_required');
    await browser(deviceBrowser,env,'eval',`(()=>{window.__handoffOriginal=new Map();for(const id of ${JSON.stringify(viewers.map(viewer=>viewer.sessionId))}){
      const view=views.get(id);if(!view?.active)throw Error('active_required');__handoffOriginal.set(id,{view,peer:view.peer,stream:view.stream,tracks:view.stream.getTracks()});}return true;})()`);
    let terminal;
    stage=`${config.operation}_handoff`;
    if(config.operation==='timeout') {
      terminal=await until(async()=>{await receive(source);return source.events.find(event=>handoffState(event,source.sessionId,handoffId,'failed'));},35000,'fixed_handoff_timeout_required');
    } else {
      const changed=await command(source,'endpointControl',{operation:config.operation,expectedRevision:ready.data.revision,handoffId,
        ...(config.operation==='commit' ? {candidateSessionId:target.sessionId} : {})},source.sessionId);
      terminal=changed.event;
      assert.ok(handoffState(terminal,source.sessionId,handoffId,config.operation==='commit' ? 'committed' : 'aborted',target.sessionId));
      const replay=await command(source,'endpointControl',changed.payload.params,source.sessionId,changed.payload);
      assert.equal(replay.replay,true,'original_terminal_must_replay');assert.deepEqual(replay.event,terminal);
    }
    const stopped=config.operation==='commit'?source:target;
    const retained=config.operation==='commit'?target:source;
    stopped.state='ended'; // 仅记已核原终态用于清理，不写设备或服务端状态。
    stage='actual_device_stop_and_retention';
    const retainedBefore=await metrics(retained);
    const facts=await until(async()=>{
      const value=evaluated(await browser(deviceBrowser,env,'eval',`JSON.stringify((()=>{const old=__handoffOriginal.get(${JSON.stringify(stopped.sessionId)});
        const keep=__handoffOriginal.get(${JSON.stringify(retained.sessionId)});const current=views.get(${JSON.stringify(retained.sessionId)});
        return {stoppedPresent:views.has(${JSON.stringify(stopped.sessionId)}),stoppedPeer:old.peer.connectionState,stoppedTracks:old.tracks.length,
          stoppedEnded:old.tracks.every(t=>t.readyState==='ended'),retainedPresent:!!current,sameRetainedPeer:current?.peer===keep.peer,
          sameRetainedStream:current?.stream===keep.stream,retainedActive:current?.active===true,retainedConnected:keep.peer.connectionState==='connected',
          retainedTracks:keep.tracks.length,retainedLive:keep.tracks.every(t=>t.readyState==='live')};})())`));
      return deviceTransferFacts(value)&&value;
    },15000,'actual_device_resource_release_required');
    const retainedAfter=await until(async()=>{const value=await metrics(retained);return decodedProgress(retainedBefore,value)&&value;},10000,'retained_actual_decode_required');
    // 排空已在途帧后，旧媒体不得继续收包和解码；不得通过主动关闭观看Peer伪造停流。
    await new Promise(resolve=>setTimeout(resolve,600));
    const stoppedBefore=await metrics(stopped);
    await new Promise(resolve=>setTimeout(resolve,600));
    const stoppedAfter=await metrics(stopped);
    assert.equal(stoppedAfter.packets,stoppedBefore.packets,'stopped_media_must_not_receive_rtp');
    assert.equal(stoppedAfter.frames,stoppedBefore.frames,'stopped_media_must_not_decode');
    outcome={result:'pass',operation:config.operation,admission:config.admission,sourceFrames:original.decoded.frames,candidateFrames:candidate.decoded.frames,
      retainedNewFrames:retainedAfter.frames-retainedBefore.frames,deviceResourceStop:deviceTransferFacts(facts),terminalRevision:terminal.data.revision};
  } catch(error) {
    outcome={result:'fail',stage,reason:/^[a-z_]+$/.test(error.message)?error.message:error instanceof assert.AssertionError?'assertion_failed':'execution_failed'};
    process.exitCode=1;
  } finally {
    clearTimeout(deadline);
    for(const viewer of viewers) if(viewer.sessionId) {
      try {await receive(viewer);if(viewer.state!=='ended')await command(viewer,'interaction.end',{reason:'userStopped'},viewer.sessionId);}
      catch {cleanupFailures.push('formal_session_end');}
    }
    for(const session of sessions) try {await browser(session,env,'close');} catch {cleanupFailures.push('named_browser');}
    if(completion&&!childExited) try {await bounded(completion,3000,'device_close_timeout');} catch {}
    await stopChild(child,completion,()=>childExited).catch(()=>cleanupFailures.push('original_child'));
    if(child&&(!childExited||childExitCode!==0))cleanupFailures.push('child_exit');
    if(server.listening)await bounded(new Promise((resolve,reject)=>server.close(error=>error?reject(error):resolve())),5000,'listener_close_timeout').catch(()=>cleanupFailures.push('local_listener'));
  }
  if(cleanupFailures.length){outcome={...outcome,result:'fail',stage:'cleanup'};process.exitCode=1;}
  outcome.cleanup={childClosed:childExited,childExitCode,listenerClosed:!server.listening,failures:cleanupFailures};
  console.log(JSON.stringify(outcome));return outcome;
}
module.exports={configuration,handoffState,decodedProgress,deviceTransferFacts,run};
if(require.main===module)run().catch(()=>{console.log(JSON.stringify({result:'fail',stage:'setup',reason:'invalid_live_configuration'}));process.exitCode=1;});
