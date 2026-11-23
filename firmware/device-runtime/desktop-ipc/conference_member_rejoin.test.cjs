// 文件说明：成员重入维护驱动顺序及拒绝判据；不把离线端口调用当作真实会议联调。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {rejoinConferenceMember,invokeEstablishedHook}=require('./conference_member_live.cjs');

// fixture 显式记录真实端口所需顺序，仅用于验证驱动不会绕过旧终态和媒体释放。
function fixture(change={}){
  const calls=[],user={index:3,id:'3',session:'original',state:'established',stateTimeline:[],events:[],ended:false};let reads=0;
  const step=(name,fn=()=>{})=>async(...args)=>{calls.push(name);return fn(...args);};
  const ports={access:step('access',()=>({memberEpoch:++reads===1?10:20})),end:step('end'),terminal:step('terminal',u=>{u.state='ended';}),
    close:step('close',()=>({peerClosed:true,captureStopped:true,remoteStopped:true})),leave:step('leave'),remaining:step('remaining'),join:step('join'),
    open:step('open',u=>{assert.equal(u.session,undefined);assert.equal(u.ended,true);u.session='fresh';}),negotiate:step('negotiate',u=>{u.state='established';}),members:step('members')};
  return {user,calls,ports:{...ports,...change}};
}
test('显式离会重入必须旧终态和实际释放先于正式IM与新腿',async()=>{
  const f=fixture(),result=await rejoinConferenceMember(f.user,f.ports);
  assert.deepEqual(f.calls,['access','end','terminal','close','leave','remaining','join','access','open','negotiate','members']);
  assert.deepEqual(result,{oldMediaReleased:true,originalMemberEpoch:10,currentMemberEpoch:20,newSession:true});assert.equal(f.user.ended,false);assert.equal(f.user.session,'fresh');
});
test('未停实际媒体或未推进加入实例不得宣称重入，后续新腿不运行',async()=>{
  for(const change of [{close:async()=>({peerClosed:false,captureStopped:true,remoteStopped:true})},{access:async()=>({memberEpoch:10})}]){
    const f=fixture(change);await assert.rejects(rejoinConferenceMember(f.user,f.ports));assert.equal(f.calls.includes('open'),false);
  }
});
test('hook向重入端口传同一取消信号并await，不在后台继续提交',async()=>{
  let observed,completed=false;
  await assert.rejects(invokeEstablishedHook(async ctx=>{await ctx.rejoinMember({index:3});},{rejoinMember:async(user,signal)=>{observed=signal;await new Promise(resolve=>signal.addEventListener('abort',resolve,{once:true}));completed=true;signal.throwIfAborted();}},5));
  assert.equal(observed.aborted,true);assert.equal(completed,true);
});
