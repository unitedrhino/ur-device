// 文件说明：显式开启的none纯文字客户端工具真实后端/模型验收；同一原进程两轮主题切换与一轮取消轮，
// 判据链固定 FrontendTools协商→toolCallExecute下行→设备实际执行→toolCallSubmit上行→明确续答→respTextDone/respDone→会话释放；
// 凭据仅从环境注入，stdout只接固定JSON事实行，stderr正文仅做有界关键词验证，报告不携带对话与设备身份。
// 设备侧合同（text_main接入客户端工具时实现）：装配真实ClientToolDisplayPort后，在sessionEstablished后输出
// {"clientToolsNegotiated":true}；request_theme被真实接纳时输出{"clientToolStarted":true,"round":N,"theme":"dark|light"}；
// take_client_tool_receipt每条原ACK输出{"clientToolSubmitted":true,"code":C,"continuation":"K","receiptIndex":I}。
// 取消判据的协议依据：respCancel使原回复的pending调用在持久账本转cancelled，迟到toolCallSubmit被
// clienttool.ErrExpired拒绝并折叠为requestAck code=409（reason=requestConflict），设备回执continuation=unknown。
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {StringDecoder} = require('node:string_decoder');
const {performance} = require('node:perf_hooks');

// rounds固定两轮不同目标主题；取消轮在设备接纳执行后立即取消，迟到提交必须被拒且回复不得复活。
const rounds = [
  {theme: 'dark', input: '请调用客户端工具set_local_display_theme，将当前设备界面切换为深色主题dark。实际成功后请只回复：第一轮主题切换完成。不要调用其他工具。', keyword: '第一轮主题切换完成'},
  {theme: 'light', input: '请调用客户端工具set_local_display_theme，将当前设备界面切换为浅色主题light。实际成功后请只回复：第二轮主题切换完成。不要调用其他工具。', keyword: '第二轮主题切换完成'},
];
// cancelRound不带关键词：目标回复应在取消后终止，不存在可供关键词匹配的续答正文。
const cancelRound = {theme: 'dark', input: '请调用客户端工具set_local_display_theme，将当前设备界面切换为深色主题dark。实际成功后请简短确认，不要调用其他工具。'};

