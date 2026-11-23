// 文件说明：真实模型→正式MCP→IM→C++桌面文件播放验收；不发送MQTT/RPC、不注入媒体事实。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn, execFile} = require('node:child_process');
const {randomUUID, createHash} = require('node:crypto');

// endSessionViaUI 将文字输入导致滚出视口的原结束按钮先滚回，再执行原CLI点击；不裸发控制、不延长结束预算或补重试。
// invoke 为原场景的变参浏览器入口；滚动失败原样向上传播，不能据CLI退出码制造实际结束事实。
async function endSessionViaUI(invoke) {
  await invoke('scrollintoview', '#end');
  await invoke('click', '#end');
}

// steps 固定分轮顺序；停止前释放完整组，不能与Owner自动结束竞速补发release。
const steps = ['invite', 'load', 'suspendAi', 'acquireFile', 'play', 'pause',
  'suspendFile', 'restoreAi', 'releaseFile', 'stop'];
// tools 仅核对正式工具事件，结果返回不等于媒体操作实际成功。
const tools = {initial: 'control_output_focus', invite: 'invite_media_playback',
  load: 'control_media_playback', suspendAi: 'control_output_focus',
  acquireFile: 'control_output_focus', play: 'control_media_playback',
  pause: 'control_media_playback', suspendFile: 'control_output_focus',
  restoreAi: 'control_output_focus', releaseFile: 'control_output_focus', stop: 'control_media_playback'};

// exact 拒绝计划中的身份/凭据/自由协议字段，计划只是有限用户输入语料。
function exact(value, keys) {
  assert.ok(value && typeof value === 'object' && !Array.isArray(value), 'invalid_plan_object');
  assert.deepEqual(Object.keys(value).sort(), keys.slice().sort(), 'invalid_plan_fields');
}
// input 校验有限文字或WAV路径及识别关键词；不以文本替代实时语音持续上行。
function input(value, chain, requireVoice = false) {
  const voice = Object.hasOwn(value || {}, 'wav');
  exact(value, [voice ? 'wav' : 'text', 'keywords']);
  assert.ok(Array.isArray(value.keywords) && value.keywords.length > 0 && value.keywords.length <= 8 &&
    value.keywords.every(word => typeof word === 'string' && word.length > 0 && word.length <= 40 &&
      !/[\x00-\x1f\x7f]/.test(word)), 'invalid_input_keywords');
  if (voice) assert.ok(typeof value.wav === 'string' && path.isAbsolute(value.wav), 'invalid_wav_path');
  else {
    assert.equal(chain, 'cascaded_agentruntime', 'native_test_requires_voice');
    assert.ok(!requireVoice && typeof value.text === 'string' && value.text.length > 0 &&
      value.text.length <= 1000 && !/[\x00-\x1f\x7f]/.test(value.text), 'invalid_text_input');
  }
}
// validatePlan 固定两个节目及每个分轮动作；原父/操作token/焦点权限由正式后端决定。
function validatePlan(plan) {
  exact(plan, ['chain', 'initial', 'programs']);
  assert.ok(['realtime_native', 'cascaded_agentruntime'].includes(plan.chain), 'invalid_execution_chain');
  input(plan.initial, plan.chain);
  assert.ok(Array.isArray(plan.programs) && plan.programs.length === 2, 'music_and_video_required');
  for (const [index, program] of plan.programs.entries()) {
    exact(program, ['kind', 'contentRef', 'inputs']);
    assert.equal(program.kind, index === 0 ? 'music' : 'video');
    assert.match(program.contentRef, /^[A-Za-z0-9_.:-]{1,128}$/, 'invalid_content_reference');
    exact(program.inputs, steps);
    for (const step of steps) input(program.inputs[step], plan.chain, step === 'pause');
  }
  assert.notEqual(plan.programs[0].contentRef, plan.programs[1].contentRef);
  return plan;
}

// installObserver 只包裹既有UI入口，记录已发生的真实事实；原send/incoming只调用一次且返回不变。
function installObserver() {
  if (globalThis.__urMcpObserver) throw Error('observer_already_installed');
  const observed = {tools: [], gates: [], capturedWhileFilePlaying: 0, recognition: false,
    keywords: [], captureOpen: false, overflow: false,
    toolDiagnostic: {startedCount: 0, pendingCount: 0, orphanResultCount: 0, terminalCount: 0,
      sequence: 0, firstTerminalSequence: null, events: [], observationTruncated: false}};
  const calls = new Map();
  // observeTool 只记录原UI已接纳的工具/终态局部顺序；诊断截断不改变原验收overflow或完成事实。
  const observeTool = kind => {
    const diagnostic = observed.toolDiagnostic;
    diagnostic.sequence = Math.min(1000000, diagnostic.sequence + 1);
    const field = {started: 'startedCount', orphan_result: 'orphanResultCount',
      ended: 'terminalCount', error: 'terminalCount'}[kind];
    if (field) diagnostic[field] = Math.min(1000000, diagnostic[field] + 1);
    if (['ended', 'error'].includes(kind) && diagnostic.firstTerminalSequence === null)
      diagnostic.firstTerminalSequence = diagnostic.sequence;
    if (diagnostic.events.length === 64) {diagnostic.events.shift(); diagnostic.observationTruncated = true;}
    diagnostic.events.push({sequence: diagnostic.sequence, kind});
    if (diagnostic.sequence === 1000000) diagnostic.observationTruncated = true;
    diagnostic.pendingCount = calls.size;
  };
  globalThis.__urMcpObserver = observed;
  const originalIncoming = incoming;
  incoming = function(value) {
    const result = originalIncoming(value);
    if (value.kind === 'toolStatus') {
      if (value.state === 'started') {
        if (calls.size >= 128) observed.overflow = true;
        else {
          const known = ['control_output_focus', 'get_output_focus_status', 'invite_media_playback',
            'control_media_playback', 'get_media_playback_status', 'recover_media_playback_operation'];
          // 串联链保留正式裸名，原生链使用服务全名；任意其它服务同后缀不属于本工具。
          const name = known.find(tool => value.name === tool || value.name === 'things_iot_mcp.' + tool);
          calls.set(value.id, name ? 'things_iot_mcp.' + name : 'other_authorized_tool');
        }
        observeTool('started');
      } else if (calls.has(value.id)) {
        if (observed.tools.length >= 128) observed.overflow = true;
        else observed.tools.push({name: calls.get(value.id), state: value.state});
        calls.delete(value.id);
        observeTool('result');
      } else if (['succeeded', 'failed', 'unknown'].includes(value.state)) {
        observeTool('orphan_result');
      }
    } else if (value.kind === 'text' && value.text.startsWith('\n用户：')) {
      const recognized = value.text.slice(4).split('\n助手：')[0];
      observed.recognition = observed.keywords.every(word => recognized.includes(word));
    } else if (value.kind === 'captureStart') observed.captureOpen = true;
    else if (value.kind === 'captureStop') observed.captureOpen = false;
    else if (['ended', 'error'].includes(value.kind)) observeTool(value.kind);
    return result;
  };
  const observeSend = (value, sent) => {
    if (!sent) return sent;
    if (value.kind === 'captured' && desktopFiles?.player?.state === 'playing')
      ++observed.capturedWhileFilePlaying;
    if (value.kind === 'fileLaneGated') {
      if (observed.gates.length >= 128) observed.overflow = true;
      else observed.gates.push({lane: value.lane, enabled: value.enabled});
    }
    return sent;
  };
  // activate先构造宿主及初始lane，均可能保存旧send引用；同原函数共享一层透明观察，绝不重发或双计。
  const wrapped = new Map();
  const wrap = original => {
    if (!wrapped.has(original)) wrapped.set(original, function(value) {return observeSend(value, original.call(this, value));});
    return wrapped.get(original);
  };
  send = wrap(send);
  for (const target of [desktopFiles, ...(desktopFiles.lanes?.values() || []), desktopFiles.player])
    if (typeof target?.send === 'function') target.send = wrap(target.send);
  // 静音源保持真实Worklet采集通道，不以循环语料自动制造重复模型轮次。
  source.disconnect();
  const silence = context.createConstantSource();
  silence.offset.value = 0;
  silence.connect(worklet);
  silence.start();
  globalThis.__urMcpSilence = silence;
  return true;
}

