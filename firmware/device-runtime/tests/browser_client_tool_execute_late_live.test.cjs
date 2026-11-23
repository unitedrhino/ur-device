// 文件说明：真实迟到Execute验收判据的离线反例；合成事实不能代替MQTT、模型或C++验收。
'use strict';
const test = require('node:test'),assert = require('node:assert/strict'),vm=require('node:vm');
const {verifyLateExecute,clickHeldExecuteCancellation,runExecuteLateProbe} = require('./browser_client_tool_execute_late_live.cjs');

// fixture 仅定义原取消终态及已实收拒绝的结构，所有标识和水位均为离线合成。
function fixture(voice = false) {
  const before = {cancelled:0,completed:0,actualRecords:0,receiptIndex:0};
  const cancelled = {cancelled:1,completed:0,actualRecords:0,receiptIndex:0,
    active:false,terminated:false,localSocketOpen:true,captureOpen:voice,captureClosed:!voice,pendingPlayback:0,theme:''};
  const after = {...cancelled,messageDiagnostics:{overflow:false,records:[{method:'toolCallExecute',sequence:3,
    watermark:4,outcome:'watermark_discarded',parsed:true,sessionMatches:true,taken:false}]}};
  return {before,cancelled,after,network:{held:true,releasedOriginal:true,failed:false}};
}
test('两种输入都要求原取消后实收拒绝，零驱动效果及零续答污染', () => {
  for(const voice of [false,true]) {const f = fixture(voice);
    assert.equal(verifyLateExecute(f.before,f.cancelled,f.after,voice,3,f.network).passed,true);}
});

