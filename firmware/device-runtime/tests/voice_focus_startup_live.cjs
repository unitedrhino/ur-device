// 文件说明：默认C++宿主启动集成；真实WebAudio/回环MQTT，Broker为明确测试夹具，非业务后端E2E。
'use strict';
const assert = require('node:assert/strict');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');
const {randomBytes,randomUUID,createHash} = require('node:crypto');
const {spawn, execFile} = require('node:child_process');
const {promisify} = require('node:util');
const {installStartupObservation} = require('./voice_focus_startup_observation.cjs');
const {installFileStartupObservation} = require('./file_startup_observation.cjs');
const invoke = promisify(execFile);
const root = path.resolve(__dirname, '../../..'); // 产物只保留本任务worktree。
let phase = 'start'; // 固定失败阶段，不记录浏览器异常正文、请求或任何令牌。
let lastDiagnostic = null; // 仅保留已知入口枚举和数量，不能输出原子进程自由正文。
const traceModes=['media','mediaUnknown','live','liveUnknown']; // 原文件和Live启动可显式旁观；其余路径不能借选项开启profiler。

// startupModes 允许精确复现单一已定义启动路径；缺省仍完整验证七条，不接受自由模式或重复样本。
function startupModes(value) {
  const all=['default','focus','unknown','media','mediaUnknown','live','liveUnknown'];
  if(!value)return all;
  const requested=value.split(',');
  assert.ok(requested.length>0&&requested.length<=all.length&&new Set(requested).size===requested.length&&requested.every(mode=>all.includes(mode)));
  return requested;
}

// startupConnectFailureRequested 仅精确专用开关进入同步连接拒绝用例；不改变默认七条启动路径。
function startupConnectFailureRequested(value) {
  if(value===undefined||value==='')return false;
  assert.equal(value,'1','invalid_connect_failure_switch');
  return true;
}

// readStartupConnectFailure 只核原stdout中的闭集首因；配对URL、自由行及凭据不进入报告。
function readStartupConnectFailure(stdout, diagnostic) {
  assert.ok(typeof stdout==='string'&&Buffer.byteLength(stdout,'utf8')<=4096,'connect_failure_stdout_limit');
  const records=stdout.split('\n').filter(line=>line.startsWith('host_failure=')); // 原真实stdout关闭后才检查完整记录数量。
  const {runtimeFailureCategory}=require('./mcp_file_media_live.cjs'); // 默认路径不装配新诊断消费器。
  if(diagnostic!=='1') {
    assert.equal(records.length,0,'disabled_connect_failure_record');
    return {failureRecords:0,runtimeFailure:null};
  }
  assert.equal(records.length,1,'connect_failure_record_count');
  const failure=runtimeFailureCategory(Buffer.from(records[0])); // 复用已冻结重复键/UTF8/层级检查，不另造宽松JSON合同。
  assert.ok(failure,'connect_failure_fact');
  for(const [key,expected] of Object.entries({kind:'runtimeFailureDiagnostic',runtime:'mqttConnect',udp:'none',pcm:'none',observation:1}))
    assert.equal(failure[key],expected,'connect_failure_fact'); // 已经严格闭集解码，逐字段不受VM对象原型影响。
  return {failureRecords:1,runtimeFailure:failure};
}

// startupTraceRequested 严格校验显式trace入口；缺省关闭，仅旁观文件／Live原启动窗口，不扩大媒体验收范围。
function startupTraceRequested(value, diagnostic, modes) {
  if(value===undefined||value==='')return false;
  if(!(value==='1'&&diagnostic==='1'&&Array.isArray(modes)&&modes.length>0&&
    modes.every(mode=>traceModes.includes(mode))))throw startupTraceFailure();
  return true;
}

