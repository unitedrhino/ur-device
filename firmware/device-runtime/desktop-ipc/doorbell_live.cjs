// 文件说明：显式门铃真实后端E2E；原事件→多端首答→源明确接听→双向PCM/单向视频及原资源回收。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {browser, evaluated, attachListeningAudio} = require('./ipc_live.cjs');
const {setupDoorbellIM} = require('./im_binding_setup.cjs');
const {bounded,stopChild} = require('./conference_member_live.cjs'); // 复用已测试的精确对象退出屏障。

// validateConfiguration 在任何进程或网络副作用前核验本机隔离和显式源驱动配置。
function validateConfiguration(env, isFile = value => fs.statSync(value).isFile()) {
  assert.equal(env.UR_DOORBELL_LIVE, '1', 'explicit_doorbell_live_required');
  assert.ok(env.UR_DOORBELL_CONCURRENT_ACCEPT === undefined || env.UR_DOORBELL_CONCURRENT_ACCEPT === '1',
    'explicit_concurrent_answer_required');
  const url = new URL(env.UR_IPC_BASE_URL);
  assert.ok(url.protocol === 'http:' && url.hostname === '127.0.0.1' && !url.username && !url.password &&
    url.pathname === '/' && !url.search && !url.hash, 'local_backend_required');
  assert.ok(env.UR_DOORBELL_NEGATIVE_CASES === undefined || env.UR_DOORBELL_NEGATIVE_CASES === '1',
    'explicit_negative_cases_required');
  const port = Number(env.UR_IPC_DEVICE_PORT);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535, 'device_port_required');
  for (const key of ['UR_IPC_DEVICE_BIN', 'UR_IPC_AUDIO_FIXTURE'])
    assert.ok(env[key] && isFile(env[key]), 'binary_or_pcm_fixture_required');
  for (const key of ['UR_IPC_APP_TOKEN', 'UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET',
    'UR_IPC_TENANT', 'UR_IPC_APP_ID', 'UR_IPC_CAMERA_ID', 'UR_IPC_DOORBELL_EVENT'])
    assert.ok(typeof env[key] === 'string' && env[key].length && !/[\x00-\x1f\x7f]/.test(env[key]), 'injected_scope_required');
  for (const key of ['UR_IPC_HUMAN_CALL', 'UR_IPC_CONFERENCE_TALK', 'UR_IPC_MICROPHONE'])
    assert.equal(env[key], '1', 'actual_shared_driver_required');
  return port;
}

// validateInvite 按当前墙钟now核验原候选、端点和相机；过期/旧单轨不能成为新一轮接听许可。
function validateInvite(event, endpoint, camera, now = Date.now()) {
  assert.equal(event?.method, 'interaction.invite', 'doorbell_invitation_required');
  assert.ok(typeof event.msgToken === 'string' && event.msgToken.length > 0, 'original_invite_token_required');
  assert.ok(typeof event.sessionId === 'string' && event.sessionId.length > 0 && event.sessionId.length <= 64, 'candidate_leg_required');
  assert.equal(event.data?.endpointId, endpoint, 'original_endpoint_required');
  const invite = event.data?.invitation;
  assert.ok(invite && invite.purpose === 'humanCall' && invite.callType === 'video' &&
    invite.admission === 'accept' && invite.transport === 'webrtc', 'explicit_human_consent_required');
  assert.ok(Number.isSafeInteger(now) && now >= 0 && typeof invite.callGroupId === 'string' &&
    invite.callGroupId.length > 0 && invite.callGroupId === event.data.callGroupId &&
    Number.isSafeInteger(invite.expiresAt) && invite.expiresAt > now &&
    typeof invite.sourceEventId === 'string' && invite.sourceEventId.length > 0, 'original_group_required');
  assert.ok(Array.isArray(invite.tracks) && invite.tracks.length === 3, 'full_three_tracks_required');
  const expected = [['mic-1', 'audio', 'mic', 'sendonly', 'opus'],
    ['speaker-1', 'audio', 'speaker', 'recvonly', 'opus'], ['video-1', 'video', camera, 'recvonly', 'H264']];
  invite.tracks.forEach((track, index) => {
    const [id, kind, source, direction, codec] = expected[index];
    assert.ok(track.trackId === id && track.kind === kind && track.sourceId === source &&
      track.direction === direction && track.codec === codec && track.state === 'requested', 'original_tracks_required');
    const keys = ['trackId', 'kind', 'sourceId', 'direction', 'codec', 'state'];
    if (index === 2) {keys.push('sourceType'); assert.equal(track.sourceType, 'camera', 'original_video_source_type_required');}
    assert.deepEqual(Object.keys(track).sort(), keys.sort(), 'strict_application_track_required');
  });
  return event;
}

