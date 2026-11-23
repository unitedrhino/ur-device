// 文件说明：三个真实应用本人同意后录制四端实际会议全混音；复用正式会议驱动，私有签名只在内存使用。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {randomUUID,createHash} = require('node:crypto');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const conference = require('./conference_member_live.cjs');
const {contentMetadata,decodedPCM,retiredPrivateURL} = require('./cloud_record_live.cjs');

// configuration 在会议及浏览器启动前要求专用开关、原固定PCM驱动与显式策略，不接受外部服务目标。
function configuration(env) {
  assert.equal(env.UR_CLOUD_CONFERENCE_RECORD_LIVE,'1','explicit_conference_record_required');
  assert.equal(env.UR_CONFERENCE_MEMBER_LIVE,'1','actual_conference_driver_required');
  assert.equal(env.UR_CONFERENCE_FIXED_PCM_TEST,'1','actual_fixed_pcm_required');
  assert.ok(typeof env.UR_CLOUD_RECORD_POLICY_ID==='string'&&/^[A-Za-z0-9_.:-]{1,128}$/.test(env.UR_CLOUD_RECORD_POLICY_ID),'explicit_record_policy_required');
  const base=new URL(env.UR_IPC_BASE_URL);
  assert.ok(base.protocol==='http:'&&base.hostname==='127.0.0.1'&&!base.username&&!base.password&&base.pathname==='/'&&!base.search&&!base.hash,'isolated_record_backend_required');
  assert.ok(env.UR_CLOUD_RECORD_MEMBER_REJOIN===undefined||['0','1'].includes(env.UR_CLOUD_RECORD_MEMBER_REJOIN),'member_rejoin_flag_invalid');
  return {policy:env.UR_CLOUD_RECORD_POLICY_ID,...(env.UR_CLOUD_RECORD_MEMBER_REJOIN==='1'?{memberRejoin:true}:{})};
}

// recordedMix 对真实封装文件解码PCM逐窗测四个频率；缺少任一输入，不能以听众mix-minus-self冒充完整录音。
function recordedMix(bytes) {
  const pcm=decodedPCM(bytes),samples=new Float32Array(bytes.length/2);
  for(let index=0;index<samples.length;index++)samples[index]=bytes.readInt16LE(index*2)/32768;
  const peaks=conference.FREQUENCIES.map(()=>0);
  for(let offset=0;offset+4096<=samples.length;offset+=2048){
    const window=conference.spectralPeaks(samples.subarray(offset,offset+4096),48000,conference.FREQUENCIES);
    window.forEach((value,index)=>{peaks[index]=Math.max(peaks[index],value);});
  }
  assert.ok(peaks.every(value=>Number.isFinite(value)&&value>0.008),'all_actual_conference_sources_required');
  return {...pcm,peaks};
}

// waitFor 用固定截止串行轮询，未决请求由各自HTTP超时终止，不遗留race后台操作。
async function waitFor(predicate,timeout=20000,signal) {
  const deadline=Date.now()+timeout;
  while(Date.now()<deadline){signal?.throwIfAborted();if(await predicate())return;await new Promise(resolve=>setTimeout(resolve,100));}
  throw Error('cloud_conference_record_timeout');
}

// createRecordingStateProjection 只累计各原端点收到的同实例状态；跨媒体腿重入不清空已确认事实，也不授媒体权限。
function createRecordingStateProjection(users,sourceSession,recordingId) {
  const facts=new Map(),latestByUser=new Map();let state=null;
  return {
    observe(user,event) {
      if(!users.includes(user) || event?.sessionId!==sourceSession || event.method!=='interaction.controlState' ||
          event.data?.command!=='recordingState')return false;
      const value=event.data.result;
      if(value?.recordingId!==recordingId || !Number.isSafeInteger(value.revision) || value.revision<=0 ||
          !['active','paused','finalizing','ready','failed'].includes(value.state) ||
          (value.reason!==undefined && (typeof value.reason!=='string'||value.reason.length>128)))return false;
      const previous=latestByUser.get(user);
      if(previous && value.revision<=previous.revision)return false; // DUP、迟到旧修订及同修订冲突不能回退原事实。
      const actual={state:value.state,revision:value.revision,...(value.reason!==undefined?{reason:value.reason}:{})};
      latestByUser.set(user,actual);facts.set(`${user.id}:${actual.state}`,actual.revision);
      if(!state||actual.revision>state.revision)state=actual;
      assert.ok(facts.size<=32&&latestByUser.size<=3,'bounded_record_observation_required');return true;
    },
    // read 返回已收到的最高修订投影；不从一端到达推断其他端到达。
    read() {return state;},
    // allObserved 只判断全部原用户是否实际收到指定状态和同一修订。
    allObserved(expected,revision) {return users.every(user=>facts.get(`${user.id}:${expected}`)===revision);},
    // indicators 返回已观察到指定状态修订的端点数量，只作有界失败旁证。
    indicators(expected,revision) {return users.filter(user=>facts.get(`${user.id}:${expected}`)===revision).length;}
  };
}

