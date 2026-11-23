// 文件说明：三台真实C++设备的AI永久接续验收；A仅兼容引导，B取得完整组后再向C移交，离线测试不代表端云通过。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {spawn, execFile} = require('node:child_process');
const {randomUUID, createHash} = require('node:crypto');
const control = require('./browser_ai_control_live.cjs');
const {parseJSON, waitClosed} = require('./browser_webrtc_text_live.cjs');
const {createDeviceFailureObserver,clientFailureCategory,runtimeFailureCategory,deviceFailureCategory} = require('./mcp_file_media_live.cjs');

// 文件说明：仅审核timeout未接听候选的预期安全退休分类；不改normalExit或生产Fail-closed。

// expectedCandidateRetirement 要求原场景成功、原期限/关联与实际释放全部齐备；缺字段不得默认成功。
// 关联/从未接听证明须由既有同scope wire原始事实在RAM严格核定后产生，不能由页面自行声明。
function expectedCandidateRetirement(proof, cleanup, diagnostics) {
  if (!proof || !cleanup || !diagnostics) return false;
  if (proof.variant !== 'timeout' || proof.role !== 'C' || proof.scenarioComplete !== true ||
      proof.sourceState !== 'aborted' || !Number.isFinite(proof.prepareElapsedMs) ||
      proof.prepareElapsedMs < 30000 || proof.prepareElapsedMs >= 45000) return false;
  for (const key of ['sameOriginalHandoff', 'neverAccepted', 'neverPublished', 'originalInputPreserved',
    'originalFullGroupRetained', 'boundaryComplete', 'terminated', 'tracksEnded', 'contextClosed', 'outputStopped'])
    if (proof[key] !== true) return false;
  for (const key of ['completed', 'voiced', 'successfulAcquires']) if (proof[key] !== 0) return false;
  if (cleanup.browserClosed !== true || cleanup.childClosed !== true || cleanup.stderrEnded !== true ||
      cleanup.forced !== false || cleanup.signal !== null || cleanup.exitCode !== 1 ||
      cleanup.normalExit !== false) return false;
  if (diagnostics.overflow !== false || diagnostics.malformed !== false || diagnostics.stdoutEnded !== true ||
      !Array.isArray(diagnostics.records) || diagnostics.records.length !== 1) return false;
  if (diagnostics.scopeFailureObserved !== false || diagnostics.stderrMalformed !== false || diagnostics.stderrEnded !== true ||
      !Array.isArray(diagnostics.stderrRecords) || diagnostics.stderrRecords.length !== 1 ||
      diagnostics.stderrRecords[0]?.kind !== 'runtime_failed' || diagnostics.stderrRecords[0]?.code !== null ||
      !Array.isArray(diagnostics.runtimeRecords) || diagnostics.runtimeRecords.length !== 1) return false;
  const runtime = diagnostics.runtimeRecords[0], expectedRuntime = {kind:'runtimeFailureDiagnostic',runtime:'clientTick',udp:'none',pcm:'none',observation:1};
  if (!runtime || Object.keys(runtime).length !== 5 || !Object.entries(expectedRuntime).every(([key,value]) => runtime[key] === value)) return false;
  const fixed = {kind:'clientFailureDiagnostic', origin:'clientFailed', cause:'candidateEstablishDeadline',
    method:'none', observation:1};
  const value = diagnostics.records[0];
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  return Object.keys(value).length === Object.keys(fixed).length &&
    Object.entries(fixed).every(([key, expected]) => value[key] === expected);
}
// expectedExpiredTargetRetirement只分类已经原wire核验的同scope自然恢复截止；不改生产Failed或normalExit。
// allCleanup仅用于控制离线截止分支核实其他两端正常且无额外故障；旧ClientTick分类保持原调用兼容。
function expectedExpiredTargetRetirement(proof, cleanup, diagnostics, allCleanup) {
  if (!proof || !cleanup || !diagnostics || proof.variant !== 'targetRecoveryExpired' || proof.role !== 'B' ||
      proof.scenarioComplete !== true || !Number.isFinite(proof.expiredMilliseconds) ||
      proof.expiredMilliseconds < 15000 || proof.expiredMilliseconds >= 20000) return false;
  for (const key of ['sameOriginalRecoveryScopeVerified','originalDeadlinePreserved','uniqueNetworkTimeout',
    'noLateInputOrAcquire','noAutomaticCapture','reconnectAfterExpiryRejected','originalInputPreserved',
    'originalEndpointPreserved','actualCaptureStopped','actualOutputStopped','terminated','tracksEnded','contextClosed'])
    if (proof[key] !== true) return false;
  if (proof.successfulAcquires !== 1 || cleanup.role !== 'B' || cleanup.browserClosed !== true ||
      cleanup.childClosed !== true || cleanup.stderrEnded !== true || cleanup.forced !== false ||
      cleanup.signal !== null || cleanup.exitCode !== 1 || cleanup.normalExit !== false) return false;
  if (diagnostics.overflow !== false || diagnostics.malformed !== false || diagnostics.stdoutEnded !== true ||
      diagnostics.scopeFailureObserved !== false || diagnostics.stderrMalformed !== false || diagnostics.stderrEnded !== true) return false;
  const expected = [
    ['records',{kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'candidateRecoveryDeadline',method:'none',observation:1}],
    ['runtimeRecords',{kind:'runtimeFailureDiagnostic',runtime:'clientTick',udp:'none',pcm:'none',observation:1}],
    ['stderrRecords',{kind:'runtime_failed',code:null}]
  ];
  // 闭集逐字段核唯一首因；任何额外host/client/UDP/PCM/信号失败均不能被退休分类吞掉。
  const legacy = expected.every(([key,value]) => Array.isArray(diagnostics[key]) && diagnostics[key].length === 1 &&
    diagnostics[key][0] && Object.keys(diagnostics[key][0]).length === Object.keys(value).length &&
    Object.entries(value).every(([name,field]) => diagnostics[key][0][name] === field));
  if (legacy) return true;
  // 独占Runtime在离线控制恢复中先走单钟tick并立即ConnectionDeadline退出，不产生ClientTick首因副本。
  // 只接受精确三组事实与完整同scope proof，普通30秒建连失败或任意runtime_failed不能借此获准。
  // exact只比较闭集数组字段，不依赖对象键顺序，也不执行toJSON或序列化自由内容。
  const exact = (value, expected) => Array.isArray(value) && value.length === expected.length &&
    value.every((entry,index) => entry && typeof entry === 'object' && !Array.isArray(entry) &&
      Object.keys(entry).length === Object.keys(expected[index]).length &&
      Object.entries(expected[index]).every(([key,field]) => entry[key] === field));
  const current = {kind:'runtimeFailureDiagnostic',runtime:'connectionDeadline',udp:'none',pcm:'none',observation:1};
  if (Object.keys(diagnostics).length !== 9 || !exact(diagnostics.records, []) ||
      !exact(diagnostics.runtimeRecords, [current]) ||
      !exact(diagnostics.stderrRecords, [{kind:'runtime_failed',code:null}])) return false;
  if (!Array.isArray(allCleanup) || allCleanup.length !== 3 ||
      allCleanup.filter(value => value.role === 'B').length !== 1 ||
      allCleanup.find(value => value.role === 'B') !== cleanup) return false;
  const others = allCleanup.filter(value => value.role === 'A' || value.role === 'C');
  if (others.length !== 2 || new Set(others.map(value => value.role)).size !== 2) return false;
  return others.every(value => {
    const fact = value.clientFailureDiagnostics;
    return value.browserClosed === true && value.childClosed === true && value.stderrEnded === true &&
      value.normalExit === true && value.exitCode === 0 && value.signal === null && value.forced === false &&
      fact && Object.keys(fact).length === 9 && fact.overflow === false && fact.malformed === false &&
      fact.stderrMalformed === false && fact.scopeFailureObserved === false &&
      fact.stdoutEnded === true && fact.stderrEnded === true && exact(fact.records, []) &&
      exact(fact.runtimeRecords, []) && exact(fact.stderrRecords, []);
  });
}

