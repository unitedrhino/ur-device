// 文件说明：四端正式会议真实ICE暂断后的同Peer自然恢复；故障仅由中央受控函数施加，不查找或停止进程。
'use strict';
const assert=require('node:assert/strict');
const {setTimeout:delay}=require('node:timers/promises');
const conference=require('./conference_member_live.cjs');

// configuration 在任何正式会议资源建立前要求显式验收，不把其他场景开关当作网络恢复许可。
function configuration(env=process.env) {
  assert.equal(env.UR_CONFERENCE_NETWORK_RECOVERY_LIVE,'1','explicit_conference_network_recovery_required');
  assert.equal(env.UR_CONFERENCE_MEMBER_LIVE,'1','actual_conference_driver_required');
  assert.equal(env.UR_CONFERENCE_FIXED_PCM_TEST,'1','actual_fixed_pcm_required');
  return true;
}

// observeResources 只在原浏览器内比较真实引用；返回有限事实，不输出端点、成员或会话身份。
function observeResources(session,isDevice,remember) {
  const view=isDevice?views.get(session):null;
  const peer=isDevice?view?.peer:window.__ipcPeer,stream=isDevice?view?.stream:window.__capture;
  const tracks=stream?.getAudioTracks()??[],remote=isDevice?view?.remoteTrack:window.__remoteTrack;
  if(!peer||!stream||tracks.length!==1||!remote)throw Error('actual_original_conference_resources_required');
  if(remember){
    const original={peer,stream,track:tracks[0],remote,enabled:tracks[0].enabled,firstDisconnectedAt:null};
    // 由真实ICE事件记首次时间，不能用四端逐个CLI取数结束时的晚时间假造五秒预算。
    original.listener=()=>{if(peer.iceConnectionState==='disconnected'&&original.firstDisconnectedAt===null)original.firstDisconnectedAt=performance.now();};
    peer.addEventListener('iceconnectionstatechange',original.listener);
    window.__conferenceNetworkOriginal=original;
  }
  const original=window.__conferenceNetworkOriginal;
  return {samePeer:peer===original?.peer,sameStream:stream===original?.stream,sameTrack:tracks[0]===original?.track,
    sameRemote:remote===original?.remote,senderBound:peer.getSenders().some(sender=>sender.track===tracks[0]),
    captureLive:tracks[0].readyState==='live',remoteLive:remote.readyState==='live',enabled:tracks[0].enabled,
    connectionState:peer.connectionState,iceConnectionState:peer.iceConnectionState,
    firstDisconnectedAgeMs:original?.firstDisconnectedAt===null?null:performance.now()-original.firstDisconnectedAt};
}

// verifyResources 不允许重新采集、替Peer、丢失sender或隐式解除静音冒充自然恢复。
function verifyResources(rows,{connected=false}={}) {
  assert.ok(Array.isArray(rows)&&rows.length===4,'four_actual_resource_observations_required');
  rows.forEach((row,index)=>{
    for(const name of ['samePeer','sameStream','sameTrack','sameRemote','senderBound','captureLive','remoteLive'])
      assert.equal(row?.[name],true,'original_conference_resources_changed');
    assert.equal(row.enabled,index!==3,'local_mute_intent_changed');
    assert.ok(['new','checking','connected','completed','disconnected'].includes(row.iceConnectionState),'terminal_ice_state_forbidden');
    assert.ok(!['failed','closed'].includes(row.connectionState),'terminal_peer_state_forbidden');
    assert.ok(row.firstDisconnectedAgeMs===null||(Number.isFinite(row.firstDisconnectedAgeMs)&&row.firstDisconnectedAgeMs>=0),'actual_ice_event_clock_required');
    if(connected){assert.equal(row.connectionState,'connected');assert.ok(['connected','completed'].includes(row.iceConnectionState));}
  });
  return true;
}

