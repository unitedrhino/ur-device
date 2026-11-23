// 文件说明：正式宿主启动的有界只读阶段旁证；不修改原Promise、播放意图、控制参数或超时。
'use strict';

// installStartupObservation 在原页面激活前装配固定阶段观察；readHost仅读取原宿主，不创建资源或授权。
function installStartupObservation(scope, readHost = () => scope.desktopLive) {
  if (scope.focusStartupObservation) return false;
  const events = [], frames = new Map(), pending = new Set(); // 96事实/64帧/32在途，满额不淘汰已有证据。
  const origin = scope.performance.now(); // 与Date墙钟无关，不能用观察时点续长驱动预算。
  let active = null, observed = 0; // 只在原presented/rAF同步栈关联等待；倒退时差不能成为新事实时点。
  const lanes = ['audio', 'video', 'audioVideo'];
  const fixedFailure = error => ['live_cancelled', 'live_actual_timeout', 'live_driver_failed',
    'live_listener_full', 'live_port_closed'].includes(error?.message) ? error.message : 'other';
  const timing = {supported: false, entries: [], truncated: false}; // 最多32条原origin之后的数值事实，不含任务名称或归因。
  let timingObserver = null, timingStopped = false, timingFault = false; // 显式停止与原控制/媒体cleanup解耦，硬错误保留未知。
  // timingEntries 只投影标准LongTask的两项数值；旧buffered记录忽略，输入与输出均有界。
  function timingEntries(records) {
    if (!Array.isArray(records)) throw new Error('timing_records_invalid');
    if (records.length > 256) timing.truncated = true;
    for (const entry of records.slice(0, 256)) {
      if (entry?.entryType !== 'longtask') continue;
      const start = entry.startTime, duration = entry.duration;
      if (typeof start !== 'number' || !Number.isFinite(start) || typeof duration !== 'number' ||
          !Number.isFinite(duration) || duration < 0 || duration > Number.MAX_SAFE_INTEGER) continue;
      const relative = start - origin; // 禁止对象/字符串隐式转换，旧或非有限时差不能成为事实。
      if (!Number.isFinite(relative) || relative < 0 || relative > Number.MAX_SAFE_INTEGER) continue;
      if (timing.entries.length >= 32) { timing.truncated = true; break; }
      timing.entries.push({startOffsetMs: relative, durationMs: duration});
    }
  }
  // stopTimingObservation 显式收录原队列后断开并返回同facts引用；失败也断开一次，迟到callback不能续采集。
  function stopTimingObservation() {
    if (timingStopped) return timing;
    timingStopped = true;
    if (timingObserver) {
      try { timingEntries(timingObserver.takeRecords()); }
      catch { timingFault = true; timing.supported = false; }
      finally {
        try { timingObserver.disconnect(); }
        catch { timingFault = true; timing.supported = false; }
      }
    }
    return timing;
  }
  scope.focusStartupTimingFacts = timing;
  scope.stopFocusStartupTimingObservation = stopTimingObservation;
  try {
    const Observer = scope.PerformanceObserver; // 只用原页面能力，不加载polyfill或新计时/绘制任务。
    const supported = Observer?.supportedEntryTypes; // 能力getter也只读一次，异常保持supported=false。
    if (typeof Observer === 'function' && Array.isArray(supported) && supported.includes('longtask')) {
      timingObserver = new Observer(list => {
        if (timingStopped) return;
        try { timingEntries(list.getEntries()); }
        catch { timingFault = true; timing.supported = false; }
      });
      timingObserver.observe({type: 'longtask', buffered: true});
      timing.supported = !timingFault;
    }
  } catch { timingFault = true; timing.supported = false; stopTimingObservation(); }
  // append 只记录原事实时点；未知枚举不反射，容量满后不淘汰已有失败证据。
  function append(phase, lane, success, error, record, timestamp) {
    if (events.length >= 96) return;
    try {
      const host = readHost();
      const visibility = scope.document?.visibilityState;
      const context = host?.context?.state;
      const elapsed = scope.performance.now() - origin;
      const validElapsed = Number.isFinite(elapsed) && elapsed >= observed;
      if (validElapsed) observed = elapsed;
      const frameTime = typeof timestamp === 'number' ? timestamp - origin : NaN; // 原rAF参数只读相对化，不改变回调入参。
      events.push({phase, lane: lanes.includes(lane) ? lane : null, success,
        elapsedMs: validElapsed ? Math.floor(elapsed) : null,
        visibility: ['visible', 'hidden', 'prerender'].includes(visibility) ? visibility : 'unknown',
        context: ['running', 'suspended', 'closed'].includes(context) ? context : 'unknown',
        aborted: host?.life?.signal?.aborted === true,
        failure: success ? null : fixedFailure(error), hasFocus: scope.document?.hasFocus?.() === true,
        actualFrames: record?.actual ?? null, rafCancelled: record?.cancelled === true,
        rafTimestampMs: phase === 'rafActual' && Number.isFinite(frameTime) && frameTime >= 0 &&
          frameTime <= Number.MAX_SAFE_INTEGER ? frameTime : null,
        executionElapsedMs: phase === 'rafActual' && validElapsed && elapsed <= Number.MAX_SAFE_INTEGER ? elapsed : null});
    } catch { /* 只读旁证失败不能影响原控制、帧回调、取消或Promise拒绝。 */ }
  }
  // observe 保留原Promise引用及拒绝，不引入新的await或改变调用方返回值。
  function observe(result, phase, lane, record) {
    if (result && typeof result.then === 'function') result.then(
      () => { append(phase, lane, true, null, record); if (record) pending.delete(record); },
      error => { append(phase, lane, false, error, record); if (record) pending.delete(record); });
    else { append(phase, lane, result !== false, null, record); if (record) pending.delete(record); }
    return result;
  }
  // 原rAF与取消仍由同一页面接收；只在presented及其嵌套回调同步栈内关联旁证。
  const originalFrame = scope.requestAnimationFrame, originalCancel = scope.cancelAnimationFrame;
  scope.requestAnimationFrame = function(callback) {
    const record = active;
    if (!record || frames.size >= 64) return originalFrame.call(scope, callback);
    let frame;
    frame = originalFrame.call(scope, function(...args) {
      frames.delete(frame); ++record.actual;
      append('rafActual', null, true, null, record, args[0]);
      const previous = active; active = record;
      try { return callback.apply(this, args); }
      finally { active = previous; }
    });
    frames.set(frame, record); append('rafScheduled', null, true, null, record);
    return frame; // 原浏览器句柄与回调this/参数/返回不变，旁证不自行交帧。
  };
  scope.cancelAnimationFrame = function(frame) {
    const record = frames.get(frame);
    if (record) {
      record.cancelled = true; append('rafCancelled', null, true, null, record); frames.delete(frame);
    }
    return originalCancel.call(scope, frame);
  };
  // laneOf 只从原宿主实际节点反查固定组名，不借任意对象字段推断媒体身份。
  function laneOf(messages) {
    const host = readHost();
    for (const lane of host?.lanes?.values() ?? [])
      if (lane.audio === messages) return lane.name;
    return null;
  }
  const originalMake = scope.UrLiveHost.prototype.makeAudio;
  scope.UrLiveHost.prototype.makeAudio = function(lane) {
    const result = originalMake.call(this, lane);
    const originalMessage = lane.node.port.onmessage;
    lane.node.port.onmessage = function(event) {
      if (event?.data?.kind === 'liveAudioMasked') append('maskActual', lane.name, true);
      return originalMessage.call(this, event); // 原消息对象、接收者和所有订阅回调保持不变。
    };
    append('nodeCreated', lane.name, true);
    return result;
  };
  const originalPost = scope.UrLiveMessages.prototype.post;
  scope.UrLiveMessages.prototype.post = function(value) {
    const mask = value?.kind === 'liveAudioMask';
    if (mask) append('maskPost', laneOf(this), true);
    try { return originalPost.call(this, value); }
    catch (error) { if (mask) append('maskPostRejected', laneOf(this), false, error); throw error; }
  };
  const originalExpect = scope.UrLiveMessages.prototype.expect;
  scope.UrLiveMessages.prototype.expect = function(...args) {
    const mask = args[0]?.kind === 'liveAudioMask';
    if (mask) append('maskWaitBegin', laneOf(this), true);
    try {
      const result = originalExpect.apply(this, args);
      return mask ? observe(result, 'maskWaitSettled', laneOf(this)) : result;
    } catch (error) {
      if (mask) append('maskWaitSettled', laneOf(this), false, error);
      throw error;
    }
  };
  const originalPresented = scope.UrLiveHost.prototype.presented;
  scope.UrLiveHost.prototype.presented = function(...args) {
    if (pending.size >= 32) return originalPresented.apply(this, args);
    const record = {actual: 0, cancelled: false}; // 原独立等待的实际回调计数，不能拼接另一等待的帧。
    pending.add(record); append('presentationBegin', null, true, null, record);
    const previous = active; active = record;
    try { return observe(originalPresented.apply(this, args), 'presentationSettled', null, record); }
    catch (error) {
      pending.delete(record); append('presentationSettled', null, false, error, record); throw error;
    } finally { active = previous; }
  };
  const originalGate = scope.UrLiveHost.prototype.gate;
  scope.UrLiveHost.prototype.gate = function(value) {
    append('gateReceived', value?.lane, true);
    try { return observe(originalGate.call(this, value), 'gateAccepted', value?.lane); }
    catch (error) { append('gateAccepted', value?.lane, false, error); throw error; }
  };
  scope.focusStartupObservation = events;
  return true;
}
module.exports = {installStartupObservation};
