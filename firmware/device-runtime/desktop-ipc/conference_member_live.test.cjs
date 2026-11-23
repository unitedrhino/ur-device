// 文件说明：会议真实E2E入口的确定性配置/频谱/CAS契约测试；不替代C++、真实账户或媒体端云结果。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {FREQUENCIES,safeUserID,authenticatedUser,readBoundedJSON,bounded,stopChild,validateConfiguration,fixtureArguments,spectralPeaks,verifyMix,macBody,mediaDiagnostic,verifyFixedPCMSettings,cleanupEndRequired,sourcePageReady,installRosterObserver,verifyRosterObservation,verifySnapshotObservation,verifySnapshotContinuity,verifyRosterTransitions,verifyParticipantTrackContract,verifyDecodeNeverReset,verifySingleEndNotice}=require('./conference_member_live.cjs');
const {verifyPermissionOnly}=require('./conference_member_live.cjs'); // 解禁许可与本地采集实态分开断言。
const {verifyGroupLeaveResult}=require('./conference_member_live.cjs'); // 原主持操作结果与终端停流判据分别核验。
const {verifyRosterReconcileAuthority}=require('./conference_member_live.cjs'); // 权威重取不更新原IM实例、MAC许可或修订。
const {formalFailureDiagnostic}=require('./conference_member_live.cjs'); // 失败仅保留固定HTTP状态和数字业务码，不输出自由正文。
const {invokeEstablishedHook}=require('./conference_member_live.cjs'); // 扩展需协作取消并等原finally退出，不能遗留后台提交。
const {validateEstablishedReplacement}=require('./conference_member_live.cjs'); // 接续仅替换同U2新端元数据，不把其他身份或终态端接管为活腿。
const {rejoinConferenceMember}=require('./conference_member_live.cjs'); // 正式重入的本地诊断不输出远端错误正文。
const {createRecordingEventRelay}=require('./conference_member_live.cjs'); // 同一原事件在ACK前观察，默认无订阅时旧行为不变。

test('录制观察仅覆盖原状态事件，默认无订阅及退订后不改变普通会议行为',()=>{
  const relay=createRecordingEventRelay(),seen=[],user={id:'synthetic-user'};
  const event={method:'interaction.controlState',data:{command:'recordingState'}};
  relay.observe(user,event);const remove=relay.subscribe((actualUser,actualEvent)=>seen.push([actualUser,actualEvent]));
  relay.observe(user,{method:'interaction.result'});relay.observe(user,{method:'interaction.controlState',data:{command:'other'}});
  relay.observe(user,event);assert.equal(seen.length,1);assert.equal(seen[0][0],user);assert.equal(seen[0][1],event);
  assert.throws(()=>relay.subscribe(()=>{}),/recording_observer_already_registered/);
  remove();remove();relay.observe(user,event);assert.equal(seen.length,1);
  const second=relay.subscribe(()=>seen.push('second'));remove();relay.observe(user,event);assert.equal(seen.length,2);second();
});
const {collectMediaDiagnostic,connectivityDiagnostic}=require('./conference_member_live.cjs'); // 连通性只留原实际状态及计数，不泄露候选或地址。

test('正式重入保留首个失败对象并标记固定本地阶段，正常路径不改变原协议顺序',async()=>{
  const names=['end','terminal','close','leave','remaining','join','open','negotiate','members'];
  const expected=['originalAccess','endCommand','waitTerminal','closeMedia','groupLeave','remainingMembers','groupJoin','renewedAccess','newSession','newMedia','activeMembers'];
  for(const failedAt of [...expected,null]){
    const user={index:3,session:'old',state:'established',ended:false},original=Error('PRIVATE-API-DETAILS');
    let access=0;const calls=[];
    const ports={access:async()=>{const phase=++access===1?'originalAccess':'renewedAccess';calls.push(phase);if(phase===failedAt)throw original;return {memberEpoch:access};}};
    for(const [index,name] of names.entries())ports[name]=async()=>{
      const phase=expected[[1,2,3,4,5,6,8,9,10][index]];
      calls.push(phase);
      if(phase===failedAt)throw original;
      if(name==='terminal')user.state='ended';
      if(name==='close')return {peerClosed:true,captureStopped:true,remoteStopped:true};
      if(name==='open')user.session='new';
      if(name==='negotiate')user.state='established';
    };
    if(failedAt===null){
      assert.deepEqual(await rejoinConferenceMember(user,ports),{oldMediaReleased:true,originalMemberEpoch:1,currentMemberEpoch:2,newSession:true});
      assert.deepEqual(calls,expected);assert.equal(user.ended,false);continue;
    }
    await assert.rejects(()=>rejoinConferenceMember(user,ports),error=>{
      assert.equal(error,original);assert.deepEqual(error.memberRejoinDiagnostic,{phase:failedAt});
      assert.equal(Object.keys(error).includes('memberRejoinDiagnostic'),false);return true;
    });
  }
});

// 直接执行正式run尾部，确保编排者收到清理之后的事实，不从console日志伪造返回值。
test('会议驱动在完整清理后返回原通过或失败事实',()=>{
  const source=fs.readFileSync(path.join(__dirname,'conference_member_live.cjs'),'utf8');
  const tail=source.slice(source.lastIndexOf("  if(passed)console.log"),source.indexOf('\nmodule.exports='));
  for(const passed of [true,false]){
    const result=vm.runInNewContext('(function(){'+tail.slice(0,tail.lastIndexOf('}'))+'})()',
      {passed,report:{actualEndpoints:4},console:{log(){}}});
    assert.equal(result?.result,passed?'pass':'fail'); assert.equal(result.actualEndpoints,4);
  }
});

test('会议扩展仅接管同U2完整新端，要求原端唯一终态及新端已建立',()=>{
  const env={},original={index:2,id:'102',token:'fixture-token',env,name:'old-browser',endpoint:'old-endpoint',session:'old-session',state:'ended',ended:true,stateTimeline:['established','ended']};
  const next={...original,name:'new-browser',endpoint:'new-endpoint',session:'new-session',state:'established',ended:false,events:[],stateTimeline:['negotiating','established']};
  const browsers=new Set([next.name]);
  assert.equal(validateEstablishedReplacement(original,next,browsers),true);
  for(const change of [{id:'103'},{index:1},{token:'other'},{env:{}},{name:original.name},{endpoint:original.endpoint},{session:original.session},{state:'ended'},{ended:true},{events:Array(129).fill(null)},{stateTimeline:['ended']}, {stateTimeline:[]}])
    assert.throws(()=>validateEstablishedReplacement(original,{...next,...change},browsers));
  for(const change of [{ended:false},{state:'established'},{stateTimeline:['established']},{stateTimeline:['ended','ended']}])
    assert.throws(()=>validateEstablishedReplacement({...original,...change},next,browsers));
  assert.throws(()=>validateEstablishedReplacement(original,next,new Set()));
});

test('会议默认无扩展，原context不被注入额外计时或控制副作用',async()=>{
  assert.equal(await invokeEstablishedHook(undefined,{unchanged:true}),undefined);
  await assert.rejects(()=>invokeEstablishedHook(null,{}));
});

