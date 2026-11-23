// 文件说明：C++单设备与三个真实登录用户的正式IM/HTTP/MQTT会议验收；PCM频谱来自实际解码，不注入媒体事实。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {browser, evaluated, confirmAdmission, attachListeningAudio} = require('./ipc_live.cjs');

// FREQUENCIES 固定四个可区分有声夹具：设备和U1/U2/U3，不把同一音调当成自身排除证据。
const FREQUENCIES = Object.freeze([400, 700, 1000, 1300]);

// safeUserID 只接纳正式self返回的安全正整数，不容许测试参数自报用户或精度丢失。
function safeUserID(value) {
  const text = typeof value === 'number' && Number.isSafeInteger(value) ? String(value) : value;
  assert.ok(typeof text === 'string' && /^[1-9][0-9]*$/.test(text) && BigInt(text) <= BigInt(Number.MAX_SAFE_INTEGER), 'safe_real_user_required');
  return text;
}

// authenticatedUser 要求正式self同时回读当前企业的启用成员关系，不能仅凭两个不同token声称同一会议身份。
function authenticatedUser(current, tenant) {
  const id=safeUserID(current?.userID);
  assert.ok(Array.isArray(current.tenants)&&current.tenants.length<=1024,'actual_enterprise_membership_required');
  const matches=current.tenants.filter(item=>item.tenantCode===tenant&&safeUserID(item.userID)===id&&item.status===1);
  assert.equal(matches.length,1,'actual_enterprise_membership_required');return id;
}

// readBoundedJSON 逐流有界读取真实API响应，重定向不转发私有凭据，超限立即取消原读流。
async function readBoundedJSON(response) {
  assert.ok(response.status<300||response.status>=400,'api_redirect_forbidden');
  const announced=response.headers.get('content-length');
  assert.ok(announced===null||(/^\d+$/.test(announced)&&Number(announced)<=1048576),'api_response_too_large');
  assert.ok(response.body&&typeof response.body.getReader==='function','api_response_stream_required');
  const reader=response.body.getReader(),chunks=[];let size=0;
  try {
    for(;;){const {done,value}=await reader.read();if(done)break;size+=value.byteLength;assert.ok(size<=1048576,'api_response_too_large');chunks.push(Buffer.from(value));}
    return JSON.parse(Buffer.concat(chunks,size).toString('utf8'));
  } catch(error) {await reader.cancel().catch(()=>{});throw error;}finally {reader.releaseLock();}
}

// formalFailureDiagnostic 只暴露固定数字状态，不能将后端正文、令牌、成员身份或URL带入报告。
function formalFailureDiagnostic(error) {
  return {httpStatus:Number.isInteger(error?.httpStatus)&&error.httpStatus>=100&&error.httpStatus<=599?error.httpStatus:null,
    code:Number.isSafeInteger(error?.actualCode)&&error.actualCode>=0?error.actualCode:null};
}

// invokeEstablishedHook 只供维护驱动注入已建立阶段的扩展；截止仅发取消，必须等待同一回调和其finally实际退出。
// 扩展须把signal传给HTTP/轮询并用独立有限清理信号收尾；不以Promise.race遗留后台提交。
async function invokeEstablishedHook(hook, context, timeout=90000) {
  if(hook===undefined)return undefined;
  assert.equal(typeof hook,'function','established_hook_required');
  assert.ok(Number.isInteger(timeout)&&timeout>0&&timeout<=90000,'established_hook_budget_invalid');
  const controller=new AbortController();
  const timer=setTimeout(()=>controller.abort(Error('established_hook_timeout')),timeout);
  try{
    const scoped={...context,signal:controller.signal}; // 仅扩展闭包使用可取消端口，不改变原会议心跳或最终清理。
    if(typeof context.api==='function')scoped.api=(user,path,body={},success=true,method='POST',signal=controller.signal)=>context.api(user,path,body,success,method,signal);
    if(typeof context.command==='function')scoped.command=(user,method,params,token,signal=controller.signal)=>context.command(user,method,params,token,signal);
    if(typeof context.pump==='function')scoped.pump=(user,signal=controller.signal)=>context.pump(user,signal);
    if(typeof context.rejoinMember==='function')scoped.rejoinMember=(user,signal=controller.signal)=>context.rejoinMember(user,signal);
    const result=await hook(scoped);
    controller.signal.throwIfAborted();return result;
  }finally{clearTimeout(timer);}
}

// createRecordingEventRelay 仅为显式录制扩展保留原pump确认前的同步观察端口，不重发或缓存控制报文。
function createRecordingEventRelay() {
  let observer=null;
  return {
    subscribe(listener) {
      assert.equal(typeof listener,'function','recording_observer_required');
      assert.equal(observer,null,'recording_observer_already_registered');observer=listener;
      let removed=false;
      return ()=>{if(!removed){removed=true;if(observer===listener)observer=null;}};
    },
    observe(user,event) {
      if(observer && event?.method==='interaction.controlState' && event.data?.command==='recordingState')
        observer(user,event);
    }
  };
}

// rejoinConferenceMember 只供显式扩展验证真实离会/重入；旧媒体必须实际释放，新JoinedAt及媒体腿不得复用。
async function rejoinConferenceMember(user,ports,signal) {
  let phase='originalAccess'; // 固定本地阶段只用于失败定位，不包含成员、媒体票据或API正文。
  try {
  assert.ok(user&&user.index>1&&user.index<=3&&!user.ended,'non_manager_actual_member_required');
  signal?.throwIfAborted();
  const originalSession=user.session,original=await ports.access(user,signal);
  assert.ok(originalSession&&Number.isSafeInteger(original.memberEpoch)&&original.memberEpoch>0,'original_member_epoch_required');
  phase='endCommand';
  await ports.end(user,signal);
  phase='waitTerminal';
  await ports.terminal(user,signal);assert.equal(user.state,'ended','old_actual_terminal_required');
  user.ended=true;
  phase='closeMedia';
  const released=await ports.close(user,signal);
  assert.ok(released?.peerClosed&&released.captureStopped&&released.remoteStopped,'old_actual_media_release_required');
  phase='groupLeave';
  await ports.leave(user,signal);
  phase='remainingMembers';
  await ports.remaining(user,signal);
  phase='groupJoin';
  await ports.join(user,signal);
  phase='renewedAccess';
  const current=await ports.access(user,signal);
  assert.ok(Number.isSafeInteger(current.memberEpoch)&&current.memberEpoch>original.memberEpoch,'fresh_joined_epoch_required');
  user.session=undefined;user.state=undefined;user.stateTimeline=[];user.events=[];
  phase='newSession';
  await ports.open(user,signal);
  assert.ok(user.session&&user.session!==originalSession,'fresh_media_session_required');
  phase='newMedia';
  await ports.negotiate(user,signal);
  assert.equal(user.state,'established','fresh_actual_established_required');user.ended=false;
  phase='activeMembers';
  await ports.members(user,signal);
  return {oldMediaReleased:true,originalMemberEpoch:original.memberEpoch,currentMemberEpoch:current.memberEpoch,newSession:true};
  } catch(error) {
    Object.defineProperty(error,'memberRejoinDiagnostic',{value:{phase},enumerable:false});
    throw error; // 保留原失败对象及错误码，不让粗粒度录制阶段覆盖真实离会/加入/媒体失败。
  }
}

// validateEstablishedReplacement 只核对扩展交接元数据；实际commit、旧端HAL停止及新端解码仍由扩展独立取证。
// 仅原控制者U1或额外U2可替换，不改变原身份/夹具/顺序；校验无网络和媒体副作用。
function validateEstablishedReplacement(original, replacement, startedBrowsers) {
  assert.equal(original?.ended,true,'replacement_original_not_ended');assert.equal(original?.state,'ended','replacement_original_terminal_required');
  verifySingleEndNotice(original.stateTimeline);
  assert.ok(replacement&&[1,2].includes(original.index)&&replacement.index===original.index&&replacement.id===original.id&&replacement.token===original.token&&replacement.env===original.env,'replacement_original_identity_required');
  for(const field of ['name','endpoint','session'])assert.ok(typeof replacement[field]==='string'&&replacement[field].length>0&&replacement[field].length<=128&&!/[\x00-\x1f\x7f]/.test(replacement[field])&&replacement[field]!==original[field],'replacement_new_binding_required');
  assert.ok(startedBrowsers.has(replacement.name)&&replacement.state==='established'&&replacement.ended===false,'replacement_actual_established_required');
  assert.ok(Array.isArray(replacement.events)&&replacement.events.length<=128&&Array.isArray(replacement.stateTimeline)&&replacement.stateTimeline.length>0&&replacement.stateTimeline.length<=128&&replacement.stateTimeline.at(-1)==='established'&&!replacement.stateTimeline.includes('ended'),'replacement_original_buffers_required');
  return true;
}

// bounded 为本任务清理提供独立有限截止，原失败不被计时器吞掉。
async function bounded(promise, milliseconds, reason) {
  let timer;try{return await Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error(reason)),milliseconds);})]);}
  finally {clearTimeout(timer);}
}

// stopChild 仅终止本次spawn的精确进程，TERM四秒后KILL两秒；未实际close不得称资源已清理。
async function stopChild(child, completion, isExited, termBudget=4000, killBudget=2000) {
  if(!child||!completion)return;if(isExited()){await completion;return;}
  child.kill('SIGTERM');
  try {await bounded(completion,termBudget,'device_term_timeout');return;}catch(error){if(error.message!=='device_term_timeout')throw error;}
  if(!isExited())child.kill('SIGKILL');await bounded(completion,killBudget,'device_kill_timeout');assert.equal(isExited(),true,'device_not_closed');
}

// validateConfiguration 任何资源启动前检查显式开关、同企业应用、三真实凭据及四份独立WAV路径。
function validateConfiguration(env, stat = fs.statSync) {
  assert.equal(env.UR_CONFERENCE_MEMBER_LIVE, '1', 'explicit_conference_live_required');
  assert.equal(env.UR_CONFERENCE_FIXED_PCM_TEST, '1', 'explicit_fixed_pcm_software_test_required');
  const base = new URL(env.UR_IPC_BASE_URL);
  assert.ok(base.protocol === 'http:' && base.hostname === '127.0.0.1' && !base.username && !base.password &&
    base.pathname === '/' && !base.search && !base.hash, 'loopback_backend_required');
  const port = Number(env.UR_IPC_DEVICE_PORT);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535, 'device_port_required');
  assert.equal(env.UR_IPC_CONFERENCE_TALK, '1'); assert.equal(env.UR_IPC_MICROPHONE, '1');
  assert.ok(env.UR_IPC_PARTICIPANT_ROSTER===undefined||env.UR_IPC_PARTICIPANT_ROSTER==='0'||env.UR_IPC_PARTICIPANT_ROSTER==='1','explicit_roster_mode_invalid');
  assert.ok(env.UR_IPC_PARTICIPANT_ROSTER_SYNC===undefined||env.UR_IPC_PARTICIPANT_ROSTER_SYNC==='0'||env.UR_IPC_PARTICIPANT_ROSTER_SYNC==='1','explicit_roster_sync_mode_invalid');
  assert.ok(env.UR_IPC_PARTICIPANT_ROSTER_SYNC!=='1'||env.UR_IPC_PARTICIPANT_ROSTER==='1','snapshot_requires_roster_capability');
  assert.ok(env.UR_CONFERENCE_HOST_REMOVE===undefined||env.UR_CONFERENCE_HOST_REMOVE==='0'||env.UR_CONFERENCE_HOST_REMOVE==='1','explicit_host_remove_mode_invalid');
  assert.ok(env.UR_CONFERENCE_HOST_REMOVE!=='1'||env.UR_IPC_PARTICIPANT_ROSTER==='1','host_remove_requires_roster_observation');
  assert.ok(env.UR_CONFERENCE_ROSTER_RECONCILE===undefined||env.UR_CONFERENCE_ROSTER_RECONCILE==='0'||env.UR_CONFERENCE_ROSTER_RECONCILE==='1','explicit_roster_reconcile_mode_invalid');
  assert.ok(env.UR_CONFERENCE_ROSTER_RECONCILE!=='1'||(env.UR_CONFERENCE_HOST_REMOVE==='1'&&env.UR_IPC_PARTICIPANT_ROSTER==='1'&&env.UR_IPC_PARTICIPANT_ROSTER_SYNC==='1'),'roster_reconcile_requires_host_remove_and_sync');
  assert.equal(env.UR_IPC_CAMERA_ID,env.UR_CONFERENCE_CAMERA_ID,'declared_camera_binding_required');
  for (const key of ['UR_DEVICE_PRODUCT','UR_DEVICE_NAME','UR_DEVICE_SECRET','UR_IPC_TENANT','UR_IPC_APP_ID','UR_CONFERENCE_CAMERA_ID'])
    assert.ok(typeof env[key] === 'string' && env[key].length && !/[\x00-\x1f\x7f]/.test(env[key]), 'injected_configuration_required');
  const tokens = ['UR_IPC_APP_TOKEN','UR_CONFERENCE_U2_TOKEN','UR_CONFERENCE_U3_TOKEN'].map(key => env[key]);
  assert.ok(tokens.every(token => typeof token === 'string' && token.length) && new Set(tokens).size === 3, 'three_distinct_credentials_required');
  const wavs = ['UR_IPC_AUDIO_FIXTURE','UR_CONFERENCE_U1_WAV','UR_CONFERENCE_U2_WAV','UR_CONFERENCE_U3_WAV'].map(key => env[key]);
  assert.ok(new Set(wavs).size === 4, 'four_distinct_audio_fixtures_required');
  for (const path of [env.UR_IPC_DEVICE_BIN,...wavs]) assert.ok(path && stat(path).isFile(), 'binary_and_audio_required');
  return {port,tokens,wavs,rosterEnabled:env.UR_IPC_PARTICIPANT_ROSTER==='1',rosterSyncEnabled:env.UR_IPC_PARTICIPANT_ROSTER_SYNC==='1',hostRemoveEnabled:env.UR_CONFERENCE_HOST_REMOVE==='1',rosterReconcileEnabled:env.UR_CONFERENCE_ROSTER_RECONCILE==='1'};
}

