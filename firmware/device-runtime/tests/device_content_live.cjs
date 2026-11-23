// 文件说明：显式收费的C++设备附件端到端入口；preflight只读本地配置，live才启动本任务透明观察端口和正式device_text。
// 正常业务仍由平台鉴权、真实私有S3、活会话和实际模型执行；离线依赖注入结果始终标offline-control-fixture。
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {createHash} = require('node:crypto');
const {spawn} = require('node:child_process');
const {StringDecoder} = require('node:string_decoder');
const {performance} = require('node:perf_hooks');
const {ContentLedger, startObservationPorts} = require('./device_content_observation.cjs');

// privateBytes仅消费本用户600普通文件，配置、证书和语料不能由软链/公开目录文件冒充私有输入。
function privateBytes(file, limit) {
  assert.ok(path.isAbsolute(file)); const stat = fs.lstatSync(file);
  assert.ok(stat.isFile() && !stat.isSymbolicLink() && (stat.mode & 0o777) === 0o600);
  assert.ok(stat.uid === process.getuid() && stat.size > 0 && stat.size <= limit);
  return fs.readFileSync(file);
}
// fileHash以固定缓冲检查原产物，不将大二进制复制到日志或内存。
function fileHash(file) {
  assert.ok(path.isAbsolute(file)); const stat = fs.lstatSync(file);
  assert.ok(stat.isFile() && !stat.isSymbolicLink() && stat.uid === process.getuid() && stat.size > 0 && stat.size <= 536870912);
  const digest = createHash('sha256'), block = Buffer.alloc(65536), fd = fs.openSync(file, 'r');
  let total = 0;
  try { let count; while ((count = fs.readSync(fd, block, 0, block.length, null)) > 0) {
    total += count; assert.ok(total <= stat.size); digest.update(block.subarray(0, count));
  } assert.equal(total, stat.size); return digest.digest('hex'); } finally { fs.closeSync(fd); }
}
// validateConfig只接受任务回环origin及两个固定附件；全双工、设备硬件、API配置或模型能力不会由此自动授予。
function validateConfig(config, checkFiles = true) {
  assert.ok(config && !Array.isArray(config));
  assert.deepEqual(Object.keys(config).sort(), ['apiHost', 'apiPort', 'binary', 'binarySha256', 'fixtures',
    'mqttHost', 'mqttPort', 's3Host', 's3Port', 'tlsCa', 'tlsCert', 'tlsKey', 'tlsPort'].sort());
  for (const key of ['apiHost', 's3Host', 'mqttHost']) assert.equal(config[key], '127.0.0.1');
  for (const key of ['apiPort', 's3Port', 'mqttPort', 'tlsPort'])
    assert.ok(Number.isInteger(config[key]) && config[key] > 0 && config[key] <= 65535);
  assert.ok(new Set(['apiPort', 's3Port', 'mqttPort', 'tlsPort'].map(key => config[key])).size === 4);
  assert.ok(path.isAbsolute(config.binary)); assert.match(config.binarySha256, /^[a-f0-9]{64}$/);
  for (const key of ['tlsKey', 'tlsCert', 'tlsCa']) {
    assert.ok(path.isAbsolute(config[key])); if (checkFiles) privateBytes(config[key], 65536);
  }
  assert.ok(Array.isArray(config.fixtures) && config.fixtures.length === 2);
  assert.deepEqual(config.fixtures.map(value => value.type).sort(), ['file', 'image']);
  const fixtures = config.fixtures.map(item => {
    assert.deepEqual(Object.keys(item).sort(), ['keywords', 'path', 'sha256', 'type']);
    assert.ok(path.isAbsolute(item.path) && !/[\r\n\0]/.test(item.path));
    assert.match(item.sha256, /^[a-f0-9]{64}$/);
    assert.ok(Array.isArray(item.keywords) && item.keywords.length > 0 && item.keywords.length <= 4);
    assert.ok(item.keywords.every(word => typeof word === 'string' && word.trim().length > 0 && word.length <= 64 && !/[\r\n\0]/.test(word)));
    if (checkFiles) {
      const bytes = privateBytes(item.path, 2097152);
      assert.equal(createHash('sha256').update(bytes).digest('hex'), item.sha256);
      return {...item, size: bytes.length};
    }
    return {...item, size: 32}; // 仅明确离线控制夹具使用，真实入口必核实际原件。
  });
  if (checkFiles) { assert.equal(fileHash(config.binary), config.binarySha256); assert.ok(fs.lstatSync(config.binary).mode & 0o111); }
  return {...config, fixtures, origin: 'https://127.0.0.1:' + config.tlsPort};
}
// preflight不创建端口、连接设备、调用API或模型；缺最终C++SHA/真实语料等必须明确失败。
function preflight(env = process.env) {
  assert.equal(env.UR_DEVICE_CONTENT_PREFLIGHT, '1');
  assert.ok(!env.UR_MQTT_CA, 'tls_mqtt_observation_not_available');
  validateConfig(JSON.parse(privateBytes(env.UR_DEVICE_CONTENT_LIVE_CONFIG || '', 16384)));
  return {configurationChecked: true, portsStarted: false, platformCalled: false, modelCalled: false,
    actualModelSelectionVerified: false, hardwareVerified: false};
}

