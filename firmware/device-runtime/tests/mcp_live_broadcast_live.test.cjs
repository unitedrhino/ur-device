// 文件说明：连续节目MCP验收器离线合同与生命周期回归；合成PCM/像素只测试判据，不作为真实模型或媒体验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const live = require('./mcp_live_broadcast_live.cjs');
const file = require('./mcp_file_media_live.cjs');

// plan 只构造有限语音和节目引用，不包含身份、许可或可执行回调。
function plan(chain = 'realtime_native') {
  return {chain, initial: {wav: '/fixture/initial.wav', keywords: ['焦点']},
    programs: [0, 1].map(index => ({kind: 'audioVideo', programRef: 'controlled-continuous-program',
      inputs: Object.fromEntries(live.steps.map(step => [step, {wav: `/fixture/${index}-${step}.wav`, keywords: ['节目']}]))}))};
}
// observation 模拟已读取白名单事实，不能代替真实设备PCM或H264。
function observation() {
  return {tools: [], gates: [{lane: 'audioVideo', enabled: true}], tracks: [],
    focusReceipts: [{operation: 'acquire', state: 'acquired', revision: 3, receiptIndex: 1, code: 200}],
    focusReceiptIndex: 1, groupAcquiredRevision: 3, groupAcquiredReceiptIndex: 1, programReleaseReceipt: null,
    captured: 10, overlap: 3, recognition: true, captureOpen: true, overflow: false,
    consumedNonSilent: 4, mode: 'fullDuplex', contextState: 'running', streamActive: true,
    stoppedGroups: 0, instanceCount: 1, pcmDetached: false, pcmWindows: 10, nonSilentWindows: 5,
    time: 14, pauseAt: 10, pauseMarker: 30, pauseAudioAt: 10, pauseAudioMarker: 30,
    live: {instance: 1, audioActive: false, videoActive: false, frames: 1,
      connected: true, outputEnabled: true, pcmEnergy: 0, pixels: false, marker: null, frequency: null, audioMarker: null, videoCleared: true,
      freshAudioGeneration: 0, freshVideoGeneration: 0}};
}
// completed 生成一个原工具返回与相应实际事实，负例会逐项移除必需证据。
function completed(step, before) {
  const after = structuredClone(before);
  after.tools.push({name: 'things_iot_mcp.' + live.tools[step], state: 'succeeded'});
  const operation = {initial: 'acquire', suspendAi: 'suspend', acquireLive: 'acquire', restoreAi: 'restore'}[step];
  if (operation) {
    after.focusReceiptIndex++;
    after.focusReceipts.push({operation, state: {acquire: 'acquired', suspend: 'suspended', release: 'released', restore: 'restored'}[operation],
      revision: 4, receiptIndex: 2, code: 200});
  }
  if (step === 'invite') {after.instanceCount++; after.live.instance++;}
  if (step === 'acquireLive') after.gates.push({lane: 'audioVideo', enabled: true});
  const mapping = {resumeAudio: ['audio', 'active'], resumeVideo: ['video', 'active'], pauseAudio: ['audio', 'paused'],
    pauseVideo: ['video', 'paused'], resumeAudioAfterPause: ['audio', 'active'], resumeVideoAfterPause: ['video', 'active'],
    stopAudio: ['audio', 'stopped'], stopVideo: ['video', 'stopped']}[step];
  if (mapping) {
    const [track, state] = mapping;
    after.tracks.push({track, state, code: 200, generation: 2, instance: before.live.instance});
    after.live[track + 'Active'] = state === 'active';
    if (state === 'active') after.live[track === 'audio' ? 'freshAudioGeneration' : 'freshVideoGeneration'] = 2;
    if (track === 'audio' && state === 'active') {after.nonSilentWindows++; after.live.pcmEnergy = 0.05; after.live.frequency = 520; after.live.audioMarker = 2;}
    if (track === 'video' && state === 'active') {after.live.frames++; after.live.pixels = true; after.live.marker = 2;
      after.live.audioMarker = 2; after.live.frequency = 520; after.live.audioActive = true; after.live.pcmEnergy = 0.05;}
    if (step === 'pauseAudio') {after.overlap++; after.captured++;}
    if (['pauseAudio','stopAudio'].includes(step)) {after.live.videoActive=true;after.live.frames++;
      after.live.pixels=true;after.live.marker=2;after.live.videoCleared=false;}
    if (step === 'stopVideo') {after.live = null; after.pcmDetached = true; after.stoppedGroups++;}
  }
  if (step === 'inspectReleasedLive') {after.live = null; after.pcmDetached = true; after.stoppedGroups = 1;
    after.programReleaseReceipt = {operation: 'release', state: 'released', code: 200, revision: 4, receiptIndex: 2};
    after.focusReceiptIndex = 2; after.focusReceipts.push(after.programReleaseReceipt);
    after.gates.push({lane: 'audioVideo', enabled: false});}
  if (['initial', 'restoreAi'].includes(step)) after.consumedNonSilent++;
  return after;
}
test('固定两次连续完整AV节目、27条语音，允许同源后继但不接受文件或授权字段', () => {
  assert.equal(live.validatePlan(plan()).chain, 'realtime_native');
  assert.equal(live.steps.length * 2 + 1, 27);
  for (const change of [p => {p.module = '/private';}, p => {p.parentSessionId = 'forged';},
    p => {p.programs[0].kind = 'music';}, p => {p.programs[0].programRef = 'http://private';},
    p => {p.programs[0].inputs.load = p.initial;}, p => {delete p.programs[1].inputs.inspectReleasedLive;},
    p => {p.initial = {text: '节目', keywords: ['节目']};}, p => {p.initial.token = 'private';}]) {
    const value = plan(); change(value); assert.throws(() => live.validatePlan(value));
  }
  assert.equal(live.validatePlan(plan('cascaded_agentruntime')).chain, 'cascaded_agentruntime');
});
test('源码场景选择拒绝任意对象、路径或模块，未显式开启Live不能运行', async () => {
  for (const name of ['../private', '', {}, 'signaling']) await assert.rejects(file.run({}, name), /invalid_fixed_scenario/);
  await assert.rejects(live.run({UR_OUTPUT_FOCUS: 'live'}), /explicit_live_opt_in_required/);
  await assert.rejects(file.run({UR_MCP_FILE_LIVE: '1', UR_OUTPUT_FOCUS: 'live'}));
});
test('取证定位缺省与1保留原journal，仅显式2/3/4选择独立journal，非法值先拒绝',()=>{const path=require('node:path'),first=live.evidenceFile({}),second=live.evidenceFile({UR_MCP_LIVE_ATTEMPT:'2'}),third=live.evidenceFile({UR_MCP_LIVE_ATTEMPT:'3'}),fourth=live.evidenceFile({UR_MCP_LIVE_ATTEMPT:'4'});assert.equal(live.evidenceFile({UR_MCP_LIVE_ATTEMPT:'1'}),first);assert.equal(path.basename(path.dirname(first)),'mcp-live-broadcast-work');assert.equal(path.basename(path.dirname(second)),'mcp-live-broadcast-work-2');assert.equal(path.basename(path.dirname(third)),'mcp-live-broadcast-work-3');assert.equal(path.basename(path.dirname(fourth)),'mcp-live-broadcast-work-4');assert.equal(new Set([first,second,third,fourth]).size,4);for(const value of ['',0,1,2,3,4,null,'02','03','04','5','../work','/another'])assert.throws(()=>live.evidenceFile({UR_MCP_LIVE_ATTEMPT:value}));});
test('每步要求唯一原工具与实际状态，原生链不能停止AI输入', () => {
  for (const step of ['initial', ...live.steps]) {
    const before = observation(), after = completed(step, before);
    assert.doesNotThrow(() => live.verifyStep(step, 'audioVideo', before, after, 'realtime_native'), step);
    for (const change of [value => {value.tools.at(-1).state = 'unknown';},
      value => {value.tools.push({...value.tools.at(-1)});}, value => {value.tools.at(-1).name = 'other';},
      value => {value.captureOpen = false;}, value => {value.overflow = true;}]) {
      const invalid = structuredClone(after); change(invalid);
      assert.throws(() => live.verifyStep(step, 'audioVideo', before, invalid, 'realtime_native'), step);
    }
  }
});
test('非静音实际PCM与原代次Fresh不可由active字符串或旧音频窗口替代', () => {
  const before = observation(), after = completed('resumeAudio', before);
  for (const change of [v => {v.nonSilentWindows = before.nonSilentWindows;}, v => {v.live.pcmEnergy = 0;},
    v => {v.live.pcmEnergy = NaN;}, v => {v.live.freshAudioGeneration = 1;},
    v => {v.tracks.at(-1).instance++;}, v => {v.tracks.at(-1).generation = 0;},
    v => {v.live.outputEnabled = false;}, v => {v.live.frequency = 529;},
    v => {v.live.audioMarker = null;}]) {
    const invalid = structuredClone(after); change(invalid);
    assert.throws(() => live.verifyStep('resumeAudio', 'audioVideo', before, invalid, 'realtime_native'));
  }
});
test('恢复实际PCM频率秒标必须随暂停时间前进，跨秒混窗不能扩大到40Hz容差', () => {
  const before = observation(), after = completed('resumeAudioAfterPause', before);
  assert.doesNotThrow(() => live.verifyStep('resumeAudioAfterPause', 'audioVideo', before, after, 'realtime_native'));
  for (const change of [v => {v.live.audioMarker = 30; v.live.frequency = 1640;},
    v => {v.live.frequency = 540;}, v => {v.live.audioMarker = 0; v.live.frequency = 440;},
    v => {v.time = 11.9;}, v => {v.time = 43;}]) {
    const invalid = structuredClone(after); change(invalid);
    assert.throws(() => live.verifyStep('resumeAudioAfterPause', 'audioVideo', before, invalid, 'realtime_native'));
  }
});
test('暂停必须实际静音且原语音被识别，不能仅工具成功或采集累计增长', () => {
  const before = observation(), after = completed('pauseAudio', before);
  for (const change of [v => {v.live.pcmEnergy = 0.01;}, v => {v.recognition = false;},
    v => {v.overlap = before.overlap;}, v => {v.captured = before.captured;}]) {
    const invalid = structuredClone(after); change(invalid);
    assert.throws(() => live.verifyStep('pauseAudio', 'audioVideo', before, invalid, 'realtime_native'));
  }
});
test('暂停或停止音频不能误停未控制视频，须仍有实际可见新帧',()=>{
  for(const step of ['pauseAudio','stopAudio']) {
    const before=observation();before.live.videoActive=true;
    const after=completed(step,before);
    assert.doesNotThrow(()=>live.verifyStep(step,'audioVideo',before,after,'realtime_native'));
    for(const change of [v=>{v.live.videoActive=false;},v=>{v.live.frames=before.live.frames;},
      v=>{v.live.pixels=false;},v=>{v.live.marker=null;}]) {
      const invalid=structuredClone(after);change(invalid);
      assert.throws(()=>live.verifyStep(step,'audioVideo',before,invalid,'realtime_native'));
    }
  }
});
test('只恢复音频时视频仍暂停且画布清空，不允许暗复活或帧水位前进',()=>{
  const before=observation(),after=completed('resumeAudioAfterPause',before);
  assert.doesNotThrow(()=>live.verifyStep('resumeAudioAfterPause','audioVideo',before,after,'realtime_native'));
  for(const change of [v=>{v.live.videoActive=true;},v=>{v.live.videoCleared=false;},
    v=>{v.live.frames++;},v=>{v.live.pixels=true;}]) {
    const invalid=structuredClone(after);change(invalid);
    assert.throws(()=>live.verifyStep('resumeAudioAfterPause','audioVideo',before,invalid,'realtime_native'));
  }
});
test('H264可见像素、实际新帧和当前秒标缺一不可，32秒环绕按实际暂停时长核对', () => {
  const before = observation(), after = completed('resumeVideoAfterPause', before);
  assert.doesNotThrow(() => live.verifyStep('resumeVideoAfterPause', 'audioVideo', before, after, 'realtime_native'));
  for (const change of [v => {v.live.marker = 30;}, v => {v.live.marker = 15;}, v => {v.live.pixels = false;},
    v => {v.live.frames = before.live.frames;}, v => {v.live.freshVideoGeneration = 0;},
    v => {v.time = before.pauseAt + 1.9;}, v => {v.time = before.pauseAt + 33;},
    v => {v.live.audioMarker = 8;}]) {
    const invalid = structuredClone(after); change(invalid);
    assert.throws(() => live.verifyStep('resumeVideoAfterPause', 'audioVideo', before, invalid, 'realtime_native'));
  }
});
test('原整组实际停止与release新修订是后继前提，历史重放或局部静音不能算释放', () => {
  const before = observation(), after = completed('inspectReleasedLive', before);
  for (const change of [v => {v.live = before.live;}, v => {v.pcmDetached = false;},
    v => {v.stoppedGroups = 0;}, v => {v.gates.at(-1).enabled = true;},
    v => {v.programReleaseReceipt.revision = 3;}, v => {v.programReleaseReceipt.state = 'acquired';},
    v => {v.programReleaseReceipt = null;}]) {
    const invalid = structuredClone(after); change(invalid);
    assert.throws(() => live.verifyStep('inspectReleasedLive', 'audioVideo', before, invalid, 'realtime_native'));
  }
});
test('Owner真实release先于模型只读轮也可观察，不要求重复副作用或将index递增当新权限', () => {
  const before = completed('inspectReleasedLive', observation()); before.tools = [];
  const after = structuredClone(before); after.tools.push({name: 'things_iot_mcp.get_output_focus_status', state: 'succeeded'});
  assert.doesNotThrow(() => live.verifyStep('inspectReleasedLive', 'audioVideo', before, after, 'realtime_native'));
  assert.equal(after.focusReceiptIndex, before.focusReceiptIndex);
});