test('会议扩展收到同一context和独立取消信号，正常完成保留原返回',async()=>{
  const users=[],result={completed:true};
  assert.equal(await invokeEstablishedHook(async context=>{
    assert.equal(context.users,users);assert.equal(context.signal.aborted,false);return result;
  },{users}),result);
});

test('会议扩展API和控制默认继承取消信号，finally可以显式传独立有限清理信号',async()=>{
  const calls=[],context={api:(...args)=>calls.push(['api',args.at(-1)]),command:(...args)=>calls.push(['command',args.at(-1)]),pump:(...args)=>calls.push(['pump',args.at(-1)])};
  await invokeEstablishedHook(async value=>{
    const cleanup=AbortSignal.timeout(1000);
    value.api({},'fixed',{});value.command({},'fixed',{});value.pump({});value.api({},'fixed',{},true,'POST',cleanup);
    assert.deepEqual(calls,[['api',value.signal],['command',value.signal],['pump',value.signal],['api',cleanup]]);
  },context);
});

test('会议扩展超时先abort并等待原finally清理，不以Promise.race让提交留在后台',async()=>{
  let drained=false;
  await assert.rejects(()=>invokeEstablishedHook(async context=>{
    try{await new Promise((resolve,reject)=>context.signal.addEventListener('abort',()=>reject(context.signal.reason),{once:true}));}
    finally{await new Promise(resolve=>setTimeout(resolve,10));drained=true;}
  },{},5),/established_hook_timeout/);
  assert.equal(drained,true);
});

test('会议扩展忽略abort但随后返回时仍先等实际退出并判超时',async()=>{
  let exited=false;
  await assert.rejects(()=>invokeEstablishedHook(async()=>{
    await new Promise(resolve=>setTimeout(resolve,15));exited=true;return {completed:true};
  },{},5),/established_hook_timeout/);
  assert.equal(exited,true);
});

test('正式接口失败诊断保留安全状态和错误码，拒绝正文身份及非数值伪码',()=>{
  assert.deepEqual(formalFailureDiagnostic({httpStatus:200,actualCode:100017,message:'secret user identity',body:{token:'secret'}}),{httpStatus:200,code:100017});
  for(const value of [-1,1.5,'100017',null,Number.MAX_SAFE_INTEGER+1])
    assert.deepEqual(formalFailureDiagnostic({httpStatus:503,actualCode:value}),{httpStatus:503,code:null});
  assert.deepEqual(formalFailureDiagnostic({httpStatus:999,actualCode:409}),{httpStatus:null,code:409});
  assert.deepEqual(formalFailureDiagnostic(null),{httpStatus:null,code:null});
});

test('名册权威重取必须显式打开且同时启用主持移出与完整同步',()=>{
  const env={...configuration(),UR_CONFERENCE_HOST_REMOVE:'1',UR_IPC_PARTICIPANT_ROSTER:'1',UR_IPC_PARTICIPANT_ROSTER_SYNC:'1'};
  assert.equal(validateConfiguration(env,()=>({isFile:()=>true})).rosterReconcileEnabled,false);
  assert.equal(validateConfiguration({...env,UR_CONFERENCE_ROSTER_RECONCILE:'1'},()=>({isFile:()=>true})).rosterReconcileEnabled,true);
  for(const key of ['UR_CONFERENCE_HOST_REMOVE','UR_IPC_PARTICIPANT_ROSTER','UR_IPC_PARTICIPANT_ROSTER_SYNC'])
    assert.throws(()=>validateConfiguration({...env,[key]:'0',UR_CONFERENCE_ROSTER_RECONCILE:'1'},()=>({isFile:()=>true})));
  for(const value of ['true','',null])assert.throws(()=>validateConfiguration({...env,UR_CONFERENCE_ROSTER_RECONCILE:value},()=>({isFile:()=>true})));
});

test('名册权威重取前后必须保持所有活成员实例和MAC许可，不以同人数替代同实例',()=>{
  const before=[{memberEpoch:1,audioRevision:2,publishAllowed:true},{memberEpoch:3,audioRevision:4,publishAllowed:false}];
  assert.equal(verifyRosterReconcileAuthority(before,structuredClone(before)),true);
  for(const change of [{memberEpoch:2},{audioRevision:5},{publishAllowed:false},{memberEpoch:0},{audioRevision:1.5}])
    assert.throws(()=>verifyRosterReconcileAuthority(before,[{...before[0],...change},before[1]]));
  for(const bad of [[],[before[0]],null,[...before,before[0],before[0]],before.map(row=>({...row,unexpected:true}))])
    assert.throws(()=>verifyRosterReconcileAuthority(before,bad));
});

test('移出后完整名册三原Peer保持解码，用户本地静音允许原上行包数不变但不能恢复采集',()=>{
  const row={samePeer:true,connected:true,received:10,sent:10,samples:100};
  const before=Array.from({length:3},()=>({...row}));
  const after=before.map((value,index)=>({...value,received:11,samples:200,sent:index===2?10:11}));
  assert.equal(verifySnapshotContinuity(before,after,3,[2]),true);
  assert.throws(()=>verifySnapshotContinuity(before,after,3));
  assert.throws(()=>verifySnapshotContinuity(before,after.map(value=>({...value,samples:100})),3,[2]));
  assert.throws(()=>verifySnapshotContinuity(before,after,3,[3]));
});

test('主持移出仅显式开启且必须观察原C++成员名册',()=>{
  const env=configuration();
  assert.equal(validateConfiguration(env,()=>({isFile:()=>true})).hostRemoveEnabled,false);
  assert.throws(()=>validateConfiguration({...env,UR_CONFERENCE_HOST_REMOVE:'1'},()=>({isFile:()=>true})));
  assert.equal(validateConfiguration({...env,UR_CONFERENCE_HOST_REMOVE:'1',UR_IPC_PARTICIPANT_ROSTER:'1'},()=>({isFile:()=>true})).hostRemoveEnabled,true);
  for(const value of ['true','',null])assert.throws(()=>validateConfiguration({...env,UR_CONFERENCE_HOST_REMOVE:value},()=>({isFile:()=>true})));
});

test('主持移出结果必须原token、原会议、原参与者、原实例与精确剩余名单',()=>{
  const command={method:'interaction.leave',msgToken:'original',sessionId:'host-leg',params:{callId:'call',participantId:'103',expectedMemberEpoch:2}};
  const result={method:'interaction.result',msgToken:'original',sessionId:'host-leg',code:200,
    data:{operation:'interaction.leave',callId:'call',participantId:'103',memberEpoch:2,remaining:['101','102'],ended:false}};
  assert.equal(verifyGroupLeaveResult(result,command,['102','101']),true);
  assert.equal(verifyGroupLeaveResult({...result,data:{...result.data,remaining:['101'],ended:true}},command,['101'],true),true);
  for(const mutation of [{method:'interaction.state'},{msgToken:'other'},{sessionId:'target-leg'},{code:409}])
    assert.throws(()=>verifyGroupLeaveResult({...result,...mutation},command,['101','102']));
  for(const mutation of [{operation:'interaction.end'},{callId:'other'},{participantId:'102'},{memberEpoch:3},
    {remaining:['101']},{remaining:['101','101']},{remaining:['101','0102']},{ended:true}])
    assert.throws(()=>verifyGroupLeaveResult({...result,data:{...result.data,...mutation}},command,['101','102']));
});

