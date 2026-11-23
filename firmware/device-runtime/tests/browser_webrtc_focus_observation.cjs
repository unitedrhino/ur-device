// 文件说明：仅旁观原浏览器 WebRTC Worklet 门控与 C++ 焦点回执；不发命令、不取得焦点、不改变媒体或业务判据。
'use strict';

// installWebRTCFocusObservation 必须显式 true，并在原 app 加载后、激活前注入页面。
// 返回是否安装；read 只给有界固定事实，stop 恢复仍属本观察器的函数。独立 helper 不判真实联调通过。
function installWebRTCFocusObservation(enabled = false) {
  if (enabled !== true) return false;
  if (globalThis.__urWebRTCFocusObservation) return false;
  if (typeof incoming !== 'function' || typeof send !== 'function' ||
      typeof rtcOutputActual !== 'function' || typeof rtcOutputFocus !== 'boolean')
    throw new Error('webrtc_focus_observation_requires_app');
  const originalIncoming = incoming, originalSend = send, originalActual = rtcOutputActual;
  const events = []; // 三类原事实共用64条预算，超限不淘汰失败，不随读取续期或扩容。
  let stopped = false, unknown = false, truncated = false, active = null;
  let initialFalseSubmitted = false, actualTrueSubmitted = false, nativeAcquiredObserved = false;
  // append 只接本观察器构造的固定字段；容量耗尽仍让原媒体函数完整执行。
  function append(value) {
    if (stopped) return;
    if (events.length >= 64) {truncated = true; return;}
    events.push(value);
  }
  // safeObserve 隔离取证读取异常；不抛自由错误，也不替换原函数的返回或异常。
  function safeObserve(callback) {
    if (stopped) return;
    try {callback();} catch {unknown = true;}
  }
  // wrappedSend 仅原 rtcOutputActual 的同步调用可记录本地提交，裸 send 或请求受理不能冒充 ACK。
  function wrappedSend(...args) {
    const result = originalSend.apply(this, args);
    safeObserve(() => {
      const value = args[0];
      if (!active || value?.kind !== 'outputGated' || Object.keys(value).length !== 3 ||
          value.ticket !== active.ticket || value.enabled !== active.enabled ||
          !rtcGateKnown || rtcGatePending !== null || rtcGateTicket.toString() !== active.ticket ||
          rtcGateEnabled !== active.enabled) return;
      if (typeof result !== 'boolean') {unknown = true; return;}
      append({kind: 'workletAck', enabled: active.enabled, localSubmitted: result});
      if (result && active.ticket === '1' && !active.enabled) initialFalseSubmitted = true;
      if (result && active.enabled) actualTrueSubmitted = true;
    });
    return result; // true仅表示原本地WS发送接纳，不是C++、Broker或云端接纳。
  }
  // wrappedActual 捕获原节点和原4字段回执，最终准入仍全由 app.rtcOutputActual 决定。
  function wrappedActual(...args) {
    const previous = active;
    safeObserve(() => {
      const [node, value] = args;
      active = !terminated && node === rtcOutputNode && value?.kind === 'callAudioGated' &&
        Object.keys(value).length === 4 && value.peerEpoch === '0' &&
        typeof value.enabled === 'boolean' && typeof value.ticket === 'string' ?
        {ticket: value.ticket, enabled: value.enabled} : null;
    });
    try {return originalActual.apply(this, args);} finally {active = previous;}
  }
  // wrappedIncoming 原函数先校验请求/回执，再确认它实际更新了原状态；不记录原正文、票据或标识。
  function wrappedIncoming(...args) {
    let result;
    try {result = originalIncoming.apply(this, args);} catch (error) {
      safeObserve(() => {if (['playbackGate', 'focusReceipt'].includes(args[0]?.kind)) unknown = true;});
      throw error; // 原错误对象与处理路径保持，不把失败吞成观察成功。
    }
    safeObserve(() => {
      const value = args[0];
      if (value?.kind === 'playbackGate' && rtcOutputFocus && rtcGatePending &&
          rtcGatePending.ticket === value.ticket && rtcGatePending.enabled === value.enabled)
        append({kind: 'gateRequested', enabled: value.enabled});
      if (value?.kind !== 'focusReceipt') return;
      const receipt = metrics.focusReceipts.at(-1);
      if (!receipt || metrics.focusReceiptIndex !== value.receiptIndex ||
          receipt.receiptIndex !== value.receiptIndex || receipt.state !== value.state ||
          receipt.operation !== value.operation || receipt.code !== value.code ||
          receipt.revision !== value.revision) {unknown = true; return;}
      append({kind: 'nativeReceipt', state: receipt.state, operation: receipt.operation,
        code: receipt.code, revision: receipt.revision, receiptIndex: receipt.receiptIndex});
      if (receipt.operation === 'acquire' && receipt.state === 'acquired' && receipt.code === 200)
        nativeAcquiredObserved = true;
      // 原C++核心结果+本地MQTT接纳的历史回执；不证明当前授权、Broker收到或模型取得焦点。
    });
    return result;
  }
  // read 返回独立投影，布尔只说明曾观察到；调用方仍须核本轮顺序、媒体、权限和未截断完整性。
  function read() {
    return {installed: true, stopped, unknown, truncated, initialFalseSubmitted,
      actualTrueSubmitted, nativeAcquiredObserved, events: events.map(value => ({...value}))};
  }
  // stop 幂等停止采样，仅恢复尚未被其他观察器替换的原函数，不关闭音频或网络。
  function stop() {
    if (stopped) return;
    stopped = true;
    if (incoming === wrappedIncoming) incoming = originalIncoming;
    if (send === wrappedSend) send = originalSend;
    if (rtcOutputActual === wrappedActual) rtcOutputActual = originalActual;
    active = null;
  }
  incoming = wrappedIncoming; send = wrappedSend; rtcOutputActual = wrappedActual;
  globalThis.__urWebRTCFocusObservation = {read, stop};
  return true;
}

module.exports = {installWebRTCFocusObservation};