// browserFixture 构造原UI及只读分析器边界；真实网络、解码器、麦克风与模型均未启动。
function browserFixture({sampling = false} = {}) {
  const connections = [], disconnected = [], listeners = [], sent = [], timers = [], cleared = [];
  const rgba = new Uint8ClampedArray(320 * 240 * 4);
  for (let i = 320 * 64 * 4; i < rgba.length; i += 4) {rgba[i] = 220; rgba[i + 2] = 10; rgba[i + 3] = 255;}
  for (const bit of [1, 4]) {const i = (28 * 320 + 28 + bit * 40) * 4; rgba.fill(255, i, i + 4);}
  const node = {connect: value => connections.push(value), disconnect: value => disconnected.push(value)};
  const analyser = {getFloatTimeDomainData: values => {
    for (let index = 0; index < values.length; index++) values[index] = 0.2 * Math.sin(2 * Math.PI * 1160 * index / 48000);
  }};
  const player = {binding: {instance: 'private-local-instance'}, connected: true, metrics: {audioActive: true, videoActive: true, visibleFrames: 3},
    lane: {node, enabled: true, canvas: {hidden: false, width: 320, height: 240, style: {display: 'block'}, parentElement: null,
      getBoundingClientRect: () => ({width: 320, height: 240}), getContext: () => ({getImageData: () => ({data: rgba})})}},
    transforms: new Map(['audio', 'video'].map(kind => [kind, {channel: {subscribe: callback => {listeners.push({kind, callback}); return () => listeners.push({removed: kind});}}}]))};
  const context = {state: 'running', createConstantSource: () => ({offset: {}, connect() {}, start() {}})};
  const scope = {incoming: value => value.kind, send: value => {sent.push(value); return true;}, context, stream: {active: true},
    source: {disconnect() {}}, worklet: {}, metrics: {captured: 2, consumedNonSilent: 1, interactionMode: 'fullDuplex', focusReceipts: [], focusReceiptIndex: 0},
    desktopLive: {player, context: {currentTime: 10, sampleRate: 48000, state: 'running', createAnalyser: () => analyser}},
    setInterval: (callback, interval) => {const timer = {callback, interval}; timers.push(timer); return timer;},
    clearInterval: timer => cleared.push(timer),
    getComputedStyle: element => ({display: element.style.display, visibility: 'visible', opacity: '1'}), Float32Array, Number, Map};
  scope.desktopLive.send = scope.send; // 正式activate先构造UrLiveHost，宿主保留安装观察器之前的函数引用。
  vm.createContext(scope);
  if (sampling) vm.runInContext('globalThis.__urMcpLiveReadSnapshot=(' + live.readSnapshot.toString() + ');', scope);
  vm.runInContext('(' + live.installObserver.toString() + ')()', scope);
  const snapshot = () => vm.runInContext('(' + live.readSnapshot.toString() + ')()', scope);
  return {scope, snapshot, connections, disconnected, listeners, analyser, sent, timers, cleared};
}
test('Live工具名仅接受正式裸名或things_iot_mcp全名，保留原unknown状态',()=>{
  for(const tool of [...new Set(Object.values(live.tools)), 'recover_media_playback_operation']){
    for(const name of [tool, 'things_iot_mcp.'+tool]){
      const fixture=browserFixture();
      fixture.scope.incoming({kind:'toolStatus',id:'original-call',name,state:'started'});
      fixture.scope.incoming({kind:'toolStatus',id:'original-call',state:'unknown'});
      assert.equal(fixture.snapshot().tools[0].name,'things_iot_mcp.'+tool);
      assert.equal(fixture.snapshot().tools[0].state,'unknown');
    }
  }
});
test('Live观察器拒绝其它服务及伪嵌套同后缀名，不把它算正式媒体工具',()=>{
  for(const name of ['other_service.control_output_focus','other.things_iot_mcp.control_live_track',
    'things_iot_mcp.invite_live_broadcast.extra','prefixcontrol_output_focus']){
    const fixture=browserFixture();
    fixture.scope.incoming({kind:'toolStatus',id:'original-call',name,state:'started'});
    fixture.scope.incoming({kind:'toolStatus',id:'original-call',state:'succeeded'});
    assert.equal(fixture.snapshot().tools[0].name,'other_authorized_tool');
    assert.equal(fixture.snapshot().tools[0].state,'succeeded');
  }
});
test('Live工具诊断区分started和完整返回，记录原终态顺序不泄露调用标识',()=>{
  const fixture=browserFixture();
  fixture.scope.incoming({kind:'toolStatus',id:'private-call',name:'things_iot_mcp.control_output_focus',state:'started'});
  const pending=fixture.snapshot();assert.equal(pending.tools.length,0);
  assert.equal(pending.toolDiagnostic.startedCount,1);assert.equal(pending.toolDiagnostic.pendingCount,1);
  fixture.scope.incoming({kind:'ended'});
  fixture.scope.incoming({kind:'toolStatus',id:'private-call',state:'succeeded'});
  fixture.scope.incoming({kind:'toolStatus',id:'private-orphan',state:'failed'});
  const observed=fixture.snapshot();assert.equal(observed.tools.length,1);
  assert.equal(observed.toolDiagnostic.pendingCount,0);assert.equal(observed.toolDiagnostic.orphanResultCount,1);
  assert.equal(observed.toolDiagnostic.terminalCount,1);assert.equal(observed.toolDiagnostic.firstTerminalSequence,2);
  assert.deepEqual(Array.from(observed.toolDiagnostic.events,value=>value.kind),['started','ended','result','orphan_result']);
  assert.ok(!JSON.stringify(observed).includes('private'));
});
test('Live诊断截断不改变原媒体验收overflow或把终态变成工具完成',()=>{
  const fixture=browserFixture();
  for(let index=0;index<80;++index)fixture.scope.incoming({kind:'ended'});
  const observed=fixture.snapshot();assert.equal(observed.toolDiagnostic.events.length,64);
  assert.equal(observed.toolDiagnostic.terminalCount,80);assert.equal(observed.toolDiagnostic.observationTruncated,true);
  assert.equal(observed.toolDiagnostic.firstTerminalSequence,1);assert.equal(observed.overflow,false);
  assert.equal(observed.tools.length,0);
});
test('分析器只读原末端节点，无新destination；相同音频窗口不重复计数且原关闭精确取消订阅', () => {
  const fixture = browserFixture(), first = fixture.snapshot(), next = fixture.snapshot();
  assert.deepEqual(fixture.connections, [fixture.analyser]);
  assert.equal(first.live.marker, 18); assert.equal(first.live.pixels, true);
  assert.equal(next.pcmWindows, first.pcmWindows); assert.equal(next.nonSilentWindows, first.nonSilentWindows);
  assert.equal(fixture.listeners.length, 2);
  fixture.scope.desktopLive.context.currentTime = 10.6;
  const measured = fixture.snapshot(); assert.equal(measured.live.audioMarker, 18);
  assert.ok(Math.abs(measured.live.frequency - 1160) <= 8); assert.ok(measured.nonSilentWindows > first.nonSilentWindows);
  fixture.listeners[0].callback({kind: 'liveEncodedFresh', generation: 2, gateTicket: 'private-ticket'});
  assert.equal(fixture.snapshot().live.freshAudioGeneration, 2);
  fixture.scope.desktopLive.player = null;
  const stopped = fixture.snapshot(); assert.equal(stopped.pcmDetached, true); assert.equal(stopped.live, null);
  assert.deepEqual(fixture.disconnected, [fixture.analyser]); assert.equal(fixture.listeners.filter(value => value.removed).length, 2);
  assert.ok(!JSON.stringify(stopped).includes('private'));
});
test('只读UI出口保持原返回及原参数，晚到其它实例/自由票据不进入报告', () => {
  const fixture = browserFixture(); fixture.snapshot();
  const original = {kind: 'liveTrackFact', instance: 'wrong-original-instance', trackId: 'broadcast-out', generation: 1, code: 200, state: 'active', gateTicket: 'private'};
  assert.equal(fixture.scope.send(original), true); assert.equal(fixture.sent.at(-1), original);
  assert.equal(fixture.snapshot().tracks.length, 0);
  fixture.scope.send({...original, instance: 'private-local-instance'});
  assert.equal(fixture.snapshot().tracks.length, 1); assert.ok(!JSON.stringify(fixture.snapshot()).includes('private'));
  fixture.scope.incoming({kind: 'toolStatus', id: 'private-call', name: 'things_iot_mcp.invite_live_broadcast', state: 'started'});
  fixture.scope.incoming({kind: 'toolStatus', id: 'private-call', state: 'succeeded'});
  const observed = fixture.snapshot(); assert.equal(observed.tools[0].name, 'things_iot_mcp.invite_live_broadcast');
  assert.ok(!JSON.stringify(observed).includes('private'));
});
test('已构造宿主保留原send引用时仍采到回执且原发送只调用一次', () => {
  const fixture = browserFixture(); fixture.snapshot();
  const fact = {kind: 'liveTrackFact', instance: 'private-local-instance', trackId: 'broadcast-out', generation: 1, code: 200, state: 'active'};
  assert.equal(fixture.scope.desktopLive.send(fact), true);
  assert.equal(fixture.sent.length, 1); assert.equal(fixture.sent[0], fact);
  assert.equal(fixture.snapshot().tracks.length, 1);
});
test('原播放器已真实disconnect全部连接时传感器不再次指定断开；隐藏祖先不能作可见像素', () => {
  const fixture = browserFixture(); const first = fixture.snapshot(); assert.equal(first.live.pixels, true);
  fixture.scope.desktopLive.player.lane.canvas.parentElement = {style: {display: 'none'}, parentElement: null};
  assert.equal(fixture.snapshot().live.pixels, false);
  fixture.scope.desktopLive.player.closed = true;
  fixture.scope.desktopLive.player = null;
  assert.equal(fixture.snapshot().pcmDetached, true); assert.equal(fixture.disconnected.length, 0);
});
test('固定只读采样保持暂停边沿基线，实际宿主关闭后清原timer且不发送任何消息', () => {
  const fixture = browserFixture({sampling: true}); assert.equal(fixture.timers.length, 1);
  assert.equal(fixture.timers[0].interval, 100); fixture.timers[0].callback();
  fixture.scope.desktopLive.context.currentTime = 10.6; fixture.timers[0].callback();
  assert.equal(fixture.scope.__urMcpObserver.lastAudioMarker, 18);
  assert.equal(fixture.scope.__urMcpObserver.lastMarker, 18);
  fixture.scope.desktopLive.send({kind: 'liveTrackFact', instance: 'private-local-instance', trackId: 'broadcast-video-out', generation: 2, code: 200, state: 'paused'});
  assert.equal(fixture.scope.__urMcpObserver.pauseMarker, 18); assert.equal(fixture.scope.__urMcpObserver.pauseAt, 10.6);
  assert.equal(fixture.sent.length, 1); // 只有原驱动实际事实，没有采样器生成的命令或事实。
  fixture.scope.desktopLive.player.closed = true; fixture.scope.desktopLive.player = null;
  fixture.scope.desktopLive.context.state = 'closed'; fixture.timers[0].callback();
  assert.deepEqual(fixture.cleared, [fixture.timers[0]]); assert.equal(fixture.scope.__urMcpObserver.samplingStopped, true);
  assert.equal(fixture.snapshot().pcmDetached, true);
});
test('固定journal读取拒绝symlink/模式/归属/替换，不接受自由路径且精确关闭原fd', () => {
  const fs = require('node:fs'), path = require('node:path');
  const code = fs.readFileSync(path.join(__dirname, 'mcp_live_broadcast_live.cjs'), 'utf8');
  for (const branch of ['valid', 'link', 'directoryLink', 'mode', 'owner', 'inode', 'realpath']) {
    let closed = 0;
    const metadata = directory => ({isDirectory: () => directory, isFile: () => !directory,
      isSymbolicLink: () => branch === (directory ? 'directoryLink' : 'link'),
      mode: directory ? 0o700 : branch === 'mode' ? 0o644 : 0o600,
      uid: branch === 'owner' ? 2 : 1, dev: 10, ino: 20, size: 0});
    const port = {constants: {O_RDONLY: 0, O_NOFOLLOW: 131072}, lstatSync: filename => metadata(!filename.endsWith('observer.jsonl')),
      realpathSync: filename => branch === 'realpath' ? '/other' : filename,
      openSync: filename => {assert.ok(filename.endsWith('/.temp/task143-ipc-env/mcp-live-broadcast-work/observer.jsonl')); return 7;},
      fstatSync: () => ({...metadata(false), ino: branch === 'inode' ? 21 : 20}),
      readFileSync: fd => {assert.equal(fd, 7); return Buffer.alloc(0);}, closeSync: fd => {assert.equal(fd, 7); closed++;}};
    const module = {exports: {}};
    vm.runInNewContext(code, {module, exports: module.exports, require: name => name === 'node:fs' ? port : require(name),
      __dirname, Buffer, process: {getuid: () => 1, env: {}}, console});
    const read = () => module.exports.readEvidence({UR_MCP_BROADCAST_EVIDENCE: 'task143'});
    if (branch === 'valid') {assert.equal(read().length, 0); assert.equal(closed, 1);}
    else {assert.throws(read); assert.equal(closed, branch === 'inode' ? 1 : 0);}
  }
});
test('结果投影只含有限计数，不把设备观察提升为后端授权或落库证明', () => {
  const after = observation(); after.privateToken = 'synthetic-private'; after.live.url = 'http://private';
  const value = live.result('resumeVideo', after);
  assert.deepEqual(Object.keys(value).sort(), ['instance', 'stoppedGroups', 'pcmWindows', 'nonSilentWindows', 'visibleFrames',
    'markerSecond', 'audioMarkerSecond', 'audioFrequency', 'audioActive', 'videoActive'].sort());
  assert.ok(!JSON.stringify(value).includes('private'));
});

