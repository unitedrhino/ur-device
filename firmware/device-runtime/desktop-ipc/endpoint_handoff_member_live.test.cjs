// 文件说明：接续driver的清理责任、同成员替换及真实HAL终态判据，离线测试不冒充端云验收。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {run,selectedMember,validateReplacement,lifecycleStopped,handoffFailureDiagnostic}=require('./endpoint_handoff_member_live.cjs');

// fixture 返回公开合成同成员新旧腿，不包含真实令牌或环境地址。
function fixture(){
  const env={};const original={id:'2',token:'synthetic',index:2,env,name:'old-browser',endpoint:'old-endpoint',session:'old-session',state:'ended',ended:true,stateTimeline:['established','ended']};
  return {original,target:{...original,name:'new-browser',endpoint:'new-endpoint',session:'new-session',state:'established',ended:false,events:[],stateTimeline:['negotiating','established']},started:new Set(['new-browser'])};
}
test('only same original IM member may replace extra APP leg',()=>{
  const {original,target,started}=fixture();assert.equal(validateReplacement(original,target,started),true);
  for(const key of ['id','token','index','env','endpoint','session','name']){
    const changed={...target,[key]:['endpoint','session','name'].includes(key)?original[key]:'other'};
    assert.throws(()=>validateReplacement(original,changed,started));
  }
});
test('old terminal must be unique and target cleanup ownership retained',()=>{
  const {original,target,started}=fixture();assert.throws(()=>validateReplacement({...original,stateTimeline:['ended','ended']},target,started));
  assert.throws(()=>validateReplacement(original,target,new Set()));
  assert.throws(()=>validateReplacement({...original,ended:false},target,started));
  assert.throws(()=>validateReplacement(original,{...target,events:new Array(257)},started));
});
test('HAL proof requires actual track and Peer stop after terminal',()=>{
  const facts={terminalObserved:true,peerClosed:true,capture:['ended'],remote:'ended',spectrumClosed:true,audioClosed:true,sinkReleased:true};
  assert.equal(lifecycleStopped(facts),true);
  for(const changed of [{terminalObserved:false},{peerClosed:false},{capture:[]},{capture:['live']},{remote:'live'},{spectrumClosed:false},{audioClosed:false},{sinkReleased:false}])assert.throws(()=>lifecycleStopped({...facts,...changed}));
});
test('no live module execution without explicit opt in',async()=>{
  const original=process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE;
  delete process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE;
  const initial=process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE;delete process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE;
  try{await assert.rejects(run({}),/explicit_single_member_handoff_required/);}finally{if(original!==undefined)process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE=original;if(initial!==undefined)process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE=initial;}
});
test('initial controller uses original index1 and keeps exact identity',()=>{
  const {original,target,started}=fixture();original.index=target.index=1;
  assert.equal(validateReplacement(original,target,started),true);
  assert.equal(selectedMember({UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE:'1'}),0);
  assert.equal(selectedMember({UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE:'1'}),1);
  assert.throws(()=>selectedMember({}));
  assert.throws(()=>selectedMember({UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE:'1',UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE:'1'}));
  assert.throws(()=>validateReplacement({...original,index:3},{...target,index:3},started));
});

test('handoff diagnostics expose only fixed phases and cleanup flag',()=>{
  assert.deepEqual(handoffFailureDiagnostic({handoffPhase:'commit',handoffCleanupFailed:true,token:'secret',message:'private'}),{phase:'commit',cleanupFailed:true});
  assert.deepEqual(handoffFailureDiagnostic({handoffPhase:'secret-device',handoffCleanupFailed:'yes'}),{phase:null,cleanupFailed:false});
  assert.deepEqual(handoffFailureDiagnostic(null),{phase:null,cleanupFailed:false});
});

test('formal registration failure preserves original API code and reports phase',async()=>{
  const previous=process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE,extra=process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE;
  process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE='1';delete process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE;
  const failure=Object.assign(Error('formal_api_rejected'),{httpStatus:200,actualCode:100021});
  const users=[1,2,3].map(index=>({index,id:String(index),state:'established',ended:false}));
  try {
    await assert.rejects(run({users,api:async()=>{throw failure;},startedBrowsers:new Set(),signal:new AbortController().signal,installSpectrum:()=>{}}),error=>{
      assert.equal(error,failure);assert.equal(error.actualCode,100021);
      assert.deepEqual(handoffFailureDiagnostic(error),{phase:'register',cleanupFailed:false});return true;
    });
  } finally {
    if(previous===undefined)delete process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE;else process.env.UR_IPC_ENDPOINT_HANDOFF_INITIAL_LIVE=previous;
    if(extra!==undefined)process.env.UR_IPC_ENDPOINT_HANDOFF_MEMBER_LIVE=extra;
  }
});