// 文件说明：只读同一原stdout/stderr整行闭集，完整结束前不得分类；不保存自由正文或新增监听。
// createRetirementDiagnostics 要求唯一client与runtime首因并拒scope失败及未知stderr；有界缓冲溢出永远拒绝。
function createRetirementDiagnostics(){
  const records=[],runtimeRecords=[],stderrRecords=[];
  let overflow=false,malformed=false,scopeFailureObserved=false,stderrMalformed=false;
  let stdoutEnded=false,stderrEnded=false,total=0;
  const buffers={stdout:Buffer.alloc(0),stderr:Buffer.alloc(0)};
  // push 只复用原data回调；任何错误正文均即时丢弃，仅投影既有严格解析器的固定类别。
  function push(stream,bytes){
    if(stream==='stdout'?stdoutEnded:stderrEnded){malformed=true;return;}
    total+=bytes.length;if(total>8388608){overflow=true;return;}
    let all=Buffer.concat([buffers[stream],bytes]);
    for(;;){const at=all.indexOf(10);if(at<0)break;const line=all.subarray(0,at);all=all.subarray(at+1);
      if(line.length>4096){overflow=true;continue;}if(line.length===0)continue;
      if(stream==='stderr'){
        const value=deviceFailureCategory(line);
        if(!value||!/^runtime_failed state=\d+ voice=\d+\r?$/.test(line.toString('utf8')))stderrMalformed=true;
        if(value){if(stderrRecords.length>=16)overflow=true;else stderrRecords.push(value);}
      }else{
        const prefix=line.toString('utf8');let value=null,list=null;
        if(prefix.startsWith('host_scope_failure=')){scopeFailureObserved=true;continue;}
        if(prefix.startsWith('host_client_failure=')){value=clientFailureCategory(line);list=records;}
        else if(prefix.startsWith('host_failure=')){value=runtimeFailureCategory(line);list=runtimeRecords;}
        if(list){if(!value)malformed=true;else if(list.length>=16)overflow=true;else list.push(value);}
      }
    }
    if(all.length>4096){overflow=true;buffers[stream]=Buffer.alloc(0);}else buffers[stream]=Buffer.from(all);
  }
  // finish 原end事件只关闭本人流；未结束行仍拒绝，不能以cleanup关socket补原完整窗口。
  function finish(stream){if(buffers[stream].length){if(stream==='stderr')stderrMalformed=true;else malformed=true;}buffers[stream]=Buffer.alloc(0);if(stream==='stdout')stdoutEnded=true;else stderrEnded=true;}
  return {push:bytes=>push('stdout',bytes),stderr:bytes=>push('stderr',bytes),finish:()=>finish('stdout'),finishStderr:()=>finish('stderr'),facts:()=>({records:structuredClone(records),runtimeRecords:structuredClone(runtimeRecords),stderrRecords:structuredClone(stderrRecords),scopeFailureObserved,overflow,malformed,stderrMalformed,stdoutEnded,stderrEnded})};
}

