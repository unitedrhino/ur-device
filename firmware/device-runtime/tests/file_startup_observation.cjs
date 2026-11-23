// 文件说明：原文件门控的有界只读绘制旁证；保留原rAF句柄/Promise/取消，不更改实际成功条件或预算。
'use strict';

// installFileStartupObservation 只观察既有文件路径；无节目/设备身份，不创建节点或代替绘制完成。
function installFileStartupObservation(scope) {
  if (scope.fileStartupObservation) return false;
  const events = [], frames = new Map(), pending = new Set(); // 96事实/64帧/32在途，满额不淘汰旧证据。
  const origin = scope.performance.now();
  let active = null; // 只在原paint和原rAF回调的同步栈内关联路径，不将其他页面动画记作媒体。
  // append 仅读固定路径/样式/生命周期枚举；不反射错误正文或任何原消息。
  function append(phase, lane, success, record, error) {
    if (events.length >= 96) return;
    try {
      const elapsed = scope.performance.now() - origin;
      const visibility = scope.document?.visibilityState;
      const context = lane.context?.state;
      const display = scope.getComputedStyle(lane.mount).display;
      events.push({phase, lane: ['audio', 'video', 'audioVideo'].includes(lane.lane) ? lane.lane : null,
        success, elapsedMs: Number.isFinite(elapsed) && elapsed >= 0 ? Math.floor(elapsed) : null,
        visibility: ['visible', 'hidden', 'prerender'].includes(visibility) ? visibility : 'unknown',
        context: ['running', 'suspended', 'closed'].includes(context) ? context : 'unknown',
        computedDisplay: ['block', 'none'].includes(display) ? display : 'other',
        connected: lane.mount?.isConnected === true, closing: lane.closing === true,
        actualFrames: record?.actual ?? null, paintCancelled: record?.cancelled === true,
        failure: success ? null : ['unknown', 'timeout', 'closed'].includes(error?.message) ? error.message : 'other'});
    } catch { /* 观察不可见也不能打断原媒体控制、回调或原异常。 */ }
  }
  // observe 返回同一个原Promise；拒绝只追加固定旁证，不吞掉原错误或改异步完成时点。
  function observe(result, phase, lane, record) {
    if (result && typeof result.then === 'function') result.then(
      value => { append(phase, lane, value !== false, record); if (record) pending.delete(record); },
      error => { append(phase, lane, false, record, error); if (record) pending.delete(record); });
    return result;
  }
  const originalFrame = scope.requestAnimationFrame, originalCancel = scope.cancelAnimationFrame;
  scope.requestAnimationFrame = function(callback) {
    const record = active;
    if (!record || frames.size >= 64) return originalFrame.call(scope, callback);
    let frame;
    frame = originalFrame.call(scope, function(...args) {
      frames.delete(frame);
      ++record.actual;
      append('rafActual', record.lane, true, record);
      const previous = active;
      active = record;
      try { return callback.apply(this, args); }
      finally { active = previous; }
    });
    frames.set(frame, record);
    append('rafScheduled', record.lane, true, record);
    return frame; // 保留原浏览器句柄，取消/队列交接行为不能被观察改写。
  };
  scope.cancelAnimationFrame = function(frame) {
    const record = frames.get(frame);
    if (record) { append('rafCancelled', record.lane, true, record); frames.delete(frame); }
    return originalCancel.call(scope, frame);
  };
  const originalPaint = scope.UrFileLane.prototype.paint;
  scope.UrFileLane.prototype.paint = function(...args) {
    if (pending.size >= 32) return originalPaint.apply(this, args);
    const record = {lane: this, actual: 0, cancelled: false};
    pending.add(record);
    append('paintBegin', this, true, record);
    const previous = active;
    active = record;
    try {
      const result = originalPaint.apply(this, args);
      const originalPaintCancel = this.cancelPaint;
      if (typeof originalPaintCancel === 'function') this.cancelPaint = function(...cancelArgs) {
        record.cancelled = true;
        append('paintCancelCalled', record.lane, true, record);
        return originalPaintCancel.apply(this, cancelArgs);
      };
      return observe(result, 'paintSettled', this, record);
    } catch (error) { pending.delete(record); append('paintSettled', this, false, record, error); throw error; }
    finally { active = previous; }
  };
  for (const name of ['initialize', 'audioGate', 'gate']) {
    const original = scope.UrFileLane.prototype[name];
    scope.UrFileLane.prototype[name] = function(...args) {
      append(`${name}Begin`, this, true);
      try { return observe(original.apply(this, args), `${name}Settled`, this); }
      catch (error) { append(`${name}Settled`, this, false, null, error); throw error; }
    };
  }
  const originalFact = scope.UrFileLane.prototype.fact;
  scope.UrFileLane.prototype.fact = function(value) {
    if (value?.kind === 'fileAudioGated') append('audioActual', this, true);
    return originalFact.call(this, value); // 原方法继续验证原pending/ticket，收件不等于匹配或整组通过。
  };
  scope.fileStartupObservation = events;
  return true;
}
module.exports = {installFileStartupObservation};
