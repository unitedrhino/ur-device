// 文件说明：共享C++宿主的单次真实模型→本人Consent→服务人工→私有留存→保持/恢复验收。
// 模块划分：严格私有输入、脱敏判据、实际浏览器观察、原进程身份、一次有界执行；后端/headed由隔离宿主启动。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn,execFile} = require('node:child_process');
const {randomUUID,createHash} = require('node:crypto');
const {StringDecoder} = require('node:string_decoder');
const {performance} = require('node:perf_hooks');
const {childClose} = require('./device_pair_live.cjs');

const root = path.resolve(__dirname,'../../..'); // 原任务worktree，不在源码目录写日志。
const fixedInput = '请简短告诉我一加一等于几。'; // 一次真实文字输入并请求语音，不能冒称ASR验收。
const diagnosticKeys = ['kind','retained','cleared','expired','ackAttempts','ackAccepted'];
// exact/count/identifier 分别限制闭集、有界数量与原UTF8公共身份；不会从字段推导授权。
function exact(value,keys) {return value!==null&&typeof value==='object'&&!Array.isArray(value)&&Object.keys(value).length===keys.length&&keys.every(k=>Object.hasOwn(value,k));}
function count(value,max=4294967295) {return Number.isSafeInteger(value)&&value>=0&&value<=max;}
function identifier(value,max=128) {return typeof value==='string'&&Buffer.byteLength(value)>0&&Buffer.byteLength(value)<=max&&!/[\x00-\x1f\x7f]/.test(value)&&!/[\ud800-\udfff]/u.test(value);}
// numericIdentity 保留HTTP AppID/本人ID的正十进制原字符串；拒绝宽松cast默认0及不可精确表示的大整数。
function numericIdentity(value) {return typeof value==='string'&&/^[1-9][0-9]*$/.test(value)&&Number.isSafeInteger(Number(value));}
// parseContextDiagnostic 只接纳native原闭集，不接身份、摘要或正文；重复键也拒绝。
function parseContextDiagnostic(raw) {
  assert.ok(typeof raw==='string'&&Buffer.byteLength(raw)<=1024,'context_diagnostic_invalid');
  const value=JSON.parse(raw),keys=[...raw.matchAll(/"([^"\\]+)"\s*:/g)].map(v=>v[1]);
  assert.ok(exact(value,diagnosticKeys)&&keys.length===diagnosticKeys.length&&new Set(keys).size===keys.length&&value.kind==='contextDiagnostic','context_diagnostic_invalid');
  for(const key of ['retained','cleared','expired'])assert.equal(typeof value[key],'boolean','context_diagnostic_invalid');
  assert.ok(count(value.ackAttempts,4096)&&count(value.ackAccepted,4096)&&value.ackAccepted<=value.ackAttempts&&!(value.retained&&(value.cleared||value.expired))&&(!value.expired||value.cleared),'context_diagnostic_invalid');
  return {...value};
}
// contextObserver 有界消费原stderr，不保存自由库日志；诊断回退/截断保留unknown，不能补造留存。
function contextObserver() {
  const decoder=new StringDecoder('utf8');let pending='',total=0,latest=null,ended=false,unknown=false;
  const line=text=>{if(!text.includes('"contextDiagnostic"'))return;try{const next=parseContextDiagnostic(text);
    if(latest&&(next.ackAttempts<latest.ackAttempts||next.ackAccepted<latest.ackAccepted||(latest.cleared&&!next.cleared)||(latest.expired&&!next.expired)))throw Error();latest=next;
  }catch{unknown=true;}};
  return {push(bytes){if(ended){unknown=true;return;}total+=bytes.length;if(total>1048576){unknown=true;return;}
    pending+=decoder.write(bytes);if(pending.length>4096&&!pending.includes('\n')){unknown=true;pending='';return;}
    let end;while((end=pending.indexOf('\n'))>=0){const text=pending.slice(0,end).replace(/\r$/,'');pending=pending.slice(end+1);if(Buffer.byteLength(text)>4096)unknown=true;else line(text);}
  },finish(){if(ended)return;ended=true;pending+=decoder.end();if(pending.length)unknown=true;pending='';},facts(){return {...(latest||{}),available:latest!==null,ended,unknown};}};
}
// parseStartupDiagnostic 只接受六个原启动事实，状态标签不等于Broker接收或焦点授权。
function parseStartupDiagnostic(raw) {
  assert.ok(typeof raw==='string'&&Buffer.byteLength(raw)<=1024,'startup_diagnostic_invalid');
  const value=JSON.parse(raw),keys=['kind','browserReady','initialIdleConfirmed','connection','voice','mediaReady','aiOwned'],seen=[...raw.matchAll(/"([^"\\]+)"\s*:/g)].map(v=>v[1]);
  assert.ok(exact(value,keys)&&seen.length===keys.length&&new Set(seen).size===keys.length&&value.kind==='startupDiagnostic','startup_diagnostic_invalid');
  for(const key of ['browserReady','initialIdleConfirmed','mediaReady','aiOwned'])assert.equal(typeof value[key],'boolean','startup_diagnostic_invalid');
  assert.ok(['idle','connecting','subscribing','reporting','ready','failed','closed','unknown'].includes(value.connection)&&['idle','reporting','creating','negotiating','established','ending','failed','unknown'].includes(value.voice),'startup_diagnostic_invalid');return {...value};
}
// startupObserver 有界旁观独立stderr类别，非法/截断锁存unknown并保留最后有效原事实，不保存自由正文。
function startupObserver() {
  const decoder=new StringDecoder('utf8');let pending='',total=0,latest=null,ended=false,unknown=false;
  const line=text=>{if(!text.includes('"startupDiagnostic"'))return;try{latest=parseStartupDiagnostic(text);}catch{unknown=true;}};
  return {push(bytes){if(ended){unknown=true;return;}total+=bytes.length;if(total>1048576){unknown=true;return;}pending+=decoder.write(bytes);
    if(pending.length>4096&&!pending.includes('\n')){unknown=true;pending='';return;}let end;
    while((end=pending.indexOf('\n'))>=0){const text=pending.slice(0,end).replace(/\r$/,'');pending=pending.slice(end+1);if(Buffer.byteLength(text)>4096)unknown=true;else line(text);}
  },finish(){if(ended)return;ended=true;pending+=decoder.end();if(pending.length)unknown=true;pending='';},facts(){return {last:latest?{...latest}:null,unknown};}};
}
// projectFailureCode 不调用异常getter或反射自由message，只保留本runner正式固定失败原因。
function projectFailureCode(error) {
  const value=error&&(typeof error==='object'||typeof error==='function')?Object.getOwnPropertyDescriptor(error,'message')?.value:undefined;
  return ['device_ended_early','browser_command_failed','phase_timeout','run_interrupted','page_activation_terminated','api_http_failed','api_rejected','api_response_limit','ordinary_self_changed','ordinary_self_required','source_unconfirmed','consent_rejected','consent_metadata_changed','receipt_scope_changed','device_exit_failed','device_close_timeout','readiness_facts_invalid','activation_facts_invalid','private_cpp_invalid','private_server_unconfirmed','cleanup_failed','child_spawn_failed','child_identity_unknown','child_identity_changed','browser_response_limit','browser_response_invalid','initial_model_turn_not_empty'].includes(value)?value:'unknown';
}
// captureFailure 在业务异常进入finally之前只读投影原原因；观察回调不接原异常对象。
async function captureFailure(work,observe) {try{return await work();}catch(error){observe(projectFailureCode(error));throw error;}}
// projectStartupObservation 只保存各原进程最后有效诊断，缺流为null，异常流不能补造有效状态。
function projectStartupObservation(values) {
  if(values===undefined)return {caller:null,human:null};assert.ok(Array.isArray(values)&&values.length===2,'startup_observation_invalid');
  const projected=values.map(value=>{if(value===null)return null;assert.ok(exact(value,['last','unknown'])&&typeof value.unknown==='boolean','startup_observation_invalid');const last=value.last===null?null:parseStartupDiagnostic(JSON.stringify(value.last));return last||value.unknown?{last,unknown:value.unknown}:null;});return {caller:projected[0],human:projected[1]};
}
// verifyPrivate 联合原CPP留存/本地ACK及正式Outbox读回；本地publish受理不是应用received。
function verifyPrivate(cpp,receipt) {
  assert.ok(cpp&&cpp.retained===true&&cpp.cleared===false&&cpp.expired===false&&count(cpp.ackAccepted,4096)&&cpp.ackAccepted>0&&cpp.ackAttempts>=cpp.ackAccepted&&!cpp.unknown,'private_cpp_unconfirmed');
  assert.ok(receipt&&receipt.status==='received'&&receipt.sameOriginal===true&&receipt.receiptOriginalInstanceMatched===true&&receipt.consumed===true&&receipt.revoked===false,'private_server_unconfirmed');return true;
}
// privateObservationReady 关联跨通道私有观察；正式received必须最终由原设备留存及ACK事实确认。
function privateObservationReady(cpp,receipt) {
  assert.ok(cpp&&cpp.unknown!==true&&!cpp.cleared&&!cpp.expired&&!cpp.ended,'private_cpp_invalid');
  if(!receipt||receipt.status==='pending')return false;
  assert.ok(receipt.status==='received'&&receipt.sameOriginal===true&&receipt.receiptOriginalInstanceMatched===true&&receipt.consumed===true&&receipt.revoked===false,'private_server_unconfirmed');
  if(cpp.available!==true||cpp.retained!==true||!count(cpp.ackAccepted,4096)||cpp.ackAccepted===0)return false;
  return verifyPrivate(cpp,receipt);
}
// escalateWhenReady 只发送一次本地意图，不能用页面按钮受理冒称平台已经转人工。
async function escalateWhenReady(read,click,until,ports={}) {await waitFor(async()=>{const value=await read();return value?.canEscalate===true;},until,ports);return click();}
// verifyModelRound 仅首个原回复完成并实际连续可播放；历史有声计数不能替代同原response。
function verifyModelRound(before,after) {
  assert.ok(before.completed===0&&before.captureClosed===true&&!before.activeReply,'initial_model_turn_not_empty');
  assert.ok(after.completed===1&&!after.activeReply&&after.nonSilent>=before.nonSilent+2&&identifier(after.completedID,128)&&after.playableID===after.completedID,'model_round_unconfirmed');
  return true;
}
// samePeer 保持/恢复只能沿原腿和原SDP，不能通过重建Peer或新会话凑媒体增长。
function samePeer(original,current) {assert.ok(identifier(original.session,64)&&identifier(original.peerEpoch)&&/^[a-f0-9]{64}$/.test(original.peerFingerprint),'media_scope_invalid');
  for(const key of ['session','peerEpoch','peerFingerprint'])assert.equal(current[key],original[key],'original_peer_changed');assert.equal(current.connected,true,'media_not_connected');}