// fixtureArguments 生成固定60秒48k单声道PCM夹具的精确ffmpeg参数；只返回命令参数，由中央资源入口执行。
function fixtureArguments(index, output) {
  assert.ok(Number.isInteger(index) && index >= 0 && index < FREQUENCIES.length && typeof output === 'string' && output.length);
  return ['-hide_banner','-loglevel','error','-nostdin','-y','-f','lavfi','-i',`sine=frequency=${FREQUENCIES[index]}:sample_rate=48000:duration=60`,
    '-ac','1','-c:a','pcm_s16le',output];
}

// spectralPeaks 只对有限实际PCM计算四音调幅度，窗函数抑制其他源泄漏；不接受RTP计数代替采样。
function spectralPeaks(samples, sampleRate, frequencies) {
  const out = [];
  for (const frequency of frequencies) {
    let real = 0, imaginary = 0, weight = 0;
    for (let i = 0; i < samples.length; i++) {
      const window = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / Math.max(1, samples.length-1));
      const phase = 2 * Math.PI * frequency * i / sampleRate;
      real += samples[i] * window * Math.cos(phase); imaginary -= samples[i] * window * Math.sin(phase); weight += window;
    }
    out.push(weight ? 2 * Math.hypot(real,imaginary) / weight : 0);
  }
  return out;
}

// verifyMix 要求每个应接收音源确实出现，禁止音源低于绝对与相对阈值；不能用静音全流满足自身排除。
function verifyMix(facts, own, allowed) {
  assert.ok(facts && Number.isSafeInteger(facts.observations) && facts.observations >= 4 && facts.peaks.length === 4, 'decoded_pcm_required');
  const required = allowed.filter(index => index !== own);
  assert.ok(required.length > 0 && required.every(index => Number.isFinite(facts.peaks[index]) && facts.peaks[index] > 0.008), 'expected_sources_not_decoded');
  const limit = Math.max(0.003, Math.min(...required.map(index => facts.peaks[index])) * 0.2);
  for (let index = 0; index < 4; index++)
    if (index === own || !allowed.includes(index)) assert.ok(Number.isFinite(facts.peaks[index]) && facts.peaks[index] < limit, 'forbidden_source_present');
  return true;
}

// verifyFixedPCMSettings 核对真实单麦克风的三项处理均关闭，不接受请求约束或缺失字段代替事实。
function verifyFixedPCMSettings(settings) {
  assert.ok(Array.isArray(settings) && settings.length === 1, 'actual_fixed_pcm_capture_required');
  for (const name of ['echoCancellation','noiseSuppression','autoGainControl'])
    assert.equal(settings[0][name],false,'actual_fixed_pcm_processing_mismatch');
  return true;
}

// sourcePageReady 仅核验固定页面功能类型与本地驱动选项，不记录地址、正文或桥认证信息。
function sourcePageReady(value) {
  const keys=['incoming','available','activate','softwareFixedPCM'];
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'source_page_probe_invalid');
  assert.deepEqual(Object.keys(value).sort(),keys.slice().sort(),'source_page_probe_invalid');
  for(const key of keys)assert.equal(typeof value[key],'boolean','source_page_probe_invalid');
  return keys.every(key=>value[key]);
}

// installRosterObserver 显式启用时观察原网页消费者的提交；不截MQTT、不改报文、媒体或消费结果。
// 身份只留浏览器闭包，对外仅布尔、计数和实际水位；预算耗尽不意味着没有事件。
// 除最新投影外按腿保留有界逐条历史供rosterTransitions断言，并统计ipcTrack下行与
// ipcTrackApplied上行计数供轨契约负判据取证；只计数种类，不记录正文或令牌。
function installRosterObserver(enabled) {
  if(enabled!==true)return false;
  const accepted=new Map(),snapshots=new Map(),wireCounts=new Map();
  let observations=0,truncated=false,snapshotObservations=0,snapshotTruncated=false;
  const original=incoming,originalSend=send;
  // 原生产不向网页下发成员轨控制，send包装只为取证"没有任何trackState回执上行"这一契约边界。
  send=value=>{
    if(value&&typeof value==='object'&&value.kind==='ipcTrackApplied'&&typeof value.sessionId==='string'){
      const counter=wireCounts.get(value.sessionId)??{ipcTrackReceived:0,trackStateSent:0};
      counter.trackStateSent++;wireCounts.set(value.sessionId,counter);
    }
    return originalSend(value);
  };
  // recordHistory 追加逐条投影；历史满时保留最新并标记截断，read仍必须取真实最新投影。
  const recordHistory=(session,entry)=>{
    let record=accepted.get(session);
    if(!record){
      if(accepted.size>=4){truncated=true;return;}
      record={records:[],historyTruncated:false};accepted.set(session,record);
    }
    if(record.records.length>=64){record.records[63]=entry;record.historyTruncated=true;return;}
    record.records.push(entry);
  };
  const wireCounter=session=>{
    const counter=wireCounts.get(session)??{ipcTrackReceived:0,trackStateSent:0};
    wireCounts.set(session,counter);return counter;
  };
  incoming=async value=>{
    if(value&&typeof value==='object'&&value.kind==='ipcTrack'&&typeof value.sessionId==='string')
      wireCounter(value.sessionId).ipcTrackReceived++;
    const candidate=value?.kind==='ipcParticipants'&&typeof value.sessionId==='string';
    const before=candidate?views.get(value.sessionId)?.rosterSequence??0:0;
    const query=value?.kind==='ipcSyncState'&&typeof value.sessionId==='string';
    const result=await original(value); // 原消费者只执行一次，原失败不被旁路吞掉。
    if(query){
      const view=views.get(value.sessionId);
      const previous=snapshots.get(value.sessionId); // 点击可能先将UI置pending，去重只用原CPP状态观察账本。
      if(!view||view.closed||!view.talk||!view.active||view.human||
        Object.keys(value).length!==4||!Number.isSafeInteger(value.eventSeq)||value.eventSeq<0||
        view.syncState!==value.state||view.syncSequence!==value.eventSeq||
        (previous?.state===value.state&&previous?.sequence===value.eventSeq))return result;
      if(snapshotObservations>=128||(!snapshots.has(value.sessionId)&&snapshots.size>=4)){snapshotTruncated=true;return result;}
      snapshotObservations++;
      snapshots.set(value.sessionId,{state:value.state,sequence:value.eventSeq,
        // 只有当前pending可传到成功；失败或ready清除旧受理，迟到成功不能借上一尝试。
        pendingObserved:value.state==='pending'||(value.state==='succeeded'&&previous?.state==='pending'&&previous.pendingObserved)});
      return result;
    }
    if(!candidate)return result;
    const view=views.get(value.sessionId);
    if(!view||view.closed||!view.talk||!view.active||!Number.isSafeInteger(value.eventSeq)||
      value.eventSeq<=before||view.rosterSequence!==value.eventSeq||!Array.isArray(view.participants))return result;
    if(observations>=128||(!accepted.has(value.sessionId)&&accepted.size>=4)){truncated=true;return result;}
    const members=view.participants;
    if(members.length>32||members.some(member=>typeof member.participantId!=='string'||
      !Array.isArray(member.tracks)||member.tracks.length>8))return result;
    observations++;
    recordHistory(value.sessionId,{sequence:view.rosterSequence,identities:members.map(member=>member.participantId),
      tracks:members.reduce((count,member)=>count+member.tracks.length,0)});
    return result;
  };
  window.__conferenceRosterObservation={
    // read 对比原实际用户集合，不把入站字段或旧同水位重放当新消费，不反射原身份。
    read(session,expected){
      const view=views.get(session),fact=accepted.get(session);
      if(!view||view.closed)accepted.delete(session); // 原UI释放后仅清观察缓存，不保留旧成员作为终态名册。
      const latest=Array.isArray(fact?.records)?fact.records[fact.records.length-1]:null;
      const available=!!view&&!view.closed&&view.talk===true&&view.active===true;
      const applied=available&&!!latest&&view.rosterSequence===latest.sequence;
      const actual=applied?latest.identities.slice().sort():[],wanted=Array.isArray(expected)?expected.slice().sort():[];
      const exact=applied&&Array.isArray(expected)&&expected.length<=32&&new Set(expected).size===expected.length&&
        actual.length===wanted.length&&actual.every((value,index)=>value===wanted[index]);
      return {enabled:true,available,consumerApplied:!!applied,eventSeq:applied?latest.sequence:null,
        memberCount:applied?latest.identities.length:0,expectedCount:wanted.length,exactMembers:!!exact,
        trackCount:applied?latest.tracks:0,truncated:truncated===true||fact?.historyTruncated===true,closed:!view||view.closed===true};
    },
    // transitions 返回逐条投影历史；腿删除后清空，旧历史不能当终态名册。
    transitions(session){
      const view=views.get(session),fact=accepted.get(session);
      if(!view||view.closed){accepted.delete(session);return {records:[],truncated:false,closed:true};}
      return {records:fact?fact.records.map(entry=>({eventSeq:entry.sequence,identities:entry.identities.slice(),trackCount:entry.tracks})):[],
        truncated:fact?.historyTruncated===true,closed:false};
    },
    // wire 按腿统计轨控制下行与回执上行计数；实现轨契约边界的负判据取证。
    wire(session){
      const view=views.get(session),counter=wireCounter(session),fact=accepted.get(session);
      if(!view||view.closed)accepted.delete(session);
      return {ipcTrackReceived:counter.ipcTrackReceived,trackStateSent:counter.trackStateSent,
        rosterTrackEntries:fact?fact.records.reduce((sum,entry)=>sum+entry.tracks,0):0,
        truncated:fact?.historyTruncated===true,
        available:!!view&&!view.closed&&view.talk===true&&view.active===true,closed:!view||view.closed===true};
    },
    // readSnapshot 只报告原CPP查询状态实际被UI安装的事实，不把增量事件或点击当完整查询成功。
    readSnapshot(session){
      const view=views.get(session),fact=snapshots.get(session);
      if(!view||view.closed)snapshots.delete(session);
      const available=!!view&&!view.closed&&view.talk===true&&view.active===true&&!view.human;
      const applied=available&&!!fact&&view.syncState===fact.state&&view.syncSequence===fact.sequence;
      return {enabled:true,available,consumerApplied:!!applied,state:applied?fact.state:'missing',
        pendingObserved:applied&&fact.pendingObserved,eventSeq:applied?fact.sequence:null,
        closed:!view||view.closed===true,truncated:snapshotTruncated};
    },
  };
  return true;
}