// projectCaptureAckDiagnostic 只接原页面严格有界快照；不保留自由正文、身份或原时刻。
function projectCaptureAckDiagnostic(raw) {
  if (typeof raw !== 'string' || Buffer.byteLength(raw) > 8192) return null;
  try {
    let value = JSON.parse(raw.trim());
    if (typeof value === 'string') value = JSON.parse(value);
    if (value === null) return {available: false};
    const exact = (v, keys) => v && typeof v === 'object' && !Array.isArray(v) &&
      Object.keys(v).length === keys.length && keys.every(k => Object.hasOwn(v, k));
    const counters = ['received', 'filterAccepted', 'filterRejected', 'sendAccepted', 'sendRejected',
      'sendException', 'ackPosted', 'ackRejected', 'ackException', 'socketUnavailable', 'socketBackpressure'];
    if (!exact(value, ['main', 'thread', 'threadRejected']) || typeof value.threadRejected !== 'boolean' ||
        !exact(value.main, [...counters, 'overflow']) || typeof value.main.overflow !== 'boolean' ||
        counters.some(k => !Number.isInteger(value.main[k]) || value.main[k] < 0 || value.main[k] > 1000000)) return null;
    const t = value.thread;
    if (t !== null) {
      const counts = ['generated', 'applied', 'oldInputRejected', 'ownerRejected'];
      const ages = ['maximumAgeMilliseconds', 'oldestPendingAgeMilliseconds'];
      if (!exact(t, ['kind', ...counts, 'highWater', 'pending', 'overflow', 'clock', ...ages]) ||
          t.kind !== 'captureAckDiagnostic' || typeof t.overflow !== 'boolean' ||
          !['valid', 'unavailable', 'invalid'].includes(t.clock) ||
          counts.some(k => !Number.isInteger(t[k]) || t[k] < 0 || t[k] > 1000000) ||
          ['highWater', 'pending'].some(k => !Number.isInteger(t[k]) || t[k] < 0 || t[k] > 16) ||
          ages.some(k => t[k] !== null && (!Number.isFinite(t[k]) || t[k] < 0 || t[k] > 900000)) ||
          (t.clock !== 'valid' && ages.some(k => t[k] !== null)) ||
          (t.pending === 0 && t.oldestPendingAgeMilliseconds !== null)) return null;
    }
    return {available: true, main: {...value.main}, thread: t ? {...t} : null, threadRejected: value.threadRejected};
  } catch { return null; }
}

