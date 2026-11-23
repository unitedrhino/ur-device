// 文件说明：取消竞态判据及原字节延迟器单测；不声称替身完成真实模型、C++或音频验收。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict'),vm = require('node:vm');
const {installCancellationHarness,clickOriginalCancellation,releaseOriginalLateFact,verifyCancellationProbe,runProbe} = require('./browser_client_tool_cancel_live.cjs');
// fixture 描述已经实际绘制后取消、尚未向C++交付完成事实的最小状态。
function fixture(voice = false) {
  const before = {actualRecords:0,cancelled:0,completed:0,receiptIndex:0};
  const cancelled = {held:true,commandHeld:true,cancelSent:true,actualStatus:'succeeded',records:1,
    theme:'dark',cancelled:1,completed:0,receipts:0,active:false,terminated:false,bridgeOpen:true,captureOpen:voice,pending:0};
  return {before,cancelled,after:{...cancelled}};
}
test('取消后旧实际事实和原票据DUP不续答、不再执行且不关闭输入', () => {
  for (const voice of [false,true]) {const f = fixture(voice);
    assert.equal(verifyCancellationProbe(f.before,f.cancelled,f.after,voice).passed,true);}
});
test('每个取消隔离条件都有失败反例', () => {
  for (const mutate of [
    f => f.cancelled.held = false, f => f.cancelled.commandHeld = false, f => f.cancelled.cancelSent = false,
    f => f.cancelled.actualStatus = 'unknown', f => f.cancelled.records = 0, f => f.cancelled.cancelled = 0,
    f => f.cancelled.completed = 1, f => f.cancelled.receipts = 1, f => f.after.active = true,
    f => f.after.terminated = true, f => f.after.bridgeOpen = false, f => f.after.captureOpen = true,
    f => f.after.pending = 1, f => f.after.records = 2, f => f.after.theme = 'light',
    f => f.after.cancelled = 2, f => f.after.completed = 1, f => f.after.receipts = 1,
  ]) {const f = fixture();mutate(f);assert.throws(() => verifyCancellationProbe(f.before,f.cancelled,f.after,false));}
});
test('延迟原主题端口回执而不是制造工具回执，原取消按钮和原命令只按需重投', () => {
  const sent = [],received = [],actual = [];
  const context = {clientTheme:{platform:{emit:value => {actual.push(value);return true;}}},
    send:value => {sent.push(value);return true;},incoming:value => {received.push(value);return true;}};
  context.document = {getElementById:id => {assert.equal(id,'cancel');return {click:() => context.send({kind:'cancel',id:'original'})};}};
  vm.createContext(context);vm.runInContext('(' + installCancellationHarness.toString() + ')()',context);
  const command = {kind:'clientThemeSet',themeTicket:'1',theme:'dark'},fact = {kind:'clientThemeActual',themeTicket:'1',status:'succeeded',theme:'dark'};
  context.incoming(command);context.clientTheme.platform.emit(fact);
  assert.equal(actual.length,0);assert.equal(sent.length,0,'实际回执回调早于核心控制投影，不能自行点击尚未启用的按钮');
  context.document.getElementById('cancel').click();
  assert.equal(sent.length,1);assert.equal(sent[0].kind,'cancel');
  vm.runInContext('(' + releaseOriginalLateFact.toString() + ')()',context);
  assert.equal(sent.length,2);assert.deepEqual(JSON.parse(JSON.stringify(sent[1])),fact);
  assert.equal(received.length,2);assert.deepEqual(JSON.parse(JSON.stringify(received[1])),command);
  assert.throws(() => vm.runInContext('(' + releaseOriginalLateFact.toString() + ')()',context));
});

