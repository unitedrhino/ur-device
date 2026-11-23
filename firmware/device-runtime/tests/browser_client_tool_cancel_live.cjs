// 文件说明：真实客户端工具取消竞态探针；仅延迟原网页实际回执并重投原本地票据，不构造平台工具报文。
'use strict';
const assert = require('node:assert/strict');

// installCancellationHarness 保留原命令与实际绘制结果；外层等待原控制投影后点取消，不在早到回调中猜按钮状态。
function installCancellationHarness() {
  if (globalThis.__urToolCancellation) throw Error('cancellation_probe_exists');
  const state = globalThis.__urToolCancellation = {command:null,fact:null,cancelSent:false,released:false,
    originalSend:send,originalIncoming:incoming,originalEmit:clientTheme.platform.emit};
  incoming = function(value) {
    if (value.kind === 'clientThemeSet' && !state.command) state.command = {...value};
    return state.originalIncoming(value);
  };
  clientTheme.platform.emit = function(value) {
    if (value.kind === 'clientThemeActual' && !state.fact && !state.released) {
      state.fact = {...value}; // 原真实样式读回，不制造工具结果或成功回执。
      return true; // 此返回仅表示本地故障注入器持有消息，不是云端受理事实。
    }
    return state.originalEmit(value);
  };
  send = function(value) {
    const sent = state.originalSend(value);
    if (value.kind === 'cancel' && sent === true) state.cancelSent = true;
    return sent;
  };
  return true;
}

// cancellationFacts 只读原状态供内存关联；对外报告由verifyCancellationProbe返回固定布尔量。
function cancellationFacts() {
  const state = globalThis.__urToolCancellation;
  return {held:!!state?.fact,commandHeld:!!state?.command,cancelSent:state?.cancelSent===true,
    actualStatus:state?.fact?.status,records:clientTheme.records.size,theme:document.body.dataset.clientTheme,
    cancelled:metrics.cancelledResponses,completed:metrics.completedResponses,receipts:metrics.clientToolReceiptIndex,
    captureOpen:metrics.captureOpen,pending:pendingPlayback.size,active:responseId !== '',
    terminated,bridgeOpen:!!socket && socket.readyState === WebSocket.OPEN,
    controlReady:controlResponseId !== '',cancelEnabled:!document.getElementById('cancel').disabled};
}

// clickOriginalCancellation 等原严格处理后的本轮唯一工具/核心目标/真实票据事实，再经原按钮取消。
// 由原 wait --fn 轮询，保留既有 CLI 截止；本地工具两秒预算和平台票据期限均不改。
function clickOriginalCancellation(expectedCancelled, toolOffset) {
  const state = globalThis.__urToolCancellation;
  if (!state || !Number.isSafeInteger(expectedCancelled) || expectedCancelled < 0 ||
      !Number.isSafeInteger(toolOffset) || toolOffset < 0 || toolOffset > 16)
    throw Error('invalid_cancel_probe_scope');
  if (state.inlineCancelAttempted === true) return state.cancelSent === true;
  if (state.cancelSent === true || terminated || !socket || socket.readyState !== WebSocket.OPEN)
    throw Error('original_cancel_scope_unavailable');
  const tools = globalThis.__urClientToolLive?.tools;
  if (!Array.isArray(tools) || __urClientToolLive.overflow === true || tools.length > 16)
    throw Error('original_cancel_scope_unavailable');
  const starts = tools.slice(toolOffset).filter(value => value.state === 'started');
  if (starts.length > 1) throw Error('original_cancel_scope_unavailable');
  const button = document.getElementById('cancel');
  if (!state.fact || state.fact.status !== 'succeeded' || !state.command ||
      state.fact.themeTicket !== state.command.themeTicket || starts.length !== 1 ||
      starts[0].approved !== true || starts[0].response !== controlResponseId || controlResponseId === '' ||
      metrics.cancelledResponses !== expectedCancelled || !button || button.disabled)
    return false;
  state.inlineCancelAttempted = true; // 即使原 send 拒绝也不重复点击或猜新的回复目标。
  button.click();
  if (state.cancelSent !== true) throw Error('original_cancel_click_failed');
  return true;
}

