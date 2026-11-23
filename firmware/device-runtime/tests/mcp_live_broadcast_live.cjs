// 文件说明：原AI会话真实模型→MCP→连续节目→C++设备验收入口；只观测实际PCM/可见H264，不创建授权或调用协议控制。
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');

// steps 固定单意图顺序，原完整组实际释放以后才允许后继邀请；直播没有文件load/seek。
const steps = ['invite', 'suspendAi', 'acquireLive', 'resumeAudio', 'resumeVideo', 'pauseAudio', 'pauseVideo',
  'resumeAudioAfterPause', 'resumeVideoAfterPause', 'stopAudio', 'stopVideo', 'inspectReleasedLive', 'restoreAi'];
// tools 是源码白名单，计划只含语料，不能改变模型调用契约。
const tools = Object.freeze(Object.fromEntries(['initial', ...steps].map(step => [step,
  step === 'invite' ? 'invite_live_broadcast' :
    step === 'inspectReleasedLive' ? 'get_output_focus_status' :
    ['initial', 'suspendAi', 'acquireLive', 'restoreAi'].includes(step) ? 'control_output_focus' : 'control_live_track'])));

// exact 拒绝授权、设备身份、URL、原操作token和动态执行字段。
function exact(value, fields) {
  assert.ok(value && typeof value === 'object' && !Array.isArray(value), 'invalid_plan_object');
  assert.deepEqual(Object.keys(value).sort(), fields.slice().sort(), 'invalid_plan_fields');
}
// validatePlan 固定两次完整音视频节目及27条语音；节目许可、原父和设备身份全部由正式后端派生。
function validatePlan(plan) {
  exact(plan, ['chain', 'initial', 'programs']);
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(plan.chain), 'invalid_execution_chain');
  const voice = value => {
    exact(value, ['wav', 'keywords']);
    assert.ok(typeof value.wav === 'string' && path.isAbsolute(value.wav), 'invalid_wav_path');
    assert.ok(Array.isArray(value.keywords) && value.keywords.length > 0 && value.keywords.length <= 8 &&
      value.keywords.every(word => typeof word === 'string' && word.length > 0 && word.length <= 40 &&
        !/[\x00-\x1f\x7f]/.test(word)), 'invalid_input_keywords');
  };
  voice(plan.initial);
  assert.ok(Array.isArray(plan.programs) && plan.programs.length === 2, 'two_successive_programs_required');
  for (const program of plan.programs) {
    exact(program, ['kind', 'programRef', 'inputs']);
    assert.equal(program.kind, 'audioVideo');
    assert.match(program.programRef, /^[A-Za-z0-9_.:-]{1,128}$/, 'invalid_program_reference');
    exact(program.inputs, steps);
    for (const step of steps) voice(program.inputs[step]);
  }
  return plan;
}

