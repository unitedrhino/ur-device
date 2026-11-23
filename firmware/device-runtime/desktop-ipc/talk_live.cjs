// 文件说明：正式IM会议、HTTP信令、C++ MQTT设备与双向Opus解码E2E；不注入媒体建立或设备应答。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {browser, evaluated, confirmAdmission, attachListeningAudio, listeningMode, sourceControlRPC, safetyTerminal} = require('./ipc_live.cjs');
const {previewCount, createPreviewGroup} = require('./talk_preview_live.cjs');

// validateTalkOptions 在创建资源前限定正式安全探针；不允许任意终态、缺失触发器或伪造媒体事实。
function validateTalkOptions(options) {
  assert.ok(options && typeof options === 'object' && !Array.isArray(options), 'invalid_talk_options');
  assert.ok(Object.keys(options).every(key => ['onReady', 'safety'].includes(key)), 'invalid_talk_options');
  if (options.onReady !== undefined) assert.equal(typeof options.onReady, 'function', 'invalid_ready_probe');
  if (options.safety !== undefined) {
    const safety = options.safety;
    assert.ok(safety && typeof safety === 'object' && !Array.isArray(safety), 'invalid_safety_probe');
    assert.deepEqual(Object.keys(safety).sort(), safety.restore === undefined ? ['cause', 'trigger'] : ['cause', 'restore', 'trigger'],
      'invalid_safety_probe');
    assert.ok(['privacyEnabled', 'permissionRevoked'].includes(safety.cause), 'invalid_safety_cause');
    assert.equal(typeof safety.trigger, 'function', 'invalid_safety_trigger');
    if (safety.restore !== undefined) assert.equal(typeof safety.restore, 'function', 'invalid_safety_restore');
  }
}

// waitFor 按实际异步事实轮询，独立截止时间防止CLI把Promise对象视为条件已满足。
async function waitFor(check, timeout = 20000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    if (await check()) return;
    await new Promise(resolve => setTimeout(resolve,100));
  }
  throw Error('actual_state_timeout');
}

// validateConfiguration 在启动任何网络/浏览器前验证显式许可、回环和真实音频夹具。
function validateConfiguration(env) {
  assert.equal(env.UR_IPC_TALK_LIVE,'1','explicit_talk_live_required');
  const url = new URL(env.UR_IPC_BASE_URL);
  assert.ok(url.protocol === 'http:' && url.hostname === '127.0.0.1' && !url.username && !url.password &&
    url.pathname === '/' && !url.search && !url.hash,'loopback_backend_required');
  const port = Number(env.UR_IPC_DEVICE_PORT);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535,'device_port_required');
  for (const name of ['UR_IPC_DEVICE_BIN','UR_IPC_AUDIO_FIXTURE'])
    assert.ok(env[name] && fs.statSync(env[name]).isFile(),'fixture_or_binary_required');
  for (const name of ['UR_IPC_APP_TOKEN','UR_DEVICE_PRODUCT','UR_DEVICE_NAME','UR_DEVICE_SECRET'])
    assert.ok(typeof env[name] === 'string' && env[name].length,'injected_identity_required');
  assert.equal(env.UR_IPC_CONFERENCE_TALK,'1');
  assert.equal(env.UR_IPC_MICROPHONE,'1');
  const previews = previewCount(env.UR_IPC_TALK_PREVIEW_COUNT);
  assert.ok(!previews || !listeningMode(env.UR_IPC_SPEAKER_CONTROL), 'combined_speaker_probe_not_supported');
  if (listeningMode(env.UR_IPC_SPEAKER_CONTROL)) {
    assert.ok(env.UR_IPC_SOURCE_CONTROL_BIN && fs.statSync(env.UR_IPC_SOURCE_CONTROL_BIN).isFile(),'source_binary_required');
    assert.ok(/^127\.0\.0\.1:[0-9]+$/.test(env.UR_IPC_SOURCE_CONTROL_RPC || ''),'source_loopback_required');
    const rpcPort = Number(env.UR_IPC_SOURCE_CONTROL_RPC.split(':')[1]);
    assert.ok(rpcPort >= 1024 && rpcPort <= 65535,'source_port_required');
    assert.ok(/^[A-Za-z0-9_-]{1,50}$/.test(env.UR_IPC_TENANT || ''),'source_tenant_required');
  }
  return port;
}