// installRoundObserver 为文件21步和Live27步的专用单意图评估关联原可信回复；不改设备策略，身份只保留在有界闭包。
// 双链均安装：串联链失败轮也需要识别匹配/回复绑定/工具计数三闭环旁证定论；判据核验仍仅全双工执行。
function installRoundObserver(observeReply = false) {
  if (globalThis.__urMcpRound) throw Error('round_observer_already_installed');
  const previous = new Set(), pending = new Map(); // 原回复和工具关联有界保存，不进入快照或stdout。
  let keywords = [], bound = '', candidate = '', armed = false, armIndex = 0, terminal = false,
    recognized = false, invalid = false, started = 0, ended = 0; // 本轮内部关联与固定状态计数。
  // validId 与桥一致按UTF8字节限128并拒绝孤立surrogate；不修改或规范化原关联身份。
  const validId = value => {
    if (typeof value !== 'string' || !value || value.length > 128 || /[\x00-\x1f\x7f]/.test(value)) return false;
    let bytes = 0; // 显式计算UTF8长度，浏览器内联观察器不依赖外部辅助模块。
    for (const character of value) {
      const point = character.codePointAt(0); // 有效代理对按同一个码点计数，孤立代理项没有可信UTF8表示。
      if (point >= 0xd800 && point <= 0xdfff) return false;
      bytes += point <= 0x7f ? 1 : point <= 0x7ff ? 2 : point <= 0xffff ? 3 : 4;
      if (bytes > 128) return false;
    }
    return true;
  };
  let diagnosticWords = [], diagnosticBound = '', diagnosticInvalid = false, diagnosticArms = 0; // 诊断独立于工具终态，只保存本轮可信ASR关联。
  const diagnosticPrevious = new Set(); // 最多64个原回复，仅在页面闭包中拒绝前轮迟到ASR，不输出身份。
  // diagnosticArm只标记一次原输入，不能替原业务arm通过；超过64轮后不续桶也不沿用旧回复。
  const diagnosticArm = value => {
    if (diagnosticBound) diagnosticPrevious.add(diagnosticBound);
    diagnosticBound = ''; diagnosticWords = []; diagnosticInvalid = false;
    if (diagnosticArms >= 64 || !Array.isArray(value) || value.length < 1 || value.length > 8 ||
        value.some(word => typeof word !== 'string' || !word || word.length > 40 || /[\x00-\x1f\x7f]/.test(word))) return false;
    ++diagnosticArms; diagnosticWords = value.slice(); return true;
  };
  // facts 只输出当前轮固定布尔与计数；旧ID、关键词、工具ID及原正文从不序列化。
  const facts = () => ({armed, armIndex, responseBound: !!bound, recognitionMatched: recognized,
    toolStartedCount: started, toolTerminalCount: ended, pendingCount: pending.size,
    associationInvalid: invalid, networkTerminal: terminal,
    roundTerminal: armed && !!bound && recognized && terminal && started > 0 && ended > 0 &&
      pending.size === 0 && !invalid});
  // arm 每次显式输入仅开启一次，原轮未完成或超出固定预算就拒绝，不续期、不补发输入。
  const arm = value => {
    if ((armed && !facts().roundTerminal) || armIndex >= 64 || !Array.isArray(value) ||
        value.length < 1 || value.length > 8 || value.some(word => typeof word !== 'string' ||
          !word || word.length > 40 || /[\x00-\x1f\x7f]/.test(word))) throw Error('invalid_round_arm');
    keywords = value.slice(); bound = candidate = ''; armed = true; ++armIndex;
    terminal = recognized = invalid = false; started = ended = 0; pending.clear();
    return true;
  };
  // 显式诊断才提供闭包内可信ASR回复；只供Node哈希，不能据候选工具或旧轮补造绑定。
  globalThis.__urMcpRound = Object.freeze({arm, facts, ...(observeReply === true ? {
    diagnosticArm,diagnosticReply: () => !diagnosticInvalid ? diagnosticBound : ''} : {})});
  // acceptToolReply 预ASR仅暂存一个可信工具回复；旧已绑定回复不成为候选，多候选保守失败而不猜归属。
  const acceptToolReply = id => {
    if (!validId(id)) {invalid = true; return false;}
    if (previous.has(id) && id !== bound) return false;
    if ((bound && id !== bound) || (candidate && id !== candidate)) {invalid = true; return false;}
    candidate = id;
    return true;
  };
  const originalIncoming = incoming; // 先让既有UI及场景观察器接纳，异常与返回值不被诊断替换。
  incoming = function(value) {
    const result = originalIncoming(value);
    if (observeReply === true && diagnosticWords.length && value.kind === 'text' &&
        typeof value.text === 'string' && value.text.startsWith('\n用户：') && validId(value.responseId)) {
      const recognizedText = value.text.slice(4).split('\n助手：')[0]; // 不保留正文，只核本轮冻结关键词。
      if (diagnosticWords.every(word => recognizedText.includes(word))) {
        if (diagnosticBound && diagnosticBound !== value.responseId) diagnosticInvalid = true;
        else if (!diagnosticPrevious.has(value.responseId)) diagnosticBound = value.responseId;
      }
    }
    if (!armed) return result;
    if (value.kind === 'text' && typeof value.text === 'string' && value.text.startsWith('\n用户：')) {
      const text = value.text.slice(4).split('\n助手：')[0]; // 临时读取原ASR，只核冻结关键词，不保存正文。
      if (!bound && validId(value.responseId) && !previous.has(value.responseId) &&
          keywords.every(word => text.includes(word))) {
        if (candidate && candidate !== value.responseId) invalid = true;
        bound = value.responseId; recognized = true; previous.add(bound);
      }
    } else if (value.kind === 'toolStatus') {
      if (!validId(value.id)) invalid = true;
      else if (!acceptToolReply(value.responseId)) return result;
      else if (value.state === 'started') {
        started = Math.min(129, started + 1);
        if (pending.has(value.id) || pending.size >= 128) invalid = true;
        else pending.set(value.id, true);
      } else if (['succeeded', 'failed', 'unknown'].includes(value.state)) {
        ended = Math.min(129, ended + 1);
        if (!pending.delete(value.id)) invalid = true;
      }
    } else if (value.kind === 'responseCompleted' && validId(value.id)) {
      if (bound && value.id === bound) terminal = true;
      // 无工具候选的孤立旧终态不能开一轮；预ASR已存在工具候选时，终态只属于该候选。
      else if (!bound && candidate && !previous.has(value.id)) {
        if (value.id !== candidate) invalid = true;
        else terminal = true;
      }
    }
    return result;
  };
  return true;
}

// verifyRoundFacts 按已核验业务资格的工具数量检查同回复完整终态；默认单工具保留Live原合同。
function verifyRoundFacts(value, expectedToolCount = 1) {
  assert.ok(expectedToolCount === 1 || expectedToolCount === 2, 'invalid_verified_tool_count');
  assert.ok(value?.armed === true && Number.isSafeInteger(value.armIndex) && value.armIndex > 0 && value.armIndex <= 64,
    'round_not_armed');
  assert.equal(value.responseBound, true, 'trusted_round_response_missing');
  assert.equal(value.recognitionMatched, true, 'round_recognition_missing');
  assert.equal(value.associationInvalid, false, 'round_tool_response_unknown');
  assert.equal(value.toolStartedCount, expectedToolCount, 'round_tool_missing_or_repeated');
  assert.equal(value.toolTerminalCount, expectedToolCount, 'round_tool_missing_or_repeated');
  assert.equal(value.pendingCount, 0, 'round_tool_pending');
  assert.equal(value.networkTerminal, true, 'original_round_network_terminal_missing');
  assert.equal(value.roundTerminal, true, 'original_round_incomplete');
}

// waitFixtureNetworkRound 仅专用全双工单意图评估等待原当前回复网络终态；持续采集、插话及在播队列不受控制。
async function waitFixtureNetworkRound(chain, wait) {
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(chain));
  if (chain === 'realtime_native') await wait('__urMcpRound.facts().roundTerminal === true');
}

// readSnapshot 白名单只读观察；不读取源URL、设备身份、令牌或模型正文。
function readSnapshot() {
  const observer = globalThis.__urMcpObserver;
  const player = desktopFiles?.player;
  const media = player?.media;
  let pixels = false;
  if (media?.tagName === 'VIDEO' && media.videoWidth > 0 && !media.hidden) {
    const canvas = document.createElement('canvas');
    canvas.width = 16; canvas.height = 16;
    const drawing = canvas.getContext('2d');
    drawing.drawImage(media, 0, 0, 16, 16);
    const data = drawing.getImageData(0, 0, 16, 16).data;
    pixels = data.some((value, index) => index % 4 !== 3 && value > 8);
  }
  return {tools: observer.tools.slice(), gates: observer.gates.slice(),
    toolDiagnostic: {...observer.toolDiagnostic, events: observer.toolDiagnostic.events.map(value => ({...value}))},
    focusReceipts: (metrics.focusReceipts || []).map(value => ({state: value.state,
      revision: value.revision, operation: value.operation, code: value.code,
      receiptIndex: value.receiptIndex})), focusReceiptIndex: metrics.focusReceiptIndex,
    overlap: observer.capturedWhileFilePlaying, recognition: observer.recognition,
    captureOpen: observer.captureOpen, overflow: observer.overflow,
    captured: metrics.captured, consumedNonSilent: metrics.consumedNonSilent,
    mode: metrics.interactionMode, contextState: context?.state, streamActive: stream?.active,
    // 原控制代次和执行中旗标识别未加载state下的在途load，不读取媒体地址或动态绑定身份。
    file: player ? {state: player.state, generation: player.generation, busy: player.busy,
      samples: player.samples, energy: player.energy,
      frames: player.frames, outputEnabled: player.outputEnabled, paused: media?.paused,
      pixels, kind: media?.tagName === 'VIDEO' ? 'video' : 'music'} : null};
}

// readStartupFacts 不依赖媒体观察器已安装，仅读取初始化状态白名单；UI自由文案不出浏览器。
// audioFailureRequested仅允许boolean true追加一次纯首因旁证，默认返回原九字段；缺失或非法旁证为null。
function readStartupFacts(audioFailureRequested = false) {
  const activate = document.getElementById('activate'), start = document.getElementById('start');
  const text = document.getElementById('status')?.textContent || '';
  const phases = [['本地配对失败', 'local_pairing'], ['麦克风授权失败', 'microphone_permission'],
    ['音频上下文失败', 'audio_context'], ['音频处理器加载失败', 'audio_worklet'],
    ['音频上下文启动失败', 'audio_context_start'], ['WebRTC音频端点失败', 'webrtc_audio_endpoint']];
  // 仅识别原release的完整固定文案；context/socket已关闭本身不能证明触发来源。
  const closures = [
    ['WebRTC远端音轨无法启动，已释放设备资源。', 'webrtc_sink_start'],
    ['WebRTC候选协商失败，已释放设备媒体。', 'webrtc_candidates'],
    ['WebRTC应答失败，已释放设备媒体。', 'webrtc_answer'],
    ['会话已结束，浏览器媒体已释放。', 'session_ended'],
    ['会话已结束，麦克风已释放。', 'session_ended'],
    ['本地连接不可用或发送积压；请重新启动设备进程。', 'local_backpressure'],
    ['设备链路失败，麦克风已释放。', 'device_link_error'],
    ['本机连接已关闭，麦克风已释放。', 'local_socket_closed'],
    ['本机连接故障，麦克风已释放。', 'local_socket_error'],
    ['本地媒体报文非法，已停止采集与播放。', 'local_media_invalid'],
    ['麦克风不可用，已结束本地连接。', 'microphone_ended'],
    ['WebRTC媒体连接中断，已释放设备资源。', 'webrtc_disconnected'],
    ['浏览器音频消费事实无效，已释放媒体。', 'audio_fact_invalid'],
    ['浏览器音频驱动失败，已释放麦克风。', 'audio_driver_error'],
    ['文件媒体驱动失败，已释放设备资源。', 'file_driver_error'],
    ['页面已关闭。', 'page_closed'],
  ];
  const state = typeof context === 'undefined' ? null : context?.state;
  const facts = {pageElementsPresent: Boolean(activate && start), activateDisabled: activate?.disabled === true,
    startEnabled: start?.disabled === false, observerInstalled: Boolean(globalThis.__urMcpObserver),
    contextState: ['running', 'suspended', 'closed'].includes(state) ? state : null,
    streamActive: typeof stream === 'undefined' ? null : stream?.active === true,
    socketOpen: typeof socket === 'undefined' ? null : socket?.readyState === 1,
    initializationFailure: phases.find(([prefix]) => text.startsWith(prefix))?.[1] || null,
    closureReason: closures.find(([original]) => text === original)?.[1] || null};
  // 原默认九字段不变；仅原catch显式请求时读取纯getter一次，不调用媒体或协议控制。
  if (audioFailureRequested === true) {
    facts.audioFailure = null; // getter缺失、异常或畸形保持未知，不将null解释为没有故障。
    try {
      const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'UrReadAudioFailureDiagnostic'); // 只读函数数据属性，拒绝执行任意property getter。
      if (descriptor && Object.hasOwn(descriptor, 'value') && typeof descriptor.value === 'function') {
        const value = descriptor.value.call(globalThis); // 同一失败前窗口仅调用原纯getter一次。
        const causes = ['configurationInvalid', 'commandInvalid', 'unknownCommand', 'captureStartInvalid',
          'zeroPendingAck', 'captureSampleInvalid', 'captureBacklogFull', 'playbackStartInvalid',
          'playbackFrameInvalid', 'playbackQueueFull', 'playbackGateInvalid', 'playbackGateConflict',
          'playbackGatePending', 'ownershipGateInvalid', 'ownershipTicketConflict', 'ownershipGatePending',
          'ownershipOwnerConflict', 'ownershipRouteCapacity']; // Root审核的十八固定首因，不采自由错误或身份。
        // 只核纯对象自有数据字段；不执行返回对象accessor，也不冒称验证了原JSON重复键。
        if (value && typeof value === 'object' && !Array.isArray(value) &&
            Reflect.ownKeys(value).length === 2) {
          const known = Object.getOwnPropertyDescriptor(value, 'known'); // 严格布尔自有数据事实。
          const cause = Object.getOwnPropertyDescriptor(value, 'cause'); // 固定首因枚举自有数据事实。
          if (known && cause && Object.hasOwn(known, 'value') && Object.hasOwn(cause, 'value') &&
              typeof known.value === 'boolean' &&
              ((!known.value && cause.value === 'unknown') || (known.value && causes.includes(cause.value))))
            facts.audioFailure = {known: known.value, cause: cause.value};
        }
      }
    } catch { /* 旁证异常不能覆盖原业务失败，也不保存自由错误正文。 */ }
  }
  return facts;
}