test('主持人解除禁麦只恢复许可，不开启成员本地麦克风或重建媒体腿',()=>{
  const before={publishAllowed:false,audioRevision:2,memberEpoch:1};
  const after={publishAllowed:true,audioRevision:3,memberEpoch:1};
  const media={samePeer:true,connected:true,sameCapture:true,sameTrack:true,senderBound:true,
    trackCount:1,trackLive:true,trackEnabled:false};
  assert.equal(verifyPermissionOnly(before,after,media),true);
  for(const change of [{publishAllowed:false},{audioRevision:2},{audioRevision:4},{memberEpoch:2}])
    assert.throws(()=>verifyPermissionOnly(before,{...after,...change},media));
  for(const change of [{samePeer:false},{connected:false},{sameCapture:false},{sameTrack:false},
    {senderBound:false},{trackCount:0},{trackCount:2},{trackLive:false},{trackEnabled:true}])
    assert.throws(()=>verifyPermissionOnly(before,after,{...media,...change}));
  assert.throws(()=>verifyPermissionOnly({...before,publishAllowed:true},after,media));
  assert.throws(()=>verifyPermissionOnly({...before,memberEpoch:0},{...after,memberEpoch:0},media));
});

// configuration 仅供离线验证的显式占位，不是真实可连接凭据。
function configuration() {
  return {UR_CONFERENCE_MEMBER_LIVE:'1',UR_CONFERENCE_FIXED_PCM_TEST:'1',UR_IPC_BASE_URL:'http://127.0.0.1:19001',UR_IPC_DEVICE_PORT:'19002',
    UR_IPC_CONFERENCE_TALK:'1',UR_IPC_MICROPHONE:'1',UR_DEVICE_PRODUCT:'example',UR_DEVICE_NAME:'source',UR_DEVICE_SECRET:'placeholder',
    UR_IPC_TENANT:'example',UR_IPC_APP_ID:'100',UR_IPC_CAMERA_ID:'declared-camera',UR_CONFERENCE_CAMERA_ID:'declared-camera',UR_IPC_APP_TOKEN:'placeholder-one',
    UR_CONFERENCE_U2_TOKEN:'placeholder-two',UR_CONFERENCE_U3_TOKEN:'placeholder-three',UR_IPC_DEVICE_BIN:'/placeholder/device_ipc',
    UR_IPC_AUDIO_FIXTURE:'/placeholder/d.wav',UR_CONFERENCE_U1_WAV:'/placeholder/u1.wav',UR_CONFERENCE_U2_WAV:'/placeholder/u2.wav',UR_CONFERENCE_U3_WAV:'/placeholder/u3.wav'};
}

test('显式四端配置不得降级为同主人多APP或共享音调',()=>{
  const env=configuration();assert.equal(validateConfiguration(env,()=>({isFile:()=>true})).port,19002);
  for(const [key,value] of [['UR_CONFERENCE_MEMBER_LIVE','0'],['UR_CONFERENCE_U2_TOKEN',env.UR_IPC_APP_TOKEN],
    ['UR_CONFERENCE_U3_WAV',env.UR_CONFERENCE_U2_WAV],['UR_IPC_CONFERENCE_TALK','0'],['UR_CONFERENCE_CAMERA_ID',''],
    ['UR_IPC_CAMERA_ID','other-camera'],['UR_IPC_DEVICE_PORT','0'],['UR_IPC_BASE_URL','http://127.0.0.1:19001/path']])
    assert.throws(()=>validateConfiguration({...env,[key]:value},()=>({isFile:()=>true})));
  assert.throws(()=>validateConfiguration(env,()=>({isFile:()=>false})));
});

test('纯音会议验收必须显式确认固定PCM软件驱动模式',()=>{
  for(const value of [undefined,'0','true'])assert.throws(()=>validateConfiguration({...configuration(),UR_CONFERENCE_FIXED_PCM_TEST:value},()=>({isFile:()=>true})));
});

test('固定PCM核验必须来自真实三false，未知或只部分关闭均失败',()=>{
  const settings={echoCancellation:false,noiseSuppression:false,autoGainControl:false};
  assert.equal(verifyFixedPCMSettings([settings]),true);
  for(const bad of [[],[settings,settings],[{}],[{...settings,echoCancellation:true}],
    [{...settings,noiseSuppression:null}],[{...settings,autoGainControl:undefined}]])assert.throws(()=>verifyFixedPCMSettings(bad));
});

test('源404或JS未执行用固定类型投影区分，不能误报登录或麦克风阶段',()=>{
  const good={incoming:true,available:true,activate:true,softwareFixedPCM:true};
  assert.equal(sourcePageReady(good),true);
  assert.equal(sourcePageReady({incoming:false,available:false,activate:false,softwareFixedPCM:false}),false);
  assert.equal(sourcePageReady({...good,softwareFixedPCM:false}),false);
  for(const bad of [null,{...good,token:'PRIVATE'},{...good,incoming:'function'},{incoming:true}])assert.throws(()=>sourcePageReady(bad));
});

test('结束清理先读原会话权威终态，缓存建立不能发新token结束旧已终结实例',async()=>{
  const user={session:'original',state:'established',ended:false};let reads=0;
  assert.equal(await cleanupEndRequired(user,async current=>{reads++;current.state='ended';}),false);
  assert.equal(reads,1);
});

test('结束清理未知或未终结仍需原end，读取失败不能伪装清理通过',async()=>{
  const user={session:'original',state:'established',ended:false};
  assert.equal(await cleanupEndRequired(user,async()=>{}),true);
  await assert.rejects(cleanupEndRequired(user,async()=>{throw Error('authority_unavailable');}),/authority_unavailable/);
  assert.equal(await cleanupEndRequired({...user,ended:true},async()=>{throw Error('unexpected_read');}),false);
});

test('身份只能从真实self安全规范整数取得',()=>{
  assert.equal(safeUserID(101),'101');assert.equal(safeUserID('9007199254740991'),'9007199254740991');
  for(const bad of [0,-1,1.1,9007199254740992,'0101','+101','9007199254740992','',null,{},'device:x'])assert.throws(()=>safeUserID(bad));
});

test('四夹具为48k单声道不同频率且无摄像头替代',()=>{
  const inputs=[];for(let index=0;index<4;index++){
    const args=fixtureArguments(index,`/placeholder/${index}.wav`);assert.ok(args.includes('pcm_s16le'));inputs.push(args[args.indexOf('-i')+1]);
  }assert.equal(new Set(inputs).size,4);assert.throws(()=>fixtureArguments(4,'x'));
});

test('实际PCM频谱能区分四音调且静音全流不能通过',()=>{
  const samples=Float32Array.from({length:4096},(_,index)=>FREQUENCIES.slice(1).reduce((sum,f)=>sum+0.05*Math.sin(2*Math.PI*f*index/48000),0));
  const peaks=spectralPeaks(samples,48000,FREQUENCIES);assert.ok(peaks[0]<0.0001);for(const amplitude of peaks.slice(1))assert.ok(amplitude>0.045&&amplitude<0.055);
  assert.equal(verifyMix({observations:8,peaks},0,[0,1,2,3]),true);
  assert.throws(()=>verifyMix({observations:8,peaks:[0,0,0,0]},0,[0,1,2,3]));
});

