// 文件说明：正式会议自然恢复维护驱动的确定性反例；Mock只核判据，不冒充真实ICE或音频验收。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const {verifyFaultReceipt,configuration,observeResources,verifyResources,verifyAuthority,probe}=require('./conference_network_recovery_live.cjs');

// rows 提供四端显式对象观察；最后一端本地静音，与服务端MAC许可独立。
function rows() {
  return Array.from({length:4},(_,index)=>({samePeer:true,sameStream:true,sameTrack:true,sameRemote:true,senderBound:true,
    captureLive:true,remoteLive:true,enabled:index!==3,connectionState:'connected',iceConnectionState:'connected',firstDisconnectedAgeMs:null}));
}
// grants 是原IM可控证明，不使用同人数或原始用户ID作恢复许可。
function grants() {return Array.from({length:3},(_,index)=>({memberEpoch:index+1,audioRevision:index+3,publishAllowed:true}));}

test('自然恢复真实入口必须三个显式开关，不因其他会议测试自动开启',()=>{
  const env={UR_CONFERENCE_NETWORK_RECOVERY_LIVE:'1',UR_CONFERENCE_MEMBER_LIVE:'1',UR_CONFERENCE_FIXED_PCM_TEST:'1'};
  assert.equal(configuration(env),true);
  for(const key of Object.keys(env))for(const value of [undefined,'',0,'0','true'])assert.throws(()=>configuration({...env,[key]:value}));
});

test('原Peer/stream/track/remote/sender和本地静音必须全部保持',()=>{
  assert.equal(verifyResources(rows(),{connected:true}),true);
  for(const key of ['samePeer','sameStream','sameTrack','sameRemote','senderBound','captureLive','remoteLive']){
    const changed=rows();changed[0][key]=false;assert.throws(()=>verifyResources(changed));
  }
  for(const index of [0,3]){const changed=rows();changed[index].enabled=!changed[index].enabled;assert.throws(()=>verifyResources(changed));}
  for(const state of ['failed','closed','bogus']){const changed=rows();changed[0].iceConnectionState=state;assert.throws(()=>verifyResources(changed));}
  const disconnected=rows();disconnected[1].iceConnectionState='disconnected';disconnected[1].connectionState='disconnected';
  assert.equal(verifyResources(disconnected),true);assert.throws(()=>verifyResources(disconnected,{connected:true}));
  for(const changed of [[],rows().slice(1),[...rows(),rows()[0]],null])assert.throws(()=>verifyResources(changed));
});

test('三份原IM成员代次/MAC权限不能被同人数新实例或解禁替代',()=>{
  assert.equal(verifyAuthority(grants(),grants()),true);
  for(const key of ['memberEpoch','audioRevision','publishAllowed']){
    const changed=grants();changed[1][key]=key==='publishAllowed'?false:99;assert.throws(()=>verifyAuthority(grants(),changed));
  }
  for(const value of [0,-1,1.5,undefined]){const changed=grants();changed[0].memberEpoch=value;assert.throws(()=>verifyAuthority(changed,changed));}
  assert.throws(()=>verifyAuthority([],[]));
});

test('资源观察在原网页比较实际对象引用，替换任一对象不能仍报告相同',()=>{
  const track={readyState:'live',enabled:true},remote={readyState:'live'},stream={getAudioTracks:()=>[track]};
  const peer={getSenders:()=>[{track}],connectionState:'connected',iceConnectionState:'connected',addEventListener(){}};
  const view={peer,stream,remoteTrack:remote};
  const context=vm.createContext({window:{},views:new Map([['public-fixture',view]]),performance:{now:()=>100}});
  const evaluate=remember=>JSON.parse(JSON.stringify(vm.runInContext(`(${observeResources.toString()})('public-fixture',true,${remember})`,context)));
  assert.equal(evaluate(true).samePeer,true);view.peer={...peer};assert.equal(evaluate(false).samePeer,false);
  view.peer=peer;view.stream={...stream};assert.equal(evaluate(false).sameStream,false);
  view.stream=stream;view.remoteTrack={...remote};assert.equal(evaluate(false).sameRemote,false);
  view.remoteTrack=remote;track.enabled=false;assert.equal(evaluate(false).enabled,false);
});

