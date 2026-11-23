// 文件说明：真实Chromium/C++音频门控组件E2E；有限超时、独占进程与浏览器，非后端/MCP验收。
const {spawn, execFile} = require('node:child_process');
const {promisify} = require('node:util');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const run = promisify(execFile);
const root = path.resolve(__dirname, '../../..'); // 所有产物仅位于本任务worktree。
// validate 验收原C++事实和独立输出图；缺少采集/实际静音/恢复任一证据都不能通过。
function validate(completion, evidence, focusMode = false) {
  assert.equal(completion.kind, 'gateComplete');
  assert.deepEqual(completion.frames, [20, 20, 20]);
  for (const counts of [completion.captureFrames, evidence.capture]) {
    assert.ok(Array.isArray(counts) && counts.length === 3);
    assert.ok(counts.every(value => Number.isSafeInteger(value) && value >= 5));
  }
  assert.equal(evidence.failure, false);
  assert.deepEqual(evidence.played, [20, 20, 20]);
  assert.deepEqual(evidence.nonSilent, [20, 0, 20]);
  assert.ok(Array.isArray(evidence.peak) && evidence.peak.length === 3);
  assert.ok(evidence.peak.every(value => Number.isFinite(value) && value >= 0 && value <= 1));
  assert.ok(evidence.peak[0] > 0.1 && evidence.peak[2] > 0.1, 'no actual analyser output before/after gate');
  assert.equal(evidence.peak[1], 0, 'actual muted output was not zero');
  if (focusMode) {
    assert.deepEqual(completion.focusStates, ['acquired', 'suspended', 'restored', 'released']);
    assert.equal(completion.focusRevision, 4);
    assert.deepEqual(evidence.gates, [false, true, false, true, false].map((enabled, index) => ({ticket: String(index + 1), enabled})));
    assert.equal(evidence.releasePeak, 0, 'release must silence actual output graph');
  } else assert.deepEqual(evidence.gates, [{ticket: '1', enabled: false}, {ticket: '2', enabled: true}]);
}
// main 保留真实C++终态及独立输出图事实；finally只清理本次精确资源。
async function main() {
  const binary = path.resolve(process.env.UR_GATE_PROBE || path.join(root, '.temp/device-runtime/desktop/audio_gate_probe'));
  const port = Number(process.env.UR_GATE_PORT || 27987);
  const focusMode = process.env.UR_GATE_FOCUS === '1'; // 显式共享焦点组件模式，不是生产许可。
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  const session = `task143-audio-gate-${process.pid}`;
  const events = [];
  let terminal, child, reader;
  // browser 不打印配对凭据/HTTP内容，只返回结构化测试指标。
  async function browser(args) {
    const output = await run('agent-browser', ['--session', session, '--json', ...args], {
      cwd: root, timeout: 25000, maxBuffer: 1048576, env: {...process.env, AGENT_BROWSER_ARGS: '--no-sandbox'}
    });
    const body = JSON.parse(output.stdout);
    assert.equal(body.success, true);
    return body.data;
  }
  try {
    child = spawn(binary, [String(port), ...(focusMode ? ['--focus'] : [])], {cwd: root, stdio: ['ignore', 'pipe', 'pipe']});
    const exit = new Promise(resolve => {
      child.once('error', () => { terminal = {code: null, failure: true}; resolve(terminal); });
      child.once('exit', (code, signal) => { terminal = {code, signal}; resolve(terminal); });
    });
    child.stderr.on('data', () => {}); // 不转印任意第三方原始错误。
    reader = readline.createInterface({input: child.stdout});
    reader.on('line', line => { if (events.length < 8) { try { events.push(JSON.parse(line)); } catch { events.push({invalid: true}); } } });
    // wait 在原进程上观察事件，不因短暂等待另起服务或改端口。
    async function wait(predicate, timeout) {
      const deadline = Date.now() + timeout;
      while (!predicate()) {
        assert.ok(!terminal, 'probe terminated before evidence');
        assert.ok(Date.now() < deadline, 'probe evidence timeout');
        await new Promise(resolve => setTimeout(resolve, 20));
      }
    }
    await wait(() => events.some(value => value.kind === 'probeReady'), 5000);
    await browser(['open', `http://127.0.0.1:${port}`]);
    const snapshot = await browser(['snapshot', '-i']);
    assert.ok(snapshot);
    await browser(['click', '#enable']);
    await wait(() => events.some(value => value.kind === 'gateComplete'), 20000);
    assert.equal((await exit).code, 0);
    const completion = events.find(value => value.kind === 'gateComplete');
    const result = await browser(['eval', '-b', Buffer.from('JSON.parse(JSON.stringify(gateEvidence))').toString('base64')]);
    const evidence = result.result;
    validate(completion, evidence, focusMode);
    const report = {scope: focusMode ? 'cpp-shared-focus-real-webaudio-component-only' : 'cpp-real-webaudio-component-only', completion, evidence};
    const directory = path.join(root, '.temp/device-runtime', focusMode ? 'audio-focus-live' : 'audio-gate-live');
    fs.mkdirSync(directory, {recursive: true});
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report));
  } finally {
    if (child && !terminal) {
      child.kill('SIGTERM');
      const kill = setTimeout(() => { if (!terminal) child.kill('SIGKILL'); }, 5000);
      await new Promise(resolve => { if (terminal) return resolve(); child.once('exit', resolve); });
      clearTimeout(kill);
    }
    reader?.close();
    await browser(['close']);
  }
}
module.exports = {validate};
if (require.main === module) main().catch(() => { console.error('audioGateLiveFailed'); process.exitCode = 1; });
