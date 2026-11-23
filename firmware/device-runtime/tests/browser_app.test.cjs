// 浏览器设备页的音频事实单测：用隔离VM运行正式脚本，不把DOM模拟当作真实播放。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');
const workletSource = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/audio-worklet.js'), 'utf8');

// diagnosticPage运行原app发送器；使用唯一原socket替身，禁止诊断失败调用release或增加业务发送。
function diagnosticPage(nativeBoot=false){
  const bodies=[];const context=vm.createContext({TextEncoder,WebSocket:{OPEN:1},...(nativeBoot?{UrInputOwnershipBoot:'7'}:{}),document:{getElementById(){return {addEventListener(){}};}},window:{addEventListener(){}}});vm.runInContext(source,context);
  vm.runInContext("duplexHumanMedia={observeStop:true,scope:{sessionId:'human',routeGeneration:'11',peerEpoch:'7'}};socket={readyState:1,bufferedAmount:0};",context);
  context.socketSend=body=>bodies.push(body);vm.runInContext('socket.send=socketSend',context);
  return {bodies,context,send:value=>context.sendHumanStopDiagnostic(value),set:script=>vm.runInContext(script,context),terminated:()=>vm.runInContext('terminated',context)};
}
test('独立Human停止sender原WS发送且失败不release，精确预留Stopped预算',()=>{
  const p=diagnosticPage(),fact={kind:'humanVoiceStopDiagnostic',sessionId:'human',routeGeneration:'11',peerEpoch:'7',known:true,cause:'peerFailed'};
  assert.equal(p.send(fact),true);assert.deepEqual(JSON.parse(p.bodies[0]),fact);assert.equal(p.terminated(),false);
  const reserve=new TextEncoder().encode(JSON.stringify(fact)).length+new TextEncoder().encode(JSON.stringify({kind:'humanVoiceStopped',sessionId:'human',routeGeneration:'11',peerEpoch:'7'})).length;
  p.set('socket.bufferedAmount='+(65536-reserve));assert.equal(p.send(fact),true);p.set('socket.bufferedAmount='+(65536-reserve+1));assert.equal(p.send(fact),false);assert.equal(p.terminated(),false);
  for(const script of ['socket.readyState=3','socket.readyState=1;socket.bufferedAmount=0;socket.send=()=>{throw Error("PRIVATE")}','duplexHumanMedia.observeStop=false']){p.set(script);assert.equal(p.send(fact),false);assert.equal(p.terminated(),false);}
  assert.equal(p.bodies.length,2);
});
test('Human旁证sender拒getter/额外字段/错scope/known矛盾，仅留固定正文',()=>{
  const p=diagnosticPage(),fact={kind:'humanVoiceStopDiagnostic',sessionId:'human',routeGeneration:'11',peerEpoch:'7',known:false,cause:'unknown'};let calls=0;
  for(const value of [{...fact,sessionId:'other'},{...fact,routeGeneration:'12'},{...fact,peerEpoch:'8'},{...fact,known:true},{...fact,cause:'peerFailed'},{...fact,body:'PRIVATE'}])assert.equal(p.send(value),false);
  const getter={...fact};Object.defineProperty(getter,'cause',{get(){calls++;throw Error('PRIVATE');}});assert.equal(p.send(getter),false);assert.equal(calls,0);assert.equal(p.bodies.length,0);assert.equal(p.terminated(),false);assert.equal(p.send(fact),true);
});
test('原Human initialize实际注入非release端口，诊断预算失败不抑制普通Stopped',async()=>{
  const p=diagnosticPage(true);let options;
  p.context.UrHumanVoiceMedia=class {constructor(value){options=value;this.observeStop=true;this.scope={sessionId:'human',routeGeneration:'11',peerEpoch:'7'};}async initialize(){return true;}};
  p.set("duplexHumanMedia=undefined;globalThis.UrHumanVoiceEnabled=true;globalThis.UrInputOwnershipSampleRate=48000;inputOwnership={};stream={};source={};context={};");
  await p.context.initializeHumanVoiceMedia();assert.equal(options.emit,p.context.send);assert.equal(options.emitDiagnostic,p.context.sendHumanStopDiagnostic);
  p.set('socket.bufferedAmount=65500');assert.equal(options.emitDiagnostic({kind:'humanVoiceStopDiagnostic',sessionId:'human',routeGeneration:'11',peerEpoch:'7',known:true,cause:'peerFailed'}),false);
  assert.equal(p.terminated(),false);assert.equal(options.emit({kind:'humanVoiceStopped',sessionId:'human',routeGeneration:'11',peerEpoch:'7'}),true);assert.equal(p.bodies.length,1);assert.equal(JSON.parse(p.bodies[0]).kind,'humanVoiceStopped');
});
// 页面release先锁terminated；只读旁证仍须在原socket关闭前可送出，但不能改变原Stopped发送策略。
test('原release实际Human close旁证不被terminated屏蔽，原资源顺序及Stopped尝试不变',async()=>{
  const humanSource=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/human-voice-media.js'),'utf8');
  for(const enabled of [false,true]){
    const p=diagnosticPage(true),order=[],attempts=[];
    p.context.closeOrder=order;p.context.stopAttempts=attempts;
    p.set(`globalThis.MediaStream=class {getAudioTracks(){return [{readyState:'live',enabled:true}];}getVideoTracks(){return [];}getTracks(){return [{stop(){closeOrder.push('track');}}];}};
      globalThis.AudioContext=class {constructor(){this.state='running';this.sampleRate=48000;this.destination={};this.audioWorklet={addModule:async()=>{}};}close(){closeOrder.push('context');}};
      globalThis.MediaStreamAudioSourceNode=class {constructor(ctx,s){this.context=ctx;this.mediaStream=s;}disconnect(){closeOrder.push('source');}};
      globalThis.AudioWorkletNode=class {constructor(){this.port={postMessage:v=>closeOrder.push(v.kind),close:()=>closeOrder.push('port')};}connect(){}disconnect(){closeOrder.push('node');}};
      socket.close=()=>closeOrder.push('socket');stream=new MediaStream();context=new AudioContext();source=new MediaStreamAudioSourceNode(context,stream);
      duplexHumanMedia=undefined;globalThis.UrHumanVoiceEnabled=true;globalThis.UrInputOwnershipSampleRate=48000;inputOwnership={close(){closeOrder.push('input');}};`);
    vm.runInContext(humanSource,p.context);await p.context.initializeHumanVoiceMedia();
    p.set(`duplexHumanMedia.emit=v=>{stopAttempts.push(v);send(v);};`);
    const bound=vm.runInContext("duplexHumanMedia.handle({kind:'humanVoiceOutputBind',sessionId:'human',routeGeneration:'11',peerEpoch:'7'})",p.context);
    assert.equal(await bound,true);order.length=0;
    if(enabled)p.set('duplexHumanMedia.enableStopDiagnostics()');
    p.context.release('PRIVATE');p.context.release('PRIVATE');
    assert.equal(p.terminated(),true);assert.equal(attempts.length,1);assert.deepEqual(Object.keys(attempts[0]).sort(),['kind','peerEpoch','routeGeneration','sessionId']);
    assert.deepEqual(order,['input','humanVoiceOutputStop','node','port','track','source','context','socket']);
    assert.equal(p.bodies.length,enabled?1:0);
    if(enabled)assert.deepEqual(JSON.parse(p.bodies[0]),{kind:'humanVoiceStopDiagnostic',sessionId:'human',routeGeneration:'11',peerEpoch:'7',known:true,cause:'explicitClose'});
  }
});