test('自身音调与已禁麦来源不得被解释为成功混音',()=>{
  assert.throws(()=>verifyMix({observations:8,peaks:[0.02,0.05,0.05,0.05]},0,[0,1,2,3]));
  assert.throws(()=>verifyMix({observations:8,peaks:[0,0.05,0.05,0.02]},0,[0,1,2]));
  assert.equal(verifyMix({observations:8,peaks:[0.05,0.05,0.05,0]},3,[0,1,2]),true,'U3禁麦仍实际接收其他3源');
  assert.throws(()=>verifyMix({observations:3,peaks:[0,0.05,0.05,0.05]},0,[0,1,2,3]));
});

test('MAC固定read3/set5契约不接自由participant或客户端epoch',()=>{
  assert.deepEqual(macBody('read','call-1','103'),{operation:'read',callId:'call-1',targetUserID:'103'});
  assert.deepEqual(macBody('set','call-1','103',false,2),{operation:'set',callId:'call-1',targetUserID:'103',allowed:false,expectedRevision:2});
  for(const revision of [0,-1,1.5,9007199254740992,'1'])assert.throws(()=>macBody('set','call-1','103',false,revision));
  assert.throws(()=>macBody('set','call-1','103',undefined,2));
});

test('self真实同企业启用成员关系不可由token不同替代',()=>{
  const current={userID:'101',tenants:[{tenantCode:'example',userID:'101',status:1}]};assert.equal(authenticatedUser(current,'example'),'101');
  for(const bad of [{userID:'101'}, {...current,tenants:[]}, {...current,tenants:[{tenantCode:'other',userID:'101',status:1}]},
    {...current,tenants:[{tenantCode:'example',userID:'101',status:2}]}, {...current,tenants:[...current.tenants,...current.tenants]}])
    assert.throws(()=>authenticatedUser(bad,'example'));
});

test('真实HTTP逐流预算拒绝redirect和超限并取消原流',async()=>{
  assert.deepEqual(await readBoundedJSON(new Response('{"code":200,"data":{}}')),{code:200,data:{}});
  await assert.rejects(readBoundedJSON(new Response('',{status:302,headers:{location:'https://example.invalid'}})));
  await assert.rejects(readBoundedJSON(new Response('{}',{headers:{'content-length':'1048577'}})));
  let cancelled=false;
  const stream=new ReadableStream({start(controller){controller.enqueue(new Uint8Array(1048577));},cancel(){cancelled=true;}});
  await assert.rejects(readBoundedJSON(new Response(stream)));assert.equal(cancelled,true);
});

test('精确child TERM失败升级KILL且等待实际close，不无限等或忽略失败',async()=>{
  let exited=false,resolve;const completion=new Promise(done=>resolve=done),signals=[];
  const child={kill(signal){signals.push(signal);if(signal==='SIGKILL'){exited=true;resolve();}}};
  await stopChild(child,completion,()=>exited,10,10);assert.deepEqual(signals,['SIGTERM','SIGKILL']);
  await assert.rejects(stopChild({kill(){}},new Promise(()=>{}),()=>false,5,5),/device_kill_timeout/);
});

test('cleanup有限截止保持原错误并不把在途heartbeatPromise当退出',async()=>{
  await assert.rejects(bounded(new Promise(()=>{}),5,'heartbeat_stop_timeout'),/heartbeat_stop_timeout/);
  await assert.rejects(bounded(Promise.reject(Error('explicit_cleanup_failed')),20,'timeout'),/explicit_cleanup_failed/);
});

test('失败媒体统计只输出实际窗口与原RTC计数白名单，不输出身份和自由正文',()=>{
  const facts={observations:8,nonSilentWindows:6,spectralPeaks:[0,.05,.04,.06],maxRMS:.08,
    receivedPackets:10,sentPackets:11,decodedSamples:3840,decodedFrames:null,peerState:'connected',contextState:'running',
    captureTracks:[{readyState:'live',enabled:true,muted:false,echoCancellation:true,noiseSuppression:true,autoGainControl:true,sampleRate:48000}],
    sessionId:'PRIVATE',token:'PRIVATE',sdp:'PRIVATE',message:'PRIVATE'};
  const result=mediaDiagnostic(0,facts);assert.equal(result.nonSilent,true);assert.equal(result.decodedFrames,null);
  assert.equal(result.decodedPCMWindows,8);assert.equal(result.decodedSamples,3840);assert.equal(result.diagnosticOnly,true);
  assert.equal(JSON.stringify(result).includes('PRIVATE'),false);assert.equal(result.captureTracks[0].noiseSuppression,true);
});

test('未获取RTC解码字段保留null而非把包数伪造为解码帧',()=>{
  const result=mediaDiagnostic(3,{observations:null,nonSilentWindows:null,spectralPeaks:null,maxRMS:null,
    receivedPackets:100,sentPackets:100,decodedSamples:null,decodedFrames:null,peerState:'connecting',contextState:null,captureTracks:[]});
  assert.equal(result.decodedFrames,null);assert.equal(result.decodedSamples,null);assert.equal(result.decodedPCMWindows,null);assert.equal(result.nonSilent,null);
});

test('失败快照拒绝无界或非法计数及非白名单媒体状态',()=>{
  const facts={observations:0,nonSilentWindows:0,spectralPeaks:[0,0,0,0],maxRMS:0,receivedPackets:0,sentPackets:0,
    decodedSamples:0,decodedFrames:null,peerState:'closed',contextState:'closed',captureTracks:[]};
  for(const [key,value] of [['observations',NaN],['nonSilentWindows',1],['spectralPeaks',[Infinity,0,0,0]],
    ['receivedPackets',9007199254740992],['peerState','secret'],['contextState','unknown'],['captureTracks',Array(3).fill({})]])
    assert.throws(()=>mediaDiagnostic(0,{...facts,[key]:value}));
  assert.throws(()=>mediaDiagnostic(4,facts));assert.equal(mediaDiagnostic(0,facts).nonSilent,false);
});

test('浏览器选中候选对仅在内部关联一次，ICE/DTLS与consent计数投影不输出地址或候选ID',async()=>{
  let calls=0;const stats=new Map([
    ['PRIVATE-transport',{type:'transport',selectedCandidatePairId:'PRIVATE-pair',dtlsState:'connected'}],
    ['PRIVATE-pair',{type:'candidate-pair',state:'succeeded',nominated:true,bytesSent:123,bytesReceived:456,
      requestsSent:3,requestsReceived:4,responsesSent:5,responsesReceived:6,consentRequestsSent:2,localCandidateId:'PRIVATE',remoteCandidateId:'PRIVATE'}],
    ['PRIVATE-address',{type:'local-candidate',address:'PRIVATE',port:99}]
  ]);
  const peer={connectionState:'connected',iceConnectionState:'completed',iceGatheringState:'complete',signalingState:'stable',async getStats(){++calls;return stats;}};
  const result=await collectMediaDiagnostic(peer,null,null);assert.equal(calls,1);
  assert.deepEqual(connectivityDiagnostic(result.connectivity),{iceConnectionState:'completed',iceGatheringState:'complete',signalingState:'stable',dtlsState:'connected',
    selectedPairState:'succeeded',nominated:true,bytesSent:123,bytesReceived:456,requestsSent:3,requestsReceived:4,responsesSent:5,responsesReceived:6,consentRequestsSent:2});
  assert.equal(JSON.stringify(result).includes('PRIVATE'),false);
});