// validMedia 固定实际PCM和Sender/输出门控快照，不以平台established替代媒体。
function validMedia(value) {assert.ok(value&&count(value.frames)&&count(value.nonSilentFrames)&&value.nonSilentFrames<=value.frames,'media_counter_invalid');for(const k of ['inputAttached','outputEnabled','held','connected'])assert.equal(typeof value[k],'boolean','media_fact_invalid');}
// verifyAudioPair 两端都须同原Peer新增非静音PCM，不只检查ICE或历史总计数。
function verifyAudioPair(before,after) {assert.equal(before.length,2);assert.equal(after.length,2);after.forEach((v,i)=>{validMedia(v);samePeer(before[i],v);assert.ok(v.inputAttached&&v.outputEnabled&&!v.held&&v.frames>before[i].frames&&v.nonSilentFrames>=before[i].nonSilentFrames+2,'actual_audio_missing');});return true;}
// verifyHeldPair 稳定窗口内双端Sender脱离、输出关闭、非静音不增长；Peer仍须原实例。
function verifyHeldPair(original,first,second) {assert.equal(first.length,2);assert.equal(second.length,2);second.forEach((v,i)=>{for(const s of [first[i],v]){validMedia(s);samePeer(original[i],s);assert.ok(s.held&&!s.inputAttached&&!s.outputEnabled,'actual_hold_missing');}assert.ok(v.frames>first[i].frames&&v.nonSilentFrames===first[i].nonSilentFrames,'held_audio_leaked');});return true;}
// verifyResumedPair 显式恢复后仍是原Peer且两端重新实际消费PCM。
function verifyResumedPair(original,held,current) {current.forEach((v,i)=>samePeer(original[i],v));return verifyAudioPair(held,current);}
// verifyControl 同轮原token可有ACK/门控中间态，但最终只允许一个原Completed；旧token或冲突终态不能被过滤掉。
function verifyControl(receipts,operation,afterIndex) {
  assert.ok(Array.isArray(receipts)&&receipts.length<=32&&['hold','resume'].includes(operation)&&count(afterIndex,512),'control_receipt_invalid');
  const keys=['kind','operation','controlToken','state','reason','revision','code','actualApplied','actualHeld','receiptIndex'];let previousIndex=0;
  for(const value of receipts){assert.ok(exact(value,keys)&&value.kind==='humanControlReceipt'&&identifier(value.controlToken,128)&&count(value.receiptIndex,512)&&value.receiptIndex>previousIndex&&count(value.revision,Number.MAX_SAFE_INTEGER)&&typeof value.actualApplied==='boolean'&&typeof value.actualHeld==='boolean','control_receipt_invalid');previousIndex=value.receiptIndex;}
  const values=receipts.filter(value=>value.receiptIndex>afterIndex);assert.ok(values.length>0,'control_unconfirmed');
  const token=values[0].controlToken,priorTokens=new Set(receipts.filter(value=>value.receiptIndex<=afterIndex).map(value=>value.controlToken));
  assert.ok(!priorTokens.has(token)&&values.every(value=>value.operation===operation&&value.controlToken===token),'control_receipt_ambiguous');
  const last=values.at(-1);assert.ok(last.state==='completed'&&last.code===200&&last.actualApplied===true&&last.actualHeld===(operation==='hold')&&last.reason===''&&last.revision>0,'control_unconfirmed');
  for(const value of values.slice(0,-1))assert.ok(value.state==='pending'&&value.reason===''&&((value.code===0&&value.revision===0)||(value.code===200&&value.revision===last.revision))&&value.actualHeld===(value.actualApplied?operation==='hold':operation==='resume'),'control_receipt_conflict');
  return true;
}
// waitFor 同一个原单调预算内有限轮询；超时或signal取消不会续期/重试副作用。
async function waitFor(read,until,ports={}) {const now=ports.now||(()=>performance.now()),sleep=ports.sleep||(ms=>new Promise(r=>setTimeout(r,ms)));while(now()<until){if(ports.cancelled?.())throw Error('run_interrupted');const value=await read();if(ports.cancelled?.())throw Error('run_interrupted');if(now()>=until)break;if(value)return value;await sleep(Math.min(100,until-now()));}throw Error('phase_timeout');}
// budgetCall 限制宿主只读查询等待；超时只中止验收，不自动重试、改变数据库或制造空成功。
async function budgetCall(read,until) {
  const remaining=until-performance.now();assert.ok(remaining>0,'phase_timeout');let timer;
  try {return await Promise.race([Promise.resolve().then(read),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('phase_timeout')),Math.min(8000,remaining));})]);}
  finally {clearTimeout(timer);}
}
// parseJSON 只解浏览器有限CLI包装，错误正文不回显。
function parseJSON(raw) {assert.ok(typeof raw==='string'&&Buffer.byteLength(raw)<=65536,'browser_response_limit');let value=JSON.parse(raw);if(typeof value==='string')value=JSON.parse(value);assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'browser_response_invalid');return value;}
// requireObservationPorts 两个正式只读入口缺一即在spawn前拒；不允许默认空快照或fixed received。
async function requireObservationPorts(observer) {assert.ok(observer&&typeof observer.resolveSource==='function'&&typeof observer.readReceipt==='function','formal_observation_unavailable');validateObservationWindow(observer.observationWindow);return observer;}
// validateObservationWindow 固定宿主创建的本轮只读时间窗；轮询不能扩大窗口或读取历史来源。
function validateObservationWindow(value,now=Date.now()) {assert.ok(exact(value,['createdAfter','createdBefore'])&&count(value.createdAfter,Number.MAX_SAFE_INTEGER)&&count(value.createdBefore,Number.MAX_SAFE_INTEGER)&&value.createdAfter>0&&value.createdBefore>value.createdAfter&&value.createdBefore-value.createdAfter<=180000&&value.createdAfter<=now&&value.createdBefore>now,'formal_observation_window_invalid');return Object.freeze({...value});}
// fingerprint 同原PID/start/exe/cwd/argv二读；离开或复用不能成为给别的进程发signal的许可。
function fingerprint(pid,start) {if(!Number.isSafeInteger(pid)||pid<2)return null;const read=()=>{try{const raw=fs.readFileSync('/proc/'+pid+'/stat','utf8'),f=raw.slice(raw.lastIndexOf(')')+2).trim().split(/\s+/);assert.ok(f.length>=20&&/^\d+$/.test(f[19]),'child_identity_unknown');if(start!==undefined)assert.equal(f[19],start,'child_identity_changed');return {start:f[19],ended:['Z','X','x'].includes(f[0])};}catch(e){if(e.code==='ENOENT')return null;throw e;}};
  const first=read();if(!first||first.ended)return null;let value;try{value={pid,start:first.start,executable:fs.readlinkSync('/proc/'+pid+'/exe'),cwd:fs.readlinkSync('/proc/'+pid+'/cwd'),argv:fs.readFileSync('/proc/'+pid+'/cmdline','utf8').split('\0').filter(Boolean)};}catch(e){const after=read();if(!after||after.ended)return null;throw e;}const after=read();if(!after||after.ended)return null;assert.equal(first.start,after.start,'child_identity_changed');return value;}