// browser 为每例创建独立页面上下文，并可把正式驱动消息送入音频线程模拟器。
function browser(postToWorklet = () => {}) {
  const elements = new Map();
  const context = vm.createContext({
    document: {getElementById(id) {
      if (!elements.has(id)) elements.set(id, {textContent: '', addEventListener() {}});
      return elements.get(id);
    }},
    window: {addEventListener() {}},
    TextEncoder,
    postToWorklet,
  });
  vm.runInContext(source, context);
  vm.runInContext('worklet = {port: {postMessage: postToWorklet}}', context);
  return {
    incoming: value => context.incoming(value),
    played: value => context.recordPlaybackConsumption(value),
    metrics: () => JSON.parse(vm.runInContext('JSON.stringify(metrics)', context)),
    transcript: () => elements.get('transcript')?.textContent ?? '',
  };
}

// frame 生成一帧固定振幅PCM16，用于验证RMS门槛和相邻帧判定。
function frame(epoch, amplitude, ticket) {
  return {kind: 'playbackFrame', epoch, ticket, samples: Array(320).fill(amplitude)};
}

// focusReceipt 仅模拟核心已关联的白名单事实，不能替代后端收录或实际驱动回执。
function focusReceipt(operation, revision, receiptIndex, code = 200, state) {
  const states = {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'};
  return {kind: 'focusReceipt', operation, revision, receiptIndex, code, state: state ?? states[operation]};
}

test('焦点观察只保存五个白名单字段，历史重放不伪造新的实际修订或播放许可', () => {
  const page = browser();
  ['acquire', 'suspend', 'restore', 'release'].forEach((operation, index) =>
    page.incoming(focusReceipt(operation, index + 1, index + 1)));
  page.incoming(focusReceipt('acquire', 1, 5)); // 原历史结果再次交付，不是新取得。
  const metrics = page.metrics();
  assert.equal(metrics.focusReceiptIndex, 5);
  assert.deepEqual(metrics.focusReceipts.map(value => value.operation),
    ['acquire', 'suspend', 'restore', 'release', 'acquire']);
  assert.equal(metrics.focusReceipts.at(-1).revision, 1);
  assert.deepEqual(Object.keys(metrics.focusReceipts.at(-1)).sort(),
    ['code', 'operation', 'receiptIndex', 'revision', 'state']);
  assert.equal(metrics.playableResponses, 0);
  assert.equal(metrics.consumed, 0);
});

test('焦点观察保持32条有界窗口，交付水位单调且重复观察不能成为新阶段证据', () => {
  const page = browser();
  for (let index = 1; index <= 40; ++index)
    page.incoming(focusReceipt('acquire', 1, index)); // 重放允许原revision，但没有新的驱动动作。
  const metrics = page.metrics();
  assert.equal(metrics.focusReceiptIndex, 40);
  assert.equal(metrics.focusReceipts.length, 32);
  assert.equal(metrics.focusReceipts[0].receiptIndex, 9);
  assert.throws(() => page.incoming(focusReceipt('acquire', 1, 40)), /invalid_focus_receipt/);
  assert.throws(() => page.incoming(focusReceipt('acquire', 1, 39)), /invalid_focus_receipt/);
  assert.equal(page.metrics().focusReceiptIndex, 40);
});

test('焦点投影拒绝身份泄漏和畸形字段，失败结果不能标为成功状态', () => {
  const page = browser();
  for (const change of [{sessionId: 'private'}, {msgToken: 'private'}, {epoch: 'private'},
    {groups: ['speaker']}, {focusId: 'private'}, {receiptIndex: 0}, {receiptIndex: 1.5},
    {revision: -1}, {revision: Number.MAX_SAFE_INTEGER + 1}, {code: 200.5}, {code: 0},
    {operation: 'automaticRestore'}, {state: 'released'}, {code: 403}])
    assert.throws(() => page.incoming({...focusReceipt('acquire', 1, 1), ...change}),
      /invalid_focus_receipt/);
  page.incoming(focusReceipt('acquire', 0, 1, 403, 'denied'));
  assert.equal(page.metrics().focusReceipts[0].code, 403);
  assert.equal(page.metrics().focusReceipts[0].state, 'denied');
  assert.equal(page.metrics().successfulActions, 0);
});

test('音频线程确认静音消费不能算成实际非静音回复', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r', epoch: '1'});
  page.incoming(frame('1', 1000, 1));
  page.incoming(frame('1', 1000, 2));
  page.played({kind: 'played', epoch: '1', ticket: 1, nonSilent: false});
  page.played({kind: 'played', epoch: '1', ticket: 2, nonSilent: false});
  assert.equal(page.metrics().consumedNonSilent, 0);
});
// mediaFinal模拟C++停播冻结事实；缺包或尾部未齐时不得标成完整。
function mediaFinal(id, networkPackets, decodedFrames, nonSilentFrames, missingPackets = 0,
  observedComplete = true) {
  return {kind: 'responseMediaFinal', id, networkPackets, decodedFrames, nonSilentFrames,
    missingPackets, observedComplete};
}

