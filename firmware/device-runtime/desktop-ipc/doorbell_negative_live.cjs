// 文件说明：门铃负向流程；调用者提供原正式API、设备按压及实际采集观察，不构造平台应答或媒体。
'use strict';
const assert = require('node:assert/strict');

// terminal 只接受原候选腿的主动绝对终态；ACK、旧腿或错误原因不能证明拒接／振铃超时。
function terminal(event, session, reason) {
  return !!event && event.method === 'interaction.state' && event.sessionId === session &&
    typeof event.msgToken === 'string' && event.msgToken.length > 0 && !Object.hasOwn(event,'code') &&
    Number.isSafeInteger(event.eventSeq) && event.eventSeq > 0 &&
    event.data?.state === 'ended' && event.data.reason === reason;
}

// sourceIdle 必须保持原源在线且没有新采集；空视图本身不足以排除瞬时越权取流。
function sourceIdle(facts) {
  return !!facts && facts.available === true && facts.closed === false && facts.views === 0 &&
    facts.retiring === 0 && facts.invitations === 0 && facts.humanSessionEmpty === true && facts.newCaptures === 0;
}

// verifyNegatives 顺序验证本端点静音、三端全拒、真实30秒超时及超时后原请求重放；只返回脱敏事实。
async function verifyNegatives(options) {
  const {api,command,receive,events,endpoints,press,sourceFacts,validateInvite,camera,previousGroups,
    waitFor,product,device,clock=Date.now,delay=ms=>new Promise(resolve=>setTimeout(resolve,ms))} = options;
  assert.ok(Array.isArray(endpoints)&&endpoints.length===3&&new Set(endpoints).size===3);
  for (const fn of [api,command,receive,press,sourceFacts,validateInvite,waitFor,clock,delay]) assert.equal(typeof fn,'function');
  assert.ok(events instanceof Map&&previousGroups instanceof Set);
  // channel 只操作正式当前用户唯一原设备绑定，已有静音不擅自关闭或覆盖。
  async function channel() {
    const value=await api('im/channel/get-list',{});
    assert.ok(Array.isArray(value?.list)&&value.list.length<=1024);
    const matches=value.list.filter(row=>row.channelType==='device'&&row.channelId===`${product}:${device}`);
    assert.equal(matches.length,1);assert.ok(Number.isSafeInteger(matches[0].id)&&matches[0].id>0);
    return matches[0];
  }
  const phase=name=>options.onStage?.(name); // 仅固定本文件阶段枚举，不向日志传递身份或API正文。
  phase('channel');const binding=await channel();assert.equal(binding.isMuted,0);let changed=false;
  const report={quiet:{passed:false,restored:false},allRejected:false,timeout:false,expiredReplay:false,postTimeoutReusable:false};
  // 正式端点仅按60秒活跃窗口选候选，现无注销API。自然等旧运行端点失活，只续本次三端，不删除Redis事实。
  phase('settling');const settleUntil=clock()+65000;
  while(clock()<settleUntil) {
    for(const endpoint of endpoints)await receive(endpoint);
    assert.ok(sourceIdle(await sourceFacts()),'settling_must_not_capture');
    await delay(Math.min(1000,Math.max(1,settleUntil-clock())));
  }
  try {
    phase('quiet');
    changed=true;await api('im/channel/set-mute',{id:binding.id,isMuted:1});
    const muted=await channel();assert.equal(muted.id,binding.id);assert.equal(muted.isMuted,1);
    events.clear();assert.equal(await press('rejected'),true,'original_denied_press_must_be_observed');const until=clock()+35000;
    while(clock()<until) {
      for(const endpoint of endpoints)await receive(endpoint);
      assert.ok(![...events.values()].some(event=>event.method==='interaction.invite'),'quiet_must_not_ring');
      assert.ok(sourceIdle(await sourceFacts()),'quiet_must_not_capture');
      await delay(Math.min(250,Math.max(1,until-clock())));
    }
    report.quiet.passed=true;
  } finally {
    phase('quiet_restore');
    if(changed) {
      const current=await channel();assert.equal(current.id,binding.id);
      assert.ok(current.isMuted===0||current.isMuted===1,'original_channel_changed');
      if(current.isMuted===1)await api('im/channel/set-mute',{id:binding.id,isMuted:0});
      const restored=await channel();assert.equal(restored.id,binding.id);assert.equal(restored.isMuted,0);
      report.quiet.restored=true;
    }
  }
  // ring 只读取本次原邀请，不复用之前成功轮的token、组或源事件。
  async function ring() {
    events.clear();assert.equal(await press('success'),true,'original_press_must_be_observed');const invitations=new Map();
    await waitFor(async()=>{
      for(const endpoint of endpoints) {
        await receive(endpoint);
        for(const event of events.values())if(event.method==='interaction.invite'&&event.data?.endpointId===endpoint)
          invitations.set(endpoint,validateInvite(event,endpoint,camera));
      }
      assert.ok(sourceIdle(await sourceFacts()),'unanswered_must_not_capture');
      return invitations.size===3;
    });
    const group=[...invitations.values()][0].data.callGroupId;
    assert.equal(new Set([...invitations.values()].map(event=>event.data.callGroupId)).size,1);
    assert.equal(new Set([...invitations.values()].map(event=>event.sessionId)).size,3);
    assert.equal(previousGroups.has(group),false);previousGroups.add(group);
    return invitations;
  }
  // ended 等待每个原候选的正式持久终态，期间原设备必须保持没有采集。
  async function ended(invitations,reason,budget=30000) {
    await waitFor(async()=>{
      for(const endpoint of endpoints)await receive(endpoint);
      assert.ok(sourceIdle(await sourceFacts()),'negative_terminal_must_not_capture');
      return endpoints.every(endpoint=>[...events.entries()].some(([key,event])=>
        key.startsWith(`${endpoint}:`)&&terminal(event,invitations.get(endpoint).sessionId,reason)));
    },budget);
  }
  // rejectAll 三端明确拒接，原组释放由紧随其后的新独立按压是否成功证明，不手工结束组。
  async function rejectAll() {
    const invitations=await ring();
    const results=await Promise.all(endpoints.map(endpoint=>{
      const invite=invitations.get(endpoint);
      return command(endpoint,invite.sessionId,'interaction.control',
        {command:'sessionReject',arguments:{reason:'rejected'}},invite.msgToken);
    }));
    assert.ok(results.every(result=>result.code===200&&result.data?.result?.state==='ended'));
    await ended(invitations,'rejected');return true;
  }
  phase('all_rejected');report.allRejected=await rejectAll();
  phase('timeout');
  const timed=await ring(),expires=[...timed.values()].map(event=>event.data.invitation.expiresAt);
  assert.equal(new Set(expires).size,1);assert.ok(expires[0]>clock()&&expires[0]-clock()<=30000);
  await ended(timed,'ringTimeout',Math.max(1000,expires[0]-clock()+10000));
  assert.ok(clock()>=expires[0],'timeout_cannot_end_early');report.timeout=true;
  phase('expired_replay');const endpoint=endpoints[0],invite=timed.get(endpoint);
  for(let attempt=0;attempt<2;attempt++) {
    events.delete(`${endpoint}:interaction.result:${invite.msgToken}`);
    const result=await command(endpoint,invite.sessionId,'interaction.control',
      {command:'sessionAccept',arguments:{acceptedTracks:['mic-1','speaker-1','video-1']}},invite.msgToken);
    assert.equal(result.code,410);assert.equal(result.data?.reason,'expired');
    assert.ok(sourceIdle(await sourceFacts()),'expired_answer_must_not_capture');
  }
  report.expiredReplay=true;phase('post_timeout');report.postTimeoutReusable=await rejectAll();
  return report;
}
module.exports={terminal,sourceIdle,verifyNegatives};
