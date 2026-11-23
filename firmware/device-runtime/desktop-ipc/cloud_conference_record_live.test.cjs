// 文件说明：会议云录制端云驱动的离线判据；不能用该单测替代正式四端WebRTC及私有产物验收。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const {configuration,recordedMix,createRecordingStateProjection,recordingFailureDiagnostic,probe,run}=require('./cloud_conference_record_live.cjs');
const {FREQUENCIES}=require('./conference_member_live.cjs');

test('导入无网络副作用且全员录制须专门显式开关',()=>{
  assert.equal(typeof probe,'function');assert.equal(typeof run,'function');
  const env={UR_CLOUD_CONFERENCE_RECORD_LIVE:'1',UR_CONFERENCE_MEMBER_LIVE:'1',UR_CONFERENCE_FIXED_PCM_TEST:'1',UR_CLOUD_RECORD_POLICY_ID:'policy',UR_IPC_BASE_URL:'http://127.0.0.1:10000/'};
  assert.deepEqual(configuration(env),{policy:'policy'});
  for(const change of [{UR_CLOUD_CONFERENCE_RECORD_LIVE:'0'},{UR_CONFERENCE_MEMBER_LIVE:'0'},{UR_CONFERENCE_FIXED_PCM_TEST:'0'},{UR_CLOUD_RECORD_POLICY_ID:''},{UR_IPC_BASE_URL:'http://external.example/'},{UR_IPC_BASE_URL:'http://127.0.0.1/path'}])assert.throws(()=>configuration({...env,...change}));
});

// pcm 固定样本仅验证频谱判据，不伪造网络或产物证据。
function pcm(frequencies){
  const bytes=Buffer.alloc(48000*2);
  for(let index=0;index<48000;index++){
    const value=frequencies.reduce((sum,frequency)=>sum+Math.sin(2*Math.PI*index*frequency/48000)*1800,0);
    bytes.writeInt16LE(Math.round(value),index*2);
  }
  return bytes;
}
test('完整混音必须含四个实际音源，任一mix-minus-self或静音都拒绝',()=>{
  const result=recordedMix(pcm(FREQUENCIES));assert.equal(result.samples,48000);assert.equal(result.peaks.length,4);
  for(let excluded=0;excluded<4;excluded++)assert.throws(()=>recordedMix(pcm(FREQUENCIES.filter((_,index)=>index!==excluded))));
  assert.throws(()=>recordedMix(Buffer.alloc(48000*2)));assert.throws(()=>recordedMix(Buffer.alloc(9600)));
});

test('录制失败只报告固定分类和数字码，不能泄漏自由错误正文',()=>{
  assert.deepEqual(recordingFailureDiagnostic({message:'fresh_joined_epoch_required',actualCode:409},'formal_member_leave_rejoin'),
    {stage:'formal_member_leave_rejoin',reason:'fresh_joined_epoch_required',code:409,cleanupFailures:[]});
  for(const error of [{message:'https://private.invalid/?token=SECRET',actualCode:'403'},{message:'secret device identity',actualCode:-1},null])
    assert.deepEqual(recordingFailureDiagnostic(error,'formal_member_leave_rejoin'),
      {stage:'formal_member_leave_rejoin',reason:'unclassified_recording_failure',code:null,cleanupFailures:[]});
});

// projectionFixture 仅定义三个原用户的同一录制实例；源会话不是新媒体腿，观察不能打开采集。
function projectionFixture() {
  const users=[1,2,3].map(id=>({id:String(id)})),projection=createRecordingStateProjection(users,'synthetic-source','synthetic-record');
  const event=(revision=4,state='paused')=>({method:'interaction.controlState',sessionId:'synthetic-source',
    data:{command:'recordingState',result:{recordingId:'synthetic-record',revision,state,reason:'recordingConsentMissing'}}});
  return {users,projection,event};
}
test('重入不清空原端已收到的录制事实，各端同修订到达才算全员指示',()=>{
  const f=projectionFixture();
  f.projection.observe(f.users[2],f.event());f.users[2].events=[];f.users[2].session='synthetic-new-leg';
  assert.equal(f.projection.allObserved('paused',4),false);assert.equal(f.projection.indicators('paused',4),1);
  f.projection.observe(f.users[0],f.event());f.projection.observe(f.users[1],f.event());
  assert.equal(f.projection.allObserved('paused',4),true);assert.equal(f.projection.read().reason,'recordingConsentMissing');
  assert.equal(f.projection.observe(f.users[2],f.event()),false);
  assert.equal(f.projection.observe(f.users[2],f.event(3,'active')),false);
  assert.equal(f.projection.observe(f.users[2],f.event(4,'failed')),false);
  for(const user of f.users)f.projection.observe(user,f.event(5,'active'));
  assert.equal(f.projection.allObserved('active',5),true);assert.equal(f.projection.read().revision,5);
  assert.equal(f.users.some(user=>Object.hasOwn(user,'capture')),false,'观察不添加采集控制或恢复麦克风');
});
test('错端点、错源、错录制实例、旧修订、未知结构及任意私密字段不进入投影',()=>{
  const mutations=[e=>e.sessionId='synthetic-other',e=>e.method='interaction.result',e=>e.data.command='other',
    e=>e.data.result.recordingId='synthetic-other',e=>e.data.result.revision=0,e=>e.data.result.revision=1.5,
    e=>e.data.result.state='arbitrary',e=>e.data.result.reason={},e=>e.data.result.reason='x'.repeat(129)];
  for(const mutate of mutations){const f=projectionFixture(),e=f.event();mutate(e);
    assert.equal(f.projection.observe(f.users[0],e),false);assert.equal(f.projection.read(),null);}
  const f=projectionFixture();assert.equal(f.projection.observe({id:'1'},f.event()),false);
  const e=f.event();e.data.result.secret='PRIVATE';e.data.result.signedURL='PRIVATE';
  assert.equal(f.projection.observe(f.users[0],e),true);assert.equal(JSON.stringify(f.projection.read()).includes('PRIVATE'),false);
});

