// 文件说明：显式开启的C++纯文字真实后端/模型验收；凭据仅从环境注入，报告不含对话正文和设备身份。
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {StringDecoder} = require('node:string_decoder');
const {performance} = require('node:perf_hooks');

// cases是固定效果语料，第二轮必须复用同一进程/会话，不以重连掩盖多轮缺陷。
const cases = [
  {input: '请只回复：第一轮纯文字通道成功。不要调用任何工具。', keyword: '第一轮纯文字通道成功'},
  {input: '请只回复：第二轮纯文字通道成功。不要调用任何工具。', keyword: '第二轮纯文字通道成功'},
];

// run只启动正式device_text；deps仅用于明确的离线控制面回归，真实入口不能从环境替换spawn。
async function run(env = process.env, deps = {}) {
  assert.equal(env.UR_TEXT_MODEL_LIVE, '1', 'explicit_live_opt_in_required');
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET', 'UR_MQTT_HOST', 'UR_MQTT_PORT'])
    assert.ok(typeof env[key] === 'string' && env[key].length > 0, 'device_configuration_missing');
  const binary = env.UR_DEVICE_TEXT_BIN;
  assert.ok(typeof binary === 'string' && path.isAbsolute(binary), 'absolute_device_binary_required');
  if (!deps.spawn) assert.ok(fs.lstatSync(binary).isFile(), 'device_binary_missing');
  const child = (deps.spawn || spawn)(binary, ['--tui'], {env, stdio: ['pipe', 'pipe', 'pipe']});
  const facts = []; // 只接固定stdout JSON诊断，原正文仅在有界内存中验证关键词。
  const decoder = new StringDecoder('utf8'); // 跨pipe块的中文不能被拆成替换字符。
  let stdout = '', text = '', textBytes = 0, closed = false, exitCode = null, failure = false;
  let established = false, sentRound = 0, acceptedRound = 0, completedRound = 0, quitting = false;
  let firstTextAt = null, submittedAt = null;
  const deadline = performance.now() + (deps.timeoutMs || 180000);
  const allowed = new Set(['sessionEstablished', 'transport', 'inputAccepted', 'round', 'replyCompleted',
    'sessionEnded', 'completedRounds', 'error']); // 取消另有专门用例，此验收固定两轮正常请求。
  const exited = new Promise(resolve => {
    child.once('error', () => { failure = true; });
    child.once('close', code => { closed = true; exitCode = code; resolve(); });
  });
  for (const stream of [child.stdin, child.stdout, child.stderr]) stream.on('error', () => { failure = true; });
  // waitClosed清理自身观察定时器；Promise先结束不能留下五秒空转或以超时冒充close。
  const waitClosed = async timeout => {
    let timer;
    try { return await Promise.race([exited.then(() => true),
      new Promise(resolve => { timer = setTimeout(() => resolve(false), timeout); })]); }
    finally { clearTimeout(timer); }
  };
  child.stdout.on('data', bytes => {
    stdout += bytes.toString('utf8');
    if (stdout.length > 4096) { failure = true; return; }
    let end;
    while ((end = stdout.indexOf('\n')) >= 0) {
      const line = stdout.slice(0, end); stdout = stdout.slice(end + 1);
      try {
        const value = JSON.parse(line);
        assert.ok(value && !Array.isArray(value) && Object.keys(value).every(key => allowed.has(key)));
        assert.ok(facts.length < 16);
        const keys = Object.keys(value).sort();
        // 原CLI事实按单一会话/本地提交关联；重复终态、跨轮ACK或仅相同累计计数不能证明两轮。
        if (value.error) failure = true;
        else if (value.sessionEstablished === true) {
          assert.deepEqual(keys, ['sessionEstablished', 'transport']);
          assert.equal(value.transport, 'none'); assert.equal(established, false); established = true;
        } else if (value.inputAccepted === true) {
          assert.deepEqual(keys, ['inputAccepted', 'round']);
          assert.equal(value.round, sentRound); assert.equal(value.round, acceptedRound + 1);
          assert.equal(completedRound, acceptedRound); acceptedRound = value.round;
        } else if (value.replyCompleted === true) {
          assert.deepEqual(keys, ['replyCompleted', 'round']);
          assert.equal(value.round, acceptedRound); assert.equal(value.round, completedRound + 1);
          completedRound = value.round;
        } else if (value.sessionEnded === true) {
          assert.deepEqual(keys, ['completedRounds', 'sessionEnded']);
          assert.equal(quitting, true); assert.equal(value.completedRounds, 2); assert.equal(completedRound, 2);
        } else throw Error('invalid_fixed_device_fact');
        facts.push(value);
      } catch { failure = true; }
    }
  });
  child.stderr.on('data', bytes => {
    textBytes += bytes.length;
    if (textBytes > 65536) { failure = true; return; }
    const value = decoder.write(bytes);
    // stdout完成与stderr换行可跨管道迟到；只有非空白正文才是下一轮首文本。
    if (value.trim().length && submittedAt !== null && firstTextAt === null) firstTextAt = performance.now();
    text += value;
  });
  // wait只观察原进程，期限或退出均失败，不重启、重发语料或续许可。
  const wait = async predicate => {
    while (!predicate()) {
      assert.ok(!failure && !closed && performance.now() < deadline, 'text_live_observation_failed');
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.equal(failure, false, 'text_live_device_failed');
  };
  // write以原stdin交用户输入，不直接投递MQTT或调用平台模型API。
  const write = value => new Promise((resolve, reject) => {
    child.stdin.write(value + '\n', error => error ? reject(Error('text_live_stdin_failed')) : resolve());
  });
  const rounds = [];
  let passed = false, phase = 'sessionEstablishment', failureReport = null;
  try {
    // 资源宿主可只观察正式spawn的原子进程；这不是替换spawn，也不改变会话或媒体行为。
    if (deps.onSpawn) {
      phase = 'processObservation';
      await deps.onSpawn(child);
      phase = 'sessionEstablishment';
    }
    await wait(() => facts.some(value => value.sessionEstablished === true && value.transport === 'none'));
    for (const [index, item] of cases.entries()) {
      const previousLength = text.length;
      firstTextAt = null; submittedAt = performance.now();
      sentRound = index + 1;
      phase = 'inputAcceptance';
      await write(item.input);
      await wait(() => facts.some(value => value.inputAccepted === true && value.round === index + 1));
      phase = 'replyCompletion';
      await wait(() => facts.some(value => value.replyCompleted === true && value.round === index + 1));
      phase = 'replyEffect';
      await wait(() => text.slice(previousLength).includes(item.keyword));
      assert.ok(firstTextAt !== null && firstTextAt >= submittedAt, 'text_live_first_text_missing');
      rounds.push({round: index + 1, keywordMatched: true,
        firstTextMs: Math.round(firstTextAt - submittedAt), completedMs: Math.round(performance.now() - submittedAt)});
    }
    phase = 'sessionClose'; quitting = true; await write('/quit');
    await wait(() => facts.some(value => value.sessionEnded === true && value.completedRounds === 2));
    assert.equal(await waitClosed(5000), true, 'text_live_exit_timeout');
    assert.equal(exitCode, 0, 'text_live_exit_failed');
    assert.equal(stdout.length, 0, 'text_live_partial_diagnostic');
    passed = true;
    return {transport: 'none', completedRounds: 2, rounds, ownedProcessReleased: closed,
      evidence: deps.spawn ? 'offline-control-fixture' : 'cpp-backend-model', hardwareVerified: false};
  } catch {
    // 失败同样保留实际阶段/数量/释放事实；不投射CLI异常、配置、关键词或模型正文。
    const error = new Error('text_model_live_failed');
    failureReport = error.facts = {failed: true, scenario: 'textOnlyModel', phase,
      completedRounds: rounds.length, ownedProcessReleased: false,
      evidence: deps.spawn ? 'offline-control-fixture' : 'cpp-backend-model'};
    throw error;
  } finally {
    child.stdin.end();
    // 只清理本函数spawn的原子进程；kill受理不当作退出，失败样本不再启动新实例。
    if (!closed) {
      child.kill('SIGTERM');
      await waitClosed(2500);
    }
    if (!closed) {
      child.kill('SIGKILL');
      await waitClosed(5000);
    }
    if (failureReport) failureReport.ownedProcessReleased = closed;
    if (!closed) {
      const error = new Error('text_live_owned_process_not_released');
      error.facts = failureReport || {failed: true, scenario: 'textOnlyModel', phase: 'resourceRelease',
        completedRounds: rounds.length, ownedProcessReleased: false,
        evidence: deps.spawn ? 'offline-control-fixture' : 'cpp-backend-model'};
      error.facts.cleanupFailed = true;
      throw error;
    }
    if (!passed) text = ''; // 对话不写入失败报告或自由异常。
  }
}
module.exports = {cases, run};
if (require.main === module) run().then(value => console.log(JSON.stringify(value))).catch(error => {
  console.error(JSON.stringify(error.facts || {failed: true, scenario: 'textOnlyModel', phase: 'configuration'})); process.exitCode = 1;
});
