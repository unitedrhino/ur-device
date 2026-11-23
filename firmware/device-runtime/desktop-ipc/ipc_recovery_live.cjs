// 文件说明：S19正式IPC媒体短断网验收；只使用原浏览器Peer、正式命令和实际解码，不伪造连接事件。
'use strict';
const assert = require('node:assert/strict');
const {browser, evaluated} = require('./ipc_live.cjs');

// recoveredVideo 比较同一已协商媒体代次内的两次观察；新SSRC计数可从零开始，不能跨代次比大小。
// 仍要求新增至少两个实际解码帧、新包与非黑像素，握手或旧画面不能替代。
function recoveredVideo(before, after) {
  return Number.isSafeInteger(before?.frames) && Number.isSafeInteger(after?.frames) &&
    after.frames >= before.frames + 2 && after.packets > before.packets &&
    after.pixels > 0 && after.connected === true;
}

// verify 由拥有隔离后端进程的验收器提供短断网fault；函数不得接受PID、自由MQTT或替换驱动回调。
// fault必须在返回/失败前恢复本任务网络运行，实际disconnected由浏览器原ICE事实独立确认。
async function verify({deviceBrowser, env, viewers, metrics, command}, fault, sourceControl) {
  assert.equal(env.UR_IPC_RECOVERY_LIVE, '1', 'explicit_recovery_test_required');
  assert.equal(typeof fault, 'function', 'owned_network_fault_required');
  assert.equal(viewers.length, 3, 'original_three_viewers_required');
  const hold=env.UR_IPC_RECOVERY_SOURCE_STATE || 'active';
  assert.ok(['active','paused','stopped'].includes(hold),'invalid_recovery_source_state');
  if(hold!=='active')assert.equal(typeof sourceControl,'function','formal_source_control_required');
  const execute = (session, ...args) => browser(session, env, ...args);
  const heldViewer=hold==='active'?null:viewers[1];
  if(heldViewer) {
    assert.equal(heldViewer.listening,true,'original_listening_source_required');
    await sourceControl(heldViewer,hold==='paused'?'pause':'stop');
    await execute(deviceBrowser,'wait','--fn',`views.get(${JSON.stringify(heldViewer.sessionId)})?.sources.get('audio-1')?.state === ${JSON.stringify(hold)}`);
  }
  const before = await Promise.all(viewers.map(metrics));
  const identities = viewers.map(viewer => viewer.sessionId);
  // 保存原资源引用并被动记录ICE状态；不调用原事件处理器、不写连接状态、不产生媒体或信令。
  await execute(deviceBrowser, 'eval', `(()=>{window.__ipcRecoveryEvidence=new Map();
    for(const id of ${JSON.stringify(identities)}){const view=views.get(id);if(!view?.active)throw Error('original_active_required');
      const evidence={id,view,peer:view.peer,stream:view.stream,tracks:view.stream.getTracks(),states:[]};
      view.peer.addEventListener('connectionstatechange',()=>evidence.states.push(view.peer.connectionState));
      __ipcRecoveryEvidence.set(id,evidence);}return true;})()`);
  for (const viewer of viewers) await execute(viewer.browser, 'eval', `(()=>{
    window.__ipcOriginalPeer=window.__ipcPeer;window.__ipcRecoveryStates=[];
    __ipcPeer.addEventListener('connectionstatechange',()=>__ipcRecoveryStates.push(__ipcPeer.connectionState));return true;})()`);
  await fault();
  const resumed = await Promise.allSettled(viewers.map(async viewer => {
    // 另一端独立报告原媒体失联并以下一代SDP重协商；不使用设备端代次替代应用端代次。
    let disconnected;
    try { disconnected = await command(viewer, 'interaction.mediaState', {mediaRevision:1,state:'disconnected'}, viewer.sessionId); }
    catch { throw Error('recovery_application_disconnect_rejected'); }
    assert.equal(disconnected.data?.mediaRevision,1);
    const offer = evaluated(await execute(viewer.browser, 'eval', `(async()=>{
      if(!__ipcRecoveryStates.includes('disconnected'))throw Error('actual_disconnect_required');
      const peer=window.__ipcPeer;if(peer!==__ipcOriginalPeer)throw Error('original_peer_required');
      await peer.setLocalDescription(await peer.createOffer({iceRestart:true}));
      if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
        const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
        peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);})()`));
    let offered;
    try { offered = await command(viewer, 'interaction.mediaOffer', {mediaRevision:2,webrtc:{iceMode:'complete',sdp:offer}}, viewer.sessionId); }
    catch { throw Error('recovery_application_offer_rejected'); }
    assert.equal(offered.data?.mediaRevision,2);
    await execute(viewer.browser, 'eval', `(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(offered.data.webrtc.sdp)}});return true;})()`);
    await execute(viewer.browser, 'wait', '--fn', '__ipcPeer.connectionState === "connected"');
    try { await command(viewer, 'interaction.mediaState', {mediaRevision:2,state:'connected'}, viewer.sessionId); }
    catch { throw Error('recovery_application_connected_rejected'); }
  }));
  // 独立三腿并发恢复，但失败仍须等所有原操作收尾，不能留下迟到请求污染下一环境。
  const failed = resumed.find(result=>result.status==='rejected');
  if(failed) throw failed.reason;
  // 新Pion出口有独立SSRC，浏览器的包/采样计数可能清零；从新代次当前实际观察建立基线。
  // 已暂停／停止音源允许旧网络包排空；新代次不得自行重发或重新采集。
  if(heldViewer)await execute(heldViewer.browser,'wait','500');
  const baseline = await Promise.all(viewers.map(metrics));
  const after = [];
  for (let index=0;index<viewers.length;index++) {
    const viewer=viewers[index];
    await execute(deviceBrowser,'wait','--fn',`views.get(${JSON.stringify(viewer.sessionId)})?.active === true && views.get(${JSON.stringify(viewer.sessionId)}).mediaRevision === 2`);
    await execute(viewer.browser,'wait','--fn',`(async()=>{let frames=0;(await __ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video')frames+=s.framesDecoded||0;});return frames>=${baseline[index].frames+2};})()`);
    const value=await metrics(viewer);
    assert.ok(recoveredVideo(baseline[index],value),'actual_new_decoded_video_required');
    if(viewer.listening && viewer!==heldViewer) {
      assert.ok(value.audioSamples>baseline[index].audioSamples,'actual_new_decoded_audio_required');
      await execute(viewer.browser,'wait','--fn','window.__ipcAudio?.run >= 2');
    }
    if(viewer===heldViewer) {
      assert.equal(value.audioPackets,baseline[index].audioPackets,'held_audio_must_not_receive_new_rtp');
      assert.equal(value.audioSamples,baseline[index].audioSamples,'held_audio_must_not_decode_new_audio');
    }
    after.push({frames:value.frames,packets:value.packets,pixels:value.pixels,audioSamples:value.audioSamples});
    viewer.before=value; // 后继独立停流从本阶段已实际解码的同代次观察继续，不使用初始SSRC计数。
  }
  const retained=evaluated(await execute(deviceBrowser,'eval',`JSON.stringify([...__ipcRecoveryEvidence.values()].every(e=>{
    const view=e.view;return views.get(e.id)===view&&view.peer===e.peer&&view.stream===e.stream&&
      e.states.includes('disconnected')&&e.tracks.length===view.stream.getTracks().length&&
      e.tracks.every(t=>view.stream.getTracks().includes(t)&&(t.readyState==='live'||
        (${JSON.stringify(hold)}==='stopped'&&e.id===${JSON.stringify(heldViewer?.sessionId||'') }&&t.kind==='audio'&&t.readyState==='ended')));}))`));
  assert.equal(retained,true,'original_device_resources_required');
  if(heldViewer) {
    const source=evaluated(await execute(deviceBrowser,'eval',`(()=>{const s=views.get(${JSON.stringify(heldViewer.sessionId)}).sources.get('audio-1');
      return JSON.stringify({state:s.state,bound:s.sender.track!==null,ended:s.track.readyState==='ended',enabled:s.track.enabled});})()`));
    assert.equal(source.state,hold,'original_source_intent_required');
    assert.equal(source.bound,false,'held_source_must_not_rebind');
    assert.equal(source.ended,hold==='stopped','original_source_stop_fact_required');
    if(hold==='paused')assert.equal(source.enabled,false,'paused_source_must_not_enable');
  }
  return {verified:true,viewers:3,mediaRevision:2,originalResourcesRetained:true,
    sourceState:hold,heldSourceNotResumed:heldViewer!==null,
    beforeRecovery:before.map(value=>({frames:value.frames,packets:value.packets,audioSamples:value.audioSamples})),
    baseline:baseline.map(value=>({frames:value.frames,packets:value.packets,audioSamples:value.audioSamples})),decoded:after};
}
module.exports={recoveredVideo,verify};