// startupDiagnostic 仅保留固定失败分类和只读事实，原错误自由正文与设备身份不进入报告。
function startupDiagnostic(phase, error, uiFacts, deviceFailure) {
  const allowed = ['device_start_timeout', 'device_spawn_failed', 'browser_command_failed', 'browser_timeout',
    'browser_launch_failed', 'browser_connection_failed'];
  return {phase, failureKind: allowed.includes(error?.message) ? error.message : 'unclassified_failure',
    uiFacts, deviceFailure};
}

// fileStartupDiagnosticRequested 默认不装观察器；只允许文件场景显式开启固定只读旁证。
function fileStartupDiagnosticRequested(value, scenarioName) {
  if (value === undefined || value === '') return false;
  assert.ok(value === '1' && scenarioName === 'file', 'invalid_file_startup_diagnostic');
  return true;
}

// readFileStartupFacts 再投影原观察器固定枚举，防止自由页面字段进入正式失败报告。
function readFileStartupFacts() {
  const original = globalThis.fileStartupObservation;
  if (!Array.isArray(original)) return {available: false, truncated: false, events: []};
  const phases = ['initializeBegin', 'initializeSettled', 'audioGateBegin', 'audioGateSettled',
    'gateBegin', 'gateSettled', 'paintBegin', 'paintSettled', 'paintCancelCalled',
    'rafScheduled', 'rafActual', 'rafCancelled', 'audioActual'];
  const events = original.slice(0, 96).map(raw => {
    const value = raw && typeof raw === 'object' ? raw : {};
    return {phase: phases.includes(value.phase) ? value.phase : 'unknown',
      lane: ['audio', 'video', 'audioVideo'].includes(value.lane) ? value.lane : null,
      success: typeof value.success === 'boolean' ? value.success : null,
      elapsedMs: Number.isFinite(value.elapsedMs) && value.elapsedMs >= 0 && value.elapsedMs <= 900000 ?
        Math.floor(value.elapsedMs) : null,
      visibility: ['visible', 'hidden', 'prerender'].includes(value.visibility) ? value.visibility : 'unknown',
      context: ['running', 'suspended', 'closed'].includes(value.context) ? value.context : 'unknown',
      computedDisplay: ['block', 'none', 'other'].includes(value.computedDisplay) ? value.computedDisplay : 'other',
      connected: typeof value.connected === 'boolean' ? value.connected : null,
      closing: typeof value.closing === 'boolean' ? value.closing : null,
      actualFrames: Number.isSafeInteger(value.actualFrames) && value.actualFrames >= 0 && value.actualFrames <= 96 ?
        value.actualFrames : null,
      paintCancelled: typeof value.paintCancelled === 'boolean' ? value.paintCancelled : null,
      failure: value.failure === null ? null :
        ['unknown', 'timeout', 'closed', 'other'].includes(value.failure) ? value.failure : 'other'};
  });
  return {available: true, truncated: original.length >= 96, events};
}

// deviceFailureCategory 仅保留C++固定失败前缀、原方法、状态码及基线403两种固定原因，不落动态正文。
function deviceFailureCategory(bytes) {
  const line = bytes.subarray(0, 4096).toString('utf8').split(/[\r\n]/, 1)[0];
  const prefix = line.split(/\s/, 1)[0];
  if (!['invalid_media_transport', 'invalid_output_focus', 'invalid_file_catalog', 'browser_bind_failed',
    'runtime_failed', 'invalid_device_environment', 'request_rejected'].includes(prefix)) return null;
  const match = prefix === 'request_rejected' ? /\bcode=(\d{3,7})(?:\s|$)/.exec(line) : null;
  const originalMethod = prefix === 'request_rejected' ? /^request_rejected method=([A-Za-z]+)(?:\s|$)/.exec(line)?.[1] : null;
  const methods = ['capabilityReport', 'sessionCreate', 'mediaOffer', 'mediaState', 'audioStart', 'audioStop',
    'respCancel', 'sessionEnd', 'inputSend']; // 仅原pending请求方法，不能从供应商或任意JSON取名称。
  const reason = /(?:^|\s)reason=([A-Za-z]+)(?:\s|$)/.exec(line)?.[1];
  return {kind: prefix, code: match ? Number(match[1]) : null,
    ...(methods.includes(originalMethod) ? {method: originalMethod} : {}),
    ...(originalMethod === 'capabilityReport' && match?.[1] === '403' &&
      ['focusDeviceUnavailable', 'requestDenied'].includes(reason) ? {reason} : {})};
}

// createDeviceFailureObserver 在完整行到达后才投影固定事实；有界内存丢弃超长整行，不落自由stderr日志。
function createDeviceFailureObserver() {
  const pending = Buffer.alloc(4096); // std::cerr可能分次flush，原方法/code/reason必须在同一完整行关联。
  let length = 0, overflow = false, value = null, observed = false, completeLines = 0, discardedOverlongLines = 0;
  const complete = () => {completeLines = Math.min(1000000, completeLines + 1);
    if (overflow) discardedOverlongLines = Math.min(1000000, discardedOverlongLines + 1);
    else if (length) value ||= deviceFailureCategory(pending.subarray(0, length));
    length = 0; overflow = false;}; // 未识别行不制造类别，首个已关联固定失败保持不变。
  return {
    push(bytes) {if (bytes.length) observed = true;
      for (const byte of bytes) {if (byte === 10) complete();
        else if (!overflow) {if (length === pending.length) {overflow = true; length = 0;} else pending[length++] = byte;}}
      return value;},
    finish() {if (length || overflow) complete(); return value;}, // stderr结束时才接纳无newline但未超限的最后一行，重复调用不重复计数。
    facts() {return {stderrObserved: observed, completeLines, discardedOverlongLines, matchedFailure: value !== null};}, // 有界计数不保存原stderr或动态字段。
  };
}

// runtimeFailureCategory 只接受原native单层五字段闭集；重复键、自由正文、非法UTF8或不可能组合均丢弃，不改业务判据。
function runtimeFailureCategory(bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length > 1024) return null;
  let line;
  try {line = new (require('node:util').TextDecoder)('utf-8', {fatal: true}).decode(bytes).replace(/\r$/, '');}
  catch {return null;}
  if (!line.startsWith('host_failure=')) return null;
  const body = line.slice('host_failure='.length);
  if (!body.startsWith('{') || !body.endsWith('}')) return null;
  const value = Object.create(null); // 固定值全为ASCII枚举/整数1，拒绝转义和重复键避免JSON.parse覆盖首份字段。
  for (const member of body.slice(1, -1).split(',')) {
    const match = /^[ \t\r]*"([A-Za-z]+)"[ \t\r]*:[ \t\r]*(?:"([A-Za-z]+)"|(1))[ \t\r]*$/.exec(member);
    if (!match || !['kind', 'runtime', 'udp', 'pcm', 'observation'].includes(match[1]) || Object.hasOwn(value, match[1])) return null;
    value[match[1]] = match[3] ? 1 : match[2];
  }
  const runtimes = ['mqttConnect', 'mqttDisconnected', 'mqttEventFailure', 'mqttSubscription',
    'clockRollback', 'connectionDeadline', 'mqttPoll', 'signalEnvelope', 'signalClientInvalid',
    'signalClientDriver', 'messageEnvelope', 'messageAck', 'messageContent', 'messageToolScope',
    'messageQueueFull', 'actionReplyPublish', 'clientTick', 'toolPoll', 'mediaPoll', 'mediaOffer',
    'mediaReady', 'missingWallClock', 'drainDeadline', 'sharedInputInvalid', 'unknown'];
  const udp = ['none', 'unconfigured', 'clockRollback', 'probeReceive', 'probeSend', 'probeDeadline',
    'captureRead', 'captureEncode', 'captureEncrypt', 'captureSend', 'replyReceive', 'replyCrypto',
    'replyBeyondEnd', 'pcmWrite', 'playbackStall', 'unknown'];
  const pcm = ['none', 'driverClosed', 'playbackInactive', 'epochMismatch', 'frameSize', 'bufferFull',
    'ticketExhausted', 'queueRejected', 'unknown'];
  if (Object.keys(value).length !== 5 || value.kind !== 'runtimeFailureDiagnostic' || value.observation !== 1 ||
      !runtimes.includes(value.runtime) || !udp.includes(value.udp) || !pcm.includes(value.pcm) ||
      (value.runtime !== 'mediaPoll' && (value.udp !== 'none' || value.pcm !== 'none')) ||
      (value.udp !== 'pcmWrite' && value.pcm !== 'none') ||
      (value.udp === 'pcmWrite' && value.pcm === 'none')) return null;
  return {kind: value.kind, runtime: value.runtime, udp: value.udp, pcm: value.pcm, observation: 1};
}