// validateEnvironment 只接隔离环境已授权的三份身份；同传输且UDP真实Worklet语料，不以替换Sender绕过完整组。
function validateEnvironment(env) {
  assert.equal(env.UR_AI_ENDPOINT_LIVE, '1');
  assert.equal(env.UR_MEDIA_TRANSPORT, 'udp');
  const recovery = ['targetRecoveryActive', 'targetRecoveryStopped', 'targetRecoveryExpired'].includes(env.UR_AI_ENDPOINT_VARIANT);
  assert.ok(['commit', 'abort', 'timeout'].includes(env.UR_AI_ENDPOINT_VARIANT) || recovery);
  if (recovery) assert.equal(env.UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC, '1');
  assert.ok(!env.UR_DEVICE_AI_ENDPOINT_LISTEN);
  const base = control.validateEnvironment({...env, UR_AI_CONTROL_LIVE: '1', UR_AI_CONTROL_VARIANT: 'active'});
  const identities = new Set(), ports = new Set();
  const devices = ['A', 'B', 'C'].map(role => {
    const childEnv = {...env};
    if (role !== 'A') {
      for (const name of ['PRODUCT', 'NAME', 'SECRET'])
        childEnv['UR_DEVICE_' + name] = env['UR_AI_ENDPOINT_' + role + '_' + name];
      childEnv.UR_BROWSER_PORT = env['UR_AI_ENDPOINT_' + role + '_PORT'];
      childEnv.UR_DEVICE_AI_ENDPOINT_LISTEN = '1';
    }
    // 新目标恢复只由实际listener/HAL声明，兼容引导A不能借目标接听开关虚报能力。
    if (recovery && role === 'A') delete childEnv.UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC;
    const settings = control.validateEnvironment({...childEnv, UR_AI_CONTROL_LIVE: '1', UR_AI_CONTROL_VARIANT: 'active'});
    // 公共目标串必须能唯一编码；认证凭据只进本机子进程，不返回报告。
    for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME'])
      assert.ok(/^[A-Za-z0-9_.-]{1,64}$/.test(childEnv[key]));
    const target = 'device:' + childEnv.UR_DEVICE_PRODUCT + ':' + childEnv.UR_DEVICE_NAME;
    assert.ok(Buffer.byteLength(target) <= 128 && !identities.has(target) && !ports.has(settings.port));
    identities.add(target); ports.add(settings.port);
    return {role, target, port: settings.port, env: childEnv};
  });
  return {...base, variant: env.UR_AI_ENDPOINT_VARIANT, devices};
}

// readEndpointFacts 只读原UI及原核心实际回执；关闭意图不能冒充音频线程false，以原轨ended和context.closed另验释放。
function readEndpointFacts() {
  return {sourceState: aiEndpointSourceState?.state || null,
    canPrepare: aiEndpointSourceState?.canPrepare === true,
    targetPhase: aiEndpointState?.phase ?? null,
    inputID: globalThis.__urAIControl.lastInput,
    focusReceipts: metrics.focusReceipts.map(value => ({...value})),
    tracksEnded: Boolean(stream && stream.getAudioTracks().length === 1 &&
      stream.getAudioTracks().every(track => track.readyState === 'ended')),
    contextClosed: context?.state === 'closed',
    prepareElapsedMs: globalThis.__urEndpointPrepareAt === undefined ? null :
      performance.now() - globalThis.__urEndpointPrepareAt};
}

// successfulFocus 只认原FocusEndpoint投影的成功水位；UI阶段和本地停流不能补造actual acquired/released。
function successfulFocus(facts, operation, after = 0) {
  assert.ok(Array.isArray(facts.focusReceipts));
  let previous = 0;
  for (const value of facts.focusReceipts) {
    assert.ok(Number.isSafeInteger(value.receiptIndex) && value.receiptIndex > previous);
    assert.ok(Number.isSafeInteger(value.revision) && value.revision >= 0);
    previous = value.receiptIndex;
  }
  return facts.focusReceipts.filter(value => value.operation === operation &&
    value.state === (operation === 'acquire' ? 'acquired' : 'released') &&
    value.code === 200 && value.receiptIndex > after);
}

// verifyCandidateReady 以原源控制器已核权的ready证明本人接听、真实UDP探测与媒体报告均完成。
// 冻结C++候选此时仍为Preparing(4)，仅ACK或本地阶段不能单独作ready；双端活且未提交才继续。
function verifyCandidateReady(source, candidate) {
  assert.equal(source.sourceState, 'ready');
  assert.equal(source.terminated, false); assert.equal(source.contextRunning, true);
  assert.equal(candidate.targetPhase, 4);
  assert.equal(candidate.terminated, false); assert.equal(candidate.contextRunning, true);
  assert.equal(candidate.captureOpen, false); assert.equal(candidate.outputStopped, true);
  assert.equal(candidate.completed, 0); assert.equal(candidate.voiced, 0);
  assert.equal(successfulFocus(candidate, 'acquire').length, 0);
  return {sourceReadyConfirmed: true, candidateHeldBeforeCommit: true};
}