// installObserver 包裹原UI出口一次，不生成调用或事实；分析器仅连接原Worklet输出，不连接新的播放destination。
function installObserver() {
  if (globalThis.__urMcpObserver) throw Error('observer_already_installed');
  const observed = {tools: [], gates: [], tracks: [], stoppedGroups: 0, capturedWhileLivePlaying: 0,
    recognition: false, keywords: [], captureOpen: false, overflow: false, instanceCount: 0,
    lastMarker: null, pauseMarker: null, pauseAt: null, lastAudioMarker: null, pauseAudioMarker: null,
    pauseAudioAt: null, audioActiveAt: null, groupAcquiredRevision: 0, groupAcquiredReceiptIndex: 0,
    pcmWindows: 0, nonSilentWindows: 0,
    fresh: {audio: 0, video: 0}, samplingTimer: null, samplingStopped: false,
    toolDiagnostic: {startedCount: 0, pendingCount: 0, orphanResultCount: 0, terminalCount: 0,
      sequence: 0, firstTerminalSequence: null, events: [], observationTruncated: false}};
  globalThis.__urMcpObserver = observed;
  const calls = new Map(); // 原call标识仅在浏览器有界关联，不进报告。
  // observeTool 只记录原UI已接纳的工具/终态局部顺序；不含标识/正文，诊断截断不改变媒体通过标准。
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
  const originalIncoming = incoming;
  incoming = function(value) {
    const returned = originalIncoming(value);
    if (value.kind === 'toolStatus') {
      if (value.state === 'started') {
        const known = ['control_output_focus', 'get_output_focus_status', 'invite_live_broadcast',
          'control_live_track', 'get_live_broadcast_status', 'recover_media_playback_operation'];
        if (calls.size >= 128) observed.overflow = true;
        else {
          // 两条正式模型链只允许裸名和精确服务全名，不能用其它服务的同后缀误配。
          const name = known.find(tool => value.name === tool || value.name === 'things_iot_mcp.' + tool);
          calls.set(value.id, name ? 'things_iot_mcp.' + name : 'other_authorized_tool');
        }
        observeTool('started');
      } else if (calls.has(value.id) && ['succeeded', 'failed', 'unknown'].includes(value.state)) {
        if (observed.tools.length >= 128) observed.overflow = true;
        else observed.tools.push({name: calls.get(value.id), state: value.state});
        calls.delete(value.id);
        observeTool('result');
      } else if (['succeeded', 'failed', 'unknown'].includes(value.state)) {
        observeTool('orphan_result');
      }
    } else if (value.kind === 'text' && typeof value.text === 'string' && value.text.startsWith('\n用户：')) {
      const recognized = value.text.slice(4).split('\n助手：')[0];
      observed.recognition = observed.keywords.every(word => recognized.includes(word));
    } else if (value.kind === 'captureStart') observed.captureOpen = true;
    else if (value.kind === 'captureStop') observed.captureOpen = false;
    else if (['ended', 'error'].includes(value.kind)) observeTool(value.kind);
    return returned;
  };
  const observeSend = (value, returned) => {
    if (!returned) return returned;
    if (value.kind === 'captured' && desktopLive?.player?.metrics.audioActive)
      ++observed.capturedWhileLivePlaying;
    if (value.kind === 'liveLaneGated' && ['audio', 'video', 'audioVideo'].includes(value.lane) && typeof value.enabled === 'boolean') {
      if (observed.gates.length >= 128) observed.overflow = true;
      else observed.gates.push({lane: value.lane, enabled: value.enabled});
    }
    if (value.kind === 'liveTrackFact' && desktopLive?.player?.binding.instance === value.instance &&
        ['broadcast-out', 'broadcast-video-out'].includes(value.trackId) &&
        Number.isSafeInteger(value.generation) && value.generation > 0 && Number.isSafeInteger(value.code) &&
        (value.code === 200 && ['active', 'paused', 'stopped'].includes(value.state) || value.code >= 400 && value.code <= 599)) {
      if (observed.tracks.length >= 128) observed.overflow = true;
      else observed.tracks.push({track: value.trackId === 'broadcast-out' ? 'audio' : 'video',
        generation: value.generation, code: value.code, state: value.code === 200 ? value.state : 'failed',
        instance: observed.instanceCount});
      if (value.trackId === 'broadcast-video-out' && value.code === 200 && value.state === 'paused') {
        observed.pauseMarker = observed.lastMarker;
        observed.pauseAt = desktopLive.context.currentTime;
      }
      if (value.trackId === 'broadcast-out' && value.code === 200) {
        if (value.state === 'active') observed.audioActiveAt = desktopLive.context.currentTime;
        else {
          observed.audioActiveAt = null;
          if (value.state === 'paused') {
            observed.pauseAudioMarker = observed.lastAudioMarker;
            observed.pauseAudioAt = desktopLive.context.currentTime;
          }
        }
      }
    }
    if (value.kind === 'liveStopped' && desktopLive?.player?.binding.instance === value.instance &&
        ['peerClosed', 'tracksStopped', 'outputsDetached', 'callbacksStopped',
      'transformsStopped'].every(field => value[field] === true)) ++observed.stoppedGroups;
    return returned;
  };
  const originalSend = send;
  send = function(value) {return observeSend(value, originalSend.call(this, value));};
  // 宿主在activate阶段已保存旧send引用；只包裹该原出口，后续player仍继承原一次发送和原返回值。
  const originalLiveSend = desktopLive.send;
  desktopLive.send = function(value) {return observeSend(value, originalLiveSend.call(this, value));};
  // 原语音采集链保持开启；只输入一次固定语料，不自动重复模型轮次。
  source.disconnect();
  const silence = context.createConstantSource(); silence.offset.value = 0; silence.connect(worklet); silence.start();
  globalThis.__urMcpSilence = silence;
  // 用户语料与模型等待期间源仍推进；固定只读采样保持真实暂停边沿基线，不发消息或改变输出。
  if (typeof globalThis.__urMcpLiveReadSnapshot === 'function') observed.samplingTimer = setInterval(() => {
    try {globalThis.__urMcpLiveReadSnapshot();} catch {observed.overflow = true;}
  }, 100);
  return true;
}