// clientFailureCategory 新本机首因独立严格解析；字段/组合均为闭集，不能保存原报文或自由错误。
function clientFailureCategory(bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length > 1024) return null;
  let line;
  try {line = new (require('node:util').TextDecoder)('utf-8', {fatal: true}).decode(bytes).replace(/\r$/, '');}
  catch {return null;}
  if (!line.startsWith('host_client_failure=')) return null;
  const body = line.slice('host_client_failure='.length);
  if (!body.startsWith('{') || !body.endsWith('}')) return null;
  const value = Object.create(null); // ASCII固定值拒绝重复键/转义，避免后值覆盖首次事实。
  for (const member of body.slice(1, -1).split(',')) {
    const match = /^[ \t\r]*"([A-Za-z]+)"[ \t\r]*:[ \t\r]*(?:"([A-Za-z]+)"|(1))[ \t\r]*$/.exec(member);
    if (!match || !['kind','origin','cause','method','observation'].includes(match[1]) || Object.hasOwn(value,match[1])) return null;
    value[match[1]] = match[3] ? 1 : match[2];
  }
  const clientCauses = ['unknown','sessionError','requestRejected','requestsTimeout','activeScopeLost',
    'candidateClock','candidateEstablishDeadline','candidateRecoveryDeadline','visionProfileInvalid',
    'mediaRecoveryDeadline','negotiatingDeadline'];
  const prepareCauses = ['unknown','endpointPrepareDeadline','endpointPrepareClock','endpointPrepareDriver','endpointPrepareDispatch'];
  const methods = ['unknown','capabilityReport','sessionCreate','sessionAccept','sessionReject','mediaState','mediaOffer',
    'audioStart','audioStop','respCancel','sessionEnd','inputSend','sessionSync','endpointControl'];
  const request = ['requestRejected','requestsTimeout'].includes(value.cause);
  if (Object.keys(value).length !== 5 || value.kind !== 'clientFailureDiagnostic' || value.observation !== 1 ||
      !(value.origin === 'clientFailed' && clientCauses.includes(value.cause) ||
        value.origin === 'endpointPrepareFailed' && prepareCauses.includes(value.cause)) ||
      !(request ? methods.includes(value.method) : value.method === 'none')) return null;
  return {kind:value.kind,origin:value.origin,cause:value.cause,method:value.method,observation:1};
}

// createDeviceLifecycleObserver 仅收录原stdout整行；首因需显式true，缺省字段/观察调用不变，不从Idle/exit0推断。
function createDeviceLifecycleObserver(failureDiagnostics = false) {
  const pending = Buffer.alloc(4096); // 有界行缓冲不保存启动URL、身份或模型正文。
  const voiceStates = []; // 最多保留64项原实际状态，截断只影响诊断而不影响验收。
  let runtimeFailure = null; // 原流中第一份合法首因不可由清理/重复stdout覆盖；关闭后只读，不重新采进程或页面。
  let clientFailure = null; // 独立细分首次实际失败，不将旧runtime缺失值推为客户端成功。
  let length = 0, overflow = false, observed = false, ended = false;
  let completeLines = 0, discardedOverlongLines = 0, normalHostExitBranch = null, observationTruncated = false;
  // complete 只按整行精确匹配白名单；过长整行丢弃，禁止自由后缀被截断成有效事实。
  const complete = () => {
    completeLines = Math.min(1000000, completeLines + 1);
    if (overflow) discardedOverlongLines = Math.min(1000000, discardedOverlongLines + 1);
    else if (length) {
      const line = pending.subarray(0, length).toString('utf8').replace(/\r$/, '');
      if (failureDiagnostics === true && runtimeFailure === null && line.startsWith('host_failure='))
        runtimeFailure = runtimeFailureCategory(pending.subarray(0, length));
      if (failureDiagnostics === true && clientFailure === null && line.startsWith('host_client_failure='))
        clientFailure = clientFailureCategory(pending.subarray(0, length));
      const state = /^voice_state=([0-6])$/.exec(line);
      const branch = /^host_exit_branch=(runtime_idle|browser_closed)$/.exec(line);
      if (state) {
        if (voiceStates.length === 64) {voiceStates.shift(); observationTruncated = true;}
        voiceStates.push(Number(state[1]));
      } else if (branch && normalHostExitBranch === null) normalHostExitBranch = branch[1];
    }
    length = 0; overflow = false;
  };
  return {
    push(bytes) {
      if (ended) return;
      if (bytes.length) observed = true;
      for (const byte of bytes) {
        if (byte === 10) complete();
        else if (!overflow) {
          if (length === pending.length) {overflow = true; length = 0;}
          else pending[length++] = byte;
        }
      }
    },
    finish() {if (!ended && (length || overflow)) complete(); ended = true;},
    facts() {return {stdoutObserved: observed, stdoutEnded: ended, completeLines, discardedOverlongLines,
      voiceStates: voiceStates.slice(), normalHostExitBranch, observationTruncated,
      ...(failureDiagnostics === true ? {runtimeFailure: runtimeFailure === null ? null : {...runtimeFailure},
        clientFailure: clientFailure === null ? null : {...clientFailure}} : {})};},
  };
}

// browserFailureCategory 只识别CLI固定超时/启动/连接前缀，无法分类仍保留未知，不输出原error正文。
function browserFailureCategory(error, body, stderr = '') {
  // CLI可能只在stderr写固定前缀；自由正文与Node包含完整命令的message均不进入报告。
  const texts = [typeof body?.error === 'string' ? body.error : '', typeof stderr === 'string' ? stderr : ''];
  if (texts.some(text => /^Wait timed out after (?:[1-9]\d{0,8}|0)(?:ms|s)(?:$|[.:\s])/.test(text))) return 'browser_timeout';
  if (error?.code === 'ETIMEDOUT' || error?.killed === true || texts.some(text => /^(?:TimeoutError:|Timeout \d+ms exceeded|page\.waitForFunction: Timeout)/.test(text))) return 'browser_timeout';
  if (texts.some(text => /^(?:Failed to launch browser|browserType\.launch:)/.test(text))) return 'browser_launch_failed';
  if (texts.some(text => /^(?:connect ECONNREFUSED|Browser disconnected|WebSocket connection failed)/.test(text))) return 'browser_connection_failed';
  return 'browser_command_failed';
}

// browserFailureFacts 保留原单次CLI失败的固定旁证；selector/表达式/自由error与stdout正文全部丢弃。
function browserFailureFacts(phase, args, error, stdout, stderr, body, parsed) {
  const commands = ['open', 'click', 'wait', 'eval', 'fill', 'close'];
  const codes = ['ETIMEDOUT', 'EAGAIN', 'ENOENT', 'ECONNREFUSED', 'ERR_CHILD_PROCESS_STDIO_MAXBUFFER'];
  const value = body?.error;
  return {phase, command: commands.includes(args[0]) ? args[0] : 'unknown',
    failureKind: browserFailureCategory(error, body, stderr),
    processCode: codes.includes(error?.code) ? error.code : (Number.isInteger(error?.code) ? 'exit_nonzero' : null),
    exitCode: Number.isInteger(error?.code) && error.code >= 0 && error.code <= 255 ? error.code : null,
    signal: ['SIGTERM', 'SIGKILL', 'SIGABRT', 'SIGSEGV'].includes(error?.signal) ? error.signal : null,
    stdoutPresent: typeof stdout === 'string' && stdout.length > 0, stdoutParsed: parsed,
    cliSuccess: typeof body?.success === 'boolean' ? body.success : null,
    cliErrorShape: value == null ? 'absent' : (typeof value === 'string' ? 'string' : (typeof value === 'object' ? 'object' : 'other')),
    stderrPresent: typeof stderr === 'string' && stderr.length > 0};
}

// verifyStep 检查唯一成功业务动作及实际设备事实，返回已验证的工具数量；只读查询终态不提高任何业务事实。
function verifyStep(step, kind, before, after, chain) {
  assert.equal(after.overflow, false, 'observer_overflow');
  // 单业务意图不是单工具调用：只允许一次正式状态查询或原引用恢复，不能替代指定动作或放宽设备事实。
  const returned = after.tools.slice(before.tools.length); // 只核本轮新增终态，前轮历史不计入。
  assert.ok(returned.length === 1 || returned.length === 2, 'formal_tool_missing_or_repeated');
  const actions = returned.filter(value => value.name === 'things_iot_mcp.' + tools[step]);
  const queries = returned.filter(value => ['things_iot_mcp.recover_media_playback_operation',
    'things_iot_mcp.get_output_focus_status', 'things_iot_mcp.get_media_playback_status'].includes(value.name));
  assert.equal(actions.length, 1, 'formal_tool_missing_or_repeated');
  assert.ok(queries.length <= 1 && actions.length + queries.length === returned.length,
    'formal_tool_missing_or_repeated');
  assert.equal(actions[0].state, 'succeeded', 'tool_result_unknown_or_failed');
  for (const query of queries) assert.ok(['succeeded', 'failed', 'unknown'].includes(query.state),
    'read_only_recovery_not_terminal'); // unknown/failed只保留查询事实，不能当成功回执或授权。
  if (step === 'invite') {
    assert.equal(before.file, null, 'previous_program_player_not_released');
    // 邀请不等于加载；允许绑定尚未到达，或原initialize的空state基线，不虚构invited播放状态。
    if (after.file !== null) {
      assert.equal(after.file?.kind, kind, 'invited_program_kind_mismatch');
      assert.equal(after.file.state, '', 'program_loaded_or_played_during_invite');
      assert.equal(after.file.generation, '0', 'program_control_started_during_invite');
      assert.equal(after.file.busy, false, 'program_control_pending_during_invite');
      assert.equal(after.file.samples, 0, 'program_audio_during_invite');
      assert.equal(after.file.energy, 0, 'program_audio_during_invite');
      assert.equal(after.file.frames, 0, 'program_video_during_invite');
      assert.equal(after.file.pixels, false, 'program_video_during_invite');
      assert.equal(after.file.paused, true, 'program_playing_during_invite');
    }
  }
  // 本地新接纳不等于新授权；四操作须有大于此前原修订高水位的真实成功回执，排除历史重放。
  const focusOperation = {initial: 'acquire', suspendAi: 'suspend', acquireFile: 'acquire',
    suspendFile: 'suspend', restoreAi: 'restore', releaseFile: 'release'}[step];
  if (focusOperation) {
    assert.ok(Array.isArray(before.focusReceipts) && Array.isArray(after.focusReceipts) &&
      after.focusReceipts.length <= 32 && Number.isSafeInteger(before.focusReceiptIndex) &&
      before.focusReceiptIndex >= 0 && Number.isSafeInteger(after.focusReceiptIndex) &&
      after.focusReceiptIndex > before.focusReceiptIndex, 'original_focus_receipt_missing');
    for (const receipt of [...before.focusReceipts, ...after.focusReceipts])
      assert.ok(Number.isSafeInteger(receipt.revision) && receipt.revision >= 0 &&
        Number.isSafeInteger(receipt.receiptIndex) && receipt.receiptIndex > 0 &&
        receipt.receiptIndex <= after.focusReceiptIndex, 'invalid_actual_focus_watermark');
    const originalRevision = Math.max(0, ...before.focusReceipts.map(value => value.revision));
    const current = after.focusReceipts.filter(value => value.receiptIndex > before.focusReceiptIndex);
    const focusState = {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'}[focusOperation];
    assert.equal(current.length, 1, 'missing_or_repeated_actual_focus_receipt');
    assert.equal(current[0].operation, focusOperation, 'actual_focus_operation_mismatch');
    assert.equal(current[0].state, focusState, 'actual_focus_state_mismatch');
    assert.equal(current[0].code, 200, 'actual_focus_was_denied');
    assert.ok(current[0].revision > originalRevision, 'historical_focus_replay_not_new_authorization');
  }
  if (step === 'load') assert.equal(after.file?.state, 'loaded');
  if (step === 'play') {
    assert.equal(after.file?.kind, kind);
    assert.equal(after.file?.state, 'playing');
    assert.ok(Number.isSafeInteger(after.file.samples) && after.file.samples >= 256 &&
      Number.isFinite(after.file.energy) && after.file.energy > 0, 'no_actual_program_audio');
    assert.equal(after.file.paused, false);
    const lane = kind === 'video' ? 'audioVideo' : 'audio';
    assert.equal(after.gates.filter(value => value.lane === lane).at(-1)?.enabled, true,
      'actual_program_output_lane_not_open');
    if (kind === 'video') assert.ok(Number.isSafeInteger(after.file.frames) &&
      after.file.frames > 0 && after.file.pixels, 'no_actual_program_video');
  }
  if (step === 'pause') {
    assert.equal(after.file?.state, 'paused');
    assert.equal(after.file?.paused, true);
    assert.ok(after.overlap > before.overlap && after.captured > before.captured && after.recognition,
      'no_recognized_voice_during_program');
  }
  if (['acquireFile', 'suspendFile'].includes(step)) {
    const lane = kind === 'video' ? 'audioVideo' : 'audio';
    assert.ok(after.gates.slice(before.gates.length).some(gate =>
      gate.lane === lane && gate.enabled === (step === 'acquireFile')), 'actual_full_group_gate_missing');
  }
  // release可能保持已关闭mask而不再次门控；不把这个观察冒充后端原focusState/released证据。
  if (step === 'releaseFile') {
    const lane = kind === 'video' ? 'audioVideo' : 'audio';
    assert.equal(after.gates.filter(value => value.lane === lane).at(-1)?.enabled, false,
      'program_output_not_closed_before_release');
  }
  if (step === 'restoreAi') assert.ok(after.consumedNonSilent > before.consumedNonSilent,
    'no_actual_ai_audio_after_explicit_restore');
  if (step === 'initial') assert.ok(after.consumedNonSilent > before.consumedNonSilent,
    'no_actual_ai_audio_after_initial_acquire');
  if (step === 'stop') assert.equal(after.file, null, 'original_player_not_actually_closed');
  if (chain === 'realtime_native') {
    assert.equal(after.mode, 'fullDuplex');
    assert.equal(after.captureOpen, true, 'continuous_input_was_stopped');
  }
  assert.equal(after.contextState, 'running');
  assert.equal(after.streamActive, true);
  return returned.length; // 仅上述资格和全部原媒体门槛通过后，才允许本轮1或2个可信同回复工具。
}

// prepareFixtureCapture 半双工先等原播放实际排空，两种模式均等采集ACK才放音；超时不重发或改设备策略。
async function prepareFixtureCapture(chain, {start, wait}) {
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(chain));
  if (chain === 'cascaded_agentruntime') {
    await wait('responseId === "" && pendingPlayback.size === 0');
    await start();
  }
  await wait('__urMcpObserver.captureOpen === true');
}