test('未有实际选中候选对或多传输映射歧义保持unknown，不能从nominated或包数推断连通',async()=>{
  for(const stats of [new Map(),new Map([['pair',{type:'candidate-pair',state:'succeeded',nominated:true,bytesSent:3}]]),
    new Map([['one',{type:'transport',dtlsState:'connected',selectedCandidatePairId:'pair'}],['two',{type:'transport',dtlsState:'connected',selectedCandidatePairId:'pair'}],['pair',{type:'candidate-pair',state:'succeeded'}]])]){
    const peer={connectionState:'failed',iceConnectionState:'failed',iceGatheringState:'complete',signalingState:'stable',getStats:async()=>stats};
    const result=await collectMediaDiagnostic(peer,null,null);assert.equal(result.connectivity.iceConnectionState,'failed');assert.equal(result.connectivity.selectedPairState,'unknown');
    assert.equal(result.connectivity.nominated,null);assert.equal(result.connectivity.bytesSent,null);assert.equal(result.connectivity.dtlsState,'unknown');
  }
  const absent=await collectMediaDiagnostic(null,null,null);assert.equal(absent.connectivity.iceConnectionState,'unknown');
});

test('连通性有限投影拒绝未知状态、非法计数和额外身份字段，浏览器缺失计数保持null',async()=>{
  const value=connectivityDiagnostic();assert.equal(value.iceConnectionState,'unknown');assert.equal(value.consentRequestsSent,null);
  for(const change of [{dtlsState:'PRIVATE'},{selectedPairState:'PRIVATE'},{iceConnectionState:'PRIVATE'},{signalingState:'PRIVATE'},
    {iceGatheringState:'PRIVATE'},{nominated:'true'},{bytesSent:-1},{responsesReceived:Number.MAX_SAFE_INTEGER+1},{consentRequestsSent:0.1},{candidateId:'PRIVATE'}])
    assert.throws(()=>connectivityDiagnostic({...value,...change}));
  const stats=new Map([['transport',{type:'transport',selectedCandidatePairId:'pair',dtlsState:'PRIVATE'}],['pair',{type:'candidate-pair',state:'PRIVATE',bytesSent:-1,requestsSent:Infinity}]]);
  const result=await collectMediaDiagnostic({connectionState:'connected',getStats:async()=>stats},null,null);
  assert.equal(result.connectivity.dtlsState,'unknown');assert.equal(result.connectivity.bytesSent,null);assert.equal(result.connectivity.requestsSent,null);
  assert.equal(JSON.stringify(result).includes('PRIVATE'),false);
});

test('页面自包含诊断函数的全部标准RTC枚举可经宿主严格投影互解，缺失字段不补连接成功',async()=>{
  const states={iceConnectionState:['new','checking','connected','completed','disconnected','failed','closed'],
    iceGatheringState:['new','gathering','complete'],signalingState:['stable','have-local-offer','have-remote-offer','have-local-pranswer','have-remote-pranswer','closed'],
    dtlsState:['new','connecting','connected','closed','failed'],selectedPairState:['frozen','waiting','in-progress','failed','succeeded']};
  for(const [field,values] of Object.entries(states))for(const value of values){
    const pair={type:'candidate-pair',state:'succeeded'},transport={type:'transport',dtlsState:'connected',selectedCandidatePairId:'PRIVATE'};
    const peer={connectionState:'connected',iceConnectionState:'completed',iceGatheringState:'complete',signalingState:'stable',getStats:async()=>new Map([['transport',transport],['PRIVATE',pair]])};
    if(field==='dtlsState')transport.dtlsState=value;else if(field==='selectedPairState')pair.state=value;else peer[field]=value;
    // 与正式驱动toString注入相同，函数不得依赖模块闭包；这里只模拟stats，不宣称真实ICE成功。
    const collected=await vm.runInNewContext('('+collectMediaDiagnostic.toString()+')(peer,null,null)',{peer,setTimeout,clearTimeout});
    const projected=connectivityDiagnostic(collected.connectivity);assert.equal(projected[field],value);assert.equal(projected.bytesReceived,null);
    assert.equal(JSON.stringify(projected).includes('PRIVATE'),false);
  }
});