// startupTraceFailure 返回固定诊断错误；不能将profiler失败冒认为原设备启动断言失败。
function startupTraceFailure() {
  const error=new Error('startup_trace_incomplete'); // 固定错误不携带原CLI正文、地址或文件路径。
  error.code='ERR_STARTUP_TRACE';
  return error;
}

// readDeviceStartupFailure 只接纳正式入口的固定校验失败枚举；自由stderr及凭据永远不进入诊断。
function readDeviceStartupFailure(value) {
  if(typeof value!=='string'||Buffer.byteLength(value,'utf8')>4096)return null;
  const runtime=/^runtime_failed state=([0-9]+) voice=([0-9]+)\s*$/m.exec(value);
  if(runtime){
    const state=Number(runtime[1]),voice=Number(runtime[2]); // 原入口有限状态枚举，不让异常数字成为可信诊断事实。
    return [state,voice].every(item=>Number.isSafeInteger(item)&&item>=0&&item<=64)?{kind:'runtimeFailed',state,voice}:null;
  }
  const validation=/^(invalid_media_transport|invalid_output_focus|invalid_file_catalog|invalid_device_environment|browser_bind_failed)\s*$/m.exec(value);
  return validation?{kind:'startupValidationFailed',reason:validation[1]}:null;
}

// createStartupTrace 在原具名session执行短profiler；options供离线测试注入，不提供自由运行时路径或类别入口。
// 固定PID/mode文件以wx、600独占预留；原native stop写入后校验同inode/大小/权限/计数，原args只留私有文件。
function createStartupTrace(mode, options={}) {
  const requested=options.requested===true; // 未显式准入时无CLI、文件或报告副作用。
  const pid=options.pid===undefined?process.pid:options.pid; // 仅正整数PID决定固定任务文件名。
  if(requested)assert.ok(traceModes.includes(mode)&&Number.isSafeInteger(pid)&&pid>0);
  const privatePath=path.join(options.directory||path.join(root,'.temp/device-runtime'),
    `voice-focus-startup-trace-${pid}-${mode}.json`); // 不将私有路径放入stdout报告。
  const browser=options.browser; // 由sample绑定原session CLI，不连接其它target或browser。
  const budget=options.budget||{bytes:0}; // 同一runner多个Live模式共享64MiB总文件预算。
  const facts={requested,started:false,stopped:false,privateFileVerified:false,complete:false,
    startFailed:false,stopFailed:false,eventsDropped:false,oversized:false,eventCount:null,bytes:null,sha256:null};
  let descriptor=null, original=null, attempted=false, startPromise=null, stopPromise=null, stopping=false;
  // start 只执行一次；先独占私有文件，再启动原native profiler，不添加任何页面帧或心跳。
  function start() {
    if(startPromise)return startPromise;
    if(stopping||!requested)return Promise.resolve(false);
    startPromise=(async()=>{
      try {
        descriptor=fs.openSync(privatePath,'wx+',0o600);
        original=fs.fstatSync(descriptor);
        assert.ok(original.isFile()&&(original.mode&0o777)===0o600);
        attempted=true;
        const result=await browser(['profiler','start','--categories',
          'devtools.timeline,renderer.scheduler,toplevel,cc,viz']);
        assert.equal(result?.started,true);
        facts.started=true;
        return true;
      } catch {facts.startFailed=true;return false;}
    })();
    return startPromise;
  }
  // stop 在原MQTT成功或原失败边界立即调用；幂等隔离所有诊断错误，不覆盖业务异常且不删除失败证据。
  function stop() {
    if(stopPromise)return stopPromise;
    stopping=true;
    stopPromise=(async()=>{
      if(startPromise)await startPromise;
      try {
        if(!requested||!attempted)return false;
        const result=await browser(['profiler','stop',privatePath]);
        assert.ok(result?.path===privatePath&&Number.isSafeInteger(result.eventCount)&&result.eventCount>=0);
        facts.stopped=true;
        facts.eventsDropped=Object.hasOwn(result,'warning');
        const current=fs.lstatSync(privatePath), opened=fs.fstatSync(descriptor); // 保持原wx句柄避免被替换文件蒙过校验。
        assert.ok(current.isFile()&&current.dev===original.dev&&current.ino===original.ino&&
          opened.dev===current.dev&&opened.ino===current.ino&&(current.mode&0o777)===0o600&&
          Number.isSafeInteger(current.size)&&current.size>0);
        facts.bytes=current.size;
        assert.ok(Number.isSafeInteger(budget.bytes)&&budget.bytes>=0);
        budget.bytes+=current.size;
        facts.oversized=budget.bytes>64*1024*1024;
        assert.ok(!facts.oversized); // 超限保留原私有文件，仅拒绝读取/冒报诊断成功。
        const bytes=fs.readFileSync(descriptor); // 限定大小后只在内存校验，不打印任何事件原args。
        assert.equal(bytes.length,current.size);
        const profile=JSON.parse(bytes.toString('utf8'));
        assert.ok(Array.isArray(profile.traceEvents)&&profile.traceEvents.length>0&&
          profile.traceEvents.length===result.eventCount);
        facts.eventCount=profile.traceEvents.length;
        facts.sha256=createHash('sha256').update(bytes).digest('hex');
        facts.privateFileVerified=true;
        facts.complete=facts.started&&!facts.startFailed&&!facts.eventsDropped;
        if(!facts.complete)facts.stopFailed=true;
        return facts.complete;
      } catch {facts.stopFailed=true;return false;}
      finally {
        if(descriptor!==null){try{fs.closeSync(descriptor);}catch{facts.stopFailed=true;facts.complete=false;}descriptor=null;}
      }
    })();
    return stopPromise;
  }
  return {facts,start,stop};
}