// verifyPreserved 在同一原输入上核持续实际采集、无额外启停和无提前候选输出；可接听/ready不等于媒体授权。
function verifyPreserved(before, after, candidate) {
  for (const value of [before, after]) {
    assert.equal(value.captureOpen, true); assert.equal(value.terminated, false);
    assert.equal(value.contextRunning, true); assert.equal(value.outputStopped, true);
    assert.ok(Number.isSafeInteger(value.captured) && value.captured >= 0);
    assert.ok(typeof value.inputID === 'string' && value.inputID);
  }
  assert.equal(after.inputID, before.inputID);
  assert.equal(after.starts, before.starts); assert.equal(after.stops, before.stops);
  assert.ok(after.captured > before.captured);
  assert.ok(candidate.captureOpen === false || (candidate.terminated === true &&
    candidate.tracksEnded === true && candidate.contextClosed === true));
  assert.equal(candidate.outputStopped, true);
  assert.equal(candidate.completed, 0); assert.equal(candidate.voiced, 0);
  assert.equal(successfulFocus(candidate, 'acquire').length, 0);
  return {originalInputPreserved: true, actualCaptureContinued: true, candidateNotPublishing: true};
}

// verifyCommitted 区分普通兼容源和已取得完整组的源；第二跳必须原full2 acquire之后实际release，不能用进程退出代替。
function verifyCommitted(sourceBefore, sourceAfter, targetAfter, fullSource) {
  assert.equal(sourceAfter.tracksEnded, true); assert.equal(sourceAfter.contextClosed, true);
  assert.equal(sourceAfter.terminated, true); assert.equal(targetAfter.targetPhase, 7);
  assert.equal(targetAfter.captureOpen, false); assert.equal(targetAfter.terminated, false);
  assert.equal(targetAfter.contextRunning, true);
  const targetAcquired = successfulFocus(targetAfter, 'acquire');
  assert.equal(targetAcquired.length, 1);
  if (fullSource) {
    const acquired = successfulFocus(sourceBefore, 'acquire'); assert.equal(acquired.length, 1);
    const released = successfulFocus(sourceAfter, 'release', acquired[0].receiptIndex);
    assert.equal(released.length, 1); assert.ok(released[0].revision > acquired[0].revision);
  }
  return {targetActualFullGroupAcquired: true, sourceActualTracksStopped: true,
    sourceActualOutputClosed: true, sourceHadFullGroup: fullSource,
    sourceOriginalFullGroupReleaseConfirmed: fullSource};
}

// failureFacts 保留固定采播计数与接续闭集状态；原输入、设备、会话、target和任何自由文本均不可输出。
function failureFacts(value) {
  const facts = control.failureFacts(value);
  if (!facts) return null;
  if (['idle', 'pending', 'preparing', 'ready', 'committed', 'aborted', 'failed', 'unknown', 'rejected'].includes(value.sourceState))
    facts.sourceState = value.sourceState;
  if (Number.isInteger(value.targetPhase) && value.targetPhase >= 0 && value.targetPhase <= 9)
    facts.targetPhase = value.targetPhase;
  // 仅透传原页面同钟准备耗时；缺失或非法值保持未知，不另建计时起点。
  if (Number.isFinite(value.prepareElapsedMs) && value.prepareElapsedMs >= 0)
    facts.prepareElapsedMs = value.prepareElapsedMs;
  for (const key of ['tracksEnded', 'contextClosed']) if (typeof value[key] === 'boolean') facts[key] = value[key];
  if (Array.isArray(value.focusReceipts)) {
    facts.successfulAcquires = value.focusReceipts.filter(item => item?.operation === 'acquire' && item.state === 'acquired' && item.code === 200).length;
    facts.successfulReleases = value.focusReceipts.filter(item => item?.operation === 'release' && item.state === 'released' && item.code === 200).length;
  }
  return facts;
}