// rosterFixture 执行正式网页incoming并计数，仅模拟已由C++通知建立的UI腿；不构造MQTT或音频。
function rosterFixture(enabled=true){
  const element=()=>({append(){},remove(){},textContent:''}),elements=new Map();
  const context=vm.createContext({TextEncoder,window:{addEventListener(){}},
    WebSocket:Object.assign(function(){},{OPEN:1}),
    document:{createElement:element,getElementById(id){if(!elements.has(id))elements.set(id,element());return elements.get(id);}},
    makeElement:element});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'web/app.js'),'utf8'),context);
  vm.runInContext(`views.set('leg',{active:true,talk:true,closed:false,element:makeElement()});
    socket={send(){return true;},readyState:1,bufferedAmount:0}; // 原send依赖真实socket;离线上行观察只统计计数,不构造MQTT。
    window.originalCalls=0;const productionIncoming=incoming;
    incoming=async value=>{window.originalCalls++;return productionIncoming(value);};window.originalIncoming=incoming;`,context);
  vm.runInContext(`(${installRosterObserver.toString()})(${enabled?'true':'false'})`,context);
  return {context,
    async incoming(value){context.value=value;await vm.runInContext('incoming(value)',context);},
    read(session='leg',expected=['101','102','103']){context.session=session;context.expected=expected;
      return JSON.parse(JSON.stringify(vm.runInContext('window.__conferenceRosterObservation.read(session,expected)',context)));},
    eval(code){return vm.runInContext(code,context);},
  };
}
// rosterNotice 仅构造已有本地展示shape，实际提交仍必须经过正式网页消费者。
function rosterNotice(eventSeq,members=['101','102','103'],sessionId='leg'){
  return {kind:'ipcParticipants',sessionId,eventSeq,participants:members.map(participantId=>({participantId,tracks:[]}))};
}
test('名册观察显式off不替换消费者、不创建观察器；能力启用必须准确1',()=>{
  const f=rosterFixture(false);
  assert.equal(f.eval('incoming===window.originalIncoming'),true);
  assert.equal(f.eval('window.__conferenceRosterObservation===undefined'),true);
  for(const value of [undefined,'0','1'])assert.equal(validateConfiguration({...configuration(),UR_IPC_PARTICIPANT_ROSTER:value},()=>({isFile:()=>true})).rosterEnabled,value==='1');
  for(const value of ['true','',1,null])assert.throws(()=>validateConfiguration({...configuration(),UR_IPC_PARTICIPANT_ROSTER:value},()=>({isFile:()=>true})));
});
test('初始三成员提交与禁麦保持成员；原消费者每通知仅执行一次、观察不输出身份',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));
  assert.equal(verifyRosterObservation(f.read(),3),true);assert.equal(f.eval('window.originalCalls'),1);
  f.eval("views.get('leg').speakerPaused=true;"); // 名册与媒体MAC无关联，不通过观察器修改轨。
  assert.equal(verifyRosterObservation(f.read(),3),true);
  assert.equal(JSON.stringify(f.read()).includes('101'),false);
  assert.equal(f.eval("views.get('leg').rosterSequence"),3);
});
test('原left后的本地投影必须更大同腿水位且精确剩余集合',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));const original=f.read().eventSeq;
  await f.incoming(rosterNotice(8,['101','102']));
  assert.equal(verifyRosterObservation(f.read('leg',['101','102']),2,original),true);
  assert.throws(()=>verifyRosterObservation(f.read(),3,original));
  assert.equal(f.eval('window.originalCalls'),2);
});
test('重复、迟到与坏notice不增消费水位，不能把已入桥当已消费',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(8));
  for(const value of [rosterNotice(8,[]),rosterNotice(7,[]),{...rosterNotice(9),secret:'forbidden'}])await f.incoming(value);
  assert.equal(f.read().eventSeq,8);assert.equal(f.read().memberCount,3);
  assert.throws(()=>verifyRosterObservation(f.read(),3,8));assert.equal(f.eval('window.originalCalls'),4);
});
test('错腿、未建立或终态不借已有view；移除后清观察成员不称完整恢复',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));
  await f.incoming(rosterNotice(4,[],'other'));assert.equal(f.read('other',[]).consumerApplied,false);
  f.eval("views.get('leg').active=false;");await f.incoming(rosterNotice(4,[]));
  assert.equal(f.read().consumerApplied,false);
  f.eval("views.delete('leg');");const closed=f.read();
  assert.equal(closed.closed,true);assert.equal(closed.memberCount,0);assert.equal(closed.eventSeq,null);
  assert.throws(()=>verifyRosterObservation(closed,0));
});
test('观察预算耗尽仍执行原消费者，明确truncated而非伪造没有成员',async()=>{
  const f=rosterFixture();for(let index=1;index<=129;index++)await f.incoming(rosterNotice(index));
  const result=f.read();assert.equal(result.truncated,true);assert.equal(result.consumerApplied,false);
  assert.equal(f.eval('window.originalCalls'),129);assert.equal(f.eval("views.get('leg').rosterSequence"),129);
  assert.throws(()=>verifyRosterObservation(result,3));
});
test('四腿观察上限独立于原消费者，第五腿不借其它腿消费事实',async()=>{
  const f=rosterFixture();
  for(let index=0;index<5;index++){
    const session='leg-'+index;f.context.session=session;
    f.eval('views.set(session,{active:true,talk:true,closed:false,element:makeElement()});');
    await f.incoming(rosterNotice(3,['101'],session));
  }
  assert.equal(f.read('leg-4',['101']).truncated,true);
  assert.equal(f.read('leg-4',['101']).consumerApplied,false);
  assert.equal(f.eval('window.originalCalls'),5);
  assert.equal(f.eval("views.get('leg-4').rosterSequence"),3);
});
test('等待原消费者实际异步完成再读取，不旁路改原返回或吞异常',async()=>{
  const f=rosterFixture(false);let complete;
  f.context.waitOriginal=new Promise(resolve=>complete=resolve);
  f.eval('incoming=async value=>{await waitOriginal;await window.originalIncoming(value);return 7;};');
  f.eval(`(${installRosterObserver.toString()})(true)`);
  f.context.value=rosterNotice(3);
  const pending=f.eval('incoming(value)');
  assert.equal(f.read().consumerApplied,false);
  complete();assert.equal(await pending,7);assert.equal(f.read().consumerApplied,true);
  await assert.rejects(f.incoming(null)); // 原网页非法命令失败仍保留，不改成观察通过。
});
test('原CPP查询状态经过正式网页消费后必须能独立证明完整查询，不能借增量名册绿',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));
  await f.incoming({kind:'ipcSyncState',sessionId:'leg',state:'ready',eventSeq:3});
  await f.incoming({kind:'ipcSyncState',sessionId:'leg',state:'pending',eventSeq:3});
  await f.incoming(rosterNotice(4));
  await f.incoming({kind:'ipcSyncState',sessionId:'leg',state:'succeeded',eventSeq:4});
  assert.equal(f.eval("typeof window.__conferenceRosterObservation.readSnapshot==='function'"),true,
    '完整查询验收入口尚缺，不能以增量名册通过代替');
  const value=JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.readSnapshot('leg')")));
  assert.equal(verifySnapshotObservation(value,f.read(),3,3),true);
  assert.equal(JSON.stringify(value).includes('101'),false);
});
// snapshotNotice 使用原CPP本地HAL四字段，不能向MQTT伪造成功回报。
function snapshotNotice(state,eventSeq,sessionId='leg'){return {kind:'ipcSyncState',sessionId,state,eventSeq};}
// snapshotRead 读取原观察器闭集事实，不返回原身份或令牌。
function snapshotRead(f,session='leg'){f.context.snapshotSession=session;return JSON.parse(JSON.stringify(f.eval('window.__conferenceRosterObservation.readSnapshot(snapshotSession)')));}
test('查询失败终态必须清除原pending证明，迟到成功不能借上一尝试的受理',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));await f.incoming(snapshotNotice('pending',3));
  await f.incoming(snapshotNotice('failed',3));await f.incoming(rosterNotice(4));
  await f.incoming(snapshotNotice('succeeded',4));
  assert.equal(snapshotRead(f).pendingObserved,false);
  assert.throws(()=>verifySnapshotObservation(snapshotRead(f),f.read(),3,3));
});
test('完整查询显式1且必须同时名册能力，默认配置不新增查询阶段',()=>{
  assert.equal(validateConfiguration(configuration(),()=>({isFile:()=>true})).rosterSyncEnabled,false);
  assert.equal(validateConfiguration({...configuration(),UR_IPC_PARTICIPANT_ROSTER:'1',UR_IPC_PARTICIPANT_ROSTER_SYNC:'1'},()=>({isFile:()=>true})).rosterSyncEnabled,true);
  for(const value of ['true','',1,null])assert.throws(()=>validateConfiguration({...configuration(),UR_IPC_PARTICIPANT_ROSTER:'1',UR_IPC_PARTICIPANT_ROSTER_SYNC:value},()=>({isFile:()=>true})));
  assert.throws(()=>validateConfiguration({...configuration(),UR_IPC_PARTICIPANT_ROSTER_SYNC:'1'},()=>({isFile:()=>true})));
});
test('只有成功状态或增量成员不能冒充完整查询，点击先pending仍须原CPP消息',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));await f.incoming(snapshotNotice('ready',3));
  f.eval("views.get('leg').syncState='pending';"); // 原UI点击先置pending，不把点击记录为CPP查询受理。
  assert.equal(snapshotRead(f).pendingObserved,false);
  await f.incoming(snapshotNotice('pending',3));assert.equal(snapshotRead(f).pendingObserved,true);
  assert.throws(()=>verifySnapshotObservation(snapshotRead(f),f.read(),3,3));
  await f.incoming(rosterNotice(4));await f.incoming(snapshotNotice('succeeded',4));
  assert.equal(verifySnapshotObservation(snapshotRead(f),f.read(),3,3),true);
  const onlySuccess=rosterFixture();await onlySuccess.incoming(rosterNotice(4));await onlySuccess.incoming(snapshotNotice('succeeded',4));
  assert.equal(snapshotRead(onlySuccess).pendingObserved,false,'未观察原CPP受理仍返回固定false，而非遗漏诊断字段');
  assert.throws(()=>verifySnapshotObservation(snapshotRead(onlySuccess),onlySuccess.read(),3,3));
});
test('未知/失败/旧水位/不同成员/多余正文与错腿查询不能通过或借旧成功',async()=>{
  for(const state of ['unknown','failed','cancelled','rejected','unavailable']){const f=rosterFixture();await f.incoming(rosterNotice(3));await f.incoming(snapshotNotice('pending',3));await f.incoming(rosterNotice(4));await f.incoming(snapshotNotice(state,4));assert.throws(()=>verifySnapshotObservation(snapshotRead(f),f.read(),3,3));}
  const f=rosterFixture();await f.incoming(rosterNotice(3));await f.incoming(snapshotNotice('pending',3));await f.incoming(snapshotNotice('succeeded',3));
  assert.throws(()=>verifySnapshotObservation(snapshotRead(f),f.read(),3,3));
  await f.incoming(rosterNotice(4,['101','102']));await f.incoming(snapshotNotice('succeeded',4));assert.throws(()=>verifySnapshotObservation(snapshotRead(f),f.read(),3,3));
  await f.incoming({...snapshotNotice('succeeded',5),secret:'PRIVATE'});assert.equal(snapshotRead(f).eventSeq,4);
  await f.incoming(snapshotNotice('pending',4,'other'));assert.equal(snapshotRead(f,'other').consumerApplied,false);
  const bogus={...snapshotRead(f),eventSeq:5};assert.throws(()=>verifySnapshotObservation(bogus,f.read('leg',['101','102']),2,3));
});
test('终态删除原UI腿清查询观察，不留成员/旧成功；有界预算满不能造通过',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));await f.incoming(snapshotNotice('pending',3));await f.incoming(rosterNotice(4));await f.incoming(snapshotNotice('succeeded',4));
  f.eval("views.delete('leg');");assert.deepEqual(snapshotRead(f),{enabled:true,available:false,consumerApplied:false,state:'missing',pendingObserved:false,eventSeq:null,closed:true,truncated:false});
  const budget=rosterFixture();await budget.incoming(rosterNotice(3));for(let index=0;index<129;index++)await budget.incoming(snapshotNotice(index%2?'pending':'ready',3));assert.equal(snapshotRead(budget).truncated,true);assert.throws(()=>verifySnapshotObservation(snapshotRead(budget),budget.read(),3,2));
});
test('完整查询后四原Peer都必须实际继续收发/解码，断连重建或静止计数拒绝',()=>{
  const before=Array.from({length:4},()=>({samePeer:true,connected:true,received:10,sent:10,samples:3840})),after=before.map(row=>({...row,received:20,sent:20,samples:7680}));assert.equal(verifySnapshotContinuity(before,after),true);
  for(const change of [rows=>{rows[0].samePeer=false;},rows=>{rows[1].connected=false;},rows=>{rows[2].samples=3840;},rows=>{rows[3].sent=10;},rows=>{rows[0].received=NaN;},rows=>{rows.pop();},rows=>{rows[0].secret='PRIVATE';}]){const changed=structuredClone(after);change(changed);assert.throws(()=>verifySnapshotContinuity(before,changed));}
  const source=fs.readFileSync(path.join(__dirname,'conference_member_live.cjs'),'utf8');assert.ok(source.includes("view.syncButton.scrollIntoView({block:'center'})"));assert.ok(source.includes("'find','role','button','click','--name','查询完整成员名册'"));assert.ok(!source.includes('view.syncButton.click()'));assert.ok(source.includes('await fullSnapshotQuery(initialRosterSequence)'));assert.ok(source.includes('four_original_peers_continue_after_snapshot'));assert.ok(source.includes('fullSnapshotQueryVerified=true'));assert.ok(source.includes('cpp_snapshot_observer_not_released'));
});
test('固定名册投影严格字段、边界、未知及非空轨都不可误PASS',async()=>{
  const f=rosterFixture();await f.incoming(rosterNotice(3));const good=f.read();
  for(const [key,value] of [['eventSeq',null],['eventSeq',2],['memberCount',4],['expectedCount',4],
    ['trackCount',1],['trackCount',257],['consumerApplied',false],['exactMembers',false],['closed',true],['truncated',true]])
    assert.throws(()=>verifyRosterObservation({...good,[key]:value},3,2));
  assert.throws(()=>verifyRosterObservation({...good,sessionId:'forbidden'},3));
});
test('名册逐条投影历史必须是合法状态序列的单调推进,跳态允许但回退/带轨/终态不符拒绝',()=>{
  const expectations=[['101'],['101','102'],['101','102','103']];
  const joined=[{eventSeq:1,identities:['101'],trackCount:0},{eventSeq:2,identities:['101','102'],trackCount:0},
    {eventSeq:3,identities:['101','102','103'],trackCount:0}];
  assert.equal(verifyRosterTransitions(joined,expectations),true);
  assert.equal(verifyRosterTransitions([joined[0],joined[2]],expectations),true,'设备2ms轮询允许跳过中间态');
  for(const bad of [[],
    [{eventSeq:3,identities:['101','102','103'],trackCount:0},{eventSeq:3,identities:['101','102','103'],trackCount:0}],
    [{eventSeq:3,identities:['101','102','103'],trackCount:1}],
    [{eventSeq:3,identities:['101','102'],trackCount:0}],
    [{eventSeq:1,identities:['101','102','103'],trackCount:0},{eventSeq:2,identities:['101'],trackCount:0}],
    [{eventSeq:1,identities:['104'],trackCount:0}],
    [{eventSeq:1,identities:['101','102','103'],trackCount:0,secret:'PRIVATE'}]])
    assert.throws(()=>verifyRosterTransitions(bad,expectations));
  assert.throws(()=>verifyRosterTransitions(joined,expectations,3),'afterSequence之后必须有新投影');
  assert.throws(()=>verifyRosterTransitions(joined,[['101'],['101','102']]));
});

