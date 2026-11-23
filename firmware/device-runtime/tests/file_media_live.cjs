// 真实组件软件E2E：正式C++执行层→回环桥→实际浏览器文件解码；不冒充MQTT/后端/MCP验收。
const {spawn, execFile} = require('node:child_process');
const {promisify} = require('node:util');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const run = promisify(execFile);
const root = path.resolve(__dirname, '../../..'); // 输入/报告只来自当前任务worktree。

// browser 调用任务独占agent-browser会话，只返回结构化结果，不记录地址/配对凭据。
async function browser(session, args) {
  const result = await run('agent-browser', ['--session', session, '--json', ...args],
    {timeout: 25000, maxBuffer: 8388608, cwd: root,
      env: {...process.env, AGENT_BROWSER_ARGS: '--no-sandbox'}});
  const body = JSON.parse(result.stdout);
  if (!body.success) throw new Error('browserCommandFailed');
  return body.data;
}
// evaluate 将代码作为独立argv传递，不经shell解释；结果仅允许测试指标，不读设备/浏览器凭据。
async function evaluate(session, expression) {
  const data = await browser(session, ['eval', '-b', Buffer.from(expression).toString('base64')]);
  return data.result;
}
// waveform 生成可重复的非静音PCM16 WAV语料，是实际解码输入，不是伪造播放结果。
function waveform(filename) {
  const rate = 48000, count = rate * 20;
  const bytes = Buffer.alloc(44 + count * 2);
  bytes.write('RIFF', 0); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(1, 22);
  bytes.writeUInt32LE(rate, 24); bytes.writeUInt32LE(rate * 2, 28);
  bytes.writeUInt16LE(2, 32); bytes.writeUInt16LE(16, 34);
  bytes.write('data', 36); bytes.writeUInt32LE(count * 2, 40);
  for (let index = 0; index < count; ++index)
    bytes.writeInt16LE(Math.round(Math.sin(index * 2 * Math.PI * 440 / rate) * 8192), 44 + index * 2);
  fs.writeFileSync(filename, bytes);
}
// fixtures 默认用ffmpeg生成真实带音轨的有限MP4；显式视频输入仍需真实解码/像素验证，不能跳过。
async function fixtures(directory) {
  const audio = path.join(directory, 'tone.wav');
  waveform(audio);
  const override = process.env.UR_FILE_VIDEO_FIXTURE;
  const video = override ? path.resolve(override) : path.join(directory, 'pattern.mp4');
  if (!override) await run('ffmpeg', ['-nostdin', '-hide_banner', '-loglevel', 'error',
    '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=15:duration=20',
    '-f', 'lavfi', '-i', 'sine=frequency=660:sample_rate=48000:duration=20',
    '-c:v', 'libx264', '-threads', '1', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
    '-movflags', '+faststart', '-shortest', video], {timeout: 60000});
  const mime = video.endsWith('.webm') ? 'video/webm' : 'video/mp4';
  const stat = fs.statSync(video);
  if (!stat.isFile() || stat.size < 1 || stat.size > 8388608) throw new Error('invalidVideoFixture');
  return [{name: 'music', mime: 'audio/wav', file: audio}, {name: 'video', mime, file: video}];
}
// Probe 仅持有本次创建的C++进程，记录真实核心回执；线程/浏览器事实由生产组件负责。
class Probe {
  constructor(binary, port, fixture) {
    this.events = []; // 有界真实stdout证据，不存本地命令中的源地址或令牌。
    this.sessionMode = process.env.UR_FILE_SESSION === '1'; // 显式会话夹具，不启用生产设备身份。
    this.dynamicMode = process.env.UR_FILE_DYNAMIC === '1'; // 新入口直到原邀请到达才解析固定受信资产。
    this.sessionId = this.dynamicMode ? `dynamic-${fixture.name}-${port}` : 'media-session'; // 与探针预置绑定不同。
    this.playbackId = this.dynamicMode ? `dynamic-player-${port}` : 'player-1'; // 平台夹具创建的独立实例。
    this.sequence = this.sessionMode ? 2 : 0; // 邀请/建立先占两个平台事件水位。
    this.revision = 0;
    this.focusMode = process.env.UR_FILE_FOCUS === '1'; // 显式正式焦点核心组件路径，仍无后端身份。
    this.laneMode = process.env.UR_FILE_LANE === '1'; // 固定物理输出路径在动态播放器前关闭。
    this.focusRevision = 0; // 独立全局输出修订号，不与播放器修订号混用。
    this.focusResources = fixture.name === 'music' ? ['speaker'] : ['speaker', 'display'];
    this.child = spawn(binary, [String(port), fixture.mime, fixture.file,
      ...(this.sessionMode ? [this.dynamicMode ? '--dynamic-session' : '--session'] : [])],
      {cwd: root, stdio: ['pipe', 'pipe', 'pipe']});
    this.exit = new Promise(resolve => {
      this.child.once('error', () => { this.terminal = {code: null, signal: null, error: 'spawnFailure'}; resolve(this.terminal); });
      this.child.once('exit', (code, signal) => { this.terminal = {code, signal}; resolve(this.terminal); });
    });
    this.lines = readline.createInterface({input: this.child.stdout});
    this.lines.on('line', line => {
      if (this.events.length >= 128) { this.invalid = true; return; }
      try { this.events.push(JSON.parse(line)); } catch { this.invalid = true; }
    });
    this.child.stderr.on('data', () => { this.stderrObserved = true; }); // 不转印任意原始错误/源信息。
  }
  // establish 真实C++协调器回原接听请求，平台ACK/建立为明确夹具，不冒称正式后端。
  async establish(fixture, prepare) {
    if (!this.sessionMode) { await prepare(); return; }
    const invite = {msgToken: 'probe-invite', method: 'sessionInvite', sessionId: this.sessionId, eventSeq: 1,
      params: {purpose: 'mediaPlayback', admission: 'direct', transport: 'none', tracks: [], expiresAt: 30000,
        playbackId: this.playbackId, mediaSource: {contentRef: 'content-1'}, inputModalities: [],
        outputModalities: fixture.name === 'music' ? ['audio'] : ['audio', 'video']}};
    this.child.stdin.write(JSON.stringify(invite) + '\n');
    // 原邀请在浏览器初始化前进入C++；只允许原实例准备完成后接听，不能靠宿主提前过滤。
    await prepare();
    const accept = await this.wait(value => value.method === 'sessionAccept');
    assert.equal(accept.msgToken, invite.msgToken);
    assert.equal(accept.sessionId, invite.sessionId);
    assert.deepEqual(accept.params, {acceptedTracks: []});
    this.child.stdin.write(JSON.stringify({msgToken: 'probe-invite', method: 'requestAck', sessionId: this.sessionId,
      code: 200, data: {requestMethod: 'sessionAccept', result: {state: 'established'}}}) + '\n');
    this.child.stdin.write(JSON.stringify({msgToken: 'probe-established', method: 'sessionEstablished', sessionId: this.sessionId,
      eventSeq: 2, data: {state: 'established', mediaRevision: 0}}) + '\n');
  }
  // wait 观察同一进程/事件，超时是失败，不新建进程或用握手当实际媒体。
  async wait(predicate, timeout = 12000) {
    const deadline = Date.now() + timeout;
    while (true) {
      if (this.invalid) throw new Error('invalidProbeOutput');
      const event = this.events.find(predicate);
      if (event) return event;
      if (this.terminal) throw new Error(`probeExitedBeforeFact:code=${this.terminal.code}:signal=${this.terminal.signal}`);
      if (Date.now() >= deadline) {
        // 只记录有界固定事件类型及错误码，不输出身份、下载地址或桥接凭据。
        const facts = this.events.slice(-8).map(value => ({kind: value.kind,
          method: value.method, code: value.code}));
        const error = new Error('probeFactTimeout');
        error.probeFacts = facts;
        error.probePhase = this.phase || 'startup';
        throw error;
      }
      await new Promise(resolve => setTimeout(resolve, 10));
    }
  }
  // command 发送原协议控制并等待同token实际回执，Accepted不算成功。
  async command(operation, extra = {}) {
    this.phase = operation; // 固定测试操作名，仅用于失败阶段定位，不含协议正文。
    const token = `fixture-${++this.sequence}`;
    const request = {msgToken: token, method: 'playbackControl', sessionId: this.sessionId,
      eventSeq: this.sequence, params: {playbackId: this.playbackId, operation,
        expectedRevision: this.revision, ...extra}};
    this.child.stdin.write(JSON.stringify(request) + '\n');
    const reply = await this.wait(value => value.method === 'playbackState' && value.msgToken === token);
    assert.equal(reply.code, 200, `${operation} did not complete successfully`);
    assert.equal(reply.sessionId, this.sessionId);
    assert.equal(reply.data.playbackId, this.playbackId);
    assert.equal(reply.data.revision, this.revision + 1);
    this.revision = reply.data.revision;
    return reply.data;
  }
  // focusCommand等待正式核心原token整组事实；门控私有票据不充当协议成功应答。
  async focusCommand(operation) {
    this.phase = `focus-${operation}`;
    const token = `focus-${++this.sequence}`;
    const request = {msgToken: token, method: 'focusControl', sessionId: this.sessionId,
      eventSeq: this.sequence, params: {operation, focusId: 'probe-focus', resources: this.focusResources,
        expectedRevision: this.focusRevision, priority: 'conversation', resumePolicy: 'explicit',
        ...(['acquire', 'restore'].includes(operation) ? {expiresAt: 120000} : {})}};
    this.child.stdin.write(JSON.stringify(request) + '\n');
    const reply = await this.wait(value => value.method === 'focusState' && value.msgToken === token);
    assert.equal(reply.code, 200, `${operation} focus did not actually complete`);
    assert.equal(reply.sessionId, this.sessionId);
    assert.equal(reply.data.focusId, 'probe-focus');
    assert.deepEqual(reply.data.resources, this.focusResources);
    assert.equal(reply.data.revision, this.focusRevision + 1);
    assert.equal(reply.data.state, {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'}[operation]);
    this.focusRevision = reply.data.revision;
    return reply;
  }
  // cleanup 只结束本次子进程；成功路径另检查released和code=0，不用SIGTERM当成功停止。
  async cleanup() {
    this.lines.close();
    this.child.stdin.end();
    if (this.terminal) return this.exit;
    if (!this.terminal) this.child.kill('SIGTERM');
    let timer;
    const end = await Promise.race([this.exit, new Promise(resolve => { timer = setTimeout(() => resolve(null), 5000); })]);
    clearTimeout(timer);
    if (!end && !this.terminal) { this.child.kill('SIGKILL'); await this.exit; }
  }
}

// execute 验证实际音频/像素、暂停/seek/倍率/恢复/停止；每个节目实例独立，不复用已终结对象。
async function execute(binary, directory, session, fixture, port, browserState) {
  const probe = new Probe(binary, port, fixture);
  try {
    await probe.wait(value => value.kind === 'probeReady');
    await probe.establish(fixture, async () => {
      browserState.attempted = true; // open失败也可能已创建本会话，finally仍须精确关闭。
      await browser(session, ['open', `http://127.0.0.1:${port}${probe.laneMode ? '/#lane=1' : ''}`]);
      const snapshot = await browser(session, ['snapshot', '-i']);
      assert.ok(snapshot); // 此次页面快照后使用固定按钮，不沿用上次页面引用。
      await browser(session, ['click', '#enable']);
      await probe.wait(value => value.kind === 'driverReady');
    });
    const focusFacts = []; // 仅保留正式原协议回执，不用探针状态证明取得租约。
    if (probe.focusMode) {
      await probe.wait(value => value.kind === 'focusInitialIdle' && value.revision === 0);
    }
    assert.equal((await probe.command('load', {contentRef: 'content-1'})).state, 'loaded');
    if (probe.focusMode) focusFacts.push(await probe.focusCommand('acquire'));
    assert.equal((await probe.command('play')).state, 'playing');
    // 独立路径门控通过实际C++请求，不在eval里直接改组件状态或伪造完成。
    probe.phase = 'outputGateOff';
    let gateOff;
    if (probe.focusMode) {
      gateOff = await probe.focusCommand('suspend');
      focusFacts.push(gateOff);
    } else {
      probe.child.stdin.write(JSON.stringify({probeOutputGate: false}) + '\n');
      gateOff = await probe.wait(v => v.kind === 'outputGateConfirmed' && !v.enabled);
    }
    const quiet = await evaluate(session, `(async () => {
      const before = {samples: fileProbe.samples, energy: fileProbe.energy, frames: fileProbe.frames};
      await new Promise(resolve => setTimeout(resolve, 200));
      return {before, after: {samples: fileProbe.samples, energy: fileProbe.energy, frames: fileProbe.frames},
        hidden: fileProbe.media.hidden, rects: fileProbe.media.getClientRects().length,
        lanePower: typeof fileLanePower === 'function' ? fileLanePower() : -1,
        contextState: context.state, paused: fileProbe.media.paused, state: fileProbe.state};
    })()`);
    if (probe.laneMode) assert.equal(quiet.lanePower, 0, '固定路径末端仍有实际声音');
    else {
      assert.deepEqual(quiet.after, quiet.before, '门控确认后仍出现新的实际音频/视频输出');
      assert.equal(quiet.hidden, true);
    }
    assert.equal(quiet.rects, 0);
    assert.equal(quiet.contextState, 'running');
    assert.equal(quiet.paused, false); // 保留播放意图，门控不是pause控制或共享上下文停止。
    assert.equal(quiet.state, 'playing');
    await browser(session, ['screenshot', path.join(directory, `${fixture.name}-gated.png`)]);
    probe.phase = 'outputGateOn';
    let gateOn;
    if (probe.focusMode) {
      gateOn = await probe.focusCommand('restore');
      focusFacts.push(gateOn);
      assert.notEqual(gateOn.msgToken, gateOff.msgToken);
    } else {
      probe.child.stdin.write(JSON.stringify({probeOutputGate: true}) + '\n');
      gateOn = await probe.wait(v => v.kind === 'outputGateConfirmed' && v.enabled);
      assert.notEqual(gateOn.ticket, gateOff.ticket);
    }
    await browser(session, ['wait', '--fn', `fileProbe.samples > ${quiet.after.samples} &&
      ${fixture.name === 'video' ? `fileProbe.frames > ${quiet.after.frames} && !fileProbe.media.hidden` : 'true'} &&
      ${probe.laneMode ? 'fileLanePower() > 0.001' : 'true'}`]);
    const groupGate = {off: gateOff, on: gateOn, quiet};
    const decoded = await evaluate(session, `(() => {
      const result = {samples: fileProbe.samples, energy: fileProbe.energy, frames: fileProbe.frames,
        lanePower: typeof fileLanePower === 'function' ? fileLanePower() : -1,
        contextState: context.state, paused: fileProbe.media.paused};
      if (fileProbe.binding.mime.startsWith('video/')) {
        const canvas = document.createElement('canvas'); canvas.width = 8; canvas.height = 8;
        canvas.getContext('2d').drawImage(fileProbe.media, 0, 0, 8, 8);
        const rgb = canvas.getContext('2d').getImageData(0, 0, 8, 8).data;
        result.rgbEnergy = Array.from(rgb).reduce((sum, value, index) => sum + (index % 4 === 3 ? 0 : value), 0);
        result.videoWidth = fileProbe.media.videoWidth; result.videoHeight = fileProbe.media.videoHeight;
      }
      return result;
    })()`);
    assert.ok(decoded.samples >= 256 && decoded.energy > 1, 'no actual non-silent decoded audio');
    assert.equal(decoded.contextState, 'running');
    assert.equal(decoded.paused, false);
    if (fixture.name === 'video') {
      assert.ok(decoded.frames > 0 && decoded.videoWidth > 0 && decoded.videoHeight > 0);
      assert.ok(decoded.rgbEnergy > 64, 'no actual rendered RGB frame');
      await browser(session, ['screenshot', path.join(directory, 'video.png')]);
    }
    // 实际暂停共享上下文再探测门控；未收到输出块不得变成静音/释放证明，随后显式恢复继续原实例。
    const suspendedGate = await evaluate(session, `(async () => {
      await context.suspend();
      let confirmed = false;
      try { await fileProbe.gate(false); confirmed = true; } catch {}
      const state = context.state;
      await context.resume();
      return {confirmed, state, resumed: context.state};
    })()`);
    assert.deepEqual(suspendedGate, {confirmed: false, state: 'suspended', resumed: 'running'});
    assert.equal((await probe.command('pause')).state, 'paused');
    const paused = await evaluate(session, '({paused: fileProbe.media.paused, hidden: fileProbe.media.hidden, context: context.state})');
    assert.deepEqual(paused, {paused: true, hidden: true, context: 'running'});
    // seek实际关键帧判据（仅带视频轨节目）：观察与seek命令并发挂载，暂停视频默认不渲染，
    // 临时恢复可见只为让浏览器真实呈现目标帧。实际位置只取requestVideoFrameCallback的mediaTime，
    // 不以currentTime或请求位置伪造帧时间，也不猜FPS容差；seek呈现帧不得越过请求位置（媒体seek语义）。
    const seekFrame = fixture.name === 'video' ? evaluate(session, `(async () => {
      const media = fileProbe.media;
      if (!media.paused) return {observed: false, reason: 'notPaused'};
      media.hidden = false;
      return await new Promise(resolve => {
        const timer = setTimeout(() => resolve({observed: false, reason: 'frameTimeout',
          currentTimeMs: Math.round(media.currentTime * 1000), durationMs: Math.round(media.duration * 1000)}), 6000);
        media.requestVideoFrameCallback((now, metadata) => {
          clearTimeout(timer);
          resolve({observed: true, mediaTimeMs: Math.round(metadata.mediaTime * 1000),
            currentTimeMs: Math.round(media.currentTime * 1000), durationMs: Math.round(media.duration * 1000)});
        });
      });
    })()`).catch(error => ({observed: false, reason: String(error.message).slice(0, 60)})) : null;
    const seek = await probe.command('seek', {positionMs: 1500});
    if (seek.state === 'seeking') await probe.wait(value => value.method === 'playbackState' &&
      !('code' in value) && value.data.state === 'paused' && value.data.revision === probe.revision + 1);
    if (seek.state === 'seeking') ++probe.revision;
    if (seekFrame) {
      const frame = await seekFrame;
      assert.equal(frame.observed, true, `seek目标帧未实际呈现(${frame.reason || 'unknown'})，无法定论实际关键帧位置`);
      assert.ok(Number.isSafeInteger(frame.mediaTimeMs) && frame.mediaTimeMs >= 0 && frame.mediaTimeMs <= 1500,
        `seek实际呈现位置${frame.mediaTimeMs}越过了请求位置1500`);
    }
    assert.equal((await probe.command('setRate', {rate: 1.5})).rate, 1.5);
    assert.equal((await probe.command('play')).state, 'playing');
    // 自然EOF判据段（UR_FILE_NATURAL_END=1显式开启）：1.5倍速playing seek至末尾前2秒，播放至自然结束。
    // ended必须在输出仍许可时到达（引擎对playing恢复事实要求输出许可），故置于焦点释放之前；
    // 自然终态后stop必为StateConflict，终态由sessionEnded进入释放链路，默认关闭保持原stop判据。
    if (process.env.UR_FILE_NATURAL_END === '1') {
      const durationMs = seek.data.durationMs;
      assert.ok(Number.isSafeInteger(durationMs) && durationMs > 2000, '自然结束判据需要有效的媒体时长回执');
      const tail = await probe.command('seek', {positionMs: durationMs - 2000});
      if (tail.state === 'seeking') await probe.wait(value => value.method === 'playbackState' &&
        !('code' in value) && value.data.state === 'playing' && value.data.revision === probe.revision + 1);
      if (tail.state === 'seeking') ++probe.revision;
      const ended = await probe.wait(value => value.method === 'playbackState' &&
        !('code' in value) && value.data.state === 'ended');
      assert.equal(ended.data.positionMs, ended.data.durationMs,
        `自然结束位置${ended.data.positionMs}不等于媒体时长${ended.data.durationMs}`);
    }
    if (probe.focusMode) focusFacts.push(await probe.focusCommand('release'));
    if (process.env.UR_FILE_NATURAL_END !== '1')
      assert.equal((await probe.command('stop')).state, 'stopped');
    if (probe.sessionMode) {
      // 明确夹具平台终态，C++必须实际关闭并等待释放；不把stop应答或子进程退出当释放。
      probe.child.stdin.write(JSON.stringify({msgToken: 'probe-ended', method: 'sessionEnded', sessionId: probe.sessionId,
        eventSeq: ++probe.sequence, data: {state: 'ended', reason: 'playbackStopped'}}) + '\n');
    }
    const release = await probe.wait(value => value.kind === 'released');
    if (probe.sessionMode) {
      assert.deepEqual(release,
        {kind: 'released', sessionEnded: true, driverReleased: true, sessionReleased: true, platformEnded: true});
      const fact = await probe.wait(value => value.method === 'playbackState' && value.msgToken === 'probe-ended');
      assert.deepEqual(fact, {msgToken: 'probe-ended', method: 'playbackState', sessionId: probe.sessionId,
        data: {playbackId: probe.playbackId, released: true, endEventSeq: probe.sequence}});
    }
    const terminal = await probe.exit;
    assert.equal(terminal.code, 0);
    assert.equal(probe.events.some(value => value.kind === 'admissionRejected'), false,
      'legal fixture flow contains rejected protocol message');
    fs.writeFileSync(path.join(directory, `${fixture.name}.jsonl`), probe.events.map(value => JSON.stringify(value)).join('\n') + '\n');
    return {name: fixture.name, identity: {sessionId: probe.sessionId, playbackId: probe.playbackId},
      decoded, groupGate, focusFacts, suspendedGate, paused, release, exitCode: terminal.code,
      states: probe.events.filter(value => value.method === 'playbackState' && value.data?.state).map(value => value.data.state)};
  } catch (error) {
    fs.writeFileSync(path.join(directory, `${fixture.name}-failed.jsonl`), probe.events.map(value => JSON.stringify(value)).join('\n') + '\n');
    // 只读取本具名页面固定诊断；保留失败当时事实，不借清理结果重写失败或泄露异常原文。
    if (browserState.attempted) error.mediaDiagnostic = await evaluate(session, `(() => ({
      state: ['loaded','playing','paused','seeking','stopped','ended','failed'].includes(fileProbe?.state) ? fileProbe.state : 'unknown',
      gateFailure: ['audio','paint','closed','audioThread'].includes(fileProbe?.outputGateFailure) ? fileProbe.outputGateFailure : 'none',
      audioFailed: fileProbe?.audioFailed === true, outputBusy: fileProbe?.outputBusy === true,
      contextRunning: context?.state === 'running', documentVisible: document.visibilityState === 'visible'
    }))()`).catch(() => ({unavailable: true}));
    throw error;
  } finally { await probe.cleanup(); }
}

// main 报告仅软件组件，不修改真实后端/模型；缺依赖或任一失败都返回非零并保留证据。
async function main() {
  const directory = fs.mkdtempSync(path.join(root, '.temp/device-runtime/file-live-'));
  const binary = process.env.UR_FILE_PROBE_BIN || path.join(root, '.temp/device-runtime/desktop/file_media_probe');
  const session = `task143-file-${Date.now()}-${process.pid}`;
  const report = {scope: process.env.UR_FILE_LANE === '1' ? 'cpp-fixed-output-lane-browser-file-fixture'
    : process.env.UR_FILE_FOCUS === '1' ? 'cpp-focus-driver-browser-file-fixture'
    : process.env.UR_FILE_DYNAMIC === '1' ? 'cpp-browser-file-resolved-endpoint-fixture'
    : process.env.UR_FILE_SESSION === '1' ? 'cpp-browser-file-endpoint-fixture' : 'cpp-browser-file-component',
  naturalEnd: process.env.UR_FILE_NATURAL_END === '1', completed: false, cases: []};
  const browserState = {attempted: false}; // 缺依赖时不为清理而启动浏览器。
  try {
    if (process.env.UR_FILE_DYNAMIC === '1' && process.env.UR_FILE_SESSION !== '1')
      throw new Error('dynamicRequiresSession'); // 不能以组件入口成功冒充动态端点验证。
    const inputs = await fixtures(directory);
    const port = Number(process.env.UR_FILE_PROBE_PORT || 40000 + process.pid % 10000);
    // 允许宿主显式分配两条回环端口；初始化失败仍立即退出，不自动换端口或节点。
    if (!Number.isInteger(port) || port < 1024 || port >= 65535) throw new Error('invalidProbePort');
    for (let index = 0; index < inputs.length; ++index)
      report.cases.push(await execute(binary, directory, session, inputs[index], port + index, browserState));
    report.completed = true;
  } catch (error) {
    report.failure = error.code === 'ENOENT' ? 'missingDependency' : String(error.message).slice(0, 160);
    if (error.probeFacts) report.probeDiagnostic = {phase: error.probePhase, facts: error.probeFacts};
    if (error.mediaDiagnostic) report.mediaDiagnostic = error.mediaDiagnostic;
    process.exitCode = 1;
  } finally {
    if (browserState.attempted) await browser(session, ['close']).catch(() => {
      report.completed = false; report.browserCleanupFailed = true; process.exitCode = 1;
    });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
    process.stdout.write(JSON.stringify({directory, ...report}) + '\n');
  }
}
if (require.main === module) main();
module.exports = {waveform, Probe};