// readStartupTiming 结束仅诊断观察并读取固定性能旁证；不切target、iframe、焦点或触碰原呈现预算。
function readStartupTiming() {
  try { globalThis.stopFocusStartupTimingObservation?.(); } catch {}
  let topLevelDocument = null, iframeCount = null, timing = null;
  try { if (globalThis.top && globalThis.self) topLevelDocument = globalThis.top === globalThis.self; } catch {}
  try {
    const count = document.querySelectorAll('iframe').length;
    if (Number.isSafeInteger(count) && count >= 0) iframeCount = Math.min(32, count);
  } catch {}
  try {
    const original = globalThis.focusStartupTimingFacts;
    if (original && typeof original.supported === 'boolean' && typeof original.truncated === 'boolean' &&
        Array.isArray(original.entries)) {
      const entries = original.entries.slice(0, 32).map(value => {
        const {startOffsetMs, durationMs} = value;
        if (!Number.isFinite(startOffsetMs) || startOffsetMs < 0 || !Number.isFinite(durationMs) || durationMs < 0)
          throw Error('invalid_timing_fact');
        return {startOffsetMs, durationMs};
      });
      timing = {supported: original.supported, entries, truncated: original.truncated || original.entries.length > 32};
    }
  } catch {}
  return {topLevelDocument, iframeCount, timing};
}

// wait 观察原进程/事实，未知或过期失败；不得因等待另起设备实例。
async function wait(predicate, timeout = 12000) {
  const deadline = Date.now() + timeout;
  while (!predicate()) {
    assert.ok(Date.now() < deadline, 'startup fact timeout');
    await new Promise(resolve => setTimeout(resolve, 20));
  }
}