// recordingFailureDiagnostic 只保留固定阶段和已知失败分类，任意API正文、用户身份、URL和错误堆栈均不能进入报告。
function recordingFailureDiagnostic(error,stage,rejoinObservation,staleObservation) {
  const reasons=new Set(['cloud_conference_record_timeout','established_hook_timeout','formal_api_rejected','browser_eval_failed',
    'original_member_epoch_required','old_actual_terminal_required','old_actual_media_release_required','fresh_joined_epoch_required',
    'fresh_media_session_required','fresh_actual_established_required','original_record_result_required','original_operation_rejected','actual_state_timeout',
    'original_member_required','non_manager_actual_member_required']);
  const phases=new Set(['originalAccess','endCommand','waitTerminal','closeMedia','groupLeave','remainingMembers','groupJoin','renewedAccess','newSession','newMedia','activeMembers']);
  const validRejoin=rejoinObservation && ['awaitPaused','allIndicators'].includes(rejoinObservation.stage) &&
    ['missing','active','paused','finalizing','ready','failed'].includes(rejoinObservation.state) &&
    typeof rejoinObservation.revisionAdvanced==='boolean' && typeof rejoinObservation.pausedReasonMatches==='boolean' &&
    Number.isInteger(rejoinObservation.indicators) && rejoinObservation.indicators>=0 && rejoinObservation.indicators<=3;
  const validStale=staleObservation && ['rejectOld','newOwnGrants','newIds'].includes(staleObservation.stage) &&
    ['notObserved','accepted','rejected'].includes(staleObservation.outcome) &&
    (staleObservation.code===null||(Number.isSafeInteger(staleObservation.code)&&staleObservation.code>=0)) &&
    Number.isInteger(staleObservation.ownGrants)&&staleObservation.ownGrants>=0&&staleObservation.ownGrants<=3 &&
    ['newGrantFormat','newGrantRevisionOne','newIdsDistinct'].every(key=>staleObservation[key]===null||typeof staleObservation[key]==='boolean');
  return {stage,reason:reasons.has(error?.message)?error.message:'unclassified_recording_failure',
    code:Number.isSafeInteger(error?.actualCode)&&error.actualCode>=0?error.actualCode:null,cleanupFailures:[],
    ...(phases.has(error?.memberRejoinDiagnostic?.phase)?{memberRejoinPhase:error.memberRejoinDiagnostic.phase}:{}),
    ...(validRejoin?{memberRejoinObservation:{stage:rejoinObservation.stage,state:rejoinObservation.state,
      revisionAdvanced:rejoinObservation.revisionAdvanced,pausedReasonMatches:rejoinObservation.pausedReasonMatches,
      indicators:rejoinObservation.indicators}}:{}),
    ...(validStale?{staleConsentObservation:{stage:staleObservation.stage,outcome:staleObservation.outcome,
      code:staleObservation.code,ownGrants:staleObservation.ownGrants,newGrantFormat:staleObservation.newGrantFormat,
      newGrantRevisionOne:staleObservation.newGrantRevisionOne,newIdsDistinct:staleObservation.newIdsDistinct}}:{})};
}