// trackChild 必须拿原ChildProcess close和双流end，不能仅靠PID消失报告释放。
function trackChild(child,binary,cwd,argv) {const held={child,binary,cwd,argv,closed:false,record:null,stdoutEnded:false,stderrEnded:false,code:null};held.spawned=new Promise((resolve,reject)=>{child.once('spawn',resolve);child.once('error',()=>reject(Error('child_spawn_failed')));});held.spawned.catch(()=>{});
  held.done=new Promise(resolve=>child.once('close',(code,signal)=>{held.closed=true;held.code=code;held.signal=signal;resolve();}));child.stdout.once('end',()=>{held.stdoutEnded=true;});child.stderr.once('end',()=>{held.stderrEnded=true;});held.originalClose=childClose(child);return held;}
// captureChild 只固定原实际spawn产物身份，不把observer自报身份当授权来源。
function captureChild(held,read=fingerprint) {const value=read(held.child.pid);assert.ok(value&&value.pid===held.child.pid&&value.executable===held.binary&&value.cwd===held.cwd&&JSON.stringify(value.argv)===JSON.stringify(held.argv),'child_identity_unknown');held.record={...value,argv:[...value.argv]};return held.record;}
// releaseChild 仅同精确原实例可收尾；等待真正close/双流，PID换代拒绝发送任何signal。
async function releaseChild(held,ports={}) {if(!held)return true;if(held.closed){assert.ok(held.stdoutEnded&&held.stderrEnded,'child_stream_end_missing');return true;}const read=ports.fingerprint||fingerprint;assert.ok(held.record,'child_identity_unknown');const same=()=>{const current=read(held.record.pid,held.record.start);if(current)assert.deepEqual(current,held.record,'child_identity_changed');return current;};
  const wait=async ms=>{let timer;try{return await Promise.race([held.done.then(()=>true),new Promise(r=>{timer=setTimeout(()=>r(false),ms);})]);}finally{clearTimeout(timer);}};
  if(same())held.child.kill('SIGTERM');if(!await wait(8000)&&same()){held.child.kill('SIGKILL');await wait(8000);}assert.ok(held.closed&&held.stdoutEnded&&held.stderrEnded,'child_close_missing');return true;}