test('可播放首音采用设备侧RMS 128门槛且必须连续两帧', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 127, 1));
  assert.equal(page.metrics().decodedNonSilent, 0);
  page.incoming(frame('1', 128, 2));
  assert.equal(page.metrics().playableResponses, 0);
  page.incoming(frame('1', -128, 3));
  assert.equal(page.metrics().playableResponses, 1);
  assert.equal(page.metrics().lastPlayableResponseId, 'r1');
  page.incoming(frame('1', 128, 4));
  assert.equal(page.metrics().playableResponses, 1);
  assert.equal(page.metrics().decodedNonSilent, 3);
});

test('静音、迟到代次和取消均不能拼接出下一轮可播放首音', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 128, 1));
  page.incoming(frame('1', 0, 2));
  page.incoming(frame('1', 128, 3));
  page.incoming(frame('old', 128, 4));
  assert.equal(page.metrics().playableResponses, 0);
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 128, 5));
  page.incoming({kind: 'playbackStart', id: 'r2', epoch: '2'});
  page.incoming(frame('2', 128, 1));
  assert.equal(page.metrics().playableResponses, 0);
  page.incoming(frame('2', 128, 2));
  assert.equal(page.metrics().playableResponses, 1);
  assert.equal(page.metrics().decodedNonSilent, 4);
});

test('工具状态只展示名称与结果状态，不解释为设备动作成功', () => {
  const page = browser();
  page.incoming({kind: 'toolStatus', id: 'call-1', state: 'started', name: '<script>setVolume</script>'});
  page.incoming({kind: 'toolStatus', id: 'call-1', state: 'unknown'});
  assert.match(page.transcript(), /正在调用工具：<script>setVolume<\/script>/);
  assert.match(page.transcript(), /工具结果待确认/);
  assert.doesNotMatch(page.transcript(), /执行成功/);
  assert.throws(() => page.incoming({kind: 'toolStatus', id: 'call-1', state: 'succeeded', name: 'forged'}), /invalid_tool_status/);
  assert.throws(() => page.incoming({kind: 'toolStatus', id: 'call-1', state: 'ok'}), /invalid_tool_status/);
});

test('原ASR唯一text与工具可选回复ID不改变显示/播放或旧消息合同',()=>{
  const page=browser();
  page.incoming({kind:'text',text:'\n用户：原识别\n助手：',responseId:'asr-response'});
  assert.equal(page.transcript(),'\n用户：原识别\n助手：');
  page.incoming({kind:'text',text:'legacy'});
  for(const state of ['started','succeeded','failed','unknown'])
    page.incoming({kind:'toolStatus',id:'call-1',state,responseId:'asr-response',
      ...(state==='started'?{name:'tools.set'}:{})});
  page.incoming({kind:'toolStatus',id:'old-call',state:'unknown'});
  assert.equal(page.metrics().playableResponses,0);
  assert.equal(page.metrics().completedResponses,0);
  assert.equal(page.metrics().successfulActions,0);
  assert.equal(page.transcript().split('用户：原识别').length-1,1);
  assert.ok(!page.transcript().includes('asr-response'));
});
test('可选回复ID严格UTF8字节界限与控制字符，非法值不能追加原text或工具行',()=>{
  for(const responseId of ['x'.repeat(128),'中'.repeat(42),'中'.repeat(42)+'ab','😀'.repeat(32)]) {
    const page=browser();
    page.incoming({kind:'text',text:'accepted',responseId});
    page.incoming({kind:'toolStatus',id:'call',state:'unknown',responseId});
  }
  const invalid=[undefined,null,1,true,{},[], '', 'x'.repeat(129),'中'.repeat(43),
    '中'.repeat(42)+'abc','😀'.repeat(33),'\ud800','\udc00',
    ...Array.from({length:32},(_,index)=>`r${String.fromCharCode(index)}x`),'r\x7fx'];
  for(const responseId of invalid) {
    const page=browser();
    assert.throws(()=>page.incoming({kind:'text',text:'must-not-append',responseId}),/invalid_text_response_id/);
    assert.throws(()=>page.incoming({kind:'toolStatus',id:'call',state:'unknown',responseId}),/invalid_tool_status/);
    assert.equal(page.transcript(),'');
  }
});
test('ASR桥调用只使用该content响应ID且不改变其它text消息入口',()=>{
  const main=fs.readFileSync(path.join(__dirname,'../adapters/desktop/browser_main.cpp'),'utf8');
  const branch=main.slice(main.indexOf('if (method == "respSttDone")'),main.indexOf('else if (method == "respDone"'));
  assert.ok(branch.replace(/\s+/g,' ').includes('content->text + "\\n助手：", content->response_id)'));
  assert.ok(branch.includes('display = browser_text(content->text);'));
  assert.ok(branch.includes('display = browser_text("\\n");'));
  assert.ok(branch.includes('display = browser_text(content->emoji);'));
  // 首文本测量可以只读同原回复ID，但只有ASR的browser_text投影携带此ID；不扩大其它文字入口。
  assert.equal((branch.match(/browser_text\([^;]*content->response_id\)/g)||[]).length,1);
  // 原宿主仍只发送一次，诊断观察必须在发送后且原失败条件不变，不能仅检查表达式出现。
  assert.equal((main.match(/bridge\.send\(display\)/g)||[]).length,1);
  const sendBegin=main.indexOf('const bool projected = !display.empty();');
  const sendEnd=main.indexOf('if (const auto diagnostic = runtime->take_message_diagnostics())');
  assert.ok(sendBegin>=0 && sendEnd>sendBegin);
  const sendBranch=main.slice(sendBegin,sendEnd);
  assert.match(sendBranch,/const bool projected = !display\.empty\(\);[^\n]*\n\s*const bool sent = projected && bridge\.send\(display\);/);
  assert.match(sendBranch,/const bool sent = projected && bridge\.send\(display\);[^\n]*\n\s*runtime->observe_content_display\(\*content, projected, sent\);\s*if \(projected && !sent\)\s*failed = done = true;/);
});

test('回复终态早于媒体停播时，最终帧数覆盖早期快照', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming({kind: 'responseCompleted', id: 'r1', audioState: 'sent',
    networkPackets: 16, decodedFrames: 16, nonSilentFrames: 0});
  assert.equal(page.metrics().lastMediaFinalResponseId, '');
  page.incoming(mediaFinal('r1', 120, 120, 95));
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  assert.equal(page.metrics().lastMediaFinalResponseId, 'r1');
  assert.equal(page.metrics().lastNetworkPackets, 120);
  assert.equal(page.metrics().lastNonSilentFrames, 95);
});