// evaluateReplyWaitDiagnostic只在显式诊断时旁观原同次求值；beginMS仅关联，页面时钟不与Node相减。
// 原谓词异常保持原对象throw；旁证异常仍按旧样本try语义忽略。4096次后只置overflow，不扩大账本。
function evaluateReplyWaitDiagnostic(beginMS, predicate, sample) {
  let stats = null, pageMS = null;
  try {
    if (Number.isFinite(beginMS) && beginMS >= 0) {
      stats = globalThis.__urMcpReplyWaitDiagnostic;
      if (!stats || stats.beginMS !== beginMS) {
        // 先登记本轮单槽再读时钟；首poll取时失败也要保留不可恢复的漏记事实。
        stats = {beginMS,firstPageMS:null,lastPageMS:null,sampleCount:0,lastPredicatePassedPageMS:null,
          predicateExceptionCount:0,referenceErrors:0,typeErrors:0,rangeErrors:0,syntaxErrors:0,otherErrors:0,
          observationExceptionCount:0,overflow:false,incomplete:false};
        globalThis.__urMcpReplyWaitDiagnostic = stats;
      }
      pageMS = performance.now();
      if (!Number.isFinite(pageMS) || pageMS < 0) {
        stats.incomplete = true;
        stats = null;
      } else {
        if (stats.firstPageMS === null) stats.firstPageMS = pageMS;
        if (stats.lastPageMS !== null && pageMS < stats.lastPageMS) stats.incomplete = true;
        if (stats.sampleCount >= 4096) stats.overflow = true;
        if (!stats.overflow) {++stats.sampleCount;stats.lastPageMS = pageMS;}
      }
    }
  } catch {
    try {const previous = globalThis.__urMcpReplyWaitDiagnostic;if (previous && previous.beginMS === beginMS) previous.incomplete = true;} catch {}
    stats = null;
  } // 诊断失效不能改变原等待是否成立或原错误，漏记后不可恢复为known。
  let passed;
  try { passed = predicate(); }
  catch (error) {
    try {
      if (stats && !stats.overflow) {
        ++stats.predicateExceptionCount;
        let field = 'otherErrors';
        // 仅读固定原生prototype，不读取自由name/message/stack或其getter。
        try {const prototype = Object.getPrototypeOf(error);
          if (prototype === ReferenceError.prototype) field = 'referenceErrors';
          else if (prototype === TypeError.prototype) field = 'typeErrors';
          else if (prototype === RangeError.prototype) field = 'rangeErrors';
          else if (prototype === SyntaxError.prototype) field = 'syntaxErrors';
        } catch {}
        ++stats[field];
      }
    } catch {}
    throw error;
  }
  try {if (stats && !stats.overflow && passed === true) stats.lastPredicatePassedPageMS = pageMS;} catch {}
  try { sample(passed); }
  catch {try {if (stats && !stats.overflow) ++stats.observationExceptionCount;} catch {}}
  return passed;
}

// readReplyWaitDiagnostic仅在失败catch原页面读取中投影同轮有界计数；不查询媒体、不补完成时刻。
// catchPageMS是此次读取时点，不能当原回复完成时点；未知/旧轮/超限明确不提供成功证据。
function readReplyWaitDiagnostic(beginMS) {
  try {
    const value = globalThis.__urMcpReplyWaitDiagnostic;
    if (!Number.isFinite(beginMS) || beginMS < 0 || !value || typeof value !== 'object') return {known:false,reason:'unavailable'};
    const fields = Object.getOwnPropertyDescriptors(value);
    const keys = ['beginMS','firstPageMS','lastPageMS','sampleCount','lastPredicatePassedPageMS',
      'predicateExceptionCount','referenceErrors','typeErrors','rangeErrors','syntaxErrors','otherErrors','observationExceptionCount','overflow','incomplete'];
    if (Reflect.ownKeys(fields).length !== keys.length || !keys.every(key => Object.hasOwn(fields,key) && Object.hasOwn(fields[key],'value')))
      return {known:false,reason:'invalid'};
    const own = Object.fromEntries(keys.map(key => [key,fields[key].value]));
    if (own.beginMS !== beginMS) return {known:false,reason:'scopeMismatch'};
    if (own.incomplete !== false) return {known:false,reason:'invalid'};
    if (own.overflow === true) return {known:false,reason:'limitReached'};
    const counters = ['sampleCount','predicateExceptionCount','referenceErrors','typeErrors','rangeErrors','syntaxErrors','otherErrors','observationExceptionCount'];
    if (own.overflow !== false || !counters.every(key => Number.isSafeInteger(own[key]) && own[key] >= 0 && own[key] <= 4096) ||
      own.sampleCount === 0 || own.predicateExceptionCount + own.observationExceptionCount > own.sampleCount ||
      own.referenceErrors + own.typeErrors + own.rangeErrors + own.syntaxErrors + own.otherErrors !== own.predicateExceptionCount ||
      !Number.isFinite(own.firstPageMS) || own.firstPageMS < 0 || !Number.isFinite(own.lastPageMS) || own.lastPageMS < own.firstPageMS ||
      !(own.lastPredicatePassedPageMS === null || (Number.isFinite(own.lastPredicatePassedPageMS) && own.lastPredicatePassedPageMS >= own.firstPageMS && own.lastPredicatePassedPageMS <= own.lastPageMS)))
      return {known:false,reason:'invalid'};
    if (own.predicateExceptionCount === own.sampleCount && own.lastPredicatePassedPageMS !== null) return {known:false,reason:'invalid'};
    const catchPageMS = performance.now();
    if (!Number.isFinite(catchPageMS) || catchPageMS < own.lastPageMS) return {known:false,reason:'clockUnknown'};
    delete own.beginMS;delete own.overflow;delete own.incomplete;
    return {known:true,...own,catchPageMS};
  } catch {return {known:false,reason:'invalid'};}
}

// waitFixtureReply 半双工等原网络终态及独立本地排空；可选diagnostic只记原一次等待时刻和水位，不改期限或结果。
async function waitFixtureReply(chain, beforeCompletedResponses, wait, diagnostic = null) {
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(chain));
  assert.ok(Number.isSafeInteger(beforeCompletedResponses) && beforeCompletedResponses >= 0);
  if (chain === 'cascaded_agentruntime') {
    const expression = `metrics.completedResponses > ${beforeCompletedResponses} && responseId === "" && pendingPlayback.size === 0`;
    if (!diagnostic) { await wait(expression); return; }
    // 旁证只包裹原一次等待；判据先求值，采样异常不能改变原返回或错误。
    diagnostic.beforeCompletedResponses = beforeCompletedResponses;
    // Node时刻仅关联本次调用；页面sampleMS属于另一时钟域，禁止跨域相减。
    diagnostic.beginMS = performance.now();
    diagnostic.endMS = null;
    try {
      const observation = diagnostic.observeReply === true ? ',observation:(' + readReplyDiagnostic.toString() + ')()' : '';
      const sample = `globalThis.__urMcpReplyDrainSample={beginMS:${diagnostic.beginMS},sampleMS:performance.now(),completedResponses:metrics.completedResponses,responseActive:responseId!=="",pendingPlaybackFrames:pendingPlayback.size,predicatePassed:passed${observation}};`;
      if (diagnostic.observeReply === true) {
        await wait('(' + evaluateReplyWaitDiagnostic.toString() + `)(${diagnostic.beginMS},()=>(${expression}),(passed)=>{${sample}})`);
      } else {
        await wait(`(()=>{const passed=(${expression});try{${sample}}catch{}return passed;})()`);
      }
    } finally { diagnostic.endMS = performance.now(); }
  }
}