// readPrivate 原600普通文件有界读取，凭据只停留内存，不自动修权限或跟随symlink。
function readPrivate(file,max=65536) {assert.ok(path.isAbsolute(file||''),'private_input_invalid');const s=fs.lstatSync(file);assert.ok(s.isFile()&&!s.isSymbolicLink()&&s.uid===process.getuid()&&(s.mode&0o777)===0o600&&s.size>0&&s.size<=max,'private_input_invalid');return fs.readFileSync(file);}
// validateEnvironment 强制单次显式headed/固定产物pin/回环API；宿主负责实际依赖启动与身份配置读回。
function validateEnvironment(env) {assert.equal(env.UR_SHARED_HANDOFF_LIVE,'1','explicit_live_opt_in_required');assert.equal(env.AGENT_BROWSER_HEADED,'1','headed_browser_required');assert.ok(env.DISPLAY&&path.isAbsolute(env.XAUTHORITY||''),'headed_browser_required');
  const binary=env.UR_SHARED_HANDOFF_BIN;assert.ok(path.isAbsolute(binary||'')&&!fs.lstatSync(binary).isSymbolicLink()&&fs.statSync(binary).isFile(),'device_binary_missing');assert.ok(/^[a-f0-9]{64}$/.test(env.UR_SHARED_HANDOFF_BIN_SHA256||'')&&createHash('sha256').update(fs.readFileSync(binary)).digest('hex')===env.UR_SHARED_HANDOFF_BIN_SHA256,'binary_pin_changed');
  const data=JSON.parse(readPrivate(env.UR_SHARED_HANDOFF_INPUT));assert.ok(exact(data,['tenantCode','ownerUserId','serviceId','caller','human','api','expiresAt']),'input_schema_invalid');assert.ok(identifier(data.tenantCode)&&identifier(data.serviceId)&&numericIdentity(data.ownerUserId)&&count(data.expiresAt,Number.MAX_SAFE_INTEGER)&&data.expiresAt>Date.now(),'input_schema_invalid');
  for(const device of [data.caller,data.human]){assert.ok(exact(device,['productID','deviceName','secret'])&&identifier(device.productID)&&identifier(device.deviceName)&&!/[\/:+#]/.test(device.productID+device.deviceName)&&identifier(device.secret,1024),'device_configuration_missing');}assert.notEqual(data.caller.productID+':'+data.caller.deviceName,data.human.productID+':'+data.human.deviceName,'distinct_devices_required');
  assert.ok(exact(data.api,['base','app','token'])&&/^http:\/\/127\.0\.0\.1:[1-9][0-9]{3,4}$/.test(data.api.base)&&Number(data.api.base.split(':').at(-1))<=65535&&numericIdentity(data.api.app)&&identifier(data.api.token,8192),'ordinary_api_configuration_missing');
  const ports=[env.UR_SHARED_HANDOFF_PORT_FIRST,env.UR_SHARED_HANDOFF_PORT_SECOND].map(v=>Number(v));assert.ok(ports.every(v=>Number.isInteger(v)&&v>=1024&&v<=65535)&&ports[0]!==ports[1],'bridge_ports_invalid');assert.ok(env.UR_MQTT_HOST==='127.0.0.1'&&Number.isInteger(Number(env.UR_MQTT_PORT))&&Number(env.UR_MQTT_PORT)>=1024&&Number(env.UR_MQTT_PORT)<=65535,'mqtt_loopback_required');
  assert.ok(path.isAbsolute(env.UR_SHARED_HANDOFF_AUDIO_FIXTURE||'')&&!/[\r\n,]/.test(env.UR_SHARED_HANDOFF_AUDIO_FIXTURE)&&/^[a-f0-9]{64}$/.test(env.UR_SHARED_HANDOFF_AUDIO_SHA256||''),'audio_fixture_missing');const wave=fs.readFileSync(env.UR_SHARED_HANDOFF_AUDIO_FIXTURE);assert.ok(wave.length>=44&&wave.length<=10485760&&wave.subarray(0,4).toString()==='RIFF'&&wave.subarray(8,12).toString()==='WAVE','audio_fixture_invalid');assert.equal(createHash('sha256').update(wave).digest('hex'),env.UR_SHARED_HANDOFF_AUDIO_SHA256,'audio_pin_changed');
  return {binary,data,ports};}
// installPageObservation 旁观原Worklet事实，不构造媒体/回执或更改业务函数返回。
function installPageObservation(observeStop=false) {if(globalThis.__urSharedHandoffObservation)throw Error('observer_duplicate');const observed={outputEnabled:null};globalThis.__urSharedHandoffObservation=observed;
  if(!duplexHumanMedia)throw Error('human_media_missing');
  // 仅原隔离宿主精确诊断开关启用旁证；普通入口不改变原媒体统计或协议。
  if(observeStop===true)duplexHumanMedia.enableStopDiagnostics();
  const original=duplexHumanMedia.publish;duplexHumanMedia.publish=function(v){const result=original.apply(this,arguments);if(v?.kind==='humanVoiceOutputGated'&&typeof v.enabled==='boolean'&&v.sessionId===this.scope?.sessionId&&v.routeGeneration===this.scope?.routeGeneration&&v.peerEpoch===this.scope?.peerEpoch)observed.outputEnabled=v.enabled;return result;};}
// readHumanAudioSnapshot只读原页面纯getter；访问器、异常、额外字段或自由cause一律不可用。
// 函数自包含供原browser eval使用，不读取身份、错误正文或再次操作Peer。
function readHumanAudioSnapshot() {
  try {
    if(!duplexHumanMedia)return null;
    const descriptor=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(duplexHumanMedia),'audioSnapshot');
    if(!descriptor||!Object.hasOwn(descriptor,'value')||typeof descriptor.value!=='function')return null;
    const value=descriptor.value.call(duplexHumanMedia);
    if(!value||typeof value!=='object'||Array.isArray(value))return null;
    const fields=Object.getOwnPropertyDescriptors(value);
    if(Reflect.ownKeys(fields).length!==2||!Object.hasOwn(fields.known||{},'value')||!Object.hasOwn(fields.cause||{},'value'))return null;
    const known=fields.known.value,cause=fields.cause.value;
    const causes=['processorError','startOutputUnavailable','startException','answerException','remoteTrackInvalid','remoteTrackSetupFailed','remoteSinkRejected','peerFailed','peerDisconnected','explicitClose'];
    return typeof known==='boolean'&&((known&&causes.includes(cause))||(!known&&cause==='unknown'))?{known,cause}:null;
  }catch{return null;}
}
// projectHumanAudioSnapshot只保留getter已返回的两枚举事实，不执行访问器或反射原对象。
function projectHumanAudioSnapshot(value) {
  try {
    if(!value||typeof value!=='object'||Array.isArray(value))return null;
    const fields=Object.getOwnPropertyDescriptors(value);
    if(Reflect.ownKeys(fields).length!==2||!Object.hasOwn(fields.known||{},'value')||!Object.hasOwn(fields.cause||{},'value'))return null;
    const known=fields.known.value,cause=fields.cause.value;
    const causes=['processorError','startOutputUnavailable','startException','answerException','remoteTrackInvalid','remoteTrackSetupFailed','remoteSinkRejected','peerFailed','peerDisconnected','explicitClose'];
    return typeof known==='boolean'&&((known&&causes.includes(cause))||(!known&&cause==='unknown'))?{known,cause}:null;
  }catch{return null;}
}
// cachedHumanAudioObservation只复制最后有效业务采样；catch不发命令，也不把历史样本称故障现场。
function cachedHumanAudioObservation(samples) {
  const result={sampling:'lastBusinessSample'};
  for(const [index,role]of ['caller','human'].entries()) {
    const last=projectHumanAudioSnapshot(samples[index]);
    result[role]={sampled:last!==null,last};
  }
  return result;
}
// projectHumanAudioObservation只投影原两次采样的data属性；未知/访问器不能补造sampled或执行自由代码。
function projectHumanAudioObservation(value) {
  const result={sampling:'lastBusinessSample',caller:{sampled:false,last:null},human:{sampled:false,last:null}};
  try {
    if(!value||typeof value!=='object'||Array.isArray(value))return result;
    const roles=Object.getOwnPropertyDescriptors(value);
    if(Reflect.ownKeys(roles).length!==3||roles.sampling?.value!=='lastBusinessSample')return result;
    for(const role of ['caller','human']) {
      if(!Object.hasOwn(roles[role]||{},'value'))continue;
      const original=roles[role].value;if(!original||typeof original!=='object'||Array.isArray(original))continue;
      const fields=Object.getOwnPropertyDescriptors(original);
      if(Reflect.ownKeys(fields).length!==2||!Object.hasOwn(fields.sampled||{},'value')||!Object.hasOwn(fields.last||{},'value'))continue;
      const last=projectHumanAudioSnapshot(fields.last.value);
      if(fields.sampled.value===true&&last)result[role]={sampled:true,last};
    }
  }catch{/* 不可用快照仍为null，原失败不会因投影异常改变。 */}
  return result;
}
// readPageFacts 只读原实际Peer/轨/PCM及控制回执；身份用于进程内关联，最终report删除。
async function readPageFacts(observeStop=false,readStop=null) {const human=duplexHumanMedia,peer=human?.peer,scope=human?.scope,stats=human?.statistics();let fingerprint='';if(peer?.localDescription?.sdp&&peer?.remoteDescription?.sdp){const bytes=new TextEncoder().encode(peer.localDescription.sdp+'\n'+peer.remoteDescription.sdp),hash=await crypto.subtle.digest('SHA-256',bytes);fingerprint=Array.from(new Uint8Array(hash)).map(v=>v.toString(16).padStart(2,'0')).join('');}
  // 只在原业务eval中读取已启用的纯getter；缺失/异常留null，默认字段形状完全不变。
  let audioSnapshot=null;if(observeStop===true&&typeof readStop==='function')try{audioSnapshot=readStop();}catch{/* 旁证不得中断原业务读取。 */}
  // 只识别正式页面已有的固定release文案；模型文本、浏览器异常正文和未知文案永不进入报告。
  const activationFailure={reason:terminated?'unknown':'none',stage:'unknown',category:'none'};
  if(terminated){const text=document.getElementById('status')?.textContent;
    const stages={'本地配对':'pairing','麦克风授权':'microphonePermission','WebRTC音频端点':'rtcEndpoint','音频上下文':'audioContext','音频处理器加载':'audioProcessor','音频上下文启动':'audioResume'};
    const categories=['NotAllowedError','NotFoundError','NotSupportedError','SecurityError','AbortError','初始化失败'];
    for(const [label,stage] of Object.entries(stages))for(const category of categories)
      if(text===`${label}失败（${category}）；请检查浏览器权限及本机设备进程。`)
        Object.assign(activationFailure,{reason:'activationFailed',stage,category:category==='初始化失败'?'initialization':category});
    const reasons={'本机连接已关闭，麦克风已释放。':'closedLocalSocket','本机连接故障，麦克风已释放。':'localSocketFault','本地媒体报文非法，已停止采集与播放。':'invalidLocalMedia','设备链路失败，麦克风已释放。':'deviceError','会话已结束，麦克风已释放。':'sessionEnded','页面已关闭。':'pageClosed','本地连接不可用或发送积压；请重新启动设备进程。':'localSendUnavailable','麦克风不可用，已结束本地连接。':'microphoneEnded','浏览器采集实态无效，已释放媒体。':'captureFactInvalid','浏览器音频消费事实无效，已释放媒体。':'playbackFactInvalid','浏览器音频驱动失败，已释放麦克风。':'audioDriverFailed','文件媒体驱动失败，已释放设备资源。':'fileDriverFailed','人工语音驱动拒绝命令，已释放设备资源。':'humanCommandRejected','人工语音驱动失败，已释放设备资源。':'humanDriverFailed','WebRTC输出实态无效，已释放设备资源。':'rtcOutputFactInvalid','WebRTC输出确认失败，已释放设备资源。':'rtcOutputAckFailed','WebRTC输出驱动失败，已释放设备资源。':'rtcOutputDriverFailed','WebRTC远端音轨无法启动，已释放设备资源。':'rtcTrackPlayFailed','WebRTC候选协商失败，已释放设备媒体。':'rtcOfferFailed','WebRTC应答失败，已释放设备媒体。':'rtcAnswerFailed','WebRTC媒体连接中断，已释放设备资源。':'rtcDisconnected'};
    if(Object.hasOwn(reasons,text))activationFailure.reason=reasons[text];
  }
  return {ready:!!socket&&socket.readyState===WebSocket.OPEN&&context?.state==='running'&&!terminated,bridgeOpen:!!socket&&socket.readyState===WebSocket.OPEN,audioRunning:context?.state==='running',terminated:terminated===true,humanMediaPresent:!!human,activationFailure,established:!document.getElementById('sendText').disabled,
    completed:metrics.completedResponses,nonSilent:metrics.consumedNonSilent,activeReply:responseId!=='',captureClosed:metrics.captureOpen===false,
    completedID:completedResponseId,playableID:metrics.lastPlayableConsumedResponseId,handoffState:handoffState?.state??0,canEscalate:handoffState?.canEscalate===true,
    controlIndex:metrics.humanControlReceiptIndex,receipts:metrics.humanControlReceipts,canHold:humanControlState?.canHold===true,canResume:humanControlState?.canResume===true,
    media:{session:scope?.sessionId||'',peerEpoch:scope?.peerEpoch||'',peerFingerprint:fingerprint,connected:stats?.connected===true,frames:stats?.frames||0,nonSilentFrames:stats?.nonSilentFrames||0,
      inputAttached:!!human?.transceiver?.sender?.track&&human.transceiver.sender.track===stream?.getAudioTracks()[0],outputEnabled:globalThis.__urSharedHandoffObservation?.outputEnabled===true,held:humanControlState?.held===true},
    released:terminated&&(!stream||stream.getTracks().every(t=>t.readyState==='ended'))&&(!peer||peer.connectionState==='closed'),...(observeStop===true?{audioSnapshot}:{})};}
// apiRequest 仅正式本人self读取或Consent创建；无重定向/重试、8秒/64KiB有界，私有响应仅在内存。
async function apiRequest(api,tenant,route,body,transport=fetch) {
  assert.ok(['system/user/self/get-one','im/context-consents/create'].includes(route),'api_route_forbidden');
  const response=await transport(api.base+'/api/v1/'+route,{method:'POST',redirect:'error',signal:AbortSignal.timeout(8000),headers:{'content-type':'application/json','tenant-code':tenant,'app-id':api.app,token:api.token},body:JSON.stringify(body)});
  assert.ok(response.ok,'api_http_failed');const reader=response.body.getReader(),chunks=[];let size=0;
  try {while(true){const {done,value}=await reader.read();if(done)break;size+=value.length;assert.ok(size<=65536,'api_response_limit');chunks.push(Buffer.from(value));}}
  finally {await reader.cancel().catch(()=>{});reader.releaseLock();}
  const reply=JSON.parse(Buffer.concat(chunks).toString('utf8'));assert.ok(reply.code===200&&reply.data&&typeof reply.data==='object'&&!Array.isArray(reply.data),'api_rejected');return reply.data;
}
// verifyOrdinarySelf 核原本人及当前企业普通client成员，管理员或另一人的Token不作为测试代签。
function verifyOrdinarySelf(value,tenant,owner) {
  assert.ok(String(value.userID)===owner&&Array.isArray(value.tenants),'ordinary_self_changed');const members=value.tenants.filter(v=>v.tenantCode===tenant);
  assert.ok(members.length===1&&members[0].status===1&&![1,true].includes(members[0].isTenantOwner)&&Array.isArray(members[0].roles)&&members[0].roles.length>0&&members[0].roles.every(v=>v.code==='client'&&v.status===1&&![1,true].includes(v.isFullAuth)),'ordinary_self_required');return true;
}
// consentRequest 仅调用正式本人创建，不从client文本拼接可信快照。
async function consentRequest(api,tenant,body) {const result=await apiRequest(api,tenant,'im/context-consents/create',body);assert.ok(result.consent,'consent_rejected');return result.consent;}
// projectReadyObservation 仅投影最后成功读取的就绪布尔；子阶段用于定位等待或安装失败，不代替原通过条件。
function projectReadyObservation(pages,step='wait') {
  const fields=['ready','bridgeOpen','audioRunning','terminated','established','humanMediaPresent'];
  if(pages===undefined){const empty=()=>Object.fromEntries([['observed',false],...fields.map(key=>[key,false])]);return {step:'notStarted',stage:'unobserved',caller:empty(),human:empty()};}
  assert.ok(['wait','installCaller','installHuman','installed'].includes(step)&&Array.isArray(pages)&&pages.length===2,'readiness_facts_invalid');
  const projected=pages.map(page=>{assert.ok(page&&fields.every(key=>typeof page[key]==='boolean')&&page.ready===(page.bridgeOpen&&page.audioRunning&&!page.terminated),'readiness_facts_invalid');return Object.fromEntries([['observed',true],...fields.map(key=>[key,page[key]])]);});
  const stage=projected.some(page=>page.terminated)?'pageTerminated':!projected.every(page=>page.bridgeOpen)?'waitingBridge':!projected.every(page=>page.audioRunning)?'waitingAudio':!projected[0].established?'waitingAISession':'ready';
  return {step,stage,caller:projected[0],human:projected[1]};
}
// requireReadyPages 终止是不可恢复事实，立即拒绝而非等待预算耗尽；其余就绪条件保持原定义。
function requireReadyPages(pages) {projectReadyObservation(pages);assert.ok(pages.every(page=>!page.terminated),'page_activation_terminated');return pages.every(page=>page.ready)&&pages[0].established;}
// projectActivationObservation 只保留页面固定失败码与原初始化阶段，缺观测为null；不得反射原status。
function projectActivationObservation(pages) {
  if(pages===undefined)return {caller:null,human:null};
  assert.ok(Array.isArray(pages)&&pages.length===2,'activation_facts_invalid');
  const reasons=['none','unknown','activationFailed','closedLocalSocket','localSocketFault','invalidLocalMedia','deviceError','sessionEnded','pageClosed','localSendUnavailable','microphoneEnded','captureFactInvalid','playbackFactInvalid','audioDriverFailed','fileDriverFailed','humanCommandRejected','humanDriverFailed','rtcOutputFactInvalid','rtcOutputAckFailed','rtcOutputDriverFailed','rtcTrackPlayFailed','rtcOfferFailed','rtcAnswerFailed','rtcDisconnected'];
  const stages=['pairing','microphonePermission','rtcEndpoint','audioContext','audioProcessor','audioResume'],categories=['NotAllowedError','NotFoundError','NotSupportedError','SecurityError','AbortError','initialization'];
  const projected=pages.map(page=>{const value=page?.activationFailure;assert.ok(exact(value,['reason','stage','category'])&&reasons.includes(value.reason)&&(value.reason==='activationFailed'?stages.includes(value.stage)&&categories.includes(value.category):value.stage==='unknown'&&value.category==='none'),'activation_facts_invalid');return {reason:value.reason,stage:value.stage,category:value.category};});
  return {caller:projected[0],human:projected[1]};
}
// hangupData 从value读取自有数据属性key；缺失、访问器或非法对象返回undefined，绝不执行诊断getter。
function hangupData(value,key){try{if(!value||typeof value!=='object'||Array.isArray(value))return undefined;const d=Object.getOwnPropertyDescriptor(value,key);return d&&Object.hasOwn(d,'value')?d.value:undefined;}catch{return undefined;}}
// hangupExact 核value是否只包含keys规定的数据属性；额外Symbol、非枚举键或访问器返回false。
function hangupExact(value,keys){try{return !!value&&typeof value==='object'&&!Array.isArray(value)&&Reflect.ownKeys(value).length===keys.length&&keys.every(key=>Object.hasOwn(Object.getOwnPropertyDescriptor(value,key)||{},'value'));}catch{return false;}}
// projectHangupSample 从同次readPageFacts快照value保留九个状态布尔；任一字段非法返回null，不保留身份、计数或正文。
function projectHangupSample(value){
  const keys=['terminated','released','bridgeOpen','audioRunning','humanMediaPresent'],media=hangupData(value,'media'),result={};
  for(const key of keys){const v=hangupData(value,key);if(typeof v!=='boolean')return null;result[key]=v;}
  for(const key of ['connected','inputAttached','outputEnabled','held']){const v=hangupData(media,key);if(typeof v!=='boolean')return null;result[key]=v;}
  return result;
}
// projectHangupObservation 校验value的等待阶段、通过标志及双端最后样本；非法闭集返回unknown，不能替代正式focus释放、私有清空或sessionEnded。
function projectHangupObservation(value){
  const empty={sampling:'lastBusinessSample',stage:'notStarted',pageReleaseWaitPassed:false,deviceCloseWaitPassed:false,caller:null,human:null,unknown:true};
  const keys=Object.keys(empty),stage=hangupData(value,'stage');
  if(!hangupExact(value,keys)||hangupData(value,'sampling')!=='lastBusinessSample'||!['notStarted','pageRelease','deviceClose','completed'].includes(stage))return empty;
  const page=hangupData(value,'pageReleaseWaitPassed'),closed=hangupData(value,'deviceCloseWaitPassed'),unknown=hangupData(value,'unknown');
  if(typeof page!=='boolean'||typeof closed!=='boolean'||typeof unknown!=='boolean'||page!==['deviceClose','completed'].includes(stage)||closed!==(stage==='completed'))return empty;
  const result={sampling:'lastBusinessSample',stage,pageReleaseWaitPassed:page,deviceCloseWaitPassed:closed,caller:null,human:null,unknown};
  for(const role of ['caller','human']){const sample=hangupData(value,role);if(sample===null)continue;const fields=['terminated','released','bridgeOpen','audioRunning','humanMediaPresent','connected','inputAttached','outputEnabled','held'];if(!hangupExact(sample,fields))return empty;const parsed=projectHangupSample({...sample,media:sample});if(!parsed)return empty;result[role]=parsed;}
  if(stage==='notStarted'&&(result.caller!==null||result.human!==null))return empty;
  return result;
}
// createHangupObservation 为每次run创建只读缓存；begin和pagesReleased仅记录原等待位置，observe仅接同次业务快照，snapshot返回固定事实且不驱动资源。
function createHangupObservation(){
  let stage='notStarted',pageReleaseWaitPassed=false,deviceCloseWaitPassed=false,unknown=false;const samples=[null,null];
  return {begin(){stage='pageRelease';},observe(index,value){if(stage==='notStarted'||!['pageRelease','deviceClose'].includes(stage)||![0,1].includes(index))return;const sample=projectHangupSample(value);if(sample)samples[index]=sample;else unknown=true;},pagesReleased(){stage='deviceClose';pageReleaseWaitPassed=true;},completed(){stage='completed';deviceCloseWaitPassed=true;},snapshot(){return projectHangupObservation({sampling:'lastBusinessSample',stage,pageReleaseWaitPassed,deviceCloseWaitPassed,caller:samples[0],human:samples[1],unknown});}};
}
// projectReport 只落固定阶段/来源/布尔和清理计数；fixture永远不能冒实际C++或模型。
function projectReport(value) {const phases=['input','boot','ready','model','source','consent','escalate','accept','audio','private','hold','resume','hangup','completed'],ready=value.readyObservation,activation=value.activationObservation,startup=value.startupObservation;const readyObservation=ready?.caller?.observed===true&&ready?.human?.observed===true?projectReadyObservation([ready.caller,ready.human],ready.step):projectReadyObservation(),activationObservation=activation?.caller&&activation?.human?projectActivationObservation([{activationFailure:activation.caller},{activationFailure:activation.human}]):projectActivationObservation(),startupObservation=startup?projectStartupObservation([startup.caller,startup.human]):projectStartupObservation();return {passed:value.passed===true,scenario:'cpp_shared_ai_service_context',phase:phases.includes(value.phase)?value.phase:'unknown',failureCode:value.passed===true?null:projectFailureCode({message:value.failureCode}),evidence:value.offline?'offline-control-fixture':'cpp-backend-model',inputMode:'textWithAudio',actualModelRun:!value.offline&&value.modelCompleted===true,actualCPP:!value.offline&&value.cppStarted===2,privateReceived:value.privateReceived===true,readyObservation,activationObservation,startupObservation,...(value.humanAudioObservation?{humanAudioObservation:projectHumanAudioObservation(value.humanAudioObservation)}:{}),...(value.hangupObservation?{hangupObservation:projectHangupObservation(value.hangupObservation)}:{}),cleanup:{devicesClosed:value.cleanup?.devicesClosed===true,browsersClosed:value.cleanup?.browsersClosed===true}};}
// run 复用实际agent-browser与两C++进程，宿主观察端口只读；无端口/初始化失败不启动模型。
async function run(env=process.env,observer,options={}) {
  const hangupObservation=createHangupObservation(); // 只记录原两个等待边界，finally不能补造释放。
  await requireObservationPorts(observer);assert.ok(!options.offline,'offline_run_not_supported');const observationWindow=validateObservationWindow(observer.observationWindow),config=validateEnvironment(env),deadline=performance.now()+Math.min(180000,observationWindow.createdBefore-Date.now());
  const sessions=['shared-ai-'+randomUUID(),'shared-human-'+randomUUID()],children=[],contexts=[],startups=[],opened=[];let phase='input',modelCompleted=false,privateReceived=false,success=false,cancelled=false,cppStarted=0,failureCode=null,readyObservation=projectReadyObservation(),activationObservation=projectActivationObservation(),startupObservation=projectStartupObservation();
  const interrupted=()=>{cancelled=true;};process.on('SIGTERM',interrupted);process.on('SIGINT',interrupted);
  const guard=()=>{assert.ok(!cancelled&&performance.now()<deadline,'run_interrupted');if(!['hangup','completed'].includes(phase))for(const held of children)assert.ok(!held.closed,'device_ended_early');};
  const browser=(i,...args)=>new Promise((resolve,reject)=>{guard();execFile('agent-browser',['--session',sessions[i],'--headed',...args],{env:{...env,AGENT_BROWSER_ARGS:['--no-sandbox','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream','--use-file-for-fake-audio-capture='+env.UR_SHARED_HANDOFF_AUDIO_FIXTURE,'--autoplay-policy=no-user-gesture-required'].join(',')},timeout:Math.max(1,Math.floor(Math.min(15000,deadline-performance.now()))),maxBuffer:65536},(error,stdout)=>error?reject(Error('browser_command_failed')):resolve(stdout.trim()));});
  const observeStop=env.UR_DEVICE_CONTEXT_DIAGNOSTIC==='1'; // 只旁观原Human停止首因，默认不追加采样或报告字段。
  const audioSamples=[null,null]; // 每端只保留最后有效原业务getter副本；不得在失败后补采页面。
  let humanAudioObservation=null; // 仅失败报告写入缓存，不新增CLI、连接或重试。
  // snapshot沿原guard/CLI/预算读取业务事实；诊断只搭载同一次eval并缓存有效副本。
  const snapshot=async i=>{const value=parseJSON(await browser(i,'eval','('+readPageFacts.toString()+')('+(observeStop?'true,('+readHumanAudioSnapshot.toString()+')':'')+')'));if(observeStop){const last=projectHumanAudioSnapshot(value.audioSnapshot);if(last)audioSamples[i]=last;}if(observeStop&&phase==='hangup')hangupObservation.observe(i,value);return value;};
  const wait=read=>waitFor(read,deadline,{cancelled:()=>cancelled});const cleanup={devicesClosed:false,browsersClosed:false};
  try {await captureFailure(async()=>{
    verifyOrdinarySelf(await budgetCall(()=>apiRequest(config.data.api,config.data.tenantCode,'system/user/self/get-one',{withTenant:true}),deadline),config.data.tenantCode,config.data.ownerUserId);
    phase='boot';for(let i=0;i<2;i++){guard();const device=i?config.data.human:config.data.caller,args=[String(config.ports[i]),...(i?['--human']:[])],child=spawn(config.binary,args,{cwd:root,env:{...env,UR_DEVICE_PRODUCT:device.productID,UR_DEVICE_NAME:device.deviceName,UR_DEVICE_SECRET:device.secret,UR_DEVICE_CONTEXT_DIAGNOSTIC:'1'},stdio:['ignore','pipe','pipe']});
      const held=trackChild(child,config.binary,root,[config.binary,...args]),diagnostic=contextObserver(),startup=startupObserver();children.push(held);contexts.push(diagnostic);startups.push(startup);let ready=false,pending='';child.stdout.on('data',bytes=>{pending+=bytes.toString('utf8');if(pending.length>8192){pending='';return;}let end;while((end=pending.indexOf('\n'))>=0){if(pending.slice(0,end)==='http://127.0.0.1:'+config.ports[i]+'/')ready=true;pending=pending.slice(end+1);}});child.stderr.on('data',bytes=>{diagnostic.push(bytes);startup.push(bytes);});child.stderr.once('end',()=>{diagnostic.finish();startup.finish();});await budgetCall(()=>held.spawned,deadline);captureChild(held);cppStarted++;if(options.onSpawn)await budgetCall(()=>options.onSpawn(child,held.record),deadline);await wait(()=>ready);
      opened.push(i);if(options.onBrowserSession)await budgetCall(()=>options.onBrowserSession(sessions[i]),deadline);await browser(i,'open','http://127.0.0.1:'+config.ports[i]+'/');await browser(i,'click','#activate');}
    phase='ready';await wait(async()=>{const both=await Promise.all([snapshot(0),snapshot(1)]);readyObservation=projectReadyObservation(both,'wait');activationObservation=projectActivationObservation(both);return requireReadyPages(both);});for(let i=0;i<2;i++){readyObservation=projectReadyObservation([readyObservation.caller,readyObservation.human],i?'installHuman':'installCaller');await browser(i,'eval','('+installPageObservation.toString()+')('+observeStop+')');}readyObservation=projectReadyObservation([readyObservation.caller,readyObservation.human],'installed');
    phase='model';await wait(async()=>{const v=await snapshot(0);return v.captureClosed&&!v.activeReply;});const before=await snapshot(0);assert.equal(before.completed,0,'initial_model_turn_not_empty');await browser(0,'fill','#textInput',fixedInput);await browser(0,'check','#textAudio');await browser(0,'click','#sendText');await wait(async()=>{try{return verifyModelRound(before,await snapshot(0));}catch{return false;}});modelCompleted=true;
    phase='source';const identity={tenantCode:config.data.tenantCode,ownerUserId:config.data.ownerUserId,subject:'device:'+config.data.caller.productID+':'+config.data.caller.deviceName,...observationWindow,deviceEstablished:true,completedReplies:1};const source=await wait(async()=>{const value=await budgetCall(()=>observer.resolveSource(identity),deadline);if(value===null)return false;assert.ok(exact(value,['sourceSessionId','expiresAt','firstTurnIndex','lastTurnIndex'])&&identifier(value.sourceSessionId,64)&&value.firstTurnIndex===1&&value.lastTurnIndex===1&&count(value.expiresAt,Number.MAX_SAFE_INTEGER)&&value.expiresAt>Date.now(),'source_unconfirmed');return value;});
    phase='consent';const expiresAt=Math.min(config.data.expiresAt,source.expiresAt);const consent=await budgetCall(()=>consentRequest(config.data.api,config.data.tenantCode,{msgToken:randomUUID(),sourceSessionId:source.sourceSessionId,serviceId:config.data.serviceId,firstTurnIndex:1,lastTurnIndex:1,expiresAt}),deadline);assert.ok(/^[a-f0-9]{64}$/.test(consent.consentId||'')&&consent.sourceSessionId===source.sourceSessionId&&consent.serviceId===config.data.serviceId&&consent.recipientSubject==='device:'+config.data.human.productID+':'+config.data.human.deviceName&&consent.scope==='completedText'&&consent.expiresAt===expiresAt,'consent_metadata_changed');
    phase='escalate';await browser(0,'select','#handoffTargetKind','service');await browser(0,'fill','#handoffTarget',config.data.serviceId);await browser(0,'check','#handoffShareContext');await browser(0,'fill','#handoffConsentId',consent.consentId);await escalateWhenReady(()=>snapshot(0),()=>browser(0,'click','#handoffEscalate'),deadline,{cancelled:()=>cancelled});phase='accept';await wait(async()=>parseJSON(await browser(1,'eval','JSON.stringify({enabled:!document.getElementById("handoffAccept").disabled})')).enabled===true);await browser(1,'click','#handoffAccept');
    phase='audio';const original=await wait(async()=>{const v=await Promise.all([snapshot(0),snapshot(1)]);return v.every(x=>x.handoffState===8&&x.media.connected&&x.media.inputAttached&&x.media.outputEnabled)&&v.map(x=>x.media);});await wait(async()=>{try{verifyAudioPair(original,(await Promise.all([snapshot(0),snapshot(1)])).map(x=>x.media));return true;}catch{return false;}});
    phase='private';const receiptRequest={tenantCode:config.data.tenantCode,ownerUserId:config.data.ownerUserId,sourceSessionId:source.sourceSessionId,consentId:consent.consentId,recipientSubject:consent.recipientSubject,serviceId:config.data.serviceId,...observationWindow};await wait(async()=>{const receipt=await budgetCall(()=>observer.readReceipt(receiptRequest),deadline);if(receipt!==null)assert.ok(exact(receipt,['status','sameOriginal','receiptOriginalInstanceMatched','consumed','revoked','expiresAt'])&&['pending','received'].includes(receipt.status)&&receipt.expiresAt===expiresAt&&receipt.expiresAt>Date.now(),'receipt_scope_changed');return privateObservationReady(contexts[1].facts(),receipt);});privateReceived=true;
    for(const initiator of [0,1]){phase='hold';await wait(async()=>(await snapshot(initiator)).canHold);const previous=await snapshot(initiator);await browser(initiator,'click','#handoffHold');await wait(async()=>{const v=await snapshot(initiator);try{verifyControl(v.receipts,'hold',previous.controlIndex);return true;}catch{return false;}});const held=await wait(async()=>{const v=(await Promise.all([snapshot(0),snapshot(1)])).map(x=>x.media);return v.every(x=>x.held&&!x.inputAttached&&!x.outputEnabled)&&v;});await new Promise(r=>setTimeout(r,400));verifyHeldPair(original,held,(await Promise.all([snapshot(0),snapshot(1)])).map(x=>x.media));phase='resume';await wait(async()=>(await snapshot(initiator)).canResume);const controlBefore=await snapshot(initiator);await browser(initiator,'click','#handoffResume');await wait(async()=>{const v=await Promise.all([snapshot(0),snapshot(1)]);try{verifyControl(v[initiator].receipts,'resume',controlBefore.controlIndex);verifyResumedPair(original,held,v.map(x=>x.media));return true;}catch{return false;}});}
    phase='hangup';if(observeStop)hangupObservation.begin();await browser(0,'click','#handoffHangup');await wait(async()=>{const both=await Promise.all([snapshot(0),snapshot(1)]);return both.every(v=>v.released);});if(observeStop)hangupObservation.pagesReleased();for(const held of children){await held.originalClose.wait();assert.ok(held.closed&&held.code===0,'device_exit_failed');}if(observeStop)hangupObservation.completed();success=true;phase='completed';
  },code=>{failureCode=code;startupObservation=projectStartupObservation([startups[0]?.facts()||null,startups[1]?.facts()||null]);});startupObservation=projectStartupObservation([startups[0]?.facts()||null,startups[1]?.facts()||null]);
  } catch(error) {
    if(observeStop)humanAudioObservation=cachedHumanAudioObservation(audioSamples);
    throw error; // 旁证不能将业务失败改成成功或被cleanup覆盖。
  } finally {
    let browsersClosed=true;for(const i of opened){try{await new Promise((resolve,reject)=>execFile('agent-browser',['--session',sessions[i],'close'],{env,timeout:10000,maxBuffer:4096},e=>e?reject(Error()):resolve()));}catch{browsersClosed=false;}}
    cleanup.browsersClosed=browsersClosed;let devicesClosed=true;for(const held of children)try{await releaseChild(held);}catch{devicesClosed=false;}cleanup.devicesClosed=devicesClosed;process.removeListener('SIGTERM',interrupted);process.removeListener('SIGINT',interrupted);
    const report=projectReport({phase,passed:success&&devicesClosed&&browsersClosed,failureCode:failureCode||'cleanup_failed',offline:false,modelCompleted,cppStarted,privateReceived,readyObservation,activationObservation,startupObservation,humanAudioObservation,...(observeStop?{hangupObservation:hangupObservation.snapshot()}:{}),cleanup});if(options.report)options.report(report);if(!report.passed){const error=Error('shared_handoff_live_failed');error.facts=report;throw error;}
  }
  return projectReport({phase,passed:true,offline:false,modelCompleted,cppStarted,privateReceived,readyObservation,activationObservation,startupObservation,...(observeStop?{hangupObservation:hangupObservation.snapshot()}:{}),cleanup});
}
module.exports={projectHangupSample,projectHangupObservation,createHangupObservation,fixedInput,parseContextDiagnostic,contextObserver,parseStartupDiagnostic,startupObserver,projectFailureCode,captureFailure,projectStartupObservation,verifyPrivate,privateObservationReady,escalateWhenReady,verifyModelRound,verifyAudioPair,verifyHeldPair,verifyResumedPair,verifyControl,waitFor,budgetCall,parseJSON,requireObservationPorts,validateObservationWindow,fingerprint,trackChild,captureChild,releaseChild,validateEnvironment,installPageObservation,readHumanAudioSnapshot,projectHumanAudioSnapshot,cachedHumanAudioObservation,readPageFacts,apiRequest,verifyOrdinarySelf,consentRequest,projectReadyObservation,requireReadyPages,projectActivationObservation,projectReport,run};
// CLI没有正式宿主只读observer时默认拒绝，不加载环境自由module或用历史报告充当当前事实。
if(require.main===module)run().then(v=>console.log(JSON.stringify(v))).catch(e=>{console.error(JSON.stringify(e.facts||{passed:false,phase:'input',reason:'formal_observation_unavailable'}));process.exitCode=1;});
