// 文件说明：嵌入正式四端会议验收的同用户APP成员热接续；不新建设备扬声器腿，不伪造媒体或ready。
'use strict';
const assert=require('node:assert/strict');
const {randomUUID}=require('node:crypto');
const {setTimeout:delay}=require('node:timers/promises');
const {attachListeningAudio}=require('./ipc_live.cjs');
const {FREQUENCIES,spectralPeaks,verifyMix,verifyFixedPCMSettings,verifySnapshotContinuity}=require('./conference_member_live.cjs');

// handoffFailureDiagnostic 仅输出固定流程阶段与清理布尔值，不读取自由错误正文或设备身份。
function handoffFailureDiagnostic(error) {
  const phases=['register','candidateMedia','audioAccess','snapshot','prepare','open','mediaOffer','mediaConnect','mediaState','ready','originalContinue','commit','terminal','sourceStop','targetStart','targetMix','finalContinuity','replay','cleanup'];
  return {phase:phases.includes(error?.handoffPhase)?error.handoffPhase:null,cleanupFailed:error?.handoffCleanupFailed===true};
}

// selectedMember 显式区分初始控制者和额外U2；两个gate不能同时选择，默认不运行真实接续。
function selectedMember(env=process.env) {
  const initial=env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE==='1',extra=env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE==='1';
  assert.equal(Number(initial)+Number(extra),1,'explicit_single_member_handoff_required');
  return initial?0:1;
}

// validateReplacement 保持原用户/IM位置，候选只替换APP端点；完整生命周期交还原会议finally继续负责。
function validateReplacement(original,target,startedBrowsers) {
  for(const key of ['id','token','index','env'])assert.equal(target[key],original[key],'same_original_member_required');
  for(const key of ['name','endpoint','session'])assert.ok(typeof target[key]==='string'&&target[key].length>0&&target[key]!==original[key],'fresh_endpoint_required');
  assert.ok([1,2].includes(target.index),'original_controller_or_extra_member_required');
  assert.equal(target.state,'established');assert.equal(target.ended,false);
  assert.ok(Array.isArray(target.events)&&target.events.length<=128);
  assert.ok(Array.isArray(target.stateTimeline)&&target.stateTimeline.length>0&&target.stateTimeline.length<=256);
  assert.equal(startedBrowsers.has(target.name),true,'candidate_cleanup_owner_required');
  assert.equal(original.state,'ended');assert.equal(original.ended,true);
  assert.equal(original.stateTimeline.filter(state=>state==='ended').length,1,'original_terminal_unique_required');
  return true;
}

// lifecycleStopped 校验实际HAL资源引用；协议终态必须先到，再停原资源，不能预先close Peer制造通过。
function lifecycleStopped(facts) {
  assert.equal(facts?.terminalObserved,true);assert.equal(facts?.peerClosed,true);
  assert.ok(Array.isArray(facts.capture)&&facts.capture.length===1&&facts.capture.every(state=>state==='ended'));
  assert.equal(facts.remote,'ended');assert.equal(facts.spectrumClosed,true);
  assert.equal(facts.audioClosed,true);assert.equal(facts.sinkReleased,true);
  return true;
}