// executeScenario 只调正式页面用户入口；端口注入仅用于测试驱动，不能替代真实实现或建立AI会话。
async function executeScenario(settings, devices, progress = () => {}, journal = {rounds: [], transfers: []}, afterBootstrap, timeoutCheckpoint) {
  const [a, b, c] = devices, {rounds, transfers} = journal;
  const round = async (device, name, active = false, preserveInput = false) => {
    progress(name); rounds.push({round: name, ...await device.audioRound(active, preserveInput)});
  };
  const start = async device => { await device.invoke('click', '#start'); await device.wait('metrics.captureOpen === true'); };
  const prepare = async (source, target) => {
    await source.evaluate('globalThis.__urEndpointPrepareAt=performance.now();({submitted:true})');
    await source.invoke('fill', '#endpointSourceTarget', target.target);
    await source.invoke('click', '#endpointSourcePrepare');
    await source.wait('aiEndpointSourceState?.state === "preparing"');
    await target.wait('aiEndpointState?.phase === 2 && !element("endpointAccept").disabled');
  };
  const commit = async (source, target, fullSource) => {
    const prefix = fullSource ? 'full_source_' : 'bootstrap_';
    await start(source); const before = await source.facts();
    if (fullSource) assert.equal(successfulFocus(before, 'acquire').length, 1);
    progress(prefix + 'prepare');
    await prepare(source, target);
    progress(prefix + 'accept');
    await target.invoke('click', '#endpointAccept');
    progress(prefix + 'ready');
    await source.wait('aiEndpointSourceState?.state === "ready" && !element("endpointSourceCommit").disabled');
    // 原平台ready由held.accepted/reported及UDP实际Ready共同生成；不能用Preparing替代该屏障。
    await target.wait('aiEndpointState?.phase === 4 && !terminated && context?.state === "running"');
    const sourceReady = await source.facts(), targetReady = await target.facts();
    const ready = verifyCandidateReady(sourceReady, targetReady);
    const preserved = verifyPreserved(before, sourceReady, targetReady);
    // 候选已ready仍由原源实际回复一整轮，证明不能以空闲播放器冒充提交前的输出路径。
    await round(source, fullSource ? 'full_source_precommit_audio' : 'bootstrap_precommit_audio', true);
    await start(source); const atCommit = await source.facts();
    progress(prefix + 'commit');
    await source.invoke('click', '#endpointSourceCommit');
    progress(prefix + 'target_actual_focus');
    await target.wait('aiEndpointState?.phase === 7 && metrics.focusReceipts.some(x=>x.operation === "acquire" && x.code === 200)');
    progress(prefix + 'source_actual_stop');
    await source.wait('terminated && context?.state === "closed" && stream.getAudioTracks().every(x=>x.readyState === "ended")' +
      (fullSource ? ' && metrics.focusReceipts.some(x=>x.operation === "release" && x.code === 200)' : ''));
    const settled = verifyCommitted(atCommit, await source.facts(), await target.facts(), fullSource);
    transfers.push({...ready, ...preserved, ...settled});
    await round(target, fullSource ? 'target_committed_audio' : 'bootstrap_target_audio');
  };
  await round(a, 'initial_source_audio');
  await commit(a, b, false);
  if (settings.variant.startsWith('targetRecovery')) {
    assert.equal(typeof afterBootstrap, 'function');
    await afterBootstrap(settings, devices, progress, journal);
    return journal;
  }
  if (settings.variant === 'commit') await commit(b, c, true);
  else {
    await start(b); const before = await b.facts();
    assert.equal(successfulFocus(before, 'acquire').length, 1);
    progress('full_source_' + settings.variant); await prepare(b, c);
    if (settings.variant === 'abort') {
      await b.invoke('click', '#endpointSourceAbort');
      await b.wait('aiEndpointSourceState?.state === "aborted"');
    } else {
      // 原30秒静音idle与候选截止独立；同输入的固定语音活动不延候选期限，也不制造Stop/Start。
      await round(b, 'timeout_wait_source_audio', true, true);
      progress('full_source_timeout');
      // 原preparing/ready自然截止的终态为aborted；failed用于waitingModel或Owner失效，不能混为正常保留源。
      await b.wait('aiEndpointSourceState?.state === "aborted" && performance.now()-globalThis.__urEndpointPrepareAt >= 30000');
    }
    const after = await b.facts();
    assert.equal(after.sourceState, 'aborted');
    const candidateAtAbort = await c.facts();
    const preserved = verifyPreserved(before, after, candidateAtAbort);
    assert.equal(successfulFocus(after, 'release').length, 0);
    if (settings.variant === 'timeout') assert.ok(after.prepareElapsedMs >= 30000 && after.prepareElapsedMs < 45000);
    transfers.push({...preserved, sourceHadFullGroup: true, originalFullGroupRetained: true,
      timeoutThirtySecondsVerified: settings.variant === 'timeout'});
    await round(b, 'retained_source_audio', true);
    if (settings.variant === 'timeout') {
      assert.equal(typeof timeoutCheckpoint, 'function', 'timeout_original_wire_checkpoint_required');
      const wire = await timeoutCheckpoint(journal);
      assert.deepEqual(Object.keys(wire).sort(), ['boundaryComplete','neverAccepted','neverPublished','sameOriginalHandoff'].sort());
      assert.ok(Object.values(wire).every(value => value === true));
      journal.timeoutRetirementBeforeCleanup = {variant:'timeout', role:'C', scenarioComplete:true, prepareElapsedMs:after.prepareElapsedMs,
        originalInputPreserved:preserved.originalInputPreserved, originalFullGroupRetained:true, ...failureFacts(await c.facts()), sourceState:after.sourceState, ...wire};
    }
  }
  return journal;
}

