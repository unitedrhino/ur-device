// 文件说明：三独立C++人际端的正式接续验收；只走本地HAL意图，不注入MQTT、接听、ICE或平台ready事实。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {spawn} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {browser,evaluated} = require('./ipc_live.cjs');
const {stopChild,bounded} = require('./conference_member_live.cjs');

// finishDevice 先等待已发送shutdown的正常退出，超时才终止本次精确进程；TERM不算正常退出。
async function finishDevice(child,completion,isExited,graceBudget=8000) {
  if(!child||!completion)return;
  try {await bounded(completion,graceBudget,'device_grace_timeout');return;}
  catch(error){if(error.message!=='device_grace_timeout')throw error;}
  await stopChild(child,completion,isExited,3000,2000);
}
// deviceCompletion 等待原子进程close而非exit；进程已退出但stderr未排空仍不算资源释放。
function deviceCompletion(device) {
  return new Promise((resolve,reject)=>{
    device.child.once('error',()=>reject(Error('device_spawn_failed')));
    device.child.once('close',code=>{device.exited=true;device.exitCode=code;resolve();});
  });
}

// configuration 在启动任何资源之前核对三真实身份、独立端口、显式能力及实际WAV，不自动复制身份。
function configuration(env,stat=fs.statSync) {
  assert.equal(env.UR_NATIVE_ENDPOINT_HANDOFF_LIVE,'1','explicit_native_handoff_required');
  assert.equal(env.UR_IPC_HUMAN_CALL,'1','explicit_human_call_required');
  assert.equal(env.UR_IPC_HUMAN_RECEIVE_VIDEO,'1','explicit_remote_video_required');
  assert.ok(['127.0.0.1','localhost','::1'].includes(env.UR_MQTT_HOST),'loopback_broker_required');
  const operation=env.UR_NATIVE_ENDPOINT_HANDOFF_OPERATION||'commit';
  assert.ok(['commit','abort','timeout'].includes(operation),'invalid_operation');
  assert.ok(stat(env.UR_IPC_DEVICE_BIN).isFile(),'device_binary_required');
  const devices=[{product:env.UR_DEVICE_PRODUCT,name:env.UR_DEVICE_NAME,secret:env.UR_DEVICE_SECRET,
    port:Number(env.UR_IPC_DEVICE_PORT),wav:env.UR_IPC_AUDIO_FIXTURE}];
  for(const role of ['RETAINED','TARGET']) devices.push({product:env[`UR_NATIVE_ENDPOINT_${role}_PRODUCT`],
    name:env[`UR_NATIVE_ENDPOINT_${role}_NAME`],secret:env[`UR_NATIVE_ENDPOINT_${role}_SECRET`],
    port:Number(env[`UR_NATIVE_ENDPOINT_${role}_PORT`]),wav:env[`UR_NATIVE_ENDPOINT_${role}_AUDIO_FIXTURE`]});
  for(const device of devices) {
    for(const key of ['product','name','secret']) assert.ok(typeof device[key]==='string'&&device[key].length>0&&!/[\x00-\x1f\x7f]/.test(device[key]),'injected_identity_required');
    assert.ok(Number.isSafeInteger(device.port)&&device.port>=1024&&device.port<=65535,'local_port_required');
    assert.ok(typeof device.wav==='string'&&stat(device.wav).isFile(),'audio_fixture_required');
  }
  assert.equal(new Set(devices.map(d=>`${d.product}:${d.name}`)).size,3,'three_distinct_identities_required');
  assert.equal(new Set(devices.map(d=>d.port)).size,3,'three_distinct_ports_required');
  return {operation,devices};
}
// decodedMedia 只接受实际音频解码、可播放PCM和非黑远端视频，不用本地摄像头编码数代替。
function decodedMedia(value) {
  return !!value&&value.connected===true&&value.active===true&&value.audioPackets>0&&value.audioSamples>=960&&
    value.energy>0&&value.audioPeak>0.001&&value.videoFrames>=2&&value.videoPackets>0&&value.pixels>0;
}
// stoppedMedia 要求原对象真正关闭，包括本地采集、远端音频与远端视频，而非只在views删键。
function stoppedMedia(value) {
  return !!value&&!value.present&&value.peer==='closed'&&value.tracks>0&&value.tracksEnded===true&&
    value.remoteAudioEnded===true&&value.remoteVideoEnded===true;
}
// run 建立原两端后才prepare第三端；30秒超时等待原绝对截止，不缩短服务端定时器。
async function run(environment=process.env,options={}) {
  assert.ok(options.onDeviceReady===undefined||typeof options.onDeviceReady==='function','invalid_device_ready_hook');
  const config=configuration(environment), devices=[];
  const controller=new AbortController(), deadline=setTimeout(()=>controller.abort(Error('native_handoff_deadline')),180000);
  const cleanupFailures=[]; let stage='setup', report;
  // execute 仅本次具名浏览器；原CLI有固定超时，取消后不再发布后续控制意图。
  async function execute(device,...args) {controller.signal.throwIfAborted();return browser(device.browser,device.env,...args);}
  async function evaluate(device,script) {return evaluated(await execute(device,'eval',script));}
  async function until(probe,budget=20000) {
    const end=performance.now()+budget;
    do {controller.signal.throwIfAborted();const value=await probe();if(value)return value;await new Promise(r=>setTimeout(r,100));} while(performance.now()<end);
    throw Error('native_state_timeout');
  }
  // local 只提交C++公开的本地用户意图；设备自行生成token并由正式后端鉴权。
  async function local(device,request) {await evaluate(device,`(()=>{send(${JSON.stringify(request)});return true;})()`);}
  async function current(device) {
    return evaluate(device,`(()=>{const value=[...views.entries()].find(([,v])=>v.human&&!v.closed);return value?{session:value[0],active:value[1].active,connected:value[1].connected}:null;})()`);
  }
  async function receipt(device,state) {
    return until(async()=>{const value=await evaluate(device,'JSON.stringify(window.__endpointReceipt||null)');
      return value&&value.state===state&&value.sessionId===device.session&&!value.pending&&!value.unknown&&value;},state==='failed'?40000:20000);
  }
  // observe 保存真实原媒体引用并接零增益PCM观察；addEventListener不覆盖正式HAL终止回调。
  async function observe(device) {
    await evaluate(device,`(()=>{const view=views.get(${JSON.stringify(device.session)});if(!view?.remoteTrack||!view?.remoteVideoTrack)throw Error('actual_remote_tracks_required');
      window.__nativeOriginal={session:${JSON.stringify(device.session)},view,peer:view.peer,stream:view.stream,remoteAudio:view.remoteTrack,remoteVideo:view.remoteVideoTrack};
      const context=new AudioContext({sampleRate:48000}),source=context.createMediaStreamSource(new MediaStream([view.remoteTrack])),analyser=context.createAnalyser(),gain=context.createGain();
      analyser.fftSize=512;gain.gain.value=0;source.connect(analyser);analyser.connect(gain);gain.connect(context.destination);
      const state=window.__nativeAudio={context,source,analyser,gain,peak:0,closed:false};const samples=new Float32Array(512);
      state.timer=setInterval(()=>{analyser.getFloatTimeDomainData(samples);let sum=0;for(const v of samples)sum+=v*v;state.peak=Math.max(state.peak,Math.sqrt(sum/samples.length));},25);
      state.stop=()=>{if(state.closed)return;state.closed=true;clearInterval(state.timer);source.disconnect();gain.disconnect();return context.close();};
      view.remoteTrack.addEventListener('ended',state.stop,{once:true});void context.resume();return true;})()`);
  }
  async function metrics(device) {
    return evaluate(device,`(async()=>{const view=views.get(${JSON.stringify(device.session)});if(!view?.peer)return null;
      let audioPackets=0,audioSamples=0,energy=0,videoFrames=0,videoPackets=0;(await view.peer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='audio'){audioPackets+=s.packetsReceived||0;audioSamples+=Math.max(0,(s.totalSamplesReceived||0)-(s.concealedSamples||0));energy+=s.totalAudioEnergy||0;}if(s.type==='inbound-rtp'&&s.kind==='video'){videoFrames+=s.framesDecoded||0;videoPackets+=s.packetsReceived||0;}});
      let pixels=0;const video=view.remoteVideo;if(video?.videoWidth&&video.readyState>=2){const canvas=document.createElement('canvas');canvas.width=16;canvas.height=16;const c=canvas.getContext('2d');c.drawImage(video,0,0,16,16);const bytes=c.getImageData(0,0,16,16).data;for(let i=0;i<bytes.length;i+=4)pixels+=bytes[i]+bytes[i+1]+bytes[i+2];}
      return JSON.stringify({connected:view.connected,active:view.active,audioPackets,audioSamples,energy,videoFrames,videoPackets,pixels,audioPeak:window.__nativeAudio?.peak||0});})()`);
  }
  async function lifecycle(device) {
    return evaluate(device,`(()=>{const o=window.__nativeOriginal;if(!o)return null;const current=views.get(o.session),tracks=o.stream.getTracks();return JSON.stringify({present:!!current,peer:o.peer.connectionState,tracks:tracks.length,tracksEnded:tracks.every(t=>t.readyState==='ended'),remoteAudioEnded:!o.remoteAudio||o.remoteAudio.readyState==='ended',remoteVideoEnded:!o.remoteVideo||o.remoteVideo.readyState==='ended',samePeer:current?.peer===o.peer,sameStream:current?.stream===o.stream,live:tracks.every(t=>t.readyState==='live')});})()`);
  }
  try {
    for(const [index,settings] of config.devices.entries()) {
      const env={...environment,UR_DEVICE_PRODUCT:settings.product,UR_DEVICE_NAME:settings.name,UR_DEVICE_SECRET:settings.secret,
        UR_IPC_AUDIO_FIXTURE:settings.wav,AGENT_BROWSER_ARGS:[environment.AGENT_BROWSER_ARGS||'--no-sandbox','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required',`--use-file-for-fake-audio-capture=${settings.wav}%noloop`].join(',')};
      const device={...settings,env,browser:`native-handoff-${index}-${randomUUID()}`,exited:false,session:''};devices.push(device);
      device.child=spawn(environment.UR_IPC_DEVICE_BIN,[String(settings.port)],{env,stdio:['ignore','ignore','pipe']});
      device.stderrFact={failed:false,closed:false};
      device.stderrEnded=false;
      device.child.stderr.once('end',()=>{device.stderrEnded=true;});
      let pending='';
      device.child.stderr.on('data',bytes=>{
        // 仅识别宿主固定终态，不收录自由日志；超限丢弃整个未完成行。
        pending=(pending+bytes.toString()).slice(-4096);
        const lines=pending.split('\n');pending=lines.pop();
        for(const line of lines){if(line==='ipc_device_failed')device.stderrFact.failed=true;if(line==='ipc_device_closed')device.stderrFact.closed=true;}
      });
      device.completion=deviceCompletion(device);
      device.completion.catch(()=>{}); // 由finally等待原promise收敛，避免启动失败先成为未处理拒绝。
      await until(async()=>{try{return(await fetch(`http://127.0.0.1:${settings.port}/`,{signal:AbortSignal.timeout(1000)})).ok;}catch{return false;}},10000);
      await execute(device,'open',`http://127.0.0.1:${settings.port}/`);device.browserOpened=true;
      await execute(device,'click','#activate');
      await execute(device,'wait','--fn',`document.getElementById('human-status').textContent==='人际通话入口就绪。'`);
      // 中央仅在实际C++上线并完成能力ACK之后走正式归属绑定；驱动不接收或伪造主人授权。
      if(options.onDeviceReady)await options.onDeviceReady({index,device:{productID:settings.product,deviceName:settings.name}});
    }
    const [source,retained,target]=devices;
    stage='original_call';await local(source,{kind:'callDial',target:`${retained.product}:${retained.name}`});
    await execute(retained,'wait','--fn',`humanSession!==''&&invitations.has(humanSession)`);
    retained.session=await evaluate(retained,'humanSession');await local(retained,{kind:'callAccept',sessionId:retained.session});
    for(const device of [source,retained]) {
      const connected=await until(async()=>{const value=await current(device);return value?.active&&value.connected&&value;});device.session=connected.session;
      await observe(device);await until(async()=>{const value=await metrics(device);return decodedMedia(value)&&value;});
    }
    const before=await metrics(retained);
    stage='prepare';await local(source,{kind:'endpointPrepare',sessionId:source.session,target:`device:${target.product}:${target.name}`});
    const prepared=await receipt(source,'preparing');assert.equal(prepared.revision,1);assert.ok(prepared.handoffId.length>0);
    await execute(target,'wait','--fn',`humanSession!==''&&invitations.has(humanSession)`);
    target.session=await evaluate(target,'humanSession');await local(target,{kind:'callAccept',sessionId:target.session});
    stage='actual_candidate_ready';const ready=await receipt(source,'ready');assert.equal(ready.candidateSessionId,target.session);assert.equal(ready.revision,2);
    await evaluate(target,`(()=>{const view=views.get(${JSON.stringify(target.session)});if(!view?.peer||!view?.stream)throw Error('candidate_actual_hal_required');window.__nativeOriginal={session:${JSON.stringify(target.session)},view,peer:view.peer,stream:view.stream,remoteAudio:view.remoteTrack,remoteVideo:view.remoteVideoTrack};return true;})()`);
    // 原腿在候选就绪后还必须实际解码，prepare不允许提前停止或更换原Peer。
    assert.ok(decodedMedia(await metrics(source)));assert.ok(decodedMedia(await metrics(retained)));
    const originalRetained=await lifecycle(retained);assert.ok(originalRetained.samePeer&&originalRetained.sameStream&&originalRetained.live);
    if(config.operation==='commit') {
      stage='commit';await local(source,{kind:'endpointCommit',sessionId:source.session});const committed=await receipt(source,'committed');
      assert.equal(committed.handoffId,prepared.handoffId);assert.equal(committed.candidateSessionId,target.session);assert.equal(committed.revision,3);
      await until(async()=>{const value=await current(target);return value?.session===target.session&&value.active&&value.connected;});
      await observe(target);await until(async()=>decodedMedia(await metrics(target)));
      await until(async()=>stoppedMedia(await lifecycle(source)));
      assert.ok(stoppedMedia(await lifecycle(source)),'source_actual_hal_stop_required');
    } else {
      stage=config.operation;
      if(config.operation==='abort')await local(source,{kind:'endpointAbort',sessionId:source.session});
      const ended=await receipt(source,config.operation==='abort'?'aborted':'failed');
      assert.equal(ended.handoffId,prepared.handoffId);assert.equal(ended.reason,config.operation==='abort'?'userCancelled':'handoffTimeout');
      await until(async()=>stoppedMedia(await lifecycle(target)));
      assert.ok(decodedMedia(await metrics(source)),'abort_timeout_preserves_source_decode');
      const original=await lifecycle(source);assert.ok(original.samePeer&&original.sameStream&&original.live,'source_original_objects_required');
    }
    stage='retained_continuity';await until(async()=>{const after=await metrics(retained);return decodedMedia(after)&&after.videoFrames>before.videoFrames&&after.audioSamples>before.audioSamples;});
    const continuity=await lifecycle(retained);assert.ok(continuity.samePeer&&continuity.sameStream&&continuity.live,'retained_original_peer_required');
    report={result:'pass',operation:config.operation,cppProcesses:3,actualAudioDecode:true,actualRemoteVideoDecode:true,retainedSamePeer:true,
      sourceStopped:config.operation==='commit',sourcePreserved:config.operation!=='commit',signalingReadyWasNotCommit:true};
  } catch(error) {
    report={result:'fail',stage,reason:/^[a-z_]{1,80}$/.test(error.message)?error.message:'native_handoff_assertion_failed'};
    // 仅投影固定UI状态及计数，不保存身份、媒体地址、平台正文或自由错误日志。
    report.devices=[];
    for(const device of devices){
      const facts={exited:device.exited,exitCode:Number.isInteger(device.exitCode)?device.exitCode:null};
      if(device.browserOpened)try{
        facts.browser=await evaluated(await browser(device.browser,device.env,'eval',`(()=>{const text=document.getElementById('human-status').textContent;
          const states={'人际通话入口就绪。':'available','人际通话入口未就绪。':'unavailable','人际来电：必须本地明确接听，不自动开启音视频。':'ringing','平台会话已建立；实际媒体状态另行采集。':'connected','正在协商媒体，尚不能确认接通。':'negotiating'};
          return JSON.stringify({state:states[text]||'unclassified',closed,views:views.size,invitations:invitations.size,sessionPresent:humanSession!==''});})()`));
      }catch{facts.browserUnavailable=true;}
      report.devices.push(facts);
    }
  }
  finally {
    clearTimeout(deadline);
    for(const device of devices) {
      // 独立清理不继承已取消业务signal；先结束原真实通话，再请求三进程各自停止。
      if(device.browserOpened)try{await browser(device.browser,device.env,'eval',`(()=>{if(window.__nativeAudio)void window.__nativeAudio.stop();for(const [session,v]of views)if(v.human&&!v.closed)send({kind:'callHangup',sessionId:session});send({kind:'shutdown'});return true;})()`);}catch{cleanupFailures.push('device_hal_cleanup_failed');}
      try{await finishDevice(device.child,device.completion,()=>device.exited);if(device.exitCode!==0)cleanupFailures.push('device_exit_nonzero');
        if(!device.stderrEnded)cleanupFailures.push('device_stderr_not_drained');}catch{cleanupFailures.push('device_process_cleanup_failed');}
      if(device.browserOpened)try{await browser(device.browser,device.env,'close');}catch{cleanupFailures.push('browser_cleanup_failed');}
    }
    report.processes=devices.map(device=>({exited:device.exited,exitCode:Number.isInteger(device.exitCode)?device.exitCode:null,
      signalled:device.child.signalCode!==null,stderrEnded:device.stderrEnded,stderr:device.stderrFact}));
    if(cleanupFailures.length)report={...report,result:'fail',cleanupFailures,
      ...(report.result==='pass'?{stage:'cleanup',reason:'resource_cleanup_failed'}:{})};
  }
  if(report.result!=='pass')throw Error(JSON.stringify(report));
  return report;
}
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(error.message);process.exitCode=1;});
module.exports={configuration,decodedMedia,stoppedMedia,finishDevice,deviceCompletion,run};