test('重入两段等待的旁证只保留固定枚举、三端计数和布尔',()=>{
  const observation={stage:'allIndicators',state:'paused',revisionAdvanced:true,pausedReasonMatches:true,indicators:2};
  const actual=recordingFailureDiagnostic(Error('cloud_conference_record_timeout'),'formal_member_leave_rejoin',
    {...observation,privateBody:'PRIVATE'});
  assert.deepEqual(actual.memberRejoinObservation,observation);assert.equal(JSON.stringify(actual).includes('PRIVATE'),false);
  for(const change of [{stage:'PRIVATE'},{state:'PRIVATE'},{indicators:4},{indicators:-1},{revisionAdvanced:1},{pausedReasonMatches:null}])
    assert.equal(Object.hasOwn(recordingFailureDiagnostic(null,'formal_member_leave_rejoin',{...observation,...change}),'memberRejoinObservation'),false);
});

test('旧授权细分仅输出固定阶段、数字码和格式布尔，不反射任何新凭证或自由正文',()=>{
  const observation={stage:'rejectOld',outcome:'rejected',code:403,ownGrants:0,
    newGrantFormat:null,newGrantRevisionOne:null,newIdsDistinct:null};
  assert.deepEqual(recordingFailureDiagnostic(null,'stale_join_epoch_consent_rejected',null,{...observation,privateBody:'PRIVATE'}).staleConsentObservation,observation);
  for(const change of [{stage:'PRIVATE'},{outcome:'PRIVATE'},{code:-1},{code:'409'},{ownGrants:4},{newIdsDistinct:'PRIVATE'}])
    assert.equal(Object.hasOwn(recordingFailureDiagnostic(null,'stale_join_epoch_consent_rejected',null,{...observation,...change}),'staleConsentObservation'),false);
});

