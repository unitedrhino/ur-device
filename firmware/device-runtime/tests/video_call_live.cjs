// 文件说明：显式真实后端测试；C++设备与两个Chromium端点验收双向音频和解码视频帧。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {spawn, execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');

// browser调用固定agent-browser会话，所有命令均由本测试的浏览器进程执行。
function browser(session, env, ...args) {
  return new Promise((resolve, reject) => {
    execFile('agent-browser', ['--session', session, ...args],
      {env, timeout: args[0] === 'wait' ? 65000 : 40000, maxBuffer: 262144},
      (error, stdout) => error ? reject(Error(`browser_${args[0]}_failed`)) : resolve(stdout.trim()));
  });
}
// evaluated对CLI的JSON字符串结果解包，不把页面异常正文写进日志。
function evaluated(value) {
  let parsed = JSON.parse(value);
  while (typeof parsed === 'string' && /^[{[\"]/.test(parsed)) {
    try { parsed = JSON.parse(parsed); } catch { break; }
  }
  return parsed;
}
// api只返回本地隔离服务成功结果，不输出令牌、SDP或设备身份。
async function api(base, token, route, payload, project) {
  const headers = {'content-type': 'application/json', 'app-id': '100',
    'tenant-code': 'platform', token};
  if (project) headers['project-id'] = project;
  const response = await fetch(`${base}/api/v1/${route}`, {method: 'POST', headers,
    body: JSON.stringify(payload), signal: AbortSignal.timeout(20000)});
  const body = await response.json();
  if (response.status !== 200 || body.code !== 200) {
    const detail = String(body.msg || body.message || '').replaceAll(token, '<redacted>')
      .replaceAll(process.env.UR_DEVICE_SECRET || '<never>', '<redacted>').slice(0, 180);
    throw Error(`api_${route.replaceAll('/', '_')}_${body.code}_${detail}`);
  }
  return body.data;
}
// run从已隔离的本机环境注入设备身份；真实音视频路径不使用Mock后端。
async function run() {
  assert.equal(process.env.UR_VIDEO_LIVE, '1');
  const binary = process.env.UR_VIDEO_DEVICE_BIN;
  const base = process.env.UR_VIDEO_BASE_URL;
  const port = Number(process.env.UR_VIDEO_DEVICE_PORT);
  assert.ok(binary && fs.statSync(binary).isFile());
  assert.equal(new URL(base).hostname, '127.0.0.1');
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  const token = process.env.UR_VIDEO_APP_TOKEN;
  assert.ok(token && process.env.UR_DEVICE_PRODUCT && process.env.UR_DEVICE_NAME && process.env.UR_DEVICE_SECRET);
  const deviceSession = `video-device-${randomUUID()}`;
  const appSession = `video-app-${randomUUID()}`;
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
    '--autoplay-policy=no-user-gesture-required'].join(',')};
  const server = http.createServer((_, response) => {
    response.writeHead(200, {'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store'});
    response.end('<!doctype html><html><body><video id="remote" autoplay playsinline></video><audio id="audio" autoplay></audio></body></html>');
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const appURL = `http://127.0.0.1:${server.address().port}/`;
  const child = spawn(binary, [String(port)], {env: process.env, stdio: ['ignore', 'pipe', 'pipe']});
  let deviceExit = null;
  const diagnostics = [];
  child.stderr.on('data', bytes => {
    for (const line of bytes.toString().split(/\r?\n/))
      if (/^[a-z_]+(?: (?:[a-z_]+=[a-z_0-9]+ ?)+)?$/.test(line)) diagnostics.push(line);
  });
  const completion = new Promise(resolve => child.once('close', code => { deviceExit = code; resolve(); }));
  const deadline = setTimeout(() => child.kill('SIGTERM'), 150000);
  let stage = 'start';
  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timer); reject(Error('device_spawn_failed')); });
      child.stdout.on('data', bytes => {
        if (bytes.toString().includes(`http://127.0.0.1:${port}/`)) {
          clearTimeout(timer); resolve();
        }
      });
    });
    stage = 'device_available';
    await browser(deviceSession, env, 'open', `http://127.0.0.1:${port}/`);
    await browser(deviceSession, env, 'click', '#activate');
    await browser(deviceSession, env, 'wait', '--fn', 'state === "available"');
    const me = await api(base, token, 'system/user/self/get-one', {});
    const userID = Number(me.userID);
    assert.ok(Number.isSafeInteger(userID) && userID > 0);
    const deviceID = `${process.env.UR_DEVICE_PRODUCT}:${process.env.UR_DEVICE_NAME}`;
    const actualDevice = await api(base, token, 'things/device/info/get-one', {
      productID: process.env.UR_DEVICE_PRODUCT, deviceName: process.env.UR_DEVICE_NAME
    }, process.env.UR_VIDEO_PROJECT_ID);
    assert.equal(Number(actualDevice.userID), userID);
    await api(base, token, 'im/channel/bind', {
      channelType: 'device', channelId: deviceID, channelName: 'TASK-143 video device'
    }, process.env.UR_VIDEO_PROJECT_ID);
    const conversation = await api(base, token, 'im/conversation/create', {
      type: 'single', participants: [{type: 'user', id: String(userID)},
        {type: 'device', id: deviceID}]}, process.env.UR_VIDEO_PROJECT_ID);
    assert.ok(Number.isSafeInteger(conversation.id) && conversation.id > 0);
    stage = 'app_offer';
    await browser(appSession, env, 'open', appURL);
    const offer = evaluated(await browser(appSession, env, 'eval', `(async()=>{
      const peer=new RTCPeerConnection({iceServers:[]});window.__callPeer=peer;
      const canvas=document.createElement('canvas');canvas.width=320;canvas.height=240;
      const graphics=canvas.getContext('2d');let count=0;
      window.__drawTimer=setInterval(()=>{graphics.fillStyle=count++%2?'#1565c0':'#fbc02d';
        graphics.fillRect(0,0,320,240);graphics.fillStyle='#ffffff';graphics.fillRect(count%250,50,50,60);},60);
      const videoStream=canvas.captureStream(15);
      const video=videoStream.getVideoTracks()[0];
      const microphone=await navigator.mediaDevices.getUserMedia({audio:true});
      window.__microphone=microphone;
      peer.addTrack(microphone.getAudioTracks()[0],microphone);
      peer.addTrack(video,videoStream);
      peer.ontrack=event=>{const element=document.getElementById(event.track.kind==='video'?'remote':'audio');
        element.srcObject=new MediaStream([event.track]);void element.play();};
      const offer=await peer.createOffer();const gathered=new Promise(resolve=>{
        if(peer.iceGatheringState==='complete')resolve();
        else peer.addEventListener('icegatheringstatechange',()=>{if(peer.iceGatheringState==='complete')resolve();});});
      await peer.setLocalDescription(offer);await gathered;
      return JSON.stringify(peer.localDescription.sdp);
    })()`));
    assert.ok(typeof offer === 'string' && offer.startsWith('v=0\r\n'));
    stage = 'invite';
    const invited = await api(base, token, 'im/call/signal', {
      type: 'native_device_call_invite', callType: 'video', conversationId: conversation.id,
      calleeId: deviceID, payload: offer}, process.env.UR_VIDEO_PROJECT_ID);
    assert.ok(invited.callId && invited.event === 'created' && invited.state === 'inviting');
    await browser(appSession, env, 'eval', `(async()=>{await window.__callPeer.setRemoteDescription({type:'answer',sdp:${JSON.stringify(invited.payload)}});return true;})()`);
    await browser(deviceSession, env, 'wait', '--fn', 'state === "ringing"');
    if (process.env.UR_VIDEO_SCENARIO === 'reject') {
      stage = 'reject';
      await browser(deviceSession, env, 'click', '#reject');
      await browser(deviceSession, env, 'wait', '--fn', 'state === "available" && !peer && !capture');
      console.log(JSON.stringify({result:'pass',scenario:'reject',deviceMediaReleased:true}));
      return;
    }
    stage = 'busy';
    const busy = await api(base, token, 'im/call/signal', {
      type: 'native_device_call_invite', callType: 'video', conversationId: conversation.id,
      calleeId: deviceID, payload: offer}, process.env.UR_VIDEO_PROJECT_ID);
    assert.equal(busy.event, 'busy');
    stage = 'connected';
    await browser(deviceSession, env, 'click', '#accept');
    await browser(deviceSession, env, 'wait', '--fn', 'state === "connected" && peer?.connectionState === "connected"');
    await browser(appSession, env, 'wait', '--fn', 'window.__callPeer?.connectionState === "connected"');
    stage = 'device_decoded_media';
    await browser(deviceSession, env, 'wait', '--fn', 'reports.videoFrames >= 2 && reports.audioPackets > 3 && document.getElementById("remote").videoWidth > 0');
    stage = 'app_decoded_media';
    await browser(appSession, env, 'wait', '--fn', `(async()=>{
      const values=await window.__callPeer.getStats();let frames=0,audio=0;
      values.forEach(item=>{if(item.type==='inbound-rtp'&&item.kind==='video')frames+=item.framesDecoded||0;
        if(item.type==='inbound-rtp'&&item.kind==='audio')audio+=item.packetsReceived||0;});
      return frames>=2&&audio>3&&document.getElementById('remote').videoWidth>0;
    })()`);
    const deviceMetrics = evaluated(await browser(deviceSession, env, 'eval', 'JSON.stringify(reports)'));
    const appMetrics = evaluated(await browser(appSession, env, 'eval', `(async()=>{
      const values=await window.__callPeer.getStats();let frames=0,audio=0;
      values.forEach(item=>{if(item.type==='inbound-rtp'&&item.kind==='video')frames+=item.framesDecoded||0;
        if(item.type==='inbound-rtp'&&item.kind==='audio')audio+=item.packetsReceived||0;});
      const video=document.getElementById('remote');const canvas=document.createElement('canvas');
      canvas.width=32;canvas.height=24;const context=canvas.getContext('2d');context.drawImage(video,0,0,32,24);
      const pixels=context.getImageData(0,0,32,24).data;let sum=0;for(let i=0;i<pixels.length;i+=4)sum+=pixels[i]+pixels[i+1]+pixels[i+2];
      return JSON.stringify({frames,audio,pixelSum:sum});})()`));
    assert.ok(deviceMetrics.videoFrames >= 2 && deviceMetrics.audioPackets > 3);
    assert.ok(appMetrics.frames >= 2 && appMetrics.audio > 3 && appMetrics.pixelSum > 1000);
    stage = 'track_controls';
    await browser(deviceSession, env, 'click', '#mute');
    await browser(deviceSession, env, 'wait', '--fn', 'trackPending === false && capture.getAudioTracks()[0].enabled === false');
    await browser(deviceSession, env, 'click', '#mute');
    await browser(deviceSession, env, 'wait', '--fn', 'trackPending === false && capture.getAudioTracks()[0].enabled === true');
    await browser(deviceSession, env, 'click', '#camera');
    await browser(deviceSession, env, 'wait', '--fn', 'trackPending === false && capture.getVideoTracks()[0].enabled === false');
    await browser(deviceSession, env, 'click', '#camera');
    await browser(deviceSession, env, 'wait', '--fn', 'trackPending === false && capture.getVideoTracks()[0].enabled === true');
    stage = 'hangup';
    const ended = await api(base, token, 'im/call/signal', {type: 'device_call_end', callId: invited.callId}, process.env.UR_VIDEO_PROJECT_ID);
    assert.equal(ended.state, 'ended');
    await browser(deviceSession, env, 'wait', '--fn', 'state === "available" && peer === null && capture === null');
    console.log(JSON.stringify({result: 'pass', appAudioPackets: appMetrics.audio,
      appDecodedVideoFrames: appMetrics.frames, appPixelSum: appMetrics.pixelSum,
      deviceAudioPackets: deviceMetrics.audioPackets,
      deviceDecodedVideoFrames: deviceMetrics.videoFrames, busyVerified: true,
      trackControlsVerified: true, hangupReleased: true}));
  } catch (error) {
    let deviceState = null;
    try {
      deviceState = evaluated(await browser(deviceSession, env, 'eval',
        'JSON.stringify({state,status:document.getElementById("status").textContent,closed,reports,peerState:peer?.connectionState,remoteWidth:document.getElementById("remote").videoWidth})'));
    } catch {}
    console.log(JSON.stringify({result: 'fail', stage, reason: error.message,
      deviceExit, deviceState, diagnostics}));
    process.exitCode = 1;
  } finally {
    clearTimeout(deadline);
    for (const session of [appSession, deviceSession]) {
      try { await browser(session, env, 'close'); } catch {}
    }
    if (deviceExit === null) child.kill('SIGTERM');
    await completion;
    await new Promise(resolve => server.close(resolve));
  }
}
run().catch(error => { console.log(JSON.stringify({result: 'fail', stage: 'setup', reason: error.message})); process.exitCode = 1; });