// run 设备身份仅交给C++；应用走正式鉴权API创建真实会议，失败也回收当前任务资源。
async function run(options = {}) {
  validateTalkOptions(options);
  const port = validateConfiguration(process.env);
  const previews = previewCount(process.env.UR_IPC_TALK_PREVIEW_COUNT); // 默认保留原单腿验收，组合必须显式选择三路。
  const env = {...process.env,AGENT_BROWSER_ARGS:[process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--mute-audio',
    '--autoplay-policy=no-user-gesture-required',`--use-file-for-fake-audio-capture=${process.env.UR_IPC_AUDIO_FIXTURE}`].join(',')};
  const device = `ipc-talk-device-${randomUUID()}`, app = `ipc-talk-app-${randomUUID()}`;
  const server = http.createServer((_,response) => {
    response.writeHead(200,{'content-type':'text/html','cache-control':'no-store'});
    response.end('<!doctype html><html><body>会议对讲验收<video id="remote" autoplay muted playsinline></video></body></html>');
  });
  let child, completion, exited = false, childExitCode = null, deadline, heartbeat, heartbeatFlight, heartbeatFailed = false;
  let outcome, safetyEnded = false, restoredMediaNotResumed = null; // 仅原会话持久安全事件能证明平台主动结束。
  const cleanupFailures = []; // 固定资源分类，不保存原端点、私有响应或底层错误。
  const openedBrowsers = new Set(); // 只关闭本次尝试启动的具名会话，失败收尾不额外创建浏览器。
  let endpointId, sessionId, callId, conversationId, state, stage = 'setup';
  let previewGroup, previewFacts, previewReleased; // 三观看者使用独立端点，但设备和安全触发器必须唯一。
  // api 只返回成功数据，不输出身份、SDP、后端自由错误或令牌。
  async function api(path,body = {}) {
    const response = await fetch(`${process.env.UR_IPC_BASE_URL}/api/v1/${path}`,{method:'POST',
      headers:{'content-type':'application/json','app-id':process.env.UR_IPC_APP_ID || '100',
        'tenant-code':process.env.UR_IPC_TENANT || 'platform',token:process.env.UR_IPC_APP_TOKEN},
      body:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
    const result = await response.json();
    assert.ok(response.status === 200 && result.code === 200,`api_${path.replaceAll('/','_')}_rejected`);
    return result.data;
  }
  // receive 先确认实际outbox投递；只匹配当前端点/会话/原请求，不把202当作设备完成。
  async function receive(token) {
    const received = await api('im/interaction/events',{endpointId,limit:32});
    let result;
    for (const delivery of received.list || []) {
      const event = delivery.event;
      if (event?.method === 'interaction.state' && event.sessionId === sessionId) state = event.data?.state;
      if (options.safety && safetyTerminal(event, sessionId, options.safety.cause)) safetyEnded = true;
      if (event?.method === 'interaction.result' && event.msgToken === token) result = event;
    }
    if (received.list?.length) await api('im/interaction/events/ack',{endpointId,
      deliveryIds:received.list.map(value => value.deliveryId)});
    return result;
  }
  // command 以真实请求token关联持久结果，可传同token验证幂等重试。
  async function command(method,params,token = randomUUID()) {
    const command = {method,msgToken:token,params};
    if (method === 'interaction.open') command.target = {productID:process.env.UR_DEVICE_PRODUCT,deviceName:process.env.UR_DEVICE_NAME};
    else command.sessionId = sessionId;
    const accepted = await api('im/interaction/commands',{endpointId,command});
    assert.equal(accepted.accepted,true); assert.equal(accepted.msgToken,token);
    let result;
    await waitFor(async () => { result = await receive(token); return !!result; });
    assert.equal(result.code,200,'actual_operation_rejected');
    return result;
  }
  try {
    // MQTT先监听不表示IM/HTTP已经就绪；先验证正式鉴权端点，避免把启动中的平台当协议失败。
    stage = 'backend_ready';
    const self = await api('system/user/self/get-one');
    assert.ok(self.userID,'authenticated_user_required');
    await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
    child = spawn(process.env.UR_IPC_DEVICE_BIN,[String(port)],{env:process.env,stdio:['ignore','pipe','pipe']});
    completion = new Promise(resolve => child.once('close',code => {exited = true;childExitCode = code;resolve();}));
    child.stderr.on('data',() => {});
    deadline = setTimeout(() => child.kill('SIGTERM'),180000);
    await new Promise((resolve,reject) => {
      const timeout = setTimeout(() => reject(Error('device_start_timeout')),10000);
      child.once('error',() => {clearTimeout(timeout);reject(Error('device_spawn_failed'));});
      child.stdout.on('data',bytes => {if(bytes.toString().includes(`http://127.0.0.1:${port}/`)){clearTimeout(timeout);resolve();}});
    });
    stage = 'device_online';
    openedBrowsers.add(device);
    await browser(device,env,'open',`http://127.0.0.1:${port}/`);
    await browser(device,env,'snapshot','-i');
    await browser(device,env,'find','role','button','click','--name','连接平台');
    await waitFor(async () => evaluated(await browser(device,env,'eval','available === true && closed === false')) === true);
    if (options.onReady) {
      stage = 'device_property_baseline';
      await options.onReady({deviceBrowser: device, env});
    }
    if (previews) {
      stage = 'combined_three_previews';
      previewGroup = createPreviewGroup({api, env, device, pageURL: `http://127.0.0.1:${server.address().port}/`,
        openedBrowsers, waitFor, cause: options.safety?.cause});
      await previewGroup.start();
    }
    stage = 'real_im_membership';
    const conversation = await api('im/conversation/create',{type:'group',name:`IPC验收-${randomUUID()}`,
      participants:[{type:'user',id:String(self.userID)},
        {type:'device',id:`${process.env.UR_DEVICE_PRODUCT}/${process.env.UR_DEVICE_NAME}`} ]});
    conversationId = conversation.id; assert.ok(conversationId);
    const call = await api('im/call/signal',{type:'group_invite',callType:'voice',conversationId,members:[]});
    callId = call.callId; assert.ok(callId);
    // 心跳串行执行，失败事实参与验收；关闭先取消调度，不保留背景续租。
    const touch = async () => {
      try {await api('im/call/signal',{type:'call_heartbeat',callId});}
      catch {heartbeatFailed = true;}
      if (heartbeat !== null) heartbeat = setTimeout(tick,5000);
    };
    const tick = () => {heartbeatFlight = touch();}; // 记录在途原心跳，收尾必须等它退出再离会。
    heartbeat = setTimeout(tick,5000);
    endpointId = (await api('im/interaction/endpoints/register')).endpointId;
    assert.ok(endpointId);
    openedBrowsers.add(app);
    await browser(app,env,'open',`http://127.0.0.1:${server.address().port}/`);
    // 用户端单音频sendrecv；设备端mic/sendonly和speaker/recvonly来自正式C++配置。
    const offer = evaluated(await browser(app,env,'eval',`(async()=>{
      const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});
      window.__capture=await navigator.mediaDevices.getUserMedia({audio:true,video:false});
      const transceiver=peer.addTransceiver(window.__capture.getAudioTracks()[0],{direction:'sendrecv',streams:[window.__capture]});
      transceiver.setCodecPreferences(RTCRtpSender.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()==='audio/opus'));
      peer.ontrack=e=>{window.__remoteTrack=e.track;(${attachListeningAudio.toString()})(e.track);};
      await peer.setLocalDescription(await peer.createOffer());
      if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
        const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
        peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);
    })()`));
    stage = 'actual_talk_invitation';
    const token = randomUUID(), params = {purpose:'ipcTalk',cameraId:'front',callId};
    const opened = await command('interaction.open',params,token);
    sessionId = opened.sessionId; assert.ok(sessionId);
    stage = 'idempotent_open';
    // 原结果已ACK，重试只返回Replay受理标记，不要求通知重新投递；设备不得新增邀请或Peer。
    const repeated = await api('im/interaction/commands',{endpointId,command:{method:'interaction.open',msgToken:token,
      params,target:{productID:process.env.UR_DEVICE_PRODUCT,deviceName:process.env.UR_DEVICE_NAME}}});
    assert.equal(repeated.accepted,true);assert.equal(repeated.msgToken,token);assert.equal(repeated.replay,true);
    stage = 'actual_local_consent';
    await confirmAdmission('accept',sessionId,(...args) => browser(device,env,...args),'ipcTalk');
    // 按真实设备接听后的配置事实推进，不能在本地按钮刚移除时抢跑平台媒体协商。
    await waitFor(async () => evaluated(await browser(device,env,'eval',
      `!!views.get(${JSON.stringify(sessionId)})?.peer?.localDescription`)) === true);
    stage = 'real_media_negotiation';
    const answer = await command('interaction.mediaOffer',{mediaRevision:1,webrtc:{iceMode:'complete',sdp:offer}});
    assert.equal(answer.data?.mediaRevision,1);
    await browser(app,env,'eval',`(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answer.data.webrtc.sdp)}});return true;})()`);
    await waitFor(async () => evaluated(await browser(app,env,'eval','__ipcPeer.connectionState === "connected"')) === true);
    await command('interaction.mediaState',{mediaRevision:1,state:'connected'});
    stage = 'actual_established_and_decode';
    await waitFor(async () => {await receive();return state === 'established';});
    let deviceStats, appStats, talkStats;
    await waitFor(async () => {
      deviceStats = evaluated(await browser(device,env,'eval','(async()=>JSON.stringify(await ipcStats()))()'));
      appStats = evaluated(await browser(app,env,'eval',`(async()=>{let packets=0,samples=0,sent=0;
        (await __ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio'){packets+=s.packetsReceived||0;
          if(Number.isFinite(s.totalSamplesReceived)&&Number.isFinite(s.concealedSamples))samples+=Math.max(0,s.totalSamplesReceived-s.concealedSamples);}
          if(s.type==='outbound-rtp'&&s.kind==='audio')sent+=s.packetsSent||0;});return JSON.stringify({packets,samples,sent,peak:__ipcAudio?.peak||0,run:__ipcAudio?.maxRun||0});})()`));
      const d = talkStats = deviceStats.find(row => row.sessionId === sessionId);
      return d?.talk && d.active && d.playing && d.audioSent > 2 && d.audioReceived > 2 && d.audioSamples > 1920 &&
        d.audioEnergy > 0 && appStats.samples > 1920 && appStats.peak > 0.001 && appStats.run >= 2;
    },25000);
    assert.equal(deviceStats.length,1 + previews); assert.equal(talkStats.frames,0);assert.equal(heartbeatFailed,false);
    if (previewGroup) {
      stage = 'combined_actual_decode';
      previewFacts = await previewGroup.verifyConcurrent();
    }
    let speakerFacts;
    if (listeningMode(process.env.UR_IPC_SPEAKER_CONTROL)) {
      stage = 'formal_speaker_pause';
      const source = request => sourceControlRPC(process.env.UR_IPC_SOURCE_CONTROL_BIN,process.env,{sessionId,mediaRevision:1,...request});
      const speakerTrack = evaluated(await browser(device,env,'eval',`(()=>{const view=views.get(${JSON.stringify(sessionId)});
        window.__speakerResources={peer:view.peer,mic:view.stream.getAudioTracks()[0]};return JSON.stringify(view.speakerTrack);})()`));
      const pause = {trackId:speakerTrack,operation:'pause',msgToken:randomUUID()};
      await source(pause);
      await waitFor(async () => evaluated(await browser(device,env,'eval',`(()=>{const v=views.get(${JSON.stringify(sessionId)});
        return !!v?.speakerPaused && !v.playing && v.sink.paused && !v.remoteTrack.enabled;})()`)) === true);
      const pauseStats = evaluated(await browser(device,env,'eval','(async()=>JSON.stringify((await ipcStats())[0]))()'));
      // 新窗口要求实际接收端解码PCM，不以设备仍发送RTP推断后端仍接收/混音。
      const appDecoded = async () => evaluated(await browser(app,env,'eval',`(async()=>{let samples=0;
        (await __ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio'&&Number.isFinite(s.totalSamplesReceived)&&Number.isFinite(s.concealedSamples))samples+=Math.max(0,s.totalSamplesReceived-s.concealedSamples);});
        return JSON.stringify({samples,peak:__ipcAudio.peak,run:__ipcAudio.maxRun});})()`));
      const appBefore = await appDecoded();
      await browser(app,env,'eval','__ipcAudio.peak=0;__ipcAudio.maxRun=0;__ipcAudio.run=0;true');
      await source(pause); // 原token重放不能重建Peer或解除暂停。
      await assert.rejects(source({trackId:speakerTrack,operation:'stop',msgToken:randomUUID()}));
      await waitFor(async () => {
        deviceStats = evaluated(await browser(device,env,'eval','(async()=>JSON.stringify(await ipcStats()))()'));
        const decoded = await appDecoded();
        return deviceStats[0].audioSent >= pauseStats.audioSent + 8 && decoded.samples > appBefore.samples + 1920 && decoded.peak > 0.001 && decoded.run >= 2;
      });
      const independent = evaluated(await browser(device,env,'eval',`(()=>{const v=views.get(${JSON.stringify(sessionId)});
        return JSON.stringify({paused:v.speakerPaused&&v.sink.paused&&!v.playing,peerSame:v.peer===__speakerResources.peer,
          micSame:v.stream.getAudioTracks()[0]===__speakerResources.mic,micLive:__speakerResources.mic.readyState==='live'&&__speakerResources.mic.enabled});})()`));
      assert.deepEqual(independent,{paused:true,peerSame:true,micSame:true,micLive:true});
      const beforeResume = deviceStats[0];
      stage = 'formal_speaker_resume';
      await source({trackId:speakerTrack,operation:'resume',msgToken:randomUUID()});
      await waitFor(async () => {
        deviceStats = evaluated(await browser(device,env,'eval','(async()=>JSON.stringify(await ipcStats()))()'));
        const d = deviceStats[0];
        return d.playing && d.audioSamples > beforeResume.audioSamples + 1920 && d.audioEnergy > beforeResume.audioEnergy;
      });
      speakerFacts = {verified:true,pauseUpstreamPackets:beforeResume.audioSent-pauseStats.audioSent,
        resumeSamples:deviceStats[0].audioSamples-beforeResume.audioSamples,independent};
    }
    stage = 'actual_hangup_release';
    await browser(device,env,'eval',`(()=>{const view=views.get(${JSON.stringify(sessionId)});window.__resources={peer:view.peer,
      capture:view.stream.getTracks(),remote:view.remoteTrack,sink:view.sink};
      window.__ipcAllTracks=[...views.values()].flatMap(v=>v.stream.getTracks());
      window.__ipcAllPeers=[...views.values()].map(v=>v.peer);return true;})()`);
    if (options.safety) {
      stage = 'formal_safety_stop';
      await options.safety.trigger();
      await waitFor(async () => {await receive();return safetyEnded && state === 'ended';});
      if (previewGroup) await previewGroup.waitSafety();
    } else {
      await command('interaction.end',{reason:'userStopped'});
      if (previewGroup) await previewGroup.end();
    }
    await waitFor(async () => evaluated(await browser(device,env,'eval','views.size === 0 && invitations.size === 0')) === true);
    const released = evaluated(await browser(device,env,'eval',`(()=>{const r=__resources;return JSON.stringify({peerClosed:r.peer.connectionState==='closed',
      captureStopped:r.capture.every(t=>t.readyState==='ended'),remoteStopped:r.remote.readyState==='ended',outputReleased:r.sink.srcObject===null&&r.sink.paused});})()`));
    assert.deepEqual(released,{peerClosed:true,captureStopped:true,remoteStopped:true,outputReleased:true});
    const allStopped = async () => evaluated(await browser(device,env,'eval',
      `__ipcAllPeers.length===${1 + previews}&&__ipcAllPeers.every(p=>p.connectionState==='closed')&&` +
      `__ipcAllTracks.length>=${1 + previews}&&__ipcAllTracks.every(t=>t.readyState==='ended')`));
    assert.equal(await allStopped(),true,'all_original_device_media_stop_required');
    if (options.safety?.restore) {
      await options.safety.restore();
      const stillStopped = async () => evaluated(await browser(device,env,'eval',`(()=>{const r=__resources;
        return views.size===0&&invitations.size===0&&r.peer.connectionState==='closed'&&r.capture.every(t=>t.readyState==='ended')&&
          r.remote.readyState==='ended'&&r.sink.srcObject===null&&r.sink.paused;})()`));
      assert.equal(await stillStopped(),true,'restored_media_must_stay_stopped');
      assert.equal(await allStopped(),true,'all_restored_device_media_must_stay_stopped');
      await new Promise(resolve => setTimeout(resolve,1000));
      assert.equal(await stillStopped(),true,'restored_media_must_stay_stopped');
      assert.equal(await allStopped(),true,'all_restored_device_media_must_stay_stopped');
      restoredMediaNotResumed = true;
    }
    // 应用客户端在自己的成功挂断后停止自身驱动；远端关闭不能代替本端采集和分析图释放。
    const appReleased = evaluated(await browser(app,env,'eval',`(async()=>{await __ipcAudio.stop();__capture.getTracks().forEach(t=>t.stop());
      __remoteTrack.stop();__ipcPeer.close();return JSON.stringify({captureStopped:__capture.getTracks().every(t=>t.readyState==='ended'),
        remoteStopped:__remoteTrack.readyState==='ended',peerClosed:__ipcPeer.connectionState==='closed',
        audioClosed:__ipcAudio.context.state==='closed',sinkReleased:__ipcAudio.sink.srcObject===null});})()`));
    assert.deepEqual(appReleased,{captureStopped:true,remoteStopped:true,peerClosed:true,audioClosed:true,sinkReleased:true});
    if (previewGroup) previewReleased = await previewGroup.release();
    const {sessionId: ignoredSession,...deviceFacts} = talkStats; // 报告仅保留数量与布尔状态，不输出会话标识。
    outcome = {result:'pass',path:'cpp_mqtt_real_im',device:deviceFacts,application:appStats,speaker:speakerFacts,
      safetyStopVerified:options.safety ? options.safety.cause : null,restoredMediaNotResumed,released,appReleased,
      previews:previewFacts,previewReleased};
  } catch (error) {
    // 只记录自有固定阶段/错误码；断言差异、后端正文与浏览器自由日志不进入报告。
    let deviceFacts;
    try {deviceFacts = evaluated(await browser(device,env,'eval','JSON.stringify({closed,available,views:views.size,invitations:invitations.size})'));}catch {}
    outcome = {result:'fail',stage,deviceFacts,deviceExited:exited,
      reason:/^[a-z_]+(?:_[0-9]{1,2})?$/.test(error.message) ? error.message : 'assertion_or_execution_failed'};process.exitCode = 1;
  } finally {
    clearTimeout(deadline);clearTimeout(heartbeat);heartbeat = null;
    const {bounded, stopChild} = require('./conference_member_live.cjs');
    if (heartbeatFlight) await bounded(heartbeatFlight,16000,'heartbeat_close_timeout').catch(() => cleanupFailures.push('original_heartbeat'));
    if (heartbeatFailed) cleanupFailures.push('original_heartbeat');
    if (options.safety?.restore) try {await options.safety.restore();} catch {cleanupFailures.push('safety_restore');}
    if (previewGroup) await previewGroup.end().catch(() => cleanupFailures.push('preview_sessions'));
    if (sessionId && endpointId) await command('interaction.end',{reason:'userStopped'}).catch(() => {});
    if (callId) await api('im/call/signal',{type:'group_leave',callId}).catch(() => cleanupFailures.push('original_call'));
    if (conversationId) await api('im/conversation/disband',{conversationId}).catch(() => cleanupFailures.push('original_conversation'));
    if (openedBrowsers.has(app)) await browser(app,env,'eval','(async()=>{if(window.__ipcAudio)await __ipcAudio.stop();window.__capture?.getTracks().forEach(t=>t.stop());window.__ipcPeer?.close();return true;})()').catch(() => cleanupFailures.push('application_media'));
    for (const session of openedBrowsers) await browser(session,env,'close').catch(() => cleanupFailures.push('named_browser'));
    if (completion && !exited) await bounded(completion, 3000, 'device_close_timeout').catch(() => cleanupFailures.push('child_close_timeout'));
    await stopChild(child, completion, () => exited).catch(() => cleanupFailures.push('original_child'));
    if (child && (!exited || childExitCode !== 0)) cleanupFailures.push('child_exit');
    if(server.listening) await bounded(new Promise((resolve,reject) => server.close(error => error ? reject(error) : resolve())),
      5000,'listener_close_timeout').catch(() => cleanupFailures.push('local_listener'));
  }
  if (cleanupFailures.length) {outcome = {...outcome, result:'fail',stage:'cleanup'};process.exitCode = 1;}
  outcome.cleanup = {childClosed:exited,childExitCode,browsersClosed:!cleanupFailures.includes('named_browser'),
    listenerClosed:!server.listening,failures:cleanupFailures};
  console.log(JSON.stringify(outcome)); return outcome;
}
module.exports = {waitFor,validateConfiguration,validateTalkOptions,run};
if(require.main === module)run().catch(() => {console.log(JSON.stringify({result:'fail',stage:'configuration'}));process.exitCode = 1;});