// createDevice 把身份留在独立native进程；页面只做本人操作，固定observer不返回自由JSON/供应商正文。
function createDevice(spec, settings, deps) {
  const session = 'ai-endpoint-' + spec.role + '-' + randomUUID();
  const child = (deps.spawn || spawn)(spec.env.UR_DEVICE_BROWSER_BIN, [String(spec.port)],
    {env: spec.env, stdio: ['ignore', 'pipe', 'pipe']});
  const observer = createDeviceFailureObserver();
  const state = {closed: false, code: null, signal: null, stderrEnded: false};
  const retirementDiagnostic = createRetirementDiagnostics();
  const completion = new Promise(resolve => child.once('close', (code, signal) => { state.closed = true; state.code = code; state.signal = signal; resolve(); }));
  child.stderr.on('data', bytes => {observer.push(bytes);retirementDiagnostic.stderr(bytes);});
  child.stderr.once('end', () => { state.stderrEnded = true; observer.finish(); retirementDiagnostic.finishStderr(); });
  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error('device_start_timeout')), 10000);
    child.once('error', () => { clearTimeout(timer); reject(Error('device_spawn_failed')); });
    child.once('close', () => { clearTimeout(timer); reject(Error('device_start_failed')); });
    child.stdout.on('data', bytes => {
      retirementDiagnostic.push(bytes);
      if (bytes.toString().includes('http://127.0.0.1:' + spec.port + '/')) { clearTimeout(timer); resolve(); }
    });
  });
  // ready在稍后逐端await前也必须接住拒绝；原错误仍由initialize抛出，不制造通过。
  ready.catch(() => {});
  child.stdout.once('end', () => retirementDiagnostic.finish());
  const invoke = (...args) => new Promise((resolve, reject) => {
    (deps.execFile || execFile)('agent-browser', ['--session', session, ...args],
      {env: {...spec.env, AGENT_BROWSER_ARGS: [spec.env.AGENT_BROWSER_ARGS || '--no-sandbox',
        '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'].join(',')},
      timeout: args[0] === 'wait' ? 65000 : 25000, maxBuffer: 65536},
      (error, stdout) => error ? reject(Error('browser_command_failed')) : resolve(stdout.trim()));
  });
  const evaluate = async expression => parseJSON(await invoke('eval', expression));
  const wait = expression => invoke('wait', '--fn', expression);
  const facts = async () => ({...await evaluate('(' + control.readFacts.toString() + ')()'),
    ...await evaluate('(' + readEndpointFacts.toString() + ')()')});
  return {role:spec.role, target: spec.target, invoke, evaluate, wait, facts,
    // readCaptureAckDiagnostic 仅原开关下在cleanup前读本人页面一次，不另起消费者或业务请求。
    async readCaptureAckDiagnostic() {
      if (spec.env.UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS !== '1') return null;
      const expression = 'typeof globalThis.UrReadCaptureAckDiagnostic === "function" ? globalThis.UrReadCaptureAckDiagnostic() : null';
      return new Promise(resolve => {
        const failed = () => resolve({role: spec.role, probe: 'commandFailed', fact: null});
        try {
          (deps.execFile || execFile)('agent-browser', ['--session', session, 'eval', expression],
            {env: {...spec.env, AGENT_BROWSER_ARGS: [spec.env.AGENT_BROWSER_ARGS || '--no-sandbox',
              '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'].join(',')}, timeout: 3000, maxBuffer: 8192},
            (error, stdout) => {
              if (error) return failed();
              const fact = projectCaptureAckDiagnostic(stdout);
              resolve({role: spec.role, probe: fact ? 'observed' : 'invalid', fact});
            });
        } catch { failed(); }
      });
    },
    async initialize() {
      await ready; await invoke('open', 'http://127.0.0.1:' + spec.port + '/');
      await invoke('eval', '(' + control.installObserver.toString() + ')()');
      await invoke('eval', 'globalThis.__urAIControl.keywords=' + JSON.stringify(settings.keywords));
      await invoke('click', '#activate');
      await wait(spec.role === 'A' ? 'aiControlState?.state === "ready" && aiEndpointSourceState?.canPrepare && !element("start").disabled' :
        'aiEndpointState?.phase === 1 && context?.state === "running"');
      await invoke('eval', 'globalThis.__urFixtureChunks=[]');
      const encoded = settings.bytes.toString('base64');
      for (let offset = 0; offset < encoded.length; offset += 24000)
        await invoke('eval', 'globalThis.__urFixtureChunks.push(' + JSON.stringify(encoded.slice(offset, offset + 24000)) + ')');
      await invoke('eval', '(' + control.installFixture.toString() + ')()');
    },
    async audioRound(active, preserveInput = false) {
      // 仅既有活动输入允许保持；新输入不能借此选项跳过原Start或停流事实。
      assert.ok(!preserveInput || active);
      const before = await facts();
      if (!active) await invoke('click', '#start');
      await wait('metrics.captureOpen === true'); await invoke('eval', 'globalThis.__urControlPlay()');
      await wait('globalThis.__urAIControl.fixtureEnded === ' + (before.fixtureEnded + 1));
      if (!preserveInput) await invoke('click', '#stop');
      await wait('metrics.completedResponses === ' + (before.completed + 1) +
        ' && responseId === "" && completedResponseId !== "" && metrics.lastPlayableConsumedResponseId === completedResponseId');
      return control.verifyAudio(before, await facts(), 'udp');
    },
    async cleanup() {
      // 正常本人结束仍走同一UI；已退休源不能重新发送end或补造控制。
      if (!state.closed) {
        await invoke('eval', '!terminated && !element("end").disabled ? (element("end").click(),true) : true').catch(() => {});
        await waitClosed(completion, deps.closeTimeoutMs || 8000);
      }
      const browserClosed = await invoke('close').then(() => true, () => false);
      let forced = false;
      if (!state.closed) {
        forced = true; try { child.kill('SIGTERM'); } catch {}
        if (!await waitClosed(completion, deps.closeTimeoutMs || 8000)) {
          try { child.kill('SIGKILL'); } catch {}
          await waitClosed(completion, deps.closeTimeoutMs || 8000);
        }
      }
      return {browserClosed, childClosed: state.closed, stderrEnded: state.stderrEnded,
        normalExit: state.closed && state.code === 0 && !forced, exitCode:state.code, signal:state.signal, forced, role:spec.role, clientFailureDiagnostics:retirementDiagnostic.facts()};
    },
    diagnostic() { return {deviceFailure: state.stderrEnded ? observer.finish() : null, deviceExitCode: state.code}; }
  };
}