// harness 仅在内存模拟回调取数，保存完整阶段以拒绝绕过实际解码阶段的假通过。
function harness() {
  const controller=new AbortController(),users=Array.from({length:3},(_,index)=>({index:index+1,id:String(index+1),name:`public-${index}`,env:{}}));
  let actual=rows(),clock=100,muted=false,authorityCalls=0,changedAuthority=false;
  const phases=[],calls=[];
  const context={users,sourceName:'public-device',sourceEnv:{},sourceSession:'public-fixture',callId:'public-call',signal:controller.signal,
    evaluated:JSON.parse,
    async browser(name,env,kind,body){
      calls.push(body);
      if(body.includes('observeResources('))return JSON.stringify(actual[name==='public-device'?0:users.find(user=>user.name===name).index]);
      if(body.includes('track.enabled=false'))muted=true;
      if(body.includes('track.enabled=true'))muted=false;
      return 'true';
    },
    async api(){const value=grants()[authorityCalls++%3];if(changedAuthority&&authorityCalls>3)value.memberEpoch++;return value;},
    async activeMembers(ids){assert.deepEqual(ids,['1','2','3']);},
    async phase(label,participants,allowed){phases.push({label,participants,allowed});},
    async snapshotMedia(remember){return Array.from({length:4},(_,index)=>({samePeer:true,connected:true,received:remember?10:20,sent:remember||index===3?10:20,samples:remember?100:200}));}
  };
  return {context,controller,phases,calls,get muted(){return muted;},clock:()=>clock,
    time(value){clock=value;},states(value){actual=value;},authorityChanged(){changedAuthority=true;},
    async fault({observeDisconnected}){
      actual=rows().map(value=>({...value,connectionState:'disconnected',iceConnectionState:'disconnected',firstDisconnectedAgeMs:0}));
      assert.equal((await observeDisconnected()).allDisconnected,true);clock=150;
      actual=rows();return {kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:7500};
    }};
}

test('完整判据先真实暂断观察，再同Peer静音混音，最后显式取消静音四源验证',async()=>{
  const h=harness(),result=await probe(h.context,{fault:h.fault,clock:h.clock});
  assert.equal(result.report.actualDisconnectedEndpoints,4);assert.equal(result.report.actualRecoveryMs,50);
  assert.equal(result.report.mqttReauthenticated,false);assert.equal(result.report.mediaRenegotiated,false);
  assert.equal(h.muted,false);assert.equal(h.phases.length,3);
  assert.deepEqual(h.phases.map(value=>value.allowed),[[0,1,2],[0,1,2],[0,1,2,3]]);
  for(const key of ['public-fixture','public-call','public-device'])assert.equal(JSON.stringify(result).includes(key),false);
});

test('只有媒体隔离回执但未见真实ICE disconnected不能冒充恢复',async()=>{
  const h=harness();await assert.rejects(()=>probe(h.context,{fault:async()=>({kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:3000}),clock:h.clock}),/four_actual_ice_disconnections_required/);
  assert.equal(h.muted,false);assert.equal(h.phases.length,1);
});

test('真实事件年龄计入首次窗口，不以四端晚轮询时刻重新给五秒',async()=>{
  for(const age of [null,5000]){
    const h=harness();
    await assert.rejects(()=>probe(h.context,{clock:h.clock,fault:async context=>{
      h.states(rows().map(value=>({...value,connectionState:'disconnected',iceConnectionState:'disconnected',firstDisconnectedAgeMs:age})));
      await context.observeDisconnected();return {kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:7500};
    }}));
    assert.equal(h.muted,false);
  }
});