// mediaPass 仅接受实际解码音频PCM和视频像素；RTP、SDP、202或单一connected都不足以通过。
function mediaPass(device, app, sourceAudio) {
  return !!(device?.human && device.active && device.connected && device.playing && device.frames >= 2 &&
    device.audioSent > 2 && device.audioReceived > 2 && device.audioSamples > 1920 && device.audioEnergy > 0 &&
    app?.connected && app.samples > 1920 && app.sent > 2 && app.frames >= 2 && app.nonBlackPixels > 0 &&
    app.closed === false && app.failed === false && app.peak > 0.001 && app.run >= 2 &&
    sourceAudio?.closed === false && sourceAudio.failed === false &&
    sourceAudio.peak > 0.001 && sourceAudio.run >= 2);
}

// uniqueAnswer 只接受三个正式结果恰好一胜两负；返回原端点下标，不按提交先后预定胜者。
function uniqueAnswer(results) {
  assert.ok(Array.isArray(results) && results.length === 3, 'three_original_answers_required');
  const winners = [];
  for (const [index, result] of results.entries()) {
    if (result?.code === 200) {
      assert.equal(result.data?.result?.state, 'negotiating', 'winner_negotiating_required');
      winners.push(index);
    } else {
      assert.equal(result?.code, 409, 'loser_cannot_allocate_media');
      assert.equal(result.data?.reason, 'alreadyAnswered', 'original_loser_reason_required');
    }
  }
  assert.equal(winners.length, 1, 'exactly_one_actual_winner_required');
  return winners[0];
}

// waitFor 等待真实端点事实，截止不因重试延长，失败阶段由外层保留。
async function waitFor(predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw Error('doorbell_fact_timeout');
}