test('全量查询后的离开检查必须采用查询已安装水位，不能把完整快照误记成离开事件',()=>{
  const members=['101','102','103'];
  const history=[{eventSeq:5,identities:members,trackCount:0},{eventSeq:6,identities:members,trackCount:0},
    {eventSeq:7,identities:['101','102'],trackCount:0}];
  assert.throws(()=>verifyRosterTransitions(history,[['101','102']],5),'旧查询前水位会错误纳入完整快照');
  assert.equal(verifyRosterTransitions(history.filter(entry=>entry.eventSeq>6),[['101','102']],6),true);
  const source=fs.readFileSync(path.join(__dirname,'conference_member_live.cjs'),'utf8');
  assert.ok(source.includes('let leaveRosterSequence=await fullSnapshotQuery(initialRosterSequence)'));
  assert.ok(source.includes("leaveRosterSequence=await fullSnapshotQuery(leaveRosterSequence,{label:'roster_reconcile_preserves_local_mute'"),'再次查询后采用最新已安装水位');
  assert.ok(source.includes("rosterTransitions('cpp_leave_transitions',[users.slice(0,2).map(user=>user.id).sort()],leaveRosterSequence??0)"));
  assert.ok(source.includes('records=history.records.filter(entry=>entry.eventSeq>afterSequence)'));
  assert.ok(source.includes('verifyRosterTransitions(records,expectations,afterSequence)'));
});
test('trackState(reason=participantLeft)契约未接线的负判据:成员轨恒空且无轨控制与回执',()=>{
  assert.equal(verifyParticipantTrackContract({ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:0,truncated:false,available:true,closed:false}),true);
  for(const bad of [{ipcTrackReceived:1,trackStateSent:0,rosterTrackEntries:0,truncated:false,available:true,closed:false},
    {ipcTrackReceived:0,trackStateSent:1,rosterTrackEntries:0,truncated:false,available:true,closed:false},
    {ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:2,truncated:false,available:true,closed:false},
    {ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:0,truncated:true,available:true,closed:false},
    {ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:0,truncated:false,available:false,closed:false},
    {ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:0,truncated:false,available:true,closed:true}])
    assert.throws(()=>verifyParticipantTrackContract(bad));
  assert.throws(()=>verifyParticipantTrackContract({...{ipcTrackReceived:0,trackStateSent:0,rosterTrackEntries:0,truncated:false,available:true,closed:false},extra:'PRIVATE'}));
});
test('成员离开后其余端解码计数不得归零或回退,端数必须一致',()=>{
  const before=[{received:100,sent:90,samples:19200},{received:80,sent:70,samples:15360},{received:60,sent:50,samples:11520}];
  assert.equal(verifyDecodeNeverReset(before,before.map(row=>({received:row.received+10,sent:row.sent+10,samples:row.samples+1920}))),true);
  assert.equal(verifyDecodeNeverReset(before,before.map(row=>({...row}))),true,'静止不误报归零,增长判据由混音阶段独立断言');
  for(const change of [rows=>{rows[0].received=0;},rows=>{rows[1].samples=15359;},rows=>{rows[2].sent=49;},rows=>{rows[0].received=NaN;},rows=>{rows.pop();}]){
    const changed=before.map(row=>({...row}));change(changed);assert.throws(()=>verifyDecodeNeverReset(before,changed));
  }
  assert.throws(()=>verifyDecodeNeverReset([],[]));
});
test('结束卡片只允许一次且必须是终态:零次、重复与非终态ended都拒绝',()=>{
  assert.equal(verifySingleEndNotice(['established','ended']),true);
  assert.throws(()=>verifySingleEndNotice(['established','ended','ended']));
  assert.throws(()=>verifySingleEndNotice(['established']));
  assert.throws(()=>verifySingleEndNotice(['ended','established']));
  assert.throws(()=>verifySingleEndNotice([]));
  assert.throws(()=>verifySingleEndNotice([1]));
});
test('观察器必须暴露逐条历史与轨契约入口;ipcTrack下行与ipcTrackApplied上行按腿计数',async()=>{
  const f=rosterFixture();
  assert.equal(typeof f.eval("window.__conferenceRosterObservation.transitions")==='function'&&
    typeof f.eval("window.__conferenceRosterObservation.wire")==='function',true,'观察器逐条历史与轨契约入口尚缺');
  await f.incoming(rosterNotice(3));
  const history=JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.transitions('leg')")));
  assert.deepEqual(history.records.map(record=>[record.eventSeq,record.identities.slice().sort()]),[[3,['101','102','103']]]);
  assert.equal(history.truncated,false);
  // 带轨条目是网页消费者接受的展示形状，但生产后端子集恒发空轨；逐条历史必须记录轨数，
  // 轨契约负判据据此拒绝任何带轨投影，而不是依赖网页层拒绝。
  await f.incoming({kind:'ipcParticipants',sessionId:'leg',eventSeq:4,
    participants:[{participantId:'101',tracks:[{trackId:'t',kind:'audio',sourceId:'101',direction:'recvonly',codec:'opus',state:'active'}]}]});
  assert.equal(f.eval("views.get('leg').rosterSequence"),4);
  assert.equal(JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.wire('leg')"))).rosterTrackEntries,1);
  await f.incoming({kind:'ipcTrack',sessionId:'leg',token:'control',trackId:'t',operation:'pause',state:'paused'});
  // 原页面收到ipcTrack即走失败回执并release本腿；下行、自动回执与腿释放都必须被观察计数捕获。
  {
    const afterTrack=JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.wire('leg')")));
    assert.equal(afterTrack.ipcTrackReceived,1);assert.equal(afterTrack.trackStateSent,1);assert.equal(afterTrack.closed,true);
    assert.throws(()=>verifyParticipantTrackContract(afterTrack));
  }
  f.eval("send({kind:'ipcTrackApplied',sessionId:'leg',token:'control',state:'paused',success:true})");
  assert.equal(JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.wire('leg')"))).trackStateSent,2);
  const other=JSON.parse(JSON.stringify(f.eval("window.__conferenceRosterObservation.wire('other')")));
  assert.equal(other.ipcTrackReceived,0);assert.equal(other.trackStateSent,0);
  const gone=JSON.parse(JSON.stringify(f.eval("views.delete('leg');JSON.stringify(window.__conferenceRosterObservation.transitions('leg'))")));
  assert.deepEqual(JSON.parse(gone),{records:[],truncated:false,closed:true});
});
test('驱动必须接线逐条断言、轨契约、解码不归零与结束卡片判据',()=>{
  const source=fs.readFileSync(path.join(__dirname,'conference_member_live.cjs'),'utf8');
  assert.ok(source.includes('rosterTransitions(\'cpp_joined_transitions\''),'初始加入必须逐条断言');
  assert.ok(source.includes('rosterTransitions(\'cpp_leave_transitions\''),'成员离开必须逐条断言');
  assert.ok(source.includes('verifyParticipantTrackContract'),'U3离开后必须取证轨契约边界');
  assert.ok(source.includes('verifyDecodeNeverReset'),'剩余端解码计数不得归零');
  assert.ok(source.includes('verifySingleEndNotice'),'结束卡片必须唯一');
  assert.ok(source.includes('ended_must_precede_heartbeat_sweep'),'主动结束必须先于心跳过期清扫窗口');
  assert.ok(source.includes('verifyRosterReconcileAuthority(macBefore,macAfter)'),'完整查询不得创建媒体许可或原IM新实例');
  assert.ok(source.includes('await membership();await localMute()'),'快照前后均核正式群关系和用户实际本地静音');
  assert.ok(source.includes("label:'roster_reconcile_after_host_remove'"),'移出后当前名册须由原端点正式重取');
  assert.ok(source.includes("report.roster.recoveryKind='authoritative_roster_reconciliation'"),'权威重取不得称网络恢复');
  assert.ok(!source.includes("reason:'snapshotOnly',resumeAllowed:false"),'UI成功不能反推固定快照许可');
  assert.ok(source.includes('report.roster.snapshotApplication='),'报告仅记录真实消费与原Peer持续事实');
});