test('只三端失联、窗口精确到期、旧Peer替换及原IM代次变化均拒绝',async()=>{
  for(const mode of ['three','expired','replaced','authority']){
    const h=harness();
    const fault=async context=>{
      const disconnected=rows().map((value,index)=>({...value,connectionState:index===3&&mode==='three'?'connected':'disconnected',iceConnectionState:index===3&&mode==='three'?'connected':'disconnected',firstDisconnectedAgeMs:0}));
      h.states(disconnected);await context.observeDisconnected();
      const recovered=rows();if(mode==='replaced')recovered[0].samePeer=false;
      h.states(recovered);h.time(mode==='expired'?5100:150);if(mode==='authority')h.authorityChanged();
      return {kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:7500};
    };
    await assert.rejects(()=>probe(h.context,{fault,clock:h.clock}));assert.equal(h.muted,false);
  }
});

test('故障回执固定不收进程ID、自由字段或未恢复状态，失败仍撤销用例本地静音',async()=>{
  for(const mutation of [{pid:123},{restored:false},{interrupted:false},{elapsedMs:0},{elapsedMs:20001},{stopped:true},{kind:'unknown'}]){
    const h=harness();await assert.rejects(()=>probe(h.context,{clock:h.clock,fault:async context=>({...await h.fault(context),...mutation})}));
    assert.equal(h.muted,false);
  }
});

test('取消必须退出当前观察，不把后续恢复解码阶段留后台执行',async()=>{
  const h=harness();await assert.rejects(()=>probe(h.context,{clock:h.clock,fault:async context=>{await h.fault(context);h.controller.abort(Error('fixed-cancel'));return {kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:7500};}}),/conference_network_recovery_cancelled/);
  assert.equal(h.muted,false);assert.equal(h.phases.length,1);
});

test('清理失败不能覆盖真实故障首因，报告仅固定阶段和失败计数',async()=>{
  const h=harness(),original=h.context.browser;
  h.context.phase=async()=>{throw Error('primary_fixed');};
  h.context.browser=async(...args)=>{
    if(args[3].includes('removeEventListener'))throw Error('private-cleanup-details');
    return original(...args);
  };
  await assert.rejects(()=>probe(h.context,{fault:h.fault,clock:h.clock}),error=>{
    assert.equal(error.message,'conference_network_recovery_operation_failed');
    assert.deepEqual(error.networkRecoveryDiagnostic,{phase:'baseline_muted_decode',reason:'operation_failed',cleanupFailures:4});
    assert.equal(error.cause.message,'primary_fixed');
    assert.equal(JSON.stringify(error).includes('private-cleanup-details'),false);
    return true;
  });
  assert.equal(h.muted,false);
});

test('首因中的自由正文不反射，真实窗口错误保留固定失败枚举',async()=>{
  const h=harness();
  await assert.rejects(()=>probe(h.context,{clock:h.clock,fault:async()=>{throw Error('secret-address-model-body');}}),error=>{
    assert.equal(error.message,'conference_network_recovery_operation_failed');
    assert.equal(error.networkRecoveryDiagnostic.phase,'fault_disconnection');
    assert.equal(error.networkRecoveryDiagnostic.reason,'operation_failed');
    assert.equal(JSON.stringify(error).includes('secret-address-model-body'),false);
    return true;
  });
});

// timedHarness 在同单调时基中模拟四端独立CLI；年龄取自调用内采样时刻，记录开始/采样/返回真实范围。
// 首端10000ms失联、其余13900ms失联；传输耗时必须消耗原五秒，不属于可重开的恢复预算。
function timedHarness({restoreCost,faultCost=200,sampleOffset=0}) {
  const h=harness(),originalBrowser=h.context.browser,events=[10000,13900,13900,13900],snapshots=[];
  let mode='baseline'; // 只有故障与恢复取数模拟CLI耗时，不改变原许可、混音阶段及静音清理。
  h.time(9000);
  h.context.browser=async(name,env,kind,body)=>{
    if(!body.includes('observeResources('))return originalBrowser(name,env,kind,body);
    const index=name==='public-device'?0:h.context.users.find(user=>user.name===name).index;
    const startedAt=h.clock(),cost=mode==='fault'?faultCost:mode==='restore'?restoreCost:0;
    const sampledAt=startedAt+(mode==='fault'?sampleOffset:0),returnedAt=startedAt+cost;
    assert.ok(startedAt<=sampledAt&&sampledAt<=returnedAt,'snapshot_must_be_inside_actual_cli_call');
    h.states(rows().map((value,i)=>({...value,
      connectionState:mode==='fault'?'disconnected':'connected',iceConnectionState:mode==='fault'?'disconnected':'connected',
      firstDisconnectedAgeMs:mode==='baseline'?null:sampledAt-events[i]})));
    const result=await originalBrowser(name,env,kind,body);h.time(returnedAt);
    snapshots.push({mode,index,startedAt,sampledAt,returnedAt,eventAt:events[index]});return result;
  };
  // run 沿用完整正式probe，不替换其时间判据；失败仍执行原finally并恢复用例主动设置的本地静音。
  const run=()=>probe(h.context,{clock:h.clock,wait:async ms=>h.time(h.clock()+ms),fault:async({observeDisconnected})=>{
    mode='fault';h.time(14000);await observeDisconnected();mode='restore';
    return{kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:4800};
  }});
  return{h,run,snapshots};
}

