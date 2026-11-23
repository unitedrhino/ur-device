// 文件说明：真实文字模型→MCP→action/actionReply→原回复有声播放的五轮桌面验收；不注入工具或动作回执。
'use strict';
const assert = require('node:assert/strict'), fs = require('node:fs');
const {spawn,execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');
const {parseJSON,waitClosed} = require('./browser_client_tool_live.cjs');
const {endSessionViaUI} = require('./mcp_file_media_live.cjs'); // 共享原END可见性准备，不复制设备结束或协议逻辑。

// volumes固定五轮实际增益变化；不用同值确认替代真实设备动作。
const volumes = Object.freeze([40,65,20,80,40]);
// installActionObserver透明观察原桥的已执行动作和原工具事件，不记录工具参数或模型自由正文。
function installActionObserver() {
  if (globalThis.__urDeviceAction) throw Error('action_observer_exists');
  const observed = globalThis.__urDeviceAction = {sequence:0,overflow:false,actions:[],tools:[],submitted:0,accepted:0};
  const original = incoming;
  incoming = function(value) {
    const result = original(value); // 原处理失败时不制造已接纳的设备事实。
    if (++observed.sequence > 4096) { observed.overflow = true; return result; }
    if (value.kind === 'actionCompleted') {
      if (observed.actions.length >= 32) observed.overflow = true;
      else observed.actions.push({code:value.code,volume:value.volume,sequence:observed.sequence});
    } else if (value.kind === 'toolStatus') {
      if (observed.tools.length >= 64) observed.overflow = true;
      else {
        const name = ['get_thing_model','device_action_send'].find(name => value.name === name || value.name === 'things_iot_mcp.' + name);
        observed.tools.push({id:value.id,response:value.responseId,state:value.state,
          name:value.state === 'started' ? name || 'other' : undefined,sequence:observed.sequence});
      }
    } else if (value.kind === 'textSubmitted' && value.result === 'submitted') ++observed.submitted;
    else if (value.kind === 'textStatus' && value.result === 'accepted') ++observed.accepted;
    return result;
  };
  return true;
}
// actionFacts仅在验收进程内关联原ID；对外报告只输出verifyActionRound的固定布尔量和数值。
function actionFacts() {
  const state = globalThis.__urDeviceAction;
  return {overflow:state.overflow,actions:state.actions.slice(),tools:state.tools.slice(),submitted:state.submitted,
    accepted:state.accepted,completed:metrics.completedResponses,cancelled:metrics.cancelledResponses,
    completedID:completedResponseId,active:responseId !== '',captureOpen:metrics.captureOpen,
    pending:pendingPlayback.size,playableResponse:metrics.lastPlayableResponseId,
    consumedResponse:metrics.lastPlayableConsumedResponseId,voicedFrames:metrics.lastVoicedConsumedResponseFrames};
}
// actionDiagnostic只输出当前原回复的固定计数及关系，不把工具ID、自由错误或模型正文写入报告。
function actionDiagnostic(value) {
  if (!value) return null;
  const count = number => Number.isSafeInteger(number) && number >= 0 && number <= 1000000 ? number : null;
  const boolean = flag => typeof flag === 'boolean' ? flag : null;
  const sameReply = id => typeof value.completedID === 'string' && value.completedID.length > 0 && id === value.completedID;
  return {submitted:count(value.submitted),accepted:count(value.accepted),completed:count(value.completed),cancelled:count(value.cancelled),
    active:boolean(value.active),captureOpen:boolean(value.captureOpen),pending:count(value.pending),voicedFrames:count(value.voicedFrames),
    actions:(Array.isArray(value.actions) ? value.actions : []).slice(-32).map(action => ({
      code:Number.isSafeInteger(action?.code) && action.code >= 100 && action.code <= 599 ? action.code : null,
      volume:Number.isSafeInteger(action?.volume) && action.volume >= 0 && action.volume <= 100 ? action.volume : null,
      sequence:count(action?.sequence)})),
    tools:(Array.isArray(value.tools) ? value.tools : []).slice(-64).map(tool => ({name:['get_thing_model','device_action_send','other'].includes(tool?.name) ? tool.name : null,
      state:['started','succeeded','failed','unknown'].includes(tool?.state) ? tool.state : 'other',sameReply:sameReply(tool?.response),
      sequence:count(tool?.sequence),idPresent:typeof tool?.id === 'string' && tool.id.length > 0})),
    decodedSameReply:sameReply(value.playableResponse),consumedSameReply:sameReply(value.consumedResponse)};
}
// verifyActionRound同时核对实际驱动增益、MCP原回复关联和真实消费，不能用模型确认文字当执行效果。
function verifyActionRound(before, after, volume) {
  assert.ok(volumes.includes(volume), 'invalid_target_volume');
  assert.equal(after.overflow,false,'action_observer_overflow');
  assert.equal(after.submitted,before.submitted + 1,'one_original_text_submit_required');
  assert.equal(after.accepted,before.accepted + 1,'original_text_acceptance_required');
  assert.equal(after.completed,before.completed + 1,'one_original_reply_required');
  assert.ok(typeof after.completedID === 'string' && after.completedID.length > 0 && after.completedID !== before.completedID,'new_original_reply_required');
  assert.equal(after.cancelled,before.cancelled,'unexpected_reply_cancel');
  assert.equal(after.active,false);assert.equal(after.captureOpen,false);assert.equal(after.pending,0);
  const actions = after.actions.slice(before.actions.length), events = after.tools.slice(before.tools.length);
  assert.ok(actions.length > 0 && actions.length <= 4,'actual_device_action_required');
  assert.ok(actions.every(action => action.code === 200),'device_action_failed');
  assert.equal(actions.at(-1).volume,volume,'actual_device_volume_mismatch');
  if (before.actions.length) assert.notEqual(before.actions.at(-1).volume,volume,'actual_volume_change_required');
  const calls = new Map(); let controls = 0, models = 0;
  for (const event of events) {
    assert.equal(event.response,after.completedID,'tool_original_reply_mismatch');
    assert.ok(typeof event.id === 'string' && event.id.length > 0,'tool_id_missing');
    if (event.state === 'started') {
      assert.ok(!calls.has(event.id),'tool_repeated_start');
      assert.ok(['get_thing_model','device_action_send'].includes(event.name),'unexpected_tool');
      calls.set(event.id,{...event,terminal:false}); if (event.name === 'device_action_send') ++controls; else ++models;
    } else {
      assert.equal(event.state,'succeeded','tool_not_successful');
      const start = calls.get(event.id);assert.ok(start && !start.terminal,'orphan_or_repeated_tool_result');
      start.terminal = true;
      // MCP状态与设备实际完成经不同投递路径抵达；首个真实失败已证明动作可先于toolCallStart到达。
      // 本入口独占设备且每轮等待排空，以本轮真实动作数、目标增益及原工具完整关联证明效果，不承诺跨路径同序。
    }
  }
  assert.ok(controls >= 1 && controls <= 4 && models <= 1 && [...calls.values()].every(call => call.terminal),'tool_round_incomplete');
  assert.equal(controls,actions.length,'device_action_and_mcp_count_mismatch');
  assert.equal(after.playableResponse,after.completedID,'original_reply_not_decoded');
  assert.equal(after.consumedResponse,after.completedID,'original_reply_not_consumed');
  assert.ok(Number.isSafeInteger(after.voicedFrames) && after.voicedFrames >= 2,'two_voiced_consumed_frames_required');
  return {volume,passed:true,actualDriverApplied:true,autonomousMcpControl:true,
    originalReplyCompleted:true,originalReplyDecodedAndConsumed:true};
}
// run只走正式C++设备进程和原UI文字输入；媒体选择有声UDP，输入采集保持关闭，不假称语音输入验收。
async function run(env = process.env, deps = {}) {
  assert.equal(env.UR_DEVICE_ACTION_LIVE,'1','explicit_action_live_required');
  assert.equal(env.UR_MEDIA_TRANSPORT,'udp','actual_udp_required');
  assert.ok(env.UR_DEVICE_BROWSER_BIN && fs.lstatSync(env.UR_DEVICE_BROWSER_BIN).isFile(),'device_binary_required');
  const port = Number(env.UR_BROWSER_PORT); assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535,'device_port_required');
  for (const key of ['UR_DEVICE_PRODUCT','UR_DEVICE_NAME','UR_DEVICE_SECRET','UR_MQTT_HOST','UR_MQTT_PORT']) assert.ok(env[key],'device_identity_required');
  assert.equal(env.UR_MQTT_HOST,'127.0.0.1','isolated_local_backend_required');
  const child = spawn(env.UR_DEVICE_BROWSER_BIN,[String(port)],{env,stdio:['ignore','pipe','pipe']});
  const session = 'device-actions-' + randomUUID(), url = 'http://127.0.0.1:' + port + '/';
  let closed = false, code, ready = false, outEnded = false, errEnded = false, streamFailed = false, buffered = '';
  const completion = new Promise(resolve => child.once('close',value => {closed = true;code = value;resolve();}));
  child.once('error',() => {streamFailed = true;});
  child.stdout.on('data',bytes => {
    buffered += bytes.toString('utf8'); if (buffered.length > 8192) {streamFailed = true;buffered = '';return;}
    let end; while ((end = buffered.indexOf('\n')) >= 0) { if (buffered.slice(0,end) === url) ready = true; buffered = buffered.slice(end + 1); }
  });
  child.stdout.once('end',() => {outEnded = true;}); child.stderr.on('data',() => {});child.stderr.once('end',() => {errEnded = true;});
  for (const stream of [child.stdout,child.stderr]) stream.on('error',() => {streamFailed = true;});
  const invoke = (...args) => new Promise((resolve,reject) => execFile('agent-browser',['--session',session,'--headed',...args],
    {env:{...env,AGENT_BROWSER_ARGS:[env.AGENT_BROWSER_ARGS || '--no-sandbox','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream'].join(',')},
      timeout:args[0] === 'wait' ? 60000 : 20000,maxBuffer:262144},
    (error,stdout) => error ? reject(Error('action_browser_failed')) : resolve(stdout.trim())));
  const facts = async () => parseJSON(await invoke('eval','(' + actionFacts.toString() + ')()'));
  let phase = 'deviceStart', successful = false, diagnostic = null; const results = [];
  const timer = setTimeout(() => {streamFailed = true;if (!closed) child.kill('SIGTERM');},360000);
  try {
    if (deps.onSpawn) await deps.onSpawn(child);
    const until = Date.now() + 10000;
    while (!ready) {assert.ok(!closed && !streamFailed && Date.now() < until,'device_start_failed');await new Promise(resolve => setTimeout(resolve,20));}
    phase = 'sessionReady';await invoke('open',url);await invoke('click','#activate');
    await invoke('wait','--fn','!document.getElementById("sendText").disabled');
    await invoke('click','#stop');await invoke('wait','--fn','metrics.captureOpen === false && responseId === ""');
    await invoke('eval','(' + installActionObserver.toString() + ')()');
    for (const volume of volumes) {
      phase = 'volume' + volume;const before = await facts();
      await invoke('fill','#textInput','请实际调用绑定设备的音量设置行为，把音量设置为' + volume + '。请根据实际设备回执用一句话确认，不要仅口头回答。');
      await invoke('check','#textAudio');await invoke('click','#sendText');
      await invoke('wait','--fn','metrics.completedResponses === ' + (before.completed + 1) + ' && responseId === "" && pendingPlayback.size === 0');
      results.push(verifyActionRound(before,await facts(),volume));
    }
    phase = 'sessionClose';await endSessionViaUI(invoke);assert.ok(await waitClosed(completion,10000));assert.equal(code,0);successful = true;
  } catch {successful = false;if (phase.startsWith('volume')) diagnostic = await facts().then(actionDiagnostic,() => null);}
  finally {
    clearTimeout(timer);const browserClosed = await invoke('close').then(() => true,() => false);
    if (!closed) {child.kill('SIGTERM');if (!await waitClosed(completion,8000)) {child.kill('SIGKILL');await waitClosed(completion,8000);}}
    const report = {passed:successful && browserClosed && closed && code === 0 && outEnded && errEnded && !streamFailed,
      evidence:'cpp-backend-model',scenario:'fiveDeviceVolumeActions',phase,results,
      ...(!successful ? {diagnostic} : {}),
      cleanup:{browserClosed,childClosed:closed,stdoutEnded:outEnded,stderrEnded:errEnded}};
    (deps.report || console.log)(JSON.stringify(report));if (!report.passed) throw Error('device_action_live_failed');return report;
  }
}
module.exports = {volumes,installActionObserver,actionFacts,actionDiagnostic,verifyActionRound,run};
if (require.main === module) run().catch(() => {console.error('device_action_live_failed');process.exitCode = 1;});