test('媒体停播早于MQTT回复终态时，迟到快照不能覆盖最终帧数', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(mediaFinal('r1', 120, 120, 95));
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  page.incoming({kind: 'responseCompleted', id: 'r1', audioState: 'sent',
    networkPackets: 16, decodedFrames: 16, nonSilentFrames: 0});
  assert.equal(page.metrics().completedResponses, 1);
  assert.equal(page.metrics().lastNetworkPackets, 120);
  assert.equal(page.metrics().lastNonSilentFrames, 95);
  page.incoming({kind: 'playbackStart', id: 'r2', epoch: '2'});
  assert.equal(page.metrics().lastMediaFinalResponseId, '');
  assert.equal(page.metrics().lastNetworkPackets, 0);
  page.incoming({kind: 'responseCompleted', id: 'r1', audioState: 'sent',
    networkPackets: 16, decodedFrames: 16, nonSilentFrames: 0});
  assert.equal(page.metrics().lastNetworkPackets, 0);
});

test('媒体最终事实只能归当前播放回复且计数必须有界', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  assert.throws(() => page.incoming(mediaFinal('old', 1, 1, 1)), /invalid_media_final/);
  assert.throws(() => page.incoming(mediaFinal('r1', 1, 2, 0)), /invalid_media_final/);
  page.incoming(mediaFinal('r1', 1, 1, 1));
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  assert.throws(() => page.incoming(mediaFinal('r1', 2, 2, 2)), /invalid_media_final/);
});

test('已消费有声帧也不能掩盖中间丢包、解码缺帧或末包未齐', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  assert.throws(() => page.incoming(mediaFinal('r1', 2, 2, 2, 1, true)), /invalid_media_final/);
  assert.throws(() => page.incoming(mediaFinal('r1', 2, 1, 1, 0, true)), /invalid_media_final/);
  assert.throws(() => page.incoming({kind: 'responseMediaFinal', id: 'r1',
    networkPackets: 2, decodedFrames: 2, nonSilentFrames: 2}), /invalid_media_final/);
  page.incoming(mediaFinal('r1', 2, 2, 2, 1, false));
  assert.equal(page.metrics().lastMissingPackets, 1);
  assert.equal(page.metrics().lastObservedComplete, false);
  page.incoming(mediaFinal('r1', 2, 2, 2, 0, false));
  assert.equal(page.metrics().lastObservedComplete, false);
});

test('有声消费按Worklet票据关联，静音、重复与旧代次不能凑数', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 0, 1));
  page.incoming(frame('1', 128, 2));
  page.incoming(frame('1', -128, 3));
  assert.equal(page.metrics().playableResponses, 1);
  assert.equal(page.metrics().consumedNonSilent, 0);
  page.played({epoch: '1', ticket: 1, nonSilent: false});
  assert.equal(page.metrics().consumedNonSilent, 0);
  page.played({epoch: '1', ticket: 2, nonSilent: true});
  page.played({epoch: '1', ticket: 2, nonSilent: true});
  assert.equal(page.metrics().consumedNonSilent, 1);
  assert.equal(page.metrics().lastPlayableConsumedResponseId, '');
  page.played({epoch: '1', ticket: 3, nonSilent: true});
  assert.equal(page.metrics().consumedNonSilent, 2);
  assert.equal(page.metrics().lastVoicedConsumedResponseId, 'r1');
  assert.equal(page.metrics().lastVoicedConsumedResponseFrames, 2);
  assert.equal(page.metrics().lastPlayableConsumedResponseId, 'r1');
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  page.played({epoch: '1', ticket: 3, nonSilent: true});
  assert.equal(page.metrics().consumedNonSilent, 2);
  page.incoming({kind: 'playbackStart', id: 'r2', epoch: '2'});
  page.incoming(frame('2', 128, 1));
  page.played({epoch: '1', ticket: 1, nonSilent: false});
  assert.equal(page.metrics().consumedNonSilent, 2);
  page.played({epoch: '2', ticket: 1, nonSilent: true});
  assert.equal(page.metrics().consumedNonSilent, 3);
  assert.equal(page.metrics().lastVoicedConsumedResponseId, 'r2');
  assert.equal(page.metrics().lastVoicedConsumedResponseFrames, 1);
  assert.equal(page.metrics().lastPlayableResponseId, '');
  assert.equal(page.metrics().lastPlayableConsumedResponseId, '');
});

test('Worklet仅按票据顺序确认连续两帧有声，静音不能跨越', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 128, 1));
  page.incoming(frame('1', 0, 2));
  page.incoming(frame('1', 128, 3));
  page.incoming(frame('1', -128, 4));
  page.played({epoch: '1', ticket: 3, nonSilent: true});
  assert.equal(page.metrics().consumedNonSilent, 0);
  page.played({epoch: '1', ticket: 1, nonSilent: true});
  page.played({epoch: '1', ticket: 2, nonSilent: false});
  page.played({epoch: '1', ticket: 3, nonSilent: true});
  assert.equal(page.metrics().lastVoicedConsumedResponseFrames, 2);
  assert.equal(page.metrics().lastPlayableConsumedResponseId, '');
  page.played({epoch: '1', ticket: 4, nonSilent: true});
  assert.equal(page.metrics().lastPlayableConsumedResponseId, 'r1');
});

test('新播放代次即使复用回复ID，也不得继承上一代次的首音事实', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 128, 1));
  page.incoming(frame('1', 128, 2));
  page.played({epoch: '1', ticket: 1, nonSilent: true});
  page.played({epoch: '1', ticket: 2, nonSilent: true});
  page.incoming(mediaFinal('r1', 2, 2, 2));
  assert.equal(page.metrics().lastPlayableResponseId, 'r1');
  assert.equal(page.metrics().lastPlayableConsumedResponseId, 'r1');
  assert.equal(page.metrics().lastMediaFinalResponseId, 'r1');
  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '2'});
  page.incoming(frame('2', 0, 1));
  page.played({epoch: '2', ticket: 1, nonSilent: false});
  assert.equal(page.metrics().lastPlayableResponseId, '');
  assert.equal(page.metrics().lastPlayableConsumedResponseId, '');
  assert.equal(page.metrics().lastVoicedConsumedResponseFrames, 0);
  assert.equal(page.metrics().lastMediaFinalResponseId, '');
});