// readReplyDiagnostic只读原页面冻结计时与ASR绑定，原ID仅经私有CLI瞬时交Node，不进入结果或日志。
function readReplyDiagnostic() {
  try {
    const roundReplyId = globalThis.__urMcpRound?.diagnosticReply?.() || '';
    const frozen = metrics.lastReplyTiming;
    // 复制已有四个C++整数时刻，不调用页面时间生成首响应，也不读取任何正文。
    const timing = frozen ? {responseId:frozen.responseId,lastVoiceSentMs:frozen.lastVoiceSentMs,
      firstTextMs:frozen.firstTextMs,firstNetworkMs:frozen.firstNetworkMs,firstPlayableMs:frozen.firstPlayableMs} : null;
    return {roundReplyId,activeReplyId:responseId,completedReplyId:completedResponseId,
      completedResponses:metrics.completedResponses,pendingPlaybackFrames:pendingPlayback.size,timing};
  } catch { return null; }
}

// readReplyDrainObservation仅读本次原等待最后样本；后续catch/cleanup不得回填已冻结的媒体事实。
function readReplyDrainObservation(beginMS) {
  const value = globalThis.__urMcpReplyDrainSample;
  return value && Number.isFinite(beginMS) && beginMS >= 0 && value.beginMS === beginMS
    ? value.observation || null : null;
}

// projectReplyDiagnostic将严格有限原事实投影为哈希和同C++钟延迟；scope声明本轮输入与上一轮锚点。
function projectReplyDiagnostic(raw, scope = {}) {
  const unknown = reason => ({clock:'cpp_monotonic_ms',firstTextMilliseconds:null,firstNetworkAudioMilliseconds:null,
    firstPlayableAudioMilliseconds:null,primaryMeasured:false,unavailable:{firstTextMilliseconds:reason,
      firstNetworkAudioMilliseconds:reason,firstPlayableAudioMilliseconds:reason},deviceConsumptionVerifiedNotTimed:false,
    physicalSpeakerOnsetMeasured:false});
  // validId与原桥UTF8上限一致；孤立代理项不得被UTF8替换后冒充同回复。
  const validId = value => typeof value === 'string' && Buffer.byteLength(value,'utf8') <= 128 &&
    !/[\x00-\x1f\x7f]/.test(value) && !/[\uD800-\uDFFF]/u.test(value);
  if (!raw || typeof raw !== 'object' || Array.isArray(raw) || Object.keys(raw).length !== 6 ||
      !['roundReplyId','activeReplyId','completedReplyId'].every(key => validId(raw[key])) ||
      !Number.isSafeInteger(raw.completedResponses) || raw.completedResponses < 0 || raw.completedResponses > 1000000 ||
      !Number.isSafeInteger(raw.pendingPlaybackFrames) || raw.pendingPlaybackFrames < 0 || raw.pendingPlaybackFrames > 32)
    return {known:false,timing:unknown('reply_observation_unavailable')};
  // replyHash与服务端原SHA256口径相同；缺绑定为null，不能按邻近时刻猜同轮。
  const hash = id => id ? createHash('sha256').update(id,'utf8').digest('hex') : null;
  const value = {known:true,replyHash:hash(raw.roundReplyId),activeReplyHash:hash(raw.activeReplyId),
    completedReplyHash:hash(raw.completedReplyId),sameActiveReply:!!raw.roundReplyId && raw.roundReplyId === raw.activeReplyId,
    sameCompletedReply:!!raw.roundReplyId && raw.roundReplyId === raw.completedReplyId,
    completedResponses:raw.completedResponses,pendingPlaybackFrames:raw.pendingPlaybackFrames,
    timing:unknown('same_reply_timing_unavailable')};
  const timing = raw.timing;
  if (!raw.roundReplyId || !timing || typeof timing !== 'object' || Array.isArray(timing) ||
      Object.keys(timing).length !== 5 || timing.responseId !== raw.roundReplyId ||
      !['lastVoiceSentMs','firstTextMs','firstNetworkMs','firstPlayableMs'].every(key => Object.hasOwn(timing,key) &&
        (timing[key] === null || (Number.isSafeInteger(timing[key]) && timing[key] >= 0)))) return value;
  // 原整数时刻只按已绑定回复保存，页面与Node时钟不进入该结构。
  value.nativeTiming = {lastVoiceSentMs:timing.lastVoiceSentMs,firstTextMs:timing.firstTextMs,
    firstNetworkMs:timing.firstNetworkMs,firstPlayableMs:timing.firstPlayableMs};
  if (scope.input !== 'voice') value.timing = unknown('current_input_not_voice');
  else if (scope.beforeCompletedResponses > 0 && !Number.isSafeInteger(scope.beforeVoiceMS))
    value.timing = unknown('previous_voice_anchor_unavailable');
  else if (timing.lastVoiceSentMs !== null && Number.isSafeInteger(scope.beforeVoiceMS) && timing.lastVoiceSentMs <= scope.beforeVoiceMS)
    value.timing = unknown('stale_voice_anchor');
  else {
    // 复用现有控制验收同钟相减/缺失/重叠校验，避免复制另一套首音口径。
    const {responseId:privateId,...replyTiming} = timing;
    try {
      // 本投影仅采原时钟，不负责同回复实际消费验收；原helper在另一正式流程的消费判据不能借用。
      value.timing = {...require('./browser_ai_control_live.cjs').verifyReplyTiming({replyTiming},'udp'),
        deviceConsumptionVerifiedNotTimed:false};
    }
    catch { value.timing = unknown('invalid_native_timing'); }
  }
  return value;
}

// installInputDiagnostic 只读包装原incoming；期望仅驻留观察闭包，返回有限事实，不参与业务判据。
// 三项仅为字面标记；“不要释放”忽略标点空白后的命中不证明否定语义，不解释用户意图或改变原输入。
function installInputDiagnostic(enabled = false) {
  if (enabled !== true) return false;
  if (globalThis.__urMcpInputDiagnostic) throw Error('input_diagnostic_already_installed');
  let expected = '', armed = false, observations = 0;
  let facts = {known: false, normalizedExactMatch: false, inviteTokenPresent: false,
    musicTokenPresent: false, negationTokenPresent: false, suspendTokenPresent: false,
    releaseTokenPresent: false, doNotReleasePhrasePresent: false, observations: 0};
  // normalized仅忽略标点和空白，不把关键词命中或改写当完整语义证明。
  const normalized = value => value.replace(/[\p{P}\p{Z}\s]/gu, '');
  const original = incoming;
  globalThis.__urMcpInputDiagnostic = {
    arm(value) {
      if (typeof value !== 'string' || !value || value.length > 4096) throw Error('invalid_fixed_fixture');
      expected = normalized(value); armed = true; observations = 0;
      facts = {known: false, normalizedExactMatch: false, inviteTokenPresent: false,
        musicTokenPresent: false, negationTokenPresent: false, suspendTokenPresent: false,
        releaseTokenPresent: false, doNotReleasePhrasePresent: false, observations: 0};
    },
    facts() { return {...facts}; },
    release() { expected = ''; armed = false; }
  };
  incoming = function(value) {
    const result = original.call(this, value);
    if (!armed || !value || value.kind !== 'text' || typeof value.text !== 'string' || !value.text.startsWith('\n用户：')) return result;
    if (++observations > 8 || value.text.length > 8192) { facts.known = false; return result; }
    const text = value.text.slice(4).split('\n助手：')[0];
    facts = {known: true, normalizedExactMatch: normalized(text) === expected,
      inviteTokenPresent: text.includes('邀请'), musicTokenPresent: text.includes('音乐'),
      negationTokenPresent: /不要|不需要|不想|别/.test(text), suspendTokenPresent: text.includes('暂停'),
      releaseTokenPresent: text.includes('释放'), doNotReleasePhrasePresent: normalized(text).includes('不要释放'), observations};
    return result;
  };
  return true;
}

// readReplyDrainFacts 仅在清理前读取原网络终态水位与播放排空事实，不暴露回复标识或改变原通过判据。
// 页面尚未就绪、计数非法或过大时明确返回未知；不能从工具成功推断网络回复已完成。
function readReplyDrainFacts() {
  if (typeof metrics !== 'object' || metrics === null || typeof responseId !== 'string' ||
      typeof pendingPlayback !== 'object' || pendingPlayback === null ||
      !Number.isSafeInteger(metrics.completedResponses) || metrics.completedResponses < 0 ||
      !Number.isSafeInteger(pendingPlayback.size) || pendingPlayback.size < 0 || pendingPlayback.size > 32)
    return {known: false};
  return {known: true, completedResponses: metrics.completedResponses,
    responseActive: responseId !== '', pendingPlaybackFrames: pendingPlayback.size};
}

// readReplyDrainSample 只投影原单次等待最后一次页面求值；旧轮或非法数值明确未知，不输出标识或正文。
function readReplyDrainSample(beginMS) {
  const value = globalThis.__urMcpReplyDrainSample;
  if (!value || value.beginMS !== beginMS || !Number.isFinite(beginMS) || beginMS < 0 ||
      !Number.isFinite(value.sampleMS) || value.sampleMS < 0 ||
      !Number.isSafeInteger(value.completedResponses) || value.completedResponses < 0 ||
      !Number.isSafeInteger(value.pendingPlaybackFrames) || value.pendingPlaybackFrames < 0 || value.pendingPlaybackFrames > 32 ||
      typeof value.responseActive !== 'boolean' || typeof value.predicatePassed !== 'boolean') return {known: false};
  return {known: true, sampleMS: value.sampleMS, completedResponses: value.completedResponses,
    responseActive: value.responseActive, pendingPlaybackFrames: value.pendingPlaybackFrames, predicatePassed: value.predicatePassed};
}