// inlineFixture 仅描述原严格投影与本地真实事实的时序，按钮仍是唯一取消路径。
function inlineFixture() {
  let clicks = 0;
  const context = {__urToolCancellation:{fact:{status:'succeeded',themeTicket:'1'},command:{themeTicket:'1'},cancelSent:false},
    __urClientToolLive:{tools:[{state:'started',approved:true,response:'synthetic-original'}],overflow:false},
    controlResponseId:'synthetic-original',metrics:{cancelledResponses:0},terminated:false,socket:{readyState:1},WebSocket:{OPEN:1}};
  const button = {disabled:false,click:()=>{++clicks;context.__urToolCancellation.cancelSent=true;}};
  context.document={getElementById:()=>button};vm.createContext(context);
  return {context,button,clicks:()=>clicks,
    poll:(offset=0)=>vm.runInContext('('+clickOriginalCancellation.toString()+')(0,'+offset+')',context)};
}
test('原控制投影晚到前不能点击，晚到后同页仅点击原按钮一次',()=>{
  const f=inlineFixture();f.context.controlResponseId='';assert.equal(f.poll(),false);assert.equal(f.clicks(),0);
  f.context.controlResponseId='synthetic-original';assert.equal(f.poll(),true);assert.equal(f.poll(),true);assert.equal(f.clicks(),1);
});
test('只使用本轮唯一approved原started目标，旧轮和双工具不能借当前取消权',()=>{
  const f=inlineFixture();f.context.__urClientToolLive.tools.unshift({state:'started',approved:true,response:'synthetic-old'});
  assert.throws(()=>f.poll(),/original_cancel_scope_unavailable/);assert.equal(f.clicks(),0);
  assert.equal(f.poll(1),true);assert.equal(f.clicks(),1);
});
test('错关联、未知真实结果与关闭状态不得猜回复或发送取消',()=>{
  for(const mutate of [c=>c.controlResponseId='synthetic-other',c=>c.__urClientToolLive.tools.length=0,
    c=>c.__urClientToolLive.tools[0].approved=false,c=>c.__urToolCancellation.fact.themeTicket='2',
    c=>c.__urToolCancellation.fact.status='unknown',c=>c.__urToolCancellation.command=null,
    c=>c.metrics.cancelledResponses=1,c=>c.__urToolCancellation.cancelSent=true,c=>c.terminated=true,
    c=>c.socket.readyState=3,c=>c.__urClientToolLive.overflow=true]) {
    const f=inlineFixture();mutate(f.context);
    try{assert.equal(f.poll(),false);}catch(error){assert.match(error.message,/original_cancel_scope_unavailable/);}
    assert.equal(f.clicks(),0);
  }
  const f=inlineFixture();f.button.disabled=true;assert.equal(f.poll(),false);assert.equal(f.clicks(),0);
});
test('原按钮发送失败保持失败且不能再次点击或改选回复',()=>{
  const f=inlineFixture();let attempts=0;f.button.click=()=>{++attempts;};
  assert.throws(()=>f.poll(),/original_cancel_click_failed/);assert.equal(f.poll(),false);assert.equal(attempts,1);
});
test('慢CLI在原两秒窗口内同页完成取消，原完整迟到隔离断言仍成立',async()=>{
  let now=0,started=null,clicks=0,separateCancelCommands=0;
  const context={metrics:{cancelledResponses:0,completedResponses:0,clientToolReceiptIndex:0,captureOpen:false},
    responseId:'',controlResponseId:'',pendingPlayback:new Map(),terminated:false,socket:{readyState:1},WebSocket:{OPEN:1},
    __urClientToolLive:{tools:[],overflow:false},document:{body:{dataset:{clientTheme:'dark'}}},
    clientTheme:{records:new Map(),platform:{emit:()=>true}},incoming:()=>true};
  context.send=value=>{
    if(value.kind==='clientThemeActual')return true;
    if(value.kind==='cancel'&&context.controlResponseId!==''){
      ++context.metrics.cancelledResponses;context.responseId='';context.controlResponseId='';return true;
    }
    return false;
  };
  const button={disabled:false,click:()=>{++clicks;context.send({kind:'cancel'});}};
  context.document.getElementById=()=>button;vm.createContext(context);
  async function invoke(kind,...args){
    now+=1100;
    if(started!==null&&now-started>=2000&&context.controlResponseId!==''){
      context.controlResponseId='';context.metrics.completedResponses=1;context.metrics.clientToolReceiptIndex=1;
    }
    if(kind==='eval')return JSON.stringify(vm.runInContext(args[0],context));
    if(kind==='fill'||kind==='uncheck')return '';
    if(kind==='click'&&args[0]==='#sendText'){
      started=now;context.controlResponseId='synthetic-original';
      context.__urClientToolLive.tools.push({state:'started',response:'synthetic-original',approved:true});
      context.incoming({kind:'clientThemeSet',themeTicket:'1',theme:'dark'});context.clientTheme.records.set('1',{});
      context.clientTheme.platform.emit({kind:'clientThemeActual',themeTicket:'1',status:'succeeded',theme:'dark'});return '';
    }
    if(kind==='click'){++separateCancelCommands;button.click();return '';}
    assert.equal(kind,'wait');if(args[0]==='--fn')assert.equal(vm.runInContext(args[1],context),true);return '';
  }
  const report=await runProbe({invoke,snapshot:async()=>({actualRecords:0,cancelled:0,completed:0,receiptIndex:0,tools:[]}),
    voice:false,parseJSON:JSON.parse});
  assert.equal(report.passed,true);assert.equal(clicks,1);assert.equal(separateCancelCommands,0);
  assert.equal(context.metrics.completedResponses,0);assert.equal(context.metrics.cancelledResponses,1);
});