test('串行CLI不能把真实5400ms恢复后移为4600ms并误收',async()=>{
  const {h,run,snapshots}=timedHarness({restoreCost:1200});
  await assert.rejects(run,error=>error.message==='conference_network_recovery_natural_recovery_window_expired'&&error.networkRecoveryDiagnostic.phase==='restored_resources');
  assert.equal(h.clock()-10000,5400);assert.equal(h.muted,false);assert.equal(h.phases.length,1);
  assert.deepEqual(snapshots.filter(row=>row.mode==='fault').map(({startedAt,sampledAt,returnedAt})=>[startedAt,sampledAt,returnedAt]),
    [[14000,14000,14200],[14000,14000,14200],[14000,14000,14200],[14000,14000,14200]]);
});

test('串行取数后的恢复精确5000ms仍到期，不能借整批结束时刻通过',async()=>{
  const {h,run}=timedHarness({restoreCost:800});
  await assert.rejects(run,/conference_network_recovery_natural_recovery_window_expired/);
  assert.equal(h.clock()-10000,5000);assert.equal(h.muted,false);assert.equal(h.phases.length,1);
});

test('调用开始即采样的真实4999ms恢复保留原三阶段合法通过',async()=>{
  const {h,run}=timedHarness({restoreCost:799}),result=await run();
  assert.equal(h.clock()-10000,4999);assert.equal(result.report.actualRecoveryMs,4999);
  assert.equal(h.muted,false);assert.deepEqual(h.phases.map(value=>value.allowed),[[0,1,2],[0,1,2],[0,1,2,3]]);
});

test('四端断线观察本身已消耗5200ms时必须在故障观察阶段拒绝',async()=>{
  const {h,run,snapshots}=timedHarness({restoreCost:25,faultCost:1200});
  await assert.rejects(run,error=>error.message==='conference_network_recovery_natural_recovery_window_expired'&&error.networkRecoveryDiagnostic.phase==='fault_observation');
  assert.equal(h.clock()-10000,5200);assert.equal(h.muted,false);assert.equal(h.phases.length,1);
  assert.equal(snapshots.some(row=>row.mode==='restore'),false);
});

test('网页在CLI调用内100ms采样时使用保守下界，不把往返延迟当新预算',async()=>{
  const {h,run,snapshots}=timedHarness({restoreCost:1200,sampleOffset:100});
  await assert.rejects(run,/conference_network_recovery_natural_recovery_window_expired/);
  assert.equal(h.clock()-10000,5400);assert.equal(h.muted,false);assert.equal(h.phases.length,1);
  const first=snapshots.find(row=>row.mode==='fault');
  assert.deepEqual([first.startedAt,first.sampledAt,first.returnedAt],[14000,14100,14200]);
  assert.equal(first.startedAt-(first.sampledAt-first.eventAt),9900); // 事件下界提前100ms，仅收紧而不延长真实五秒。
});