// readSnapshot 仅读原可见canvas和原输出PCM；固定测试源频率编码秒标，跨秒混窗无法归类时继续等待而不放宽阈值。
function readSnapshot() {
  const observer = globalThis.__urMcpObserver, player = desktopLive?.player;
  if (desktopLive.context.state === 'closed' && observer.samplingTimer !== null) {
    clearInterval(observer.samplingTimer); observer.samplingTimer = null; observer.samplingStopped = true;
  }
  const sensor = globalThis.__urMcpLiveSensor;
  if (sensor && sensor.player !== player) {
    // 正式player.close已断开原node全部连接；只有尚未实际关闭的原节点需要移除此分析支路。
    if (!sensor.player.closed) sensor.node.disconnect(sensor.analyser);
    for (const remove of sensor.removes.values()) remove();
    globalThis.__urMcpLiveSensor = null;
  }
  if (player && !globalThis.__urMcpLiveSensor) {
    const analyser = desktopLive.context.createAnalyser(); analyser.fftSize = 32768;
    analyser.channelCount = 1; analyser.channelCountMode = 'explicit';
    player.lane.node.connect(analyser); // 分析器不连接destination，不改变原实际输出或原许可。
    globalThis.__urMcpLiveSensor = {player, node: player.lane.node, analyser, removes: new Map(), lastSample: -1};
    ++observer.instanceCount;
    observer.lastMarker = null; observer.pauseMarker = null; observer.pauseAt = null;
    observer.lastAudioMarker = null; observer.pauseAudioMarker = null; observer.pauseAudioAt = null;
    observer.audioActiveAt = player.metrics.audioActive ? desktopLive.context.currentTime : null;
    observer.fresh = {audio: 0, video: 0};
    observer.groupAcquiredRevision = 0; observer.groupAcquiredReceiptIndex = 0;
  }
  const current = globalThis.__urMcpLiveSensor;
  const now = desktopLive.context.currentTime;
  let pcmEnergy = null, marker = null, pixels = false, frequency = null, audioMarker = null, videoCleared = false;
  if (player?.lane.enabled) {
    for (const receipt of metrics.focusReceipts || [])
      if (receipt.operation === 'acquire' && receipt.state === 'acquired' && receipt.code === 200 &&
        Number.isSafeInteger(receipt.revision) && receipt.revision > observer.groupAcquiredRevision &&
        Number.isSafeInteger(receipt.receiptIndex) && receipt.receiptIndex > observer.groupAcquiredReceiptIndex) {
        observer.groupAcquiredRevision = receipt.revision; observer.groupAcquiredReceiptIndex = receipt.receiptIndex;
      }
  }
  if (current) {
    for (const [kind, transform] of player.transforms) if (!current.removes.has(kind)) {
      current.removes.set(kind, transform.channel.subscribe(value => {
        if (value.kind === 'liveEncodedFresh' && Number.isSafeInteger(value.generation) && value.generation > 0)
          observer.fresh[kind] = value.generation; // 不存本地票据、SDP或媒体字节。
      }));
    }
    const buffer = new Float32Array(32768); current.analyser.getFloatTimeDomainData(buffer);
    const samples = buffer.subarray(buffer.length - 24000); // 实际48k单声道末500ms窗，不保存原PCM。
    if (desktopLive.context.sampleRate === 48000 && samples.every(Number.isFinite)) {
      pcmEnergy = samples.reduce((sum, sample) => sum + sample * sample, 0) / samples.length;
      const activeWindow = player.metrics.audioActive && observer.audioActiveAt !== null && now - observer.audioActiveAt >= 0.5;
      if (pcmEnergy > 1e-8 && activeWindow) {
        let crossings = 0;
        for (let index = 1; index < samples.length; index++)
          if (samples[index - 1] <= 0 && samples[index] > 0) ++crossings;
        frequency = crossings * 48000 / samples.length;
        const second = Math.round((frequency - 440) / 40);
        if (second >= 0 && second < 32 && Math.abs(frequency - (440 + second * 40)) <= 8) {
          audioMarker = second; observer.lastAudioMarker = audioMarker;
        }
      }
      if (now - current.lastSample >= 0.5) {
        current.lastSample = now; ++observer.pcmWindows;
        if (pcmEnergy > 1e-8 && activeWindow) ++observer.nonSilentWindows;
      }
    }
    const canvas = player.lane.canvas;
    const style = getComputedStyle(canvas), rect = canvas.getBoundingClientRect();
    let visible = !canvas.hidden && style.display !== 'none' && style.visibility === 'visible' &&
      Number(style.opacity) > 0 && rect.width > 0 && rect.height > 0;
    for (let ancestor = canvas.parentElement; ancestor; ancestor = ancestor.parentElement) {
      const parentStyle = getComputedStyle(ancestor);
      visible &&= parentStyle.display !== 'none' && parentStyle.visibility === 'visible' && Number(parentStyle.opacity) > 0;
    }
    if (!player.metrics.videoActive && style.display === 'none') {
      const rgba = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
      videoCleared = rgba.every((value, index) => index % 4 === 3 || value === 0);
    }
    if (player.metrics.videoActive && visible && canvas.width === 320 && canvas.height === 240) {
      const drawing = canvas.getContext('2d');
      const rgba = drawing.getImageData(0, 0, 320, 240).data;
      let colored = 0;
      for (let index = 320 * 64 * 4; index < rgba.length; index += 4)
        if (Math.max(rgba[index], rgba[index + 1], rgba[index + 2]) - Math.min(rgba[index], rgba[index + 1], rgba[index + 2]) > 30) ++colored;
      pixels = colored > 1000;
      let second = 0, valid = true;
      for (let bit = 0; bit < 5; bit++) {
        const offset = (28 * 320 + 28 + bit * 40) * 4;
        const white = rgba[offset] > 200 && rgba[offset + 1] > 200 && rgba[offset + 2] > 200;
        const black = rgba[offset] < 55 && rgba[offset + 1] < 55 && rgba[offset + 2] < 55;
        if (!white && !black) valid = false;
        if (white) second |= 1 << bit;
      }
      if (valid && pixels) {marker = second; observer.lastMarker = marker;}
    }
  }
  return {tools: observer.tools.slice(), gates: observer.gates.slice(), tracks: observer.tracks.slice(),
    toolDiagnostic: {...observer.toolDiagnostic, events: observer.toolDiagnostic.events.map(value => ({...value}))},
    groupAcquiredRevision: observer.groupAcquiredRevision, groupAcquiredReceiptIndex: observer.groupAcquiredReceiptIndex,
    programReleaseReceipt: (metrics.focusReceipts || []).filter(value => value.operation === 'release' &&
      value.state === 'released' && value.code === 200 && value.revision > observer.groupAcquiredRevision &&
      value.receiptIndex > observer.groupAcquiredReceiptIndex).map(value => ({operation: value.operation,
      state: value.state, code: value.code, revision: value.revision, receiptIndex: value.receiptIndex})).at(-1) || null,
    focusReceipts: (metrics.focusReceipts || []).map(value => ({state: value.state, revision: value.revision,
      operation: value.operation, code: value.code, receiptIndex: value.receiptIndex})), focusReceiptIndex: metrics.focusReceiptIndex,
    captured: metrics.captured, overlap: observer.capturedWhileLivePlaying, recognition: observer.recognition,
    captureOpen: observer.captureOpen, overflow: observer.overflow, consumedNonSilent: metrics.consumedNonSilent,
    mode: metrics.interactionMode, contextState: context?.state, streamActive: stream?.active,
    stoppedGroups: observer.stoppedGroups, instanceCount: observer.instanceCount, pcmDetached: !current,
    pcmWindows: observer.pcmWindows, nonSilentWindows: observer.nonSilentWindows, time: now,
    pauseAt: observer.pauseAt, pauseMarker: observer.pauseMarker,
    pauseAudioAt: observer.pauseAudioAt, pauseAudioMarker: observer.pauseAudioMarker,
    live: player ? {instance: observer.instanceCount, audioActive: player.metrics.audioActive,
      videoActive: player.metrics.videoActive, frames: player.metrics.visibleFrames,
      connected: player.connected, outputEnabled: player.lane.enabled, pcmEnergy, pixels, marker, frequency, audioMarker, videoCleared,
      freshAudioGeneration: observer.fresh.audio, freshVideoGeneration: observer.fresh.video} : null};
}