// run 只启动本任务正式设备及具名浏览器，后端环境与授权内容须由调用方预先配置。
async function run(env = process.env, scenarioName = 'file') {
  // 仅两个源码固定场景可共享既有生命周期；计划不能指定模块、工具映射或执行回调。
  assert.ok(['file', 'live'].includes(scenarioName), 'invalid_fixed_scenario');
  const scenario = scenarioName === 'live' ? require('./mcp_live_broadcast_live.cjs').scenario :
    {steps, tools, validatePlan, installObserver, readSnapshot, verifyStep,
      result: (_step, after) => ({fileState: after.file?.state || null,
        samples: after.file?.samples || 0, frames: after.file?.frames || 0})};
  const {steps: scenarioSteps, tools: scenarioTools, validatePlan: checkPlan,
    installObserver: install, readSnapshot: read, verifyStep: verify} = scenario;
  assert.equal(env[scenarioName === 'file' ? 'UR_MCP_FILE_LIVE' : 'UR_MCP_BROADCAST_LIVE'], '1', 'explicit_live_opt_in_required');
  const fileStartupDiagnostic = fileStartupDiagnosticRequested(env.UR_MCP_STARTUP_DIAGNOSTIC, scenarioName);
  // 仅25/26旁路比较原固定TTS输入；原关键词、工具和实际媒体通过标准保持不变。
  const contextInputDiagnostic = scenarioName === 'file' && ['25', '26'].includes(env.UR_MCP_ATTEMPT);
  assert.equal(env.UR_OUTPUT_FOCUS, scenarioName === 'file' ? 'media' : 'live');
  assert.equal(env.UR_MEDIA_TRANSPORT || 'udp', 'udp', 'media_host_udp_only');
  assert.ok((scenarioName !== 'file' || env.UR_FILE_CATALOG) && env.UR_DEVICE_PRODUCT && env.UR_DEVICE_NAME && env.UR_DEVICE_SECRET);
  assert.ok(env.UR_MQTT_HOST && env.UR_MQTT_PORT);
  const binary = env.UR_DEVICE_BROWSER_BIN;
  assert.ok(binary && path.isAbsolute(binary) && fs.statSync(binary).isFile());
  const planPath = env[scenarioName === 'file' ? 'UR_MCP_FILE_PLAN' : 'UR_MCP_BROADCAST_PLAN'];
  assert.ok(planPath && path.isAbsolute(planPath));
  const planBytes = fs.readFileSync(planPath);
  assert.ok(planBytes.length <= 32768, 'plan_too_large');
  const plan = checkPlan(JSON.parse(planBytes));
  const port = Number(env.UR_BROWSER_PORT);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  const session = 'mcp-media-' + randomUUID();
  const browserEnv = {...env, AGENT_BROWSER_ARGS: [env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'].join(',')};
  let phase = 'device_start', deviceFailure = null; // 初始化细分阶段使后继尝试可定位，未知不补发协议命令。
  let browserCommandFailure = null; // 原首失败独立保存，catch只读eval或finally close错误不得覆盖它。
  let replyWait = null; // 原回复等待的有限旁证；不参与通过判据或增加CLI调用。
  let stepWait = null; // 只记录原三次等待的当前子段；不重试、不改变顺序或截止。
  let replyScope = null, replyBegin = null; // 本轮输入类别及开始排空的有限投影，不保存原ID或正文。
  let previousVoiceMS = null, replyDiagnosticsCount = 0; // 64轮+唯一限额通知，不因轮次重置预算。
  const results = [];
  const evidenceScope = {}; // 固定Live场景私有只读关联，不接受计划或模型提供身份/授权。
  const invoke = args => new Promise((resolve, reject) => {
    execFile('agent-browser', ['--session', session, '--json', ...args],
      {env: browserEnv, timeout: 65000, maxBuffer: 131072}, (error, stdout, stderr) => {
        let body, parsed = false; try {body = JSON.parse(stdout); parsed = true;} catch {}
        if (error || !body?.success) {
          browserCommandFailure ||= browserFailureFacts(args[0] === 'close' ? 'cleanup' : phase, args,
            error, stdout, stderr, body, parsed);
          return reject(Error(browserFailureCategory(error, body, stderr)));
        }
        try {
          resolve(body.data);
        } catch { reject(Error('browser_command_failed')); }
      });
  });
  const evaluate = async expression => (await invoke(['eval', '-b', Buffer.from(expression).toString('base64')])).result;
  const snapshot = async () => {
    const value = await evaluate('(' + read.toString() + ')()'); // 原场景设备事实仍由各自观察器读取。
    // 双链旁读轮次三闭环诊断：全双工保持原facts合同供判据核验；串联链附加arm拒绝事实，仅作失败取证。
    value.round = await evaluate(plan.chain === 'realtime_native' ?
      'globalThis.__urMcpRound?.facts() || null' :
      '(()=>{try{return{facts:globalThis.__urMcpRound?.facts()||null,armFault:globalThis.__urMcpRoundArmFault||null}}catch{return null}})()');
    return value; // 两个场景共同附加固定轮次诊断，不暴露闭包内ID或模型正文。
  };
  const wait = expression => invoke(['wait', '--fn', expression]);
  const child = spawn(binary, [String(port)], {env, stdio: ['ignore', 'pipe', 'pipe']});
  let exited = false, spawned = false, stderrEnded = false, exitCode = null, failedReport = null, successReport = null;
  child.once('spawn', () => {spawned = true;});
  const done = new Promise(resolve => { child.once('error', () => resolve(-1));
    child.once('close', code => { exited = true; exitCode = Number.isSafeInteger(code) ? code : null; resolve(code); }); });
  const closed = new Promise(resolve => {child.once('close', resolve);}); // error不等于退出；只有close证明子进程与stdio实际关闭。
  // waitForClose 只接受原进程close事实；关闭截止只限制等待，不能作为退出或排空证据。
  const waitForClose = async () => {
    if (exited) return true;
    let timer;
    try {return await Promise.race([closed.then(() => true), new Promise(resolve => {
      timer = setTimeout(() => resolve(false), 5000);
    })]);} finally {clearTimeout(timer);}
  };
  const deviceFailureObserver = createDeviceFailureObserver(); // 完整行内固定原因关联，不用单次pipe chunk猜原ACK。
  const deviceLifecycleObserver = createDeviceLifecycleObserver(env.UR_DEVICE_MESSAGE_DIAGNOSTICS === '1'); // 复用原精确开关，不增listener或采样，不改原退出判据。
  child.stdout.on('data', bytes => deviceLifecycleObserver.push(bytes));
  child.stdout.on('end', () => deviceLifecycleObserver.finish());
  child.stderr.on('data', bytes => {deviceFailure = deviceFailureObserver.push(bytes);});
  child.stderr.on('end', () => {stderrEnded = true; deviceFailure = deviceFailureObserver.finish();});
  const deadline = setTimeout(() => child.kill('SIGTERM'), 900000);
  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timer); reject(Error('device_spawn_failed')); });
      child.stdout.on('data', bytes => { if (bytes.toString().includes(`http://127.0.0.1:${port}/`)) {
        clearTimeout(timer); resolve();
      } });
    });
    phase = 'browser_open';
    await invoke(['open', `http://127.0.0.1:${port}/`]);
    if (fileStartupDiagnostic) {
      phase = 'startup_observer_install';
      const {installFileStartupObservation} = require('./file_startup_observation.cjs');
      assert.equal(await evaluate('(' + installFileStartupObservation.toString() + ')(globalThis)'), true,
        'file_startup_observer_install_failed');
    }
    phase = 'media_activate';
    await invoke(['click', '#activate']);
    phase = 'session_ready';
    await wait('document.getElementById("start").disabled === false');
    phase = 'observer_install';
    await evaluate((scenarioName === 'live' ? 'globalThis.__urMcpLiveReadSnapshot=(' + read.toString() + ');' : '') +
      '(' + install.toString() + ')()');
    if (contextInputDiagnostic) {
      assert.equal(await evaluate('(' + installInputDiagnostic.toString() + ')(true)'), true, 'context_input_observer_install');
    }
    // 观察器双链安装：串联链失败轮可从失败报告的round旁证实时获得三闭环诊断；持续采集开始仍仅全双工需要。
    await evaluate('(' + installRoundObserver.toString() + ')(' + (fileStartupDiagnostic ? 'true' : '') + ')');
    if (plan.chain === 'realtime_native') {
      phase = 'continuous_input_start'; await invoke(['click', '#start']);
    }

    // submit 只输入一次有限语料；识别后不把工具未知解释成需要重新说一遍。
    const submit = async value => {
      if (contextInputDiagnostic) {
        const expected = require('../../../.temp/task143-ipc-env/mcp-context-fixed-corpus.cjs').resolve(value);
        await evaluate(`__urMcpInputDiagnostic.arm(${JSON.stringify(expected)});true`);
      }
      await evaluate(`__urMcpObserver.keywords=${JSON.stringify(value.keywords)};__urMcpObserver.recognition=false;true`);
      // 全双工arm失败即判据失败；串联链只旁读诊断，上一轮未闭环时记录拒绝事实并保留旧轮残态，不破坏流程。
      if (plan.chain === 'realtime_native')
        await evaluate(`__urMcpRound.arm(${JSON.stringify(value.keywords)})`);
      else
        await evaluate(`(()=>{try{__urMcpRound.arm(${JSON.stringify(value.keywords)});` +
          `globalThis.__urMcpRoundArmFault=null;return true}` +
          `catch(error){globalThis.__urMcpRoundArmFault=String(error&&error.message||'invalid_round_arm');return false}})()`);
      if (fileStartupDiagnostic) await evaluate(`__urMcpRound.diagnosticArm(${JSON.stringify(value.keywords)})`);
      if (value.text !== undefined) {
        await invoke(['fill', '#textInput', value.text]);
        await invoke(['click', '#sendText']);
        return;
      }
      const bytes = fs.readFileSync(value.wav);
      assert.ok(bytes.length > 44 && bytes.length <= 480000, 'invalid_voice_fixture_size');
      await evaluate('globalThis.__urMcpVoiceParts=[];true');
      for (let offset = 0; offset < bytes.length; offset += 16384)
        await evaluate(`__urMcpVoiceParts.push(${JSON.stringify(bytes.subarray(offset, offset + 16384).toString('base64'))});true`);
      await evaluate(`(async()=>{
        const chunks=__urMcpVoiceParts.map(v=>Uint8Array.from(atob(v),c=>c.charCodeAt(0)));
        const bytes=new Uint8Array(chunks.reduce((n,v)=>n+v.length,0));let offset=0;
        for(const part of chunks){bytes.set(part,offset);offset+=part.length;}
        globalThis.__urMcpVoiceParts=[];
        const decoded=await context.decodeAudioData(bytes.buffer);
        if(decoded.numberOfChannels!==1||decoded.duration<=0||decoded.duration>12)throw Error('invalid_voice_fixture');
        const speech=context.createBufferSource();speech.buffer=decoded;speech.connect(worklet);
        globalThis.__urMcpVoice=speech;globalThis.__urMcpVoiceEnded=false;
        speech.onended=()=>{speech.disconnect();globalThis.__urMcpVoiceEnded=true;};return true;
      })()`);
      await prepareFixtureCapture(plan.chain, {start: () => invoke(['click', '#start']), wait});
      await evaluate('__urMcpVoice.start(context.currentTime+0.05);true');
      await wait('__urMcpVoiceEnded === true');
      if (plan.chain === 'cascaded_agentruntime') await invoke(['click', '#stop']);
    };
    const execute = async (step, kind, value) => {
      phase = `${kind || 'ai'}_${step}`;
      const before = await snapshot();
      const beforeCompletedResponses = await evaluate('metrics.completedResponses');
      replyScope = {input:value.text !== undefined ? 'text' : 'voice',beforeCompletedResponses,beforeVoiceMS:previousVoiceMS};
      replyWait = null; replyBegin = null; // 下一轮失败不得带入上一轮原等待的样本。
      const evidenceBefore = scenario.readEvidence?.(env);
      await submit(value);
      // 同时等待工具正式结果与对应设备事实；pending不能让此等待完成。
      stepWait = 'toolDevice';
      await wait(`(()=>{try{const assert={equal:(a,b)=>{if(a!==b)throw Error('mismatch');},
        ok:v=>{if(!v)throw Error('missing');}};const tools=${JSON.stringify(scenarioTools)};
        const before=${JSON.stringify(before)};const after=(${read.toString()})();
        (${verify.toString()})(${JSON.stringify(step)},${JSON.stringify(kind)},before,after,${JSON.stringify(plan.chain)});
        return true;}catch{return false;}})()`);
      const observed = await snapshot(); // 首次满足工具及设备事实仍只是早期观察，不代表回复整轮完成。
      verify(step, kind, before, observed, plan.chain);
      stepWait = plan.chain === 'cascaded_agentruntime' ? 'replyDrain' : null;
      replyWait = plan.chain === 'cascaded_agentruntime' && fileStartupDiagnostic ? {observeReply:true} : null;
      if (fileStartupDiagnostic) replyBegin = projectReplyDiagnostic(await evaluate('(' + readReplyDiagnostic.toString() + ')()').catch(() => null),replyScope);
      await waitFixtureReply(plan.chain, beforeCompletedResponses, wait, replyWait);
      stepWait = plan.chain === 'realtime_native' ? 'networkRound' : null;
      await waitFixtureNetworkRound(plan.chain, wait);
      stepWait = null;
      // 半双工原网络终态与实际排空后重新核验，防止晚到工具或加载被早期PASS掩盖；不提交任何新输入。
      // 全双工仅等精确本轮网络终态，不等本地排空；这是单意图工具评估，不替代通用播放中插话用例。
      const after = await snapshot(); // 两种模式都以各自原终态边界后的真实事实记录结果。
      let replyDiagnostic; // 只在显式诊断中读已有原回复冻结时刻，不影响业务验证结果。
      if (fileStartupDiagnostic && replyDiagnosticsCount < 65) {
        ++replyDiagnosticsCount;
        if (replyDiagnosticsCount === 65) replyDiagnostic = {known:false,limitReached:true};
        else {
          const raw = await evaluate('(' + readReplyDiagnostic.toString() + ')()').catch(() => null);
          replyDiagnostic = {input:replyScope.input,begin:replyBegin,completed:projectReplyDiagnostic(raw,replyScope)};
          // 只有严格投影成功的本轮冻结锚点可作为后续反残留水位；未知保持未知。
          previousVoiceMS = replyDiagnostic.completed.known && raw?.timing?.responseId === raw?.roundReplyId &&
            Number.isSafeInteger(raw?.timing?.lastVoiceSentMs) ? raw.timing.lastVoiceSentMs : null;
        }
      }
      const verifiedToolCount = verify(step, kind, before, after, plan.chain);
      if (plan.chain === 'realtime_native') verifyRoundFacts(after.round,
        verifiedToolCount === undefined ? 1 : verifiedToolCount); // Live自身仍硬1，不按未经核验的returned长度授权。
      // Live证据核验会绑定父/子关联，故仅以最终快照调用一次，不能重复改变原证据作用域。
      if (scenario.verifyEvidence) scenario.verifyEvidence(step, evidenceBefore, scenario.readEvidence(env), before, after, evidenceScope);
      results.push({phase, passed: true, captured: after.captured, overlap: after.overlap,
        ...(fileStartupDiagnostic ? {replyDiagnostic:replyDiagnostic || {known:false,limitReached:true}} : {}),
        ...(contextInputDiagnostic ? {inputDiagnostic: await evaluate('__urMcpInputDiagnostic.facts()')} : {}),
        toolsReturned: after.tools.length - before.tools.length,
        ...(scenarioName === 'file' ? {businessActionCount: 1, readOnlyQueryCount: verifiedToolCount - 1,
          readOnlyRecoveryCount: after.tools.slice(before.tools.length).filter(value =>
            value.name === 'things_iot_mcp.recover_media_playback_operation').length} : {}),
        ...scenario.result(step, after)});
    };
    await execute('initial', null, plan.initial);
    for (const program of plan.programs)
      for (const step of scenarioSteps) {
        // 暂停期间公共源继续前进；不发协议命令或重复语料，只等待真实浏览器单调时钟跨过两秒。
        if (scenarioName === 'live' && step === 'resumeAudioAfterPause')
          await wait('globalThis.__urMcpObserver.pauseAt !== null && desktopLive.context.currentTime - __urMcpObserver.pauseAt >= 2');
        // Live正常结束由原Owner释放；先等原设备实际关闭/释放，再说一次只读查询语料，不重试未知工具。
        if (scenarioName === 'live' && step === 'inspectReleasedLive')
          await wait('(()=>{const v=__urMcpLiveReadSnapshot(),r=v.programReleaseReceipt;return v.live===null && v.pcmDetached && v.stoppedGroups>0 && r?.code===200 && r.state==="released" && r.revision>v.groupAcquiredRevision;})()');
        await execute(step, program.kind, program.inputs[step]);
      }
    phase = 'parent_end';
    await endSessionViaUI((...args) => invoke(args));
    await wait(scenarioName === 'file' ?
      'context.state === "closed" && stream.active === false && desktopFiles.player === null' :
      'context.state === "closed" && stream.active === false && desktopLive.player === null && __urMcpObserver.samplingStopped === true');
    const code = await Promise.race([done, new Promise((_, reject) => {
      const timer = setTimeout(() => reject(Error('device_exit_timeout')), 10000); timer.unref();
    })]);
    assert.equal(code, 0, 'device_exit_failure');
    successReport = {passed: true, configuredExecutionChain: plan.chain,
      backendExecutionChainVerified: false, results,
      boundary: scenarioName === 'file' ? 'explicit_model_calls_real_software_media_not_automatic_workflow_or_hardware_AEC' :
        'continuous_source_real_PCM_frequency_and_H264_pixel_second_markers_not_hardware_AEC',
      deviceFocusReceiptVerified: true, focusReleaseLedgerVerified: false, released: true};
  } catch (error) {
    const uiFacts = await evaluate('(' + readStartupFacts.toString() + ')(' +
      (env.UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS === '1' ? 'true' : '') + ')').catch(() => null);
    failedReport = {passed: false, configuredExecutionChain: plan.chain, phase, results,
      observed: await snapshot().catch(() => null), diagnostic: {
        ...startupDiagnostic(phase, error, uiFacts, null), browserCommand: browserCommandFailure,
        stepWait,
        replyWait,
        replyDrain: await evaluate(replyWait ? '(()=>{const facts=(' + readReplyDrainFacts.toString() +
          ')();return {...facts,waitSample:(' + readReplyDrainSample.toString() + ')(' + replyWait.beginMS + ')' +
          (replyWait.observeReply === true ? ',waitDiagnostic:(' + readReplyWaitDiagnostic.toString() + ')(' + replyWait.beginMS + ')' : '') + '}})()' :
          '(' + readReplyDrainFacts.toString() + ')()').catch(() => ({known: false})),
        uiFactsCapture: 'before_script_cleanup',
        deviceLifecycleBeforeCleanup: {...deviceLifecycleObserver.facts(), childClosed: exited, exitCode}}};
    if (fileStartupDiagnostic) failedReport.diagnostic.fileStartup =
      await evaluate('(' + readFileStartupFacts.toString() + ')()').catch(() => null);
    if (fileStartupDiagnostic) failedReport.diagnostic.replyDiagnostic = {input:replyScope?.input || 'unknown',begin:replyBegin,
      waitSample:projectReplyDiagnostic(replyWait ? await evaluate('(' + readReplyDrainObservation.toString() + ')(' +
        replyWait.beginMS + ')').catch(() => null) : null,replyScope || {}),
      beforeCleanup:projectReplyDiagnostic(await evaluate('(' + readReplyDiagnostic.toString() + ')()').catch(() => null),replyScope || {})};
    if (contextInputDiagnostic) failedReport.diagnostic.inputDiagnostic =
      await evaluate('__urMcpInputDiagnostic.facts()').catch(() => null);
    throw Error(scenarioName === 'file' ? 'mcp_file_media_live_failed' : 'mcp_live_broadcast_live_failed');
  } finally {
    clearTimeout(deadline);
    if (contextInputDiagnostic) await evaluate('__urMcpInputDiagnostic?.release();true').catch(() => false);
    const browserClosed = await invoke(['close']).then(() => true, () => false);
    // 信号受理不代表退出；TERM与KILL各只等有限原close，超时保留失败供外层精确资源治理。
    if (!exited) {
      try {child.kill('SIGTERM');} catch {} // signal异常也只能由原close证明是否退出，不回显自由错误。
      if (!await waitForClose() && !exited) {
        try {child.kill('SIGKILL');} catch {}
        await waitForClose();
      }
    }
    const cleanup = {browserClosed, childClosed: exited, stderrEnded};
    const cleanupPassed = browserClosed && exited && stderrEnded;
    if (!cleanupPassed && !failedReport) failedReport = {passed: false, configuredExecutionChain: plan.chain,
      phase: 'cleanup', results, observed: null,
      diagnostic: startupDiagnostic('cleanup', Error('mcp_media_cleanup_failed'), null, null)};
    if (failedReport) {
      failedReport.diagnostic.deviceFailure = deviceFailure;
      failedReport.diagnostic.browserCommand = browserCommandFailure;
      failedReport.diagnostic.childLifecycle = {spawned, closed: exited, exitCode, stderrEnded, ...deviceFailureObserver.facts()};
      failedReport.diagnostic.deviceLifecycle = deviceLifecycleObserver.facts();
      failedReport.diagnostic.cleanup = cleanup;
      console.log(JSON.stringify(failedReport)); // 无固定输出仍为unknown/null，不从cleanup关闭或历史403推导原因。
    } else if (cleanupPassed) console.log(JSON.stringify({...successReport,
      deviceLifecycle: deviceLifecycleObserver.facts()})); // 唯一PASS仍在原实际资源退出之后；新增只读诊断不参与通过判据。
    if (!cleanupPassed) throw Error('mcp_media_cleanup_failed');
  }
}

module.exports = {steps, tools, endSessionViaUI, validatePlan, installObserver, readSnapshot, readStartupFacts, startupDiagnostic, readReplyDrainFacts,
  fileStartupDiagnosticRequested, readFileStartupFacts,
  readReplyDiagnostic, readReplyDrainObservation, projectReplyDiagnostic, evaluateReplyWaitDiagnostic, readReplyWaitDiagnostic,
  installRoundObserver, installInputDiagnostic, verifyRoundFacts, waitFixtureNetworkRound,
  deviceFailureCategory, createDeviceFailureObserver, createDeviceLifecycleObserver, runtimeFailureCategory, clientFailureCategory,
  browserFailureCategory, browserFailureFacts, readReplyDrainSample, verifyStep, prepareFixtureCapture, waitFixtureReply, run};
if (require.main === module) run().catch(() => {
  console.error('mcp_file_media_live_failed'); process.exitCode = 1;
});