// verifyAuthority 核原活加入实例及MAC不变；不把群成员展示列表当作发言许可。
function verifyAuthority(before,after) {
  assert.ok(Array.isArray(before)&&before.length===3&&Array.isArray(after)&&after.length===3,'three_im_authority_observations_required');
  for(let index=0;index<3;index++) {
    for(const key of ['memberEpoch','audioRevision'])assert.ok(Number.isSafeInteger(before[index][key])&&before[index][key]>0,'actual_im_authority_required');
    assert.equal(typeof before[index].publishAllowed,'boolean');
    for(const key of ['memberEpoch','audioRevision','publishAllowed'])assert.equal(after[index]?.[key],before[index][key],'original_im_authority_changed');
  }
  return true;
}

// probe 使用原onEstablished窗口；fault必须在finally恢复其精确资源，不能返回后仍有后台施障任务。
async function probe(context,{fault,clock=()=>performance.now(),wait=milliseconds=>delay(milliseconds)}={}) {
  const {users,api,browser,evaluated,sourceName,sourceEnv,sourceSession,callId,signal}=context;
  assert.equal(typeof fault,'function','central_owned_fault_function_required');
  assert.ok(signal instanceof AbortSignal&&users?.length===3&&typeof context.phase==='function');
  const endpoints=[{name:sourceName,env:sourceEnv,index:0},...users];
  // resources 仅在四个独立session并发读取；安装原观察器仍串行，不能并发静音或释放动作。
  async function resources(remember=false) {
    assert.equal(new Set(endpoints.map(endpoint=>endpoint.name)).size,4,'four_independent_browser_sessions_required');
    // read 保留每端调用开始的单调下界；网页采样到返回之间的耗时不能延长原五秒。
    async function read(endpoint) {
      signal.throwIfAborted();
      const snapshotStartedAt=clock();
      const row=evaluated(await browser(endpoint.name,endpoint.env,'eval',
        `(${observeResources.toString()})(${JSON.stringify(sourceSession)},${endpoint.index===0},${remember})`));
      return {...row,snapshotStartedAt};
    }
    if(remember){const rows=[];for(const endpoint of endpoints)rows.push(await read(endpoint));return rows;}
    // 失败或取消也等待同批全部原调用结束，不让清理与残留读取并行；每端顺序与来源保持不变。
    const settled=await Promise.allSettled(endpoints.map(read));
    signal.throwIfAborted();
    const rejected=settled.find(result=>result.status==='rejected');
    if(rejected)throw rejected.reason;
    return settled.map(result=>result.value);
  }
  // authority 经本人登录正式读取IM，只有三份原许可都不变才能称原会议继续。
  async function authority() {
    const rows=[];
    for(const user of users){
      signal.throwIfAborted();
      const value=await api(user,'im/interaction/talk/audio-access',{operation:'read',callId,targetUserID:user.id},true,'POST',signal);
      rows.push({memberEpoch:value.memberEpoch,audioRevision:value.audioRevision,publishAllowed:value.publishAllowed});
    }
    await context.activeMembers(users.map(user=>user.id));
    return rows;
  }
  let localMuted=false,firstDisconnected=null,observedAllDisconnected=false,peakDisconnected=0,recoveredAt=null;
  let phase='initial_authority',primary=null; // 仅闭集阶段进入报告，原异常正文只保留在不可枚举cause。
  let beforeAuthority;
  try {
    beforeAuthority=await authority();phase='local_mute';
    // 原APP测试宿主没有独立静音按钮，此处执行本端用户驱动动作，不下发远程开麦或修改MAC。
    await browser(users[2].name,users[2].env,'eval','(()=>{const track=__capture.getAudioTracks()[0];if(!track||!track.enabled)throw Error("original_live_mic_required");track.enabled=false;return true;})()');
    localMuted=true;
    phase='initial_resources';verifyResources(await resources(true),{connected:true});
    phase='baseline_muted_decode';
    await context.phase('network_recovery_local_mute_before_fault',[0,1,2,3],[0,1,2]);
    phase='baseline_snapshot';
    const beforeMedia=await context.snapshotMedia(true);
    // observeDisconnected 只读真实ICE；首次任一端disconnected开启同Peer五秒窗口，不能由fault返回值伪造。
    async function observeDisconnected() {
      phase='fault_observation';
      const rows=await resources();verifyResources(rows);
      const count=rows.filter(row=>row.iceConnectionState==='disconnected').length;
      if(count){
        const ages=rows.filter(row=>row.iceConnectionState==='disconnected').map(row=>row.firstDisconnectedAgeMs);
        assert.ok(ages.every(age=>Number.isFinite(age)&&age>=0),'actual_disconnection_event_required');
        // 调用开始时间减网页事件年龄不晚于实际事件；逐端取最早保守下界，不能用整批结束时刻后移首事件。
        const observed=Math.min(...rows.filter(row=>row.iceConnectionState==='disconnected')
          .map(row=>row.snapshotStartedAt-row.firstDisconnectedAgeMs));
        firstDisconnected=firstDisconnected===null?observed:Math.min(firstDisconnected,observed);
      }
      peakDisconnected=Math.max(peakDisconnected,count);
      observedAllDisconnected ||= count===4;
      if(firstDisconnected!==null)assert.ok(clock()-firstDisconnected<5000,'natural_recovery_window_expired');
      phase='fault_disconnection';
      return {disconnectedCount:count,allDisconnected:count===4};
    }
    phase='fault_disconnection';
    const faultFact=await fault({signal,observeDisconnected});
    phase='fault_receipt';
    verifyFaultReceipt(faultFact);
    // 暂停整个鉴权后端仅是安全停流负向；不能因短暂停后Peer仍连接就冒称媒体网络恢复。
    assert.equal(faultFact.kind,'owned_media_udp_tuple_drop','backend_pause_not_media_recovery');
    assert.equal(observedAllDisconnected,true,'four_actual_ice_disconnections_required');
    assert.notEqual(firstDisconnected,null,'actual_ice_disconnection_required');
    let actualResources;
    phase='restored_resources';
    while(clock()-firstDisconnected<5000) {
      signal.throwIfAborted();actualResources=await resources();verifyResources(actualResources);
      if(actualResources.every(row=>row.connectionState==='connected'&&['connected','completed'].includes(row.iceConnectionState))){recoveredAt=clock();break;}
      await wait(50);
    }
    assert.notEqual(recoveredAt,null,'natural_recovery_window_expired');
    assert.ok(recoveredAt-firstDisconnected<5000,'natural_recovery_window_expired');
    verifyResources(actualResources,{connected:true});
    phase='restored_muted_decode';
    await context.phase('network_recovery_same_peers_fresh_muted_mix',[0,1,2,3],[0,1,2]);
    const afterMedia=await context.snapshotMedia(false);
    phase='restored_media_continuity';
    conference.verifySnapshotContinuity(beforeMedia,afterMedia,4,[3]);
    phase='restored_authority';
    verifyAuthority(beforeAuthority,await authority());
    // 断网恢复本身不解除本端静音；只有本次明确用户动作才再次启用原track并验四个真实源。
    phase='explicit_unmute';
    await browser(users[2].name,users[2].env,'eval','(()=>{const original=window.__conferenceNetworkOriginal,track=__capture.getAudioTracks()[0];if(track!==original?.track||track.enabled)throw Error("original_muted_mic_required");track.enabled=true;return true;})()');
    localMuted=false;
    phase='restored_four_source_decode';
    await context.phase('network_recovery_explicit_unmute_four_actual_sources',[0,1,2,3],[0,1,2,3]);
    return {report:{path:'formal_conference_same_peer_natural_ice_recovery',actualDisconnectedEndpoints:peakDisconnected,
      faultKind:faultFact.kind,interruptionElapsedMs:faultFact.elapsedMs,actualRecoveryMs:Math.round(recoveredAt-firstDisconnected),samePeers:true,
      sameCaptureStreams:true,localMutePreserved:true,imMemberEpochAndMACPreserved:true,freshDecodedFourEndpoints:true,
      mqttReauthenticated:false,mediaRenegotiated:false}};
  } catch(error) {
    // 只保留协议判据闭集；浏览器/服务原正文、标识及地址不能通过Error反射进报告。
    const reasons=new Set(['natural_recovery_window_expired','four_actual_ice_disconnections_required',
      'actual_ice_disconnection_required','actual_disconnection_event_required','fixed_fault_receipt_required',
      'bounded_actual_backend_pause_required','bounded_actual_media_interruption_required','backend_pause_not_media_recovery','original_conference_resources_changed','local_mute_intent_changed',
      'terminal_ice_state_forbidden','terminal_peer_state_forbidden','actual_ice_event_clock_required',
      'original_im_authority_changed','browser_eval_failed']);
    const firstLine=typeof error?.message==='string'?error.message.split('\n',1)[0]:'';
    const reason=signal.aborted?'cancelled':reasons.has(firstLine)?firstLine:'operation_failed';
    primary=new Error(`conference_network_recovery_${reason}`,{cause:error});
    primary.networkRecoveryDiagnostic={phase,reason,cleanupFailures:0};
    throw primary;
  } finally {
    // 失败时仅恢复本用例主动设置的本端静音；正式会议run的finally继续负责原会话/Peer/设备释放。
    let cleanupFailures=0;
    // 每端独立尝试，单个已关闭页面不能阻断其余原观察器清理，更不能覆盖已保存真实首因。
    if(localMuted)try{await browser(users[2].name,users[2].env,'eval','(()=>{const track=__capture?.getAudioTracks()[0];if(track&&track.readyState==="live")track.enabled=true;return true;})()');}catch{cleanupFailures++;}
    for(const endpoint of endpoints)try{await browser(endpoint.name,endpoint.env,'eval','(()=>{const original=window.__conferenceNetworkOriginal;if(original?.listener)original.peer.removeEventListener("iceconnectionstatechange",original.listener);return true;})()');}catch{cleanupFailures++;}
    if(primary)primary.networkRecoveryDiagnostic.cleanupFailures=cleanupFailures;
    else if(cleanupFailures){
      const error=new Error('conference_network_recovery_cleanup_failed');
      error.networkRecoveryDiagnostic={phase:'cleanup',reason:'cleanup_failed',cleanupFailures};
      throw error;
    }
  }
}