// parallelHarness 用确定性批次模拟独立CLI及五秒源关闭；不替换正式probe，不冒充真实浏览器。
async function parallelHarness({faultCost=200,restoreCost=500,failIndex=null,cancel=false,duplicate=false,expireSource=true}={}){
  const controller=new AbortController(),users=Array.from({length:3},(_,i)=>({index:i+1,name:`public-${i+1}`,id:String(i+1),env:{}}));
  if(duplicate)users[1].name=users[0].name;
  const traces=[],phases=[],events=[10000,13900,13900,13900];let now=9000,mode='baseline',pending=[],scheduled=false,active=0,maxRead=0,maxInstall=0,muted=false,cleanupWhileActive=false,actionWhileActive=false;
  // flush 完成已实际发起的一批请求；兄弟拒绝不会令未完成请求消失。
  function flush(){const group=pending;pending=[];scheduled=false;now=Math.max(now,...group.map(item=>item.end));for(const item of group){active--;item.trace.settledAt=now;if(item.error)item.reject(item.error);else item.resolve(JSON.stringify(item.row));}}
  const context={users,sourceName:'public-source',sourceEnv:{},sourceSession:'public-session',callId:'public-call',signal:controller.signal,evaluated:JSON.parse,
    browser(name,env,kind,body){
      assert.equal(kind,'eval');
      if(!body.includes('observeResources(')){actionWhileActive ||= active>0;if(body.includes('removeEventListener'))cleanupWhileActive ||= active>0;if(body.includes('track.enabled=false'))muted=true;if(body.includes('track.enabled=true'))muted=false;return Promise.resolve('true');}
      const index=name==='public-source'?0:users.find(user=>user.name===name).index,install=body.endsWith(',true)');
      const begin=now,cost=mode==='fault'?faultCost:mode==='restore'?restoreCost:0,end=begin+cost,sample=mode==='restore'?end:begin;
      const row={samePeer:true,sameStream:true,sameTrack:true,sameRemote:true,senderBound:true,captureLive:true,remoteLive:true,enabled:index!==3,
        connectionState:mode==='fault'?'disconnected':'connected',iceConnectionState:mode==='fault'?'disconnected':'connected',firstDisconnectedAgeMs:mode==='baseline'?null:sample-events[index]};
      const trace={mode,index,install,begin,sample,end};traces.push(trace);active++;if(install)maxInstall=Math.max(maxInstall,active);else maxRead=Math.max(maxRead,active);
      let error=null;if(mode==='restore'&&((expireSource&&index===0&&sample>=15000)||index===failIndex))error=Error('browser_eval_failed');
      if(mode==='restore'&&cancel&&index===1)controller.abort(Error('controlled_cancel'));
      // 第二端立即拒绝，而其余已启动原请求尚未settle；候选必须等待兄弟结束再进入finally。
      if(mode==='restore'&&index===failIndex){active--;trace.settledAt=now;return Promise.reject(error);}
      return new Promise((resolve,reject)=>{pending.push({end,row,error,resolve,reject,trace});if(!scheduled){scheduled=true;setImmediate(flush);}});
    },async api(user){return{memberEpoch:user.index,audioRevision:1,publishAllowed:true};},async activeMembers(){},async phase(label){phases.push(label);},
    async snapshotMedia(remember){return Array.from({length:4},(_,i)=>({samePeer:true,connected:true,received:remember?10:20,sent:remember||i===3?10:20,samples:remember?100:200}));}};
  let accepted=false,reported=null,diagnostic=null;
  try{const result=await probe(context,{clock:()=>now,wait:async ms=>{now+=ms;},fault:async({observeDisconnected})=>{mode='fault';now=14000;await observeDisconnected();mode='restore';return{kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:4800};}});accepted=true;reported=result.report.actualRecoveryMs;}catch(error){diagnostic=error.networkRecoveryDiagnostic;}
  return{accepted,reported,diagnostic,actualMs:now-10000,maxRead,maxInstall,muted,cleanupWhileActive,actionWhileActive,active,traces,phases};
}

test('仅四独立session的只读快照并发，安装观察器与静音等动作仍串行',async()=>{
  const result=await parallelHarness();
  assert.equal(result.accepted,true);assert.equal(result.reported,4700);assert.equal(result.actualMs,4700);
  assert.equal(result.maxInstall,1);assert.equal(result.maxRead,4);assert.equal(result.phases.length,3);
  assert.equal(result.active,0);assert.equal(result.actionWhileActive,false);assert.equal(result.muted,false);
  assert.deepEqual(result.traces.filter(row=>row.mode==='fault').map(row=>[row.begin,row.sample,row.end]),
    [[14000,14000,14200],[14000,14000,14200],[14000,14000,14200],[14000,14000,14200]]);
});

