// 文件说明：显式C++配置→正式浏览器驱动→原生Pion的媒体集成验证；不是MQTT鉴权、IM授权或完整会话E2E。
'use strict';
const assert = require('node:assert/strict');
const {execFile} = require('node:child_process');

// localEndpoint 限定测试服务为回环HTTP且无凭据、路径或查询，不接受任意媒体源。
function localEndpoint(value) {
  const url = new URL(value);
  assert.ok(url.protocol === 'http:' && url.hostname === '127.0.0.1' && Number(url.port) >= 1024 &&
    !url.username && !url.password && url.pathname === '/' && !url.search && !url.hash, 'loopback_media_test_required');
  return url.origin;
}
// evaluated 解开浏览器CLI的JSON包，不保存页面内容、SDP或自由错误正文。
function evaluated(raw) {
  let value = JSON.parse(raw);
  while (typeof value === 'string') { try { value = JSON.parse(value); } catch { break; } }
  return value;
}
// execute 只操作由Go测试生成的本任务命名会话，调用失败不泄漏浏览器输出。
function execute(session, env, ...args) {
  return new Promise((resolve, reject) => execFile('agent-browser', ['--session', session, ...args],
    {env, timeout: 40000, maxBuffer: 262144}, (error, stdout) => error ? reject(Error(`browser_${args[0]}_failed`)) : resolve(stdout.trim())));
}
// main 验证持续双向实际解码以及结束后真轨/Peer/播放输出释放；浏览器静音开关只避免向宿主扬声器发声。
async function main() {
  assert.equal(process.env.UR_IPC_BROWSER_MEDIA_LIVE, '1', 'explicit_media_test_required');
  const endpoint = localEndpoint(process.env.UR_IPC_MEDIA_TEST_URL);
  const session = process.env.UR_IPC_MEDIA_TEST_SESSION;
  assert.ok(/^task143-talk-[a-z0-9-]{1,48}$/.test(session), 'isolated_browser_session_required');
  assert.ok(process.env.UR_IPC_MEDIA_TEST_WAV, 'controlled_pcm_fixture_required');
  const env = {...process.env, AGENT_BROWSER_ARGS: [
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--no-sandbox',
    '--autoplay-policy=no-user-gesture-required', '--mute-audio',
    `--use-file-for-fake-audio-capture=${process.env.UR_IPC_MEDIA_TEST_WAV}`].join(',')};
  const browser = (...args) => execute(session, env, ...args);
  let stage = 'open'; // 只记录固定步骤，不用浏览器自由错误正文作为报告。
  try {
    await browser('open', endpoint);
    await browser('snapshot', '-i');
    stage = 'configure';
    // 此桥是显式媒体夹具，不冒充真实C++设备进程或平台持久状态；配置来自实际C++探针。
    await browser('eval', `(async()=>{
      window.__mediaFacts={failed:false,quality:null};
      const originalConfigure=configure;
      configure=async value=>{try{return await originalConfigure(value);}catch(error){window.__mediaFacts.reason=error.name;
        if(/^[a-z_]+$/.test(error.message))window.__mediaFacts.code=error.message;throw error;}};
      socket={readyState:WebSocket.OPEN,bufferedAmount:0,close(){},send(body){
        const message=JSON.parse(body);
        if(message.kind==='ipcFailure')window.__mediaFacts.failed=true;
        if(message.kind==='ipcQualityApplied')window.__mediaFacts.quality=message;
        if(message.kind==='ipcOffer')void fetch('/offer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sdp:message.sdp})})
          .then(response=>{if(!response.ok)throw Error('offer_rejected');return response.json();})
          .then(answer=>incoming({kind:'ipcAnswer',sessionId:message.sessionId,sdp:answer.sdp})).catch(()=>{window.__mediaFacts.failed=true;});
        if(message.kind==='ipcConnected')void incoming({kind:'ipcActive',sessionId:message.sessionId});
      }};
      const configuration=await(await fetch('/configure')).json();
      window.__mediaSession=configuration.sessionId;await incoming(configuration);return true;
    })()`);
    stage = 'decoded_audio';
    // CLI wait 条件不能把Promise对象当成功；在页面中等待真实统计值并设独立截止时间。
    await browser('eval', `(async()=>{const deadline=Date.now()+25000;while(Date.now()<deadline){
      if(window.__mediaFacts.failed)throw Error('media_driver_failed');const states=await ipcStats();const view=states[0];
      const facts=await(await fetch('/facts')).json();if(states.length===1&&view.talk&&view.connected&&view.active&&view.playing&&
      view.frames===0&&view.audioSent>2&&view.audioReceived>2&&view.audioSamples>1920&&view.audioEnergy>0&&facts.nonSilentFrames>=2)return true;
      await new Promise(resolve=>setTimeout(resolve,100));}throw Error('decoded_audio_timeout');})()`);
    stage = 'facts';
    const facts = evaluated(await browser('eval', `(async()=>{const states=await ipcStats();const server=await(await fetch('/facts')).json();
      const view=views.get(window.__mediaSession);window.__mediaResources={peer:view.peer,capture:view.stream.getTracks(),remote:view.remoteTrack,sink:view.sink};
      return JSON.stringify({client:states[0],server,videoTracks:view.stream.getVideoTracks().length});})()`));
    assert.equal(facts.videoTracks, 0); assert.ok(facts.server.nonSilentFrames >= 2);
    assert.ok(facts.client.audioSamples > 1920 && facts.client.audioEnergy > 0);
    stage = 'speaker_pause';
    // 仅验证实际桌面播放驱动，明确由测试桥调用；不冒充后端已下发speaker控制。
    const paused = evaluated(await browser('eval', `(async()=>{
      const sid=window.__mediaSession,view=views.get(sid),before=await(await fetch('/facts')).json();
      const capture=view.stream.getAudioTracks()[0],sender=view.peer.getSenders().find(s=>s.track===capture);
      await incoming({kind:'ipcTrack',sessionId:sid,token:'media-speaker-pause',trackId:'speaker-1',operation:'pause'});
      await incoming({kind:'ipcActive',sessionId:sid});
      const deadline=Date.now()+8000;while(Date.now()<deadline){
        const after=await(await fetch('/facts')).json();if(after.nonSilentFrames>=before.nonSilentFrames+5){
          return JSON.stringify({playing:view.playing,paused:view.sink.paused,remoteEnabled:view.remoteTrack.enabled,
            captureLive:capture.readyState==='live',sameSender:sender.track===capture,peerConnected:view.peer.connectionState==='connected',
            uplinkDuringPause:after.nonSilentFrames-before.nonSilentFrames});}
        await new Promise(resolve=>setTimeout(resolve,100));}throw Error('pause_stopped_uplink');})()`));
    assert.equal(paused.playing, false); assert.equal(paused.paused, true); assert.equal(paused.remoteEnabled, false);
    assert.equal(paused.captureLive, true); assert.equal(paused.sameSender, true); assert.equal(paused.peerConnected, true);
    assert.ok(paused.uplinkDuringPause >= 5);
    stage = 'speaker_resume';
    const resumed = evaluated(await browser('eval', `(async()=>{
      const sid=window.__mediaSession,view=views.get(sid),before=(await ipcStats())[0];
      await incoming({kind:'ipcTrack',sessionId:sid,token:'media-speaker-resume',trackId:'speaker-1',operation:'resume'});
      const deadline=Date.now()+8000;while(Date.now()<deadline){const after=(await ipcStats())[0];
        if(after.audioSamples>=before.audioSamples+1920&&after.audioEnergy>before.audioEnergy)
          return JSON.stringify({playing:view.playing,paused:view.sink.paused,remoteEnabled:view.remoteTrack.enabled,
            decodedSamplesAfterResume:after.audioSamples-before.audioSamples,energyAfterResume:after.audioEnergy-before.audioEnergy});
        await new Promise(resolve=>setTimeout(resolve,100));}throw Error('resume_audio_timeout');})()`));
    assert.equal(resumed.playing, true); assert.equal(resumed.paused, false); assert.equal(resumed.remoteEnabled, true);
    assert.ok(resumed.decodedSamplesAfterResume >= 1920 && resumed.energyAfterResume > 0);
    stage = 'quality';
    // 原sender实际调整后继续双向解码；此夹具不冒充MQTT受理或平台计划终态。
    const quality = [];
    for (const [profile, bitrate] of [['low',16000], ['high',64000], ['standard',32000]]) {
      const observed = evaluated(await browser('eval', `(async()=>{
        const sid=window.__mediaSession,view=views.get(sid),source=view.sources.get('mic-1');
        const peer=view.peer,capture=source.track,before=(await ipcStats())[0],nativeBefore=await(await fetch('/facts')).json();
        window.__mediaFacts.quality=null;
        await incoming({kind:'ipcQuality',sessionId:sid,token:'media-quality-${profile}',trackId:'mic-1',profile:'${profile}'});
        const fact=window.__mediaFacts.quality;
        if(!fact||!fact.success||fact.token!=='media-quality-${profile}'||fact.state!=='active')throw Error('quality_driver_failed');
        const parameters=source.sender.getParameters();
        if(parameters.encodings.length!==1||parameters.encodings[0].maxBitrate!==${bitrate})throw Error('quality_parameter_mismatch');
        const deadline=Date.now()+8000;while(Date.now()<deadline){const after=(await ipcStats())[0],nativeAfter=await(await fetch('/facts')).json();
          if(after.audioSamples>=before.audioSamples+1920&&after.audioEnergy>before.audioEnergy&&nativeAfter.nonSilentFrames>=nativeBefore.nonSilentFrames+5)
            return JSON.stringify({profile:'${profile}',maxBitrate:parameters.encodings[0].maxBitrate,
              samePeer:views.get(sid).peer===peer,sameCapture:source.track===capture,captureLive:capture.readyState==='live',
              decodedSamples:after.audioSamples-before.audioSamples,decodedEnergy:after.audioEnergy-before.audioEnergy,
              nativeNonSilentFrames:nativeAfter.nonSilentFrames-nativeBefore.nonSilentFrames});
          await new Promise(resolve=>setTimeout(resolve,100));}throw Error('quality_stopped_audio');})()`));
      assert.equal(observed.maxBitrate,bitrate);
      assert.equal(observed.samePeer,true); assert.equal(observed.sameCapture,true); assert.equal(observed.captureLive,true);
      assert.ok(observed.decodedSamples>=1920 && observed.decodedEnergy>0 && observed.nativeNonSilentFrames>=5);
      quality.push(observed);
    }
    stage = 'release';
    await browser('eval', `(async()=>{await incoming({kind:'ipcStop',sessionId:window.__mediaSession});return true;})()`);
    const ended = evaluated(await browser('eval', `(()=>{const r=window.__mediaResources;return JSON.stringify({sessions:views.size,
      peerClosed:r.peer.connectionState==='closed',captureStopped:r.capture.every(t=>t.readyState==='ended'),remoteStopped:r.remote.readyState==='ended',
      outputReleased:r.sink.srcObject===null&&r.sink.paused});})()`));
    assert.deepEqual(ended, {sessions:0,peerClosed:true,captureStopped:true,remoteStopped:true,outputReleased:true});
    console.log(JSON.stringify({result:'pass',audioSent:facts.client.audioSent,audioReceived:facts.client.audioReceived,
      decodedSamples:facts.client.audioSamples,decodedEnergy:facts.client.audioEnergy,uplinkNonSilentFrames:facts.server.nonSilentFrames,
      speakerControlScope:'browserMediaComponent',qualityControlScope:'browserMediaComponent',quality,paused,resumed,videoTracks:0,ended}));
  } catch (error) {
    let driver;
    if (stage !== 'open') {
      try { driver = evaluated(await browser('eval', `(async()=>JSON.stringify({facts:window.__mediaFacts,stats:typeof ipcStats==='function'?await ipcStats():[]}))()`)); }
      catch { driver = {unavailable:true}; }
    }
    console.log(JSON.stringify({result:'fail',stage,driver}));
    throw error;
  } finally { await browser('close').catch(() => {}); }
}
if (require.main === module) main().catch(() => { console.error('ipc_talk_media_integration_failed'); process.exitCode = 1; });
module.exports = {localEndpoint};
