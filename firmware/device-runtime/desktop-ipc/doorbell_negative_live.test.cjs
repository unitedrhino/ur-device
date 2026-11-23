// 文件说明：负向门铃编排的离线断言；可控时钟不代表真实超时或媒体已验证。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {terminal,sourceIdle,verifyNegatives}=require('./doorbell_negative_live.cjs');

// fixture 只模拟正式接口响应和原端点事实，便于逐个注入错误；不启动网络或设备。
function fixture(fault=''){
  let now=1000,muted=0,round=0,expires=0;
  const events=new Map(),endpoints=['a','b','c'];
  const ended=(endpoint,reason)=>events.set(`${endpoint}:interaction.state:${round}`,{
    method:'interaction.state',msgToken:`state-${round}`,sessionId:`session-${round}-${endpoint}`,eventSeq:2,data:{state:'ended',reason}});
  const idle={available:true,closed:false,views:0,retiring:0,invitations:0,humanSessionEmpty:true,newCaptures:0};
  const options={events,endpoints,previousGroups:new Set(),product:'p',device:'d',camera:'front',clock:()=>now,
    delay:async ms=>{now+=ms;},
    api:async(path,body)=>{
      if(path==='im/channel/get-list')return {list:[{id:1,channelType:'device',channelId:'p:d',isMuted:muted}]};
      assert.equal(path,'im/channel/set-mute');muted=body.isMuted;
      if(fault==='mute_ack_lost'&&muted===1)throw Error('original_ack_lost');
    },
    press:async expected=>{assert.equal(expected,muted?'rejected':'success');if(muted)return true;round++;expires=now+30000;
      for(const endpoint of endpoints)events.set(`${endpoint}:interaction.invite:invite-${round}-${endpoint}`,{
        method:'interaction.invite',msgToken:`invite-${round}-${endpoint}`,sessionId:`session-${round}-${endpoint}`,
        data:{endpointId:endpoint,callGroupId:`group-${round}`,invitation:{expiresAt:expires}}});
      return fault!=='press_not_sent';
    },
    receive:async()=>{if(round===2&&(now>=expires||fault==='early_timeout'))for(const endpoint of endpoints)ended(endpoint,'ringTimeout');},
    validateInvite:event=>event,
    sourceFacts:async()=>({...idle,...(fault==='capture'&&muted?{newCaptures:1}:{})}),
    command:async(endpoint,session,method,params)=>{
      assert.equal(session,`session-${round}-${endpoint}`);assert.equal(method,'interaction.control');
      if(params.command==='sessionReject'){
        ended(endpoint,fault==='wrong_terminal'?'alreadyAnswered':'rejected');
        return {code:200,data:{result:{state:'ended'}}};
      }
      assert.equal(params.command,'sessionAccept');
      return {code:410,data:{reason:fault==='wrong_expiry'?'alreadyAnswered':'expired'}};
    },
    waitFor:async(predicate,budget=30000)=>{
      const deadline=now+budget;
      while(now<deadline){if(await predicate())return;now+=250;}
      throw Error('original_fact_timeout');
    }};
  return {options,muted:()=>muted};
}

test('只接受原腿明确终态和实际零采集，ACK或历史腿不足以证明停止',()=>{
  const event={method:'interaction.state',msgToken:'state',sessionId:'original',eventSeq:1,data:{state:'ended',reason:'rejected'}};
  assert.equal(terminal(event,'original','rejected'),true);
  for(const changed of [{code:200},{eventSeq:0},{sessionId:'old'},{method:'interaction.result'},
    {data:{state:'ended',reason:'ringTimeout'}}])assert.equal(terminal({...event,...changed},'original','rejected'),false);
  const good={available:true,closed:false,views:0,retiring:0,invitations:0,humanSessionEmpty:true,newCaptures:0};
  assert.equal(sourceIdle(good),true);
  for(const changed of [{newCaptures:1},{available:false},{closed:true},{views:1},{retiring:1},{invitations:1},{humanSessionEmpty:false}])
    assert.equal(sourceIdle({...good,...changed}),false);
});

test('全流程必须恢复静音配置，三端全拒和真实期限后重放仍拒绝，下一组可用',async()=>{
  const value=fixture();
  assert.deepEqual(await verifyNegatives(value.options),{quiet:{passed:true,restored:true},allRejected:true,
    timeout:true,expiredReplay:true,postTimeoutReusable:true});
  assert.equal(value.muted(),0);assert.equal(value.options.previousGroups.size,3);
});

test('静音写入已发生但ACK丢失也必须正式读回恢复',async()=>{
  const value=fixture('mute_ack_lost');await assert.rejects(verifyNegatives(value.options),/original_ack_lost/);
  assert.equal(value.muted(),0);
});

for(const fault of ['capture','wrong_terminal','early_timeout','wrong_expiry','press_not_sent'])test(`错误事实不能通过：${fault}`,async()=>{
  const value=fixture(fault);await assert.rejects(verifyNegatives(value.options));assert.equal(value.muted(),0);
});