// verifyRosterObservation 只认可原消费者的新水位、精确原成员与当前空轨子集；不授权媒体或会议话权。
function verifyRosterObservation(value,expectedCount,afterSequence=0){
  const keys=['enabled','available','consumerApplied','eventSeq','memberCount','expectedCount','exactMembers','trackCount','truncated','closed'];
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'cpp_roster_observation_required');
  assert.deepEqual(Object.keys(value).sort(),keys.slice().sort(),'cpp_roster_observation_invalid');
  for(const name of ['enabled','available','consumerApplied','exactMembers','truncated','closed'])assert.equal(typeof value[name],'boolean','cpp_roster_observation_invalid');
  for(const name of ['memberCount','expectedCount','trackCount'])assert.ok(Number.isSafeInteger(value[name])&&value[name]>=0&&value[name]<=(name==='trackCount'?256:32),'cpp_roster_observation_invalid');
  assert.ok(Number.isSafeInteger(expectedCount)&&expectedCount>=0&&expectedCount<=32&&Number.isSafeInteger(afterSequence)&&afterSequence>=0,'cpp_roster_expectation_invalid');
  assert.ok(Number.isSafeInteger(value.eventSeq)&&value.eventSeq>afterSequence,'cpp_roster_new_consumption_required');
  assert.ok(value.enabled&&value.available&&value.consumerApplied&&value.exactMembers&&!value.truncated&&!value.closed,'cpp_roster_consumption_not_confirmed');
  assert.equal(value.memberCount,expectedCount,'cpp_roster_member_mismatch');assert.equal(value.expectedCount,expectedCount,'cpp_roster_member_mismatch');
  assert.equal(value.trackCount,0,'current_roster_track_subset_required');return true;
}

// verifySnapshotObservation 同腿原pending到成功、新名册水位和实际成员全部成立；snapshotOnly不等于恢复媒体授权。
function verifySnapshotObservation(value,roster,expectedCount,afterSequence){
  const keys=['enabled','available','consumerApplied','state','pendingObserved','eventSeq','closed','truncated'];
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'cpp_snapshot_observation_required');
  assert.deepEqual(Object.keys(value).sort(),keys.slice().sort(),'cpp_snapshot_observation_invalid');
  for(const name of keys.filter(key=>key!=='state'&&key!=='eventSeq'))assert.equal(typeof value[name],'boolean','cpp_snapshot_observation_invalid');
  assert.ok(value.enabled&&value.available&&value.consumerApplied&&value.pendingObserved&&!value.closed&&!value.truncated,'cpp_snapshot_not_consumed');
  assert.equal(value.state,'succeeded','cpp_snapshot_not_succeeded');
  verifyRosterObservation(roster,expectedCount,afterSequence);
  assert.equal(value.eventSeq,roster.eventSeq,'cpp_snapshot_roster_watermark_mismatch');return true;
}

// verifySnapshotContinuity 核对原活Peer实际解码持续；本地静音端上行可不增长，但不得回退或重建Peer。
function verifySnapshotContinuity(before,after,expectedCount=4,quietIndices=[]){
  assert.ok(Number.isSafeInteger(expectedCount)&&expectedCount>=2&&expectedCount<=4,'actual_snapshot_peer_count_invalid');
  assert.ok(Array.isArray(quietIndices)&&new Set(quietIndices).size===quietIndices.length&&quietIndices.every(index=>Number.isSafeInteger(index)&&index>=0&&index<expectedCount),'snapshot_quiet_endpoint_invalid');
  for(const rows of [before,after]){assert.ok(Array.isArray(rows)&&rows.length===expectedCount,'four_actual_snapshot_peers_required');for(const row of rows){assert.deepEqual(Object.keys(row).sort(),['connected','received','samePeer','samples','sent']);assert.equal(row.connected,true);assert.equal(row.samePeer,true);for(const key of ['received','sent','samples'])assert.ok(Number.isSafeInteger(row[key])&&row[key]>0,'actual_snapshot_media_count_required');}}
  for(let index=0;index<expectedCount;index++)for(const key of ['received','sent','samples'])assert.ok(key==='sent'&&quietIndices.includes(index)?after[index][key]>=before[index][key]:after[index][key]>before[index][key],'snapshot_media_not_continuing');return true;
}

// verifyRosterReconcileAuthority 权威同步只读原活成员的加入代次及MAC事实，不能借快照重加成员或解禁。
function verifyRosterReconcileAuthority(before,after){
  for(const rows of [before,after]){
    assert.ok(Array.isArray(rows)&&rows.length>=2&&rows.length<=3,'actual_roster_authority_required');
    for(const row of rows){
      assert.ok(row&&typeof row==='object'&&!Array.isArray(row),'actual_roster_authority_invalid');
      assert.deepEqual(Object.keys(row).sort(),['audioRevision','memberEpoch','publishAllowed']);
      for(const key of ['memberEpoch','audioRevision'])assert.ok(Number.isSafeInteger(row[key])&&row[key]>0,'actual_roster_authority_invalid');
      assert.equal(typeof row.publishAllowed,'boolean','actual_roster_authority_invalid');
    }
  }
  assert.deepEqual(after,before,'snapshot_must_not_mutate_original_im_authority');return true;
}

// verifyRosterTransitions 逐条断言设备端participantChanged投影历史：水位严格递增且晚于基准、
// 每条都是合法成员集合（显式空轨）、状态沿合法序列单调推进且终态精确。C++以2ms轮询投影当前
// 累积名册，允许跳过中间态，因此只要求子序列单调覆盖，不接受回退、重复水位或带轨条目。
function verifyRosterTransitions(history,expectations,afterSequence=0){
  assert.ok(Array.isArray(history)&&history.length>0,'cpp_roster_history_required');
  assert.ok(Array.isArray(expectations)&&expectations.length>0&&expectations.every(Array.isArray),'cpp_roster_expectations_invalid');
  assert.ok(Number.isSafeInteger(afterSequence)&&afterSequence>=0,'cpp_roster_expectation_invalid');
  const normalized=expectations.map(expectation=>{assert.ok(Array.isArray(expectation)&&expectation.length<=32&&
    new Set(expectation).size===expectation.length&&expectation.every(member=>typeof member==='string'),'cpp_roster_expectation_invalid');
    return expectation.slice().sort();});
  for(let index=1;index<normalized.length;index++)
    assert.ok(normalized[index].join(',')!==normalized[index-1].join(','),'cpp_roster_expectation_repeated');
  const match=value=>normalized.findIndex(expectation=>expectation.length===value.length&&
    expectation.every((member,index)=>member===value[index]));
  let previous=-1,previousSeq=0;
  for(const entry of history){
    assert.ok(entry&&typeof entry==='object'&&!Array.isArray(entry)&&Object.keys(entry).length===3,'cpp_roster_history_invalid');
    assert.ok(Number.isSafeInteger(entry.eventSeq)&&entry.eventSeq>afterSequence,'cpp_roster_history_watermark');
    assert.ok(entry.eventSeq>previousSeq,'cpp_roster_history_watermark_repeated');
    previousSeq=entry.eventSeq;
    assert.ok(Array.isArray(entry.identities)&&entry.identities.every(member=>typeof member==='string'),'cpp_roster_history_invalid');
    assert.equal(entry.trackCount,0,'current_roster_track_subset_required');
    const current=match(entry.identities.slice().sort());
    assert.ok(current>=0,'cpp_roster_state_illegal');
    assert.ok(current>=previous,'cpp_roster_state_regression');
    previous=current;
  }
  assert.equal(match(history[history.length-1].identities.slice().sort()),normalized.length-1,'cpp_roster_final_state_mismatch');
  return true;
}

// verifyParticipantTrackContract 取证当前生产边界：名册条目轨恒空、无ipcTrack控制下行、
// 无trackState回执上行。场景文档的trackState(reason=participantLeft)属于待实现的独立成员轨契约，
// 驱动显式断言其缺失而非伪造发生；任何轨控制或回执出现即判定越界。
function verifyParticipantTrackContract(facts){
  const keys=['ipcTrackReceived','trackStateSent','rosterTrackEntries','truncated','available','closed'];
  assert.ok(facts&&typeof facts==='object'&&!Array.isArray(facts),'cpp_track_contract_required');
  assert.deepEqual(Object.keys(facts).sort(),keys.slice().sort(),'cpp_track_contract_invalid');
  for(const name of ['ipcTrackReceived','trackStateSent','rosterTrackEntries'])
    assert.ok(Number.isSafeInteger(facts[name])&&facts[name]>=0,'cpp_track_contract_invalid');
  for(const name of ['truncated','available','closed'])assert.equal(typeof facts[name],'boolean','cpp_track_contract_invalid');
  assert.ok(facts.available&&!facts.closed&&!facts.truncated,'cpp_track_contract_leg_inactive');
  assert.equal(facts.ipcTrackReceived,0,'participant_track_control_not_expected');
  assert.equal(facts.trackStateSent,0,'participant_track_receipt_not_expected');
  assert.equal(facts.rosterTrackEntries,0,'current_roster_track_subset_required');
  return true;
}

// verifyDecodeNeverReset 断言成员离开动作本身不让其余端的实际解码计数回退或归零；
// 静止（相等）不算失败，持续增长由混音阶段的频谱与统计阈值独立判定。
function verifyDecodeNeverReset(before,after){
  for(const rows of [before,after]){
    assert.ok(Array.isArray(rows)&&rows.length>0&&rows.length<=4,'decode_baseline_required');
    for(const row of rows){
      assert.ok(row&&typeof row==='object'&&!Array.isArray(row)&&Object.keys(row).length===3,'decode_count_invalid');
      for(const key of ['received','sent','samples'])assert.ok(Number.isSafeInteger(row[key])&&row[key]>0,'decode_count_required');
    }
  }
  assert.equal(after.length,before.length,'decode_endpoint_mismatch');
  for(let index=0;index<before.length;index++)for(const key of ['received','sent','samples'])
    assert.ok(after[index][key]>=before[index][key],'decode_reset_detected');
  return true;
}

// verifySingleEndNotice 断言原用户状态时间线中ended迁移恰好一次且为终态，
// 重复结束卡片或非终态ended都拒绝；时间线只含状态字面量，不含正文或身份。
function verifySingleEndNotice(timeline){
  assert.ok(Array.isArray(timeline)&&timeline.length>0,'state_timeline_required');
  assert.ok(timeline.every(state=>typeof state==='string'&&state.length>0),'state_timeline_invalid');
  assert.equal(timeline.filter(state=>state==='ended').length,1,'end_notice_not_unique');
  assert.equal(timeline[timeline.length-1],'ended','end_notice_not_terminal');
  return true;
}

// cleanupEndRequired 先消费原会话权威终态再判断是否需要结束；未知读取失败不能变成已清理。
async function cleanupEndRequired(user, pump) {
  if (!user.session || user.ended) return false;
  await pump(user);
  return user.state !== 'ended';
}

// connectivityDiagnostic 只保留标准RTC闭集状态和原选中候选对计数；未知/缺失保持unknown/null，绝不输出候选身份或地址。
function connectivityDiagnostic(value){
  const states={iceConnectionState:['new','checking','connected','completed','disconnected','failed','closed','unknown'],
    iceGatheringState:['new','gathering','complete','unknown'],signalingState:['stable','have-local-offer','have-remote-offer','have-local-pranswer','have-remote-pranswer','closed','unknown'],
    dtlsState:['new','connecting','connected','closed','failed','unknown'],selectedPairState:['frozen','waiting','in-progress','failed','succeeded','unknown']};
  const counters=['bytesSent','bytesReceived','requestsSent','requestsReceived','responsesSent','responsesReceived','consentRequestsSent'];
  const keys=[...Object.keys(states),'nominated',...counters];
  if(value===undefined)return Object.fromEntries(keys.map(key=>[key,Object.hasOwn(states,key)?'unknown':null]));
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'invalid_diagnostic_connectivity');
  assert.deepEqual(Object.keys(value).sort(),keys.slice().sort(),'invalid_diagnostic_connectivity');
  const result={};
  for(const [key,allowed] of Object.entries(states)){assert.ok(allowed.includes(value[key]),'invalid_diagnostic_connectivity_state');result[key]=value[key];}
  assert.ok(value.nominated===null||typeof value.nominated==='boolean','invalid_diagnostic_connectivity_nomination');result.nominated=value.nominated;
  for(const key of counters){assert.ok(value[key]===null||(Number.isSafeInteger(value[key])&&value[key]>=0),'invalid_diagnostic_connectivity_count');result[key]=value[key];}
  return result;
}