// run 在唯一正式onEstablished窗口执行；signal贯穿HTTP及等待，finally使用独立有限清理信号。
async function run(context) {
  const sourcePosition=selectedMember();
  const {users,api,browser,evaluated,startedBrowsers,viewerURL,installSpectrum,sourceName,sourceEnv,sourceSession,callId,signal}=context;
  assert.ok(signal instanceof AbortSignal&&typeof installSpectrum==='function'&&users.length===3);
  signal.throwIfAborted();
  const source=users[sourcePosition],target={...source,name:`conference-handoff-${randomUUID()}`,endpoint:'',session:'',events:[],state:'',ended:false,stateTimeline:[]};
  assert.equal(source.index,sourcePosition+1);assert.equal(source.state,'established');assert.equal(source.ended,false);
  const retainedIndices=[0,1,2,3].filter(index=>index!==source.index);
  const handoffId=randomUUID(),tokens={prepare:randomUUID(),commit:randomUUID(),abort:randomUUID()};
  let prepared=false,committed=false,complete=false,revision=0,commitPayload,oldStopped=false,phase='register',failure;
  const report={path:sourcePosition===0?'initial_controller_same_owner_actual_cpp_conference':'original_im_member_same_owner_actual_cpp_conference',steadyEndpoints:4,peakEndpoints:5,originalMemberPreserved:false,originalHALStopped:false};
  // request 不用原pump吞掉endpointState；原鉴权API尾参数保留独立AbortSignal。
  const request=(user,path,body,operationSignal=signal)=>api(user,path,body,true,'POST',operationSignal);
  // receive 保存所有本场景事实及原状态时间线，只在确实消费后确认outbox。
  async function receive(user,operationSignal=signal) {
    const value=await request(user,'im/interaction/events',{endpointId:user.endpoint,limit:32},operationSignal);
    for(const delivery of value.list??[]) {
      assert.ok(typeof delivery.deliveryId==='string'&&delivery.event&&typeof delivery.event.msgToken==='string');
      const event=delivery.event;
      if(event.method==='interaction.state'&&event.sessionId===user.session){
        user.state=event.data?.state;user.stateTimeline.push(user.state);user.ended=user.state==='ended';
      }
      assert.ok(user.events.length<128&&user.stateTimeline.length<=256,'bounded_handoff_history_required');user.events.push(event);
    }
    if(value.list?.length)await request(user,'im/interaction/events/ack',{endpointId:user.endpoint,deliveryIds:value.list.map(item=>item.deliveryId)},operationSignal);
  }
  // until 等待同一原操作的实际结果，不延长prepare绝对30秒，不在取消后后台继续提交。
  async function until(probe,operationSignal=signal,milliseconds=15000) {
    const deadline=performance.now()+milliseconds;
    do{operationSignal.throwIfAborted();const result=await probe();if(result)return result;await delay(100,undefined,{signal:operationSignal});}while(performance.now()<deadline);
    throw Error('member_handoff_actual_state_timeout');
  }
  // command 根据method选择严格关联的原结果；HTTPaccepted不作为ready或commit成功。
  async function command(user,method,params,token=randomUUID(),operationSignal=signal) {
    const command={method,msgToken:token,params,...(method==='interaction.open'?{target:{productID:process.env.UR_DEVICE_PRODUCT,deviceName:process.env.UR_DEVICE_NAME}}:{sessionId:user.session})};
    if(method==='endpointControl'&&params.operation==='commit')commitPayload=command;
    const accepted=await request(user,'im/interaction/commands',{endpointId:user.endpoint,command},operationSignal);
    assert.equal(accepted.accepted,true);assert.equal(accepted.msgToken,token);
    return until(async()=>{
      await receive(user,operationSignal);
      const expected=method==='endpointControl'?'endpointState':'interaction.result';
      const index=user.events.findIndex(event=>event.method===expected&&event.msgToken===token&&Object.hasOwn(event,'code'));
      if(index<0)return false;
      const result=user.events.splice(index,1)[0];assert.equal(result.code,200,'member_handoff_original_operation_rejected');return result;
    },operationSignal);
  }
  // stopMedia 只有收到终态才作正常HAL停止；失败finally另走精确强制收尾，不作为通过证据。
  async function stopMedia(user,terminalObserved) {
    const facts=evaluated(await browser(user.name,user.env,'eval',`(async()=>{const peer=window.__ipcPeer,capture=window.__capture,remote=window.__remoteTrack;
      if(${terminalObserved===true}&&(!peer||!capture))throw Error('actual_original_resources_required');
      for(const track of capture?.getTracks()??[])track.stop();if(remote)remote.stop();if(peer)peer.close();
      if(window.__conferenceSpectrum)await __conferenceSpectrum.stop();
      if(window.__ipcAudio)await __ipcAudio.stop();
      for(const sink of document.querySelectorAll('audio')){sink.pause();sink.srcObject=null;sink.remove();}
      return JSON.stringify({terminalObserved:${terminalObserved===true},peerClosed:peer?.connectionState==='closed',capture:(capture?.getAudioTracks()??[]).map(track=>track.readyState),
        remote:remote?.readyState??'absent',spectrumClosed:window.__conferenceSpectrum?.closed===true,
        audioClosed:window.__ipcAudio?.context.state==='closed',sinkReleased:window.__ipcAudio?.sink.srcObject===null});})()`));
    if(terminalObserved)lifecycleStopped(facts);
    return facts;
  }
  try {
    target.endpoint=(await request(target,'im/interaction/endpoints/register',{})).endpointId;assert.ok(target.endpoint&&target.endpoint!==source.endpoint);
    phase='candidateMedia';
    startedBrowsers.add(target.name);await browser(target.name,target.env,'open',viewerURL);
    // 浏览器冷启动/采集放在prepare之前；硬关闭候选麦克风，held服务端屏障之外再核物理采集发送状态。
    const offer=evaluated(await browser(target.name,target.env,'eval',`(async()=>{const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});
      const capture=window.__capture=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false},video:false});
      const track=capture.getAudioTracks()[0];track.enabled=false;const transceiver=peer.addTransceiver(track,{direction:'sendrecv',streams:[capture]});
      transceiver.setCodecPreferences(RTCRtpSender.getCapabilities('audio').codecs.filter(codec=>codec.mimeType.toLowerCase()==='audio/opus'));
      peer.ontrack=event=>{window.__remoteTrack=event.track;(${attachListeningAudio.toString()})(event.track);
        (${installSpectrum.toString()})(event.track,${JSON.stringify(FREQUENCIES)},${spectralPeaks.toString()});};
      await peer.setLocalDescription(await peer.createOffer());if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>reject(Error('candidate_ice_timeout')),8000);const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
        peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);})()`));
    const settings=evaluated(await browser(target.name,target.env,'eval','JSON.stringify(__capture.getAudioTracks().map(track=>{const s=track.getSettings();return {echoCancellation:s.echoCancellation,noiseSuppression:s.noiseSuppression,autoGainControl:s.autoGainControl};}))'));
    verifyFixedPCMSettings(settings);
    phase='audioAccess';
    const permission=await request(source,'im/interaction/talk/audio-access',{operation:'read',callId,targetUserID:source.id});
    phase='snapshot';
    const before=await context.snapshotMedia(true,retainedIndices);
    phase='prepare';
    const prepare=await command(source,'endpointControl',{operation:'prepare',expectedRevision:0,handoffId,target:{kind:'endpoint',id:target.endpoint}},tokens.prepare);
    assert.equal(prepare.data?.state,'preparing');revision=prepare.data.revision;prepared=true;
    phase='open';
    const opened=await command(target,'interaction.open',{purpose:'ipcTalk',cameraId:process.env.UR_CONFERENCE_CAMERA_ID,callId,handoff:{sourceSessionId:source.session,handoffId}});
    target.session=opened.sessionId;assert.ok(target.session&&target.session!==source.session);
    phase='mediaOffer';
    const answered=await command(target,'interaction.mediaOffer',{mediaRevision:1,webrtc:{iceMode:'complete',sdp:offer}});
    await browser(target.name,target.env,'eval',`(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answered.data.webrtc.sdp)}});return true;})()`);
    phase='mediaConnect';
    await until(async()=>evaluated(await browser(target.name,target.env,'eval','__ipcPeer.connectionState==="connected"'))===true);
    phase='mediaState';
    await command(target,'interaction.mediaState',{mediaRevision:1,state:'connected'});
    phase='ready';
    const ready=await until(async()=>{await receive(source);await receive(target);return source.events.find(event=>event.method==='endpointState'&&!Object.hasOwn(event,'code')&&event.sessionId===source.session&&event.data?.handoffId===handoffId&&event.data?.state==='ready');});
    assert.equal(ready.data.candidateSessionId,target.session);revision=ready.data.revision;
    assert.equal(evaluated(await browser(target.name,target.env,'eval','__capture.getAudioTracks().every(track=>track.enabled===false&&track.readyState==="live")')),true,'candidate_microphone_enabled_before_commit');
    phase='originalContinue';
    assert.equal(source.ended,false);await context.phase('handoff_ready_original_four_continue',[0,1,2,3],[0,1,2,3]);
    const held=evaluated(await browser(target.name,target.env,'eval','JSON.stringify({received:window.__conferenceSpectrum?.nonSilentWindows??0})'));
    assert.equal(held.received,0,'candidate_must_not_receive_mix_before_commit');
    phase='commit';
    const result=await command(source,'endpointControl',{operation:'commit',expectedRevision:revision,handoffId,candidateSessionId:target.session},tokens.commit);
    assert.equal(result.data?.state,'committed');committed=true;
    phase='terminal';
    await until(async()=>{await receive(source);await receive(target);return source.state==='ended'&&target.state==='established';});
    phase='sourceStop';
    await stopMedia(source,true);oldStopped=true;await browser(source.name,source.env,'close');startedBrowsers.delete(source.name);
    phase='targetStart';
    await browser(target.name,target.env,'eval','(()=>{__capture.getAudioTracks()[0].enabled=true;return true;})()');
    phase='targetMix';
    await until(async()=>{
      const facts=evaluated(await browser(target.name,target.env,'eval','JSON.stringify({observations:__conferenceSpectrum?.observations??0,peaks:__conferenceSpectrum?.peaks??[]})'));
      try{verifyMix(facts,target.index,[0,1,2,3]);return true;}catch{return false;}
    });
    phase='finalContinuity';
    const after=await context.snapshotMedia(false,retainedIndices);verifySnapshotContinuity(before,after,3,[]);
    const current=await request(target,'im/interaction/talk/audio-access',{operation:'read',callId,targetUserID:target.id});
    assert.equal(current.memberEpoch,permission.memberEpoch);assert.equal(current.publishAllowed,permission.publishAllowed);assert.equal(current.audioRevision,permission.audioRevision);
    await context.activeMembers(users.map(user=>user.id));
    assert.equal(evaluated(await browser(sourceName,sourceEnv,'eval','__conferenceInvites.size')),1,'new_device_speaker_leg_forbidden');
    assert.equal(evaluated(await browser(sourceName,sourceEnv,'eval',`views.get(${JSON.stringify(sourceSession)})?.active===true`)),true);
    phase='replay';
    const replay=await command(source,'endpointControl',commitPayload.params,tokens.commit);assert.deepEqual(replay,result,'original_commit_result_changed');
    validateReplacement(source,target,startedBrowsers);
    context.validateEstablishedReplacement(source,target,startedBrowsers);
    report.originalMemberPreserved=true;report.originalHALStopped=true;
    complete=true;return {replacementUser:target,report};
  } catch(error) {
    failure=error instanceof Error?error:Error('member_handoff_failed');
    failure.handoffPhase=phase;
    throw failure;
  } finally {
    if(!complete) {
      const cleanupSignal=AbortSignal.timeout(20000);let cleanupError;
      // 取消可能发生在HTTP已送达而应答尚未回读之后：先观察原token持久事实，不重提commit推进业务。
      if(commitPayload&&!committed)try{
        await receive(source,cleanupSignal);
        const result=source.events.find(event=>event.method==='endpointState'&&event.msgToken===tokens.commit&&event.data?.handoffId===handoffId&&Object.hasOwn(event,'code'));
        if(result?.code===200&&result.data?.state==='committed')committed=true;
      }catch(error){cleanupError=error;}
      // 无论取消时commit应答是否已到，精确结束候选都会阻止晚到提交再获活媒体；不把未知提交宣称abort成功。
      if(prepared&&!committed)try{await command(source,'endpointControl',{operation:'abort',expectedRevision:revision,handoffId},tokens.abort,cleanupSignal);}catch(error){cleanupError=error;}
      if(target.session)try{await command(target,'interaction.end',{reason:'userStopped'},randomUUID(),cleanupSignal);}catch(error){cleanupError??=error;}
      if(startedBrowsers.has(target.name)){
        try{await stopMedia(target,false);}catch(error){cleanupError??=error;}
        try{await browser(target.name,target.env,'close');}catch(error){cleanupError??=error;}
      }
      if(committed&&!oldStopped)try{await receive(source,cleanupSignal);if(source.state==='ended')await stopMedia(source,true);}catch(error){cleanupError??=error;}
      if(cleanupError) {
        // 清理失败不能覆盖原正式API码或等待阶段，否则会丢失首次可复现的业务根因。
        if(failure)failure.handoffCleanupFailed=true;
        else {const error=Error('member_handoff_cleanup_failed',{cause:cleanupError});error.handoffPhase='cleanup';error.handoffCleanupFailed=true;throw error;}
      }
    }
  }
}

module.exports={run,selectedMember,validateReplacement,lifecycleStopped,handoffFailureDiagnostic};