// fixture 仅回应MQTT握手/订阅并记录能力；不认证业务身份或发送虚构会话/媒体。
async function fixture() {
  const state = {connections: 0, connectAt: null, capability: null, failed: false};
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', () => { state.failed = true; });
    let buffer = Buffer.alloc(0); // CONNECT含测试签名，只在内存消费，绝不输出或落盘。
    socket.on('data', bytes => {
      try {
        buffer = Buffer.concat([buffer, bytes]);
        assert.ok(buffer.length <= 65536);
        while (buffer.length >= 2) {
          let size = 0, factor = 1, offset = 1;
          do {
            if (offset >= buffer.length) return;
            assert.ok(offset <= 4);
            size += (buffer[offset] & 127) * factor;
            factor *= 128;
          } while (buffer[offset++] & 128);
          if (buffer.length < offset + size) return;
          const header = buffer[0], body = buffer.subarray(offset, offset + size);
          buffer = buffer.subarray(offset + size);
          const type = header >> 4;
          if (type === 1) {
            state.connections++;
            state.connectAt = Date.now();
            socket.write(Buffer.from([0x20, 2, 0, 0]));
          } else if (type === 8) {
            assert.ok(body.length >= 5);
            socket.write(Buffer.from([0x90, 3, body[0], body[1], 1]));
          } else if (type === 3) {
            assert.ok(body.length >= 2);
            const topicSize = body.readUInt16BE(0), qos = (header >> 1) & 3;
            const start = 2 + topicSize + (qos ? 2 : 0);
            assert.ok(start <= body.length);
            const message = JSON.parse(body.subarray(start).toString('utf8'));
            assert.equal(message.method, 'capabilityReport');
            state.capability = message;
            if (qos) socket.write(Buffer.from([0x40, 2, body[2 + topicSize], body[3 + topicSize]]));
          } else if (type === 12) socket.write(Buffer.from([0xd0, 0]));
          else if (type !== 14) throw Error('unexpected MQTT packet');
        }
      } catch { state.failed = true; socket.destroy(); }
    });
  });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  return {state, port: server.address().port,
    close: async () => { for (const socket of sockets) socket.destroy(); await new Promise(resolve => server.close(resolve)); }};
}

