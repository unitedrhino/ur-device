// 文件说明：真实MCP媒体验收器的离线契约回归；夹具不代表后端、模型或设备已完成联调。
'use strict';

// END回归复现原CLI对视口外按钮返回成功但未分发点击；修复必须只滚动原按钮再原点击，不改变控制或结束期限。
require('node:test')('原END共享入口先显式滚入视口，不以CLI成功冒充点击', async () => {
  const assert = require('node:assert/strict');
  const {endSessionViaUI} = require('./mcp_file_media_live.cjs');
  const calls=[];let belowViewport=true,clicks=0;
  await endSessionViaUI(async (...args) => {
    calls.push(args);assert.equal(args[1],'#end');
    if(args[0]==='scrollintoview')belowViewport=false;
    else if(args[0]==='click'&&!belowViewport)++clicks;
    return 'cli-success';
  });
  assert.deepEqual(calls,[['scrollintoview','#end'],['click','#end']]);assert.equal(clicks,1);
});
require('node:test')('原END滚动/点击失败原样传播，不重试、不裸发或绕过原按钮', async () => {
  const assert=require('node:assert/strict'),{endSessionViaUI}=require('./mcp_file_media_live.cjs');
  for(const failure of ['scrollintoview','click']){
    const calls=[],error=Error('synthetic-private-error');
    await assert.rejects(endSessionViaUI(async (...args)=>{calls.push(args);if(args[0]===failure)throw error;}),value=>value===error);
    assert.equal(calls.length,failure==='scrollintoview'?1:2);assert.ok(calls.every(v=>v[1]==='#end'));
  }
});
require('node:test')('原21与实际文字滚屏驱动均接同一END入口，无复制结束或通用按钮重构', () => {
  const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
  for(const file of ['mcp_file_media_live.cjs','browser_client_tool_live.cjs','browser_device_action_live.cjs',
    'browser_webrtc_text_live.cjs','browser_live.cjs']){
    const source=fs.readFileSync(path.join(__dirname,file),'utf8');
    assert.match(source,/await endSessionViaUI\(/);
    assert.equal((source.match(/await invoke\((?:\[)?['"]click['"],\s*['"]#end['"]/g)||[]).length,
      file==='mcp_file_media_live.cjs'?1:0); // 唯一原点击留在共享helper，其他场景不复制。
  }
});
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {steps, tools, validatePlan, installObserver, verifyStep, run} = require('./mcp_file_media_live.cjs');
const liveDiagnostics = require('./mcp_file_media_live.cjs'); // 原验收入口新增的安全诊断不能依赖媒体观察器已安装。
// replyDiagnosticFixture提供同轮冻结时刻，不能代替真实设备音频事实。
function replyDiagnosticFixture() {
  return {roundReplyId:'private-current', activeReplyId:'private-current', completedReplyId:'private-current',
    completedResponses:2, pendingPlaybackFrames:3,
    timing:{responseId:'private-current',lastVoiceSentMs:100,firstTextMs:120,firstNetworkMs:140,firstPlayableMs:160}};
}
test('排空计时投影缺raw、待播帧或仅合法时钟都不能声称已验消费',()=>{
  const scope={input:'voice',beforeCompletedResponses:1,beforeVoiceMS:50};
  for(const raw of [null,replyDiagnosticFixture(),{...replyDiagnosticFixture(),pendingPlaybackFrames:0}]) {
    const value=liveDiagnostics.projectReplyDiagnostic(raw,scope);
    assert.equal(value.timing.deviceConsumptionVerifiedNotTimed,false);
    assert.equal(value.timing.physicalSpeakerOnsetMeasured,false);
  }
});
test('逐轮首响应使用同回复最后有效语音时刻且只保存不可逆哈希',()=>{
  const value=liveDiagnostics.projectReplyDiagnostic(replyDiagnosticFixture(),{input:'voice',beforeCompletedResponses:1,beforeVoiceMS:50});
  assert.equal(value.replyHash,require('node:crypto').createHash('sha256').update('private-current').digest('hex'));
  assert.equal(value.sameActiveReply,true);assert.equal(value.pendingPlaybackFrames,3);
  assert.deepEqual([value.timing.firstTextMilliseconds,value.timing.firstNetworkAudioMilliseconds,value.timing.firstPlayableAudioMilliseconds],[20,40,60]);
  assert.equal(JSON.stringify(value).includes('private-current'),false);
});
test('文字、前轮残留、缺前轮基点、不匹配回复与跨时钟都明确未知',()=>{
  for(const [raw,scope,reason] of [
    [replyDiagnosticFixture(),{input:'text',beforeCompletedResponses:1,beforeVoiceMS:50},'current_input_not_voice'],
    [replyDiagnosticFixture(),{input:'voice',beforeCompletedResponses:1,beforeVoiceMS:100},'stale_voice_anchor'],
    [replyDiagnosticFixture(),{input:'voice',beforeCompletedResponses:1,beforeVoiceMS:null},'previous_voice_anchor_unavailable'],
    [{...replyDiagnosticFixture(),timing:{...replyDiagnosticFixture().timing,responseId:'private-old'}},{input:'voice',beforeCompletedResponses:0,beforeVoiceMS:null},'same_reply_timing_unavailable'],
    [{...replyDiagnosticFixture(),timing:{...replyDiagnosticFixture().timing,clock:'page_ms'}},{input:'voice',beforeCompletedResponses:0,beforeVoiceMS:null},'same_reply_timing_unavailable']]) {
    const value=liveDiagnostics.projectReplyDiagnostic(raw,scope);assert.equal(value.timing.primaryMeasured,false);
    assert.equal(value.timing.unavailable.firstPlayableAudioMilliseconds,reason);
  }
  for(const raw of [null,{...replyDiagnosticFixture(),pendingPlaybackFrames:33},{...replyDiagnosticFixture(),roundReplyId:'x'.repeat(129)},
    {...replyDiagnosticFixture(),extra:'PRIVATE'}])assert.equal(liveDiagnostics.projectReplyDiagnostic(raw,{}).known,false);
});
test('最后等待样本冻结在失败前，cleanup后时刻不能回填原样本',async()=>{
  const realm={metrics:{completedResponses:0,lastReplyTiming:null},responseId:'private-current',completedResponseId:'',
    pendingPlayback:new Map([[1,true]]),performance:{now:()=>10},__urMcpRound:{diagnosticReply:()=> 'private-current'}};
  const diagnostic={observeReply:true};
  await assert.rejects(liveDiagnostics.waitFixtureReply('cascaded_agentruntime',0,async expression=>{
    assert.equal(vm.runInNewContext(expression,realm),false);throw Error('synthetic-timeout');
  },diagnostic));
  realm.metrics.lastReplyTiming=replyDiagnosticFixture().timing;realm.pendingPlayback.clear();realm.responseId='';
  const sample=vm.runInNewContext('('+liveDiagnostics.readReplyDrainObservation.toString()+')('+diagnostic.beginMS+')',realm);
  assert.equal(sample.pendingPlaybackFrames,1);assert.equal(sample.timing,null);
  assert.equal(vm.runInNewContext('('+liveDiagnostics.readReplyDrainObservation.toString()+')(0)',realm),null);
});
test('诊断ASR绑定不借工具闭环，逐轮重置且64轮后拒绝旧回复',()=>{
  const realm={incoming:()=>true};vm.runInNewContext('('+liveDiagnostics.installRoundObserver.toString()+')(true)',realm);
  const observed=realm.__urMcpRound;
  observed.arm(['合成']);observed.diagnosticArm(['合成']);realm.incoming({kind:'text',text:'\n用户：合成\n助手：',responseId:'private-first'});
  assert.equal(observed.diagnosticReply(),'private-first');assert.equal(observed.facts().roundTerminal,false);
  assert.throws(()=>observed.arm(['下一轮']));assert.equal(observed.diagnosticArm(['下一轮']),true);
  assert.equal(observed.diagnosticReply(),'');realm.incoming({kind:'text',text:'\n用户：下一轮\n助手：',responseId:'private-first'});
  assert.equal(observed.diagnosticReply(),'');realm.incoming({kind:'text',text:'\n用户：下一轮\n助手：',responseId:'private-next'});
  assert.equal(observed.diagnosticReply(),'private-next');
  realm.incoming({kind:'text',text:'\n用户：下一轮\n助手：',responseId:'private-conflict'});assert.equal(observed.diagnosticReply(),'');
  for(let index=2;index<64;++index)assert.equal(observed.diagnosticArm(['合成']),true);
  assert.equal(observed.diagnosticArm(['合成']),false);assert.equal(observed.diagnosticReply(),'');
  const disabled={incoming:()=>true};vm.runInNewContext('('+liveDiagnostics.installRoundObserver.toString()+')()',disabled);
  assert.equal(disabled.__urMcpRound.diagnosticArm,undefined);
});
// replyDrainFactsFixture 只提供原页面计数，不模拟真实网络终态或媒体播放。
function replyDrainFactsFixture(realm) {
  return JSON.parse(JSON.stringify(vm.runInNewContext('(' + liveDiagnostics.readReplyDrainFacts.toString() + ')()', realm)));
}
test('回复排空旁证区分网络终态与独立播放队列，不暴露原回复ID', () => {
  const responseId = 'synthetic-private-response';
  const queued = replyDrainFactsFixture({metrics: {completedResponses: 2}, responseId,
    pendingPlayback: new Map([[7, true]])});
  assert.deepEqual(queued, {known: true, completedResponses: 2, responseActive: true, pendingPlaybackFrames: 1});
  assert.ok(!JSON.stringify(queued).includes(responseId));
  assert.deepEqual(replyDrainFactsFixture({metrics: {completedResponses: 3}, responseId: '', pendingPlayback: new Map()}),
    {known: true, completedResponses: 3, responseActive: false, pendingPlaybackFrames: 0});
});
test('回复排空旁证缺页面或畸形计数必须未知，不能猜测为排空', () => {
  for (const realm of [{}, {metrics: null}, {metrics: {completedResponses: -1}, responseId: '', pendingPlayback: new Map()},
    {metrics: {completedResponses: Infinity}, responseId: '', pendingPlayback: new Map()},
    {metrics: {completedResponses: 0}, responseId: null, pendingPlayback: new Map()},
    {metrics: {completedResponses: 0}, responseId: '', pendingPlayback: {size: 33}}])
    assert.deepEqual(replyDrainFactsFixture(realm), {known: false});
});
// inputDiagnosticFixture只提供合成incoming端口，正式纯观察函数与现场共用，不读取任务私有语料。
function inputDiagnosticFixture(enabled) {
  const received=[];
  const realm={incoming:value=>{received.push(value);return 'original_input_result';}};
  const installed=vm.runInNewContext('('+liveDiagnostics.installInputDiagnostic.toString()+')('+JSON.stringify(enabled)+')',realm);
  return {realm,received,installed,observer:realm.__urMcpInputDiagnostic};
}
test('纯输入旁证默认关闭不包装，显式开启也保持原incoming返回与唯一调用',()=>{
 for(const disabled of [false,undefined,'1',1]) {
  const fixture=inputDiagnosticFixture(disabled);
  assert.equal(fixture.installed,false);assert.equal(fixture.observer,undefined);
  assert.equal(fixture.realm.incoming({kind:'text',text:'合成输入'}),'original_input_result');
  assert.equal(fixture.received.length,1);
 }
 const fixture=inputDiagnosticFixture(true);fixture.observer.arm('邀请音乐，不要加载');
 assert.equal(fixture.realm.incoming({kind:'text',text:'\n用户：邀请音乐，不要加载。\n助手：'}),'original_input_result');
 assert.equal(fixture.received.length,1);assert.equal(fixture.observer.facts().normalizedExactMatch,true);
 assert.ok(!JSON.stringify(fixture.observer.facts()).includes('不要加载'));
});
test('纯输入旁证拒绝改写、过限和重复安装，release后不继续观察或留存期望',()=>{
 const fixture=inputDiagnosticFixture(true);
 for(const invalid of ['',null,1,'x'.repeat(4097)])assert.throws(()=>fixture.observer.arm(invalid),/invalid_fixed_fixture/);
 fixture.observer.arm('邀请音乐，不要加载');
 fixture.realm.incoming({kind:'text',text:'\n用户：停止音乐，不要加载\n助手：'});
 assert.equal(fixture.observer.facts().normalizedExactMatch,false);
 for(let index=0;index<8;++index)fixture.realm.incoming({kind:'text',text:'\n用户：邀请音乐，不要加载\n助手：'});
 assert.equal(fixture.observer.facts().known,false);
 assert.throws(()=>vm.runInNewContext('('+liveDiagnostics.installInputDiagnostic.toString()+')(true)',fixture.realm),/input_diagnostic_already_installed/);
 const before=JSON.stringify(fixture.observer.facts());fixture.observer.release();
 fixture.realm.incoming({kind:'text',text:'\n用户：邀请音乐，不要加载\n助手：'});
 assert.equal(JSON.stringify(fixture.observer.facts()),before);
 const oversized=inputDiagnosticFixture(true);oversized.observer.arm('合成');
 oversized.realm.incoming({kind:'text',text:'\n用户：'+'x'.repeat(8192)});
 assert.equal(oversized.observer.facts().known,false);
});
// captureFixture 明确模拟原半双工Busy与异步采集ACK；不生成网络或实际媒体事实。
function captureFixture({playing = false, reject = null} = {}) {
  const calls = [];
  let capturing = false;
  return {calls, get capturing() { return capturing; },
    start: async () => { calls.push('start'); if (!playing) calls.push('startAccepted'); },
    wait: async expression => {
      calls.push(expression);
      if (reject === expression) throw Error('fixture_wait_timeout');
      if (expression === 'responseId === "" && pendingPlayback.size === 0') playing = false;
      if (expression === '__urMcpObserver.captureOpen === true') capturing = true;
    }};
}
test('半双工测试录音须先实际排空再唯一开始并等待采集ACK', async () => {
  const fixture = captureFixture({playing: true});
  await liveDiagnostics.prepareFixtureCapture('cascaded_agentruntime', fixture);
  assert.deepEqual(fixture.calls, ['responseId === "" && pendingPlayback.size === 0', 'start', 'startAccepted',
    '__urMcpObserver.captureOpen === true']);
  assert.equal(fixture.capturing, true);
});
test('全双工不等回复排空或补发开始，但录音仍须已确认采集', async () => {
  const fixture = captureFixture({playing: true});
  await liveDiagnostics.prepareFixtureCapture('realtime_native', fixture);
  assert.deepEqual(fixture.calls, ['__urMcpObserver.captureOpen === true']);
  assert.equal(fixture.capturing, true);
});
test('采集排空或ACK缺失保持失败，绝不重发开始或提前放音', async t => {
  for (const reject of ['responseId === "" && pendingPlayback.size === 0', '__urMcpObserver.captureOpen === true'])
    await t.test(reject, async () => {
      const fixture = captureFixture({playing: true, reject});
      await assert.rejects(liveDiagnostics.prepareFixtureCapture('cascaded_agentruntime', fixture), /fixture_wait_timeout/);
      assert.equal(fixture.capturing, false);
      assert.equal(fixture.calls.filter(value => value === 'start').length,
        reject === '__urMcpObserver.captureOpen === true' ? 1 : 0);
    });
});
test('半双工每轮等待新网络终态和独立实际排空，首音或工具返回不是完成', async () => {
  const calls = [];
  await liveDiagnostics.waitFixtureReply('cascaded_agentruntime', 7, async expression => calls.push(expression));
  assert.deepEqual(calls, ['metrics.completedResponses > 7 && responseId === "" && pendingPlayback.size === 0']);
});
test('通用全双工回复等待不加排空屏障，专用网络终态验证另行执行', async () => {
  let waits = 0;
  await liveDiagnostics.waitFixtureReply('realtime_native', 7, async () => ++waits);
  assert.equal(waits, 0);
});

// roundFixture 只模拟桥已接纳的ContentSignal；响应及调用ID保留在测试闭包，不是实际设备或模型证据。
function roundFixture() {
  const received = [], sent = []; // 验证观察器不改变原incoming/send调用与采集行为。
  const realm = {incoming: value => {received.push(value); return 'original_result';},
    send: value => {sent.push(value);return true;}, metrics: {captured: 0},
    pendingPlayback: new Map([['still-playing', true]]), responseId: 'still-playing'};
  vm.createContext(realm);
  vm.runInContext('(' + liveDiagnostics.installRoundObserver.toString() + ')()', realm);
  const emit = value => realm.incoming(value); // 只走原接纳入口，不直接修改轮次判据。
  const recognize = responseId => emit({kind: 'text', text: '\n用户：请邀请音乐\n助手：', responseId});
  const tool = (state, responseId, id = 'private-call') => emit({kind: 'toolStatus',
    id, state, ...(state === 'started' ? {name: 'invite_media_playback'} : {}), responseId});
  return {realm, received, sent, emit, recognize, tool, arm: () => realm.__urMcpRound.arm(['邀请', '音乐']),
    facts: () => JSON.parse(JSON.stringify(realm.__urMcpRound.facts()))};
}
test('全双工绑定正确ASR和同回复工具后，仅同回复网络终态完成本轮', () => {
  const fixture = roundFixture(); // 前一轮终态和未知关联不能提供本轮完成证据。
  fixture.arm();fixture.emit({kind: 'responseCompleted', id: 'private-old'});
  assert.equal(fixture.facts().roundTerminal, false);
  fixture.recognize('private-current');fixture.tool('started','private-current');fixture.tool('succeeded','private-current');
  fixture.emit({kind: 'responseCompleted', id: 'private-old'});
  assert.equal(fixture.facts().roundTerminal,false);
  assert.equal(fixture.emit({kind:'responseCompleted',id:'private-current'}),'original_result');
  assert.equal(fixture.facts().roundTerminal,true);
  assert.doesNotThrow(() => liveDiagnostics.verifyRoundFacts(fixture.facts()));
  assert.ok(!JSON.stringify(fixture.facts()).includes('private'));
  assert.equal(fixture.received.length,6);
});
test('合法工具开始与结果先于最终ASR时，可信同回复链不能被永久判未知', () => {
  const fixture=roundFixture();fixture.arm(); // 对应生产speechStart后工具先到，最终转写稍后到的合法顺序。
  fixture.tool('started','private-current');fixture.tool('succeeded','private-current');
  assert.equal(fixture.facts().responseBound,false);assert.equal(fixture.facts().roundTerminal,false);
  fixture.recognize('private-current');fixture.emit({kind:'responseCompleted',id:'private-current'});
  assert.doesNotThrow(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
});
test('工具与网络终态先于ASR时只暂存同原回复，关键词确认后才完成', () => {
  const fixture=roundFixture();fixture.arm(); // 串行转发顺序不强制ASR先于原工具或终态。
  fixture.tool('started','private-current');fixture.tool('succeeded','private-current');
  fixture.emit({kind:'responseCompleted',id:'private-current'});
  assert.equal(fixture.facts().roundTerminal,false);
  fixture.recognize('private-current');
  assert.doesNotThrow(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
});
test('预ASR多候选、错误候选及孤立终态均不能提高为本轮成功', () => {
  for(const invalid of ['different_tools','different_terminal','different_asr','orphan']) {
    const fixture=roundFixture();fixture.arm(); // 候选只能按可信ID暂存，不能因后来的关键词抹掉冲突。
    if(invalid!=='orphan')fixture.tool('started','private-candidate');
    fixture.tool('succeeded',invalid==='different_tools'?'private-other':'private-candidate');
    fixture.emit({kind:'responseCompleted',id:invalid==='different_terminal'?'private-other':'private-candidate'});
    fixture.recognize(invalid==='different_asr'?'private-other':'private-candidate');
    assert.equal(fixture.facts().roundTerminal,false);
    assert.throws(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
  }
});
test('原已结束回复的迟到工具和终态不能成为新轮预ASR候选', () => {
  const fixture=roundFixture();fixture.arm(); // 只排除闭包已验证过的原回复，不猜未见过的ID归属。
  fixture.recognize('private-old');fixture.tool('started','private-old');fixture.tool('succeeded','private-old');
  fixture.emit({kind:'responseCompleted',id:'private-old'});fixture.arm();
  fixture.tool('started','private-old','private-old-late');fixture.tool('succeeded','private-old','private-old-late');
  fixture.emit({kind:'responseCompleted',id:'private-old'});
  assert.equal(fixture.facts().responseBound,false);assert.equal(fixture.facts().roundTerminal,false);
  fixture.tool('started','private-current');fixture.tool('succeeded','private-current');
  fixture.recognize('private-current');fixture.emit({kind:'responseCompleted',id:'private-current'});
  assert.doesNotThrow(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
});
test('预ASR工具账本最多128条，超限和重复工具终态失败，重复网络终态不改原事实', () => {
  const overflow=roundFixture();overflow.arm(); // 同一候选的真实工具关联只占有界Map，不保存自由正文。
  for(let index=0;index<128;++index)overflow.tool('started','private-current','private-call-'+index);
  assert.equal(overflow.facts().pendingCount,128);
  overflow.tool('started','private-current','private-overflow');
  assert.equal(overflow.facts().pendingCount,128);assert.equal(overflow.facts().associationInvalid,true);
  overflow.recognize('private-current');overflow.emit({kind:'responseCompleted',id:'private-current'});
  assert.throws(()=>liveDiagnostics.verifyRoundFacts(overflow.facts()));
  const duplicate=roundFixture();duplicate.arm(); // 孤立或重复工具终态不能被网络重复终态抹去。
  duplicate.tool('started','private-current');duplicate.tool('succeeded','private-current');
  duplicate.recognize('private-current');duplicate.emit({kind:'responseCompleted',id:'private-current'});
  const original=duplicate.facts(); // 固定安全facts不能携带或泄漏原回复标识。
  duplicate.emit({kind:'responseCompleted',id:'private-current'});
  assert.deepEqual(duplicate.facts(),original);
  duplicate.tool('succeeded','private-current');
  assert.equal(duplicate.facts().roundTerminal,false);
  assert.throws(()=>liveDiagnostics.verifyRoundFacts(duplicate.facts()));
});
test('旧桥缺失或非法可信responseId保持未知，绝不猜全局正在播放ID', () => {
  for (const responseId of [undefined, '', 1, 'bad\n', 'x'.repeat(129), '😀'.repeat(33), '\ud800', '\udc00']) {
    const fixture = roundFixture(); // 即便全局播放reply和网络终态存在，无可信ASR也不能绑定。
    fixture.arm();fixture.recognize(responseId);fixture.tool('started',responseId);fixture.tool('succeeded',responseId);
    fixture.emit({kind:'responseCompleted',id:'still-playing'});
    assert.equal(fixture.facts().responseBound,false);assert.equal(fixture.facts().roundTerminal,false);
    assert.throws(() => liveDiagnostics.verifyRoundFacts(fixture.facts()));
  }
});
test('关键词不符、跨回复工具或孤立工具终态不能满足本轮', () => {
  for (const invalid of ['keywords','wrong_tool_response','missing_tool_response','orphan']) {
    const fixture = roundFixture();fixture.arm(); // 每个异常使用独立轮次，不重试原输入。
    if(invalid==='keywords') fixture.emit({kind:'text',text:'\n用户：其他内容\n助手：',responseId:'private-current'});
    else fixture.recognize('private-current');
    if(invalid!=='orphan') fixture.tool('started',invalid==='missing_tool_response'?undefined:'private-current');
    fixture.tool('succeeded',invalid==='wrong_tool_response'?'private-other':'private-current');
    fixture.emit({kind:'responseCompleted',id:'private-current'});
    assert.equal(fixture.facts().roundTerminal,false);
  }
});
test('连续本轮arm拒绝重试，上一轮迟到ASR和网络终态不误满足新轮', () => {
  const fixture = roundFixture();fixture.arm(); // 同轮未结束前不能arm新意图或复用旧ID。
  assert.throws(() => fixture.arm());
  fixture.recognize('private-first');fixture.tool('started','private-first');fixture.tool('succeeded','private-first');
  fixture.emit({kind:'responseCompleted',id:'private-first'});fixture.arm();
  fixture.recognize('private-first');fixture.emit({kind:'responseCompleted',id:'private-first'});
  assert.equal(fixture.facts().responseBound,false);assert.equal(fixture.facts().roundTerminal,false);
  fixture.recognize('private-second');fixture.tool('started','private-second');fixture.tool('succeeded','private-second');
  fixture.emit({kind:'responseCompleted',id:'private-first'});
  assert.equal(fixture.facts().roundTerminal,false);
  fixture.emit({kind:'responseCompleted',id:'private-second'});
  assert.doesNotThrow(() => liveDiagnostics.verifyRoundFacts(fixture.facts()));
  assert.equal(fixture.facts().armIndex,2);
});
test('全双工专用评估只等本轮网络终态，采集增长且在播音频不用排空', async () => {
  const fixture = roundFixture(), waits=[];fixture.arm(); // 原播放队列故意保留，不假造actualdrain。
  await liveDiagnostics.waitFixtureNetworkRound('realtime_native', async expression => {
    waits.push(expression);fixture.recognize('private-current');fixture.tool('started','private-current');
    fixture.tool('succeeded','private-current');fixture.realm.send({kind:'captured'});++fixture.realm.metrics.captured;
    fixture.emit({kind:'responseCompleted',id:'private-current'});
    assert.equal(vm.runInContext(expression,fixture.realm),true);
  });
  assert.deepEqual(waits,['__urMcpRound.facts().roundTerminal === true']);
  assert.equal(fixture.realm.metrics.captured,1);assert.equal(fixture.realm.pendingPlayback.size,1);
  assert.equal(fixture.realm.responseId,'still-playing');assert.equal(fixture.sent.length,1);
  assert.doesNotThrow(() => liveDiagnostics.verifyRoundFacts(fixture.facts()));
  await liveDiagnostics.waitFixtureNetworkRound('cascaded_agentruntime',()=>assert.fail('unexpected_wait'));
});
test('全双工终态关联保持有界，非法arm、重复安装和缺失工具终态均不伪造完成', async () => {
  const fixture = roundFixture(); // 有界回合及原函数行为独立验证，不运行真实请求。
  for(const words of [[],[''],['x'.repeat(41)],['bad\n'],['a','b','c','d','e','f','g','h','i']])
    assert.throws(()=>fixture.realm.__urMcpRound.arm(words));
  assert.equal(fixture.facts().armed,false);
  assert.throws(()=>vm.runInContext('(' + liveDiagnostics.installRoundObserver.toString() + ')()',fixture.realm));
  for(let index=0;index<64;++index) {
    fixture.arm();fixture.recognize('private-response-'+index);fixture.tool('started','private-response-'+index);
    fixture.emit({kind:'responseCompleted',id:'private-response-'+index});
    assert.equal(fixture.facts().roundTerminal,false);
    fixture.tool('succeeded','private-response-'+index);
    assert.doesNotThrow(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
  }
  assert.throws(()=>fixture.arm());assert.equal(fixture.facts().armIndex,64);
  await assert.rejects(liveDiagnostics.waitFixtureNetworkRound('invalid',()=>assert.fail('unexpected_wait')));
  await assert.rejects(liveDiagnostics.waitFixtureNetworkRound('realtime_native',()=>{throw Error('fixture_network_timeout');}),
    /fixture_network_timeout/);
});
test('全双工额外同回复工具及重复开始即使网络终态已到仍不能满足单意图', () => {
  for(const duplicate of [false,true]) {
    const fixture=roundFixture();fixture.arm();fixture.recognize('private-current'); // 同回复多个工具不是唯一计划动作。
    fixture.tool('started','private-current');fixture.tool('started','private-current',duplicate?'private-call':'private-extra');
    fixture.tool('succeeded','private-current');
    if(!duplicate)fixture.tool('succeeded','private-current','private-extra');
    fixture.emit({kind:'responseCompleted',id:'private-current'});
    assert.throws(()=>liveDiagnostics.verifyRoundFacts(fixture.facts()));
  }
});
test('原回复网络终态或实际排空缺失必须传播超时，不取消或重试', async () => {
  let waits = 0;
  await assert.rejects(liveDiagnostics.waitFixtureReply('cascaded_agentruntime', 7, async () => {
    ++waits; throw Error('fixture_reply_timeout');
  }), /fixture_reply_timeout/);
  assert.equal(waits, 1);
});
test('未知链或非法回复水位在等待与副作用前拒绝', async t => {
  for (const value of [-1, 1.5, '1', null, NaN, Number.MAX_SAFE_INTEGER + 1])
    await t.test(String(value), async () => {
      await assert.rejects(liveDiagnostics.waitFixtureReply('cascaded_agentruntime', value, () => assert.fail('unexpected_wait')));
    });
  const fixture = captureFixture();
  await assert.rejects(liveDiagnostics.prepareFixtureCapture('untrusted', fixture));
  await assert.rejects(liveDiagnostics.waitFixtureReply('untrusted', 0, () => assert.fail('unexpected_wait')));
  assert.deepEqual(fixture.calls, []);
});
test('初始化诊断独立于未安装观察器，不回显自由UI文本或令牌', () => {
  const read = liveDiagnostics.readStartupFacts || (() => null);
  const context = {document:{getElementById:id=>({disabled:id==='start',textContent:'本地配对失败（初始化失败）；synthetic-private-token'})},
    context:{state:'running'},stream:{active:true},socket:{readyState:1},globalThis:{},};
  const result = vm.runInNewContext('('+read.toString()+')()',context);
  assert.ok(result); assert.equal(result.startEnabled,false); assert.equal(result.observerInstalled,false);
  assert.equal(result.initializationFailure,'local_pairing'); assert.ok(!JSON.stringify(result).includes('synthetic'));
});
test('startup失败保存固定细分阶段和类别，不伪造已识别根因', () => {
  const diagnostic = liveDiagnostics.startupDiagnostic || (()=>({phase:'startup'}));
  const result = diagnostic('session_ready',Error('browser_command_failed'),null,null);
  assert.equal(result.phase,'session_ready'); assert.equal(result.failureKind,'browser_command_failed');
  assert.equal(result.uiFacts,null);assert.equal(result.deviceFailure,null);
  assert.equal(diagnostic('browser_open',Error('free-private-token'),null,null).failureKind,'unclassified_failure');
});

// audioFailureFacts 在浏览器同一realm定义纯getter，验证可选读取不触发协议、媒体或自由异常输出。
function audioFailureFacts(expression, requested) {
  const realm = {document: {getElementById: id => ({disabled: id === 'start', textContent: ''})},
    context: {state: 'running'}, stream: {active: true}, socket: {readyState: 1}, calls: 0};
  vm.createContext(realm);
  vm.runInContext(expression, realm);
  const result = vm.runInContext('(' + liveDiagnostics.readStartupFacts.toString() + ')(' +
    (requested === undefined ? '' : JSON.stringify(requested)) + ')', realm);
  return {facts: JSON.parse(JSON.stringify(result)), calls: realm.calls};
}
test('音频首因诊断原默认九字段不变，未显式请求不能调用纯getter', () => {
  const expression = 'globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;throw Error("private-secret");};';
  const original = audioFailureFacts(expression).facts;
  assert.equal(Object.keys(original).length, 9);
  for (const requested of [undefined, false, '1', 1, null]) {
    const result = audioFailureFacts(expression, requested);
    assert.deepEqual(result.facts, original);
    assert.equal(result.calls, 0);
  }
});
test('显式音频首因读取保留knownfalse/unknown，缺失或异常为null而非无故障', () => {
  const known = audioFailureFacts('globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;return {known:false,cause:"unknown"};};', true);
  assert.deepEqual(known.facts.audioFailure, {known: false, cause: 'unknown'});
  assert.equal(known.calls, 1);
  for (const expression of ['', 'globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;return null;};',
    'globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;throw Error("private-secret");};']) {
    const result = audioFailureFacts(expression, true);
    assert.equal(result.facts.audioFailure, null);
    assert.equal(JSON.stringify(result.facts).includes('private-secret'), false);
  }
});
test('十八音频首因闭集及known组合严格核验，不保存自由字段或JSON来源证明', () => {
  const causes = ['configurationInvalid', 'commandInvalid', 'unknownCommand', 'captureStartInvalid',
    'zeroPendingAck', 'captureSampleInvalid', 'captureBacklogFull', 'playbackStartInvalid',
    'playbackFrameInvalid', 'playbackQueueFull', 'playbackGateInvalid', 'playbackGateConflict',
    'playbackGatePending', 'ownershipGateInvalid', 'ownershipTicketConflict', 'ownershipGatePending',
    'ownershipOwnerConflict', 'ownershipRouteCapacity']; // 与Root批准的纯getter合同一致，不代表现场原因已确认。
  for (const cause of causes) {
    const result = audioFailureFacts('globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;return ' +
      JSON.stringify({known: true, cause}) + ';};', true);
    assert.deepEqual(result.facts.audioFailure, {known: true, cause});
    assert.equal(result.calls, 1);
  }
  for (const body of [{known: true, cause: 'unknown'}, {known: false, cause: 'playbackQueueFull'},
    {known: 'true', cause: 'playbackQueueFull'}, {known: true, cause: 'private-secret'},
    {known: true}, {cause: 'unknown'}, {known: false, cause: 'unknown', body: 'private-secret'},
    [], false, 'private-secret']) {
    const result = audioFailureFacts('globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;return ' +
      JSON.stringify(body) + ';};', true);
    assert.equal(result.facts.audioFailure, null);
    assert.equal(result.calls, 1);
    assert.equal(JSON.stringify(result.facts).includes('private-secret'), false);
  }
});
test('音频首因函数或结果accessor一律拒绝且不执行getter，隐藏额外键亦拒绝', () => {
  for (const expression of [
    'Object.defineProperty(globalThis,"UrReadAudioFailureDiagnostic",{get(){calls++;throw Error("private-secret");}});',
    'globalThis.UrReadAudioFailureDiagnostic=()=>({get known(){calls++;throw Error("private-secret");},cause:"unknown"});',
    'globalThis.UrReadAudioFailureDiagnostic=()=>({known:false,get cause(){calls++;throw Error("private-secret");}});',
    'globalThis.UrReadAudioFailureDiagnostic=()=>Object.assign(Object.create({known:false}),{cause:"unknown"});',
    'globalThis.UrReadAudioFailureDiagnostic=()=>Object.defineProperty({known:false,cause:"unknown"},"body",{value:"private-secret"});',
    'globalThis.UrReadAudioFailureDiagnostic=()=>Object.assign({known:false,cause:"unknown"},{[Symbol("private-secret")]:true});'
  ]) {
    const result = audioFailureFacts(expression, true);
    assert.equal(result.facts.audioFailure, null);
    assert.equal(result.calls, 0);
    assert.equal(JSON.stringify(result.facts).includes('private-secret'), false);
  }
});

// 文件启动旁证必须显式开启；诊断失败不能替代真实门控或泄露页面自由字段。
test('文件启动旁证默认关闭，非法旗标和实时广播入口拒绝', () => {
  assert.equal(liveDiagnostics.fileStartupDiagnosticRequested(undefined, 'file'), false);
  assert.equal(liveDiagnostics.fileStartupDiagnosticRequested('', 'file'), false);
  assert.equal(liveDiagnostics.fileStartupDiagnosticRequested('1', 'file'), true);
  for (const [value, scenario] of [['0', 'file'], ['true', 'file'], ['1', 'live']])
    assert.throws(() => liveDiagnostics.fileStartupDiagnosticRequested(value, scenario));
});

test('文件启动旁证只读投影阶段和数量，拒绝自由文本且最多96项', () => {
  const read = liveDiagnostics.readFileStartupFacts;
  const scope = {globalThis: {fileStartupObservation: [{phase: 'audioGateSettled', lane: 'audio',
    success: false, elapsedMs: 12, visibility: 'visible', context: 'running',
    computedDisplay: 'none', connected: true, closing: false, actualFrames: null,
    paintCancelled: false, failure: 'timeout', secret: 'synthetic-private-secret'},
    {phase: 'synthetic-private-secret', lane: 'synthetic-private-secret', elapsedMs: Infinity,
      actualFrames: -1, failure: 'synthetic-private-secret'}]}};
  const result = vm.runInNewContext('(' + read.toString() + ')()', scope);
  assert.equal(result.available, true);
  assert.equal(result.events[0].phase, 'audioGateSettled');
  assert.equal(result.events[0].failure, 'timeout');
  assert.equal(result.events[1].phase, 'unknown');
  assert.equal(result.events[1].elapsedMs, null);
  assert.equal(result.events[1].actualFrames, null);
  assert.ok(!JSON.stringify(result).includes('synthetic-private'));
  scope.globalThis.fileStartupObservation = Array.from({length: 100}, () => ({phase: 'rafActual'}));
  const bounded = vm.runInNewContext('(' + read.toString() + ')()', scope);
  assert.equal(bounded.events.length, 96);
  assert.equal(bounded.truncated, true);
  delete scope.globalThis.fileStartupObservation;
  assert.equal(vm.runInNewContext('(' + read.toString() + ')()', scope).available, false);
});
test('C++与CLI诊断仅提取固定分类和稳定code，自由method/body不保存',()=>{
  assert.deepEqual(liveDiagnostics.deviceFailureCategory(Buffer.from('request_rejected method=synthetic-private code=403\n')),{kind:'request_rejected',code:403});
  assert.deepEqual(liveDiagnostics.deviceFailureCategory(Buffer.from('invalid_device_environment\n')),{kind:'invalid_device_environment',code:null});
  assert.equal(liveDiagnostics.deviceFailureCategory(Buffer.from('synthetic-private-secret')),null);
  assert.equal(liveDiagnostics.browserFailureCategory(null,{error:'Timeout 30000ms exceeded synthetic-private'}),'browser_timeout');
  assert.equal(liveDiagnostics.browserFailureCategory(null,{error:'Failed to launch browser synthetic-private'}),'browser_launch_failed');
  assert.equal(liveDiagnostics.browserFailureCategory(null,{error:'arbitrary-private-message'}),'browser_command_failed');
});
test('C++初始化拒绝只保留原协议方法白名单，未知方法仍丢弃',()=>{
  for (const method of ['capabilityReport', 'sessionCreate', 'mediaOffer', 'mediaState', 'audioStart', 'audioStop', 'respCancel', 'sessionEnd', 'inputSend']) {
    assert.deepEqual(liveDiagnostics.deviceFailureCategory(Buffer.from(`request_rejected method=${method} code=403\n`)),
      {kind:'request_rejected',code:403,method});
  }
  assert.deepEqual(liveDiagnostics.deviceFailureCategory(Buffer.from('request_rejected method=sessionCreate-private code=403\n')),
    {kind:'request_rejected',code:403});
  assert.deepEqual(liveDiagnostics.deviceFailureCategory(Buffer.from('request_rejected method=capabilityReport code=403 private=synthetic-secret\n')),
    {kind:'request_rejected',code:403,method:'capabilityReport'});
});
test('能力基线403仅识别两种原ACK固定原因，未知正文及无关联方法仍丢弃',()=>{
  for(const reason of ['focusDeviceUnavailable','requestDenied'])assert.deepEqual(
    liveDiagnostics.deviceFailureCategory(Buffer.from(`request_rejected method=capabilityReport code=403 reason=${reason}\n`)),
    {kind:'request_rejected',code:403,method:'capabilityReport',reason});
  for(const line of ['request_rejected method=capabilityReport code=403 reason=synthetic-private',
    'request_rejected method=sessionCreate code=403 reason=requestDenied',
    'request_rejected method=capabilityReport code=409 reason=requestDenied']){
    const result=liveDiagnostics.deviceFailureCategory(Buffer.from(line+'\n'));assert.ok(!Object.hasOwn(result,'reason'));
  }
});
test('stderr跨片段完整行后才分类原因，超长整行不得截断成成功诊断',()=>{
  const create=liveDiagnostics.createDeviceFailureObserver; // 真实收集器不得用逐chunk旧逻辑替代完整行关联。
  const observer=create();assert.equal(observer.push(Buffer.from('request_rejected method=capabilityReport code=403')),null);
  assert.equal(observer.push(Buffer.from(' reason=focusDevice')),null);
  assert.deepEqual(observer.push(Buffer.from('Unavailable\n')),{kind:'request_rejected',method:'capabilityReport',code:403,reason:'focusDeviceUnavailable'});
  assert.deepEqual(observer.finish(),{kind:'request_rejected',method:'capabilityReport',code:403,reason:'focusDeviceUnavailable'});
  const tooLong=create();assert.equal(tooLong.push(Buffer.from('request_rejected method=capabilityReport code=403 '+ 'x'.repeat(4096))),null);
  assert.equal(tooLong.push(Buffer.from(' reason=requestDenied\n')),null);assert.equal(tooLong.finish(),null);
  const tail=create();assert.equal(tail.push(Buffer.from('request_rejected method=capabilityReport code=403 reason=requestDenied')),null);
  assert.equal(tail.finish().reason,'requestDenied');
});

test('stdout仅完整固定voice_state与宿主退出分支入账，无分支不凭Idle或exit0猜测',()=>{
  const observer=liveDiagnostics.createDeviceLifecycleObserver?.();
  assert.ok(observer, 'bounded_lifecycle_observer_required');
  observer.push(Buffer.from('voice_sta'));
  assert.equal(observer.facts().voiceStates.length,0);
  observer.push(Buffer.from('te=4\nvoice_state=0\nprivate-secret\n'));
  assert.deepEqual(observer.facts().voiceStates,[4,0]);
  assert.equal(observer.facts().normalHostExitBranch,null);
  observer.push(Buffer.from('host_exit_branch=runtime_idle\n'));
  assert.equal(observer.facts().normalHostExitBranch,'runtime_idle');
  assert.ok(!JSON.stringify(observer.facts()).includes('private'));
});
test('stdout拒绝自由后缀和越界state，超长整行不截断投影，末行只在排空后记账',()=>{
  const observer=liveDiagnostics.createDeviceLifecycleObserver?.();
  assert.ok(observer, 'bounded_lifecycle_observer_required');
  observer.push(Buffer.from('voice_state=7\nvoice_state=4 private-secret\nhost_exit_branch=free-secret\n'));
  observer.push(Buffer.from('voice_state=0'+ 'x'.repeat(4096)+'\nvoice_state=6'));
  assert.deepEqual(observer.facts().voiceStates,[]);
  observer.finish();observer.finish();
  assert.deepEqual(observer.facts().voiceStates,[6]);
  assert.equal(observer.facts().discardedOverlongLines,1);
  assert.equal(observer.facts().stdoutEnded,true);
  assert.equal(observer.facts().normalHostExitBranch,null);
  assert.ok(!JSON.stringify(observer.facts()).includes('secret'));
});
test('stdout状态历史有界而不改变退出事实，返回快照不能修改内部诊断',()=>{
  const observer=liveDiagnostics.createDeviceLifecycleObserver?.();
  assert.ok(observer, 'bounded_lifecycle_observer_required');
  observer.push(Buffer.from('voice_state=4\n'.repeat(80)));
  const facts=observer.facts();assert.equal(facts.voiceStates.length,64);
  assert.equal(facts.observationTruncated,true);assert.equal(facts.normalHostExitBranch,null);
  facts.voiceStates.push(0);assert.equal(observer.facts().voiceStates.length,64);
});

// toolDiagnosticFixture 仅模拟原UI出口，观察局部工具/终态顺序，不启动媒体或生成协议事实。
function toolDiagnosticFixture(originalIncoming=()=>true) {
  const realm={incoming:originalIncoming,send:()=>true,desktopFiles:{player:null},source:{disconnect(){}},
    context:{state:'running',createConstantSource:()=>({offset:{},connect(){},start(){}})},worklet:{},
    metrics:{focusReceipts:[],focusReceiptIndex:0},stream:{active:true}};
  vm.createContext(realm);vm.runInContext('('+installObserver.toString()+')()',realm);
  return {realm,snapshot:()=>vm.runInContext('('+liveDiagnostics.readSnapshot.toString()+')()',realm)};
}
test('文件工具名仅接受正式裸名或things_iot_mcp全名，保留原unknown状态',()=>{
  for(const tool of [...new Set(Object.values(tools)), 'get_output_focus_status', 'get_media_playback_status', 'recover_media_playback_operation']){
    for(const name of [tool, 'things_iot_mcp.'+tool]){
      const {realm,snapshot}=toolDiagnosticFixture();
      realm.incoming({kind:'toolStatus',id:'original-call',name,state:'started'});
      realm.incoming({kind:'toolStatus',id:'original-call',state:'unknown'});
      assert.equal(snapshot().tools[0].name,'things_iot_mcp.'+tool);
      assert.equal(snapshot().tools[0].state,'unknown');
    }
  }
});
test('文件观察器拒绝其它服务及伪嵌套同后缀名，不把它算正式媒体工具',()=>{
  for(const name of ['other_service.control_output_focus','other.things_iot_mcp.control_media_playback',
    'things_iot_mcp.control_output_focus.extra','prefixcontrol_output_focus']){
    const {realm,snapshot}=toolDiagnosticFixture();
    realm.incoming({kind:'toolStatus',id:'original-call',name,state:'started'});
    realm.incoming({kind:'toolStatus',id:'original-call',state:'succeeded'});
    assert.equal(snapshot().tools[0].name,'other_authorized_tool');
    assert.equal(snapshot().tools[0].state,'succeeded');
  }
});
test('文件观察区分未完成工具和孤立结果，终态前后顺序不输出原调用ID或正文',()=>{
  const {realm,snapshot}=toolDiagnosticFixture();
  realm.incoming({kind:'toolStatus',id:'private-orphan',state:'succeeded'});
  realm.incoming({kind:'toolStatus',id:'private-call',name:'private.tool',state:'started'});
  const pending=snapshot();assert.equal(pending.tools.length,0);
  assert.equal(pending.toolDiagnostic.startedCount,1);assert.equal(pending.toolDiagnostic.pendingCount,1);
  assert.equal(pending.toolDiagnostic.orphanResultCount,1);
  realm.incoming({kind:'ended'});
  realm.incoming({kind:'toolStatus',id:'private-call',state:'succeeded'});
  const result=snapshot();assert.equal(result.tools.length,1);
  assert.equal(result.toolDiagnostic.pendingCount,0);assert.equal(result.toolDiagnostic.terminalCount,1);
  assert.deepEqual(Array.from(result.toolDiagnostic.events,event=>event.kind),['orphan_result','started','ended','result']);
  assert.equal(result.toolDiagnostic.firstTerminalSequence,3);
  assert.ok(!JSON.stringify(result).includes('private'));
});
test('工具诊断最多64项且独立于原验收overflow，不重发或改变历史成功结果',()=>{
  const {realm,snapshot}=toolDiagnosticFixture();
  for(let index=0;index<80;++index)realm.incoming({kind:'toolStatus',id:'private-orphan-'+index,state:'unknown'});
  const result=snapshot();assert.equal(result.toolDiagnostic.events.length,64);
  assert.equal(result.toolDiagnostic.observationTruncated,true);assert.equal(result.toolDiagnostic.orphanResultCount,80);
  assert.equal(result.overflow,false);assert.equal(result.tools.length,0);
  result.toolDiagnostic.events.push({kind:'forged'});assert.equal(snapshot().toolDiagnostic.events.length,64);
});
test('原UI拒绝报文不被诊断接受为事实，同调用重复started仍仅一个pending',()=>{
  const failed=toolDiagnosticFixture(()=>{throw Error('synthetic-private');});
  assert.throws(()=>failed.realm.incoming({kind:'ended'}));
  assert.equal(failed.snapshot().toolDiagnostic.terminalCount,0);
  const {realm,snapshot}=toolDiagnosticFixture();
  const started={kind:'toolStatus',id:'private-call',name:'things_iot_mcp.control_output_focus',state:'started'};
  realm.incoming(started);realm.incoming(started);
  assert.equal(snapshot().toolDiagnostic.startedCount,2);assert.equal(snapshot().toolDiagnostic.pendingCount,1);
  realm.incoming({kind:'toolStatus',id:'private-call',state:'unknown'});
  assert.equal(snapshot().tools.length,1);assert.equal(snapshot().tools[0].state,'unknown');
});

// failureLifecycle 通过可控进程/浏览器端口模拟迟到诊断，不启动设备、浏览器、网络或真实API。
async function failureLifecycle({stderr='request_rejected method=capabilityReport code=403 reason=requestDenied\n',
  browserStdout='{}',browserStderr='',browserCode=null,closeFails=false,stdoutBefore='',stdoutOnCleanup='',
  startupDiagnostic=false,audioFailureSwitch=undefined,messageDiagnosticSwitch=undefined,audioRealm=null,onCommand=()=>{}}={}){
  const {EventEmitter}=require('node:events'),fs=require('node:fs'),path=require('node:path');
  const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();let killCalls=0;
  child.kill=()=>{killCalls++;queueMicrotask(()=>{if(stdoutOnCleanup)child.stdout.emit('data',Buffer.from(stdoutOnCleanup));child.stdout.emit('end');
    if(stderr)child.stderr.emit('data',Buffer.from(stderr));child.stderr.emit('end');child.emit('close',1);});return true;};
  const output=[];const execFile=(_binary,args,_options,callback)=>queueMicrotask(()=>{
    onCommand(args);
    if(args.includes('wait')){const error=Error('synthetic-private-browser-error');if(browserCode)error.code=browserCode;
      return callback(error,browserStdout,browserStderr);}
    if(args.includes('close')&&closeFails)return callback(Object.assign(Error('synthetic-private-cleanup'),{code:'ENOENT'}),'{}','synthetic-private-cleanup');
    if(args.includes('eval')) {
      const expression=Buffer.from(args.at(-1),'base64').toString('utf8');
      const result=expression.startsWith('(function readStartupFacts')&&audioRealm?vm.runInContext(expression,audioRealm):
        expression.includes('installFileStartupObservation')?true:
        expression.includes('readFileStartupFacts')?{available:true,truncated:false,events:[]}:null;
      return callback(null,JSON.stringify({success:true,data:{result}}));
    }
    callback(null,JSON.stringify({success:true,data:{}}));
  });
  const module={exports:{}};const source=fs.readFileSync(path.join(__dirname,'mcp_file_media_live.cjs'),'utf8');
  const requirePort=name=>name==='node:child_process'?{spawn:()=>{queueMicrotask(()=>{child.emit('spawn');child.stdout.emit('data',Buffer.from('http://127.0.0.1:39741/\n'+stdoutBefore));});return child;},execFile}:
    name==='node:fs'?{statSync:()=>({isFile:()=>true}),readFileSync:()=>Buffer.from(JSON.stringify(plan()))}:require(name);
  vm.runInNewContext(source,{module,exports:module.exports,require:requirePort,Buffer,console:{log:line=>output.push(JSON.parse(line)),error:()=>{}},setTimeout,clearTimeout});
  await assert.rejects(module.exports.run({UR_MCP_FILE_LIVE:'1',UR_OUTPUT_FOCUS:'media',UR_FILE_CATALOG:'synthetic',UR_DEVICE_PRODUCT:'synthetic',UR_DEVICE_NAME:'synthetic',UR_DEVICE_SECRET:'synthetic-private',UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:'1883',UR_DEVICE_BROWSER_BIN:'/synthetic/device',UR_MCP_FILE_PLAN:'/synthetic/plan',UR_BROWSER_PORT:'39741',
    ...(startupDiagnostic?{UR_MCP_STARTUP_DIAGNOSTIC:'1'}:{}),
    ...(audioFailureSwitch===undefined?{}:{UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS:audioFailureSwitch}),
    ...(messageDiagnosticSwitch===undefined?{}:{UR_DEVICE_MESSAGE_DIAGNOSTICS:messageDiagnosticSwitch})}));
  assert.equal(killCalls,1);assert.equal(output.length,1);return output[0];
}

test('实际runner精确消息开关必须保留原stdout首因且catch/finally不能改写或新增采样',async()=>{
  const first={kind:'runtimeFailureDiagnostic',runtime:'mediaPoll',udp:'pcmWrite',pcm:'bufferFull',observation:1};
  const commands=[]; // 只观察原命令，不在诊断分支新增进程或页面调用。
  const report=await failureLifecycle({stderr:'',messageDiagnosticSwitch:'1',
    stdoutBefore:'host_failure='+JSON.stringify(first)+'\n',
    stdoutOnCleanup:'host_failure='+JSON.stringify({...first,runtime:'clientTick',udp:'none',pcm:'none'})+'\n',
    onCommand:args=>commands.push(args)});
  assert.deepEqual(JSON.parse(JSON.stringify(report.diagnostic.deviceLifecycleBeforeCleanup.runtimeFailure)),first);
  assert.deepEqual(JSON.parse(JSON.stringify(report.diagnostic.deviceLifecycle.runtimeFailure)),first);
  assert.equal(report.diagnostic.failureKind,'browser_command_failed');
  assert.equal(report.passed,false);
  const baseline=[];await failureLifecycle({stderr:'',onCommand:args=>baseline.push(args)});
  assert.deepEqual(commands.map(args=>args[args.indexOf('--json')+1]),baseline.map(args=>args[args.indexOf('--json')+1]));
});

// runtimeFailureFixture 只有闭集原native合成输出；没有会话/工具身份或实际媒体成功证明。
function runtimeFailureFixture(change = {}) {
  return {kind:'runtimeFailureDiagnostic',runtime:'mediaPoll',udp:'pcmWrite',pcm:'bufferFull',observation:1,...change};
}
// 新本机首因是独立固定行；不混入既有runtime五字段，不影响原成功判据。
test('客户端细分首因仅显式观察并一次锁存，清理不覆盖', () => {
  const value = {kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'requestsTimeout',method:'audioStop',observation:1};
  const observer = liveDiagnostics.createDeviceLifecycleObserver(true);
  observer.push(Buffer.from('host_client_failure=' + JSON.stringify(value) + '\n'));
  assert.deepEqual(observer.facts().clientFailure, value);
  const copy = observer.facts(); copy.clientFailure.method = 'private token';
  observer.push(Buffer.from('host_client_failure=' + JSON.stringify({...value,method:'respCancel'}) + '\n'));
  observer.finish();
  assert.deepEqual(observer.facts().clientFailure, value);
  assert.equal(observer.facts().runtimeFailure, null);
  const off = liveDiagnostics.createDeviceLifecycleObserver();
  off.push(Buffer.from('host_client_failure=' + JSON.stringify(value) + '\n'));
  assert.equal(Object.hasOwn(off.facts(), 'clientFailure'), false);
});
test('客户端细分严格核固定来源原因方法组合及UTF8，不输出自由字段', () => {
  const original = {kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'requestsTimeout',method:'audioStop',observation:1};
  const parse = value => liveDiagnostics.clientFailureCategory(Buffer.from('host_client_failure=' + JSON.stringify(value)));
  assert.deepEqual(parse(original),original);
  const clientCauses = ['unknown','sessionError','activeScopeLost','candidateClock','candidateEstablishDeadline',
    'candidateRecoveryDeadline','visionProfileInvalid','mediaRecoveryDeadline','negotiatingDeadline'];
  for (const cause of clientCauses) {
    const value = {...original,cause,method:'none'};
    assert.deepEqual(parse(value),value);
    assert.equal(parse({...value,method:'audioStart'}),null);
  }
  for (const cause of ['unknown','endpointPrepareDeadline','endpointPrepareClock','endpointPrepareDriver','endpointPrepareDispatch']) {
    const value = {...original,origin:'endpointPrepareFailed',cause,method:'none'};
    assert.deepEqual(parse(value),value);
  }
  for (const method of ['unknown','capabilityReport','sessionCreate','sessionAccept','sessionReject','mediaState','mediaOffer',
    'audioStart','audioStop','respCancel','sessionEnd','inputSend','sessionSync','endpointControl'])
    for (const cause of ['requestsTimeout','requestRejected']) assert.deepEqual(parse({...original,cause,method}),{...original,cause,method});
  for (const value of [{...original,text:'private token'},{...original,cause:'free error'},
    {...original,method:'private token'},{...original,origin:'free error'},{...original,observation:2},
    {...original,method:'none'},{...original,origin:'endpointPrepareFailed'},
    {...original,cause:'endpointPrepareDeadline',method:'none'}]) assert.equal(parse(value),null);
  const body = JSON.stringify(original);
  for (const text of [body.replace('"method":','"method":"respCancel","method":'),
    body.replace('"method":','"met\\u0068od":'),body.replace('"observation":1','"observation":1.0'),body+' trailing'])
    assert.equal(liveDiagnostics.clientFailureCategory(Buffer.from('host_client_failure='+text)),null);
  assert.equal(liveDiagnostics.clientFailureCategory(Buffer.concat([Buffer.from('host_client_failure='),Buffer.from([0xc0,0xaf]),Buffer.from(body)])),null);
});
test('原首因解析核所有runtime及UDP/PCM文本闭集，未知缺失仍是unknown而非推成功',()=>{
  const parse=value=>liveDiagnostics.runtimeFailureCategory(Buffer.from('host_failure='+JSON.stringify(value)));
  const runtimes=['mqttConnect','mqttDisconnected','mqttEventFailure','mqttSubscription','clockRollback',
    'connectionDeadline','mqttPoll','signalEnvelope','signalClientInvalid','signalClientDriver','messageEnvelope',
    'messageAck','messageContent','messageToolScope','messageQueueFull','actionReplyPublish','clientTick','toolPoll',
    'mediaPoll','mediaOffer','mediaReady','missingWallClock','drainDeadline','sharedInputInvalid','unknown'];
  const udp=['none','unconfigured','clockRollback','probeReceive','probeSend','probeDeadline','captureRead',
    'captureEncode','captureEncrypt','captureSend','replyReceive','replyCrypto','replyBeyondEnd','pcmWrite','playbackStall','unknown'];
  const pcm=['none','driverClosed','playbackInactive','epochMismatch','frameSize','bufferFull','ticketExhausted','queueRejected','unknown'];
  for(const runtime of runtimes){const value=runtimeFailureFixture({runtime,udp:'none',pcm:'none'});assert.deepEqual(parse(value),value);}
  for(const kind of udp){const value=runtimeFailureFixture({udp:kind,pcm:kind==='pcmWrite'?'unknown':'none'});assert.deepEqual(parse(value),value);}
  for(const kind of pcm){const value=runtimeFailureFixture({udp:kind==='none'?'none':'pcmWrite',pcm:kind});assert.deepEqual(parse(value),value);}
  const unknown=runtimeFailureFixture({runtime:'unknown',udp:'none',pcm:'none'});assert.deepEqual(parse(unknown),unknown);
  assert.equal(parse({...unknown,runtime:'secret-value'}),null);
});
test('首因严拒重复键转义未知枚举额外正文错误类型非合法组合与溢出',()=>{
  const body=JSON.stringify(runtimeFailureFixture()),parse=text=>liveDiagnostics.runtimeFailureCategory(Buffer.from('host_failure='+text));
  for(const invalid of [body.replace('"runtime":','"runtime":"clientTick","runtime":'),
    body.replace('"runtime":','"runt\\u0069me":'),body.replace('"observation":1','"observation":1,"observation":1'),
    body.replace('"kind":"runtimeFailureDiagnostic"','"kind":"other"'),
    body.replace('"runtime":"mediaPoll"','"runtime":"clientTick"'),
    body.replace('"udp":"pcmWrite"','"udp":"replyCrypto"'),
    body.replace('"pcm":"bufferFull"','"pcm":"none"'),
    body.replace('"pcm":"bufferFull"','"pcm":"privateBody"'),
    body.replace('"observation":1','"observation":true'),body.replace('"observation":1','"observation":"1"'),
    body.replace('"observation":1','"observation":1.0'),body.replace('"observation":1','"observation":2'),
    body.replace('"observation":1','"observation":9007199254740992'),body.replace('"observation":1','"observation":-1'),
    body.replace('"observation":1','"observation":1,"text":"privateBody"'),
    body.replace('"udp":"pcmWrite",',''),body+' privateBody','['+body+']',body+' '.repeat(1024)])assert.equal(parse(invalid),null);
  const spaced='{ "pcm" : "unknown", "observation" : 1, "kind" : "runtimeFailureDiagnostic", "runtime" : "mediaPoll", "udp" : "pcmWrite" }';
  assert.deepEqual(parse(spaced),runtimeFailureFixture({pcm:'unknown'}));
  const corrupt=Buffer.concat([Buffer.from('host_failure={"kind":"runtimeFailureDiagnostic",'),Buffer.from([0xc0,0xaf]),Buffer.from(body.slice(1))]);
  assert.equal(liveDiagnostics.runtimeFailureCategory(corrupt),null);
  assert.equal(liveDiagnostics.runtimeFailureCategory(body),null);
});
test('首因按有界分块UTF8整行接纳，超长整行丢弃且关闭后保留原第一份',()=>{
  const value=runtimeFailureFixture(),line=Buffer.from('host_failure='+JSON.stringify(value)+'\r\n');
  for(let split=0;split<=line.length;++split){const observer=liveDiagnostics.createDeviceLifecycleObserver(true);observer.push(Buffer.from('其他本地日志\n'));
    observer.push(line.subarray(0,split));if(split<line.length)assert.equal(observer.facts().runtimeFailure,null);
    observer.push(line.subarray(split));assert.deepEqual(observer.facts().runtimeFailure,value);
    const copy=observer.facts();copy.runtimeFailure.pcm='forged';assert.equal(observer.facts().runtimeFailure.pcm,'bufferFull');
    observer.push(Buffer.from('host_failure='+JSON.stringify(runtimeFailureFixture({pcm:'epochMismatch'}))+'\n'));
    observer.finish();observer.finish();observer.push(Buffer.from('voice_state=0\n'));assert.deepEqual(observer.facts().runtimeFailure,value);
    assert.equal(observer.facts().completeLines,3);
  }
  const observer=liveDiagnostics.createDeviceLifecycleObserver(true);observer.push(Buffer.from('host_failure='+JSON.stringify(value)+' '.repeat(4096)+'\n'));
  assert.equal(observer.facts().runtimeFailure,null);assert.equal(observer.facts().discardedOverlongLines,1);
  const corrupt=Buffer.concat([Buffer.from('host_failure='+JSON.stringify(value)),Buffer.from([0xe4,0xb8]),Buffer.from('\n')]);
  for(const byte of corrupt)observer.push(Buffer.from([byte]));assert.equal(observer.facts().runtimeFailure,null);
  observer.push(line);assert.deepEqual(observer.facts().runtimeFailure,value);
  assert.ok(!JSON.stringify(observer.facts()).includes('其他本地日志'));
});
test('无换行首因只在原stdout结束接纳，二次非法首因不能覆盖原首份',()=>{
  const value=runtimeFailureFixture(),observer=liveDiagnostics.createDeviceLifecycleObserver(true);
  observer.push(Buffer.from('host_failure='+JSON.stringify(value)));assert.equal(observer.facts().runtimeFailure,null);
  observer.finish();assert.deepEqual(observer.facts().runtimeFailure,value);observer.push(Buffer.from('host_failure={"text":"privateBody"}\n'));
  assert.deepEqual(observer.facts().runtimeFailure,value);assert.equal(observer.facts().completeLines,1);
});
test('实际runner默认消息开关无新报告字段，仅字符串1开启且缺首因保持null',async()=>{
  for(const enabled of [undefined,'', '0','true',true,1,'1']){
    const report=await failureLifecycle({stderr:'',messageDiagnosticSwitch:enabled,
      stdoutBefore:'host_failure='+JSON.stringify(runtimeFailureFixture())+'\n'});
    assert.equal(Object.hasOwn(report.diagnostic.deviceLifecycle,'runtimeFailure'),enabled==='1');
    assert.equal(Object.hasOwn(report.diagnostic.deviceLifecycleBeforeCleanup,'runtimeFailure'),enabled==='1');
    assert.equal(report.passed,false);assert.equal(report.diagnostic.failureKind,'browser_command_failed');
  }
  const missing=await failureLifecycle({stderr:'',messageDiagnosticSwitch:'1'});
  assert.equal(missing.diagnostic.deviceLifecycle.runtimeFailure,null);assert.equal(missing.diagnostic.deviceLifecycleBeforeCleanup.runtimeFailure,null);
});

test('实际runner音频首因仅env精确1在原catch单eval读取，原失败阶段及cleanup不覆盖', async () => {
  let originalCommands = null; // 记录原端口调用数量而非新取证产生的业务动作。
  for (const enabled of [undefined, '0', 'true', true, 1, '1']) {
    const commands = [], realm = {document: {getElementById: () => ({disabled: true, textContent: ''})},
      context: {state: 'closed'}, stream: {active: false}, socket: {readyState: 3}, calls: 0}; // 原失败前只读浏览器事实。
    vm.createContext(realm);
    vm.runInContext('globalThis.UrReadAudioFailureDiagnostic=()=>{calls++;return {known:true,cause:"playbackGateConflict"};};', realm);
    const report = await failureLifecycle({stderr: '', closeFails: true, audioFailureSwitch: enabled,
      audioRealm: realm, onCommand: args => commands.push(args)});
    assert.equal(report.phase, 'session_ready');
    assert.equal(report.diagnostic.phase, 'session_ready');
    assert.equal(report.diagnostic.failureKind, 'browser_command_failed');
    assert.equal(report.diagnostic.browserCommand.command, 'wait');
    assert.equal(report.diagnostic.uiFactsCapture, 'before_script_cleanup');
    assert.equal(report.diagnostic.cleanup.browserClosed, false);
    assert.equal(report.diagnostic.childLifecycle.closed, true);
    const reads = commands.filter(args => args.includes('eval') &&
      Buffer.from(args.at(-1), 'base64').toString().startsWith('(function readStartupFacts'));
    assert.equal(reads.length, 1);
    assert.equal(realm.calls, enabled === '1' ? 1 : 0);
    if (enabled === '1') assert.deepEqual(JSON.parse(JSON.stringify(report.diagnostic.uiFacts.audioFailure)),
      {known: true, cause: 'playbackGateConflict'});
    else assert.equal(Object.hasOwn(report.diagnostic.uiFacts, 'audioFailure'), false);
    const counts = commands.map(args => args[args.indexOf('--json') + 1]);
    if (originalCommands === null) originalCommands = counts;
    else assert.deepEqual(counts, originalCommands);
    assert.equal(JSON.stringify(report).includes('synthetic-private'), false);
  }
});

// 实际runner接线也须覆盖：此处端口是离线夹具，不把安装顺序证明称为浏览器实际绘制。
test('文件启动旁证先于activate安装；默认关闭不执行额外eval或报告',async()=>{
  for(const enabled of [false,true]) {
    const commands=[];
    const report=await failureLifecycle({startupDiagnostic:enabled,onCommand:args=>commands.push(args)});
    const install=commands.findIndex(args=>args.includes('eval')&&
      Buffer.from(args.at(-1),'base64').toString('utf8').includes('installFileStartupObservation'));
    const activate=commands.findIndex(args=>args.includes('click')&&args.includes('#activate'));
    assert.ok(activate>=0);
    if(enabled) {
      assert.ok(install>=0&&install<activate);
      assert.deepEqual(JSON.parse(JSON.stringify(report.diagnostic.fileStartup)),
        {available:true,truncated:false,events:[]});
    } else {
      assert.equal(install,-1);
      assert.equal(Object.hasOwn(report.diagnostic,'fileStartup'),false);
    }
    assert.ok(!JSON.stringify(report).includes('synthetic-private'));
  }
});
test('失败报告必须等原子进程退出和stderr排空，不能丢失close前迟到完整行',async()=>{
  const result=await failureLifecycle();
  assert.deepEqual(JSON.parse(JSON.stringify(result.diagnostic.deviceFailure)),{kind:'request_rejected',method:'capabilityReport',code:403,reason:'requestDenied'});
  assert.deepEqual(JSON.parse(JSON.stringify(result.diagnostic.childLifecycle)),{spawned:true,closed:true,exitCode:1,stderrEnded:true,stderrObserved:true,completeLines:1,discardedOverlongLines:0,matchedFailure:true});
  assert.ok(!JSON.stringify(result).includes('synthetic-private'));
});
test('无固定stderr退出只能报告unknown；不从关闭状态或此前403猜原因',async()=>{
  const result=await failureLifecycle({stderr:''});assert.equal(result.diagnostic.deviceFailure,null);
  assert.deepEqual(JSON.parse(JSON.stringify(result.diagnostic.childLifecycle)),{spawned:true,closed:true,exitCode:1,stderrEnded:true,stderrObserved:false,completeLines:0,discardedOverlongLines:0,matchedFailure:false});
});
test('失败时原进程仍活与清理后Idle必须分开保存，固定stdout诊断不替换首失败',async()=>{
  const report=await failureLifecycle({stderr:'',stdoutBefore:'voice_state=4\nprivate-secret\n',stdoutOnCleanup:'voice_state=0\n'});
  const before=report.diagnostic.deviceLifecycleBeforeCleanup,after=report.diagnostic.deviceLifecycle;
  assert.deepEqual(Array.from(before.voiceStates),[4]);assert.equal(before.childClosed,false);assert.equal(before.exitCode,null);
  assert.deepEqual(Array.from(after.voiceStates),[4,0]);assert.equal(after.stdoutEnded,true);
  assert.equal(after.normalHostExitBranch,null);assert.equal(report.diagnostic.failureKind,'browser_command_failed');
  assert.equal(report.diagnostic.childLifecycle.closed,true);assert.ok(!JSON.stringify(report).includes('private-secret'));
});
test('无换行末行等stderr结束后可分类，超长输出仅记有界丢弃事实',async()=>{
  const tail=await failureLifecycle({stderr:'request_rejected method=capabilityReport code=403 reason=focusDeviceUnavailable'});
  assert.equal(tail.diagnostic.deviceFailure.reason,'focusDeviceUnavailable');assert.equal(tail.diagnostic.childLifecycle.completeLines,1);
  const long=await failureLifecycle({stderr:'request_rejected method=capabilityReport code=403 '+ 'x'.repeat(4096)+'\n'});
  assert.equal(long.diagnostic.deviceFailure,null);assert.equal(long.diagnostic.childLifecycle.discardedOverlongLines,1);
});
test('首个wait失败保留固定CLI旁证，catch与close失败不得替换原命令',async()=>{
  const report=await failureLifecycle({stderr:'',browserStdout:'synthetic-private-invalid-json',
    browserStderr:'synthetic-private-stderr',browserCode:'EAGAIN',closeFails:true});
  assert.deepEqual(JSON.parse(JSON.stringify(report.diagnostic.browserCommand)),{
    phase:'session_ready',command:'wait',failureKind:'browser_command_failed',processCode:'EAGAIN',exitCode:null,
    signal:null,stdoutPresent:true,stdoutParsed:false,cliSuccess:null,cliErrorShape:'absent',stderrPresent:true});
  assert.equal(report.diagnostic.cleanup.browserClosed,false);
  assert.ok(!JSON.stringify(report).includes('synthetic-private'));
});
test('CLI仅stderr返回固定等待超时仍分类，但不保存错误正文或原等待表达式',async()=>{
  const report=await failureLifecycle({stderr:'',browserStdout:'',
    browserStderr:'TimeoutError: page.waitForFunction synthetic-private-secret',browserCode:1});
  assert.equal(report.diagnostic.failureKind,'browser_timeout');
  assert.equal(report.diagnostic.browserCommand.command,'wait');assert.equal(report.diagnostic.browserCommand.exitCode,1);
  assert.equal(report.diagnostic.browserCommand.stderrPresent,true);assert.ok(!JSON.stringify(report).includes('synthetic-private'));
});
test('只读页面关闭原因仅匹配原固定文案，不把已关闭布尔推定为失败原因',()=>{
  for(const [text,expected] of [['本地媒体报文非法，已停止采集与播放。','local_media_invalid'],
    ['本机连接已关闭，麦克风已释放。','local_socket_closed'],['synthetic-private-error',null]]){
    const context={document:{getElementById:id=>({disabled:true,textContent:id==='status'?text:''})},
      context:{state:'closed'},stream:{active:false},socket:{readyState:3},globalThis:{}};
    const result=vm.runInNewContext('('+liveDiagnostics.readStartupFacts.toString()+')()',context);
    assert.equal(result.closureReason,expected);assert.ok(!JSON.stringify(result).includes('synthetic-private'));
  }
});

// cleanupClock 只驱动离线端口定时器；先收敛原微任务，再推进虚拟截止，避免CPU负载改变两个串行等待的先后。
function cleanupClock() {
  const timers=new Map(),expired=[];let now=0,sequence=0,turns=0;
  // schedule 保留原期限及创建顺序；unref只兼容原API，不创建任何墙钟计时器。
  const schedule=(callback,milliseconds)=>{
    const timer={callback,deadline:now+milliseconds,sequence:sequence++,unref(){return this;}};
    timers.set(timer,timer);return timer;
  };
  // cancel 仅移除本夹具仍待执行的句柄，保持原clearTimeout幂等语义。
  const cancel=timer=>timers.delete(timer);
  // settle 最多八次事件循环让步；未完成的原Promise仍为still_waiting，不伪造close或业务完成。
  const settle=async invocation=>{
    let outcome='still_waiting';
    invocation.then(()=>{outcome='resolved';},()=>{outcome='rejected';});
    for(turns=0;turns<8;) {
      ++turns;await new Promise(resolve=>setImmediate(resolve));
      if(outcome!=='still_waiting')break;
      const next=[...timers.values()].sort((a,b)=>a.deadline-b.deadline || a.sequence-b.sequence)[0];
      if(next) {timers.delete(next);now=next.deadline;expired.push(now);next.callback();}
    }
    return outcome;
  };
  return {schedule,cancel,settle,facts:()=>({expired:[...expired],now,turns})};
}
test('离线收尾调度器在前段微任务收敛后才创建后段截止，不依赖墙钟watchdog',async()=>{
  const clock=cleanupClock();let completed=false;
  const invocation=(async()=>{
    await new Promise(resolve=>clock.schedule(resolve,5000));
    await Promise.resolve();
    await new Promise(resolve=>clock.schedule(resolve,5000));
    completed=true;
  })();
  assert.equal(await clock.settle(invocation),'resolved');assert.equal(completed,true);
  assert.deepEqual(clock.facts().expired,[5000,10000]);
});
test('离线收尾调度器对永不完成且无截止的Promise最多八轮返回未知',async()=>{
  const clock=cleanupClock();assert.equal(await clock.settle(new Promise(()=>{})),'still_waiting');
  assert.deepEqual(clock.facts(),{expired:[],now:0,turns:8});
});
// checkoutFixtureFs 只模拟确切固定私有语料文件缺失，TEMP里的正式源码仍交原读取端口。
// externalCorpusUnavailable为缺失意图，originalFs为原文件端口；返回独立包装，不改共享fs。
function checkoutFixtureFs(externalCorpusUnavailable, originalFs) {
  const path=require('node:path');
  const privateCorpusFile=path.resolve(__dirname,'../../../.temp/task143-ipc-env/mcp-context-fixed-corpus.cjs'); // 原固定resolver文件。
  return {...originalFs,readFileSync:(filename,...args)=>{
    if(externalCorpusUnavailable&&path.resolve(String(filename))===privateCorpusFile)
      throw Error('external_fixture_unavailable');
    return originalFs.readFileSync(filename,...args);
  }};
}
test('私有语料缺失只拒确切文件，TEMP中正式runner源码仍原样加载',()=>{
  const originalFs=require('node:fs'),path=require('node:path');
  const source=path.join(__dirname,'mcp_file_media_live.cjs'); // 正式runner可位于本次TEMP复制树。
  const temporarySource=path.join(__dirname,'.temp/source-copy/mcp_file_media_live.cjs'); // 正式checkout也固定覆盖TEMP路径边界。
  const privateCorpusFile=path.resolve(__dirname,'../../../.temp/task143-ipc-env/mcp-context-fixed-corpus.cjs'); // 缺失模拟唯一目标。
  const reads=[]; // 核私有读取在原文件端口之前拒绝，不访问真实私有字节。
  const observed={...originalFs,readFileSync:(filename,...args)=>{
    reads.push(String(filename)); // 只把合成TEMP副本路径映射回同一真实正式源码，不读取私有内容。
    return originalFs.readFileSync(filename===temporarySource?source:filename,...args);
  }};
  const port=checkoutFixtureFs(true,observed); // 缺少私有语料不等于缺少正式源。
  assert.equal(port.readFileSync(source,'utf8'),originalFs.readFileSync(source,'utf8'));
  assert.equal(port.readFileSync(temporarySource,'utf8'),originalFs.readFileSync(source,'utf8'));
  assert.deepEqual(reads,[source,temporarySource]);
  assert.throws(()=>port.readFileSync(privateCorpusFile,'utf8'),/^Error: external_fixture_unavailable$/);
  assert.deepEqual(reads,[source,temporarySource]);
});
// cleanupLifecycle 执行原run及全部21步验收，仅以可控进程/浏览器端口复现收尾，不启动真实服务。
async function cleanupLifecycle({browserCloseFails=false, neverCloses=false, stderrNeverEnds=false,
  lateDuringReply=null, lateDuringNetwork=null, chain='cascaded_agentruntime', withRecovery=false,
  readOnlyStatus=null, contextInputDiagnostic=false, contextDiagnosticAttempt='25', externalCorpusUnavailable=false,
  missingDiagnosticResolver=false, failStepWait=null, skipRoundClosures=0, replyDiagnostic=false}={}) {
  const {EventEmitter}=require('node:events'),originalFs=require('node:fs'),path=require('node:path');
  const fs=checkoutFixtureFs(externalCorpusUnavailable,originalFs); // 只缺原固定语料，不拒正式run源码。
  const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();
  const output=[],signals=[],events=[];let current={...observation(),file:null},index=0;
  const round=roundFixture(); // 双链均真实安装观察器；串联链只旁读诊断，网络终态等待表达式仍仅全双工发出。
  const fixturePlan=plan(chain); // 固定21步语料仅在离线端口使用，不读真实文件或模型配置。
  // 仅25/26使用合成语料的旁路端口；真实固定语料和密钥不属于默认离线测试依赖。
  const inputRealm={incoming:value=>{events.push('original_input');return value;}};
  // fixtureExpected仅核本夹具的文件与关键词原值；不是正式语料SHA、权限或现场ASR证明。
  const fixtureExpected=input=>{
    const rows=[fixturePlan.initial,...fixturePlan.programs.flatMap(program=>Object.values(program.inputs))];
    const matched=rows.filter(row=>row.wav===input.wav);
    assert.equal(matched.length,1);assert.equal(input.keywords.length,matched[0].keywords.length);
    assert.ok(input.keywords.every((word,index)=>word===matched[0].keywords[index]));
    return matched[0].keywords.join(' ');
  };
  const ordered=[['initial',null],...['music','video'].flatMap(kind=>steps.map(step=>[step,kind]))];
  const close=()=>{if(!stderrNeverEnds)child.stderr.emit('end');events.push('child_closed');child.emit('close',0);};
  child.kill=signal=>{signals.push(signal);if(!neverCloses)queueMicrotask(close);return true;};
  const execFile=(_binary,args,_options,callback)=>queueMicrotask(()=>{
    if(args.includes('close')) {events.push('browser_close');return callback(browserCloseFails?Error('synthetic-private'):null,JSON.stringify({success:!browserCloseFails,data:{}}));}
    if(neverCloses && args.includes('wait'))return callback(Error('synthetic-private'),'{}');
    if(args.includes('wait') && args.at(-1).includes('formal_tool_missing_or_repeated')) {
      if(index===1&&failStepWait==='toolDevice')return callback(Error('synthetic-private'),'{}');
      const [step,kind]=ordered[index++],before=current,ordinal=index-1;
      const overrides={};
      if(['initial','restoreAi'].includes(step))overrides.consumedNonSilent=before.consumedNonSilent+1;
      if(step==='load')overrides.file={...observation().file,kind,state:'loaded'};
      if(step==='play')overrides.file={...before.file,state:'playing',paused:false};
      if(step==='pause')Object.assign(overrides,{file:{...before.file,state:'paused',paused:true},overlap:before.overlap+1,captured:before.captured+1});
      if(['acquireFile','suspendFile'].includes(step))overrides.gates=[...before.gates,{lane:kind==='video'?'audioVideo':'audio',enabled:step==='acquireFile'}];
      if(step==='stop')overrides.file=null;
      current=completed(step,before,overrides);
      if(contextInputDiagnostic){
        const input=step==='initial'?fixturePlan.initial:fixturePlan.programs.find(row=>row.kind===kind).inputs[step];
        inputRealm.incoming({kind:'text',text:'\n用户：'+fixtureExpected(input)+'\n助手：'});
      }
      if(withRecovery) current.tools.push({name:'things_iot_mcp.recover_media_playback_operation',state:'unknown'});
      // 查询在本轮动作之前，但不能插到历史工具之前，否则会伪造原before水位后的终态集合。
      if(readOnlyStatus) current.tools.splice(before.tools.length,0,
        {name:'things_iot_mcp.'+readOnlyStatus,state:'succeeded'});
      // skipRoundClosures模拟串联链模型未按关键词调用/识别的失败轮；这些轮不闭环，下一轮arm按真实观察器拒绝。
      if(round && ordinal>=skipRoundClosures) {
        const words=step==='initial'?fixturePlan.initial.keywords:fixturePlan.programs.find(value=>value.kind===kind).inputs[step].keywords;
        round.emit({kind:'text',text:'\n用户：'+words.join(' ')+'\n助手：',responseId:'private-round-'+index});
        round.tool('started','private-round-'+index);round.tool('succeeded','private-round-'+index);
        if(withRecovery || readOnlyStatus) {
          round.tool('started','private-round-'+index,'private-recovery');
          round.tool(withRecovery?'unknown':'succeeded','private-round-'+index,'private-recovery');
        }
      }
    }
    if(args.includes('wait') && (args.at(-1).startsWith('metrics.completedResponses >') || args.at(-1).startsWith('(()=>{const passed=(metrics.completedResponses >') || args.at(-1).startsWith('(function evaluateReplyWaitDiagnostic('))) {
      if(index===2&&failStepWait==='replyDrain')return callback(Error('synthetic-private'),'{}');
      events.push('reply_drained:' + index); // 模拟原终态及实际排空期间到达的事实，不启动真实计时或媒体。
      // 串联链的回复网络终态在排空等待期到达观察器；全双工终态由专用networkRound等待单独投递。
      if(chain!=='realtime_native' && index-1>=skipRoundClosures) round.emit({kind:'responseCompleted',id:'private-round-'+index});
      if(index===2 && lateDuringReply==='extra_tool')
        current.tools.push({name:'things_iot_mcp.control_media_playback',state:'succeeded'});
      if(index===2 && lateDuringReply==='load') current.file={...observation().file,kind:'music',state:'loaded',
        generation:'1',busy:false};
      if(lateDuringReply==='capture_progress') current.captured+=1;
    }
    if(args.includes('wait') && args.at(-1)==='__urMcpRound.facts().roundTerminal === true') {
      if(index===2&&failStepWait==='networkRound')return callback(Error('synthetic-private'),'{}');
      events.push('network_terminal:' + index); // 原当前回复终态不清空播放队列，也不发采集停止。
      if(index===2 && lateDuringNetwork==='extra_tool') {
        current.tools.push({name:'things_iot_mcp.control_media_playback',state:'succeeded'});
        round.tool('started','private-round-'+index,'private-extra');round.tool('succeeded','private-round-'+index,'private-extra');
      }
      if(index===2 && lateDuringNetwork==='load') current.file={...observation().file,kind:'music',state:'loaded',generation:'1',busy:false};
      ++current.captured;round.emit({kind:'responseCompleted',id:'private-round-'+index});
      assert.equal(round.facts().roundTerminal,true);
    }
    if(args.includes('click') && args.at(-1)==='#end')close();
    let result=true;
    if(args.includes('eval')) {
      const expression=Buffer.from(args.at(-1),'base64').toString();
      if(expression.startsWith('(function installRoundObserver'))events.push('round_observer:installed');
      if(contextInputDiagnostic&&(expression.startsWith('(function installInputDiagnostic')||expression.startsWith('__urMcpInputDiagnostic.')||expression.startsWith('__urMcpInputDiagnostic?.')))
        result=vm.runInNewContext(expression,inputRealm);
      if(expression.startsWith('(function readSnapshot')) {
        events.push('snapshot:' + index);result=current;
      }
      if(expression.startsWith('(function readStartupFacts'))result=null;
      if(expression.startsWith('(function readReplyDiagnostic'))result=null; // 原宿主未投影计时时必须未知，不借Node时间补数。
      if(expression.startsWith('(function readReplyDrainFacts')) {
        events.push('reply_facts:' + index);
        result={known:true,completedResponses:index-1,responseActive:true,pendingPlaybackFrames:0};
      }
      if(expression === 'metrics.completedResponses')result=index; // 收尾夹具明确给出原逐轮终态计数，不把默认布尔true伪作水位。
      if(expression.startsWith('__urMcpRound.arm(')) result=round.realm.__urMcpRound.arm(JSON.parse(expression.slice(17,-1)));
      // 串联链容错arm与round旁读按真实浏览器realm执行：观察器闭包在round.realm内，故障事实也留在同一闭包。
      if(expression.startsWith('(()=>{try{__urMcpRound.arm(')) result=vm.runInContext(expression,round.realm);
      if(expression.startsWith('(()=>{try{return{facts:globalThis.__urMcpRound')) result=vm.runInContext(expression,round.realm);
      if(expression==='__urMcpRound.facts()' || expression==='globalThis.__urMcpRound?.facts() || null') result=round?.facts() || null;
    }
    callback(null,JSON.stringify({success:true,data:{result}}));
  });
  const module={exports:{}};
  // requirePort只注入正式resolver的外部端口，不读真实TEMP文件或另造宽松的真实映射。
  const requirePort=name=>{
    if(name==='../../../.temp/task143-ipc-env/mcp-context-fixed-corpus.cjs') {
      events.push('resolver_requested');
      if(missingDiagnosticResolver)throw Error('fixed_resolver_unavailable');
      return {resolve:fixtureExpected};
    }
    if(name==='node:child_process')return {spawn:()=>{queueMicrotask(()=>{child.emit('spawn');child.stdout.emit('data',Buffer.from('http://127.0.0.1:39741/'));});return child;},execFile};
    if(name==='node:fs')return {statSync:()=>({isFile:()=>true}),readFileSync:filename=>filename==='/synthetic/plan'?Buffer.from(JSON.stringify(fixturePlan)):Buffer.alloc(100)};
    return require(name);
  };
  const clock=cleanupClock(); // 原期限仅由离线调度器推进；截止与kill受理仍不能证明实际close。
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'mcp_file_media_live.cjs'),'utf8'),{module,exports:module.exports,require:requirePort,Buffer,
    console:{log:line=>{const value=JSON.parse(line);output.push(value);events.push(value.passed?'pass':'fail');},error(){}},performance:require('node:perf_hooks').performance,setTimeout:clock.schedule,clearTimeout:clock.cancel});
  const invocation=module.exports.run({...(replyDiagnostic?{UR_MCP_STARTUP_DIAGNOSTIC:'1'}:{}),...(contextInputDiagnostic?{UR_MCP_ATTEMPT:contextDiagnosticAttempt}:{}),UR_MCP_FILE_LIVE:'1',UR_OUTPUT_FOCUS:'media',UR_FILE_CATALOG:'synthetic',UR_DEVICE_PRODUCT:'synthetic',UR_DEVICE_NAME:'synthetic',UR_DEVICE_SECRET:'synthetic-private',UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:'1883',UR_DEVICE_BROWSER_BIN:'/synthetic/device',UR_MCP_FILE_PLAN:'/synthetic/plan',UR_BROWSER_PORT:'39741'});
  const outcome=await clock.settle(invocation);
  return {outcome,output,signals,events,clock:clock.facts()};
}
test('全部媒体通过仍须浏览器和原进程真实收尾后才能输出唯一PASS',async()=>{
  const value=await cleanupLifecycle();assert.equal(value.outcome,'resolved');assert.equal(value.output.length,1);
  assert.equal(value.output[0].passed,true);assert.equal(value.output[0].results.length,21);
  assert.ok(value.events.indexOf('pass')>value.events.indexOf('browser_close'));
  assert.ok(value.events.indexOf('pass')>value.events.indexOf('child_closed'));
  assert.deepEqual(value.clock.expired,[]); // 原close已发生时不能依靠推进任何截止来成功。
});
test('正式21步显式诊断逐轮保存unknown且缺省关闭不新增媒体事实',async()=>{
  const enabled=await cleanupLifecycle({replyDiagnostic:true});assert.equal(enabled.outcome,'resolved');
  assert.equal(enabled.output[0].results.length,21);
  for(const row of enabled.output[0].results) {
    assert.equal(row.replyDiagnostic.completed.known,false);assert.equal(row.replyDiagnostic.completed.timing.primaryMeasured,false);
  }
  const disabled=await cleanupLifecycle();assert.ok(disabled.output[0].results.every(row=>!Object.hasOwn(row,'replyDiagnostic')));
});
test('正式排空失败保留本轮开始与末样本和清理前unknown，收尾事实不回填',async()=>{
  const value=await cleanupLifecycle({replyDiagnostic:true,failStepWait:'replyDrain'});
  assert.equal(value.outcome,'rejected');assert.equal(value.output[0].diagnostic.stepWait,'replyDrain');
  const diagnostic=value.output[0].diagnostic.replyDiagnostic;
  assert.equal(diagnostic.begin.known,false);assert.equal(diagnostic.waitSample.known,false);
  assert.equal(diagnostic.beforeCleanup.known,false);assert.equal(value.output[0].diagnostic.cleanup.childClosed,true);
});
test('轮次观察器双链均安装一次，串联链不再省略旁证安装',async()=>{
  for(const chain of ['cascaded_agentruntime','realtime_native']) {
    const value=await cleanupLifecycle({chain});
    assert.equal(value.outcome,'resolved',chain);
    assert.equal(value.events.filter(event=>event==='round_observer:installed').length,1,chain);
  }
});
test('串联链失败轮快照round保留上一轮三闭环残态与arm拒绝事实，仅供诊断不参与判据',async()=>{
  const value=await cleanupLifecycle({failStepWait:'toolDevice',skipRoundClosures:1});
  assert.equal(value.outcome,'rejected');assert.equal(value.output[0].phase,'music_invite');
  const round=value.output[0].observed.round;
  assert.deepEqual(Object.keys(round).sort(),['armFault','facts']);
  assert.equal(round.armFault,'invalid_round_arm'); // initial轮未闭环，music_invite轮容错arm按原观察器拒绝。
  assert.equal(round.facts.armed,true);
  assert.equal(round.facts.armIndex,1);
  assert.equal(round.facts.roundTerminal,false);
  assert.equal(round.facts.responseBound,false);
  assert.ok(!JSON.stringify(value.output).includes('private-round')); // 原回复与调用ID不进入报告。
});
test('全双工失败轮round保持原裸facts合同，判据核验不因双链安装改变',async()=>{
  const value=await cleanupLifecycle({chain:'realtime_native',failStepWait:'networkRound'});
  assert.equal(value.outcome,'rejected');assert.equal(value.output[0].phase,'music_invite');
  const round=value.output[0].observed.round;
  assert.equal(Object.hasOwn(round,'armFault'),false); // 全双工快照仍直接是facts，verifyRoundFacts合同不变。
  assert.equal(round.armed,true);assert.equal(round.roundTerminal,false);
});
test('默认离线诊断合同在私有语料不存在的干净checkout仍验证完整21步',async()=>{
 const value=await cleanupLifecycle({contextInputDiagnostic:true,externalCorpusUnavailable:true});
 assert.equal(value.outcome,'resolved');assert.equal(value.output[0].results.length,21);
 for(const row of value.output[0].results)assert.equal(row.inputDiagnostic.normalizedExactMatch,true);
});
test('显式诊断缺少固定resolver必须失败，而默认不加载该外部端口',async()=>{
 const absent=await cleanupLifecycle({contextInputDiagnostic:true,missingDiagnosticResolver:true,externalCorpusUnavailable:true});
 assert.equal(absent.outcome,'rejected');assert.equal(absent.output[0].passed,false);
 assert.equal(absent.output[0].phase,'ai_initial');assert.equal(absent.events.filter(v=>v==='resolver_requested').length,1);
 assert.equal(absent.events.filter(v=>v==='original_input').length,0); // 缺resolver不能退化为未校验输入。
 const normal=await cleanupLifecycle({missingDiagnosticResolver:true,externalCorpusUnavailable:true});
 assert.equal(normal.outcome,'resolved');assert.equal(normal.events.filter(v=>v==='resolver_requested').length,0);
});
test('25实际run合成oracle端口只观察21句且释放，原额外工具仍失败',async()=>{
 const value=await cleanupLifecycle({contextInputDiagnostic:true});
 assert.equal(value.outcome,'resolved',value.output[0]?.phase);assert.equal(value.output[0].results.length,21);
 assert.equal(value.events.filter(event=>event==='original_input').length,21);
 for(const result of value.output[0].results){assert.equal(result.inputDiagnostic.known,true);assert.equal(result.inputDiagnostic.normalizedExactMatch,true);}
 assert.ok(!JSON.stringify(value.output).includes('请只邀请'));
 const failed=await cleanupLifecycle({contextInputDiagnostic:true,lateDuringReply:'extra_tool'});
 assert.equal(failed.outcome,'rejected');assert.equal(failed.output[0].phase,'music_invite');
 assert.equal(failed.output[0].diagnostic.inputDiagnostic.normalizedExactMatch,true);
});
test('26实际run显式合成oracle端口覆盖21句，旁证不放宽额外工具门槛',async()=>{
 const value=await cleanupLifecycle({contextInputDiagnostic:true,contextDiagnosticAttempt:'26'});
 assert.equal(value.outcome,'resolved');assert.equal(value.output[0].results.length,21);
 for(const result of value.output[0].results){assert.equal(result.inputDiagnostic?.known,true);assert.equal(result.inputDiagnostic.normalizedExactMatch,true);}
 assert.ok(!JSON.stringify(value.output).includes('请只邀请'));
 const failed=await cleanupLifecycle({contextInputDiagnostic:true,contextDiagnosticAttempt:'26',lateDuringReply:'extra_tool'});
 assert.equal(failed.outcome,'rejected');assert.equal(failed.output[0].phase,'music_invite');
 assert.equal(failed.output[0].diagnostic.inputDiagnostic?.normalizedExactMatch,true);
});
test('原三次等待失败分别保留toolDevice、replyDrain与networkRound固定子段',async()=>{
 for(const [failStepWait,chain] of [['toolDevice','cascaded_agentruntime'],['replyDrain','cascaded_agentruntime'],['networkRound','realtime_native']]) {
  const value=await cleanupLifecycle({failStepWait,chain});
  assert.equal(value.outcome,'rejected');assert.equal(value.output[0].phase,'music_invite');
  assert.equal(value.output[0].diagnostic.stepWait,failStepWait);
  const observedIndex = failStepWait === 'toolDevice' ? 1 : 2; // 首个等待拒绝时原夹具尚未推进工具/回复水位。
  assert.deepEqual(value.output[0].diagnostic.replyDrain,
    {known:true,completedResponses:observedIndex-1,responseActive:true,pendingPlaybackFrames:0});
  assert.ok(value.events.indexOf('reply_facts:' + observedIndex) < value.events.indexOf('browser_close'));
  assert.equal(value.output[0].results.length,1);assert.ok(!JSON.stringify(value.output).includes('synthetic-private'));
 }
});
test('原语料oracle不得泛化到未知轮次或非精确字符串25与26',async()=>{
 for(const attempt of ['24','27','025','026','',25,26]){
  const value=await cleanupLifecycle({contextInputDiagnostic:true,contextDiagnosticAttempt:attempt});
  assert.equal(value.outcome,'resolved');assert.ok(value.output[0].results.every(result=>!Object.hasOwn(result,'inputDiagnostic')));
 }
});
test('半双工整轮结束前晚到额外工具不能被早期合法快照掩盖',async()=>{
  const value=await cleanupLifecycle({lateDuringReply:'extra_tool'}); // 首次邀请快照合法，额外load工具仅在排空期间到达。
  assert.equal(value.outcome,'rejected');assert.equal(value.output.length,1);
  assert.equal(value.output[0].passed,false);assert.equal(value.output[0].phase,'music_invite');
  assert.equal(value.output[0].results.length,1);
  assert.ok(value.events.indexOf('reply_drained:2')>value.events.indexOf('snapshot:2'));
});
test('半双工回复排空期间隐式加载须复验实际状态，不能记录早期邀请PASS',async()=>{
  const value=await cleanupLifecycle({lateDuringReply:'load'}); // 不伪造额外工具；真实播放器事实独立否定未加载基线。
  assert.equal(value.outcome,'rejected');assert.equal(value.output.length,1);
  assert.equal(value.output[0].passed,false);assert.equal(value.output[0].phase,'music_invite');
  assert.equal(value.output[0].results.length,1);
});
test('半双工结果使用原终态排空后的最终计数，不复用早期snapshot',async()=>{
  const value=await cleanupLifecycle({lateDuringReply:'capture_progress'}); // 原采集事实每轮排空期间再增长一次。
  assert.equal(value.outcome,'resolved');assert.equal(value.output[0].passed,true);
  assert.equal(value.output[0].results[0].captured,11);
  assert.equal(value.output[0].results.length,21);
});
test('全双工21步最终快照取本轮网络终态，持续采集且不等待原播放排空',async()=>{
  const value=await cleanupLifecycle({chain:'realtime_native'}); // 每輪可信ASR/工具及same-id终态走真实只读观察器。
  assert.equal(value.outcome,'resolved');assert.equal(value.output[0].passed,true);
  assert.equal(value.output[0].results.length,21);assert.equal(value.output[0].results[0].captured,11);
  assert.equal(value.events.filter(event=>event.startsWith('network_terminal:')).length,21);
  assert.equal(value.events.filter(event=>event.startsWith('reply_drained:')).length,0);
  assert.ok(!JSON.stringify(value.output).includes('private-round'));
});
test('两链21步执行路径以已验证业务类别回报双工具，不放宽终态或重复输入',async()=>{
  for(const chain of ['cascaded_agentruntime','realtime_native']) {
    const value=await cleanupLifecycle({chain,withRecovery:true}); // 原run、verifyStep和fullD网络观察全部真实执行。
    assert.equal(value.outcome,'resolved');assert.equal(value.output[0].passed,true);
    assert.equal(value.output[0].results.length,21);
    for(const row of value.output[0].results) {
      assert.equal(row.toolsReturned,2);assert.equal(row.businessActionCount,1);
      assert.equal(row.readOnlyQueryCount,1);assert.equal(row.readOnlyRecoveryCount,1);
    }
    assert.equal(value.events.filter(event=>event.startsWith('network_terminal:')).length,
      chain==='realtime_native'?21:0);
    assert.equal(value.events.filter(event=>event.startsWith('reply_drained:')).length,
      chain==='cascaded_agentruntime'?21:0);
  }
});
test('两链21步安全状态查询单独计数，不冒充恢复或本轮业务控制',async()=>{
  for(const chain of ['cascaded_agentruntime','realtime_native'])
    for(const readOnlyStatus of ['get_output_focus_status','get_media_playback_status']) {
      const value=await cleanupLifecycle({chain,readOnlyStatus}); // 执行原入口、严格设备判据及精确本轮终态。
      assert.equal(value.outcome,'resolved');assert.equal(value.output[0].passed,true);
      assert.equal(value.output[0].results.length,21);
      for(const row of value.output[0].results) {
        assert.equal(row.toolsReturned,2);assert.equal(row.businessActionCount,1);
        assert.equal(row.readOnlyQueryCount,1);assert.equal(row.readOnlyRecoveryCount,0);
      }
    }
});
test('全双工早期合法邀请不能掩盖网络终态前晚到工具或实际load',async()=>{
  for(const lateDuringNetwork of ['extra_tool','load']) {
    const value=await cleanupLifecycle({chain:'realtime_native',lateDuringNetwork}); // 固定一次语料，没有重试或换权限。
    assert.equal(value.outcome,'rejected');assert.equal(value.output[0].passed,false);
    assert.equal(value.output[0].phase,'music_invite');assert.equal(value.output[0].results.length,1);
  }
});
test('全部媒体已通过但浏览器关闭失败必须总体失败且不泄露自由错误',async()=>{
  const value=await cleanupLifecycle({browserCloseFails:true});assert.equal(value.outcome,'rejected');
  assert.equal(value.output.length,1);assert.equal(value.output[0].passed,false);
  assert.equal(value.output[0].diagnostic.cleanup.browserClosed,false);
  assert.ok(!JSON.stringify(value.output).includes('synthetic-private'));
});
test('TERM和KILL被受理但无实际close必须有界失败，不等待永不完成的done',async()=>{
  const value=await cleanupLifecycle({neverCloses:true});assert.equal(value.outcome,'rejected');
  assert.deepEqual(value.signals,['SIGTERM','SIGKILL']);assert.equal(value.output.length,1);
  assert.equal(value.output[0].passed,false);assert.equal(value.output[0].diagnostic.cleanup.childClosed,false);
  assert.deepEqual(value.clock.expired,[5000,10000]); // 两段原5000ms分别耗尽；不因第二段创建较晚而丢失失败报告。
});
test('进程close没有stderr排空证据不能当完整收尾成功',async()=>{
  const value=await cleanupLifecycle({stderrNeverEnds:true});assert.equal(value.outcome,'rejected');
  assert.equal(value.output.length,1);assert.equal(value.output[0].passed,false);
  assert.equal(value.output[0].diagnostic.cleanup.stderrEnded,false);
});

// plan 生成有限录音路径夹具，不读取文件或启动浏览器/模型。
function plan(chain = 'realtime_native') {
  return {chain, initial: {wav: '/fixture/initial.wav', keywords: ['输出']},
    programs: ['music', 'video'].map(kind => ({kind, contentRef: 'fixture-' + kind,
      inputs: Object.fromEntries(steps.map(step => [step,
        {wav: `/fixture/${kind}-${step}.wav`, keywords: [step === 'pause' ? '暂停' : kind]}]))}))};
}
// observation 表示验收器已读取的有限事实；不以此夹具冒充真实媒体播放。
function observation() {
  return {tools: [], gates: [{lane: 'audioVideo', enabled: true}], overlap: 2, captured: 10, recognition: true, captureOpen: true,
    focusReceiptIndex: 1, focusReceipts: [{operation: 'acquire', state: 'acquired', code: 200,
      revision: 3, receiptIndex: 1}],
    overflow: false, consumedNonSilent: 4, mode: 'fullDuplex', contextState: 'running',
    streamActive: true, file: {state: 'playing', samples: 512, energy: 123, frames: 2,
      outputEnabled: true, paused: false, pixels: true, kind: 'video'}};
}
// completed 添加正式工具返回展示，媒体事实仍需另外验证。
function completed(step, before, overrides = {}) {
  const operation = {initial: 'acquire', suspendAi: 'suspend', acquireFile: 'acquire',
    suspendFile: 'suspend', restoreAi: 'restore', releaseFile: 'release'}[step];
  const focus = operation ? {focusReceiptIndex: before.focusReceiptIndex + 1,
    focusReceipts: [...before.focusReceipts, {operation,
      state: {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'}[operation],
      code: 200, revision: Math.max(0, ...before.focusReceipts.map(value => value.revision)) + 1,
      receiptIndex: before.focusReceiptIndex + 1}]} : {};
  return {...structuredClone(before), ...overrides,
    ...focus,
    tools: [...before.tools, {name: 'things_iot_mcp.' + tools[step], state: 'succeeded'}]};
}

test('计划要求音乐和带音轨视频的完整分轮顺序，不允许权限或协议字段', () => {
  assert.equal(validatePlan(plan()).chain, 'realtime_native');
  for (const change of [value => { value.version = 2; },
    value => { value.parentSessionId = 'forged'; }, value => { value.initial.epoch = 1; },
    value => { delete value.programs[1].inputs.releaseFile; },
    value => { value.programs.reverse(); }, value => { value.programs[1].contentRef = 'https://source/file.mp4'; },
    value => { value.programs[1].contentRef = value.programs[0].contentRef; }]) {
    const invalid = plan(); change(invalid);
    assert.throws(() => validatePlan(invalid));
  }
});

test('实时用例拒绝文字替代语音，暂停阶段两条链都必须用实际语音', () => {
  const native = plan(); native.initial = {text: '取得焦点', keywords: ['焦点']};
  assert.throws(() => validatePlan(native));
  const cascaded = plan('cascaded_agentruntime');
  cascaded.initial = {text: '取得焦点', keywords: ['焦点']};
  assert.equal(validatePlan(cascaded).chain, 'cascaded_agentruntime');
  cascaded.programs[0].inputs.pause = {text: '暂停', keywords: ['暂停']};
  assert.throws(() => validatePlan(cascaded));
});

test('工具返回成功或playing字符串不能代替实际非静音音频及视频像素', () => {
  const before = observation();
  assert.doesNotThrow(() => verifyStep('play', 'video', before, completed('play', before), 'realtime_native'));
  for (const change of [file => { file.samples = 0; }, file => { file.energy = 0; },
    file => { file.frames = 0; }, file => { file.pixels = false; },
    file => { file.paused = true; }, file => { file.kind = 'music'; }]) {
    const after = completed('play', before); change(after.file);
    assert.throws(() => verifyStep('play', 'video', before, after, 'realtime_native'));
  }
});

test('播放期间采集计数、云端识别和独立输入开启须同时成立', () => {
  const before = observation();
  const after = completed('pause', before, {overlap: 3, captured: 12,
    file: {...before.file, state: 'paused', paused: true}});
  assert.doesNotThrow(() => verifyStep('pause', 'video', before, after, 'realtime_native'));
  for (const [key, value] of [['overlap', 2], ['captured', 10], ['recognition', false],
    ['captureOpen', false], ['mode', 'halfDuplex'], ['contextState', 'suspended'], ['streamActive', false]]) {
    assert.throws(() => verifyStep('pause', 'video', before, {...after, [key]: value}, 'realtime_native'));
  }
});

test('带音轨视频必须实际关闭或取得完整audioVideo路径，不能只切音频', () => {
  const before = observation();
  for (const step of ['acquireFile', 'suspendFile']) {
    const after = completed(step, before, {gates: [...before.gates,
      {lane: 'audioVideo', enabled: step === 'acquireFile'}]});
    assert.doesNotThrow(() => verifyStep(step, 'video', before, after, 'realtime_native'));
    after.gates.at(-1).lane = 'audio';
    assert.throws(() => verifyStep(step, 'video', before, after, 'realtime_native'));
  }
});

test('未知结果不接受为成功，显式恢复要求新增AI有声消费，停止要求实际关闭', () => {
  const before = observation();
  const unknown = completed('load', before, {file: {...before.file, state: 'loaded'}});
  unknown.tools[0].state = 'unknown';
  assert.throws(() => verifyStep('load', 'video', before, unknown, 'realtime_native'));
  assert.throws(() => verifyStep('restoreAi', 'video', before, completed('restoreAi', before), 'realtime_native'));
  assert.doesNotThrow(() => verifyStep('restoreAi', 'video', before,
    completed('restoreAi', before, {consumedNonSilent: 5}), 'realtime_native'));
  assert.throws(() => verifyStep('stop', 'video', before, completed('stop', before), 'realtime_native'));
  assert.doesNotThrow(() => verifyStep('stop', 'video', before,
    completed('stop', before, {file: null}), 'realtime_native'));
});

test('同阶段重复副作用工具或非有限音频事实不能算通过', () => {
  const before = observation();
  const repeated = completed('play', before);
  repeated.tools.push({...repeated.tools[0]});
  assert.throws(() => verifyStep('play', 'video', before, repeated, 'realtime_native'));
  const invalid = completed('play', before);
  invalid.file.energy = Infinity;
  assert.throws(() => verifyStep('play', 'video', before, invalid, 'realtime_native'));
});

test('文件21步均要求本轮唯一成功业务动作，可旁观一次只读恢复且历史终态不重复计入', () => {
  for (const [step, kind] of [['initial', null], ...['music', 'video'].flatMap(kind =>
    steps.map(step => [step, kind]))]) {
    const before = observation(); // 同一父会话可能保留前轮合法终态，只截取新增结果。
    before.tools = [{name: 'things_iot_mcp.get_output_focus_status', state: 'succeeded'}];
    before.file.kind = kind || 'music';
    if (step === 'invite') before.file = null;
    const overrides = {}; // 独立构造每轮现有设备判据，不以工具文字替代媒体事实。
    if (['initial', 'restoreAi'].includes(step)) overrides.consumedNonSilent = before.consumedNonSilent + 1;
    if (step === 'load') overrides.file = {...before.file, state: 'loaded'};
    if (step === 'pause') Object.assign(overrides, {file: {...before.file, state: 'paused', paused: true},
      overlap: before.overlap + 1, captured: before.captured + 1});
    if (['acquireFile', 'suspendFile', 'releaseFile'].includes(step)) overrides.gates = [...before.gates,
      {lane: kind === 'video' ? 'audioVideo' : 'audio', enabled: step === 'acquireFile'}];
    if (step === 'play' && kind === 'music') overrides.gates = [...before.gates, {lane: 'audio', enabled: true}];
    if (step === 'stop') overrides.file = null;
    const after = completed(step, before, overrides); // 每轮先证明原唯一意图和设备事实可通过。
    assert.doesNotThrow(() => verifyStep(step, kind, before, after, 'realtime_native'));
    after.tools.push({name: 'things_iot_mcp.recover_media_playback_operation', state: 'unknown'});
    assert.equal(verifyStep(step, kind, before, after, 'realtime_native'), 2);
    after.tools.push({name: 'things_iot_mcp.get_media_playback_status', state: 'succeeded'});
    assert.throws(() => verifyStep(step, kind, before, after, 'realtime_native'), /formal_tool_missing_or_repeated/);
  }
});

// FILE23真实模型先查询原焦点再acquire；此处复刻工具顺序，媒体及回执均为明确标注的合成数据。
test('安全状态查询不是第二个业务动作，初始焦点仍须本轮唯一acquire和实际有声回执', () => {
  for (const chain of ['cascaded_agentruntime', 'realtime_native'])
    for (const name of ['get_output_focus_status', 'get_media_playback_status'])
      for (const state of ['succeeded', 'failed', 'unknown'])
        for (const position of ['before', 'after']) {
          const before = {...observation(), file: null};
          const after = completed('initial', before, {consumedNonSilent: before.consumedNonSilent + 1});
          const query = {name: 'things_iot_mcp.' + name, state};
          if (position === 'before') after.tools.unshift(query);
          else after.tools.push(query);
          assert.equal(verifyStep('initial', null, before, after, chain), 2);
        }
});

test('状态查询不能提供缺失的控制成功、焦点许可、实际音频或放宽单查询上限', () => {
  const before = {...observation(), file: null};
  const query = {name: 'things_iot_mcp.get_output_focus_status', state: 'succeeded'};
  for (const change of [
    value => { value.tools = [query]; },
    value => { value.tools[0].state = 'unknown'; value.tools.push(query); },
    value => { value.tools.push(query); value.focusReceipts = before.focusReceipts; },
    value => { value.tools.push(query); value.consumedNonSilent = before.consumedNonSilent; },
    value => { value.tools.push(query, {...query}); },
    value => { value.tools.push(query, {name: 'things_iot_mcp.recover_media_playback_operation', state: 'unknown'}); },
    value => { value.tools.push({...query, state: 'started'}); },
    value => { value.tools.push({...query, name: 'other.get_output_focus_status'}); },
    value => { value.tools.push({...query, name: 'things_iot_mcp.get_output_focus_status.extra'}); },
  ]) {
    const after = completed('initial', before, {consumedNonSilent: before.consumedNonSilent + 1});
    change(after);
    assert.throws(() => verifyStep('initial', null, before, after, 'cascaded_agentruntime'));
  }
});

test('文件单意图不能因预期工具成功而放过同轮其他成功控制工具', () => {
  const before = {...observation(), file: null}; // 邀请前没有上一节目的播放器。
  for (const extra of ['control_media_playback', 'control_output_focus']) {
    const after = completed('invite', before); // 仅正式邀请成功是本轮原计划。
    after.tools.push({name: 'things_iot_mcp.' + extra, state: 'succeeded'});
    assert.throws(() => verifyStep('invite', 'music', before, after, 'realtime_native'),
      /formal_tool_missing_or_repeated/);
  }
});

test('文件工具须匹配完整正式名称，恢复目录或引用不能替代邀请', () => {
  const before = {...observation(), file: null}; // 只读恢复不能代替本轮指定动作。
  for (const names of [['other.things_iot_mcp.invite_media_playback'],
    ['things_iot_mcp.recover_media_playback_operation'],
    ['things_iot_mcp.recover_media_playback_operation', 'things_iot_mcp.recover_media_playback_operation']]) {
    const after = completed('invite', before); // 合成终态只用于核验，不请求真实工具。
    after.tools = names.map(name => ({name, state: 'succeeded'}));
    assert.throws(() => verifyStep('invite', 'music', before, after, 'realtime_native'));
  }
});

test('文件邀请单业务意图可附一次只读恢复，恢复终态不提升邀请或媒体事实', () => {
  const before = {...observation(), file: null}; // 无历史播放器，不能借恢复展示冒充新节目。
  for (const chain of ['cascaded_agentruntime', 'realtime_native'])
    for (const state of ['unknown', 'failed', 'succeeded'])
      for (const position of ['before', 'after']) {
        const after = completed('invite', before); // 指定动作仍须自己的唯一成功结果。
        const recovery = {name: 'things_iot_mcp.recover_media_playback_operation', state};
        if (position === 'before') after.tools.unshift(recovery);
        else after.tools.push(recovery);
        assert.doesNotThrow(() => verifyStep('invite', 'music', before, after, chain));
      }
});

test('文件全双工经业务资格核验的两个工具仍须同原回复完整结束', () => {
  const before={...observation(),file:null}; // 数量必须先来自实际业务工具类别核验，而不是直接信观察到两个结果。
  const after=completed('invite',before);
  after.tools.push({name:'things_iot_mcp.recover_media_playback_operation',state:'unknown'});
  const verifiedToolCount=verifyStep('invite','music',before,after,'realtime_native');
  const fixture = roundFixture();
  fixture.arm();fixture.recognize('private-current');
  fixture.tool('started', 'private-current', 'private-recovery');
  fixture.tool('unknown', 'private-current', 'private-recovery');
  fixture.tool('started', 'private-current', 'private-invite');
  fixture.tool('succeeded', 'private-current', 'private-invite');
  fixture.emit({kind: 'responseCompleted', id: 'private-current'});
  assert.doesNotThrow(() => liveDiagnostics.verifyRoundFacts(fixture.facts(), verifiedToolCount));
  assert.throws(() => liveDiagnostics.verifyRoundFacts(fixture.facts())); // 默认1仍供既有Live与单工具回归使用。
});

test('只读恢复不能替代失败动作、引入重复动作或接纳伪工具及未结束查询', () => {
  const before = {...observation(), file: null}; // 全部组合必须仍有唯一明确成功的邀请。
  const recovery = {name: 'things_iot_mcp.recover_media_playback_operation', state: 'unknown'};
  for (const state of ['unknown', 'failed', 'started']) {
    const after = completed('invite', before);
    after.tools[0].state = state;
    after.tools.push({...recovery, state: 'succeeded'});
    assert.throws(() => verifyStep('invite', 'music', before, after, 'cascaded_agentruntime'));
  }
  for (const extras of [[recovery, {...recovery, state: 'failed'}],
    [{name: 'things_iot_mcp.invite_media_playback', state: 'succeeded'}],
    ...['recover_media_playback_operation', 'other.things_iot_mcp.recover_media_playback_operation',
      'things_iot_mcp.recover_media_playback_operation.extra', 'things_iot_mcp.RECOVER_media_playback_operation']
      .map(name => [{name, state: 'succeeded'}]),
    ...['started', '', null, 'other'].map(state => [{...recovery, state}])]) {
    const after = completed('invite', before);
    after.tools.push(...extras);
    assert.throws(() => verifyStep('invite', 'music', before, after, 'cascaded_agentruntime'));
  }
  const missing = completed('invite', before);
  missing.tools = [recovery];
  assert.throws(() => verifyStep('invite', 'music', before, missing, 'cascaded_agentruntime'));
});

test('附加只读恢复不放宽邀请空基线、真实解码、焦点回执及AI持续条件', () => {
  const before = observation(); // 成功查询与失败媒体事实必须相互独立。
  for (const [step, kind, previous, after] of [
    ['invite', 'music', {...before,file:null}, completed('invite',{...before,file:null},{file:{...before.file,state:'loaded'}})],
    ['load', 'video', before, completed('load',before)],
    ['play', 'video', before, completed('play',before,{file:{...before.file,samples:0,energy:0,frames:0,pixels:false}})],
    ['pause', 'video', before, completed('pause',before,{file:{...before.file,state:'paused',paused:true}})],
    ['restoreAi', 'video', before, completed('restoreAi',before)],
    ['initial', null, before, completed('initial',before,{consumedNonSilent:5})],
    ['stop', 'video', before, completed('stop',before)],
    ['invite', 'music', {...before,file:null}, completed('invite',{...before,file:null},{captureOpen:false})]]) {
    if(step==='initial') after.focusReceipts=before.focusReceipts; // completed会产生新回执，故生成后才构造确实缺失的原事实。
    after.tools.push({name:'things_iot_mcp.recover_media_playback_operation',state:'succeeded'});
    assert.throws(()=>verifyStep(step,kind,previous,after,'realtime_native'));
  }
});

test('双工具仍须同原回复、无孤立重复或pending，显式数量不能自行放宽', () => {
  // 标准工具片段的归属来自可信responseId，不能用总数2猜本轮两个工具已结束。
  for (const mutate of [fixture => fixture.tool('unknown', 'private-other', 'private-recovery'),
    fixture => fixture.tool('started', 'private-current', 'private-invite'),
    fixture => fixture.tool('unknown', 'private-current', 'private-recovery'),
    fixture => fixture.tool('succeeded', 'private-current', 'private-orphan')]) {
    const fixture = roundFixture();fixture.arm();fixture.recognize('private-current');
    fixture.tool('started', 'private-current', 'private-recovery');
    fixture.tool('unknown', 'private-current', 'private-recovery');
    fixture.tool('started', 'private-current', 'private-invite');
    fixture.tool('succeeded', 'private-current', 'private-invite');
    mutate(fixture);
    fixture.emit({kind: 'responseCompleted', id: 'private-current'});
    assert.throws(() => liveDiagnostics.verifyRoundFacts(fixture.facts(), 2));
  }
  const pending = roundFixture();pending.arm();pending.recognize('private-current');
  pending.tool('started', 'private-current', 'private-recovery');
  pending.tool('started', 'private-current', 'private-invite');
  pending.tool('succeeded', 'private-current', 'private-invite');
  pending.emit({kind: 'responseCompleted', id: 'private-current'});
  assert.throws(() => liveDiagnostics.verifyRoundFacts(pending.facts(), 2));
  const valid = roundFixture();valid.arm();valid.recognize('private-current');
  valid.tool('started', 'private-current');valid.tool('succeeded', 'private-current');
  valid.emit({kind: 'responseCompleted', id: 'private-current'});
  for (const count of [0, 3, 1.5, '2', null])
    assert.throws(() => liveDiagnostics.verifyRoundFacts(valid.facts(), count));
});

test('原19组合按当时唯一工具判据失败保留，新单业务意图判据另行验证', () => {
  // 首轮焦点成功保留为高水位；音乐轮只核新增结果，不把累计三个工具当本轮三个。
  const before = {...observation(), file: null,
    tools: [{name: 'things_iot_mcp.control_output_focus', state: 'succeeded'}]};
  const invited = completed('invite', before, {file: null}); // 唯一邀请成功且播放器为空本身允许。
  assert.doesNotThrow(() => verifyStep('invite', 'music', before, invited, 'cascaded_agentruntime'));
  const after = {...invited, tools: [...before.tools,
    {name: 'things_iot_mcp.recover_media_playback_operation', state: 'unknown'},
    {name: 'things_iot_mcp.invite_media_playback', state: 'succeeded'}]};
  assert.equal(after.tools.slice(before.tools.length).length, 2);
  assert.equal(after.file, null);
  // 原19真实失败不追改；旧数量判据确实拒绝两结果，新判据仅允许旁路查询，不证明邀请送达或播放。
  assert.throws(() => assert.equal(after.tools.slice(before.tools.length).length, 1));
  assert.doesNotThrow(() => verifyStep('invite', 'music', before, after, 'cascaded_agentruntime'));
});

test('邀请只允许无播放器或真实未加载基线，不允许额外加载和播放', () => {
  const before = {...observation(), file: null}; // 新节目必须从原播放器已释放的基线开始。
  const invited = {state: '', generation: '0', busy: false, samples: 0, energy: 0,
    frames: 0, outputEnabled: true, paused: true, pixels: false, kind: 'music'}; // 现有播放器initialize的实际字段。
  assert.doesNotThrow(() => verifyStep('invite', 'music', before, completed('invite', before), 'realtime_native'));
  assert.doesNotThrow(() => verifyStep('invite', 'music', before,
    completed('invite', before, {file: invited}), 'realtime_native'));
  for (const change of [file => { file.state = 'loaded'; }, file => { file.state = 'playing'; },
    file => { file.generation = '1'; }, file => { file.busy = true; },
    file => { file.samples = 1; }, file => { file.energy = 1; }, file => { file.frames = 1; },
    file => { file.paused = false; }, file => { file.pixels = true; }, file => { file.kind = 'video'; }]) {
    const after = completed('invite', before, {file: {...invited}}); // 空state也可能处于尚未完成的load，不能只看state。
    change(after.file);
    assert.throws(() => verifyStep('invite', 'music', before, after, 'realtime_native'));
  }
  assert.throws(() => verifyStep('invite', 'music', {...before, file: invited},
    completed('invite', before, {file: invited}), 'realtime_native'));
});

test('邀请基线读取真实控制代次和busy，浏览器内联校验不依赖模块外助手', () => {
  const player = {state: '', generation: '0', busy: false, samples: 0, energy: 0, frames: 0,
    outputEnabled: true, media: {paused: true, tagName: 'AUDIO', src: 'synthetic-private-address'},
    binding: {sessionId: 'synthetic-private-session'}}; // 夹具故意包含私有字段，观察器只能读白名单。
  const realm = {desktopFiles: {player}, metrics: {focusReceipts: [], focusReceiptIndex: 0,
    captured: 10, consumedNonSilent: 4, interactionMode: 'fullDuplex'}, context: {state: 'running'},
    stream: {active: true}, __urMcpObserver: {tools: [], gates: [], toolDiagnostic: {events: []},
      capturedWhileFilePlaying: 0, recognition: true, captureOpen: true, overflow: false}}; // 不创建真实媒体或网络。
  const before = {...observation(), file: null}; // 内联等待和进程端必须使用相同唯一意图判据。
  for (const busy of [false, true]) {
    player.busy = busy;
    player.generation = busy ? '1' : '0';
    const after = vm.runInNewContext('(' + liveDiagnostics.readSnapshot.toString() + ')()', realm); // 仅读当前原字段。
    after.tools = [{name: 'things_iot_mcp.invite_media_playback', state: 'succeeded'}];
    after.tools.push({name:'things_iot_mcp.recover_media_playback_operation',state:'unknown'});
    assert.equal(after.file.busy, busy);
    assert.equal(after.file.generation, busy ? '1' : '0');
    assert.ok(!JSON.stringify(after).includes('synthetic-private'));
    const evaluate = () => vm.runInNewContext('(' + verifyStep.toString() +
      ')("invite","music",before,after,"realtime_native")', {assert, tools, before, after}); // 不注入新增外部函数。
    if (busy) assert.throws(evaluate, /program_control_started_during_invite/);
    else assert.doesNotThrow(evaluate);
  }
});

test('释放已暂停租约不猜额外门控，但要求原播放器输出已关闭', () => {
  const before = observation();
  assert.throws(() => verifyStep('releaseFile', 'video', before, completed('releaseFile', before), 'realtime_native'));
  assert.doesNotThrow(() => verifyStep('releaseFile', 'video', before,
    completed('releaseFile', before, {gates: [...before.gates, {lane: 'audioVideo', enabled: false}]}), 'realtime_native'));
});

test('焦点四操作须核对实际新修订回执，历史重放与成功文字不授予新权限', () => {
  const before = observation();
  for (const step of ['initial', 'suspendAi', 'acquireFile', 'suspendFile', 'restoreAi', 'releaseFile']) {
    const after = completed(step, before, {consumedNonSilent: 5,
      gates: [...before.gates, {lane: 'audioVideo', enabled: step === 'acquireFile'}]});
    assert.doesNotThrow(() => verifyStep(step, 'video', before, after, 'realtime_native'));
    for (const change of [value => { value.focusReceiptIndex = before.focusReceiptIndex; },
      value => { value.focusReceipts = []; },
      value => { value.focusReceipts.at(-1).operation = 'unexpected'; },
      value => { value.focusReceipts.at(-1).state = 'denied'; },
      value => { value.focusReceipts.at(-1).code = 409; },
      value => { value.focusReceipts.at(-1).revision = 3; },
      value => { value.focusReceipts.at(-1).revision = Infinity; },
      value => { value.focusReceipts.at(-1).receiptIndex = 0; },
      value => { value.focusReceipts.at(-1).receiptIndex += 1; },
      value => { value.focusReceipts.push({...value.focusReceipts.at(-1)}); }]) {
      const invalid = structuredClone(after); change(invalid);
      assert.throws(() => verifyStep(step, 'video', before, invalid, 'realtime_native'));
    }
  }
});

test('只读观察不重发send，不更改入口返回值，不保存工具ID或自由名称正文', () => {
  const forwarded = [];
  const received = [];
  const realm = {incoming: value => { received.push(value); return 'incoming_return'; },
    send: value => { forwarded.push(value); return value.kind !== 'denied'; },
    desktopFiles: {player: {state: 'playing'}}, source: {disconnect() {}},
    context: {createConstantSource: () => ({offset: {value: 1}, connect() {}, start() {}})},
    worklet: {}};
  vm.createContext(realm);
  vm.runInContext('(' + installObserver.toString() + ')()', realm);
  assert.equal(realm.incoming({kind: 'toolStatus', id: 'private-id', state: 'started',
    name: 'private-source-secret'}), 'incoming_return');
  realm.incoming({kind: 'toolStatus', id: 'private-id', state: 'succeeded'});
  assert.equal(realm.send({kind: 'captured'}), true);
  assert.equal(realm.send({kind: 'denied'}), false);
  assert.equal(forwarded.length, 2);
  assert.equal(received.length, 2);
  assert.equal(realm.__urMcpObserver.capturedWhileFilePlaying, 1);
  assert.equal(JSON.stringify(realm.__urMcpObserver).includes('private'), false);
  realm.__urMcpObserver.keywords = ['暂停'];
  realm.incoming({kind: 'text', text: '\n用户：请暂停音乐\n助手：'});
  assert.equal(realm.__urMcpObserver.recognition, true);
  realm.incoming({kind: 'text', text: '助手回复：暂停'});
  assert.equal(realm.__urMcpObserver.recognition, true);
  realm.incoming({kind: 'text', text: '\n用户：播放音乐\n助手：'});
  assert.equal(realm.__urMcpObserver.recognition, false);
});

test('没有显式真实开关时入口拒绝，不启动设备或访问环境', async () => {
  await assert.rejects(run({}), /explicit_live_opt_in_required/);
});
test('正式文件宿主先构造保存原send引用，观察器仍采到实际门控且仅发送一次', () => {
  const sent = [];
  const realm = {incoming: () => true, send: value => {sent.push(value); return true;},
    desktopFiles: {player: null}, source: {disconnect() {}},
    context: {createConstantSource: () => ({offset: {}, connect() {}, start() {}})}, worklet: {}};
  realm.desktopFiles.send = realm.send; // 与正式activate构造UrDesktopFiles(context, send, ...)一致。
  const lane = {send: realm.send}; realm.desktopFiles.lanes = new Map([['audioVideo', lane]]);
  vm.createContext(realm); vm.runInContext('(' + installObserver.toString() + ')()', realm);
  const fact = {kind: 'fileLaneGated', lane: 'audioVideo', enabled: true, gateTicket: 'synthetic-private'};
  assert.equal(realm.desktopFiles.send(fact), true); assert.equal(sent.length, 1); assert.equal(sent[0], fact);
  assert.equal(realm.__urMcpObserver.gates.length, 1); assert.equal(realm.__urMcpObserver.gates[0].enabled, true);
  assert.equal(lane.send({...fact, enabled: false}), true); assert.equal(sent.length, 2);
  assert.equal(realm.__urMcpObserver.gates.length, 2); assert.equal(realm.__urMcpObserver.gates[1].enabled, false);
  assert.ok(!JSON.stringify(realm.__urMcpObserver).includes('synthetic-private'));
});

// 回复等待旁证只记录固定值，不改变原CLI调用数、谓词、错误或期限。
test('CLI原固定Wait超时前缀精确分类且敏感正文不投影', () => {
  for (const text of ['Wait timed out after 25000ms', 'Wait timed out after 25s. secret=https://private'])
    assert.equal(liveDiagnostics.browserFailureCategory(null, {error:text}), 'browser_timeout');
  for (const text of ['prefix Wait timed out after 25s', 'Wait timed out after -1ms', 'Wait timed out after Infinityms', 'Wait timed out after 1234567890ms', 'Wait timed out after 25banana'])
    assert.equal(liveDiagnostics.browserFailureCategory(null, {error:text}), 'browser_command_failed');
  assert.equal(liveDiagnostics.browserFailureCategory(null, {error:{name:'TimeoutError',message:'private'}}), 'browser_command_failed');
});
test('原回复等待单次采样不将旧水位未增长改为成功', async () => {
  const vm=require('node:vm'), diagnostic={}, calls=[];
  const context={metrics:{completedResponses:7},responseId:'',pendingPlayback:new Map(),performance:{now:()=>10}};
  await liveDiagnostics.waitFixtureReply('cascaded_agentruntime',7,async expression=>{
    calls.push(expression);assert.equal(vm.runInNewContext(expression,context),false);
    context.metrics.completedResponses=8;assert.equal(vm.runInNewContext(expression,context),true);
  },diagnostic);
  assert.equal(calls.length,1);assert.equal(diagnostic.beforeCompletedResponses,7);
  assert.ok(Number.isFinite(diagnostic.beginMS)&&diagnostic.endMS>=diagnostic.beginMS);
  assert.equal(context.__urMcpReplyDrainSample.predicatePassed,true);
});
test('CLI失败或截止后终态不能由旁证改为PASS且错误对象保留', async () => {
  const vm=require('node:vm');for(const ready of [false,true]){
    const diagnostic={}, error=Error('private-fixed-fixture'),context={metrics:{completedResponses:ready?1:0},responseId:'',pendingPlayback:new Map(),performance:{now:()=>10}};
    let calls=0;await assert.rejects(liveDiagnostics.waitFixtureReply('cascaded_agentruntime',0,async expression=>{
      ++calls;assert.equal(vm.runInNewContext(expression,context),ready);throw error;
    },diagnostic),actual=>actual===error);
    assert.equal(calls,1);assert.ok(diagnostic.endMS>=diagnostic.beginMS);
    context.metrics.completedResponses=1;assert.equal(context.__urMcpReplyDrainSample.predicatePassed,ready);
  }
});
test('页面采样未知或旧轮拒绝且投影不输出自由字段', () => {
  const vm=require('node:vm'), source='('+liveDiagnostics.readReplyDrainSample.toString()+')(5)';
  const value={beginMS:5,sampleMS:10,completedResponses:1,responseActive:false,pendingPlaybackFrames:0,predicatePassed:true,secret:'https://private'};
  const result=vm.runInNewContext(source,{__urMcpReplyDrainSample:value});assert.equal(result.known,true);assert.equal(JSON.stringify(result).includes('private'),false);
  for(const change of [{beginMS:4},{sampleMS:Infinity},{completedResponses:NaN},{pendingPlaybackFrames:33},{responseActive:null}])
    assert.equal(vm.runInNewContext(source,{__urMcpReplyDrainSample:{...value,...change}}).known,false);
  assert.equal(vm.runInNewContext(source,{}).known,false);
});

test('默认关闭时不写页面且成功任意CLI数据不改变原undefined返回', async () => {
  const vm=require('node:vm'),context={metrics:{completedResponses:1},responseId:'',pendingPlayback:new Map()};let calls=0;
  const result=await liveDiagnostics.waitFixtureReply('cascaded_agentruntime',0,async expression=>{
    ++calls;assert.equal(expression,'metrics.completedResponses > 0 && responseId === "" && pendingPlayback.size === 0');
    assert.equal(vm.runInNewContext(expression,context),true);return {privateData:'not-returned'};
  });assert.equal(result,undefined);assert.equal(calls,1);assert.equal(context.__urMcpReplyDrainSample,undefined);
});
test('显式采样也保留成功undefined与原错误对象', async () => {
  const diagnostic={};assert.equal(await liveDiagnostics.waitFixtureReply('cascaded_agentruntime',0,async ()=>({privateData:'not-returned'}),diagnostic),undefined);
  const error=Error('original');await assert.rejects(liveDiagnostics.waitFixtureReply('cascaded_agentruntime',0,async()=>{throw error;},{}),value=>value===error);
});