// verifyStep 只允许原模型单次工具结果和实际设备事实共同完成，不把历史active或未知当当前播放。
function verifyStep(step, kind, before, after, chain) {
  assert.equal(after.overflow, false, 'observer_overflow');
  const returned = after.tools.slice(before.tools.length);
  assert.equal(returned.length, 1, 'unexpected_or_repeated_tool');
  assert.equal(returned[0]?.name, 'things_iot_mcp.' + tools[step], 'formal_tool_missing');
  assert.equal(returned[0]?.state, 'succeeded', 'tool_result_unknown_or_failed');
  const focus = {initial: 'acquire', suspendAi: 'suspend', acquireLive: 'acquire', restoreAi: 'restore'}[step];
  if (focus) {
    assert.ok(Array.isArray(before.focusReceipts) && Array.isArray(after.focusReceipts) && after.focusReceipts.length <= 32 &&
      Number.isSafeInteger(before.focusReceiptIndex) && Number.isSafeInteger(after.focusReceiptIndex) &&
      after.focusReceiptIndex > before.focusReceiptIndex, 'original_focus_receipt_missing');
    const receipts = after.focusReceipts.filter(value => value.receiptIndex > before.focusReceiptIndex);
    assert.equal(receipts.length, 1, 'missing_or_repeated_actual_focus_receipt');
    const value = receipts[0], high = Math.max(0, ...before.focusReceipts.map(receipt => receipt.revision));
    assert.equal(value.operation, focus, 'actual_focus_operation_mismatch');
    assert.equal(value.state, {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'}[focus]);
    assert.equal(value.code, 200, 'actual_focus_denied');
    assert.ok(Number.isSafeInteger(value.revision) && value.revision > high &&
      Number.isSafeInteger(value.receiptIndex) && value.receiptIndex <= after.focusReceiptIndex, 'historical_focus_replay');
  }
  if (step === 'invite') assert.ok(after.live?.connected && after.instanceCount === before.instanceCount + 1 &&
    !after.live.audioActive && !after.live.videoActive, 'invitation_not_actual_connected_or_opened_output');
  if (step === 'acquireLive') assert.ok(after.gates.slice(before.gates.length).some(value =>
    value.lane === 'audioVideo' && value.enabled) && after.live?.outputEnabled, 'complete_group_not_actually_open');
  const control = {resumeAudio: ['audio', 'active'], resumeVideo: ['video', 'active'], pauseAudio: ['audio', 'paused'],
    pauseVideo: ['video', 'paused'], resumeAudioAfterPause: ['audio', 'active'], resumeVideoAfterPause: ['video', 'active'],
    stopAudio: ['audio', 'stopped'], stopVideo: ['video', 'stopped']}[step];
  if (control) {
    const facts = after.tracks.slice(before.tracks.length);
    assert.equal(facts.length, 1, 'original_track_fact_missing_or_repeated');
    const fact = facts[0], [track, state] = control;
    assert.equal(fact.track, track); assert.equal(fact.state, state); assert.equal(fact.code, 200);
    assert.equal(fact.instance, before.live?.instance, 'late_original_instance_fact');
    const prior = Math.max(0, ...before.tracks.filter(value => value.instance === fact.instance && value.track === track).map(value => value.generation));
    assert.ok(Number.isSafeInteger(fact.generation) && fact.generation > prior, 'old_track_generation');
    if (state === 'active') {
      assert.ok(after.live?.outputEnabled && after.live[track + 'Active'], 'actual_output_not_active');
      assert.equal(after.live[track === 'audio' ? 'freshAudioGeneration' : 'freshVideoGeneration'], fact.generation,
        'original_fresh_generation_not_observed');
      if (track === 'audio') assert.ok(after.nonSilentWindows > before.nonSilentWindows &&
        Number.isFinite(after.live.pcmEnergy) && after.live.pcmEnergy > 1e-8 &&
        Number.isFinite(after.live.frequency) && Number.isInteger(after.live.audioMarker) &&
        after.live.audioMarker >= 0 && after.live.audioMarker < 32 &&
        Math.abs(after.live.frequency - (440 + after.live.audioMarker * 40)) <= 8, 'actual_PCM_clock_missing');
      else assert.ok(after.live.frames > (before.live?.frames || 0) && after.live.pixels &&
        Number.isInteger(after.live.marker), 'actual_H264_pixels_missing');
    } else if (step !== 'stopVideo') {
      assert.equal(after.live?.[track + 'Active'], false, 'actual_track_not_stopped');
      if (track === 'audio') assert.ok(Number.isFinite(after.live.pcmEnergy) && after.live.pcmEnergy <= 1e-8,
        'audio_output_still_non_silent');
    }
  }
  if (step === 'pauseAudio') assert.ok(after.overlap > before.overlap && after.captured > before.captured && after.recognition,
    'voice_not_recognized_while_actual_program_playing');
  if (step === 'pauseVideo') assert.equal(after.live?.videoCleared, true, 'paused_video_not_actually_cleared');
  // 独立音频控制不能暗停未控制视频；仍须原播放器实际可见新帧，而非仅保留active字符串。
  if (['pauseAudio', 'stopAudio'].includes(step)) assert.ok(after.live?.videoActive &&
    after.live.frames > (before.live?.frames || 0) && after.live.pixels &&
    Number.isInteger(after.live.marker), 'uncontrolled_video_not_actually_continuing');
  if (step === 'resumeAudioAfterPause') {
    // 视频恢复是下一条显式意图；音频恢复期间原暂停画布和帧水位必须保持，不用旧画面假装停流。
    assert.ok(after.live?.videoActive === false && after.live.videoCleared === true &&
      after.live.pixels === false && after.live.frames === before.live?.frames, 'paused_video_reopened_by_audio_control');
    const elapsed = after.time - before.pauseAudioAt;
    const delta = (after.live.audioMarker - before.pauseAudioMarker + 32) % 32;
    assert.ok(Number.isFinite(elapsed) && elapsed >= 2 && elapsed < 28 && Number.isInteger(before.pauseAudioMarker) &&
      delta >= Math.max(1, Math.floor(elapsed) - 1) && delta <= Math.ceil(elapsed) + 1, 'paused_audio_replayed_old_PCM');
  }
  if (['resumeVideo', 'resumeVideoAfterPause'].includes(step)) assert.ok(after.live?.audioActive &&
    Number.isFinite(after.live.frequency) && Number.isInteger(after.live.audioMarker) &&
    Math.abs(after.live.frequency - (440 + after.live.audioMarker * 40)) <= 8, 'actual_AV_audio_clock_missing');
  if (step === 'resumeVideoAfterPause') {
    const elapsed = after.time - before.pauseAt;
    const delta = (after.live.marker - before.pauseMarker + 32) % 32;
    assert.ok(Number.isFinite(elapsed) && elapsed >= 2 && elapsed < 28 && Number.isInteger(before.pauseMarker) &&
      delta >= Math.max(1, Math.floor(elapsed) - 1) && delta <= Math.ceil(elapsed) + 1, 'paused_video_replayed_old_pixels');
    const separation = (after.live.marker - after.live.audioMarker + 32) % 32;
    assert.ok(Number.isInteger(after.live.audioMarker) && [0, 1, 31].includes(separation), 'actual_audio_video_clock_mismatch');
  }
  if (step === 'stopVideo') assert.ok(after.live === null && after.pcmDetached &&
    after.stoppedGroups === before.stoppedGroups + 1, 'original_full_media_group_not_actually_closed');
  if (step === 'inspectReleasedLive') {
    assert.ok(after.live === null && after.pcmDetached && after.stoppedGroups > 0 &&
      after.gates.filter(value => value.lane === 'audioVideo').at(-1)?.enabled === false, 'complete_group_not_actually_closed_before_release');
    const receipt = after.programReleaseReceipt;
    assert.ok(Number.isSafeInteger(after.groupAcquiredRevision) && after.groupAcquiredRevision > 0 &&
      Number.isSafeInteger(after.groupAcquiredReceiptIndex) && after.groupAcquiredReceiptIndex > 0 &&
      receipt && receipt.operation === 'release' && receipt.state === 'released' && receipt.code === 200 &&
      Number.isSafeInteger(receipt.revision) && receipt.revision > after.groupAcquiredRevision &&
      Number.isSafeInteger(receipt.receiptIndex) && receipt.receiptIndex > after.groupAcquiredReceiptIndex &&
      receipt.receiptIndex <= after.focusReceiptIndex, 'original_owner_release_receipt_missing');
  }
  if (['initial', 'restoreAi'].includes(step)) assert.ok(after.consumedNonSilent > before.consumedNonSilent,
    'no_actual_AI_reply_after_focus');
  if (chain === 'realtime_native') {assert.equal(after.mode, 'fullDuplex'); assert.equal(after.captureOpen, true, 'AI_input_stopped');}
  assert.equal(after.contextState, 'running'); assert.equal(after.streamActive, true);
}

// result 输出固定实际计数，不输出节目引用、源地址、原会话或票据；后端落库由独立只读证据验证。
function result(_step, after) {
  return {instance: after.instanceCount, stoppedGroups: after.stoppedGroups, pcmWindows: after.pcmWindows,
    nonSilentWindows: after.nonSilentWindows, visibleFrames: after.live?.frames || 0,
    markerSecond: after.live?.marker ?? null, audioMarkerSecond: after.live?.audioMarker ?? null,
    audioFrequency: after.live?.frequency ?? null, audioActive: after.live?.audioActive || false,
    videoActive: after.live?.videoActive || false};
}
// parseEvidence 严格解析固定透明取证器有限白名单；HMAC只关联原请求，不被解释成用户权限或落库证明。
function parseEvidence(bytes) {
  assert.ok(Buffer.isBuffer(bytes) && bytes.length <= 262144, 'observer_journal_size');
  const lines = bytes.toString('utf8').split('\n');
  assert.equal(lines.pop(), '', 'observer_journal_incomplete');
  assert.ok(lines.length <= 256, 'observer_journal_rows');
  return lines.map(line => {
    assert.ok(line.length > 0 && line.length <= 2048, 'observer_row_size');
    const value = JSON.parse(line);
    const allowed = ['toolName', 'parentHash', 'operationHash', 'inputSHA256', 'grantPresent', 'fallbackAuthPresent',
      'grantConsumptionVerified', 'childHash', 'focusReleasedObserved', 'focusRevision', 'responseVerified',
      'phase', 'httpStatus', 'jsonrpcErrorCode', 'mcpIsError'];
    assert.ok(value && !Array.isArray(value) && Object.keys(value).every(key => allowed.includes(key)), 'observer_private_field');
    assert.ok(['control_output_focus', 'get_output_focus_status', 'invite_live_broadcast', 'control_live_track',
      'get_live_broadcast_status', 'recover_media_playback_operation'].includes(value.toolName), 'observer_tool_unknown');
    assert.match(value.parentHash, /^[0-9a-f]{64}$/); assert.match(value.inputSHA256, /^[0-9a-f]{64}$/);
    for (const hash of [value.operationHash, value.childHash]) assert.ok(hash === null ||
      hash === undefined && value.phase === 'transport_unknown' || typeof hash === 'string' && /^[0-9a-f]{64}$/.test(hash), 'observer_hash_invalid');
    assert.equal(value.grantPresent, true); assert.equal(value.fallbackAuthPresent, false); assert.equal(value.grantConsumptionVerified, false);
    assert.ok(['http_response', 'transport_unknown'].includes(value.phase));
    assert.ok(value.httpStatus === null || Number.isInteger(value.httpStatus) && value.httpStatus >= 100 && value.httpStatus <= 599);
    assert.ok(value.jsonrpcErrorCode === null || Number.isSafeInteger(value.jsonrpcErrorCode));
    assert.ok([true, false, null].includes(value.mcpIsError));
    if (value.phase === 'http_response') {
      assert.equal(typeof value.responseVerified, 'boolean'); assert.equal(typeof value.focusReleasedObserved, 'boolean');
      assert.ok(value.focusRevision === null || Number.isSafeInteger(value.focusRevision) && value.focusRevision > 0);
    }
    return value;
  });
}
// evidenceFile 只定位本任务缺省/1或显式2/3/4的独立journal；非法值不能借计划、自由目录或其它任务替换取证。
function evidenceFile(env){const attempt=env.UR_MCP_LIVE_ATTEMPT;assert.ok(attempt===undefined||['1','2','3','4'].includes(attempt),'fixed_live_attempt_required');return path.resolve(__dirname,'../../../.temp/task143-ipc-env',attempt==='2'||attempt==='3'||attempt==='4'?'mcp-live-broadcast-work-'+attempt:'mcp-live-broadcast-work','observer.jsonl');}
// readEvidence 只打开本任务固定600普通journal，拒绝自由路径/链接/归属变化；不连接平台或读取原Grant/正文。
function readEvidence(env) {
  assert.equal(env.UR_MCP_BROADCAST_EVIDENCE, 'task143', 'explicit_fixed_evidence_required');
  const filename = evidenceFile(env);
  const parent = path.dirname(filename), directory = fs.lstatSync(parent);
  assert.ok(directory.isDirectory() && !directory.isSymbolicLink() && (directory.mode & 0o777) === 0o700 &&
    fs.realpathSync(parent) === parent && typeof process.getuid === 'function' && directory.uid === process.getuid(), 'observer_directory_invalid');
  const before = fs.lstatSync(filename);
  assert.ok(before.isFile() && !before.isSymbolicLink() && (before.mode & 0o777) === 0o600 && before.uid === process.getuid(), 'observer_file_invalid');
  const fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const actual = fs.fstatSync(fd);
    assert.ok(actual.dev === before.dev && actual.ino === before.ino && actual.isFile() && actual.uid === before.uid &&
      (actual.mode & 0o777) === 0o600 && actual.size <= 262144, 'observer_file_changed');
    return parseEvidence(fs.readFileSync(fd));
  } finally {fs.closeSync(fd);}
}
// verifyEvidence 关联本轮原父/当前邀请子及唯一原HTTP结果；自动Owner释放只由原query事实和设备回执共同确认。
function verifyEvidence(step, before, after, _deviceBefore, deviceAfter, scope) {
  assert.ok(after.length === before.length + 1 && before.every((value, index) => JSON.stringify(value) === JSON.stringify(after[index])), 'original_observer_history_changed');
  const value = after.at(-1);
  assert.equal(value.toolName, tools[step]); assert.equal(value.phase, 'http_response'); assert.equal(value.httpStatus, 200);
  assert.equal(value.jsonrpcErrorCode, null); assert.ok(value.mcpIsError === false || value.mcpIsError === null);
  assert.equal(value.responseVerified, true, 'original_JSONRPC_result_unverified');
  if (step === 'initial') {assert.equal(scope.parentHash, undefined); scope.parentHash = value.parentHash;}
  assert.equal(value.parentHash, scope.parentHash, 'original_AI_parent_changed');
  if (step === 'invite') {
    assert.match(value.childHash, /^[0-9a-f]{64}$/); assert.notEqual(value.childHash, scope.childHash, 'original_child_reinvited');
    scope.childHash = value.childHash;
  } else if (['suspendAi', 'restoreAi', 'initial'].includes(step)) assert.equal(value.childHash, null, 'AI_focus_target_changed');
  else assert.equal(value.childHash, scope.childHash, 'original_program_child_changed');
  if (step === 'inspectReleasedLive') {
    assert.equal(value.focusReleasedObserved, true, 'original_live_query_not_released');
    assert.equal(value.focusRevision, deviceAfter.programReleaseReceipt?.revision, 'original_release_revision_mismatch');
  }
}
// run 复用正式文件入口的已验证进程/浏览器/语音生命周期，仅选择固定Live场景；环境由中央预先授权配置。
async function run(env = process.env) {
  assert.equal(env.UR_MCP_BROADCAST_LIVE, '1', 'explicit_live_opt_in_required');
  readEvidence(env);
  return require('./mcp_file_media_live.cjs').run(env, 'live');
}
const scenario = Object.freeze({steps, tools, validatePlan, installObserver, readSnapshot, verifyStep, result, readEvidence, verifyEvidence});
module.exports = {steps, tools, validatePlan, installObserver, readSnapshot, verifyStep, result, parseEvidence, evidenceFile, readEvidence, verifyEvidence, scenario, run};
if (require.main === module) run().catch(() => {console.error('mcp_live_broadcast_live_failed'); process.exitCode = 1;});