test('原pump已ACK唯一paused时重入全员观察仍完成，不能因新腿events重置丢录制事实',async()=>{
  for(const mode of ['afterAllPaused','oldRejectUnexpected','oldAccepted','newGrantBadRevision','newIdsReused']) {
  const users=[1,2,3].map(id=>({id:String(id),endpoint:'synthetic-endpoint-'+id,events:[],stateTimeline:[],session:'synthetic-leg-'+id}));
  const inbox=new Map(users.map(user=>[user.id,[]]));let callback=null,pausedAcknowledged=false,rejoinedSources=false,clock=0,recordingId;
  const originalDate=Date.now,originalTimer=global.setTimeout,stop=Error('synthetic-stop-after-all-paused');
  const source=fs.readFileSync(path.join(__dirname,'conference_member_live.cjs'),'utf8');
  const begin=source.indexOf('  async function pump('),end=source.indexOf('\n  // command ',begin);
  assert.ok(begin>=0&&end>begin);
  const relay={observe:(user,event)=>callback?.(user,event)};
  const api=async(user,route,body)=>{
    if(route==='im/interaction/events')return {list:inbox.get(user.id).splice(0)};
    if(route==='im/interaction/events/ack'){if(body.deliveryIds.includes('synthetic-rejoin-pause-3'))pausedAcknowledged=true;return {};}
    assert.equal(route,'im/interaction/commands');const command=body.command,args=command.params.arguments,selector=command.params.command;
    if(selector==='recordingConsent'&&rejoinedSources&&mode==='afterAllPaused')throw stop; // 后续真实产物链不以合成文件代替。
    if(selector==='recordingControl'&&args.operation==='resume'&&args.expectedRevision===4){
      rejoinedSources=true;
      if(mode!=='oldAccepted'){const conflict=Error('synthetic-old-consent');conflict.actualCode=mode==='oldRejectUnexpected'?403:409;throw conflict;}
    }
    if(selector==='recordingControl')recordingId=args.recordingId;
    const revisions={start:1,pause:2,resume:3,stop:5},state=args.operation==='pause'?'paused':args.operation==='stop'?'finalizing':'active';
    const data=selector==='recordingConsent'?{consentId:user.id.repeat(64),revision:rejoinedSources&&mode==='newGrantBadRevision'?2:1,sourceTrackIds:['conference-audio']}:
      {state,revision:revisions[args.operation]};
    inbox.get(user.id).push({deliveryId:'synthetic-result-'+command.msgToken,event:{method:'interaction.result',sessionId:'synthetic-source',msgToken:command.msgToken,code:200,data}});
    if(selector==='recordingControl')for(const member of users)inbox.get(member.id).push({deliveryId:'synthetic-state-'+member.id,event:{method:'interaction.controlState',sessionId:'synthetic-source',data:{command:'recordingState',result:{recordingId:args.recordingId,state,revision:data.revision}}}});
    return {accepted:true,msgToken:command.msgToken};
  };
  const pump=vm.runInNewContext('('+source.slice(begin,end).trim()+')',{api,assert,recordingEvents:relay});
  const context={users,sourceSession:'synthetic-source',api,snapshotMedia:async()=>[],activeMembers:async()=>{},
    observeRecordingState:fn=>{assert.equal(callback,null);callback=fn;return ()=>{callback=null;};},
    rejoinMember:async user=>{
      for(const member of users)inbox.get(member.id).push({deliveryId:'synthetic-rejoin-pause-'+member.id,event:{method:'interaction.controlState',sessionId:'synthetic-source',data:{command:'recordingState',result:{recordingId,state:'paused',reason:'recordingConsentMissing',revision:4}}}});
      await pump(user);user.events=[];user.session='synthetic-new-leg';return {oldMediaReleased:true,newSession:true};
    }};
  // 只加速离线时钟，不改变正式20秒轮询/90秒扩展或真实中央的任何截止。
  Date.now=()=>clock;global.setTimeout=(fn,ms)=>{clock+=ms;queueMicrotask(fn);return 0;};
  try {await assert.rejects(()=>probe(context,{policy:'synthetic-policy',memberRejoin:true}),error=>{
    const observed=error.recordingDiagnostic?.staleConsentObservation;
    assert.ok(observed,'stale_consent_substage_required');
    if(mode==='afterAllPaused'){assert.equal(error,stop);assert.equal(observed.stage,'newOwnGrants');assert.equal(observed.outcome,'rejected');}
    else if(mode==='oldRejectUnexpected'){assert.equal(observed.stage,'rejectOld');assert.equal(observed.code,403);assert.equal(observed.outcome,'rejected');}
    else if(mode==='oldAccepted'){assert.equal(observed.stage,'rejectOld');assert.equal(observed.code,200);assert.equal(observed.outcome,'accepted');}
    else if(mode==='newGrantBadRevision'){assert.equal(observed.stage,'newOwnGrants');assert.equal(observed.newGrantFormat,true);assert.equal(observed.newGrantRevisionOne,false);assert.equal(observed.ownGrants,0);}
    else {assert.equal(observed.stage,'newIds');assert.equal(observed.ownGrants,3);assert.equal(observed.newIdsDistinct,false);}
    return true;
  });}
  finally {Date.now=originalDate;global.setTimeout=originalTimer;}
  assert.equal(pausedAcknowledged,true);assert.equal(rejoinedSources,true);assert.equal(callback,null);
  }
});

test('实际录制probe的finally保留首因，停录失败只能单列未知清理',async()=>{
  const users=[1,2,3].map(id=>({id:String(id),endpoint:`endpoint-${id}`})),inbox=new Map(users.map(user=>[user.id,[]]));
  const original=Error('formal_api_rejected');original.actualCode=409;
  const context={users,sourceSession:'source-session',snapshotMedia:async()=>[],activeMembers:async()=>{},
    api:async(user,path,body)=>{
      if(path==='im/interaction/events')return {list:inbox.get(user.id).splice(0)};
      if(path==='im/interaction/events/ack')return {};
      assert.equal(path,'im/interaction/commands');
      const command=body.command,selector=command.params.command,args=command.params.arguments;
      if(selector==='recordingControl'&&args.operation==='pause')throw original;
      if(selector==='recordingControl'&&args.operation==='stop')throw Error('PRIVATE-CLEANUP-DETAILS');
      const data=selector==='recordingConsent'?{consentId:String(user.id).repeat(64),revision:1,sourceTrackIds:['conference-audio']}:{state:'active',revision:1};
      inbox.get(user.id).push({deliveryId:`result-${command.msgToken}`,event:{sessionId:'source-session',method:'interaction.result',msgToken:command.msgToken,code:200,data}});
      if(selector==='recordingControl')for(const member of users)inbox.get(member.id).push({deliveryId:`state-${member.id}`,event:{sessionId:'source-session',method:'interaction.controlState',data:{command:'recordingState',result:{recordingId:args.recordingId,state:'active',revision:1}}}});
      return {accepted:true,msgToken:command.msgToken};
    }};
  await assert.rejects(()=>probe(context,{policy:'policy'}),error=>{
    assert.equal(error,original);
    assert.deepEqual(error.recordingDiagnostic,{stage:'explicit_pause',reason:'formal_api_rejected',code:409,cleanupFailures:['recording_stop_unconfirmed']});
    assert.equal(Object.keys(error).includes('recordingDiagnostic'),false);
    return true;
  });
});