test('浏览器拒绝非法播放票据与PCM，不能从坏帧生成有声事实', () => {
  const page = browser();
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  assert.throws(() => page.incoming(frame('1', 128, 0)), /invalid_playback_frame/);
  assert.throws(() => page.incoming(frame('1', NaN, 1)), /invalid_playback_frame/);
  assert.throws(() => page.incoming({kind: 'playbackFrame', epoch: '1', ticket: 1,
    samples: [128]}), /invalid_playback_frame/);
  assert.equal(page.metrics().decodedNonSilent, 0);
  assert.equal(page.metrics().consumedNonSilent, 0);
});

test('正式页面与Worklet联动：整帧输出后才确认有声消费，停播丢弃半帧', () => {
  let processor;
  const page = browser(value => processor.command(value));
  let Processor;
  class Port {
    constructor() {
      this.port = {postMessage(value) {
        if (value.kind === 'played') page.played(value);
        else assert.fail(`音频线程异常: ${value.kind}`);
      }};
    }
  }
  vm.runInNewContext(workletSource, {
    AudioWorkletProcessor: Port, sampleRate: 16000,
    registerProcessor: (_name, constructor) => { Processor = constructor; },
  });
  processor = new Processor({processorOptions: {sampleRate: 16000, frameMs: 20}});
  page.incoming({kind: 'playbackStart', id: 'r1', epoch: '1'});
  page.incoming(frame('1', 0, 1));
  page.incoming(frame('1', 128, 2));
  page.incoming(frame('1', -128, 3));
  assert.equal(page.metrics().lastPlayableResponseId, 'r1');
  assert.equal(page.metrics().consumedNonSilent, 0);
  assert.equal(processor.process([], [[new Float32Array(128)]]), true);
  assert.equal(page.metrics().consumed, 0);
  for (const length of [192, 320, 320])
    assert.equal(processor.process([], [[new Float32Array(length)]]), true);
  assert.equal(page.metrics().consumed, 3);
  assert.equal(page.metrics().consumedNonSilent, 2);
  assert.equal(page.metrics().lastPlayableConsumedResponseId, 'r1');

  page.incoming({kind: 'playbackStop', id: 'r1', epoch: '1'});
  page.incoming({kind: 'playbackStart', id: 'r2', epoch: '2'});
  page.incoming(frame('2', 128, 1));
  assert.equal(processor.process([], [[new Float32Array(128)]]), true);
  page.incoming({kind: 'playbackStop', id: 'r2', epoch: '2'});
  assert.equal(processor.process([], [[new Float32Array(320)]]), true);
  assert.equal(page.metrics().consumedNonSilent, 2);
  assert.equal(page.metrics().lastPlayableConsumedResponseId, '');
});

// audioFailureAssembly运行完整activate及真实页面/Worklet源码，以可控异步端口验证反馈而非假造ACK。
async function audioFailureAssembly(flag) {
  const audioMessages = [], pageMessages = [], sent = [], elements = new Map();
  let Processor, processor, nodeOptions;
  class ProcessorPort {
    constructor() { this.port = {postMessage: value => audioMessages.push(JSON.parse(JSON.stringify(value)))}; }
  }
  vm.runInNewContext(workletSource, {AudioWorkletProcessor: ProcessorPort, sampleRate: 16000,
    registerProcessor: (_name, constructor) => { Processor = constructor; }});
  const track = {readyState: 'live', getSettings: () => ({echoCancellation: false}), stop() { this.readyState = 'ended'; }};
  const stream = {getAudioTracks: () => [track], getTracks: () => [track], get active() { return track.readyState === 'live'; }};
  class Context {
    constructor(options) { this.sampleRate = options.sampleRate; this.state = 'suspended'; this.destination = {}; this.audioWorklet = {addModule: async () => {}}; }
    createMediaStreamSource() { return {connect() {}, disconnect() {}}; }
    async resume() { this.state = 'running'; }
    async close() { this.state = 'closed'; }
  }
  class Worklet {
    constructor(_context, name, options) {
      assert.equal(name, 'ur-device-audio'); nodeOptions = options.processorOptions;
      processor = new Processor(options);
      this.port = {postMessage: value => pageMessages.push(JSON.parse(JSON.stringify(value))), close() {}};
    }
    connect() {}
    disconnect() {}
  }
  class Socket {
    static OPEN = 1;
    constructor() { this.readyState = 1; this.bufferedAmount = 0; queueMicrotask(() => this.onopen()); }
    send(body) { sent.push(JSON.parse(body)); }
    close() { this.readyState = 3; }
  }
  const page = vm.createContext({UrAudioFailureDiagnostics: flag, TextEncoder, URLSearchParams, AudioContext: Context,
    AudioWorkletNode: Worklet, WebSocket: Socket, setTimeout, clearTimeout,
    fetch: async () => ({ok: true, json: async () => ({token: '0'.repeat(64)})}),
    location: {host: 'fixture.invalid'}, navigator: {mediaDevices: {getUserMedia: async () => stream}},
    document: {getElementById(id) { if (!elements.has(id)) elements.set(id, {textContent: '', addEventListener() {}}); return elements.get(id); }},
    window: {addEventListener() {}},
  });
  vm.runInContext(source, page);
  await page.activate();
  assert.ok(processor, '原activate必须实际装配主Worklet');
  assert.equal(vm.runInContext('terminated', page), false);
  // drain每方向保留独立队列，setImmediate代表事件循环轮次，不能把post当已经执行。
  const drain = async direction => {
    await new Promise(resolve => setImmediate(resolve));
    const queue = direction === 'page' ? audioMessages : pageMessages;
    for (const value of queue.splice(0)) {
      if (direction === 'page') page.receiveAudioThread(value);
      else processor.command(value);
    }
  };
  const flush = async () => {
    let passes = 0;
    while (audioMessages.length || pageMessages.length) {
      assert.ok(++passes <= 16, '合成端口不得自发无限产生命令');
      await drain('page'); await drain('audio');
    }
  };
  await flush();
  return {page, processor, nodeOptions, sent, drain, flush,
    incoming: value => page.incoming(value),
    render: () => processor.process([[new Float32Array(320)]], [[new Float32Array(128)]]),
    closed: () => vm.runInContext('terminated', page),
    fact: () => JSON.parse(JSON.stringify(page.UrReadAudioFailureDiagnostic())),
    status: () => elements.get('status').textContent,
    captured: () => vm.runInContext('metrics.captured', page),
  };
}