// failedLifecycle 仅注入有限进程/浏览器端口，验证Live共享原收尾，绝不启动实际设备或API。
async function failedLifecycle() {
  const {EventEmitter} = require('node:events'), fs = require('node:fs'), path = require('node:path');
  const child = new EventEmitter(); child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
  let kills = 0, submits = 0; const output = [];
  child.kill = () => {kills++; queueMicrotask(() => {
    child.stderr.emit('data', Buffer.from('request_rejected method=capabilityReport code=403 reason=focusDeviceUnavailable\n'));
    child.stderr.emit('end'); child.emit('close', 1);
  }); return true;};
  const execFile = (_binary, args, _options, callback) => queueMicrotask(() => {
    if (args.includes('fill')) submits++;
    if (args.includes('wait')) return callback(Error('synthetic-private'), '{}');
    callback(null, JSON.stringify({success: true, data: {result: null}}));
  });
  const module = {exports: {}};
  const requirePort = name => name === 'node:child_process' ? {spawn: () => {
    queueMicrotask(() => {child.emit('spawn'); child.stdout.emit('data', Buffer.from('http://127.0.0.1:39741/'));}); return child;
  }, execFile} : name === 'node:fs' ? {statSync: () => ({isFile: () => true}), readFileSync: () => Buffer.from(JSON.stringify(plan()))} : require(name);
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'mcp_file_media_live.cjs'), 'utf8'), {module, exports: module.exports,
    require: requirePort, Buffer, console: {log: value => output.push(JSON.parse(value)), error() {}}, setTimeout, clearTimeout});
  await assert.rejects(module.exports.run({UR_MCP_BROADCAST_LIVE: '1', UR_OUTPUT_FOCUS: 'live', UR_DEVICE_PRODUCT: 'synthetic',
    UR_DEVICE_NAME: 'synthetic', UR_DEVICE_SECRET: 'synthetic-private', UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '1883',
    UR_DEVICE_BROWSER_BIN: '/synthetic/device', UR_MCP_BROADCAST_PLAN: '/synthetic/plan', UR_BROWSER_PORT: '39741'}, 'live'), /mcp_live_broadcast_live_failed/);
  return {kills, submits, output};
}
test('Live复用精确退出/stderr排空的原生命周期，startup失败不重说或伪造成功', async () => {
  const result = await failedLifecycle(); assert.equal(result.kills, 1); assert.equal(result.submits, 0);
  assert.equal(result.output.length, 1); const report = result.output[0];
  assert.equal(report.passed, false); assert.equal(report.phase, 'session_ready'); assert.deepEqual(report.results, []);
  assert.equal(report.diagnostic.deviceFailure.reason, 'focusDeviceUnavailable');
  assert.equal(report.diagnostic.childLifecycle.closed, true); assert.equal(report.diagnostic.childLifecycle.stderrEnded, true);
  assert.ok(!JSON.stringify(report).includes('synthetic-private'));
});
// proof 仅合成透明取证器原白名单，HMAC不是权限；不写文件或调用真实服务。
function proof(step, override = {}) {
  return {toolName: live.tools[step], parentHash: 'a'.repeat(64), operationHash: step === 'inspectReleasedLive' ? null : 'b'.repeat(64),
    inputSHA256: 'c'.repeat(64), grantPresent: true, fallbackAuthPresent: false, grantConsumptionVerified: false,
    childHash: ['initial', 'suspendAi', 'restoreAi'].includes(step) ? null : 'd'.repeat(64),
    focusReleasedObserved: step === 'inspectReleasedLive', focusRevision: step === 'inspectReleasedLive' ? 4 : null,
    responseVerified: true, phase: 'http_response', httpStatus: 200, jsonrpcErrorCode: null, mcpIsError: null, ...override};
}
test('固定journal只接受有界完整白名单，无正文/票据/自由工具或fallback授权', () => {
  const row = proof('initial'); const bytes = Buffer.from(JSON.stringify(row) + '\n');
  assert.equal(live.parseEvidence(bytes).length, 1); assert.deepEqual(live.parseEvidence(Buffer.alloc(0)), []);
  for (const value of [{...row, body: 'private'}, {...row, grantPresent: false}, {...row, fallbackAuthPresent: true},
    {...row, parentHash: 'free-parent'}, {...row, toolName: 'other'}, {...row, childHash: 'private'},
    {...row, responseVerified: 'true'}, {...row, focusRevision: Number.MAX_SAFE_INTEGER + 1}])
    assert.throws(() => live.parseEvidence(Buffer.from(JSON.stringify(value) + '\n')));
  assert.throws(() => live.parseEvidence(bytes.subarray(0, bytes.length - 1)));
  assert.throws(() => live.parseEvidence(Buffer.alloc(262145)));
  assert.throws(() => live.readEvidence({UR_MCP_BROADCAST_EVIDENCE: '/private/other-journal'}), /explicit_fixed_evidence_required/);
});
test('原父、当前邀请子及原query released修订共同匹配，不以UI工具成功替代结果事实', () => {
  const scope = {}, rows = [];
  let next = [...rows, proof('initial')]; live.verifyEvidence('initial', rows, next, null, observation(), scope); rows.push(next[0]);
  next = [...rows, proof('invite')]; live.verifyEvidence('invite', rows, next, null, observation(), scope); rows.push(next.at(-1));
  const device = completed('inspectReleasedLive', observation());
  const current = proof('inspectReleasedLive');
  assert.doesNotThrow(() => live.verifyEvidence('inspectReleasedLive', rows, [...rows, current], null, device, scope));
  for (const value of [{...current, childHash: 'e'.repeat(64)}, {...current, parentHash: 'e'.repeat(64)},
    {...current, focusReleasedObserved: false}, {...current, focusRevision: 3}, {...current, responseVerified: false},
    {...current, httpStatus: 202}, {...current, mcpIsError: true}, {...current, phase: 'transport_unknown'},
    {...current, toolName: 'control_output_focus'}])
    assert.throws(() => live.verifyEvidence('inspectReleasedLive', rows, [...rows, value], null, device, scope));
  assert.throws(() => live.verifyEvidence('inspectReleasedLive', rows, [...rows, current, current], null, device, scope));
  const mutated = structuredClone(rows); mutated[0].inputSHA256 = 'f'.repeat(64);
  assert.throws(() => live.verifyEvidence('inspectReleasedLive', rows, [...mutated, current], null, device, scope));
  assert.throws(() => live.verifyEvidence('invite', rows, [...rows, proof('invite')], null, device, scope));
});