// run先真实选中image/file，再逐轮核三阶段上传和typed消费；deps仅供离线回归，不从环境接受假spawn或假模型。
// displayKeywordsMatch 只忽略现行显示清理产生的ASCII空白；字符、标点、大小写和顺序仍须精确。
function displayKeywordsMatch(text, keywords) {
  const displayed = text.replace(/[\t\r\n ]/g, '');
  return keywords.every(word => displayed.includes(word.replace(/[\t\r\n ]/g, '')));
}

// run 仅认证本轮原回复字符，显示空白归一不替代上传、原响应关联或completed判据。
async function run(env = process.env, deps = {}) {
  assert.equal(env.UR_DEVICE_CONTENT_MODEL_LIVE, '1', 'explicit_real_model_cost_opt_in_required');
  assert.ok(!env.UR_MQTT_CA, 'tls_mqtt_observation_not_available');
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET'])
    assert.ok(typeof env[key] === 'string' && env[key].length > 0 && env[key].length <= 4096 && !/[\r\n\0]/.test(env[key]));
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME']) assert.match(env[key], /^[A-Za-z0-9_.-]{1,128}$/);
  const offline = !!(deps.spawn || deps.startPorts || deps.config);
  const config = validateConfig(deps.config || JSON.parse(privateBytes(env.UR_DEVICE_CONTENT_LIVE_CONFIG || '', 16384)), !offline);
  const ledger = new ContentLedger(env.UR_DEVICE_PRODUCT + '/' + env.UR_DEVICE_NAME, config.origin);
  let ports, child, closed = false, exitCode = null, released = false, phase = 'observationStartup';
  let failure = false, stdout = '', text = '', textBytes = 0, established = false, accepted = 0, completed = 0, rawStdout = '', badFactLine = '';
  let submitted = 0, quitting = false, ended = false, submittedAt = null;
  const decoder = new StringDecoder('utf8'), rounds = [];
  const deadline = performance.now() + (deps.timeoutMs || 180000);
  let exited;
  // waitClosed等待原child close事实，kill受理或计时器结束不是资源释放证明。
  const waitClosed = async timeout => {
    let timer;
    try { return await Promise.race([exited.then(() => true), new Promise(resolve => { timer = setTimeout(() => resolve(false), timeout); })]); }
    finally { clearTimeout(timer); }
  };
  // wait不自动重连、重发上传、重试模型或延长授权；失败样本的原阶段被保留。
  const wait = async predicate => {
    while (!predicate()) {
      assert.ok(!failure && !ledger.failed && !closed && performance.now() < deadline);
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.ok(!failure && !ledger.failed);
  };
  const write = value => new Promise((resolve, reject) => child.stdin.write(value + '\n', error => error ? reject(Error('content_stdin_failed')) : resolve()));
  let failureReport;
  try {
    ports = await (deps.startPorts || startObservationPorts)(config, ledger);
    phase = 'processStartup';
    child = (deps.spawn || spawn)(config.binary, ['--tui'], {env: {...env,
      UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: String(ports.mqttPort), UR_DEVICE_CONTENT_ORIGIN: config.origin,
      UR_DEVICE_CONTENT_CA: config.tlsCa}, stdio: ['pipe', 'pipe', 'pipe']});
    exited = new Promise(resolve => {
      child.once('error', () => { failure = true; });
      child.once('close', code => { closed = true; exitCode = code; resolve(); });
    });
    for (const stream of [child.stdin, child.stdout, child.stderr]) stream.on('error', () => { failure = true; });
    child.stdout.on('data', bytes => {
      stdout += bytes.toString('utf8');
      if (process.env.UR_DEVICE_CONTENT_DEBUG && stdout.length <= 8192) rawStdout += bytes.toString('utf8'); // 调试：保留原始事实行（含解析失败行）。
      if (stdout.length > 4096) { failure = true; return; }
      let end;
      while ((end = stdout.indexOf('\n')) >= 0) {
        const line = stdout.slice(0, end); stdout = stdout.slice(end + 1);
        try {
          const value = JSON.parse(line), keys = Object.keys(value).sort();
          if (value.sessionEstablished === true) {
            assert.deepEqual(keys, ['sessionEstablished', 'transport']); assert.equal(value.transport, 'none');
            assert.equal(established, false); established = true;
          } else if (value.inputAccepted === true) {
            assert.deepEqual(keys, ['inputAccepted', 'round']); assert.equal(value.round, submitted);
            assert.equal(value.round, accepted + 1); assert.equal(completed, accepted); accepted = value.round;
          } else if (value.replyCompleted === true) {
            assert.deepEqual(keys, ['replyCompleted', 'round']); assert.equal(value.round, accepted);
            assert.equal(value.round, completed + 1); completed = value.round;
          } else if (value.sessionEnded === true) {
            assert.deepEqual(keys, ['completedRounds', 'sessionEnded']); assert.equal(quitting, true);
            assert.equal(value.completedRounds, 2); assert.equal(completed, 2); assert.equal(ended, false); ended = true;
          } else throw Error('content_device_fact_invalid');
        } catch (parseError) {
          if (process.env.UR_DEVICE_CONTENT_DEBUG && line.includes('"error"')) badFactLine = line; // 设备错误事实行原样保留。
          failure = true;
          void parseError;
        }
      }
    });
    child.stderr.on('data', bytes => {
      textBytes += bytes.length;
      if (textBytes > 131072) { failure = true; return; }
      const value = decoder.write(bytes);
      text += value;
    });
    if (deps.onSpawn) await deps.onSpawn(child);
    phase = 'actualSessionSelection'; await wait(() => ledger.established && established);
    // 断网围栏模式（UR_DEVICE_CONTENT_NETFAULT=1）：会话建立后注入TCP RST模拟异常断网，
    // 核平台15秒恢复围栏（租约/恢复截止超时后会话必须ended并投递sessionEnded）与设备终止；
    // 设备产物设计为异常断开后不自动重连（重连必须重新协商），故不验证会话保持而验证确定性终止。
    if (env.UR_DEVICE_CONTENT_NETFAULT === '1') {
      phase = 'networkFaultInjection';
      const {spawnSync} = require('node:child_process');
      assert.ok(Number.isInteger(ports.mqttPort) && ports.mqttPort > 0);
      const kill = spawnSync('ss', ['-K', 'dst', '127.0.0.1', 'dport', '=', String(ports.mqttPort)], {encoding: 'utf8', timeout: 10000});
      if (kill.status !== 0) throw Error('content_netfault_inject_failed');
      const faultAt = performance.now();
      phase = 'deviceDisconnectedObservation';
      await waitClosed(20000); // 设备异常断开后按产物设计终止；进程不退出则此等待超时后由下方DB围栏兜底。
      phase = 'platformRecoveryFence';
      const pgContainer = env.UR_DEVICE_CONTENT_PG_CONTAINER;
      assert.ok(typeof pgContainer === 'string' && pgContainer.length > 0, 'content_netfault_pg_container_required');
      assert.ok(ledger.session && ledger.session.length <= 128);
      const sessionRow = () => {
        const probe = spawnSync('docker', ['exec', pgContainer, 'psql', '-U', 'postgres', '-d', 'postgres', '-t', '-A',
          '-c', `SELECT state, coalesce(ended_reason,'') FROM im_signaling_sessions WHERE id = '${ledger.session}'`], {encoding: 'utf8', timeout: 10000});
        if (probe.status !== 0) return '';
        return probe.stdout.trim();
      };
      const outboxProbe = () => {
        const probe = spawnSync('docker', ['exec', pgContainer, 'psql', '-U', 'postgres', '-d', 'postgres', '-t', '-A',
          '-c', `SELECT count(*) FROM im_signaling_outbox WHERE session_id = '${ledger.session}' AND payload LIKE '%sessionEnded%' AND created_time > to_timestamp(${Math.floor(faultAt / 1000)})`], {encoding: 'utf8', timeout: 10000});
        return probe.status === 0 ? probe.stdout.trim() : '0';
      };
      const fenceDeadline = performance.now() + 60000;
      let platformEnded = false, endedReason = '';
      while (performance.now() < fenceDeadline) {
        const row = sessionRow();
        if (row.startsWith('ended')) { platformEnded = true; endedReason = row.slice(row.indexOf('|') + 1); break; }
        await new Promise(resolve => setTimeout(resolve, 1000));
      }
      assert.ok(platformEnded, 'content_netfault_session_not_ended');
      const outboxCount = outboxProbe();
      assert.notEqual(outboxCount, '0', 'content_netfault_session_ended_not_delivered');
      released = await ports.close(); ports = null; assert.equal(released, true);
      return {scenario: 'deviceContentNetFault', transport: 'none', rounds: [], evidence: offline ? 'offline-control-fixture' : 'cpp-platform-netfault-fence',
        ownedProcessReleased: closed, ownedPortsReleased: released, hardwareVerified: false,
        sessionEndedVerified: platformEnded, endedReason, sessionEndedDelivered: outboxCount !== '0',
        deviceProcessTerminated: closed, netFaultInjectedMs: Math.round(performance.now() - faultAt)};
    }
    for (const [index, item] of config.fixtures.entries()) {
      ledger.begin(item.type, item.size, item.sha256); const previous = text.length;
      submitted = index + 1; submittedAt = performance.now();
      phase = 'prepareSignedPutComplete'; await write('/' + item.type + ' ' + item.path);
      await wait(() => ledger.current.ready);
      phase = 'originalTypedInputAcknowledgement'; await wait(() => accepted === submitted && ledger.current.accepted);
      phase = 'actualModelCompletion'; await wait(() => completed === submitted && ledger.current.done);
      phase = 'actualModelEffect'; await wait(() => displayKeywordsMatch(text.slice(previous), item.keywords));
      assert.ok(ledger.current.firstNetworkTextAt >= ledger.current.inputSentAt && ledger.current.inputSentAt >= submittedAt);
      rounds.push({type: item.type, originalBytes: item.size, byteSha256: item.sha256,
        ...ledger.facts(), keywordMatched: true,
        // 首项包含本地读件/哈希及上传，不命名模型TTFT；第二项仅从设备实际typed PUBLISH观察时刻起算。
        endToEndInputToFirstNetworkTextMs: Math.round(ledger.current.firstNetworkTextAt - submittedAt),
        typedPublishToFirstNetworkTextMs: Math.round(ledger.current.firstNetworkTextAt - ledger.current.inputSentAt),
        uploadToReadyMs: Math.round(ledger.current.readyAt - submittedAt),
        endToEndCompletedMs: Math.round(performance.now() - submittedAt)});
    }
    phase = 'sessionClose'; quitting = true; await write('/quit'); await wait(() => ended);
    assert.equal(await waitClosed(5000), true); assert.equal(exitCode, 0); assert.equal(stdout, '');
    released = await ports.close(); ports = null; assert.equal(released, true);
    return {scenario: 'deviceContentModel', transport: 'none', rounds,
      evidence: offline ? 'offline-control-fixture' : 'cpp-platform-private-s3-model',
      ownedProcessReleased: closed, ownedPortsReleased: released, hardwareVerified: false};
  } catch (liveError) {
    const error = new Error('device_content_live_failed');
    failureReport = error.facts = {failed: true, scenario: 'deviceContentModel', phase, completedRounds: rounds.length,
      ...ledger.facts(), evidence: offline ? 'offline-control-fixture' : 'cpp-platform-private-s3-model',
      ownedProcessReleased: !child || closed, ownedPortsReleased: !ports, hardwareVerified: false};
    if (process.env.UR_DEVICE_CONTENT_DEBUG) { // 调试证据：设备TUI日志尾部与stdout事实行，定位启动配置/会话/上传卡点。
      failureReport.stderrTail = text.slice(-1600);
      failureReport.stdoutTail = (rawStdout || stdout).slice(-400);
      failureReport.badFactLine = badFactLine;
      failureReport.exitCode = exitCode;
    }
    throw error;
  } finally {
    if (child) {
      child.stdin.end();
      if (!closed) { child.kill('SIGTERM'); await waitClosed(2500); }
      if (!closed) { child.kill('SIGKILL'); await waitClosed(5000); }
    }
    if (ports) released = await ports.close();
    text = ''; stdout = '';
    if (failureReport) { failureReport.ownedProcessReleased = !child || closed; failureReport.ownedPortsReleased = !ports || released; }
    if ((child && !closed) || (ports && !released)) {
      const error = new Error('device_content_owned_resource_not_released');
      error.facts = failureReport || {failed: true, scenario: 'deviceContentModel', phase: 'resourceRelease', completedRounds: rounds.length};
      error.facts.cleanupFailed = true; throw error;
    }
  }
}
module.exports = {run, preflight, validateConfig, fileHash, displayKeywordsMatch};
if (require.main === module) {
  Promise.resolve().then(() => process.env.UR_DEVICE_CONTENT_PREFLIGHT === '1' ? preflight() : run())
    .then(result => console.log(JSON.stringify(result))).catch(error => {
      console.error(JSON.stringify(error.facts || {failed: true, scenario: 'deviceContentModel', phase: 'configuration'})); process.exitCode = 1;
    });
}