// run 使用正式鉴权API和原C++事件出口，两个连续完整轮次证明原实际停止后可以安全重新预约。
async function run() {
  const port = validateConfiguration(process.env);
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio',
    '--autoplay-policy=no-user-gesture-required', `--use-file-for-fake-audio-capture=${process.env.UR_IPC_AUDIO_FIXTURE}`].join(',')};
  const source = `task143-doorbell-source-${randomUUID()}`, app = `task143-doorbell-app-${randomUUID()}`;
  const server = http.createServer((_, response) => {
    response.writeHead(200, {'content-type': 'text/html', 'cache-control': 'no-store'});
    response.end('<!doctype html><html><body><video id="remote" autoplay playsinline muted></video></body></html>');
  });
  let child, completion, observer, exited = false, exitCode = null, deadline, stage = 'configuration';
  let successful = false; // 业务通过之后仍须完成所有清理，不能提前输出pass。
  const cleanupFailures = []; // 只记录固定资源类型，禁止输出正文、令牌或任意进程标识。
  const endpoints = [], events = new Map(), rounds = [], previousGroups = new Set(); // 本任务两轮原组不可混用。
  let winnerSession = '', winnerEndpoint = '', lastFacts = {};
  // api 不把后端正文、身份和SDP写报告，错误只保留固定自有枚举。
  async function api(path, body = {}) {
    const response = await fetch(`${process.env.UR_IPC_BASE_URL}/api/v1/${path}`, {method: 'POST',
      headers: {'content-type': 'application/json', token: process.env.UR_IPC_APP_TOKEN,
        'tenant-code': process.env.UR_IPC_TENANT, 'app-id': process.env.UR_IPC_APP_ID},
      body: JSON.stringify(body), signal: AbortSignal.timeout(15000)});
    const result = await response.json();
    assert.ok(response.status === 200 && result.code === 200, 'doorbell_api_rejected');
    return result.data;
  }
  // receive 保留原通知事实后再ACK，仅本测试注册端点；结果缓存有界不无限积压。
  async function receive(endpoint) {
    const data = await api('im/interaction/events', {endpointId: endpoint, limit: 32});
    assert.ok(Array.isArray(data.list) && data.list.length <= 32, 'bounded_events_required');
    for (const delivery of data.list) {
      const event = delivery.event;
      assert.ok(event && typeof event.msgToken === 'string', 'event_envelope_required');
      events.set(`${endpoint}:${event.method}:${event.msgToken}`, event);
      if (events.size > 128) events.delete(events.keys().next().value);
    }
    if (data.list.length) await api('im/interaction/events/ack', {endpointId: endpoint,
      deliveryIds: data.list.map(item => item.deliveryId)});
  }
  // command 以原token关联持久结果；接听必须原invite token，不能换身份假重试。
  async function command(endpoint, session, method, params, token = randomUUID()) {
    const accepted = await api('im/interaction/commands', {endpointId: endpoint,
      command: {method, msgToken: token, sessionId: session, params}});
    assert.equal(accepted.accepted, true); assert.equal(accepted.msgToken, token);
    let result;
    await waitFor(async () => {await receive(endpoint); result = events.get(`${endpoint}:interaction.result:${token}`); return !!result;});
    return result;
  }
  try {
    stage = 'formal_im_setup';
    await setupDoorbellIM({live: '1', base: process.env.UR_IPC_BASE_URL, token: process.env.UR_IPC_APP_TOKEN,
      tenant: process.env.UR_IPC_TENANT, appId: process.env.UR_IPC_APP_ID,
      productId: process.env.UR_DEVICE_PRODUCT, deviceName: process.env.UR_DEVICE_NAME});
    for (let index = 0; index < 3; index++) endpoints.push((await api('im/interaction/endpoints/register')).endpointId);
    assert.equal(new Set(endpoints).size, 3, 'three_independent_endpoints_required');
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    if(process.env.UR_DOORBELL_NEGATIVE_CASES==='1')observer=await require('./doorbell_event_observer.cjs').createObserver({targetPort:Number(process.env.UR_MQTT_PORT),
      product:process.env.UR_DEVICE_PRODUCT,device:process.env.UR_DEVICE_NAME,event:process.env.UR_IPC_DOORBELL_EVENT});
    child = spawn(process.env.UR_IPC_DEVICE_BIN, [String(port)], {env: {...process.env,UR_MQTT_PORT:observer?String(observer.port):process.env.UR_MQTT_PORT}, stdio: ['ignore', 'pipe', 'pipe']});
    child.stderr.on('data', () => {});
    completion = new Promise(resolve => child.once('close', code => {exited = true; exitCode = code; resolve();}));
    deadline = setTimeout(() => child.kill('SIGTERM'), 240000);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('doorbell_device_start_timeout')), 10000);
      child.once('error', () => {clearTimeout(timer); reject(Error('doorbell_device_spawn_failed'));});
      child.stdout.on('data', bytes => {if (bytes.toString().includes(`http://127.0.0.1:${port}/`)) {clearTimeout(timer); resolve();}});
    });
    stage = 'shared_device_available';
    await browser(source, env, 'open', `http://127.0.0.1:${port}/`);
    await browser(source, env, 'click', '#activate');
    await waitFor(async () => evaluated(await browser(source, env, 'eval', 'available && doorbellAvailable && !closed')) === true);
    await browser(app, env, 'open', `http://127.0.0.1:${server.address().port}/`);
    for (let round = 0; round < 2; round++) {
      events.clear();
      lastFacts = {round: round + 1}; // 新轮次失败不能挂载上一轮历史媒体事实。
      stage = 'original_device_event';
      await browser(source, env, 'click', '#doorbell');
      const invitations = new Map();
      await waitFor(async () => {
        for (const endpoint of endpoints) {
          await receive(endpoint);
          for (const event of events.values()) if (event.method === 'interaction.invite' && event.data?.endpointId === endpoint)
            invitations.set(endpoint, validateInvite(event, endpoint, process.env.UR_IPC_CAMERA_ID));
        }
        return invitations.size === 3;
      });
      const invite = invitations.get(endpoints[0]);
      assert.equal(new Set([...invitations.values()].map(item => item.sessionId)).size, 3, 'independent_candidate_legs_required');
      assert.equal(new Set([...invitations.values()].map(item => item.data.callGroupId)).size, 1, 'shared_original_group_required');
      assert.equal(new Set([...invitations.values()].map(item => item.data.invitation.sourceEventId)).size, 1, 'shared_original_event_required');
      assert.equal(previousGroups.has(invite.data.callGroupId), false, 'new_press_requires_new_group');
      previousGroups.add(invite.data.callGroupId);
      let originalEventReplayVerified=false;
      if(observer&&round===0){
        stage='original_event_replay';
        await waitFor(async()=>observer.facts().success===1);
        observer.replayLatestOriginal();
        await waitFor(async()=>observer.facts().duplicateReplies===1);
        assert.equal(observer.facts().submitted,1);assert.equal(observer.facts().failed,false);
        // 原事件第二个正式回执在业务分发之后返回；逐端排空持久收件箱，不能再生成另一组或另一条邀请腿。
        for(const endpoint of endpoints)await receive(endpoint);
        const received=[...events.values()].filter(event=>event.method==='interaction.invite');
        assert.equal(received.length,3,'duplicate_event_must_not_create_invites');
        for(const event of received){const original=invitations.get(event.data.endpointId);assert.ok(original);
          assert.equal(event.sessionId,original.sessionId);assert.equal(event.msgToken,original.msgToken);
          assert.equal(event.data.callGroupId,original.data.callGroupId);}
        originalEventReplayVerified=true;
      }
      assert.equal(evaluated(await browser(source, env, 'eval', 'views.size === 0')), true, 'no_media_before_source_accept');
      stage = 'application_first_answer';
      const concurrent = process.env.UR_DOORBELL_CONCURRENT_ACCEPT === '1';
      // 并发模式向正式HTTP同时提交三个原邀请；保留原token、会话、返回及后端唯一胜者编排。
      const answerCandidate = endpoint => {
        const invitation = invitations.get(endpoint);
        return command(endpoint, invitation.sessionId, 'interaction.control',
          {command: 'sessionAccept', arguments: {acceptedTracks: ['mic-1', 'speaker-1', 'video-1']}}, invitation.msgToken);
      };
      const answers = concurrent ? await Promise.all(endpoints.map(answerCandidate)) :
        [await answerCandidate(endpoints[0]), await answerCandidate(endpoints[1]), await answerCandidate(endpoints[2])];
      const winnerIndex = uniqueAnswer(answers);
      winnerEndpoint = endpoints[winnerIndex]; winnerSession = invitations.get(winnerEndpoint).sessionId;
      for (const endpoint of endpoints.filter(value => value !== winnerEndpoint)) {
        const loser = invitations.get(endpoint);
        await waitFor(async () => {
          await receive(endpoint);
          return [...events.entries()].some(([key, event]) => key.startsWith(`${endpoint}:`) &&
            event.method === 'interaction.state' && event.sessionId === loser.sessionId &&
            event.data?.state === 'ended' && event.data.reason === 'answeredElsewhere');
        });
      }
      stage = 'explicit_original_source_consent';
      await waitFor(async () => evaluated(await browser(source, env, 'eval', '!!humanSession && invitations.has(humanSession) && !views.has(humanSession)')) === true);
      await browser(source, env, 'find', 'role', 'button', 'click', '--name', '接听本次人际通话');
      await waitFor(async () => evaluated(await browser(source, env, 'eval', '!!views.get(humanSession)?.peer?.localDescription')) === true);
      stage = 'original_webrtc_media';
      const offer = evaluated(await browser(app, env, 'eval', `(async()=>{
        window.__callPeer=new RTCPeerConnection({iceServers:[]});
        window.__capture=await navigator.mediaDevices.getUserMedia({audio:true,video:false});
        __callPeer.addTransceiver(__capture.getAudioTracks()[0],{direction:'sendrecv',streams:[__capture]});
        const video=__callPeer.addTransceiver('video',{direction:'recvonly'});
        video.setCodecPreferences(RTCRtpReceiver.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/h264'&&(c.sdpFmtpLine||'').includes('packetization-mode=1')));
        window.__remoteTracks=[];__callPeer.ontrack=e=>{__remoteTracks.push(e.track);
          if(e.track.kind==='audio')(${attachListeningAudio.toString()})(e.track);
          else {const sink=document.getElementById('remote');sink.srcObject=new MediaStream([e.track]);void sink.play();}};
        await __callPeer.setLocalDescription(await __callPeer.createOffer());
        if(__callPeer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
          const check=()=>{if(__callPeer.iceGatheringState==='complete'){clearTimeout(timer);__callPeer.removeEventListener('icegatheringstatechange',check);resolve();}};
          __callPeer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(__callPeer.localDescription.sdp);
      })()`));
      const answer = await command(winnerEndpoint, winnerSession, 'interaction.mediaOffer',
        {mediaRevision: 1, webrtc: {iceMode: 'complete', sdp: offer}});
      assert.equal(answer.code, 200); assert.equal(answer.data?.mediaRevision, 1);
      assert.ok(typeof answer.data.webrtc?.sdp === 'string', 'actual_answer_required');
      await browser(app, env, 'eval', `(async()=>{await __callPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answer.data.webrtc.sdp)}});return true;})()`);
      await waitFor(async () => evaluated(await browser(app, env, 'eval', '__callPeer.connectionState === "connected"')) === true);
      assert.equal((await command(winnerEndpoint, winnerSession, 'interaction.mediaState', {mediaRevision: 1, state: 'connected'})).code, 200);
      await waitFor(async () => evaluated(await browser(source, env, 'eval', '!!views.get(humanSession)?.remoteTrack')) === true);
      await browser(source, env, 'eval', `(()=>{(${attachListeningAudio.toString()})(views.get(humanSession).remoteTrack);return true;})()`);
      stage = 'actual_pcm_and_video_pixels';
      let sourceFacts, appFacts, sourceAudio;
      await waitFor(async () => {
        sourceFacts = evaluated(await browser(source, env, 'eval', '(async()=>JSON.stringify((await ipcStats()).find(s=>s.human)))()'));
        sourceAudio = evaluated(await browser(source, env, 'eval', 'JSON.stringify({peak:__ipcAudio.peak,run:__ipcAudio.maxRun,closed:__ipcAudio.closed,failed:__ipcAudio.failed})'));
        appFacts = evaluated(await browser(app, env, 'eval', `(async()=>{let samples=0,sent=0,frames=0;
          (await __callPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio'&&Number.isFinite(s.totalSamplesReceived)&&Number.isFinite(s.concealedSamples))samples+=Math.max(0,s.totalSamplesReceived-s.concealedSamples);
            if(s.type==='outbound-rtp'&&s.kind==='audio')sent+=s.packetsSent||0;
            if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});const v=document.getElementById('remote'),canvas=document.createElement('canvas');
          canvas.width=32;canvas.height=24;let nonBlackPixels=0;if(v.readyState>=2){const c=canvas.getContext('2d');c.drawImage(v,0,0,32,24);
            const pixels=c.getImageData(0,0,32,24).data;for(let i=0;i<pixels.length;i+=4)if(pixels[i]+pixels[i+1]+pixels[i+2]>12)nonBlackPixels++;}
          return JSON.stringify({samples,sent,connected:__callPeer.connectionState==='connected',frames,
            nonBlackPixels,peak:window.__ipcAudio?.peak||0,run:window.__ipcAudio?.maxRun||0,
            closed:window.__ipcAudio?.closed,failed:window.__ipcAudio?.failed});})()`));
        const {sessionId: ignoredSession, ...sourceSummary} = sourceFacts || {};
        lastFacts = {source: sourceSummary, application: appFacts, sourceAudio};
        return mediaPass(sourceFacts, appFacts, sourceAudio);
      });
      if (concurrent) {
        stage = 'loser_late_original_answer_replay';
        for (const endpoint of endpoints.filter(value => value !== winnerEndpoint)) {
          const loser = invitations.get(endpoint);
          // 删除本地旧回执缓存再重投原请求，必须取得正式后端的新投递，不能用旧409冒充重放成功。
          events.delete(`${endpoint}:interaction.result:${loser.msgToken}`);
          const replay = await answerCandidate(endpoint);
          assert.equal(replay.code, 409);assert.equal(replay.data?.reason, 'alreadyAnswered');
        }
        assert.equal(evaluated(await browser(source, env, 'eval', 'views.size === 1 && views.get(humanSession)?.peer?.connectionState === "connected"')),true,
          'loser_replay_must_not_allocate_or_interrupt');
      }
      stage = 'actual_hangup_release';
      await browser(source, env, 'eval', '(()=>{const v=views.get(humanSession);window.__old={peer:v.peer,tracks:v.stream.getTracks(),remote:v.remoteTrack,sink:v.sink};return true;})()');
      assert.equal((await command(winnerEndpoint, winnerSession, 'interaction.end', {reason: 'userStopped'})).code, 200);
      await waitFor(async () => evaluated(await browser(source, env, 'eval', 'views.size === 0 && retiringViews.size === 0 && invitations.size === 0 && humanSession === ""')) === true);
      const released = evaluated(await browser(source, env, 'eval', `(async()=>{await __ipcAudio.stop();return JSON.stringify({peerClosed:__old.peer.connectionState==='closed',
        tracksStopped:__old.tracks.every(t=>t.readyState==='ended'),remoteStopped:__old.remote.readyState==='ended',sinkReleased:__old.sink.srcObject===null&&__old.sink.paused,
        audioClosed:__ipcAudio.context.state==='closed'});})()`));
      assert.deepEqual(released, {peerClosed: true, tracksStopped: true, remoteStopped: true, sinkReleased: true, audioClosed: true});
      const appReleased = evaluated(await browser(app, env, 'eval', `(async()=>{await __ipcAudio.stop();__capture.getTracks().forEach(t=>t.stop());__remoteTracks.forEach(t=>t.stop());__callPeer.close();
        const v=document.getElementById('remote');v.pause();v.srcObject=null;return JSON.stringify({peerClosed:__callPeer.connectionState==='closed',
          tracksStopped:__capture.getTracks().every(t=>t.readyState==='ended')&&__remoteTracks.every(t=>t.readyState==='ended'),audioClosed:__ipcAudio.context.state==='closed',videoReleased:v.srcObject===null});})()`));
      assert.deepEqual(appReleased, {peerClosed: true, tracksStopped: true, audioClosed: true, videoReleased: true});
      const {sessionId: ignored, ...sourceSummary} = sourceFacts;
      rounds.push({round: round + 1, source: sourceSummary, sourceAudio, application: appFacts, released, appReleased,
        answerRace: {concurrent, candidates:3, winners:1, losers:2, lateLoserReplayVerified:concurrent},
        ...(observer?{originalEventReplayVerified}: {})});
      winnerSession = ''; winnerEndpoint = '';
    }

    if(process.env.UR_DOORBELL_NEGATIVE_CASES==='1'){
    stage = 'negative_capture_observer';
    assert.equal(evaluated(await browser(source,env,'eval',`(()=>{
      if(window.__doorbellNegative)throw Error('duplicate_probe');
      const media=navigator.mediaDevices,originalMedia=media.getUserMedia,originalSend=send;
      const facts={captures:0,submitted:0,pending:0,receipts:0,lastReceipt:''};
      const observedMedia=function(...args){facts.captures++;return Reflect.apply(originalMedia,this,args);};
      const observedSend=function(value){const accepted=originalSend(value);
        if(accepted&&value.kind==='doorbellPress')facts.submitted++;return accepted;};
      const listener=event=>{let value;try{value=JSON.parse(event.data);}catch{return;}
        if(value.kind!=='doorbellState')return;
        if(value.state==='pending')facts.pending++;
        if(['ack','unknown'].includes(value.state)){facts.receipts++;facts.lastReceipt=value.state;}};
      media.getUserMedia=observedMedia;send=observedSend;socket.addEventListener('message',listener);
      facts.restore=()=>{if(media.getUserMedia!==observedMedia||send!==observedSend)throw Error('observer_changed');
        media.getUserMedia=originalMedia;send=originalSend;socket.removeEventListener('message',listener);delete window.__doorbellNegative;};
      window.__doorbellNegative=facts;return true;})()`)),true);
    stage = 'negative_formal_flows';
    const negatives=await require('./doorbell_negative_live.cjs').verifyNegatives({
      api,command,receive,events,endpoints,previousGroups,waitFor,validateInvite,camera:process.env.UR_IPC_CAMERA_ID,
      onStage:name=>{stage='negative_'+name;},
      product:process.env.UR_DEVICE_PRODUCT,device:process.env.UR_DEVICE_NAME,
      press:async expected=>{
        await waitFor(async()=>evaluated(await browser(source,env,'eval','doorbellAvailable && !doorbellPending && !closed'))===true);
        const before=evaluated(await browser(source,env,'eval',
          'JSON.stringify({submitted:__doorbellNegative.submitted,pending:__doorbellNegative.pending,receipts:__doorbellNegative.receipts})'));
        const networkBefore=observer.facts();assert.equal(networkBefore.failed,false);
        await browser(source,env,'click','#doorbell');
        let observed;
        await waitFor(async()=>{observed=evaluated(await browser(source,env,'eval',
          'JSON.stringify({submitted:__doorbellNegative.submitted,pending:__doorbellNegative.pending,receipts:__doorbellNegative.receipts,lastReceipt:__doorbellNegative.lastReceipt})'));
          return observed.submitted===before.submitted+1&&observed.pending===before.pending+1&&observed.receipts===before.receipts+1;});
        const original=observer.facts();assert.equal(original.failed,false);
        assert.equal(original.submitted,networkBefore.submitted+1);assert.equal(original.replied,networkBefore.replied+1);
        assert.ok(expected==='success'||expected==='rejected');assert.equal(original[expected],networkBefore[expected]+1);
        return observed.lastReceipt===(expected==='success'?'ack':'unknown');
      },
      sourceFacts:async()=>evaluated(await browser(source,env,'eval',
        'JSON.stringify({available,closed,views:views.size,retiring:retiringViews.size,invitations:invitations.size,humanSessionEmpty:humanSession==="",newCaptures:__doorbellNegative.captures})'))
    });
    rounds.push({negativeFlows:negatives});
    assert.equal(evaluated(await browser(source,env,'eval','(()=>{__doorbellNegative.restore();return true;})()')),true);

    }
    successful = true;
  } catch {
    console.log(JSON.stringify({result: 'fail', stage, completedRounds: rounds, lastFacts, sourceExited: exited,
      reason: 'doorbell_fact_or_operation_failed'}));
    process.exitCode = 1;
  } finally {
    clearTimeout(deadline);
    if (winnerSession && winnerEndpoint) await command(winnerEndpoint, winnerSession, 'interaction.end', {reason: 'userStopped'}).catch(() => {cleanupFailures.push('original_session');});
    for (const session of [source, app]) {
      await browser(session, env, 'eval', '(async()=>{window.__doorbellNegative?.restore();if(window.__ipcAudio)await __ipcAudio.stop();window.__capture?.getTracks().forEach(t=>t.stop());window.__callPeer?.close();return true;})()').catch(() => {cleanupFailures.push('browser_media');});
      await browser(session, env, 'close').catch(() => {cleanupFailures.push('named_browser');});
    }
    await stopChild(child,completion,()=>exited).catch(() => {cleanupFailures.push('original_child');});
    if(observer){await observer.close().catch(()=>{cleanupFailures.push('event_observer');});
      const facts=observer.facts();if(facts.failed||!facts.connectionsClosed)cleanupFailures.push('event_observer');}
    if (child && exitCode !== 0) cleanupFailures.push('child_exit');
    if (server.listening) await bounded(new Promise((resolve,reject) => server.close(error=>error?reject(error):resolve())),5000,'listener_close_timeout')
      .catch(() => {cleanupFailures.push('local_listener');});
  }
  const passed = successful && cleanupFailures.length === 0;
  if (passed) console.log(JSON.stringify({result:'pass',path:'original_event_cpp_single_mqtt_real_im',rounds,
    cleanup:{childClosed:exited,childExitCode:exitCode,browsersClosed:true,listenerClosed:!server.listening,...(observer?{eventObserver:observer.facts()}:{})},
    scope:'linux_virtual_media_not_hardware_aec',endpointCleanup:'one_hour_expiry_no_unregister_api'}));
  else if (cleanupFailures.length) {process.exitCode=1;console.log(JSON.stringify({result:'fail',stage:'cleanup',failures:cleanupFailures}));}
  return {passed};
}
module.exports = {run,validateConfiguration, validateInvite, mediaPass, waitFor, uniqueAnswer};
if (require.main === module) run().catch(() => {
  console.log(JSON.stringify({result:'fail',stage:'configuration',reason:'invalid_live_configuration'}));process.exitCode=1;
});