// releaseOriginalLateFact 交付原真实事实及原命令DUP；主题端口必须重放旧结果而非再次产生副作用。
function releaseOriginalLateFact() {
  const state = globalThis.__urToolCancellation;
  if (!state?.fact || !state.command || !state.cancelSent || state.released) throw Error('invalid_late_release');
  state.released = true;
  if (!state.originalSend(state.fact)) throw Error('late_fact_send_failed');
  state.originalIncoming(state.command); // 原票据原字节语义重投，不生成新授权或新工具调用。
  return true;
}

// verifyCancellationProbe 已发生的UI效果不假称回滚；只证明旧事实/DUP不会续答、再执行或关闭新会话。
function verifyCancellationProbe(before, cancelled, after, voice) {
  assert.ok(cancelled.held && cancelled.commandHeld && cancelled.cancelSent,'actual_tool_and_cancel_required');
  assert.equal(cancelled.actualStatus,'succeeded');
  assert.equal(cancelled.records,before.actualRecords + 1);
  assert.equal(cancelled.cancelled,before.cancelled + 1);
  assert.equal(cancelled.completed,before.completed);
  assert.equal(cancelled.receipts,before.receiptIndex);
  for (const value of [cancelled,after]) {
    assert.equal(value.active,false);assert.equal(value.terminated,false);assert.equal(value.bridgeOpen,true);
    assert.equal(value.captureOpen,voice);assert.equal(value.pending,0);
  }
  for (const key of ['records','theme','cancelled','completed','receipts']) assert.equal(after[key],cancelled[key],'late_fact_changed_current_state');
  return {passed:true,actualEffectNotRolledBack:true,originalReplyCancelled:true,
    lateFactIsolated:true,duplicateTicketNotReexecuted:true,sessionStillOpen:true};
}

// runProbe 复用主验收器的真实UI与固定语料；收费模型必须自主产生原工具调用才会走到取消。
async function runProbe({invoke,snapshot,voice,wave,prepareSpeech,parseJSON}) {
  const before = await snapshot();
  await invoke('eval','(' + installCancellationHarness.toString() + ')()');
  if (voice) {
    await invoke('eval','globalThis.__urClientToolVoiceParts=[];true');
    for (let offset = 0; offset < wave.length; offset += 16384)
      await invoke('eval','__urClientToolVoiceParts.push(' + JSON.stringify(wave.subarray(offset,offset + 16384).toString('base64')) + ');true');
    await invoke('eval','(' + prepareSpeech.toString() + ')(__urClientToolVoiceParts,"深色")');
    await invoke('wait','--fn','__urClientToolSpeech.ended === true');
  } else {
    await invoke('fill','#textInput','请调用客户端工具set_local_display_theme，把当前界面改成深色dark。');
    await invoke('uncheck','#textAudio');await invoke('click','#sendText');
  }
  // 原事实和核心目标可能错开；在同一页面轮询中核权并点击，避免下一 CLI 往返耗尽原本地工具预算。
  await invoke('wait','--fn','(' + clickOriginalCancellation.toString() + ')(' +
    before.cancelled + ',' + before.tools.length + ')');
  await invoke('wait','--fn','__urToolCancellation.cancelSent && metrics.cancelledResponses === ' +
    (before.cancelled + 1) + ' && responseId === "" && pendingPlayback.size === 0');
  const facts = () => invoke('eval','(' + cancellationFacts.toString() + ')()').then(parseJSON);
  const cancelled = await facts();
  await invoke('eval','(' + releaseOriginalLateFact.toString() + ')()');
  // 等待只用于让原桥实际处理迟到消息；最终仍断言原状态，不以时间本身当作成功。
  await invoke('wait','500');
  return verifyCancellationProbe(before,cancelled,await facts(),voice);
}
module.exports = {installCancellationHarness,cancellationFacts,clickOriginalCancellation,releaseOriginalLateFact,verifyCancellationProbe,runProbe};