// cancellationFixture 只模拟原严格投影及原按钮send回调，不制造平台取消ACK或新的工具授权。
function cancellationFixture() {
  let clicks=0,sends=0;
  const context={controlResponseId:'synthetic-original',metrics:{cancelledResponses:0,completedResponses:0},
    terminated:false,socket:{readyState:1},WebSocket:{OPEN:1},clientTheme:{closed:false,records:new Map()},
    __urClientToolLive:{overflow:false,tools:[{state:'started',approved:true,id:'synthetic-tool',response:'synthetic-original'}]}};
  context.send=message=>{++sends;return message.kind==='cancel'&&message.id===context.controlResponseId;};
  const originalSend=context.send;
  const button={disabled:false,click:()=>{++clicks;context.send({kind:'cancel',id:context.controlResponseId});}};
  context.document={getElementById:id=>{assert.equal(id,'cancel');return button;}};
  vm.createContext(context);
  return {context,button,originalSend,clicks:()=>clicks,sends:()=>sends,
    poll:(response='synthetic-original',offset=0)=>vm.runInContext('('+clickHeldExecuteCancellation.toString()+')('+
      JSON.stringify(response)+',0,0,0,'+offset+')',context)};
}
test('代理原scope与本轮strict started同时成立才点一次原按钮，并恢复原send',()=>{
  const f=cancellationFixture();f.context.controlResponseId='';assert.equal(f.poll(),false);assert.equal(f.clicks(),0);
  f.context.controlResponseId='synthetic-original';assert.equal(f.poll(),true);assert.equal(f.poll(),true);
  assert.equal(f.clicks(),1);assert.equal(f.sends(),1);assert.equal(f.context.send,f.originalSend);
  assert.throws(()=>f.poll('synthetic-other'),/original_execute_cancel_scope_unavailable/);
});
test('旧轮工具不能授权当前取消，本轮仅唯一approved started可用',()=>{
  const f=cancellationFixture();f.context.__urClientToolLive.tools.unshift({state:'started',approved:true,response:'synthetic-old'});
  assert.throws(()=>f.poll(),/original_execute_cancel_scope_unavailable/);assert.equal(f.clicks(),0);
  assert.equal(f.poll('synthetic-original',1),true);assert.equal(f.clicks(),1);
});
test('Done或作用域丢失、错关联、驱动已执行、工具终态和断连均不能复活取消资格',()=>{
  for(const mutate of [
    f=>{f.context.controlResponseId='';f.context.metrics.completedResponses=1;},
    f=>f.context.controlResponseId='synthetic-other',f=>f.context.metrics.cancelledResponses=1,
    f=>f.context.clientTheme.records.set('synthetic-ticket',{}),f=>f.context.clientTheme.closed=true,
    f=>f.context.__urClientToolLive.tools.length=0,f=>f.context.__urClientToolLive.tools[0].approved=false,
    f=>f.context.__urClientToolLive.tools[0].response='synthetic-other',
    f=>f.context.__urClientToolLive.tools.push({state:'completed',response:'synthetic-original'}),
    f=>f.context.__urClientToolLive.overflow=true,f=>f.context.terminated=true,
    f=>f.context.socket.readyState=3,f=>f.button.disabled=true,
  ]) {const f=cancellationFixture();mutate(f);
    try {assert.equal(f.poll(),false);}catch(error){assert.match(error.message,/original_execute_cancel_scope_unavailable/);}
    assert.equal(f.clicks(),0);assert.equal(f.sends(),0);assert.equal(f.context.send,f.originalSend);
  }
});
test('原按钮发送拒绝、错目标、重复发送及抛错均保留失败，不重试按钮',()=>{
  for(const fault of ['rejected','wrongTarget','duplicate','throws','throwsAfterSend']) {
    const f=cancellationFixture();
    if(fault==='rejected')f.context.send=()=>false;
    else if(fault==='wrongTarget')f.button.click=()=>f.context.send({kind:'cancel',id:'synthetic-other'});
    else if(fault==='duplicate')f.button.click=()=>{f.context.send({kind:'cancel',id:'synthetic-original'});f.context.send({kind:'cancel',id:'synthetic-original'});};
    else if(fault==='throwsAfterSend')f.button.click=()=>{f.context.send({kind:'cancel',id:'synthetic-original'});throw Error('synthetic-button-failure');};
    else f.button.click=()=>{throw Error('synthetic-button-failure');};
    const original=f.context.send;
    assert.throws(()=>f.poll(),/original_execute_cancel_click_failed|synthetic-button-failure/);
    assert.equal(f.poll(),false);assert.equal(f.context.send,original);
  }
});
test('非法范围参数在原按钮或send前拒绝',()=>{
  for(const args of [['',0,0,0,0],['synthetic-original',-1,0,0,0],['synthetic-original',0,0,0,17],
    ['synthetic-original',0,0,0,0.5],['synthetic-original',0,0,NaN,0]]) {
    const f=cancellationFixture();assert.throws(()=>vm.runInContext('('+clickHeldExecuteCancellation.toString()+')('+
      args.map(value=>Number.isNaN(value)?'NaN':JSON.stringify(value)).join(',')+')',f.context),/invalid_execute_cancel_scope/);
    assert.equal(f.clicks(),0);assert.equal(f.sends(),0);
  }
});