// probe 只在原四端均已真实解码后运行；三份签署分别沿各自正式登录与原端点进入原主会话。
async function probe(context,config) {
  const {users,api,sourceSession,snapshotMedia,activeMembers}=context;
  assert.ok(users?.length===3&&new Set(users.map(user=>user.id)).size===3&&sourceSession,'three_real_participants_required');
  const recordingId=`conference-${randomUUID()}`,results=new Map();
  const projection=createRecordingStateProjection(users,sourceSession,recordingId);
  let removeObserver=null;
  let rejoinWait=null,rejoinPreviousRevision=0; // 只记录两段等待的固定阶段和原水位，不保存成员或会话关联。
  let staleObservation=null; // 旧授权拒绝/新本人授权/新ID验证分开观察，不因assert丢失原数字业务码。
  let state=null,stopped=false,started=false,stage='own_consent',primaryFailure=null;
  // receive 同一事实按各自真实端点读取，不从一个用户结果推断其他用户已获录制指示。
  async function receive(user,keepContent=false,signal=context.signal) {
    const inbox=await api(user,'im/interaction/events',{endpointId:user.endpoint,limit:32},true,'POST',signal);
    assert.ok(Array.isArray(inbox.list)&&inbox.list.length<=32,'bounded_record_events_required');
    const acknowledge=[];
    for(const delivery of inbox.list){
      const event=delivery.event;
      if(event?.sessionId===sourceSession){
        if(event.method==='interaction.result')results.set(`${user.id}:${event.msgToken}`,event);
        projection.observe(user,event);state=projection.read();
      }
      if(!(keepContent&&event?.data?.command==='recordingContent'))acknowledge.push(delivery.deliveryId);
    }
    if(acknowledge.length)await api(user,'im/interaction/events/ack',{endpointId:user.endpoint,deliveryIds:acknowledge},true,'POST',signal);
    assert.ok(results.size<=32,'bounded_record_observation_required');
    return inbox;
  }
  // command 只消费当前本人原token；202/accepted不是录制完成或其他成员同意的替代。
  async function command(user,selector,args,keepContent=false,signal=context.signal) {
    const token=randomUUID(),key=`${user.id}:${token}`;
    const accepted=await api(user,'im/interaction/commands',{endpointId:user.endpoint,command:{msgToken:token,method:'interaction.control',sessionId:sourceSession,params:{command:selector,arguments:args}}},true,'POST',signal);
    assert.ok(accepted.accepted===true&&accepted.msgToken===token,'original_record_acceptance_required');
    await waitFor(async()=>{await receive(user,keepContent,signal);return results.has(key);},20000,signal);
    const result=results.get(key);results.delete(key);
    if(result.code!==200){const error=Error('original_record_result_required');error.actualCode=Number.isSafeInteger(result.code)?result.code:null;throw error;}
    return result;
  }
  // allObserved 逐个本人读取并比较同一实例修订，禁止只看管理者收到状态便声称全员已通知。
  async function allObserved(expected,revision) {
    await waitFor(async()=>{
      for(const user of users)await receive(user);
      return projection.allObserved(expected,revision);
    },20000,context.signal);
  }
  const manager=users[0],proofs=[];
  try {
    if(typeof context.observeRecordingState==='function')removeObserver=context.observeRecordingState((user,event)=>{
      projection.observe(user,event);state=projection.read();
    });
    for(const user of users){
      const grant=await command(user,'recordingConsent',{operation:'grant',recordingId,policyId:config.policy,expiresAt:Date.now()+240000});
      assert.ok(/^[a-f0-9]{64}$/.test(grant.data?.consentId)&&grant.data.revision===1,'persistent_own_consent_required');
      assert.deepEqual(grant.data.sourceTrackIds,['conference-audio'],'full_conference_record_track_required');
      proofs.push(grant.data.consentId);
    }
    let before=await snapshotMedia(true);
    stage='full_mix_start';
    const active=await command(manager,'recordingControl',{operation:'start',recordingId,expectedRevision:0,sourceTrackIds:['conference-audio'],consentIds:proofs,policyId:config.policy});
    started=true;assert.equal(active.data.state,'active');
    await allObserved('active',active.data.revision);
    await new Promise(resolve=>setTimeout(resolve,1600));
    stage='explicit_pause';
    const paused=await command(manager,'recordingControl',{operation:'pause',recordingId,expectedRevision:active.data.revision});
    assert.equal(paused.data.state,'paused');await allObserved('paused',paused.data.revision);
    await new Promise(resolve=>setTimeout(resolve,400));
    stage='explicit_resume';
    let resumed=await command(manager,'recordingControl',{operation:'resume',recordingId,expectedRevision:paused.data.revision,consentIds:proofs});
    assert.equal(resumed.data.state,'active');await allObserved('active',resumed.data.revision);
    let memberRejoin;
    if(config.memberRejoin){
      stage='formal_member_leave_rejoin';
      assert.equal(typeof context.rejoinMember,'function','formal_member_rejoin_port_required');
      memberRejoin=await context.rejoinMember(users[2]);
      rejoinWait='awaitPaused';rejoinPreviousRevision=resumed.data.revision;
      await waitFor(async()=>{for(const user of users)await receive(user);return state?.state==='paused'&&state.reason==='recordingConsentMissing'&&state.revision>resumed.data.revision;},20000,context.signal);
      rejoinWait='allIndicators';
      await allObserved('paused',state.revision);
      rejoinWait=null;
      stage='stale_join_epoch_consent_rejected';
      staleObservation={stage:'rejectOld',outcome:'notObserved',code:null,ownGrants:0,
        newGrantFormat:null,newGrantRevisionOne:null,newIdsDistinct:null};
      let denied=false;
      try{await command(manager,'recordingControl',{operation:'resume',recordingId,expectedRevision:state.revision,consentIds:proofs});
        staleObservation.outcome='accepted';staleObservation.code=200;}
      catch(error){staleObservation.outcome='rejected';staleObservation.code=Number.isSafeInteger(error.actualCode)&&error.actualCode>=0?error.actualCode:null;
        assert.equal(error.actualCode,409,'stale_original_consent_must_conflict');denied=true;}
      assert.equal(denied,true,'old_member_consent_reused');
      const nextProofs=[];
      staleObservation.stage='newOwnGrants';
      for(const user of users){
        staleObservation.outcome='notObserved';staleObservation.code=null;staleObservation.newGrantFormat=null;staleObservation.newGrantRevisionOne=null;
        let granted;
        try{granted=await command(user,'recordingConsent',{operation:'grant',recordingId,policyId:config.policy,expiresAt:Date.now()+240000});}
        catch(error){staleObservation.outcome='rejected';staleObservation.code=Number.isSafeInteger(error.actualCode)&&error.actualCode>=0?error.actualCode:null;throw error;}
        staleObservation.outcome='accepted';staleObservation.code=200;
        staleObservation.newGrantFormat=/^[a-f0-9]{64}$/.test(granted.data?.consentId);
        staleObservation.newGrantRevisionOne=granted.data?.revision===1;
        assert.ok(staleObservation.newGrantFormat&&staleObservation.newGrantRevisionOne);
        nextProofs.push(granted.data.consentId);++staleObservation.ownGrants;
      }
      staleObservation.stage='newIds';staleObservation.outcome='notObserved';staleObservation.code=null;
      staleObservation.newIdsDistinct=nextProofs.every(id=>!proofs.includes(id));
      assert.ok(staleObservation.newIdsDistinct,'old_member集合_consent_must_not_migrate');
      proofs.splice(0,proofs.length,...nextProofs);
      stage='explicit_resume_after_new_own_consents';
      staleObservation=null;
      resumed=await command(manager,'recordingControl',{operation:'resume',recordingId,expectedRevision:state.revision,consentIds:proofs});
      assert.equal(resumed.data.state,'active');await allObserved('active',resumed.data.revision);
      await context.phase('rejoined_actual_four_sources',[0,1,2,3],[0,1,2,3]);
      before=await snapshotMedia(true);
      memberRejoin={...memberRejoin,automaticPause:true,staleConsentRejected:true,allOwnConsentRenewed:true,explicitResume:true};
    }
    await new Promise(resolve=>setTimeout(resolve,1600));
    stage='real_finalize';
    const finalizing=await command(manager,'recordingControl',{operation:'stop',recordingId,expectedRevision:resumed.data.revision});
    assert.equal(finalizing.data.state,'finalizing');stopped=true;
    await waitFor(async()=>{for(const user of users)await receive(user);return state?.state==='ready'||state?.state==='failed';},65000,context.signal);
    assert.equal(state.state,'ready','actual_conference_artifact_ready_required');await allObserved('ready',state.revision);
    stage='private_content';
    const contentEvent=await command(manager,'recordingContent',{recordingId},true),content=contentMetadata(contentEvent,recordingId,Date.now());
    const downloadSignal=context.signal?AbortSignal.any([context.signal,AbortSignal.timeout(15000)]):AbortSignal.timeout(15000);
    const response=await fetch(content.signedURL,{redirect:'error',signal:downloadSignal});
    assert.equal(response.status,200,'actual_private_artifact_required');
    const chunks=[];let size=0;
    for await(const chunk of response.body){size+=chunk.length;assert.ok(size<=content.size,'artifact_size_budget');chunks.push(chunk);}
    const bytes=Buffer.concat(chunks);assert.equal(bytes.length,content.size);assert.equal(createHash('sha256').update(bytes).digest('hex'),content.sha256);
    const root=path.resolve('.temp/task143-remediation-20261008');fs.mkdirSync(root,{recursive:true});
    assert.ok(fs.realpathSync(root).startsWith(`${fs.realpathSync('.temp')}${path.sep}`),'record_evidence_must_stay_in_task_temp');
    const artifact=path.join(root,`cloud-conference-record-${randomUUID()}.mkv`);fs.writeFileSync(artifact,bytes,{mode:0o600,flag:'wx'});
    stage='actual_four_frequency_decode';
    const audio=await promisify(execFile)('ffmpeg',['-v','error','-nostdin','-i',artifact,'-map','0:a:0','-t','10','-f','s16le','-ac','1','-ar','48000','pipe:1'],{encoding:'buffer',timeout:15000,maxBuffer:2<<20,signal:context.signal});
    const decoded=recordedMix(audio.stdout);
    const wav=artifact.replace(/\.mkv$/,'.wav');fs.closeSync(fs.openSync(wav,'wx',0o600));
    await promisify(execFile)('ffmpeg',['-v','error','-nostdin','-y','-i',artifact,'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_s16le',wav],{encoding:'buffer',timeout:15000,maxBuffer:1<<20,signal:context.signal});
    stage='own_consent_revoke';
    await command(users[2],'recordingConsent',{operation:'revoke',recordingId,consentId:proofs[2],expectedRevision:1});
    const reread=await receive(manager,true),denied=reread.list.find(item=>item.event?.msgToken===contentEvent.msgToken)?.event;
    assert.ok(denied?.code===403&&denied.data?.state==='failed'&&denied.data.reason==='permissionDenied'&&!Object.hasOwn(denied.data,'signedURL')&&!Object.hasOwn(denied.data,'contentRef'),'withdrawn_conference_content_must_fail');
    if(reread.list.length)await api(manager,'im/interaction/events/ack',{endpointId:manager.endpoint,deliveryIds:reread.list.map(item=>item.deliveryId)});
    const retired=await retiredPrivateURL(content);
    stage='original_four_peers_continue';
    await activeMembers(users.map(user=>user.id));
    const after=await snapshotMedia(false);
    assert.equal(after.length,4);assert.equal(before.length,4);
    assert.ok(after.every((value,index)=>value.samePeer&&value.connected&&value.received>before[index].received&&value.sent>before[index].sent&&value.samples>before[index].samples),'recording_must_preserve_all_original_media');
    return {actualConsentingUsers:3,allParticipantIndicators:true,fullMixDecoded:true,pauseResume:true,artifactBytes:size,sha256:content.sha256,durationMs:content.durationMs,decodedPCM:decoded,revokedReadDenied:true,retiredPrivateURL:retired,representativeWAV:true,mainMediaContinues:true,...(memberRejoin?{memberRejoin}:{})};
  } catch(error) {
    primaryFailure=error;
    const actual=projection.read();
    const rejoinObservation=rejoinWait?{stage:rejoinWait,state:actual?.state??'missing',
      revisionAdvanced:(actual?.revision??0)>rejoinPreviousRevision,
      pausedReasonMatches:actual?.state==='paused'&&actual.reason==='recordingConsentMissing',
      indicators:actual?projection.indicators('paused',actual.revision):0}:null;
    Object.defineProperty(error,'recordingDiagnostic',{value:recordingFailureDiagnostic(error,stage,rejoinObservation,staleObservation),enumerable:false});
    throw error;
  } finally {
    // 已启动但未正式stop时先显式停副本；失败仍由外层原会议finally关闭原Owner，绝不遗留后台Promise。
    try {
      if(started&&!stopped){
        const cleanup=AbortSignal.timeout(5000);
        await receive(manager,false,cleanup);
        if(state&&['active','paused'].includes(state.state))await command(manager,'recordingControl',{operation:'stop',recordingId,expectedRevision:state.revision},false,cleanup);
      }
    } catch(error) {
      if(!primaryFailure)throw error;
      primaryFailure.recordingDiagnostic.cleanupFailures.push('recording_stop_unconfirmed');
    } finally {
      if(removeObserver)removeObserver(); // 原扩展完成或失败均退订，不让后续旧会议动作继续进入录制观察。
      if(primaryFailure)console.log(JSON.stringify({result:'fail',scenario:'S15-cloud-conference-recording',stage,
        reason:'recording_probe_failed',recordingDiagnostic:primaryFailure.recordingDiagnostic}));
    }
  }
}

// run 使用原驱动完整的端点/会议/浏览器/设备清理，只有原流程与录制副本均通过才返回pass。
async function run() {
  const config=configuration(process.env);let recording;
  const outcome=await conference.run({onEstablished:async context=>{recording=await probe(context,config);return recording.memberRejoin?{rejoinedMember:true}:undefined;}});
  assert.ok(recording?.fullMixDecoded,'recording_hook_not_executed');
  if(outcome.result==='pass')console.log(JSON.stringify({result:'pass',scenario:'S15-cloud-conference-recording',recording}));
  return {...outcome,recording};
}

module.exports={configuration,recordedMix,createRecordingStateProjection,recordingFailureDiagnostic,probe,run};
if(require.main===module)run().catch(()=>{console.log(JSON.stringify({result:'fail',scenario:'S15-cloud-conference-recording',reason:'cloud_conference_record_failed'}));process.exitCode=1;});