// sample 使用生产device_browser与页面；抑制原门控回执验证未知输出绝不开始MQTT；可选trace只旁观原启动窗口。
async function sample(mode, traceRequested=false, traceBudget={bytes:0}, connectFailureDiagnostic=null) {
  const connectFailure=connectFailureDiagnostic!==null; // 仅专用CLI调用传入固定1/off/非法诊断值，原sample默认参数不变。
  const absentCa=path.join(root,'.temp/device-runtime/startup-mqtt-connect-reject.absent.pem'); // 测试专用必须不存在，绝不创建/删除证书。
  if(connectFailure){assert.equal(mode,'default');assert.equal(traceRequested,false);assert.ok(['1','','true'].includes(connectFailureDiagnostic));
    assert.equal(fs.existsSync(absentCa),false,'connect_failure_ca_already_exists');}
  const mediaMode = mode === 'media' || mode === 'mediaUnknown'; // 正式全媒体配置，不创建文件业务会话。
  const liveMode = mode === 'live' || mode === 'liveUnknown'; // 独立真实接收驱动自检，不提供节目或实际平台授权。
  const completeGroup = mediaMode || liveMode; // 同一实际扬声器/显示仲裁，来源权限仍分开。
  const unknown = mode === 'unknown' || mode === 'mediaUnknown' || mode === 'liveUnknown'; // 仅丢原实际回执，不能伪造新关闭。
  phase = `${mode}:fixture`;
  const broker = await fixture();
  const reservation = net.createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const session = `task143-voice-focus-startup-${process.pid}-${mode}`;
  let child, terminal, stdout = '';
  let childClosed=false,stdoutEnded=false; // 只在专用拒绝用例观察原close与EOF，不用exit事件冒认stdout已排空。
  let connectEvidence=null; // 原进程关闭后一次保存固定计数，后续catch/finally不重新采样或覆盖。
  const connectCleanup={childClosed:false,brokerClosed:false,browserClosed:false}; // 返回或失败旁证引用同一清理事实，finally不覆盖首因。
  let deviceFailure = null; // 原stderr只投影固定runtime失败行，不落身份、地址或配对凭据。
  // browser 只返回固定测试事实，配对令牌及MQTT签名不出现在日志。
  async function browser(args) {
    const output = await invoke('agent-browser', ['--session', session, '--json', ...args], {
      cwd: root, timeout: 25000, maxBuffer: 1048576,
      env: {...process.env, AGENT_BROWSER_ARGS: '--no-sandbox,--use-fake-device-for-media-stream,--use-fake-ui-for-media-stream'}
    });
    const result = JSON.parse(output.stdout);
    assert.equal(result.success, true);
    return result.data;
  }
  const trace=createStartupTrace(mode,{requested:traceRequested,browser,budget:traceBudget}); // 仅显式trace报告固定私有文件校验事实。
  try {
    child = spawn(path.join(root, '.temp/device-runtime/desktop/device_browser'), [String(port)], {
      cwd: root, stdio: ['ignore', 'pipe', 'pipe'], env: {...process.env,
        UR_MEDIA_TRANSPORT: 'udp', UR_OUTPUT_FOCUS: mode === 'default' ? '' : liveMode ? 'live' : mediaMode ? 'media' : 'voice',
        UR_FILE_CATALOG: mediaMode ? JSON.stringify({entries:[{contentRef:'startup-music',
          url:'https://media.example.com/music.wav',origin:'https://media.example.com',mime:'audio/wav',
          audio:true,expiresAt:Date.now()+60000,maxBytes:1024}]}) : '',
        UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: String(broker.port), UR_MQTT_CA: '',
        UR_DEVICE_PRODUCT: 'startup_fixture', UR_DEVICE_NAME: 'cpp_fixture',
        UR_DEVICE_SECRET: randomBytes(32).toString('base64'),
        ...(connectFailure?{UR_MQTT_CA:absentCa,UR_DEVICE_MESSAGE_DIAGNOSTICS:connectFailureDiagnostic}:{})}
    });
    child.on('error', () => { terminal = {failed: true}; });
    child.on('exit', code => { terminal = {code}; });
    if(connectFailure){child.on('close',()=>{childClosed=true;});child.stdout.on('end',()=>{stdoutEnded=true;});}
    child.stdout.on('data', bytes => { if (stdout.length < 4096) stdout += bytes.toString(); });
    child.stderr.on('data', bytes => {
      const failure=readDeviceStartupFailure(bytes.toString());
      if(failure)deviceFailure=failure;
    });
    await wait(() => stdout.includes(`http://127.0.0.1:${port}`) || terminal);
    assert.ok(!terminal);
    phase = `${mode}:open`;
    await browser(['open', `http://127.0.0.1:${port}`]);
    await browser(['snapshot', '-i']);
    phase = `${mode}:observe`;
    await browser(['eval', '-b', Buffer.from(`globalThis.focusStartupFacts=[]; const originalSend=send;
      send=function(value){if(value.kind==='outputGated'||value.kind==='fileLaneGated'||value.kind==='liveLaneGated'){focusStartupFacts.push({kind:value.kind,lane:value.lane,ticket:value.ticket||value.gateTicket,enabled:value.enabled,at:Date.now()});
        ${mode === 'unknown' ? 'return true;' : mode === 'mediaUnknown' || mode === 'liveUnknown' ? "if(value.lane==='audioVideo')return true;" : ''}}return originalSend(value);}; true`).toString('base64')]);
    // 文件旁证保留原Promise/rAF句柄/取消，仅区分实际两帧、1500ms无帧或原关闭级联，不伪成功。
    if(mediaMode&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')await browser(['eval','-b',
      Buffer.from(`(${installFileStartupObservation.toString()})(globalThis);true`).toString('base64')]);
    // 原返回值/Promise/消息不变；最多96条固定阶段与实际Worklet收件旁证，不延长原2秒预算。
    if(liveMode&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')await browser(['eval','-b',
      Buffer.from(`(${installStartupObservation.toString()})(globalThis,()=>desktopLive);true`).toString('base64')]);
    if(traceRequested&&!(await trace.start()))throw startupTraceFailure();
    try {
      phase = `${mode}:enable`;
      await browser(['click', '#activate']);
      if(connectFailure){
        await wait(()=>childClosed&&stdoutEnded);
        connectEvidence={exitCode:terminal?.code??null,connections:broker.state.connections,childClosed,stdoutEnded,
          failureRecords:Buffer.byteLength(stdout,'utf8')<=4096?Math.min(2,stdout.split('\n').filter(line=>line.startsWith('host_failure=')).length):null};
        assert.equal(terminal?.code,1,'connect_failure_native_exit');
        assert.equal(broker.state.connections,0,'connect_failure_broker_connected');
        assert.equal(broker.state.capability,null,'connect_failure_capability_reported');
        return {mode:'mqttConnectRejected',diagnostic:connectFailureDiagnostic==='1'?'on':connectFailureDiagnostic===''?'off':'invalid',
          exitCode:terminal.code,connections:broker.state.connections,childClosed,stdoutEnded,
          ...readStartupConnectFailure(stdout,connectFailureDiagnostic),cleanup:connectCleanup};
      }
      phase = `${mode}:mqtt`;
      if (unknown) {
        await wait(() => terminal);
        assert.equal(terminal.code, 1);
        assert.equal(broker.state.connections, 0);
        assert.equal(broker.state.capability, null);
      } else {
        await wait(() => broker.state.capability || terminal || broker.state.failed);
        assert.ok(broker.state.capability && !broker.state.failed && !terminal);
      }
    } finally {
      if(traceRequested)await trace.stop(); // 原启动结果一确定就停止，不让后续格式/transform复核进入短trace。
    }
    if(traceRequested&&!trace.facts.complete)throw startupTraceFailure();
    phase = `${mode}:facts`;
    const observed = await browser(['eval', '-b', Buffer.from('JSON.parse(JSON.stringify(focusStartupFacts))').toString('base64')]);
    const facts = observed.result;
    if (mode === 'default') {
      assert.deepEqual(facts, []);
      assert.equal(broker.state.capability.params.controlCapabilities, undefined);
    } else {
      assert.equal(facts.length, completeGroup ? 4 : 1);
      for (const fact of facts) { assert.equal(fact.enabled,false); assert.equal(fact.ticket,'1'); }
      if (mediaMode) assert.deepEqual(facts.filter(value=>value.kind==='fileLaneGated').map(value=>value.lane).sort(),['audio','audioVideo','video']);
      if (liveMode) assert.deepEqual(facts.filter(value=>value.kind==='liveLaneGated').map(value=>value.lane).sort(),['audio','audioVideo','video']);
      if (!unknown) {
        assert.ok(broker.state.connectAt >= Math.max(...facts.map(value=>value.at)));
        const resources = completeGroup ? ['speaker','display'] : ['speaker'];
        assert.deepEqual(broker.state.capability.params.controlCapabilities, {
          operations: ['focusControl'], focusResources: resources,
          baseline: {state: 'idle', resources, revision: 0}});
        assert.equal(broker.state.capability.params.purposes.includes('mediaPlayback'), mediaMode);
        assert.equal(broker.state.capability.params.purposes.includes('liveBroadcast'),liveMode);
        if (liveMode) {
          assert.equal(broker.state.capability.params.audio.sampleRate,16000);
          assert.deepEqual(broker.state.capability.params.liveReceive,{audio:{format:'opus',sampleRate:48000,channels:1,frameDuration:20},videoCodecs:['H264']});
        }
      }
    }
    const startupObservation=completeGroup&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1'?
      (await browser(['eval','-b',Buffer.from(`JSON.parse(JSON.stringify(${liveMode?'focusStartupObservation':'fileStartupObservation'}))`).toString('base64')])).result:null;
    const startupTiming=completeGroup&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1'?
      (await browser(['eval','-b',Buffer.from(`(${readStartupTiming.toString()})()`).toString('base64')])).result:null;
    return {mode, connections: broker.state.connections, actualGateFacts: facts.length,
      baselineReported: !unknown && mode !== 'default', unknownRejected: unknown,
      ...(startupObservation?{startupObservation}:{}), ...(startupTiming?{startupTiming}:{}),
      ...(traceRequested?{startupTrace:trace.facts}:{})}; // 成功也留原阶段旁证，不能因未失败而丢掉时间差。
  } catch(error) {
    if(traceRequested)await trace.stop(); // 包括start返回不确定的异常路径；幂等且不吞原业务异常。
    let gated = null, laneFacts = null, liveDriver = null, liveGateFacts = null, startupTiming = null;
    try {gated=(await browser(['eval','-b',Buffer.from('focusStartupFacts.length').toString('base64')])).result;}catch{}
    if(mediaMode&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')try{laneFacts=(await browser(['eval','-b',Buffer.from('JSON.parse(JSON.stringify(fileStartupObservation))').toString('base64')])).result;}catch{}
    if(liveMode&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')try{liveGateFacts=(await browser(['eval','-b',Buffer.from(`({events:focusStartupObservation,context:desktopLive.context?.state,closing:desktopLive.closing,lanes:[...desktopLive.lanes.values()].map(value=>({lane:value.name,pending:value.pending,ready:value.ready,enabled:value.enabled,nodeClosed:value.nodeClosed}))})`).toString('base64')])).result;}catch{}
    if(completeGroup&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')try{startupTiming=(await browser(['eval','-b',Buffer.from(`(${readStartupTiming.toString()})()`).toString('base64')])).result;}catch{}
    // 失败后独立复核固定格式和接收pipeline；不发布能力、不触碰原节目或改变断言。
    if(liveMode)try{liveDriver=(await browser(['eval','-b',Buffer.from(`(async()=>({format:await probeLiveReceive(),rtcH264:RTCRtpReceiver.getCapabilities('video')?.codecs?.some(value=>value.mimeType.toLowerCase()==='video/h264')===true,videoCallback:typeof HTMLVideoElement.prototype.requestVideoFrameCallback==='function',videoTransform:await desktopLive.probeTransform('video')}))()`).toString('base64')])).result;}catch{}
    const receive=broker.state.capability?.params?.liveReceive;
    // 仅输出公开格式数值与固定能力布尔；未知或自由字段不投影到日志。
    const liveFormat=liveMode?{present:!!receive,audioPresent:!!receive?.audio,
      opus:receive?.audio?.format==='opus',sampleRate:Number.isInteger(receive?.audio?.sampleRate)?receive.audio.sampleRate:null,
      channels:Number.isInteger(receive?.audio?.channels)?receive.audio.channels:null,
      frameDuration:Number.isInteger(receive?.audio?.frameDuration)?receive.audio.frameDuration:null,
      h264Only:Array.isArray(receive?.videoCodecs)&&receive.videoCodecs.length===1&&receive.videoCodecs[0]==='H264'}:null;
    lastDiagnostic={mode,phase,deviceFailure,connections:broker.state.connections,
      fixtureFailed:broker.state.failed,capabilitySeen:!!broker.state.capability,gateFacts:gated,
      childTerminal:terminal||null,laneFacts,liveFormat,liveDriver,liveGateFacts,startupTiming,
      ...(connectFailure?{connectFailure:true,connectEvidence,childClosed,stdoutEnded,cleanup:connectCleanup}:{}),
      ...(traceRequested?{startupTrace:trace.facts}:{})};
    throw error;
  } finally {
    if(traceRequested)await trace.stop(); // 清理优先：diagnostic stop失败也继续原observer/child/session释放。
    // 诊断未读或读取失败仍尽力断开本页面观察；随后原具名browser.close释放页面，不触碰其它会话。
    if(liveMode&&process.env.UR_FOCUS_STARTUP_DIAGNOSTIC==='1')await browser(['eval','-b',
      Buffer.from('try{globalThis.stopFocusStartupTimingObservation?.();}catch{}true').toString('base64')]).catch(()=>{});
    if (child && !terminal) {
      child.kill('SIGTERM');
      const timer = setTimeout(() => { if (!terminal) child.kill('SIGKILL'); }, 5000);
      await wait(() => terminal, 7000).finally(() => clearTimeout(timer));
    }
    if(connectFailure){
      connectCleanup.childClosed=childClosed;
      try{await broker.close();connectCleanup.brokerClosed=true;}finally{
        await browser(['close']);connectCleanup.browserClosed=true;
      }
    }else{
      await broker.close();
      await browser(['close']);
    }
  }
}

// main 三条实际宿主路径均须通过，报告不包含任何本地身份/配对/原始报文。
async function main() {
  if(startupConnectFailureRequested(process.env.UR_FOCUS_STARTUP_MQTT_CONNECT_FAILURE)) {
    const samples=[]; // 固定三种精确诊断开关，均执行原配对/ready/activate及同步TLS拒绝，不运行模型。
    for(const diagnostic of ['1','','true'])samples.push(await sample('default',false,{bytes:0},diagnostic));
    const report={scope:'cpp-host-mqtt-connect-rejected-real-startup-only',samples};
    fs.writeFileSync(path.join(root,'.temp/device-runtime',`mqtt-connect-startup-report-${randomUUID()}.json`),JSON.stringify(report,null,2)+'\n',{flag:'wx',mode:0o600});
    console.log(JSON.stringify(report));
    return;
  }
  const samples = [];
  const modes=startupModes(process.env.UR_FOCUS_STARTUP_MODES); // 所有入口先校验，再读设备或创建实际资源。
  const traceRequested=startupTraceRequested(process.env.UR_FOCUS_STARTUP_TRACE,process.env.UR_FOCUS_STARTUP_DIAGNOSTIC,modes);
  const traceBudget={bytes:0}; // 文件／Live全部显式短trace共用64MiB，不能按新mode重置预算。
  const provenance={deviceSha256:createHash('sha256').update(fs.readFileSync(path.join(root,'.temp/device-runtime/desktop/device_browser'))).digest('hex'),
    runnerSha256:createHash('sha256').update(fs.readFileSync(__filename)).digest('hex')};
  for (const mode of modes) samples.push(await sample(mode,traceRequested,traceBudget));
  const report = {scope: 'default-cpp-host-startup-real-webaudio-test-mqtt-only',provenance,samples};
  fs.writeFileSync(path.join(root, `.temp/device-runtime/voice-host-focus-startup-report-${randomUUID()}.json`), JSON.stringify(report, null, 2) + '\n',{mode:0o600});
  console.log(JSON.stringify(report));
}
if (require.main === module) main().catch(error => {
  console.error(JSON.stringify({failure: error?.code==='ERR_STARTUP_TRACE'?'voiceFocusStartupTraceFailed':'voiceFocusStartupFailed', phase,
    assertion: error?.code === 'ERR_ASSERTION', operator: error?.operator,diagnostic:lastDiagnostic}));
  process.exitCode = 1;
});
// readStartupFailureFacts 仅交回原固定阶段与已脱敏诊断，独立编排不得用自由异常正文替代原失败证据。
function readStartupFailureFacts() { return {phase,diagnostic:lastDiagnostic}; }
module.exports={startupModes,readStartupTiming,startupTraceRequested,createStartupTrace,main,readStartupFailureFacts,readDeviceStartupFailure,
  startupConnectFailureRequested,readStartupConnectFailure}; // 独立显示编排复用原入口与全部断言，不另造成功路径。