// mediaDiagnostic 只保留失败现场的有限实际计数与驱动状态；缺失解码字段为null，不由收包推断帧或播放。
function mediaDiagnostic(endpoint, facts) {
  assert.ok(Number.isInteger(endpoint)&&endpoint>=0&&endpoint<4,'invalid_diagnostic_endpoint');
  const count=value=>{assert.ok(value===null||(Number.isSafeInteger(value)&&value>=0),'invalid_diagnostic_count');return value;};
  const scalar=value=>{assert.ok(value===null||(Number.isFinite(value)&&value>=0&&value<=16),'invalid_diagnostic_amplitude');return value;};
  const observations=count(facts.observations),nonSilentWindows=count(facts.nonSilentWindows);
  assert.ok(nonSilentWindows===null||(observations!==null&&nonSilentWindows<=observations),'invalid_diagnostic_windows');
  assert.ok(facts.spectralPeaks===null||(Array.isArray(facts.spectralPeaks)&&facts.spectralPeaks.length===4),'invalid_diagnostic_spectrum');
  assert.ok(facts.peerState===null||['new','connecting','connected','disconnected','failed','closed'].includes(facts.peerState),'invalid_diagnostic_peer');
  assert.ok(facts.contextState===null||['suspended','running','closed','interrupted'].includes(facts.contextState),'invalid_diagnostic_context');
  assert.ok(Array.isArray(facts.captureTracks)&&facts.captureTracks.length<=2,'invalid_diagnostic_tracks');
  const captureTracks=facts.captureTracks.map(track=>{
    assert.ok(['live','ended'].includes(track.readyState)&&typeof track.enabled==='boolean'&&typeof track.muted==='boolean','invalid_diagnostic_track');
    const value={readyState:track.readyState,enabled:track.enabled,muted:track.muted};
    for(const name of ['echoCancellation','noiseSuppression','autoGainControl']){assert.ok(track[name]===null||typeof track[name]==='boolean','invalid_diagnostic_setting');value[name]=track[name];}
    value.sampleRate=count(track.sampleRate);return value;
  });
  return {endpoint,diagnosticOnly:true,decodedPCMWindows:observations,nonSilentWindows,nonSilent:nonSilentWindows===null?null:nonSilentWindows>0,
    spectralPeaks:facts.spectralPeaks===null?null:facts.spectralPeaks.map(scalar),maxRMS:scalar(facts.maxRMS),
    receivedPackets:count(facts.receivedPackets),sentPackets:count(facts.sentPackets),decodedSamples:count(facts.decodedSamples),
    decodedFrames:count(facts.decodedFrames),peerState:facts.peerState,contextState:facts.contextState,captureTracks,
    connectivity:connectivityDiagnostic(facts.connectivity)};
}

// collectMediaDiagnostic 在浏览器读取当前真实Peer及PCM观察水位，不修改轨、许可或会话；音频RTC帧计数不存在时明确留空。
async function collectMediaDiagnostic(peer, spectrum, capture) {
  let receivedPackets=null,sentPackets=null,decodedSamples=null,decodedFrames=null;
  // 浏览器函数会以toString注入原页面，因此本闭集读取自包含；只查询，不更换Peer/轨或触发信令。
  const state=(value,allowed)=>allowed.includes(value)?value:'unknown';
  const connectivity={iceConnectionState:state(peer?.iceConnectionState,['new','checking','connected','completed','disconnected','failed','closed']),
    iceGatheringState:state(peer?.iceGatheringState,['new','gathering','complete']),
    signalingState:state(peer?.signalingState,['stable','have-local-offer','have-remote-offer','have-local-pranswer','have-remote-pranswer','closed']),
    dtlsState:'unknown',selectedPairState:'unknown',nominated:null,bytesSent:null,bytesReceived:null,requestsSent:null,requestsReceived:null,
    responsesSent:null,responsesReceived:null,consentRequestsSent:null};
  if(peer){let timer;try{
    const stats=await Promise.race([peer.getStats(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('diagnostic_stats_timeout')),2000);})]);
    const transports=[]; // 最多保留两个本地引用：多个传输时不猜测哪条候选对属于当前音频。
    stats.forEach(item=>{
      if(item.type==='transport'&&transports.length<2)transports.push(item);
      if(item.type==='inbound-rtp'&&item.kind==='audio'){
        if(Number.isSafeInteger(item.packetsReceived))receivedPackets=(receivedPackets??0)+item.packetsReceived;
        if(Number.isFinite(item.totalSamplesReceived)&&Number.isFinite(item.concealedSamples))decodedSamples=(decodedSamples??0)+Math.max(0,item.totalSamplesReceived-item.concealedSamples);
        if(Number.isSafeInteger(item.framesDecoded))decodedFrames=(decodedFrames??0)+item.framesDecoded;
      }
      if(item.type==='outbound-rtp'&&item.kind==='audio'&&Number.isSafeInteger(item.packetsSent))sentPackets=(sentPackets??0)+item.packetsSent;
    });
    if(transports.length===1){
      const transport=transports[0];connectivity.dtlsState=state(transport.dtlsState,['new','connecting','connected','closed','failed']);
      // 仅在内部按标准transport指针寻找原选中候选；nominated或收包不能替代selected事实，身份不返回宿主。
      const pair=typeof transport.selectedCandidatePairId==='string'&&transport.selectedCandidatePairId.length<=256&&typeof stats.get==='function'
        ?stats.get(transport.selectedCandidatePairId):null;
      if(pair?.type==='candidate-pair'){
        connectivity.selectedPairState=state(pair.state,['frozen','waiting','in-progress','failed','succeeded']);
        connectivity.nominated=typeof pair.nominated==='boolean'?pair.nominated:null;
        for(const key of ['bytesSent','bytesReceived','requestsSent','requestsReceived','responsesSent','responsesReceived','consentRequestsSent'])
          connectivity[key]=Number.isSafeInteger(pair[key])&&pair[key]>=0?pair[key]:null;
      }
    }
  }finally{clearTimeout(timer);}}
  return {observations:spectrum?.observations??null,nonSilentWindows:spectrum?.nonSilentWindows??null,
    spectralPeaks:spectrum?.peaks??null,maxRMS:spectrum?.maxRMS??null,receivedPackets,sentPackets,decodedSamples,decodedFrames,
    peerState:peer?.connectionState??null,contextState:spectrum?.context.state??null,connectivity,
    captureTracks:(capture?.getAudioTracks()??[]).map(track=>{const settings=track.getSettings();return{readyState:track.readyState,enabled:track.enabled,muted:track.muted,
      echoCancellation:settings.echoCancellation??null,noiseSuppression:settings.noiseSuppression??null,autoGainControl:settings.autoGainControl??null,sampleRate:settings.sampleRate??null};})};
}

// installSpectrum 在实际浏览器有限观察已解码远端轨；不覆盖生产onended，也不改变track.enabled或麦克风许可。
function installSpectrum(track, frequencies, spectrum, key = '__conferenceSpectrum') {
  if (!track || track.kind !== 'audio' || track.readyState !== 'live') throw Error('actual_audio_track_required');
  const context = new AudioContext({sampleRate:48000});
  const source = context.createMediaStreamSource(new MediaStream([track]));
  const analyser = context.createAnalyser(); analyser.fftSize = 4096;
  const gain = context.createGain(); gain.gain.value = 0;
  source.connect(analyser); analyser.connect(gain); gain.connect(context.destination);
  const samples = new Float32Array(4096);
  const state = window[key] = {context,source,analyser,gain,observations:0,nonSilentWindows:0,maxRMS:0,peaks:[0,0,0,0],closed:false};
  state.reset = () => {state.observations=0;state.nonSilentWindows=0;state.maxRMS=0;state.peaks=[0,0,0,0];};
  state.timer = setInterval(() => {
    analyser.getFloatTimeDomainData(samples);
    const peaks = spectrum(samples,context.sampleRate,frequencies);
    state.peaks = state.peaks.map((value,index) => Math.max(value,peaks[index])); state.observations++;
    let sum=0;for(const value of samples)sum+=value*value;
    const rms=Math.sqrt(sum/samples.length);state.maxRMS=Math.max(state.maxRMS,rms);if(rms>0.001)state.nonSilentWindows++;
  },100);
  state.stop = async () => {
    if (state.closed) return state.closing;
    state.closed=true;clearInterval(state.timer);source.disconnect();gain.disconnect();state.closing=context.close();return state.closing;
  };
  track.addEventListener('ended',state.stop,{once:true});
  void context.resume().catch(() => state.stop());
}

// waitFor 有限等待实际条件，失败阶段留在报告，绝不以额外Sleep掩盖协议未建立。
async function waitFor(check, timeout = 25000) {
  const deadline = Date.now()+timeout;
  while (Date.now()<deadline) {if (await check()) return; await new Promise(resolve => setTimeout(resolve,100));}
  throw Error('actual_state_timeout');
}

// macBody 固定read3/set5字段，不用自由participant或客户端epoch替代原登录用户。
function macBody(operation, callId, targetUserID, allowed, expectedRevision) {
  const body={operation,callId,targetUserID:safeUserID(targetUserID)};
  assert.ok(operation==='read'||operation==='set');
  if (operation==='set') {assert.equal(typeof allowed,'boolean');assert.ok(Number.isSafeInteger(expectedRevision)&&expectedRevision>0);Object.assign(body,{allowed,expectedRevision});}
  return body;
}

// verifyPermissionOnly 检查主持人恢复的是原成员发言许可，不替用户开启麦克风或更换媒体腿。
function verifyPermissionOnly(before, after, media) {
  assert.equal(before.publishAllowed, false, 'original_revoked_permission_required');
  assert.equal(after.publishAllowed, true, 'original_permission_not_restored');
  assert.ok(Number.isSafeInteger(before.audioRevision) && before.audioRevision > 0);
  assert.equal(after.audioRevision, before.audioRevision + 1, 'permission_revision_mismatch');
  assert.equal(after.memberEpoch, before.memberEpoch, 'permission_must_not_rejoin_member');
  assert.ok(Number.isSafeInteger(before.memberEpoch) && before.memberEpoch > 0);
  assert.ok(media && media.samePeer === true && media.connected === true &&
    media.sameCapture === true && media.sameTrack === true && media.senderBound === true &&
    media.trackCount === 1 && media.trackLive === true && media.trackEnabled === false,
  'host_permission_must_not_open_local_microphone');
  return true;
}

// verifyGroupLeaveResult 严格关联原请求、原目标实例和剩余集合；业务结果不声称终端媒体已停止。
function verifyGroupLeaveResult(result, command, expectedRemaining, ended=false) {
  assert.ok(result&&result.method==='interaction.result'&&result.code===200&&result.msgToken===command.msgToken&&
    result.sessionId===command.sessionId,'original_leave_result_required');
  const data=result.data,params=command.params;
  assert.ok(data&&data.operation==='interaction.leave'&&data.callId===params.callId&&
    data.participantId===params.participantId&&data.memberEpoch===params.expectedMemberEpoch&&data.ended===ended,'original_leave_binding_required');
  assert.ok(Array.isArray(data.remaining)&&data.remaining.length===expectedRemaining.length,'original_remaining_members_required');
  assert.deepEqual(data.remaining.map(safeUserID).sort(),expectedRemaining.map(safeUserID).sort(),'original_remaining_members_required');
  return true;
}