test('音频首因旁证组合：默认关闭/精确启用通过原activate选项，不改变101帧及ACK', async () => {
  for (const flag of [undefined, false, '1', 1, true]) {
    const r = await audioFailureAssembly(flag);
    assert.equal(r.nodeOptions.observeFailure === true, flag === true);
    assert.deepEqual(r.fact(), flag === true ? {known: false, cause: 'unknown'} : null);
    r.incoming({kind: 'playbackGate', ticket: '1', enabled: false});
    await r.drain('audio');
    assert.equal(r.sent.some(v => v.kind === 'outputGated'), false, '命令入队不能确认初始门控');
    r.render(); await r.flush();
    assert.equal(r.sent.filter(v => v.kind === 'outputGated').length, 1);
    r.incoming({kind: 'captureStart', id: 'private-original-input'}); await r.flush();
    for (let n = 0; n < 101; ++n) { assert.equal(r.render(), true); await r.flush(); }
    assert.equal(r.captured(), 101); assert.equal(r.processor.capturePending, 0); assert.equal(r.closed(), false);
    r.incoming({kind: 'captureStop', id: 'private-original-input'}); await r.flush();
    r.page.release('固定测试收尾');
    assert.deepEqual(r.fact(), flag === true ? {known: false, cause: 'unknown'} : null);
  }
});

test('音频首因旁证组合：17未确认帧、重复Start、零pendingACK保留原关闭并可事后读首因', async () => {
  for (const cause of ['captureBacklogFull', 'captureStartInvalid', 'zeroPendingAck']) {
    const r = await audioFailureAssembly(true);
    r.incoming({kind: 'captureStart', id: 'private-original-input'}); await r.flush();
    if (cause === 'captureBacklogFull') {
      for (let n = 0; n < 16; ++n) assert.equal(r.render(), true);
      assert.equal(r.closed(), false, '主线程未收件不能冒认为已经关闭');
      assert.equal(r.render(), false);
    } else if (cause === 'captureStartInvalid') {
      r.incoming({kind: 'captureStart', id: 'private-original-input'}); await r.drain('audio');
    } else r.processor.command({kind: 'captureAck', id: 'private-original-input'});
    await r.flush();
    assert.equal(r.closed(), true); assert.equal(r.status(), '浏览器音频驱动失败，已释放麦克风。');
    assert.deepEqual(r.fact(), {known: true, cause});
    const fact = r.page.UrReadAudioFailureDiagnostic(); fact.cause = 'private mutation';
    assert.deepEqual(r.fact(), {known: true, cause}, 'getter返回副本且不推进媒体或消费队列');
    assert.equal(r.sent.some(v => v.kind === 'audioFailureDiagnostic'), false, '独立Worklet事实不能成为桥协议');
  }
});

test('音频首因旁证组合：播放非法/队列满/门控冲突均沿原audioFailed关闭', async () => {
  for (const cause of ['playbackFrameInvalid', 'playbackQueueFull', 'playbackGateInvalid', 'playbackGateConflict', 'playbackGatePending']) {
    const r = await audioFailureAssembly(true);
    if (cause.startsWith('playbackGate')) {
      r.incoming({kind: 'playbackGate', ticket: '1', enabled: false}); await r.drain('audio');
      r.incoming({kind: 'playbackGate', ticket: cause === 'playbackGatePending' ? '2' : '1',
        enabled: true, ...(cause === 'playbackGateInvalid' ? {secret: 'private extra'} : {})});
    } else {
      r.incoming({kind: 'playbackStart', id: 'private-reply', epoch: '1'}); await r.drain('audio');
      // 畸形PCM会先被原页面拒绝，所以仅在原线程端施加其防御边界。
      if (cause === 'playbackFrameInvalid') r.processor.command({kind: 'playbackFrame', epoch: '1', ticket: 2, samples: []});
      else {
        for (let ticket = 1; ticket <= 32; ++ticket) r.incoming(frame('1', 0, ticket));
        await r.drain('audio');
        r.processor.command({kind: 'playbackFrame', epoch: '1', ticket: 33, samples: Array(320).fill(0)});
      }
    }
    await r.flush(); assert.equal(r.closed(), true);
    assert.deepEqual(r.fact(), {known: true, cause});
  }
});

test('音频首因旁证组合：畸形/未知/额外字段/访问器不能读取正文或改变原失败', async () => {
  const r = await audioFailureAssembly(true);
  let reads = 0;
  for (const value of [null, {kind: 'audioFailureDiagnostic'},
    {kind: 'audioFailureDiagnostic', cause: 'private cause'},
    {kind: 'audioFailureDiagnostic', cause: 'zeroPendingAck', private: 'private body'},
    {kind: 'audioFailureDiagnostic', cause: 'zeroPendingAck', [Symbol('private body')]: true},
    {kind: 'audioFailureDiagnostic', get cause() { ++reads; throw Error('private body'); }}]) {
    if (value !== null) r.page.receiveAudioThread(value);
  }
  assert.equal(reads, 0); assert.equal(r.closed(), false);
  assert.deepEqual(r.fact(), {known: false, cause: 'unknown'});
  r.processor.command({kind: 'unknown'}); await r.flush();
  r.page.receiveAudioThread({kind: 'audioFailureDiagnostic', cause: 'captureBacklogFull'});
  assert.deepEqual(r.fact(), {known: true, cause: 'unknownCommand'});
  assert.equal(JSON.stringify(r.fact()).includes('private'), false);
});

test('音频首因旁证组合：defaultoff未知收件仍按旧语义关闭，getter不能授资源', async () => {
  const r = await audioFailureAssembly(false);
  r.page.receiveAudioThread({kind: 'audioFailureDiagnostic', cause: 'captureBacklogFull'});
  assert.equal(r.closed(), true); assert.equal(r.fact(), null);
  r.processor.command({kind: 'unknown'}); await r.flush();
  assert.equal(r.closed(), true); assert.equal(r.fact(), null);
  assert.equal(r.status(), '浏览器音频驱动失败，已释放麦克风。');
});

test('音频首因旁证组合：unknown首因仍表示不可分类，后续清理与合法旁证不能覆盖', async () => {
  const r = await audioFailureAssembly(true);
  r.page.receiveAudioThread({kind: 'audioFailureDiagnostic', cause: 'unknown'});
  r.page.receiveAudioThread({kind: 'audioFailureDiagnostic', cause: 'zeroPendingAck'});
  r.processor.command({kind: 'unknown'}); await r.flush();
  assert.equal(r.closed(), true); assert.deepEqual(r.fact(), {known: false, cause: 'unknown'});
  assert.equal(r.status(), '浏览器音频驱动失败，已释放麦克风。');
});

