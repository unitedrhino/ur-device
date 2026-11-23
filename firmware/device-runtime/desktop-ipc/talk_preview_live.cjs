// 文件说明：会议对讲同设备三路视频预览验收；复用正式应用API、独立端点及原C++，不创建第二个设备进程。
'use strict';
const assert = require('node:assert/strict');
const {randomUUID} = require('node:crypto');
const {browser, evaluated, confirmAdmission, safetyTerminal} = require('./ipc_live.cjs');

// previewCount 仅开放明确的三路组合验收；不把拼错配置静默降级为单腿测试。
function previewCount(value) {
  assert.ok(value === undefined || value === '0' || value === '3', 'talk_preview_count_invalid');
  return value === '3' ? 3 : 0;
}

// decodedVideo 验证原浏览器真正解码非黑画面，且视频预览没有隐含订阅音频。
function decodedVideo(value) {
  return !!value && value.connected === true && value.active === true && value.audio === false &&
    Number.isSafeInteger(value.frames) && value.frames >= 2 && Number.isSafeInteger(value.packets) && value.packets > 0 &&
    Number.isFinite(value.pixels) && value.pixels > 0;
}

// createPreviewGroup 为一次组合验收保留原端点、会话与浏览器；未启动或失败也可精确关闭已经受理的会话。
function createPreviewGroup({api, env, device, pageURL, openedBrowsers, waitFor, cause}) {
  const viewers = []; // 每个观看者独立收件箱和SDP，不借用对讲端点或混淆请求关联。
  // receive 只读取当前观看者持久outbox，并在记录原会话事实后ACK；未知会话不能满足终态。
  async function receive(viewer, token) {
    const received = await api('im/interaction/events', {endpointId: viewer.endpointId, limit: 32});
    let result;
    for (const delivery of received.list || []) {
      const event = delivery.event;
      if (event?.method === 'interaction.state' && event.sessionId === viewer.sessionId) viewer.state = event.data?.state;
      if (cause && safetyTerminal(event, viewer.sessionId, cause)) viewer.safetyEnded = true;
      if (event?.method === 'interaction.result' && event.msgToken === token) result = event;
    }
    if (received.list?.length) await api('im/interaction/events/ack', {endpointId: viewer.endpointId,
      deliveryIds: received.list.map(row => row.deliveryId)});
    return result;
  }
  // command 用原token接收真实设备操作结果；202不是媒体协商成功。
  async function command(viewer, method, params) {
    const token = randomUUID(), command = {method, msgToken: token, params};
    if (method === 'interaction.open') command.target = {productID: env.UR_DEVICE_PRODUCT, deviceName: env.UR_DEVICE_NAME};
    else command.sessionId = viewer.sessionId;
    const accepted = await api('im/interaction/commands', {endpointId: viewer.endpointId, command});
    assert.equal(accepted.accepted, true); assert.equal(accepted.msgToken, token);
    let result;
    await waitFor(async () => {result = await receive(viewer, token); return !!result;});
    // 先保留受理后分配的原会话，失败也可在finally精确结束，不能遗失半建立资源。
    if (method === 'interaction.open' && result.sessionId) viewer.sessionId = result.sessionId;
    assert.equal(result.code, 200, 'combined_preview_operation_rejected');
    return result;
  }
  // metrics 从真实RTP解码和已渲染像素读取，不输出SDP、身份或原会话ID。
  async function metrics(viewer) {
    return evaluated(await browser(viewer.browser, env, 'eval', `(async()=>{let frames=0,packets=0;
      (await __ipcPeer.getStats()).forEach(s=>{if(s.type==='inbound-rtp'&&s.kind==='video'){frames+=s.framesDecoded||0;packets+=s.packetsReceived||0;}});
      const video=document.getElementById('remote'),canvas=document.createElement('canvas');canvas.width=16;canvas.height=16;
      const ctx=canvas.getContext('2d');ctx.drawImage(video,0,0,16,16);const data=ctx.getImageData(0,0,16,16).data;
      let pixels=0;for(let i=0;i<data.length;i+=4)pixels+=data[i]+data[i+1]+data[i+2];
      return JSON.stringify({frames,packets,pixels,connected:__ipcPeer.connectionState==='connected',
        active:video.videoWidth>0,audio:__ipcPeer.getReceivers().some(r=>r.track?.kind==='audio')});})()`));
  }
  return {
    // start 顺序建立三个真实视频会话后返回，设备仍为同一原连接，随后才建立会议对讲。
    start: async () => {
      for (let index = 0; index < 3; ++index) {
        const viewer = {endpointId: (await api('im/interaction/endpoints/register')).endpointId,
          browser: `ipc-talk-preview-${randomUUID()}`, safetyEnded: false};
        assert.ok(viewer.endpointId); viewers.push(viewer); openedBrowsers.add(viewer.browser);
        await browser(viewer.browser, env, 'open', pageURL);
        const offer = evaluated(await browser(viewer.browser, env, 'eval', `(async()=>{
          const peer=window.__ipcPeer=new RTCPeerConnection({iceServers:[]});
          peer.ontrack=e=>{document.getElementById('remote').srcObject=new MediaStream([e.track]);};
          const t=peer.addTransceiver('video',{direction:'recvonly'});
          const codecs=RTCRtpReceiver.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/h264'&&(c.sdpFmtpLine||'').includes('packetization-mode=1'));
          if(!codecs.length)throw Error('h264_required');t.setCodecPreferences(codecs);
          await peer.setLocalDescription(await peer.createOffer());
          if(peer.iceGatheringState!=='complete')await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('ice_timeout')),8000);
            const check=()=>{if(peer.iceGatheringState==='complete'){clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);resolve();}};
            peer.addEventListener('icegatheringstatechange',check);check();});return JSON.stringify(peer.localDescription.sdp);
        })()`));
        assert.ok(typeof offer === 'string' && offer.startsWith('v=0\r\n'));
        const opened = await command(viewer, 'interaction.open', {purpose: 'ipcLiveView', cameraId: 'front'});
        assert.ok(viewer.sessionId && opened.data?.sessionId === viewer.sessionId);
        await confirmAdmission('accept', viewer.sessionId, (...args) => browser(device, env, ...args));
        await waitFor(async () => evaluated(await browser(device, env, 'eval',
          `!!views.get(${JSON.stringify(viewer.sessionId)})?.peer?.localDescription`)) === true);
        const answer = await command(viewer, 'interaction.mediaOffer', {mediaRevision: 1, webrtc: {iceMode: 'complete', sdp: offer}});
        assert.equal(answer.data?.mediaRevision, 1); assert.equal(answer.data?.webrtc?.iceMode, 'complete');
        assert.ok(typeof answer.data.webrtc.sdp === 'string' && answer.data.webrtc.sdp.startsWith('v=0\r\n'));
        await browser(viewer.browser, env, 'eval', `(async()=>{await __ipcPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(answer.data.webrtc.sdp)}});return true;})()`);
        await waitFor(async () => evaluated(await browser(viewer.browser, env, 'eval', '__ipcPeer.connectionState === "connected"')) === true);
        await command(viewer, 'interaction.mediaState', {mediaRevision: 1, state: 'connected'});
        await waitFor(async () => {await receive(viewer); return viewer.state === 'established';});
        await waitFor(async () => {viewer.before = await metrics(viewer); return decodedVideo(viewer.before);});
      }
    },
    // verifyConcurrent 要求对讲已解码期间三路均继续产生新帧，不以先前的静止计数充当并发媒体。
    verifyConcurrent: async () => {
      const facts = [];
      for (const viewer of viewers) {
        let current;
        await waitFor(async () => {current = await metrics(viewer); return decodedVideo(current) && current.frames > viewer.before.frames;});
        facts.push(current);
      }
      assert.equal(facts.length, 3); return facts;
    },
    // waitSafety 三个应用各自接收原安全终态，不能仅用设备总表清空证明服务器扇出完成。
    waitSafety: async () => {
      for (const viewer of viewers) await waitFor(async () => {
        await receive(viewer); return viewer.safetyEnded && viewer.state === 'ended';
      });
    },
    // end 正常路径主动结束自己的会话；安全路径只允许在终态证据检查后用于精确清理。
    end: async () => {for (const viewer of viewers) if (viewer.sessionId && viewer.state !== 'ended')
      await command(viewer, 'interaction.end', {reason: 'userStopped'});},
    // release 应用收到正式终态后停止自身视频出口，真实轨和Peer分别检查。
    release: async () => {
      const facts = [];
      for (const viewer of viewers) facts.push(evaluated(await browser(viewer.browser, env, 'eval', `(()=>{
        const video=document.getElementById('remote'),tracks=video.srcObject?.getTracks()||[];
        if(tracks.length!==1)throw Error('original_video_track_required');tracks.forEach(t=>t.stop());__ipcPeer.close();
        video.pause();video.srcObject=null;return JSON.stringify({tracksStopped:tracks.every(t=>t.readyState==='ended'),
          peerClosed:__ipcPeer.connectionState==='closed',sinkReleased:video.srcObject===null&&video.paused});})()`)));
      for (const fact of facts) assert.deepEqual(fact, {tracksStopped: true, peerClosed: true, sinkReleased: true});
      return facts;
    }
  };
}
module.exports = {previewCount, decodedVideo, createPreviewGroup};