test('只读并发不能延长源五秒窗口，实际5000ms仍拒绝',async()=>{
  const result=await parallelHarness({restoreCost:800,expireSource:false});
  assert.equal(result.accepted,false);assert.equal(result.actualMs,5000);
  assert.equal(result.diagnostic.reason,'natural_recovery_window_expired');
  assert.equal(result.active,0);assert.equal(result.cleanupWhileActive,false);assert.equal(result.muted,false);
});

test('一端立即拒绝必须等同批其他读取实际结束，不能让finally提前释放',async()=>{
  const result=await parallelHarness({failIndex:1}),traces=result.traces.filter(row=>row.mode==='restore');
  assert.equal(result.accepted,false);assert.equal(result.diagnostic.reason,'browser_eval_failed');
  assert.ok(traces.find(row=>row.index===1).settledAt<traces.find(row=>row.index===0).settledAt);
  assert.equal(result.active,0);assert.equal(result.cleanupWhileActive,false);assert.equal(result.actionWhileActive,false);
  assert.equal(result.muted,false);
});

test('读取批次中取消必须等待已启动原请求settle，不留后台或继续混音阶段',async()=>{
  const result=await parallelHarness({cancel:true});
  assert.equal(result.accepted,false);assert.equal(result.diagnostic.reason,'cancelled');
  assert.equal(result.active,0);assert.equal(result.cleanupWhileActive,false);assert.equal(result.actionWhileActive,false);
  assert.equal(result.phases.length,1);assert.equal(result.muted,false);
});

test('重复browser session在任何资源read前拒绝，不复用同会话假造四端',async()=>{
  const result=await parallelHarness({duplicate:true});
  assert.equal(result.accepted,false);assert.equal(result.traces.length,0);
  assert.equal(result.active,0);assert.equal(result.cleanupWhileActive,false);assert.equal(result.muted,false);
});

// 闭集回执只验证端口事实，不假造浏览器事件；正常media不得要求backend stopped。
test('两种fault回执严格闭集、20秒边界、未知或旧无kind回执拒绝',()=>{
  assert.equal(verifyFaultReceipt({kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:20000}),true);
  assert.equal(verifyFaultReceipt({kind:'backend_pause',stopped:true,resumed:true,pauseElapsedMs:20000}),true);
  for(const value of [null,{}, {stopped:true,resumed:true,pauseElapsedMs:1000},
    {kind:'backend_pause',stopped:false,resumed:true,pauseElapsedMs:1000},
    {kind:'backend_pause',stopped:true,resumed:false,pauseElapsedMs:1000},
    {kind:'owned_media_udp_tuple_drop',interrupted:true,restored:true,elapsedMs:1000,stopped:true}])assert.throws(()=>verifyFaultReceipt(value));
  for(const elapsed of [0,-1,NaN,Infinity,20001])for(const kind of ['backend_pause','owned_media_udp_tuple_drop'])assert.throws(()=>verifyFaultReceipt(kind==='backend_pause'?{kind,stopped:true,resumed:true,pauseElapsedMs:elapsed}:{kind,interrupted:true,restored:true,elapsedMs:elapsed}));
});

test('同Peer全部恢复的backend暂停仍是负向，不进入恢复解码或伪媒体PASS',async()=>{
  const h=harness();await assert.rejects(()=>probe(h.context,{clock:h.clock,fault:async port=>{
    await h.fault(port);return {kind:'backend_pause',stopped:true,resumed:true,pauseElapsedMs:7500};
  }}),error=>{assert.equal(error.message,'conference_network_recovery_backend_pause_not_media_recovery');assert.equal(error.networkRecoveryDiagnostic.phase,'fault_receipt');return true;});
  assert.equal(h.phases.length,1);assert.equal(h.muted,false);
});
