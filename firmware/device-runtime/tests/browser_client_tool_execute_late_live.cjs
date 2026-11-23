// 文件说明：真实MQTT原Execute迟到验收；不替换设备incoming、不制造命令、ACK、结果或工具副作用。
'use strict';
const assert = require('node:assert/strict');

// verifyLateExecute 要求原取消终态、原报文实收且被水位拒绝，以及无执行/续答污染和持续会话。
function verifyLateExecute(before,cancelled,after,voice,sequence,network) {
  assert.ok(network.held && network.releasedOriginal && !network.failed,'original_mqtt_frame_required');
  for(const facts of [cancelled,after]) {
    assert.equal(facts.cancelled,before.cancelled + 1);
    assert.equal(facts.completed,before.completed);
    assert.equal(facts.actualRecords,before.actualRecords);
    assert.equal(facts.receiptIndex,before.receiptIndex);
    assert.equal(facts.active,false);assert.equal(facts.terminated,false);assert.equal(facts.localSocketOpen,true);
    assert.equal(facts.captureOpen,voice);assert.equal(facts.captureClosed,!voice);assert.equal(facts.pendingPlayback,0);
  }
  assert.equal(after.theme,cancelled.theme);
  assert.equal(after.messageDiagnostics?.overflow,false);
  assert.ok(after.messageDiagnostics.records.some(record => record.method === 'toolCallExecute' &&
    record.sequence === sequence && record.watermark > sequence && record.outcome === 'watermark_discarded' &&
    record.parsed === true && record.sessionMatches === true && record.taken === false),'actual_old_execute_must_be_received_and_rejected');
  return {passed:true,originalMqttExecuteDelayed:true,originalReplyCancelled:true,
    actualExecuteRejected:true,noLocalEffect:true,noLateSubmitOrContinuation:true,sessionStillOpen:true};
}

// clickHeldExecuteCancellation 在原wait轮询内核本轮严格工具投影和代理持有的回复，再点击原取消按钮。
// 仅观察原send的真实返回；不构造报文、不复活已Done的目标、不跨CLI保存过期取消权限。
function clickHeldExecuteCancellation(expectedResponse,expectedCancelled,expectedCompleted,expectedRecords,toolOffset) {
  if(typeof expectedResponse !== 'string' || !expectedResponse || expectedResponse.length > 256 ||
      /[\r\n\x00]/.test(expectedResponse) ||
      ![expectedCancelled,expectedCompleted,expectedRecords,toolOffset].every(value=>Number.isSafeInteger(value)&&value>=0) ||
      toolOffset > 16)
    throw Error('invalid_execute_cancel_scope');
  const previous=globalThis.__urExecuteLateCancellation;
  if(previous) {
    if(previous.response !== expectedResponse || previous.cancelled !== expectedCancelled ||
        previous.completed !== expectedCompleted || previous.records !== expectedRecords || previous.offset !== toolOffset)
      throw Error('original_execute_cancel_scope_unavailable');
    return previous.sent === true;
  }
  if(terminated || !socket || socket.readyState !== WebSocket.OPEN)
    throw Error('original_execute_cancel_scope_unavailable');
  const observer=globalThis.__urClientToolLive;
  if(!observer || !Array.isArray(observer.tools) || observer.overflow === true || observer.tools.length > 16)
    throw Error('original_execute_cancel_scope_unavailable');
  const tools=observer.tools.slice(toolOffset),starts=tools.filter(value=>value.state==='started');
  if(starts.length > 1 || tools.some(value=>value.state!=='started'))
    throw Error('original_execute_cancel_scope_unavailable');
  const button=document.getElementById('cancel');
  if(starts.length !== 1 || starts[0].approved !== true || starts[0].response !== expectedResponse ||
      controlResponseId !== expectedResponse ||
      metrics.cancelledResponses !== expectedCancelled || metrics.completedResponses !== expectedCompleted ||
      !clientTheme || clientTheme.closed || clientTheme.records.size !== expectedRecords || !button || button.disabled)
    return false;
  const state={response:expectedResponse,cancelled:expectedCancelled,completed:expectedCompleted,
    records:expectedRecords,offset:toolOffset,sent:false};
  globalThis.__urExecuteLateCancellation=state; // 原按钮失败后也不能重复点击，或改选其他回复。
  const originalSend=send;
  let attempts=0,unexpected=false;
  send=function(message) {
    const result=originalSend.apply(this,arguments);
    ++attempts;
    if(message?.kind !== 'cancel' || message.id !== expectedResponse)unexpected=true;
    else if(result === true)state.sent=true;
    return result;
  };
  try {button.click();} catch(error) {state.sent=false;throw error;} finally {send=originalSend;}
  if(attempts !== 1 || unexpected || !state.sent) {
    state.sent=false;
    throw Error('original_execute_cancel_click_failed');
  }
  return true;
}

// runExecuteLateProbe 仅经原UI提交固定语料；透明代理持有正式平台报文后，取消再原字节放行。
async function runExecuteLateProbe({invoke,snapshot,voice,wave,prepareSpeech,parseJSON,delay}) {
  const before = await snapshot();
  if(voice) {
    await invoke('eval','globalThis.__urClientToolVoiceParts=[];true');
    for(let offset = 0;offset < wave.length;offset += 16384)
      await invoke('eval','__urClientToolVoiceParts.push(' + JSON.stringify(wave.subarray(offset,offset + 16384).toString('base64')) + ');true');
    await invoke('eval','(' + prepareSpeech.toString() + ')(__urClientToolVoiceParts,"深色")');
    await invoke('wait','--fn','__urClientToolSpeech.ended === true');
  } else {
    await invoke('fill','#textInput','请调用客户端工具set_local_display_theme，把当前界面改成深色dark。');
    await invoke('uncheck','#textAudio');await invoke('click','#sendText');
  }
  const deadline = Date.now() + 45000;
  while(!delay.identity()) {
    assert.ok(!delay.facts().failed && Date.now() < deadline,'original_execute_not_held');
    await new Promise(resolve => setTimeout(resolve,10));
  }
  const original = delay.identity();
  await invoke('wait','--fn','(' + clickHeldExecuteCancellation.toString() + ')(' +
    [original.response,before.cancelled,before.completed,before.actualRecords,before.tools.length].map(JSON.stringify).join(',') + ')');
  await invoke('wait','--fn','metrics.cancelledResponses === ' + (before.cancelled + 1) +
    ' && responseId === "" && pendingPlayback.size === 0');
  const cancelled = await snapshot();
  delay.release(original.session,original.response);
  await invoke('wait','--fn','__urClientToolLive.messageDiagnostics?.records.some(record => ' +
    'record.method === "toolCallExecute" && record.sequence === ' + original.sequence +
    ' && record.outcome === "watermark_discarded") === true');
  return verifyLateExecute(before,cancelled,await snapshot(),voice,original.sequence,delay.facts());
}
module.exports = {verifyLateExecute,clickHeldExecuteCancellation,runExecuteLateProbe};