// nativeAudioFailureAssets仅解释正式入口诊断分支的固定字面量，不能替测试另行注入flag。
function nativeAudioFailureAssets(flag) {
  const main = fs.readFileSync(path.join(__dirname, '../adapters/desktop/browser_main.cpp'), 'utf8');
  const block = main.match(/if \(env\("UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS"\) == "1"\) \{([\s\S]*?)\n    \}/);
  assert.ok(block, 'native必须精确1才装配固定诊断资产');
  const html = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/index.html'), 'utf8');
  const assets = {'/index.html': html, '/app.js': source,
    '/client-theme.js': fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/client-theme.js'), 'utf8')};
  if (flag !== '1') return assets;
  // 同时识别原inline与修复后的同源前缀，使RED覆盖真实旧分支而不是故意缺少夹具。
  const insertion = block[1].match(/assets\.at\("(\/index\.html|\/app\.js)"\)\s*\.insert\((marker|0),\s*("(?:[^"\\]|\\.)*")\);/);
  assert.ok(insertion, '仅允许固定资产和固定C++字面量');
  const [, asset, position, literal] = insertion;
  const offset = position === '0' ? 0 : assets[asset].indexOf('  <script src="/app.js"');
  assert.ok(offset >= 0);
  assets[asset] = assets[asset].slice(0, offset) + JSON.parse(literal) + assets[asset].slice(offset);
  return assets;
}

// loadSelfScriptPage采用桥实际HTTP测试锁定的script-src self规则；VM只执行允许的同源脚本。
function loadSelfScriptPage(assets) {
  const context = vm.createContext({document: {getElementById() { return {textContent: '', addEventListener() {}}; }},
    window: {addEventListener() {}}, TextEncoder});
  let blockedInline = 0;
  for (const script of assets['/index.html'].matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/g)) {
    const url = script[1].match(/\bsrc="([^"]+)"/);
    if (url) {
      assert.ok(['/client-theme.js', '/app.js'].includes(url[1]), '默认页只加载原同源入口');
      vm.runInContext(assets[url[1]], context);
    } else ++blockedInline;
  }
  return {context, blockedInline,
    fact: () => JSON.parse(vm.runInContext('JSON.stringify(UrReadAudioFailureDiagnostic())', context))};
}

test('音频首因旁证native实际CSP组合：精确1从同源app加载，旧inline不能启用getter', () => {
  const assets = nativeAudioFailureAssets('1');
  const page = loadSelfScriptPage(assets);
  assert.deepEqual(page.fact(), {known: false, cause: 'unknown'}, '原script-src self禁止inline，必须在实际app加载前开启');
  assert.equal(page.blockedInline, 0, '新开关不能依赖被CSP禁止的inline');
  assert.equal(assets['/index.html'], nativeAudioFailureAssets('')['/index.html']);
  assert.equal(assets['/app.js'], "'use strict';globalThis.UrAudioFailureDiagnostics=true;\n" + source);
  assert.throws(() => vm.runInContext(assets['/app.js'] + '\nundeclaredDiagnosticWrite = 1;',
    vm.createContext({document: {getElementById() { return {addEventListener() {}}; }},
      window: {addEventListener() {}}, TextEncoder})), /undeclaredDiagnosticWrite/, '前缀不得使原脚本退出严格模式');
  const legacy = {...nativeAudioFailureAssets('')};
  legacy['/index.html'] = legacy['/index.html'].replace('  <script src="/app.js"',
    '  <script>globalThis.UrAudioFailureDiagnostics=true;</script>\n  <script src="/app.js"');
  const oldPage = loadSelfScriptPage(legacy);
  assert.equal(oldPage.blockedInline, 1); assert.equal(oldPage.fact(), null);
});

