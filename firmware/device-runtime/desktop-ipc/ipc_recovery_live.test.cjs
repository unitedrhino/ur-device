// 文件说明：恢复验收判据必须拒绝仅握手、旧帧、黑画面和断开的Peer；实际网络另由正式E2E验证。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {recoveredVideo,verify}=require('./ipc_recovery_live.cjs');
test('恢复要求至少两个新解码帧与实际像素和新网络包',()=>{
  const before={frames:10,packets:20},after={frames:12,packets:21,pixels:200,connected:true};
  assert.equal(recoveredVideo(before,after),true);
  for(const bad of [{frames:10},{frames:11},{frames:12.5},{packets:20},{pixels:0},{connected:false}])
    assert.equal(recoveredVideo(before,{...after,...bad}),false);
  assert.equal(recoveredVideo(undefined,after),false);
});
test('未显式启用或没有精确归属故障入口时先拒绝，不能启动网络',async()=>{
  await assert.rejects(verify({env:{},viewers:[]},()=>{}),/explicit_recovery_test_required/);
  await assert.rejects(verify({env:{UR_IPC_RECOVERY_LIVE:'1'},viewers:[]}),/owned_network_fault_required/);
});
test('新SSRC可清零但必须相对新代次基线继续实际解码',()=>{
  const old={frames:200,packets:1000},baseline={frames:1,packets:4},after={frames:3,packets:8,pixels:200,connected:true};
  assert.equal(recoveredVideo(old,after),false);
  assert.equal(recoveredVideo(baseline,after),true);
  assert.equal(recoveredVideo(baseline,{...after,frames:1}),false);
});
test('已停音源恢复必须显式状态与正式源控制，不能凭空改驱动事实',async()=>{
  const context={env:{UR_IPC_RECOVERY_LIVE:'1',UR_IPC_RECOVERY_SOURCE_STATE:'stopped'},viewers:[{},{},{}]};
  await assert.rejects(verify(context,()=>{}),/formal_source_control_required/);
  await assert.rejects(verify({...context,env:{...context.env,UR_IPC_RECOVERY_SOURCE_STATE:'typo'}},()=>{},()=>{}),/invalid_recovery_source_state/);
});
