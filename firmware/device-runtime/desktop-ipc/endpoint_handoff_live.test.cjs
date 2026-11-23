// 文件说明：接续验收器判据的离线回归；伪ready、仅握手、单边停流和错误事务均不得通过。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {configuration,handoffState,decodedProgress,deviceTransferFacts}=require('./endpoint_handoff_live.cjs');

// fixture 仅提供不可连接的占位配置，离线测试不会启动浏览器或设备进程。
function fixture(){return {UR_IPC_ENDPOINT_HANDOFF_LIVE:'1',UR_IPC_LIVE:'1',UR_IPC_BASE_URL:'http://127.0.0.1:19001',UR_IPC_DEVICE_PORT:'19002',
  UR_IPC_APP_TOKEN:'placeholder',UR_DEVICE_PRODUCT:'placeholder',UR_DEVICE_NAME:'placeholder',UR_DEVICE_SECRET:'placeholder',UR_IPC_DEVICE_BIN:'/placeholder/device'};}
test('真实执行必须显式本机双开关，操作与准入策略均严格',()=>{
  const stat=()=>({isFile:()=>true}),env=fixture();assert.equal(configuration(env,stat).operation,'commit');
  for(const operation of ['commit','abort','timeout'])assert.equal(configuration({...env,UR_IPC_ENDPOINT_HANDOFF_OPERATION:operation},stat).operation,operation);
  for(const change of [{UR_IPC_ENDPOINT_HANDOFF_LIVE:'0'},{UR_IPC_LIVE:'0'},{UR_IPC_ENDPOINT_HANDOFF_OPERATION:'ready'},
    {UR_IPC_BASE_URL:'https://127.0.0.1:19001'},{UR_IPC_BASE_URL:'http://example.invalid'},
    {UR_IPC_BASE_URL:'http://127.0.0.1:19001/path'},{UR_IPC_BASE_URL:'http://user:secret@127.0.0.1:19001'},
    {UR_IPC_DEVICE_PORT:'0'},{UR_DEVICE_SECRET:''},{UR_IPC_ADMISSION:'automatic'}])assert.throws(()=>configuration({...env,...change},stat));
  assert.throws(()=>configuration(env,()=>({isFile:()=>false})));
});
test('ready必须真实原事务事件且两套水位独立，不能以准备或错误候选冒充',()=>{
  const event={msgToken:'event',method:'endpointState',sessionId:'source',eventSeq:50,data:{state:'ready',revision:2,handoffId:'original',candidateSessionId:'candidate'}};
  assert.equal(handoffState(event,'source','original','ready','candidate'),true);
  for(const change of [{sessionId:'other'},{method:'interaction.state'},{eventSeq:0},{code:200},{data:{...event.data,state:'preparing'}},
    {data:{...event.data,handoffId:'other'}},{data:{...event.data,candidateSessionId:'other'}},{data:{...event.data,revision:0}}])
    assert.equal(handoffState({...event,...change},'source','original','ready','candidate'),false);
});
test('超时必须主动事实，abort必须用户取消，准备不得携带候选',()=>{
  const event={method:'endpointState',sessionId:'source',eventSeq:9,data:{state:'failed',revision:3,handoffId:'h',reason:'handoffTimeout'}};
  assert.equal(handoffState(event,'source','h','failed'),true);
  assert.equal(handoffState({...event,code:200,eventSeq:undefined},'source','h','failed'),false);
  assert.equal(handoffState({...event,data:{...event.data,reason:'userCancelled'}},'source','h','failed'),false);
  const abort={method:'endpointState',sessionId:'source',code:200,data:{state:'aborted',revision:3,handoffId:'h',reason:'userCancelled'}};
  assert.equal(handoffState(abort,'source','h','aborted'),true);
  assert.equal(handoffState({...abort,data:{...abort.data,reason:'handoffTimeout'}},'source','h','aborted'),false);
  const preparing={...abort,data:{state:'preparing',revision:1,handoffId:'h'}};
  assert.equal(handoffState(preparing,'source','h','preparing'),true);
  assert.equal(handoffState({...preparing,data:{...preparing.data,candidateSessionId:'candidate'}},'source','h','preparing'),false);
});
test('保留端必须实际新增两个解码帧且非黑画面，不只握手或接收包',()=>{
  const before={frames:10,packets:20},after={frames:12,packets:23,pixels:1,connected:true};
  assert.equal(decodedProgress(before,after),true);
  for(const change of [{frames:10},{frames:11},{packets:20},{pixels:0},{connected:false},{frames:12.5}])assert.equal(decodedProgress(before,{...after,...change}),false);
});
test('停旧必须原Peer关闭与轨结束，保留端不可重建或停止采集',()=>{
  const good={stoppedPresent:false,stoppedPeer:'closed',stoppedTracks:1,stoppedEnded:true,retainedPresent:true,sameRetainedPeer:true,
    sameRetainedStream:true,retainedActive:true,retainedConnected:true,retainedTracks:1,retainedLive:true};
  assert.equal(deviceTransferFacts(good),true);
  for(const change of [{stoppedPresent:true},{stoppedPeer:'connected'},{stoppedTracks:0},{stoppedEnded:false},{retainedPresent:false},
    {sameRetainedPeer:false},{sameRetainedStream:false},{retainedActive:false},{retainedConnected:false},{retainedTracks:0},{retainedLive:false}])
    assert.equal(deviceTransferFacts({...good,...change}),false);
});