test('音频首因旁证native实际CSP组合：defaultoff及非精确1资产逐字不变', () => {
  for (const flag of ['', '0', 'true', '01', '1 ', ' 1']) {
    const assets = nativeAudioFailureAssets(flag);
    assert.equal(assets['/app.js'], source);
    assert.equal(assets['/index.html'], fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/index.html'), 'utf8'));
    const page = loadSelfScriptPage(assets);
    assert.equal(page.fact(), null); assert.equal(page.blockedInline, 0);
  }
});

// captureAckThread以原constructor/command/process执行正式脚本，并用同线程模拟currentTime。
function captureAckThread(enabled, ownership = false, source = workletSource) {
  const events = [];
  const clock = {currentTime: 0};
  let Constructor;
  class Port { constructor() { this.port = {postMessage: v => events.push(JSON.parse(JSON.stringify(v)))}; } }
  vm.runInNewContext(source, {AudioWorkletProcessor: Port, sampleRate: 16000,
    get currentTime() { return clock.currentTime; }, registerProcessor: (_, value) => { Constructor = value; }});
  const p = new Constructor({processorOptions: {sampleRate: 16000, frameMs: 20,
    observeFailure: enabled, inputOwnership: ownership, ownershipBootGeneration: '7'}});
  const scope = {sessionId: 'scope', routeGeneration: '10'};
  if (ownership) {
    p.command({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
    p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  }
  p.command({kind: 'captureStart', id: 'input', ...(ownership ? scope : {})});
  return {p, events, clock, scope,
    frame() { clock.currentTime += 0.02; return p.process([[new Float32Array(320)]], [[new Float32Array(320)]]); }};
}

// captureAckPage运行完整正式脚本，而非只截取观察函数；业务socket发送与ACK只留本机闭集测试数据。
function captureAckPage(enabled, scriptSource = source) {
  const bodies = [], posted = [];
  const context = vm.createContext({TextEncoder, WebSocket: {OPEN: 1}, UrAudioFailureDiagnostics: enabled,
    document: {getElementById: () => ({addEventListener() {}, textContent: ''})},
    window: {addEventListener() {}}, sendBody: v => bodies.push(JSON.parse(v)), post: v => posted.push(v)});
  vm.runInContext(scriptSource, context);
  vm.runInContext('socket={readyState:1,bufferedAmount:0,send:sendBody};worklet={port:{postMessage:post}};', context);
  return {context, bodies, posted, receive: v => context.receiveAudioThread(v),
    read: () => context.UrReadCaptureAckDiagnostic(), set: text => vm.runInContext(text, context)};
}

test('默认关闭原帧和ACK/失败消息逐字段相同，零额外快照', () => {
  const changed = captureAckThread(false);
  for (let n = 0; n < 17; n++) changed.frame();
  assert.equal(changed.events.length, 17);
  for (const value of changed.events.slice(0, 16)) {
    assert.deepEqual(Object.keys(value).sort(), ['id', 'kind', 'samples']);
    assert.equal(value.kind, 'captured'); assert.equal(value.id, 'input');
    assert.equal(value.samples.length, 320); assert.ok(value.samples.every(sample => sample === 0));
  }
  assert.deepEqual(changed.events[16], {kind: 'audioFailed'});
  assert.equal(changed.events.filter(v => v.kind.includes('Diagnostic')).length, 0);
  assert.equal(changed.p.captureAckDiagnostic, null);
  const drained = captureAckThread(false);
  for (let n = 0; n < 64; n++) {
    assert.equal(drained.frame(), true); assert.equal(drained.p.capturePending, 1);
    drained.p.command({kind: 'captureAck', id: 'input'});
    assert.equal(drained.p.capturePending, 0);
  }
  assert.equal(drained.p.failed, false);
  assert.equal(drained.events.length, 64);
  const a = captureAckPage(false), frame = {kind: 'captured', id: 'input', samples: [0]};
  a.receive(frame);
  assert.deepEqual(a.bodies, [frame]);
  assert.deepEqual(JSON.parse(JSON.stringify(a.posted)), [{kind: 'captureAck', id: 'input'}]);
  assert.equal(a.read(), null);
});

test('完整原receive/send/ACK接纳计数与同线程age，仅失败时发送一次快照', () => {
  const r = captureAckThread(true), a = captureAckPage(true);
  for (let n = 0; n < 16; n++) r.frame();
  assert.equal(r.events.filter(v => v.kind === 'captureAckDiagnostic').length, 0);
  for (const v of r.events.splice(0)) a.receive(v);
  assert.equal(a.read().main.received, 16);
  assert.equal(a.read().main.sendAccepted, 16);
  assert.equal(a.read().main.ackPosted, 16);
  r.clock.currentTime += 0.1;
  for (const ack of a.posted.splice(0)) r.p.command(ack);
  assert.equal(r.p.capturePending, 0);
  assert.equal(r.p.captureAckDiagnostic.applied, 16);
  assert.ok(r.p.captureAckDiagnostic.maximumAgeMilliseconds >= 100);
  for (let n = 0; n < 17; n++) r.frame();
  const facts = r.events.filter(v => v.kind === 'captureAckDiagnostic');
  assert.equal(facts.length, 1);
  assert.equal(facts[0].pending, 16);
  assert.equal(facts[0].highWater, 16);
  assert.ok(Math.abs(facts[0].oldestPendingAgeMilliseconds - 320) < 0.000001);
  a.receive(facts[0]);
  const first = a.read(); first.thread.generated = 0;
  assert.notEqual(a.read().thread.generated, 0);
  assert.equal(a.read().threadRejected, false);
});

test('主线程过滤、发送背压与ACK拒绝闭计数，不添加业务发送或自动释放', () => {
  const a = captureAckPage(true), frame = {kind: 'captured', id: 'input', samples: [0], sessionId: 'scope', routeGeneration: '10'};
  a.set('inputOwnership={forwardCapture:()=>false,forwardControl:()=>true}'); a.receive(frame);
  assert.equal(a.read().main.filterRejected, 1); assert.equal(a.bodies.length, 0);
  a.set('inputOwnership={forwardCapture:()=>true,forwardControl:()=>false}'); a.receive(frame);
  assert.equal(a.read().main.ackRejected, 1); assert.equal(a.bodies.length, 1);
  a.set('release=()=>{};socket.bufferedAmount=65537'); a.receive(frame);
  assert.equal(a.read().main.socketBackpressure, 1);
  assert.equal(a.read().main.sendRejected, 1); assert.equal(a.bodies.length, 1);
});

test('原send异常保同一error，诊断post异常不阻断原audioFailed', () => {
  const a = captureAckPage(true); a.context.originalError = Error('controlled');
  a.set('socket.send=()=>{throw originalError}');
  assert.throws(() => a.receive({kind: 'captured', id: 'input', samples: [0]}), e => e === a.context.originalError);
  assert.equal(a.read().main.sendException, 1);
  const r = captureAckThread(true), messages = [];
  r.p.port.postMessage = v => { if (v.kind === 'captureAckDiagnostic') throw Error('controlled'); messages.push(v.kind); };
  for (let n = 0; n < 17; n++) r.frame();
  assert.equal(messages.filter(v => v === 'audioFailed').length, 1);
  assert.equal(r.p.failed, true);
  const broken = captureAckThread(true);
  Object.defineProperty(broken.p.captureAckDiagnostic, 'generated', {get() { throw Error('controlled'); }});
  assert.equal(broken.frame(), true);
  assert.equal(broken.p.capturePending, 1);
  assert.equal(broken.p.captureAckDiagnostic.overflow, true);
  const gate = captureAckPage(true); gate.context.originalError = Error('controlled');
  gate.set('inputOwnership={forwardCapture:()=>true,forwardControl:()=>{throw originalError}}');
  assert.throws(() => gate.receive({kind: 'captured', id: 'input', samples: [0],
    sessionId: 'scope', routeGeneration: '10'}), e => e === gate.context.originalError);
  assert.equal(gate.read().main.ackException, 1);
  assert.equal(gate.bodies.length, 1, '原发送一次已接受，不由诊断补发或吞错误');
});

test('线程快照未知/敏感键/访问器拒绝，不求getter或改变原首因', () => {
  const fact = captureAckThread(true).p.captureAckSnapshot();
  for (const change of [{...fact, identity: 'private'}, {...fact, clock: 'other'}, {...fact, pending: 17}]) {
    const a = captureAckPage(true); a.receive(change); assert.equal(a.read().threadRejected, true); assert.equal(a.read().thread, null);
  }
  const a = captureAckPage(true); let reads = 0;
  const bad = {...fact}; Object.defineProperty(bad, 'generated', {get() { reads++; throw Error('controlled'); }});
  a.receive(bad); assert.equal(reads, 0); assert.equal(a.read().threadRejected, true);
});