// verifyFaultReceipt 区分两个固定故障端口回执：后台暂停用于负向，媒体隔离才可进入恢复判据；两者原20秒上限不变。
function verifyFaultReceipt(value){
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'fixed_fault_receipt_required');
  if(value.kind==='backend_pause'){
    assert.equal(Object.keys(value).sort().join(','),'kind,pauseElapsedMs,resumed,stopped','fixed_fault_receipt_required');
    assert.equal(value.stopped,true,'fixed_fault_receipt_required');assert.equal(value.resumed,true,'fixed_fault_receipt_required');
    assert.ok(Number.isFinite(value.pauseElapsedMs)&&value.pauseElapsedMs>0&&value.pauseElapsedMs<=20000,'bounded_actual_backend_pause_required');
  }else{
    assert.equal(value.kind,'owned_media_udp_tuple_drop','fixed_fault_receipt_required');
    assert.equal(Object.keys(value).sort().join(','),'elapsedMs,interrupted,kind,restored','fixed_fault_receipt_required');
    assert.equal(value.interrupted,true,'fixed_fault_receipt_required');assert.equal(value.restored,true,'fixed_fault_receipt_required');
    assert.ok(Number.isFinite(value.elapsedMs)&&value.elapsedMs>0&&value.elapsedMs<=20000,'bounded_actual_media_interruption_required');
  }
  return true;
}

// run 只接受中央显式注入的故障函数；默认会议驱动无hook行为保持不变。
async function run(options={},env=process.env) {
  configuration(env);assert.equal(typeof options.fault,'function','central_owned_fault_function_required');
  let report;
  const outcome=await conference.run({onEstablished:async context=>{const result=await probe(context,options);report=result.report;return result;}});
  assert.equal(outcome?.result,'pass','complete_formal_conference_required');assert.ok(report);
  return {...outcome,networkRecovery:report};
}

module.exports={verifyFaultReceipt,configuration,observeResources,verifyResources,verifyAuthority,probe,run};