// run 只有三端实际场景与正常资源退出均成立才PASS；部署、模型配置和冻结产物均由调用方准备。
async function run(env = process.env, deps = {}) {
  const settings = validateEnvironment(env), devices = [];
  let phase = 'device_start', primary = null, success = false;
  let observedFacts = null;
  const result = {rounds: [], transfers: []}; // 失败也保留已验收轮次，不用引导成功替代第二跳。
  const binarySHA = createHash('sha256').update(fs.readFileSync(env.UR_DEVICE_BROWSER_BIN)).digest('hex');
  try {
    for (const spec of settings.devices) {
      const device = createDevice(spec, settings, deps); devices.push(device);
      phase = 'initialize_' + spec.role; await device.initialize();
    }
    await executeScenario(settings, devices, value => { phase = value; }, result, deps.afterBootstrap, deps.timeoutCandidateWitness);
    success = true;
  } catch (error) {
    primary = {phase, reason: control.failureReason(error)};
    observedFacts = [];
    for (const device of devices) observedFacts.push(await device.facts().then(failureFacts).catch(() => null));
  }
  // 首因及场景结果已确定后、任何cleanup动作前读取；采证失败不替换业务结果或延原媒体窗口。
  if (env.UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS === '1') {
    result.captureAckDiagnostics = [];
    for (const device of devices) result.captureAckDiagnostics.push(await device.readCaptureAckDiagnostic());
  }
  const cleanup = [];
  for (const device of devices.slice().reverse()) cleanup.push(await device.cleanup());
  for (const value of cleanup) {
    value.expectedCandidateRetirement = settings.variant === 'timeout' && value.role === 'C' && expectedCandidateRetirement(result.timeoutRetirementBeforeCleanup, value, value.clientFailureDiagnostics);
    value.expectedExpiredTargetRetirement = settings.variant === 'targetRecoveryExpired' && value.role === 'B' && expectedExpiredTargetRetirement(result.expiredRetirementBeforeCleanup, value, value.clientFailureDiagnostics, cleanup);
  }
  const passed = success && cleanup.length === 3 &&
    cleanup.every(value => value.browserClosed && value.childClosed && value.stderrEnded && (value.normalExit || value.expectedCandidateRetirement || value.expectedExpiredTargetRetirement));
  const report = {result: passed ? 'pass' : 'fail', phase: passed ? 'completed' : phase,
    variant: settings.variant, transport: 'udp', configuredExecutionChain: settings.chain,
    binarySHA, corpusSHA: createHash('sha256').update(settings.bytes).digest('hex'),
    ...result, cleanup, ...(!passed ? {diagnostic: {primary: primary || {phase: 'cleanup', reason: 'cleanup_failed'},
      facts: observedFacts, devices: devices.map(device => device.diagnostic())}} : {})};
  (deps.report || console.log)(JSON.stringify(report));
  if (!passed) throw Error('ai_endpoint_live_failed');
  return report;
}
module.exports = {validateEnvironment, readEndpointFacts, successfulFocus, verifyCandidateReady, verifyPreserved, failureFacts,
  verifyCommitted, executeScenario, createDevice, projectCaptureAckDiagnostic, run, expectedCandidateRetirement, expectedExpiredTargetRetirement, createRetirementDiagnostics};
if (require.main === module) run().catch(() => { console.error('ai_endpoint_live_failed'); process.exitCode = 1; });