// run只启动正式device_text；deps仅用于明确的离线控制面回归，真实入口不能从环境替换spawn。
async function run(env = process.env, deps = {}) {
  assert.equal(env.UR_NONE_CLIENT_TOOL_LIVE, '1', 'explicit_live_opt_in_required');
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET', 'UR_MQTT_HOST', 'UR_MQTT_PORT'])
    assert.ok(typeof env[key] === 'string' && env[key].length > 0, 'device_configuration_missing');
  const binary = env.UR_DEVICE_TEXT_BIN;
  assert.ok(typeof binary === 'string' && path.isAbsolute(binary), 'absolute_device_binary_required');
  if (!deps.spawn) assert.ok(fs.lstatSync(binary).isFile(), 'device_binary_missing');
  const child = (deps.spawn || spawn)(binary, ['--tui'], {env, stdio: ['pipe', 'pipe', 'pipe']});
  const facts = []; // 只接固定stdout JSON诊断，原正文仅在有界内存中验证关键词。
  const decoder = new StringDecoder('utf8'); // 跨pipe块的中文不能被拆成替换字符。
  const receipts = []; // 原toolCallSubmit回执按receiptIndex单调收纳，轮次与取消结局都从这里判定。
  let stdout = '', text = '', textBytes = 0, closed = false, exitCode = null, failure = false;
  let established = false, negotiated = false, sentRound = 0, acceptedRound = 0, completedRound = 0;
  let lastReceiptIndex = 0, cancelRequested = 0, cancelled = false, cancelSettled = false, quitting = false;
  const startedRounds = new Set(); // 每轮至多一次真实执行接纳；重投Execute不得再次产生执行事实。
  let firstTextAt = null, submittedAt = null;
  const deadline = performance.now() + (deps.timeoutMs || 240000);
  // 取消轮另有专门判据；附件路径在本验收环境不启用，未知事实键一律失败。
  const allowed = new Set(['sessionEstablished', 'transport', 'clientToolsNegotiated', 'inputAccepted', 'round',
    'clientToolStarted', 'theme', 'clientToolSubmitted', 'code', 'continuation', 'receiptIndex',
    'replyCompleted', 'replyCancelled', 'cancelRequested', 'cancelAcknowledged', 'sessionEnded',
    'completedRounds', 'error']);
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
    if (stdout.length > 8192) { failure = true; return; }
    let end;
    while ((end = stdout.indexOf('\n')) >= 0) {
      const line = stdout.slice(0, end); stdout = stdout.slice(end + 1);
      try {
        const value = JSON.parse(line);
        assert.ok(value && !Array.isArray(value) && Object.keys(value).every(key => allowed.has(key)));
        assert.ok(facts.length < 24);
        const keys = Object.keys(value).sort();
        // 原CLI事实按单一会话/本地提交关联；重复终态、跨轮ACK或仅相同累计计数不能证明多轮。
        if (value.error) failure = true;
        else if (value.sessionEstablished === true) {
          assert.deepEqual(keys, ['sessionEstablished', 'transport']);
          assert.equal(value.transport, 'none'); assert.equal(established, false); established = true;
        } else if (value.clientToolsNegotiated === true) {
          // 协商事实只在建会后、任何输入前生效一次；缺省即设备未装配真实主题端口，验收不能继续。
          assert.deepEqual(keys, ['clientToolsNegotiated']);
          assert.equal(established, true); assert.equal(negotiated, false); assert.equal(sentRound, 0);
          negotiated = true;
        } else if (value.inputAccepted === true) {
          assert.deepEqual(keys, ['inputAccepted', 'round']);
          assert.equal(value.round, sentRound); assert.equal(value.round, acceptedRound + 1);
          assert.equal(completedRound, acceptedRound); acceptedRound = value.round;
        } else if (value.clientToolStarted === true) {
          // 设备只在真实接纳Execute并请求本地主题驱动时上报；主题必须与当轮目标一致且每轮至多一次。
          assert.deepEqual(keys, ['clientToolStarted', 'round', 'theme']);
          assert.equal(value.round, acceptedRound);
          assert.equal(value.theme, acceptedRound === 3 ? cancelRound.theme : rounds[acceptedRound - 1].theme);
          assert.equal(startedRounds.has(value.round), false); startedRounds.add(value.round);
          assert.equal(cancelSettled, false);
        } else if (value.clientToolSubmitted === true) {
          // 原ACK回执必须水位连续；200只接纳accepted/unknown，非200只能是4xx/5xx且续答未知。
          assert.deepEqual(keys, ['clientToolSubmitted', 'code', 'continuation', 'receiptIndex']);
          assert.equal(value.receiptIndex, lastReceiptIndex + 1); lastReceiptIndex = value.receiptIndex;
          assert.equal(receipts.length, value.receiptIndex - 1);
          if (value.code === 200)
            assert.ok(value.continuation === 'accepted' || value.continuation === 'unknown');
          else {
            assert.ok(Number.isInteger(value.code) && value.code >= 400 && value.code <= 599);
            assert.equal(value.continuation, 'unknown');
          }
          receipts.push(value);
        } else if (value.replyCompleted === true) {
          assert.deepEqual(keys, ['replyCompleted', 'round']);
          assert.equal(value.round, acceptedRound); assert.equal(value.round, completedRound + 1);
          // 取消生效后任何新完成轮次都是复活；明确续答与完成轮次的先后在主流程按轮核验。
          assert.equal(cancelSettled, false);
          completedRound = value.round;
        } else if (value.cancelRequested === true) {
          assert.deepEqual(keys, ['cancelRequested']);
          assert.equal(cancelRequested, 0); assert.equal(cancelled, false); cancelRequested = 1;
        } else if (value.cancelAcknowledged === true) {
          // SIG取消应答与respDone取消终态可并存；不单独计轮，也不能替代replyCancelled。
          assert.deepEqual(keys, ['cancelAcknowledged']);
        } else if (value.replyCancelled === true) {
          assert.deepEqual(keys, ['replyCancelled']);
          assert.equal(cancelled, false); cancelled = true; cancelSettled = true;
        } else if (value.sessionEnded === true) {
          assert.deepEqual(keys, ['completedRounds', 'sessionEnded']);
          assert.equal(quitting, true); assert.equal(value.completedRounds, 2);
          assert.equal(completedRound, 2); assert.equal(cancelled, true);
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
      assert.ok(!failure && !closed && performance.now() < deadline, 'tool_live_observation_failed');
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.equal(failure, false, 'tool_live_device_failed');
  };
  // write以原stdin交用户输入，不直接投递MQTT或调用平台模型API。
  const write = value => new Promise((resolve, reject) => {
    child.stdin.write(value + '\n', error => error ? reject(Error('tool_live_stdin_failed')) : resolve());
  });
  const roundsResult = [];
  let passed = false, phase = 'sessionEstablishment', failureReport = null;
  try {
    // 资源宿主可只观察正式spawn的原子进程；这不是替换spawn，也不改变会话、工具或媒体行为。
    if (deps.onSpawn) {
      phase = 'processObservation';
      await deps.onSpawn(child);
      phase = 'sessionEstablishment';
    }
    await wait(() => facts.some(value => value.sessionEstablished === true && value.transport === 'none'));
    await wait(() => facts.some(value => value.clientToolsNegotiated === true));
    for (const [index, item] of rounds.entries()) {
      const round = index + 1, previousLength = text.length;
      firstTextAt = null; submittedAt = performance.now();
      sentRound = round;
      phase = 'inputAcceptance';
      await write(item.input);
      await wait(() => facts.some(value => value.inputAccepted === true && value.round === round));
      phase = 'toolExecution';
      await wait(() => facts.some(value =>
        value.clientToolStarted === true && value.round === round && value.theme === item.theme));
      phase = 'toolSubmission';
      await wait(() => receipts.length >= round);
      // 明确续答(code=200且continuation=accepted)是完成轮次的前提；unknown或被拒提交不能续答。
      assert.ok(receipts[index].code === 200 && receipts[index].continuation === 'accepted',
        'tool_submit_not_accepted');
      phase = 'replyEffect';
      await wait(() => text.slice(previousLength).includes(item.keyword));
      phase = 'replyCompletion';
      await wait(() => facts.some(value => value.replyCompleted === true && value.round === round));
      assert.ok(firstTextAt !== null && firstTextAt >= submittedAt, 'tool_live_first_text_missing');
      roundsResult.push({round, theme: item.theme, keywordMatched: true,
        submittedCode: receipts[index].code, continuation: receipts[index].continuation,
        firstTextMs: Math.round(firstTextAt - submittedAt),
        completedMs: Math.round(performance.now() - submittedAt)});
    }
    // 取消轮：Execute接纳后立即取消；respCancel后迟到提交按账本终态拒绝，回复不得复活。
    sentRound = 3;
    phase = 'cancelArm';
    await write(cancelRound.input);
    await wait(() => facts.some(value => value.inputAccepted === true && value.round === 3));
    await wait(() => facts.some(value =>
      value.clientToolStarted === true && value.round === 3 && value.theme === cancelRound.theme));
    phase = 'cancelRequest';
    await write('/cancel');
    await wait(() => facts.some(value => value.cancelRequested === true));
    phase = 'cancelSettlement';
    await wait(() => facts.some(value => value.replyCancelled === true));
    // 结局分类：409拒收为强判据；提交先被受理也必须取消生效；设备未发出提交则抑制；三者都不得复活。
    const lateReceipt = receipts.length >= 3 ? receipts[2] : null;
    const cancelOutcome = !lateReceipt ? 'submitSuppressed'
      : lateReceipt.code === 409 ? 'expiredReject' : 'cancelAfterAccepted';
    phase = 'sessionClose'; quitting = true; await write('/quit');
    await wait(() => facts.some(value => value.sessionEnded === true && value.completedRounds === 2));
    assert.equal(await waitClosed(5000), true, 'tool_live_exit_timeout');
    assert.equal(exitCode, 0, 'tool_live_exit_failed');
    assert.equal(stdout.length, 0, 'tool_live_partial_diagnostic');
    passed = true;
    return {transport: 'none', scenario: 'noneClientTool', completedRounds: 2, cancelledRounds: 1,
      rounds: roundsResult, cancelOutcome,
      acceptedContinuations: receipts.filter(value => value.code === 200 && value.continuation === 'accepted').length,
      expiredReceipts: receipts.filter(value => value.code === 409).length,
      ownedProcessReleased: closed,
      evidence: deps.spawn ? 'offline-control-fixture' : 'cpp-backend-model', hardwareVerified: false};
  } catch {
    // 失败同样保留实际阶段/数量/取消结局/释放事实；不投射CLI异常、配置、关键词或模型正文。
    const error = new Error('none_client_tool_live_failed');
    failureReport = error.facts = {failed: true, scenario: 'noneClientTool', phase,
      completedRounds: roundsResult.length, receipts: receipts.length,
      cancelOutcome: cancelled ? 'settled' : 'pending', ownedProcessReleased: false,
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
      const error = new Error('tool_live_owned_process_not_released');
      error.facts = failureReport || {failed: true, scenario: 'noneClientTool', phase: 'resourceRelease',
        completedRounds: roundsResult.length, receipts: receipts.length,
        cancelOutcome: cancelled ? 'settled' : 'pending', ownedProcessReleased: false,
        evidence: deps.spawn ? 'offline-control-fixture' : 'cpp-backend-model'};
      error.facts.cleanupFailed = true;
      throw error;
    }
    if (!passed) text = ''; // 对话不写入失败报告或自由异常。
  }
}
module.exports = {rounds, cancelRound, run};
if (require.main === module) run().then(value => console.log(JSON.stringify(value))).catch(error => {
  console.error(JSON.stringify(error.facts || {failed: true, scenario: 'noneClientTool', phase: 'configuration'})); process.exitCode = 1;
});