// run 真实端云链路唯一设备腿、三实际登录用户；资源和许可失败保持失败，不重启设备绕过状态栅栏。
async function run(options={}) {
  assert.ok(options&&typeof options==='object'&&!Array.isArray(options),'conference_options_required');
  assert.ok(options.onEstablished===undefined||typeof options.onEstablished==='function','established_hook_required');
  const config=validateConfiguration(process.env);
  const users=config.tokens.map((token,index)=>({token,index:index+1,name:`conference-u${index+1}-${randomUUID()}`,events:[],state:'',ended:false,stateTimeline:[]}));
  const sourceName=`conference-device-${randomUUID()}`;
  const browserEnv=wav=>({...process.env,AGENT_BROWSER_ARGS:[process.env.AGENT_BROWSER_ARGS||'--no-sandbox',
    '--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--mute-audio','--autoplay-policy=no-user-gesture-required',
    `--use-file-for-fake-audio-capture=${wav}`].join(',')});
  const sourceEnv=browserEnv(config.wavs[0]);users.forEach((user,index)=>user.env=browserEnv(config.wavs[index+1]));
  const server=http.createServer((_,res)=>{res.writeHead(200,{'content-type':'text/html','cache-control':'no-store'});res.end('<!doctype html><html><body>会议音频核心验收</body></html>');});
  let child, completion, exited=false, processDeadline, heartbeat, heartbeatsStopped=false, heartbeatFailed=false;
  let heartbeatInFlight=Promise.resolve(); // 清理必须等原续租调用实际退出，不让其在disband后再写同群。
  let heartbeatCount=0,heartbeatsStoppedAt=null; // 心跳续租次数与停止时刻，供人数边界的过期清扫取证。
  const startedBrowsers=new Set(); // 只关闭本任务已尝试打开的具名会话。
  let conversationId,callId,stage='configuration',sourceSession,sourceResourcesCaptured=false;
  let passed=false; // 最终通过必须晚于本任务精确清理，不能先打印pass再吞掉清理失败。
  const report={path:'cpp_mqtt_formal_http_im_four_actual_endpoints',actualUsers:3,actualEndpoints:4,phases:[]};
  const recordingEvents=createRecordingEventRelay(); // 录制观察不随新媒体腿events重置而丢失，默认无订阅即无新增行为。
  // api 保留固定状态供负面授权检查，不反射真实凭据或后端自由正文。
  async function api(user,path,body={},expectSuccess=true,method='POST',requestSignal) {
    const response=await fetch(`${process.env.UR_IPC_BASE_URL}/api/v1/${path}`,{method,
      headers:{'content-type':'application/json','app-id':process.env.UR_IPC_APP_ID,'tenant-code':process.env.UR_IPC_TENANT,token:user.token},
      ...(method==='GET'?{}:{body:JSON.stringify(body)}),redirect:'manual',
      signal:requestSignal?AbortSignal.any([requestSignal,AbortSignal.timeout(15000)]):AbortSignal.timeout(15000)});
    const value=await readBoundedJSON(response);
    if(expectSuccess&&(response.status!==200||value.code!==200)){
      const error=Error('formal_api_rejected');error.httpStatus=response.status;error.actualCode=value.code;throw error;
    }
    return expectSuccess?value.data:{status:response.status,code:value.code};
  }
  // pump 固定128条未消费结果，先保存再确认原outbox；不得丢弃其他原请求事实。
  async function pump(user,requestSignal) {
    const received=await api(user,'im/interaction/events',{endpointId:user.endpoint,limit:32},true,'POST',requestSignal);
    for(const delivery of received.list||[]) {
      const event=delivery.event;
      recordingEvents.observe(user,event); // 同一原事件在原ACK之前投影；观察不能把一端事实冒作其他端已收到。
      if(event?.method==='interaction.state'&&event.sessionId===user.session){user.state=event.data?.state;
        user.stateTimeline.push(user.state);} // 状态时间线只留字面量，供结束卡片唯一性断言。
      if(event?.method==='interaction.result') {assert.ok(user.events.length<128,'event_buffer_full');user.events.push(event);}
    }
    if(received.list?.length)await api(user,'im/interaction/events/ack',{endpointId:user.endpoint,deliveryIds:received.list.map(item=>item.deliveryId)},true,'POST',requestSignal);
  }
  // command 只按原token取实际持久结果，202受理不能当作业务完成。
  async function command(user,method,params,originalToken,requestSignal) {
    const token=originalToken??randomUUID(),command={method,msgToken:token,params};
    assert.ok(typeof token==='string'&&token.length>0&&token.length<=128&&!/[\x00-\x1f\x7f]/.test(token),'original_command_token_required');
    if(method==='interaction.open')command.target={productID:process.env.UR_DEVICE_PRODUCT,deviceName:process.env.UR_DEVICE_NAME};else command.sessionId=user.session;
    const accepted=await api(user,'im/interaction/commands',{endpointId:user.endpoint,command},true,'POST',requestSignal);
    assert.equal(accepted.accepted,true);assert.equal(accepted.msgToken,token);
    let result;await waitFor(async()=>{requestSignal?.throwIfAborted();await pump(user,requestSignal);const index=user.events.findIndex(event=>event.msgToken===token);if(index<0)return false;result=user.events.splice(index,1)[0];return true;});
    if(result.code!==200) {
      const error=Error('original_operation_rejected');
      error.actualCode=Number.isSafeInteger(result.code)&&result.code>=0&&result.code<=Number.MAX_SAFE_INTEGER?result.code:null;
      throw error;
    }
    return result;
  }
  // activeMembers 使用原IM只读接口，不把多个APP端点或D计算成IM用户。
  async function activeMembers(expected) {
    const active=await api(users[0],`im/call/active?conversationId=${conversationId}`,{},true,'GET');
    if(expected.length===0)assert.equal(active.active,false,'original_im_not_ended');
    else {assert.equal(active.active,true);assert.equal(active.callId,callId);assert.deepEqual((active.participantIds||[]).map(safeUserID).sort(),expected.slice().sort());}
  }
  // roster 读取C++桥消费者提交后的投影，沿用既有阶段预算；不重发、不改腿或媒体采集。
  async function roster(label,expected,afterSequence=0){
    if(!config.rosterEnabled)return null;
    stage=label;
    let observed;
    await waitFor(async()=>{
      observed=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.read(${JSON.stringify(sourceSession)},${JSON.stringify(expected)}))`));
      try{verifyRosterObservation(observed,expected.length,afterSequence);return true;}catch{return false;}
    });
    report.roster??={consumer:'cpp_accepted_local_ui',fullSnapshotRecoveryVerified:false,phases:[]};
    report.roster.phases.push({label,...observed});return observed.eventSeq;
  }
  // rosterTransitions 读取设备端逐条投影历史并按合法推进序列断言；设备轮询允许跳过
  // 中间态但不接受回退，历史有损即失败。报告只记录条数，不反射成员身份。
  async function rosterTransitions(label,expectations,afterSequence){
    if(!config.rosterEnabled)return null;
    stage=label;
    let history,records;
    await waitFor(async()=>{
      history=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.transitions(${JSON.stringify(sourceSession)}))`));
      // 观察器保留整条腿的历史；只截取动作水位之后的记录，再逐条验证，不能把查询前历史当新事件。
      records=history.records.filter(entry=>entry.eventSeq>afterSequence);
      try{verifyRosterTransitions(records,expectations,afterSequence);return true;}catch{return false;}
    });
    assert.equal(history.truncated,false,'cpp_roster_history_truncated');
    report.roster??={consumer:'cpp_accepted_local_ui',fullSnapshotRecoveryVerified:false,phases:[]};
    report.roster.transitions??=[];
    report.roster.transitions.push({label,records:records.length});return records.length;
  }
  // snapshotMedia 读取原四端RTC样本并在浏览器内比较原对象；不对外输出Peer/成员标识或伪造解码数量。
  async function snapshotMedia(remember,indices=[0,1,2,3]){
    const rows=[];
    for(const endpoint of [{name:sourceName,env:sourceEnv,index:0},...users.map((user,index)=>({...user,index:index+1}))].filter(endpoint=>indices.includes(endpoint.index))){
      rows.push(evaluated(await browser(endpoint.name,endpoint.env,'eval',`(async()=>{
        const peer=${endpoint.index===0?`views.get(${JSON.stringify(sourceSession)})?.peer`:'window.__ipcPeer'};if(!peer)throw Error('actual_snapshot_peer_required');
        if(${remember?'true':'false'})window.__conferenceSnapshotOriginalPeer=peer;
        let received=0,sent=0,samples=0;(await peer.getStats()).forEach(s=>{
          if(s.type==='inbound-rtp'&&s.kind==='audio'){received+=s.packetsReceived||0;if(Number.isFinite(s.totalSamplesReceived)&&Number.isFinite(s.concealedSamples))samples+=Math.max(0,s.totalSamplesReceived-s.concealedSamples);}
          if(s.type==='outbound-rtp'&&s.kind==='audio')sent+=s.packetsSent||0;});
        return JSON.stringify({samePeer:peer===window.__conferenceSnapshotOriginalPeer,connected:peer.connectionState==='connected',received,sent,samples});})()`)));
    }
    return rows;
  }
  // fullSnapshotQuery 由同一原UI端点提交既有ipcSync；重取当前名册不伪造断网，也不恢复媒体或IM话权。
  async function fullSnapshotQuery(afterSequence,options={}){
    if(!config.rosterSyncEnabled)return afterSequence;
    const expected=options.expected??users.map(user=>user.id),indices=options.indices??[0,1,2,3],allowed=options.allowed??indices;
    const label=options.label??'four_original_peers_continue_after_snapshot',before=await snapshotMedia(true,indices);
    // authority 查询只读取原活成员，不把展示名册的空轨子集当作MAC控制对象。
    const authority=async()=>Promise.all(expected.map(async id=>{
      const value=await api(users[0],'im/interaction/talk/audio-access',macBody('read',callId,id));
      return {memberEpoch:value.memberEpoch,audioRevision:value.audioRevision,publishAllowed:value.publishAllowed};
    }));
    const macBefore=await authority();
    // localMute 只核原浏览器真实对象，查询不能替用户重新打开原采集轨。
    const localMute=async()=>{
      if(!options.locallyMuted)return;
      const user=options.locallyMuted;
      const actual=evaluated(await browser(user.name,user.env,'eval','(()=>{const saved=__conferenceLocalMute,tracks=__capture.getAudioTracks();return JSON.stringify({samePeer:__ipcPeer===saved.peer,connected:__ipcPeer.connectionState==="connected",sameCapture:__capture===saved.capture,sameTrack:tracks[0]===saved.track,senderBound:__ipcPeer.getSenders().some(sender=>sender.track===saved.track),trackCount:tracks.length,trackLive:tracks[0]?.readyState==="live",trackEnabled:tracks[0]?.enabled});})()'));
      verifyPermissionOnly(options.permissionBefore,options.permissionAfter,actual);
    };
    const membership=async()=>{
      if(!options.reconcile)return;
      await activeMembers(expected);
      const current=await api(users[0],'im/conversation/get-members',{conversationId});
      assert.ok(Array.isArray(current.list),'formal_group_members_required');
      assert.deepEqual(current.list.filter(member=>member.participantType==='user').map(member=>safeUserID(member.participantId)).sort(),users.map(user=>user.id).sort(),'snapshot_must_not_change_original_group_membership');
    };
    await membership();await localMute();
    stage='cpp_full_snapshot_ready';
    await waitFor(async()=>evaluated(await browser(sourceName,sourceEnv,'eval',`(()=>{const view=views.get(${JSON.stringify(sourceSession)});return !!view?.syncButton&&!view.syncButton.disabled&&['ready','succeeded'].includes(view.syncState);})()`))===true);
    stage='cpp_full_snapshot_query';
    await browser(sourceName,sourceEnv,'eval',`(()=>{const view=views.get(${JSON.stringify(sourceSession)});if(!view?.syncButton||view.syncButton.disabled||!['ready','succeeded'].includes(view.syncState))throw Error('original_snapshot_button_required');view.syncButton.scrollIntoView({block:'center'});return true;})()`);
    await browser(sourceName,sourceEnv,'snapshot','-i');
    await browser(sourceName,sourceEnv,'find','role','button','click','--name','查询完整成员名册');
    let observed,installed;
    // 查询失败只留有限观测次数、基准与固定原因，区分未消费、拒绝和水位不匹配，不反射任何成员或令牌。
    const queryStarted=Date.now();
    const queryDiagnostic=report.queryDiagnostic={attempts:0,elapsedMs:0,afterSequence,expectedCount:expected.length,lastFailure:'notObserved'};
    const queryFailures=new Set(['cpp_snapshot_not_consumed','cpp_snapshot_not_succeeded','cpp_snapshot_roster_watermark_mismatch',
      'cpp_roster_new_consumption_required','cpp_roster_consumption_not_confirmed','cpp_roster_member_mismatch','current_roster_track_subset_required']);
    await waitFor(async()=>{
      observed=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.readSnapshot(${JSON.stringify(sourceSession)}))`));
      installed=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.read(${JSON.stringify(sourceSession)},${JSON.stringify(expected)}))`));
      queryDiagnostic.attempts++;queryDiagnostic.elapsedMs=Date.now()-queryStarted;
      try{verifySnapshotObservation(observed,installed,expected.length,afterSequence);queryDiagnostic.lastFailure='none';return true;}
      catch(error){queryDiagnostic.lastFailure=queryFailures.has(error.message)?error.message:'invalidObservation';return false;}
    });
    await phase(label,indices,allowed);
    let after;
    await waitFor(async()=>{after=await snapshotMedia(false,indices);try{verifySnapshotContinuity(before,after,indices.length,options.quietIndices??[]);return true;}catch{return false;}});
    const macAfter=await authority();verifyRosterReconcileAuthority(macBefore,macAfter);
    await membership();await localMute();
    report.roster.fullSnapshotQueryVerified=true;
    // 只报告原消费者和四端Peer实际事实；恢复许可的真假不能由UI成功反推，更不能伪造固定reason。
    report.roster.snapshotApplication={coreAccepted:observed.state==='succeeded',freshWatermark:installed.eventSeq>afterSequence,
      originalPeersContinued:after.every(row=>row.samePeer&&row.connected)};
    report.roster.query={...observed,exactMembers:installed.exactMembers,memberCount:installed.memberCount};
    report.roster.queryMedia={before,after}; // 普通查询只读；显式重取矩阵需两个阶段均通过才标展示名册核对完成。
    if(options.reconcile){
      report.roster.reconciliation??=[];
      report.roster.reconciliation.push({label,sameEndpoint:true,freshWatermark:installed.eventSeq>afterSequence,
        exactMembers:installed.exactMembers,memberCount:installed.memberCount,originalIMInstancesAndMACUnchanged:true,
        originalGroupMembershipRetained:true,originalPeersContinued:true,actualPCMChecked:true,localMuteRetained:!!options.locallyMuted});
      report.roster.fullSnapshotRecoveryVerified=report.roster.reconciliation.length===2;
      report.roster.recoveryKind='authoritative_roster_reconciliation';report.roster.networkRecoveryVerified=false;
    }
    return installed.eventSeq; // 后续增量必须从已实际安装的完整水位起算，不能把本次快照误记为成员离开。
  }
  // closeApp 实際退出当前APP音频驱动，服务端终态不是本端资源自动已停止的证据。
  async function closeApp(user) {
    const result=evaluated(await browser(user.name,user.env,'eval',`(async()=>{
      const p=window.__ipcPeer,c=window.__capture,r=window.__remoteTrack,a=window.__ipcAudio,s=window.__conferenceSpectrum;
      if(s)await s.stop();if(a)await a.stop();c?.getTracks().forEach(t=>t.stop());r?.stop();p?.close();
      return JSON.stringify({peerClosed:p?.connectionState==='closed',captureStopped:!!c&&c.getTracks().every(t=>t.readyState==='ended'),
        remoteStopped:r?.readyState==='ended',audioClosed:a?.context.state==='closed',spectrumClosed:s?.context.state==='closed',sinkReleased:a?.sink.srcObject===null});})()`));
    assert.deepEqual(result,{peerClosed:true,captureStopped:true,remoteStopped:true,audioClosed:true,spectrumClosed:true,sinkReleased:true});return result;
  }
  // rejoinMember 不用于普通会议流程，只由显式录制扩展调用；正式IM加入后协商全新真实Peer。
  async function rejoinMember(user,signal) {
    assert.ok(users.includes(user),'original_member_required');
    return rejoinConferenceMember(user,{
      access:(member,s)=>api(member,'im/interaction/talk/audio-access',macBody('read',callId,member.id),true,'POST',s),
      end:(member,s)=>command(member,'interaction.end',{reason:'userHangup'},undefined,s),
      terminal:async(member,s)=>waitFor(async()=>{s?.throwIfAborted();await pump(member,s);return member.state==='ended';}),
      close:member=>closeApp(member),
      leave:(member,s)=>api(member,'im/call/signal',{type:'group_leave',callId},true,'POST',s),
      remaining:()=>activeMembers(users.filter(member=>member!==user).map(member=>member.id)),
      join:(member,s)=>api(member,'im/call/signal',{type:'group_join',callId},true,'POST',s),
      open:async(member,s)=>{const opened=await command(member,'interaction.open',{purpose:'ipcTalk',cameraId:process.env.UR_CONFERENCE_CAMERA_ID,callId},undefined,s);member.session=opened.sessionId;},
      negotiate:async(member,s)=>{
        s?.throwIfAborted();
        const offer=evaluated(await browser(member.name,member.env,'eval',`(async()=>{
          const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});window.__capture=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false},video:false});
          const t=peer.addTransceiver(__capture.getAudioTracks()[0],{direction:'sendrecv',streams:[__capture]});t.setCodecPreferences(RTCRtpSender.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()==='audio/opus'));
          peer.ontrack=e=>{window.__remoteTrack=e.track;(${attachListeningAudio.toString()})(e.track);(${installSpectrum.toString()})(e.track,${JSON.stringify(FREQUENCIES)},${spectralPeaks.toString()});};
          await peer.setLocalDescription(await peer.createOffer());if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);})()`));
        const answered=await command(member,'interaction.mediaOffer',{mediaRevision:1,webrtc:{iceMode:'complete',sdp:offer}},undefined,s);
        assert.equal(answered.data?.mediaRevision,1);
        await browser(member.name,member.env,'eval',`(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answered.data.webrtc.sdp)}});return true;})()`);
        await waitFor(async()=>{s?.throwIfAborted();return evaluated(await browser(member.name,member.env,'eval','__ipcPeer.connectionState === "connected"'))===true;});
        await command(member,'interaction.mediaState',{mediaRevision:1,state:'connected'},undefined,s);
        await waitFor(async()=>{s?.throwIfAborted();await pump(member,s);return member.state==='established';});
      },
      members:()=>activeMembers(users.map(member=>member.id))
    },signal);
  }
  // phase 每阶段清空频峰水位，控制生效前旧样本不能满足后阶段的禁麦/退出结论。
  async function phase(label,participants,allowed) {
    const endpoints=participants.map(index=>index===0?{name:sourceName,env:sourceEnv,index}:users[index-1]);
    for(const endpoint of endpoints)await browser(endpoint.name,endpoint.env,'eval','(()=>{__conferenceSpectrum.reset();return true;})()');
    let facts;
    await waitFor(async()=>{
      facts=[];
      for(const endpoint of endpoints)facts.push(evaluated(await browser(endpoint.name,endpoint.env,'eval',
        'JSON.stringify({observations:__conferenceSpectrum.observations,peaks:__conferenceSpectrum.peaks})')));
      if(facts.some(item=>item.observations<8))return false;
      try {facts.forEach((item,index)=>verifyMix(item,endpoints[index].index,allowed));return true;}catch{return false;}
    },25000);
    const decode=[];
    for(const endpoint of endpoints) {
      const stats=evaluated(await browser(endpoint.name,endpoint.env,'eval',`(async()=>{
        const peer=${endpoint.index===0?`views.get(${JSON.stringify(sourceSession)})?.peer`:'window.__ipcPeer'};if(!peer)throw Error('actual_peer_required');
        let received=0,sent=0,samples=0;const stats=await peer.getStats();stats.forEach(s=>{
          if(s.type==='inbound-rtp'&&s.kind==='audio'){received+=s.packetsReceived||0;if(Number.isFinite(s.totalSamplesReceived)&&Number.isFinite(s.concealedSamples))samples+=Math.max(0,s.totalSamplesReceived-s.concealedSamples);}
          if(s.type==='outbound-rtp'&&s.kind==='audio')sent+=s.packetsSent||0;});return JSON.stringify({received,sent,samples});})()`));
      assert.ok(stats.received>2&&stats.sent>2&&stats.samples>1920,'actual_opus_decode_stats_required');decode.push(stats);
    }
    report.phases.push({label,decodedPCM: facts.map((item,index)=>({endpoint:endpoints[index].index,...item,...decode[index]}))});
    return decode; // 返回各端实际计数，供成员离开后的"解码不归零"跨阶段比较。
  }
  try {
    stage='authenticated_three_distinct_users';
    for(const user of users)user.id=authenticatedUser(await api(user,'system/user/self/get-one',{withTenant:true}),process.env.UR_IPC_TENANT);
    assert.equal(new Set(users.map(user=>user.id)).size,3,'three_actual_users_required');
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    child=spawn(process.env.UR_IPC_DEVICE_BIN,[String(config.port)],{env:process.env,stdio:['ignore','pipe','pipe']});
    completion=new Promise(resolve=>child.once('close',()=>{exited=true;resolve();}));child.stderr.on('data',()=>{});
    // 显式扩展最多占独立90秒，默认四端验收仍保留原240秒总截止，不延长原常规测试。
    processDeadline=setTimeout(()=>child.kill('SIGTERM'),options.onEstablished?330000:240000);
    await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(Error('device_start_timeout')),10000);
      child.once('error',()=>{clearTimeout(timer);reject(Error('device_spawn_failed'));});
      child.stdout.on('data',bytes=>{if(bytes.toString().includes(`http://127.0.0.1:${config.port}/`)){clearTimeout(timer);resolve();}});
    });
    stage='source_page_open';
    startedBrowsers.add(sourceName);await browser(sourceName,sourceEnv,'open',`http://127.0.0.1:${config.port}/#softwareFixedPcm=1`);
    stage='source_page_probe';
    report.sourcePage=evaluated(await browser(sourceName,sourceEnv,'eval','JSON.stringify({incoming:typeof incoming==="function",available:typeof available==="boolean",activate:!!document.getElementById("activate"),softwareFixedPCM:typeof softwareFixedPcmMode==="function"&&softwareFixedPcmMode()})'));
    assert.equal(sourcePageReady(report.sourcePage),true,'source_page_unavailable');
    // 只读观测真实C++桥邀请；不修改incoming报文、原session或媒体事实。
    await browser(sourceName,sourceEnv,'eval',`(()=>{window.__conferenceInvites=new Set();const original=incoming;
      incoming=async value=>{if(value.kind==='ipcInvite'&&value.talk===true)__conferenceInvites.add(value.sessionId);return original(value);};return true;})()`);
    if(config.rosterEnabled)assert.equal(evaluated(await browser(sourceName,sourceEnv,'eval',`(${installRosterObserver.toString()})(true)`)),true,'roster_observer_install_failed');
    stage='source_platform_connection';
    await browser(sourceName,sourceEnv,'find','role','button','click','--name','连接平台');
    await waitFor(async()=>evaluated(await browser(sourceName,sourceEnv,'eval','available === true && closed === false'))===true);
    stage='original_im_membership';
    const created=await api(users[0],'im/conversation/create',{type:'group',name:`会议验收-${randomUUID()}`,participants:users.map(user=>({type:'user',id:user.id}))});
    conversationId=Number(safeUserID(created.id));
    const group=await api(users[0],'im/call/signal',{type:'group_invite',callType:'voice',conversationId,members:users.slice(1).map(user=>user.id)});
    callId=group.callId;assert.ok(typeof callId==='string'&&callId.length);
    for(const user of users.slice(1))await api(user,'im/call/signal',{type:'group_join',callId});
    await activeMembers(users.map(user=>user.id));
    const touch=()=>{if(heartbeatsStopped)return;heartbeatInFlight=(async()=>{
      for(const user of users.filter(user=>!user.ended)){if(heartbeatsStopped)break;if(user.ended)continue;try{await api(user,'im/call/signal',{type:'call_heartbeat',callId});heartbeatCount++;}catch{heartbeatFailed=true;}}
      if(!heartbeatsStopped)heartbeat=setTimeout(touch,5000);})();};heartbeat=setTimeout(touch,5000);
    for(const user of users) {
      user.endpoint=(await api(user,'im/interaction/endpoints/register')).endpointId;assert.ok(user.endpoint);
      startedBrowsers.add(user.name);await browser(user.name,user.env,'open',`http://127.0.0.1:${server.address().port}/`);
    }
    stage='single_original_device_invitation';
    const opened=await command(users[0],'interaction.open',{purpose:'ipcTalk',cameraId:process.env.UR_CONFERENCE_CAMERA_ID,callId});
    users[0].session=sourceSession=opened.sessionId;assert.ok(sourceSession);
    await confirmAdmission('accept',sourceSession,(...args)=>browser(sourceName,sourceEnv,...args),'ipcTalk');
    await waitFor(async()=>evaluated(await browser(sourceName,sourceEnv,'eval',`!!views.get(${JSON.stringify(sourceSession)})?.peer?.localDescription`))===true);
    for(const user of users) {
      stage=`actual_app_${user.index}_negotiation`;
      if(user.index>1) {const extra=await command(user,'interaction.open',{purpose:'ipcTalk',cameraId:process.env.UR_CONFERENCE_CAMERA_ID,callId});user.session=extra.sessionId;assert.ok(user.session);}
      const offer=evaluated(await browser(user.name,user.env,'eval',`(async()=>{
        const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});window.__capture=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false},video:false});
        const t=peer.addTransceiver(__capture.getAudioTracks()[0],{direction:'sendrecv',streams:[__capture]});
        t.setCodecPreferences(RTCRtpSender.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()==='audio/opus'));
        peer.ontrack=e=>{window.__remoteTrack=e.track;(${attachListeningAudio.toString()})(e.track);
          (${installSpectrum.toString()})(e.track,${JSON.stringify(FREQUENCIES)},${spectralPeaks.toString()});};
        await peer.setLocalDescription(await peer.createOffer());if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{
          const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
          peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);})()`));
      const answered=await command(user,'interaction.mediaOffer',{mediaRevision:1,webrtc:{iceMode:'complete',sdp:offer}});
      assert.equal(answered.data?.mediaRevision,1);
      await browser(user.name,user.env,'eval',`(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answered.data.webrtc.sdp)}});return true;})()`);
      await waitFor(async()=>evaluated(await browser(user.name,user.env,'eval','__ipcPeer.connectionState === "connected"'))===true);
      await command(user,'interaction.mediaState',{mediaRevision:1,state:'connected'});
      await waitFor(async()=>{await pump(user);return user.state==='established';});
    }
    assert.equal(new Set(users.map(user=>user.session)).size,3,'distinct_app_legs_required');
    const captureSettings=[];
    for (const endpoint of [{name:sourceName,env:sourceEnv,index:0},...users]) {
      const capture=endpoint.index===0?`views.get(${JSON.stringify(sourceSession)})?.stream`:'window.__capture';
      const settings=evaluated(await browser(endpoint.name,endpoint.env,'eval',`(()=>{const capture=${capture};if(!capture)throw Error('actual_capture_required');
        return JSON.stringify(capture.getAudioTracks().map(track=>{const s=track.getSettings();return {echoCancellation:s.echoCancellation??null,noiseSuppression:s.noiseSuppression??null,autoGainControl:s.autoGainControl??null};}));})()`));
      verifyFixedPCMSettings(settings);captureSettings.push({endpoint:endpoint.index,...settings[0]});
    }
    report.captureSettings=captureSettings;report.softwareFixedPCM=true;report.hardwareAECVerified=false;
    await browser(sourceName,sourceEnv,'eval',`(()=>{const view=views.get(${JSON.stringify(sourceSession)});if(!view?.remoteTrack)throw Error('actual_track_required');
      (${installSpectrum.toString()})(view.remoteTrack,${JSON.stringify(FREQUENCIES)},${spectralPeaks.toString()});
      (${installSpectrum.toString()})(view.stream.getAudioTracks()[0],${JSON.stringify(FREQUENCIES)},${spectralPeaks.toString()},'__conferenceCaptureSpectrum');
      window.__conferenceSourceResources={peer:view.peer,capture:view.stream.getTracks(),remote:view.remoteTrack,sink:view.sink};return true;})()`);sourceResourcesCaptured=true;
    stage='actual_source_fixed_pcm';
    await waitFor(async()=>evaluated(await browser(sourceName,sourceEnv,'eval','__conferenceCaptureSpectrum.observations >= 8'))===true);
    report.sourceCapture=evaluated(await browser(sourceName,sourceEnv,'eval','JSON.stringify({observations:__conferenceCaptureSpectrum.observations,nonSilentWindows:__conferenceCaptureSpectrum.nonSilentWindows,maxRMS:__conferenceCaptureSpectrum.maxRMS,peaks:__conferenceCaptureSpectrum.peaks})'));
    assert.ok(report.sourceCapture.observations>=8&&report.sourceCapture.nonSilentWindows>0&&report.sourceCapture.peaks[0]>0.008,'actual_source_pcm_required');
    stage='four_actual_opus_mix_minus_self';await phase(stage,[0,1,2,3],[0,1,2,3]);
    const initialRosterSequence=config.rosterEnabled?await roster('cpp_initial_joined_members',users.map(user=>user.id)):null;
    if(config.rosterEnabled){
      // 初始加入逐条断言：三条joined增量到达顺序即成员ID排序，设备轮询允许并步但不接受回退。
      const sorted=[...users.map(user=>user.id)].sort();
      await rosterTransitions('cpp_joined_transitions',sorted.map((_,index)=>sorted.slice(0,index+1)),0);
    }
    let leaveRosterSequence=await fullSnapshotQuery(initialRosterSequence);
    if(options.onEstablished){
      stage='established_extension';
      const extension=await invokeEstablishedHook(options.onEstablished,{users,api,command,pump,sourceName,sourceEnv,sourceSession,
        callId,conversationId,viewerURL:`http://127.0.0.1:${server.address().port}/`,startedBrowsers,browser,evaluated,
        snapshotMedia,activeMembers,phase,installSpectrum,validateEstablishedReplacement,rejoinMember,
        observeRecordingState:recordingEvents.subscribe});
      if(extension?.replacementUser){
        const replacement=extension.replacementUser,position=replacement.index-1;
        assert.ok([0,1].includes(position),'replacement_member_position_required');
        const original=users[position];
        validateEstablishedReplacement(original,replacement,startedBrowsers);
        users[position]=replacement;
      }
      report.establishedExtension={executed:true,replacedU1:extension?.replacementUser?.index===1,replacedU2:extension?.replacementUser?.index===2};
      if(extension?.rejoinedMember)leaveRosterSequence=await fullSnapshotQuery(leaveRosterSequence,{label:'roster_after_formal_member_rejoin'});
    }
    assert.equal(evaluated(await browser(sourceName,sourceEnv,'eval','__conferenceInvites.size')),1,'second_device_invitation_forbidden');
    stage='formal_mac_authorization';
    const original=await api(users[0],'im/interaction/talk/audio-access',macBody('read',callId,users[2].id));
    const forbidden=await api(users[1],'im/interaction/talk/audio-access',macBody('set',callId,users[2].id,false,original.audioRevision),false);
    assert.notEqual(forbidden.code,200,'non_initiator_control_forbidden');
    const revoked=await api(users[0],'im/interaction/talk/audio-access',macBody('set',callId,users[2].id,false,original.audioRevision));
    assert.equal(revoked.publishAllowed,false);assert.equal(revoked.memberEpoch,original.memberEpoch);assert.equal(revoked.audioRevision,original.audioRevision+1);
    const stale=await api(users[0],'im/interaction/talk/audio-access',macBody('set',callId,users[2].id,true,original.audioRevision),false);
    assert.notEqual(stale.code,200,'stale_cas_forbidden');
    await browser(users[2].name,users[2].env,'eval','(()=>{__capture.getAudioTracks()[0].enabled=true;return true;})()');
    // 原许可最多两秒；读取等待当前服务器MAC对后端原混音门生效，PCM用新窗验证而不是上行本地enabled。
    await waitFor(async()=>{const current=await api(users[2],'im/interaction/talk/audio-access',macBody('read',callId,users[2].id));return current.publishAllowed===false;});
    await new Promise(resolve=>setTimeout(resolve,2100));
    stage='revoked_u3_still_receives';await phase(stage,[0,1,2,3],[0,1,2]);
    if(config.rosterEnabled)await roster('cpp_mac_does_not_remove_member',users.map(user=>user.id));
    stage='u3_local_microphone_off';
    // 这是用户本地关闭采集轨，不注入媒体事实；保留原Peer和发送轨供主持人解禁后的实态检查。
    await browser(users[2].name,users[2].env,'eval','(()=>{const track=__capture.getAudioTracks()[0];if(!track)throw Error("capture_required");track.enabled=false;window.__conferenceLocalMute={peer:__ipcPeer,capture:__capture,track};return true;})()');
    stage='host_restores_permission_only';
    const restored=await api(users[0],'im/interaction/talk/audio-access',macBody('set',callId,users[2].id,true,revoked.audioRevision));
    const localMute=evaluated(await browser(users[2].name,users[2].env,'eval','(()=>{const saved=__conferenceLocalMute,tracks=__capture.getAudioTracks();return JSON.stringify({samePeer:__ipcPeer===saved.peer,connected:__ipcPeer.connectionState==="connected",sameCapture:__capture===saved.capture,sameTrack:tracks[0]===saved.track,senderBound:__ipcPeer.getSenders().some(sender=>sender.track===saved.track),trackCount:tracks.length,trackLive:tracks[0]?.readyState==="live",trackEnabled:tracks[0]?.enabled});})()'));
    verifyPermissionOnly(revoked,restored,localMute);
    await waitFor(async()=>{const current=await api(users[2],'im/interaction/talk/audio-access',macBody('read',callId,users[2].id));return current.publishAllowed===true&&current.audioRevision===restored.audioRevision;});
    await new Promise(resolve=>setTimeout(resolve,2100));
    stage='restored_permission_local_microphone_stays_off';await phase(stage,[0,1,2,3],[0,1,2]);
    if(config.rosterReconcileEnabled)leaveRosterSequence=await fullSnapshotQuery(leaveRosterSequence,{label:'roster_reconcile_preserves_local_mute',
      allowed:[0,1,2],quietIndices:[3],reconcile:true,locallyMuted:users[2],permissionBefore:revoked,permissionAfter:restored});
    stage='u3_explicit_local_unmute';
    await browser(users[2].name,users[2].env,'eval','(()=>{if(__capture.getAudioTracks()[0]!==__conferenceLocalMute.track)throw Error("original_track_required");__conferenceLocalMute.track.enabled=true;return true;})()');
    await phase('u3_self_unmutes_four_decoders_continue',[0,1,2,3],[0,1,2,3]);
    report.macPermissionOnly={restoredWithoutOpeningMicrophone:true,originalPeerAndTrackRetained:true,
      mutedSourceAbsentInActualMix:true,explicitLocalUnmuteActuallyDecoded:true};
    let u3Released;
    if(config.hostRemoveEnabled){
      stage='formal_host_remove_non_initiator';
      const removeParams={callId,participantId:users[2].id,expectedMemberEpoch:restored.memberEpoch};
      const removalToken=randomUUID(),originalRemoval={method:'interaction.leave',msgToken:removalToken,sessionId:users[0].session,params:removeParams};
      const denied=await api(users[1],'im/interaction/commands',{endpointId:users[1].endpoint,
        command:{...originalRemoval,msgToken:randomUUID(),sessionId:users[1].session}},false);
      assert.notEqual(denied.code,200,'non_initiator_remove_forbidden');
      stage='formal_host_remove_verify_original_members';
      await activeMembers(users.map(user=>user.id));
      stage='formal_host_remove_original_member';
      const result=await command(users[0],'interaction.leave',removeParams,removalToken);
      stage='formal_host_remove_verify_original_result';
      verifyGroupLeaveResult(result,originalRemoval,users.slice(0,2).map(user=>user.id));
      // 必须收到目标原Owner的真实终态再执行该端HAL停采集；不先end或冒充目标group_leave。
      stage='formal_host_remove_wait_target_terminal';
      await waitFor(async()=>{await pump(users[2]);return users[2].state==='ended';});
      assert.equal(verifySingleEndNotice(users[2].stateTimeline),true,'removed_member_terminal_not_unique');
      users[2].ended=true;
      stage='formal_host_remove_release_target';
      u3Released=await closeApp(users[2]); // 原终态立即驱动HAL关闭；历史结果重放不能延迟目标停止采集。
      stage='formal_host_remove_original_replay';
      const replayed=await command(users[0],'interaction.leave',removeParams,removalToken);
      assert.deepEqual(replayed,result,'original_remove_replay_changed');
      stage='formal_host_remove_verify_group_membership';
      const membership=await api(users[2],'im/conversation/get-members',{conversationId});
      assert.ok(Array.isArray(membership.list),'formal_group_members_required');
      assert.deepEqual(membership.list.filter(member=>member.participantType==='user').map(member=>safeUserID(member.participantId)).sort(),users.map(user=>user.id).sort(),'meeting_remove_must_not_leave_group');
      report.hostRemove={nonInitiatorRejected:true,originalResultMatched:true,originalResultReplayMatched:true,
        targetOriginalTerminalObserved:true,targetTerminalNotices:1,groupMembershipRetained:true};
    }else{
      stage='u3_original_end_and_leave';
      await command(users[2],'interaction.end',{reason:'userHangup'});users[2].ended=true;
      await api(users[2],'im/call/signal',{type:'group_leave',callId});
    }
    u3Released??=await closeApp(users[2]);await activeMembers(users.slice(0,2).map(user=>user.id));
    const leftRosterSequence=config.rosterEnabled?await roster('cpp_original_member_left',users.slice(0,2).map(user=>user.id),leaveRosterSequence??0):null;
    if(config.rosterEnabled){
      // 离开投影逐条断言：离开后每个新水位都必须是精确剩余集合。
      await rosterTransitions('cpp_leave_transitions',[users.slice(0,2).map(user=>user.id).sort()],leaveRosterSequence??0);
      // 场景文档M45的trackState(reason=participantLeft)属待实现的独立成员轨契约：负判据取证
      // "无成员轨、无ipcTrack下行、无回执上行"，出现任何一项即越界。
      const contract=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.wire(${JSON.stringify(sourceSession)}))`));
      verifyParticipantTrackContract(contract);
      report.roster.trackContract={...contract,participantLeftReceipt:'trackState(reason=participantLeft) not implemented'};
    }
    const continuing=await phase('remaining_device_u1_u2_continue',[0,1,2],[0,1,2]);
    // U3离开动作不得让剩余端媒体重建：离开前最后四端阶段的前三个端计数只增不减。
    {
      const beforeLeave=report.phases.find(item=>item.label==='revoked_u3_still_receives')
        .decodedPCM.filter(row=>row.endpoint<3).map(row=>({received:row.received,sent:row.sent,samples:row.samples}));
      verifyDecodeNeverReset(beforeLeave,continuing);
    }
    if(config.rosterReconcileEnabled)await fullSnapshotQuery(leftRosterSequence,{label:'roster_reconcile_after_host_remove',
      expected:users.slice(0,2).map(user=>user.id),indices:[0,1,2],allowed:[0,1,2],reconcile:true});
    assert.equal(evaluated(await browser(sourceName,sourceEnv,'eval','__conferenceInvites.size')),1);
    stage='below_two_im_users_ends_call';
    // 最后一次人数终止之前先等待续租退出，不能把预期终态后的心跳拒绝混成随机测试故障。
    heartbeatsStopped=true;heartbeatsStoppedAt=Date.now();clearTimeout(heartbeat);await bounded(heartbeatInFlight,20000,'heartbeat_stop_timeout');
    if(config.hostRemoveEnabled){
      const access=await api(users[1],'im/interaction/talk/audio-access',macBody('read',callId,users[1].id));
      const leaveParams={callId,participantId:users[1].id,expectedMemberEpoch:access.memberEpoch},token=randomUUID();
      const reply=await command(users[1],'interaction.leave',leaveParams,token);
      verifyGroupLeaveResult(reply,{method:'interaction.leave',msgToken:token,sessionId:users[1].session,params:leaveParams},[users[0].id],true);
      await waitFor(async()=>{await pump(users[1]);return users[1].state==='ended';});users[1].ended=true;
      report.hostRemove.selfLeaveEndedOriginalCall=true;
      // 新结束后旧端清理仍合法；后续原卡片唯一断言必须覆盖旧新混用，不仅验证纯新路径。
      await api(users[1],'im/call/signal',{type:'group_leave',callId});
      report.hostRemove.legacyCleanupAfterNewEnd=true;
    }else{
      await command(users[1],'interaction.end',{reason:'userHangup'});users[1].ended=true;
      await api(users[1],'im/call/signal',{type:'group_leave',callId});
    }
    const u2Released=await closeApp(users[1]);await activeMembers([]);
    await waitFor(async()=>{await pump(users[0]);return users[0].state==='ended';});
    // 人数边界取证：结束卡片恰好一次；主动结束必须发生在心跳过期清扫窗口（60s扫描+30s轮询）之前，
    // 否则终态可能来自清扫而非U2离开，判据不成立。
    assert.equal(verifySingleEndNotice(users[0].stateTimeline),true,'end_notice_not_unique');
    assert.ok(Date.now()-heartbeatsStoppedAt<60000,'ended_must_precede_heartbeat_sweep');
    assert.ok(heartbeatCount>0,'heartbeat_renewal_not_observed');
    report.memberBoundary={u2LeaveEndedCall:true,endNotices:1,heartbeatCount,
      heartbeatStoppedAt:heartbeatsStoppedAt,callEndedAt:Date.now(),sweepWindowMs:Date.now()-heartbeatsStoppedAt};
    await waitFor(async()=>evaluated(await browser(sourceName,sourceEnv,'eval','views.size === 0 && invitations.size === 0'))===true);
    if(config.rosterEnabled){
      const released=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.read(${JSON.stringify(sourceSession)},[]))`));
      assert.deepEqual(released,{enabled:true,available:false,consumerApplied:false,eventSeq:null,memberCount:0,
        expectedCount:0,exactMembers:false,trackCount:0,truncated:false,closed:true},'cpp_roster_not_released');
      report.roster.released=true; // 原UI腿已删除，不能把旧观察缓存当结束后的完整名册。
      if(config.rosterSyncEnabled){
        const snapshotReleased=evaluated(await browser(sourceName,sourceEnv,'eval',`JSON.stringify(window.__conferenceRosterObservation.readSnapshot(${JSON.stringify(sourceSession)}))`));
        assert.deepEqual(snapshotReleased,{enabled:true,available:false,consumerApplied:false,state:'missing',pendingObserved:false,eventSeq:null,closed:true,truncated:false},'cpp_snapshot_observer_not_released');
        report.roster.queryReleased=true;
      }
    }
    const deviceReleased=evaluated(await browser(sourceName,sourceEnv,'eval',`(async()=>{await __conferenceSpectrum.stop();const r=__conferenceSourceResources;return JSON.stringify({peerClosed:r.peer.connectionState==='closed',
      captureStopped:r.capture.every(t=>t.readyState==='ended'),remoteStopped:r.remote.readyState==='ended',sinkReleased:r.sink.srcObject===null&&r.sink.paused,
      spectrumClosed:__conferenceSpectrum.context.state==='closed'});})()`));
    assert.deepEqual(deviceReleased,{peerClosed:true,captureStopped:true,remoteStopped:true,sinkReleased:true,spectrumClosed:true});
    report.released={device:deviceReleased,users:[await closeApp(users[0]),u2Released,u3Released]};
    assert.equal(heartbeatFailed,false,'original_heartbeat_failed');report.deviceInvitations=1;report.formalMACVerified=true;
    passed=true;
  } catch(error) {
    // 清理之前并行读取最多四端实际状态；失败仅留下固定原因，不反射浏览器、私有身份或SDP。
    const mediaDiagnostics=await Promise.all([0,1,2,3].map(async index=>{
      const endpoint=index===0?{name:sourceName,env:sourceEnv}:users[index-1];
      if(!startedBrowsers.has(endpoint.name))return{endpoint:index,diagnosticOnly:true,unavailable:'browser_not_started'};
      try{
        const peer=index===0?`views.get(${JSON.stringify(sourceSession)})?.peer`:'window.__ipcPeer';
        const capture=index===0?`views.get(${JSON.stringify(sourceSession)})?.stream`:'window.__capture';
        const facts=evaluated(await bounded(browser(endpoint.name,endpoint.env,'eval',`(async()=>JSON.stringify(await (${collectMediaDiagnostic.toString()})(${peer},window.__conferenceSpectrum,${capture})))()`),4000,'diagnostic_read_timeout'));
        return mediaDiagnostic(index,facts);
      }catch{return{endpoint:index,diagnosticOnly:true,unavailable:'actual_observation_unavailable'};}
    }));
    let rosterDiagnostics=null; // 仅原消费者固定布尔／计数／枚举，不能把失败快照变成查询成功或反射成员身份。
    if(config.rosterSyncEnabled&&sourceSession&&startedBrowsers.has(sourceName))try{
      rosterDiagnostics=evaluated(await bounded(browser(sourceName,sourceEnv,'eval',`JSON.stringify({
        query:window.__conferenceRosterObservation.readSnapshot(${JSON.stringify(sourceSession)}),
        roster:window.__conferenceRosterObservation.read(${JSON.stringify(sourceSession)},${JSON.stringify(users.map(user=>user.id))})})`),4000,'roster_diagnostic_timeout'));
    }catch{}
    console.log(JSON.stringify({result:'fail',stage,deviceExited:exited,completedPhases:report.phases,mediaDiagnostics,rosterDiagnostics,
      queryDiagnostic:report.queryDiagnostic??null,formalFailure:formalFailureDiagnostic(error),
      networkRecoveryDiagnostic:error.networkRecoveryDiagnostic??null,
      handoffDiagnostic:require('./endpoint_handoff_member_live.cjs').handoffFailureDiagnostic(error),
      captureSettings:report.captureSettings??null,sourceCapture:report.sourceCapture??null,
      sourcePage:report.sourcePage??null,
      reason:/^[a-z_]+$/.test(error.message)?error.message:'assertion_or_execution_failed'}));process.exitCode=1;
  } finally {
    heartbeatsStopped=true;clearTimeout(heartbeat);clearTimeout(processDeadline);
    const cleanupFailures=[],cleanupObservations=[];
    // cleanup 只收录白名单步骤，不输出用户、自由服务错误或浏览器内容。
    const cleanup=async(label,work,user)=>{try{await work();}catch(error){
      cleanupFailures.push(label);
      if(label==='original_end')cleanupObservations.push({operation:label,
        code:Number.isSafeInteger(error.actualCode)?error.actualCode:null,
        state:['established','negotiating','ended'].includes(user?.state)?user.state:null});
    }};
    await cleanup('heartbeat',()=>bounded(heartbeatInFlight,20000,'heartbeat_stop_timeout'));
    for(const user of users) {
      await cleanup('original_end',async()=>{if(await cleanupEndRequired(user,pump))await command(user,'interaction.end',{reason:'userStopped'});},user);
      if(callId)await cleanup('original_leave',async()=>{
        const active=await api(user,`im/call/active?conversationId=${conversationId}`,{},true,'GET');
        if(active.active&&active.callId===callId&&(active.participantIds||[]).includes(user.id))await api(user,'im/call/signal',{type:'group_leave',callId});
      });
      if(startedBrowsers.has(user.name)){
        await cleanup('app_media',()=>browser(user.name,user.env,'eval','(async()=>{if(window.__conferenceSpectrum)await __conferenceSpectrum.stop();if(window.__ipcAudio)await __ipcAudio.stop();window.__capture?.getTracks().forEach(t=>t.stop());window.__remoteTrack?.stop();window.__ipcPeer?.close();return true;})()'));
        await cleanup('app_browser',()=>browser(user.name,user.env,'close'));
      }
    }
    if(conversationId)await cleanup('created_conversation',()=>api(users[0],'im/conversation/disband',{conversationId}));
    if(startedBrowsers.has(sourceName)) {
      if(sourceResourcesCaptured)await cleanup('source_observer',async()=>{
        const closed=evaluated(await browser(sourceName,sourceEnv,'eval','(async()=>{await __conferenceSpectrum.stop();await __conferenceCaptureSpectrum.stop();return __conferenceSpectrum.context.state==="closed"&&__conferenceCaptureSpectrum.context.state==="closed";})()'));
        assert.equal(closed,true,'actual_source_observers_not_closed');
      });
      await cleanup('source_browser',()=>browser(sourceName,sourceEnv,'close'));
    }
    await cleanup('device_process',()=>stopChild(child,completion,()=>exited));
    if(server.listening)await cleanup('viewer_server',()=>bounded(new Promise((resolve,reject)=>server.close(error=>error?reject(error):resolve())),5000,'server_close_timeout'));
    if(cleanupFailures.length){passed=false;process.exitCode=1;console.log(JSON.stringify({result:'fail',stage:'cleanup',failures:cleanupFailures,observations:cleanupObservations}));}
  }
  if(passed)console.log(JSON.stringify({result:'pass',...report}));
  // 调用者必须取得清理后的最终事实；不得根据提前打印的阶段日志认定整个扩展通过。
  return {...report,result:passed?'pass':'fail'};
}

module.exports={run,FREQUENCIES,safeUserID,authenticatedUser,readBoundedJSON,formalFailureDiagnostic,invokeEstablishedHook,createRecordingEventRelay,rejoinConferenceMember,validateEstablishedReplacement,bounded,stopChild,validateConfiguration,fixtureArguments,spectralPeaks,verifyMix,macBody,verifyPermissionOnly,verifyGroupLeaveResult,mediaDiagnostic,connectivityDiagnostic,collectMediaDiagnostic,verifyFixedPCMSettings,cleanupEndRequired,sourcePageReady,installRosterObserver,verifyRosterObservation,verifySnapshotObservation,verifySnapshotContinuity,verifyRosterReconcileAuthority,verifyRosterTransitions,verifyParticipantTrackContract,verifyDecodeNeverReset,verifySingleEndNotice};
if(require.main===module)run().catch(()=>{console.log(JSON.stringify({result:'fail',stage:'configuration'}));process.exitCode=1;});
