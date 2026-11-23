// 文件说明：显式真实后端测试；C++持有MQTT设备身份，Chromium只提供真实WebRTC音频驱动。
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {spawn, execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');

// fixtureScript生成正式页面中的有界语料安装脚本；离线回归执行同一脚本，不模拟模型成功。
function fixtureScript(barge) {
  assert.equal(typeof barge, 'boolean');
  return `(async()=>{
      const bytes=Uint8Array.from(atob(window.__urFixtureChunks.join('')),c=>c.charCodeAt(0));
      delete window.__urFixtureChunks;
      const decoded=await context.decodeAudioData(bytes.buffer);
      if(decoded.numberOfChannels!==1||!Number.isFinite(decoded.duration)||decoded.duration<=0||decoded.duration>10)throw Error('invalid_fixture');
      const originalStream=stream; // 原已授权采集源只在替换成功后退休，不重新getUserMedia。
      const destination=context.createMediaStreamDestination();
      const track=destination.stream.getAudioTracks()[0];track.enabled=false;
      let silentBed=null; // 测试静音床保留原插话节奏，安装失败立即归还该临时节点。
      try {
        ${barge ? `silentBed=context.createConstantSource();silentBed.offset.value=0;
          silentBed.connect(destination);silentBed.start();` : ''}
        const sender=rtcPeer.getSenders().find(item=>item.track?.kind==='audio');
        if(!sender||!originalStream?.getAudioTracks().includes(sender.track))throw Error('missing_sender');
        await sender.replaceTrack(track);
        stream=destination.stream; // 正式rtcCreatePeer复用此变量；只换sender会使重连退回旧假麦克风。
        originalStream.getTracks().forEach(previous=>previous.stop());
        if(silentBed)window.__urSilentBed=silentBed;
      } catch(error) {
        track.stop();
        if(silentBed){silentBed.stop();silentBed.disconnect();}
        throw error;
      }
      window.__urFixtureEnded=0;
      window.__urPlayFixture=function(){
        const input=context.createBufferSource();input.buffer=decoded;input.connect(destination);
        input.onended=()=>{input.disconnect();++window.__urFixtureEnded;};
        input.start(context.currentTime+0.05);
      };
      return 'fixture_ready';
    })()`;
}

// run在显式开关下沿正式设备与界面执行有界真实语音验收，失败保留阶段并释放本轮资源。
async function run() {
  assert.equal(process.env.UR_BROWSER_LIVE, '1', '真实测试须显式开启');
  const binary = process.env.UR_DEVICE_BROWSER_BIN;
  const corpus = process.env.UR_BROWSER_INPUT_WAV;
  const port = Number(process.env.UR_BROWSER_PORT);
  const barge = process.env.UR_BROWSER_BARGE_IN === '1';
  const expectVolume = process.env.UR_BROWSER_EXPECT_VOLUME === undefined ? null :
    Number(process.env.UR_BROWSER_EXPECT_VOLUME);
  assert.ok(expectVolume === null || (Number.isInteger(expectVolume) &&
    expectVolume >= 0 && expectVolume <= 100), '音量工具断言必须为0至100');
  assert.ok(!barge || process.env.UR_BROWSER_EXPECT_MODE === 'fullDuplex', '插话测试须固定全双工预期');
  assert.ok(binary && fs.statSync(binary).isFile());
  assert.ok(corpus && path.isAbsolute(corpus) && fs.statSync(corpus).isFile());
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  const session = `device-webrtc-${randomUUID()}`;
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
    `--use-file-for-fake-audio-capture=${corpus}`].join(',')};
  const child = spawn(binary, [String(port)], {
    env: {...process.env, UR_MEDIA_TRANSPORT: 'webrtc'}, stdio: ['ignore', 'pipe', 'pipe']
  });
  let exited = false;
  let exitCode = null;
  const diagnostics = [];
  const completion = new Promise(resolve => child.once('close', code => {
    exited = true; exitCode = code; resolve();
  }));
  child.stderr.on('data', bytes => {
    for (const line of bytes.toString().split(/\r?\n/)) {
      const match = line.match(/^(?:request_rejected method=([A-Za-z]+) code=(\d{3})|runtime_failed state=(\d+) voice=(\d+))$/);
      if (match) diagnostics.push(match.slice(1));
    }
  });
  let phase = 'start';
  const invoke = (...args) => new Promise((resolve, reject) => {
    execFile('agent-browser', ['--session', session, ...args],
      {env, timeout: args[0] === 'wait' ? 90000 : 40000, maxBuffer: 65536}, (error, stdout) => {
        if (error) reject(new Error(`browser_command_failed:${phase}:${args[0]}`));
        else resolve(stdout.trim());
      });
  });
  const deadline = setTimeout(() => child.kill('SIGTERM'), 180000);
  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timer); reject(new Error('device_spawn_failed')); });
      child.stdout.on('data', bytes => {
        if (bytes.toString().includes(`http://127.0.0.1:${port}/?media=webrtc`)) {
          clearTimeout(timer); resolve();
        }
      });
    });
    phase = 'open';
    await invoke('open', `http://127.0.0.1:${port}/?media=webrtc`);
    await invoke('snapshot', '-i');
    await invoke('click', '#activate');
    phase = 'webrtc_established';
    await invoke('wait', '--fn', 'document.getElementById("start").disabled === false && rtcPeer?.connectionState === "connected"');
    const bytes = fs.readFileSync(corpus);
    assert.ok(bytes.length <= 180000, '语料必须有界');
    phase = 'install_bounded_voice';
    // CLI单次参数有长度限制；分段注入只影响测试语料，不改变设备媒体协议。
    const encoded = bytes.toString('base64');
    await invoke('eval', 'window.__urFixtureChunks=[]');
    for (let offset = 0; offset < encoded.length; offset += 24000)
      await invoke('eval', `window.__urFixtureChunks.push(${JSON.stringify(encoded.slice(offset, offset + 24000))})`);
    await invoke('eval', fixtureScript(barge));
    phase = 'actual_audio_up';
    await invoke('click', '#start');
    await invoke('wait', '--fn', 'rtcPeer.getSenders().some(item => item.track?.kind === "audio" && item.track.enabled)');
    await invoke('eval', 'window.__urPlayFixture()');
    await invoke('wait', '--fn', 'window.__urFixtureEnded === 1');
    let interrupted = 0;
    if (barge) {
      phase = 'barge_overlap';
      await invoke('wait', '--fn', 'responseId !== "" && metrics.lastPlayableConsumedResponseId === responseId');
      let oldResponse = JSON.parse(await invoke('eval', 'JSON.stringify(responseId)'));
      if (typeof oldResponse === 'string' && oldResponse.startsWith('"')) oldResponse = JSON.parse(oldResponse);
      assert.ok(typeof oldResponse === 'string' && oldResponse.length > 0);
      const prior = JSON.parse(await invoke('eval', 'JSON.stringify(metrics.interruptedPlaybacks)'));
      const before = JSON.parse(await invoke('eval', `(async()=>{const values=await rtcPeer.getStats();
        let sent=0;values.forEach(item=>{if(item.type==='outbound-rtp'&&item.kind==='audio')sent+=item.packetsSent||0;});
        return JSON.stringify(sent);})()`));
      const sentBefore = typeof before === 'string' ? JSON.parse(before) : before;
      await invoke('eval', 'window.__urPlayFixture()');
      await invoke('wait', '--fn', 'window.__urFixtureEnded === 2');
      const after = JSON.parse(await invoke('eval', `(async()=>{const values=await rtcPeer.getStats();
        let sent=0;values.forEach(item=>{if(item.type==='outbound-rtp'&&item.kind==='audio')sent+=item.packetsSent||0;});
        return JSON.stringify(sent);})()`));
      const sentAfter = typeof after === 'string' ? JSON.parse(after) : after;
      assert.ok(sentAfter > sentBefore + 20, '播放期间上行RTP未持续');
      await invoke('click', '#stop');
      await invoke('wait', '--fn', `metrics.interruptedPlaybacks > ${prior}`);
      interrupted = 1;
      phase = 'barge_recovery';
      await invoke('wait', '--fn', `completedResponseId !== "" && completedResponseId !== ${JSON.stringify(oldResponse)} &&
        metrics.lastPlayableConsumedResponseId === completedResponseId && responseId === ""`);
    } else await invoke('click', '#stop');
    phase = 'actual_audio_down';
    await invoke('wait', '--fn', 'metrics.completedResponses > 0 && responseId === "" && metrics.lastPlayableConsumedResponseId === completedResponseId && completedResponseId !== ""');
    if (expectVolume !== null) {
      phase = 'tool_applied';
      await invoke('wait', '--fn', `metrics.successfulActions > 0 && metrics.lastActualVolume === ${expectVolume}`);
    }
    const stats = JSON.parse(await invoke('eval', `(async()=>{
      const values=await rtcPeer.getStats();let sent=0,received=0;
      values.forEach(item=>{if(item.type==='outbound-rtp'&&item.kind==='audio')sent+=item.packetsSent||0;
        if(item.type==='inbound-rtp'&&item.kind==='audio')received+=item.packetsReceived||0;});
      return JSON.stringify({sent,received,mode:metrics.interactionMode,voiced:metrics.consumedNonSilent,
        completed:metrics.completedResponses,playable:metrics.playableResponses,
        actions:metrics.successfulActions,actualVolume:metrics.lastActualVolume});
    })()`));
    const result = typeof stats === 'string' ? JSON.parse(stats) : stats;
    assert.ok(result.sent > 0 && result.received > 0 && result.voiced >= 2 &&
      result.completed > 0 && result.playable > 0, '真实RTP、解码有声与回复完成缺一不可');
    if (process.env.UR_BROWSER_EXPECT_MODE)
      assert.equal(result.mode, process.env.UR_BROWSER_EXPECT_MODE);
    if (expectVolume !== null) {
      assert.ok(result.actions > 0);
      assert.equal(result.actualVolume, expectVolume);
    }
    phase = 'release';
    await invoke('click', '#end');
    await completion;
    assert.equal(exitCode, 0);
    console.log(JSON.stringify({result: 'pass', phase, sentPackets: result.sent,
      receivedPackets: result.received, voicedSoftwareWindows: result.voiced,
      completedResponses: result.completed, interruptedPlaybacks: interrupted,
      successfulActions: result.actions, actualVolume: result.actualVolume, mode: result.mode}));
  } catch (error) {
    let mediaState = null;
    try {
      const value = await invoke('eval', `(async()=>{const stats=await rtcPeer?.getStats();
        let sent=0,received=0,energy=0,duration=0,emitted=0,concealed=0,discarded=0,codec='none',jitter=0;
        stats?.forEach(item=>{if(item.type==='outbound-rtp'&&item.kind==='audio')sent+=item.packetsSent||0;
          if(item.type==='inbound-rtp'&&item.kind==='audio'){received+=item.packetsReceived||0;
            energy+=item.totalAudioEnergy||0;duration+=item.totalSamplesDuration||0;
            emitted+=item.jitterBufferEmittedCount||0;concealed+=item.concealedSamples||0;
            discarded+=item.packetsDiscarded||0;codec=stats.get(item.codecId)?.mimeType||'none';
            jitter=Math.max(jitter,item.jitter||0);}});
        return JSON.stringify({peer:rtcPeer?.connectionState||'none',audioContext:context?.state||'none',responseActive:responseId!=='',
          responseCompleted:completedResponseId!=='',completed:metrics.completedResponses,
          playable:metrics.playableResponses,voiced:metrics.consumedNonSilent,
          sampled:metrics.rtcSampleWindows,peakPower:metrics.rtcPeakPower,
          analyser:!!rtcAnalyser,remoteMuted:rtcPeer?.getReceivers()[0]?.track?.muted||false,
          direction:rtcPeer?.getTransceivers()[0]?.currentDirection||'none',codec,
          sent,received,energy,duration,emitted,concealed,discarded,jitter});})()`);
      const parsed = JSON.parse(value);
      mediaState = typeof parsed === 'string' ? JSON.parse(parsed) : parsed;
    } catch {}
    console.log(JSON.stringify({result: 'fail', phase, reason: error.message,
      diagnostics, mediaState, deviceExitCode: exitCode}));
    process.exitCode = 1;
  } finally {
    clearTimeout(deadline);
    try { await invoke('close'); } catch {}
    if (!exited) child.kill('SIGTERM');
    await completion;
  }
}

module.exports = {fixtureScript}; // 导入只暴露测试脚本，不启动设备、浏览器或收费API。
if (require.main === module) run().catch(error => {
  console.log(JSON.stringify({result: 'fail', phase: 'setup', reason: error.message}));
  process.exitCode = 1;
});
