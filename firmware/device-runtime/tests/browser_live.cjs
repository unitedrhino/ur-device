// 显式真实浏览器端到端：C++设备进程接真实后端；虚拟麦克风语料不是硬件/AEC验收。
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {spawn, execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {endSessionViaUI} = require('./mcp_file_media_live.cjs'); // 文字输入滚屏后，仍仅通过原可见按钮完成结束。

// run要求调用方先通过仓库重测试门禁，并只经环境注入设备身份和明确目标。
async function run() {
  assert.equal(process.env.UR_BROWSER_LIVE, '1', '真实后端测试必须显式开启');
  const binary = process.env.UR_DEVICE_BROWSER_BIN;
  const corpus = process.env.UR_BROWSER_INPUT_WAV;
  const port = Number(process.env.UR_BROWSER_PORT);
  const textOnly = process.env.UR_BROWSER_TEXT_ONLY === '1'; // 独立验证新会话不启动音频输入时的离散文字收发。
  const count = Number(process.env.UR_BROWSER_ROUNDS || (textOnly ? '0' : '2')); // 同会话有限多轮，避免意外无限计费。
  const timedCorpus = process.env.UR_BROWSER_TIMED_CORPUS === '1'; // 单次语料经真实WebAudio/Worklet上行，不使用循环麦克风作效果判定。
  const continuousBarge = timedCorpus && process.env.UR_BROWSER_BARGE_IN === '1'; // 插话用例保持输入常开，不能靠反复暂停/恢复伪装全双工。
  const keywords = (process.env.UR_BROWSER_EXPECT_ASR_KEYWORDS || '').split(',').filter(Boolean); // 固定语料识别检查，只报告布尔值。
  assert.ok(keywords.length <= 10 && keywords.every(word => word.length <= 64 && !/[\n\r]/.test(word)));
  assert.ok(binary && fs.statSync(binary).isFile());
  assert.ok(corpus && path.isAbsolute(corpus) && !/[\n,]/.test(corpus) && fs.statSync(corpus).isFile());
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  assert.ok(Number.isInteger(count) && count >= (textOnly ? 0 : 1) && count <= 10);
  assert.ok(!textOnly || process.env.UR_BROWSER_TEXT_INPUT === '1', '文字独立测试必须同时开启文字输入');
  const session = `device-live-${randomUUID()}`;
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
    `--use-file-for-fake-audio-capture=${corpus}`].join(',')};
  const ownedCancellation = require('./browser_live_owned_cancel.cjs').create(process);
  let child;
  try { child = spawn(binary, [String(port)], {env: process.env, stdio: ['ignore', 'pipe', 'pipe']}); }
  catch (error) { ownedCancellation.finish(); throw error; }
  ownedCancellation.attach(child);
  let exited = false;
  let exitCode;
  const deviceDiagnostics = []; // 仅白名单固定字段，永不保存子进程原始输出。
  const completion = new Promise(resolve => child.once('close', code => {
    exited = true; exitCode = code; resolve(code);
  }));
  // 只保留协议方法/状态码或枚举状态，拒绝任何正文、地址和凭据进入报告。
  child.stderr.on('data', bytes => {
    for (const line of bytes.toString().split(/\r?\n/)) {
      const match = line.match(/^(?:request_rejected method=([A-Za-z]+) code=(\d{3})|runtime_failed state=(\d+) voice=(\d+))$/);
      if (match) deviceDiagnostics.push(match.slice(1));
    }
  });
  let phase = 'start'; // 只记录固定阶段，失败报告不包含URL中的配对令牌。
  const rounds = []; // 失败也保留此前逐轮计数，不能只留下最后一个聚合值。
  const invoke = (...args) => new Promise((resolve, reject) => {
    execFile('agent-browser', ['--session', session, ...args],
      {env, timeout: args[0] === 'wait' ? 90000 : 40000, maxBuffer: 65536}, (error, stdout) => {
        if (error) reject(new Error(`browser_command_failed:${phase}:${args[0]}`));
        else resolve(stdout.trim());
      });
  });
  // 十轮真实模型用例的每轮都须等待实际播完；按有限轮数给总预算，仍保持硬性上限。
  const deadline = setTimeout(() => child.kill('SIGTERM'), Math.min(750000, Math.max(240000, count * 75000)));
  try {
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timeout); reject(new Error('device_spawn_failed')); });
      child.stdout.on('data', bytes => {
        if (bytes.toString().includes(`http://127.0.0.1:${port}/`)) { clearTimeout(timeout); resolve(); }
      });
    });
    await invoke('open', `http://127.0.0.1:${port}/`);
    await invoke('snapshot', '-i');
    await invoke('click', '#activate');
    phase = 'ready';
    await invoke('wait', '--fn', 'document.getElementById("start").disabled === false');
    if (timedCorpus) {
      // 只用于虚拟媒体验收：断开循环假麦克风的音频连接，保留授权/释放检查；不声称经过麦克风AEC。
      const bytes = fs.readFileSync(corpus);
      assert.ok(bytes.length <= 90000, '定时语料必须保持有界，不能塞入大文件浏览器命令');
      phase = 'install_timed_corpus';
      await invoke('eval', `(async()=>{
        const bytes=Uint8Array.from(atob(${JSON.stringify(bytes.toString('base64'))}),c=>c.charCodeAt(0));
        const decoded=await context.decodeAudioData(bytes.buffer);
        if(decoded.numberOfChannels!==1||decoded.duration<=0||decoded.duration>10)throw Error('invalid_fixture');
        const buffer=context.createBuffer(1,decoded.length+Math.ceil(context.sampleRate*0.2),context.sampleRate);
        buffer.copyToChannel(decoded.getChannelData(0),0);
        source.disconnect();
        window.__urFixtureEnded=0;
        ${continuousBarge ? `const idle=context.createConstantSource();idle.offset.value=0;idle.connect(worklet);idle.start();` : ''}
        window.__urPlayFixture=function(){
          const input=context.createBufferSource();input.buffer=buffer;input.connect(worklet);
          input.onended=()=>{input.disconnect();++window.__urFixtureEnded;};
          input.start(context.currentTime+0.05);
          return 'fixture_started';
        };
        const original=incoming;
        incoming=function(value){original(value);if(value.kind==='captureStart')window.__urPlayFixture();};
        return 'fixture_ready';
      })()`);
    }
    let fixtureCount = 0; // 每次真实采集启动只播放一次，避免半句重复引起假插话。
    const finishCapture = async (baseline, stopCapture = true) => {
      if (timedCorpus) {
        ++fixtureCount;
        await invoke('wait', '--fn', `window.__urFixtureEnded >= ${fixtureCount} && metrics.captured > ${baseline.captured}`);
      } else {
        await invoke('wait', '--fn', `metrics.captured >= ${baseline.captured + 150} || metrics.completedResponses > ${baseline.completedResponses}`);
      }
      if (stopCapture) await invoke('click', '#stop');
    };
    for (let round = 1; round <= count; ++round) {
      phase = `round_${round}_capture`;
      const before = JSON.parse(await invoke('eval', 'JSON.stringify(metrics)'));
      // agent-browser eval对字符串再做JSON包装，统一解一次可选字符串层。
      const baseline = typeof before === 'string' ? JSON.parse(before) : before;
      const transcriptOffset = Number(await invoke('eval', 'document.getElementById("transcript").textContent.length'));
      assert.ok(Number.isSafeInteger(transcriptOffset) && transcriptOffset >= 0);
      await invoke('snapshot', '-i');
      await invoke('click', '#start');
      await finishCapture(baseline, !continuousBarge);
      let interruptedResponse = ''; // 插话前真实播放的身份，旧回复完成不能替代续答。
      if (process.env.UR_BROWSER_BARGE_IN === '1') {
        phase = 'barge_in_overlap';
        await invoke('wait', '--fn', 'responseId !== "" && currentReplyConsumed > 0');
        // 循环语料可能产生多条回复，先等当前播放与完成身份相同，再固定中断目标。
        if (process.env.UR_BROWSER_LATE_BARGE_IN === '1') {
          phase = 'late_barge_completed_but_playing';
          await invoke('wait', '--fn', 'completedResponseId !== "" && responseId === completedResponseId && currentReplyConsumed > 0');
        }
        const raw = JSON.parse(await invoke('eval', 'JSON.stringify(metrics)'));
        const prior = typeof raw === 'string' ? JSON.parse(raw) : raw;
        interruptedResponse = JSON.parse(await invoke('eval', 'JSON.stringify(responseId)'));
        if (typeof interruptedResponse === 'string' && interruptedResponse.startsWith('"'))
          interruptedResponse = JSON.parse(interruptedResponse);
        assert.ok(typeof interruptedResponse === 'string' && interruptedResponse);
        phase = 'barge_capture_overlap';
        if (continuousBarge) await invoke('eval', 'window.__urPlayFixture()');
        else await invoke('click', '#start');
        await invoke('wait', '--fn', `metrics.uplinkWhilePlaying > ${prior.uplinkWhilePlaying}`);
        phase = 'barge_capture_utterance';
        await finishCapture(prior, !continuousBarge);
      }
      phase = `round_${round}_playback`;
      await invoke('wait', '--fn', `metrics.completedResponses > ${baseline.completedResponses} && responseId === "" && ` +
        `metrics.lastMediaFinalResponseId === completedResponseId && ` +
        `metrics.lastObservedComplete === true && metrics.lastMissingPackets === 0 && ` +
        `metrics.lastPlayableResponseId === completedResponseId && ` +
        `metrics.lastPlayableConsumedResponseId === completedResponseId && ` +
        `metrics.lastVoicedConsumedResponseId === completedResponseId && ` +
        `metrics.lastVoicedConsumedResponseFrames >= 2`);
      if (interruptedResponse)
        await invoke('wait', '--fn', `completedResponseId !== ${JSON.stringify(interruptedResponse)} && completedResponseId !== "" && consumedResponseId === completedResponseId && responseId === ""`);
      const raw = JSON.parse(await invoke('eval', 'JSON.stringify(metrics)'));
      const state = typeof raw === 'string' ? JSON.parse(raw) : raw;
      if (process.env.UR_BROWSER_EXPECT_MODE)
        assert.equal(state.interactionMode, process.env.UR_BROWSER_EXPECT_MODE);
      assert.ok(state.captured > baseline.captured &&
        state.decodedNonSilent > baseline.decodedNonSilent &&
        state.playableResponses > baseline.playableResponses &&
        state.lastPlayableResponseId === state.lastMediaFinalResponseId &&
        state.lastPlayableConsumedResponseId === state.lastMediaFinalResponseId &&
        state.lastVoicedConsumedResponseId === state.lastMediaFinalResponseId &&
        state.lastVoicedConsumedResponseFrames >= 2 &&
        state.lastObservedComplete === true && state.lastMissingPackets === 0,
      `本轮未完成无可见丢包的有声回复：缺包=${state.lastMissingPackets}，可观察完整=${state.lastObservedComplete}`);
      if (process.env.UR_BROWSER_BARGE_IN === '1')
        assert.ok(state.interruptedPlaybacks > baseline.interruptedPlaybacks, '本轮旧播放未实际中断');
      else if (timedCorpus)
        assert.equal(state.cancelledResponses, baseline.cancelledResponses, '普通单次语料不应产生取消回复');
      if (keywords.length) {
        const matched = await invoke('eval', `(()=>{
          const text=document.getElementById('transcript').textContent.slice(${transcriptOffset});
          const recognized=text.split(String.fromCharCode(10)).filter(line=>line.startsWith('用户：')).map(line=>line.slice(3));
          return recognized.length>0&&recognized.every(line=>${JSON.stringify(keywords)}.every(word=>line.includes(word)));
        })()`);
        assert.equal(matched, 'true', '本轮实际识别未覆盖固定语料关键词');
        state.recognitionMatched = true;
      }
      rounds.push(state);
      console.log(JSON.stringify({event: 'roundVerified', round, state}));
      if (continuousBarge) await invoke('click', '#stop');
    }
    if (process.env.UR_BROWSER_EXPECT_VOLUME !== undefined) {
      assert.match(process.env.UR_BROWSER_EXPECT_VOLUME, /^(100|[1-9]?[0-9])$/);
      assert.ok(rounds.at(-1).successfulActions > 0, '未收到实际软件驱动成功事实');
      assert.equal(rounds.at(-1).lastActualVolume, Number(process.env.UR_BROWSER_EXPECT_VOLUME));
    }
    if (process.env.UR_BROWSER_BARGE_IN === '1') {
      assert.equal(rounds.at(-1).interactionMode, 'fullDuplex');
      assert.ok(rounds.at(-1).uplinkWhilePlaying > 0, '未观察到实际播放期间上行');
      assert.ok(rounds.at(-1).interruptedPlaybacks > 0, '设备未实际应用定向播放中断');
    }
    if (process.env.UR_BROWSER_TEXT_INPUT === '1') {
      phase = 'text_input';
      const before = JSON.parse(await invoke('eval', 'JSON.stringify(metrics)'));
      const baseline = typeof before === 'string' ? JSON.parse(before) : before;
      const textOffset = Number(await invoke('eval', 'document.getElementById("transcript").textContent.length'));
      await invoke('fill', '#textInput', '请用一句话介绍你自己。');
      await invoke('click', '#sendText');
      await invoke('wait', '--fn', 'metrics.textInputResult !== null');
      const ack = JSON.parse(await invoke('eval', 'JSON.stringify(metrics.textInputResult)'));
      const ackResult = typeof ack === 'string' && ack.startsWith('"') ? JSON.parse(ack) : ack;
      assert.equal(ackResult, 'accepted', `云端文字输入未受理:${ackResult}`);
      await invoke('wait', '--fn', `metrics.completedResponses > ${baseline.completedResponses} && responseId === ""`);
      if (process.env.UR_BROWSER_TEXT_EXPECT_AUDIO === '1') {
        // MQTT回复终态与UDP/Worklet没有跨通道严格同序；另等本回复达到首可播放门槛并被音频线程消费。
        await invoke('wait', '--fn', `metrics.playableResponses > ${baseline.playableResponses} && ` +
          `metrics.lastPlayableResponseId === completedResponseId && ` +
          `metrics.lastPlayableConsumedResponseId === completedResponseId && ` +
          `metrics.lastVoicedConsumedResponseId === completedResponseId && ` +
          `metrics.lastVoicedConsumedResponseFrames >= 2 && ` +
          `metrics.lastObservedComplete === true && metrics.lastMissingPackets === 0 && ` +
          `metrics.lastMediaFinalResponseId === completedResponseId && completedResponseId !== ""`)
          .catch(() => {});
      }
      const after = JSON.parse(await invoke('eval', 'JSON.stringify(metrics)'));
      const state = typeof after === 'string' ? JSON.parse(after) : after;
      assert.ok(state.completedResponses > baseline.completedResponses, '文字输入未收到回复终态');
      if (textOnly)
        assert.equal(state.captured, baseline.captured, '独立文字用例不应向云端发送音频输入');
      const audioState = state.lastAudioState;
      assert.ok(['none', 'sent', 'cancelled', 'failed'].includes(audioState), '回复终态缺少音频发送状态');
      const outputChars = Number(await invoke('eval', `document.getElementById("transcript").textContent.length - ${textOffset}`));
      assert.ok(Number.isSafeInteger(outputChars) && outputChars > 0, '文字输入未产生可见模型文字回复');
      if (process.env.UR_BROWSER_TEXT_EXPECT_AUDIO === '1') {
        const consumedCurrent = await invoke('eval', 'consumedResponseId !== "" && consumedResponseId === completedResponseId');
        const finalCurrent = await invoke('eval', 'completedResponseId !== "" && metrics.lastMediaFinalResponseId === completedResponseId');
        assert.ok(state.playableResponses > baseline.playableResponses &&
          state.lastPlayableResponseId === state.lastMediaFinalResponseId &&
          state.lastPlayableConsumedResponseId === state.lastMediaFinalResponseId &&
          state.lastVoicedConsumedResponseId === state.lastMediaFinalResponseId &&
          state.lastVoicedConsumedResponseFrames >= 2 &&
          state.lastObservedComplete === true && state.lastMissingPackets === 0 &&
          consumedCurrent === 'true' &&
          finalCurrent === 'true',
          `文字输入未完成无可见丢包的有声回复，云端音频发送状态=${audioState}，媒体计数口径=${finalCurrent === 'true' ? 'final' : 'provisional'}，设备有效包=${state.lastNetworkPackets}，设备解码帧=${state.lastDecodedFrames}，设备非静音帧=${state.lastNonSilentFrames}，缺包=${state.lastMissingPackets}，可观察完整=${state.lastObservedComplete}，本回复有声消费帧=${state.lastVoicedConsumedResponseFrames}`);
      }
      rounds.push({...state, textInputVerified: true, textOutputChars: outputChars, audioState,
        mediaFinalReported: await invoke('eval', 'completedResponseId !== "" && metrics.lastMediaFinalResponseId === completedResponseId') === 'true',
        networkPackets: state.lastNetworkPackets, decodedFrames: state.lastDecodedFrames,
        nonSilentFrames: state.lastNonSilentFrames});
    }
    await invoke('snapshot', '-i');
    await endSessionViaUI(invoke);
    phase = 'release';
    await invoke('wait', '--fn', 'context.state === "closed" && stream.active === false');
    const code = await Promise.race([completion, new Promise((_, reject) => {
      const timer = setTimeout(() => reject(new Error('device_exit_timeout')), 10000);
      timer.unref();
    })]);
    assert.equal(code, 0);
    assert.equal(ownedCancellation.cancelled, false, 'owned_run_cancelled');
    assert.equal(ownedCancellation.operationFailed, false, 'owned_child_signal_failed');
    console.log(JSON.stringify({passed: true, rounds, released: true,
      boundary: timedCorpus ? 'timed_webaudio_source_not_microphone_AEC_or_hardware' : 'virtual_browser_pcm_not_hardware_or_latency_baseline'}));
  } catch (error) {
    // 失败样本只采集计数与媒体生命周期；不采集模型正文、配对URL或设备凭据。
    const observed = await invoke('eval', 'JSON.stringify({metrics,contextState:context?.state,streamActive:stream?.active,status:document.getElementById("status")?.textContent})').catch(() => null);
    console.log(JSON.stringify({passed: false, phase, rounds, observed, deviceDiagnostics}));
    throw error;
  } finally {
    clearTimeout(deadline);
    await invoke('close').catch(() => {});
    if (!exited) ownedCancellation.stopChild();
    await completion;
    ownedCancellation.finish();
    if (exitCode !== 0) process.exitCode = 1;
  }
}
run().catch(error => { console.error(error.message); process.exitCode = 1; });