// slowFixture 在每个CLI推进1100毫秒；原回复两秒后自然Done，跨CLI保存目标必然失效。
function slowFixture(){
  let now=0,start=null,clicks=0,released=false,separateCancelCommands=0;
  const c={controlResponseId:'',responseId:'',metrics:{cancelledResponses:0,completedResponses:0},
    terminated:false,socket:{readyState:1},WebSocket:{OPEN:1},pendingPlayback:new Map(),
    __urClientToolLive:{tools:[],overflow:false,messageDiagnostics:{overflow:false,records:[]}},clientTheme:{records:new Map()}};
  c.send=m=>{if(m.kind!=='cancel'||m.id!==c.controlResponseId||!m.id)return false;
    ++c.metrics.cancelledResponses;c.controlResponseId='';c.responseId='';return true;};
  const button={disabled:false,click:()=>{++clicks;c.send({kind:'cancel',id:c.controlResponseId});}};
  c.document={getElementById:()=>button};vm.createContext(c);
  const before={cancelled:0,completed:0,actualRecords:0,receiptIndex:0,tools:[]};
  const snapshot=async()=>({...before,cancelled:c.metrics.cancelledResponses,completed:c.metrics.completedResponses,
    active:c.responseId!=='',terminated:false,localSocketOpen:true,captureOpen:false,captureClosed:true,
    pendingPlayback:0,theme:'',tools:c.__urClientToolLive.tools.slice(),messageDiagnostics:c.__urClientToolLive.messageDiagnostics});
  const invoke=async(kind,...args)=>{
    now+=1100;
    if(start!==null&&now-start>=2000&&c.controlResponseId!==''){
      c.controlResponseId='';c.responseId='';++c.metrics.completedResponses;
    }
    if(kind==='fill'||kind==='uncheck')return '';
    if(kind==='click'&&args[0]==='#sendText'){
      start=now;c.controlResponseId='synthetic-original';c.responseId='synthetic-original';
      c.__urClientToolLive.tools.push({state:'started',approved:true,id:'synthetic-tool',response:'synthetic-original'});return '';
    }
    if(kind==='click'){++separateCancelCommands;button.click();return '';}
    if(kind==='eval')return JSON.stringify(vm.runInContext(args[0],c));
    assert.equal(kind,'wait');assert.equal(args[0],'--fn');assert.equal(vm.runInContext(args[1],c),true);return '';
  };
  const delay={identity:()=>start===null?null:{session:'synthetic-session',response:'synthetic-original',sequence:3},
    facts:()=>({held:start!==null,releasedOriginal:released,failed:false}),release:(session,response)=>{
      assert.equal(session,'synthetic-session');assert.equal(response,'synthetic-original');released=true;
      c.__urClientToolLive.messageDiagnostics.records.push({method:'toolCallExecute',sequence:3,watermark:4,
        outcome:'watermark_discarded',parsed:true,sessionMatches:true,taken:false});}};
  return {invoke,snapshot,delay,clicks:()=>clicks,separateCancelCommands:()=>separateCancelCommands,context:c};
}
test('慢CLI同页取消在原两秒窗口内完成，原网络迟到水位及无副作用判据不退化',async()=>{
  const f=slowFixture();const report=await runExecuteLateProbe({...f,voice:false,parseJSON:JSON.parse});
  assert.equal(report.passed,true);assert.equal(f.clicks(),1);assert.equal(f.separateCancelCommands(),0);
  assert.equal(f.context.metrics.completedResponses,0);assert.equal(f.context.metrics.cancelledResponses,1);
});
test('缺原字节、未实收、错水位、错误会话或迟到副作用均不能通过', () => {
  for(const mutate of [
    f => f.network.held = false,f => f.network.releasedOriginal = false,f => f.network.failed = true,
    f => f.after.actualRecords++,f => f.after.receiptIndex++,f => f.after.completed++,
    f => f.cancelled.cancelled = 0,f => f.after.active = true,f => f.after.terminated = true,
    f => f.after.localSocketOpen = false,f => f.after.captureOpen = true,f => f.after.pendingPlayback = 1,
    f => f.after.theme = 'dark',f => f.after.messageDiagnostics.overflow = true,
    f => f.after.messageDiagnostics.records = [],
    f => f.after.messageDiagnostics.records[0].sequence = 5,
    f => f.after.messageDiagnostics.records[0].watermark = 3,
    f => f.after.messageDiagnostics.records[0].outcome = 'execute_accepted',
    f => f.after.messageDiagnostics.records[0].parsed = false,
    f => f.after.messageDiagnostics.records[0].sessionMatches = false,
    f => f.after.messageDiagnostics.records[0].taken = true,
  ]) {const f = fixture();mutate(f);
    assert.throws(() => verifyLateExecute(f.before,f.cancelled,f.after,false,3,f.network));}
});
