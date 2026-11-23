// 文件说明：共享AI→服务人工端云runner的离线判据回归；不启动模型、MQTT、浏览器或虚构真实验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {PassThrough} = require('node:stream');
const vm = require('node:vm');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {createHash} = require('node:crypto');
// nativeExecFile仅验证Node对非法毫秒预算的同步拒绝；此路径不得创建子进程。
const {execFile:nativeExecFile} = require('node:child_process');
const runner = require('./shared_handoff_live.cjs');

// diagnostic 固定native脱敏诊断夹具，绝不带正文、会话或授权身份。
function diagnostic(overrides = {}) {
  return {kind:'contextDiagnostic',retained:true,cleared:false,expired:false,ackAttempts:1,ackAccepted:1,...overrides};
}
// startupDiagnostic 只描述合成原状态，不把creating等同后端已收到请求或媒体完成。
function startupDiagnostic(overrides={}) {return {kind:'startupDiagnostic',browserReady:true,initialIdleConfirmed:true,connection:'ready',voice:'creating',mediaReady:false,aiOwned:false,...overrides};}

test('启动诊断严格六事实与枚举，不接受正文、身份、重复键或自由错误', () => {
  const value=startupDiagnostic();assert.deepEqual(runner.parseStartupDiagnostic(JSON.stringify(value)),value);
  for(const bad of [{...value,text:'SECRET'},{...value,connection:'other'},{...value,browserReady:1},{...value,voice:'SECRET'}])assert.throws(()=>runner.parseStartupDiagnostic(JSON.stringify(bad)));
  assert.throws(()=>runner.parseStartupDiagnostic(JSON.stringify(value).replace('"browserReady":true','"browserReady":true,"browserReady":true')));
});

test('启动流只保留最后有效原观测，跨stderr分片与原context诊断互不污染', () => {
  const observer=runner.startupObserver(),text=JSON.stringify(startupDiagnostic())+'\n';
  observer.push(Buffer.from(text.slice(0,19)));observer.push(Buffer.from(text.slice(19)));
  observer.push(Buffer.from(JSON.stringify(diagnostic())+'\n自由 SECRET\n'));
  assert.deepEqual(observer.facts(),{last:startupDiagnostic(),unknown:false});
  observer.push(Buffer.from('{"kind":"startupDiagnostic","body":"SECRET"}\n'));
  assert.equal(observer.facts().unknown,true);assert.deepEqual(observer.facts().last,startupDiagnostic());
  observer.finish();assert.equal(JSON.stringify(observer.facts()).includes('SECRET'),false);
  const partial=runner.startupObserver();partial.push(Buffer.from(text.slice(0,19)));partial.finish();assert.equal(partial.facts().unknown,true);
});

test('实际异常捕获保留固定失败码，不执行自由message getter或借finally覆盖为unknown', async () => {
  for(const code of ['device_ended_early','browser_command_failed','phase_timeout','page_activation_terminated','device_close_timeout']) {
    let observed=null;
    await assert.rejects(runner.captureFailure(async()=>{throw Error(code);},value=>{observed=value;}),new RegExp(code));
    const report=runner.projectReport({phase:'ready',failureCode:observed});
    assert.equal(report.failureCode,code);
  }
  let reads=0,observed=null;const secret={};Object.defineProperty(secret,'message',{get(){reads++;throw Error('SECRET');}});
  await assert.rejects(runner.captureFailure(async()=>{throw secret;},value=>{observed=value;}));
  assert.equal(observed,'unknown');assert.equal(reads,0);
  assert.equal(runner.projectReport({failureCode:'SECRET'}).failureCode,'unknown');
  assert.equal(runner.projectReport({passed:true}).failureCode,null);
});

test('正式捕获器进入finally后的通用外层失败仍携带原失败码和清理前最后启动事实', async () => {
  const observer=runner.startupObserver();observer.push(Buffer.from(JSON.stringify(startupDiagnostic())+'\n'));
  let cause=null,original=null;
  const failure=await (async()=>{
    try {await runner.captureFailure(async()=>{throw Error('device_ended_early');},code=>{cause=code;original=runner.projectStartupObservation([observer.facts(),null]);});}
    finally {
      observer.push(Buffer.from(JSON.stringify(startupDiagnostic({connection:'closed',voice:'idle'}))+'\n'));
      const error=Error('shared_handoff_live_failed');error.facts=runner.projectReport({phase:'ready',failureCode:cause,startupObservation:original,cleanup:{devicesClosed:true,browsersClosed:true}});throw error;
    }
  })().catch(error=>error);
  assert.equal(failure.message,'shared_handoff_live_failed');assert.equal(failure.facts.failureCode,'device_ended_early');
  assert.equal(failure.facts.startupObservation.caller.last.connection,'ready');
  assert.equal(failure.facts.startupObservation.caller.last.voice,'creating');
  assert.equal(failure.facts.actualModelRun,false);assert.equal(failure.facts.actualCPP,false);
  const bounded=runner.startupObserver();bounded.push(Buffer.alloc(1048577));assert.equal(bounded.facts().unknown,true);
  assert.deepEqual(runner.projectStartupObservation(),{caller:null,human:null});
  assert.deepEqual(runner.projectStartupObservation([bounded.facts(),null]),{caller:{last:null,unknown:true},human:null});
});
// media 原同Peer实际PCM和Sender门控的只读夹具；不是浏览器或WebRTC事实。
function media(overrides = {}) {
  return {session:'human-leg',peerEpoch:'1',peerFingerprint:'a'.repeat(64),connected:true,
    frames:100,nonSilentFrames:20,inputAttached:true,outputEnabled:true,held:false,...overrides};
}
// controlReceipt 对齐原Host/app十字段，本地控制token只留测试内存，不进入公开报告。
function controlReceipt(overrides = {}) {return {kind:'humanControlReceipt',operation:'hold',controlToken:'original-control',state:'completed',reason:'',revision:1,code:200,actualApplied:true,actualHeld:true,receiptIndex:2,...overrides};}

test('native诊断严格闭集、不接受身份或正文', () => {
  assert.deepEqual(runner.parseContextDiagnostic(JSON.stringify(diagnostic())),diagnostic());
  for (const extra of [{text:'private'},{sessionId:'private'},{hash:'a'.repeat(64)}])
    assert.throws(()=>runner.parseContextDiagnostic(JSON.stringify({...diagnostic(),...extra})));
  for (const change of [{ackAttempts:4097},{ackAccepted:2},{retained:1},{expired:true}, {ackAttempts:-1}])
    assert.throws(()=>runner.parseContextDiagnostic(JSON.stringify(diagnostic(change))));
  assert.throws(()=>runner.parseContextDiagnostic('{"kind":"contextDiagnostic","retained":true'));
  assert.throws(()=>runner.parseContextDiagnostic('{"kind":"contextDiagnostic","retained":true,"retained":true,"cleared":false,"expired":false,"ackAttempts":1,"ackAccepted":1}'));
});

test('诊断流处理UTF8分片、单调计数和不保存自由正文', () => {
  const observer=runner.contextObserver();
  observer.push(Buffer.from('库自由secret正文\n'+JSON.stringify(diagnostic())+'\n'));
  assert.equal(observer.facts().ackAccepted,1);
  const end=JSON.stringify(diagnostic({retained:false,cleared:true,expired:true,ackAttempts:2,ackAccepted:1}))+'\n';
  observer.push(Buffer.from(end.slice(0,15)));observer.push(Buffer.from(end.slice(15)));
  observer.finish();
  assert.equal(observer.facts().expired,true);
  assert.equal(observer.facts().unknown,false);
  assert.equal(JSON.stringify(observer.facts()).includes('secret'),false);
});

test('诊断计数回退、非法marker、超限与缺流终态不冒留存证明', () => {
  for (const suffix of [JSON.stringify(diagnostic({ackAttempts:0,ackAccepted:0}))+'\n',
    '{"kind":"contextDiagnostic","text":"private"}\n','x'.repeat(4097)+'\n']) {
    const observer=runner.contextObserver();observer.push(Buffer.from(JSON.stringify(diagnostic())+'\n'+suffix));
    observer.finish();assert.equal(observer.facts().unknown,true);
  }
  const missing=runner.contextObserver();missing.push(Buffer.from(JSON.stringify(diagnostic())));missing.finish();
  assert.equal(missing.facts().unknown,true);
});

test('私有CPP实际留存+本地ACK不能代替正式Outbox received', () => {
  assert.equal(runner.verifyPrivate(diagnostic(),{status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false}),true);
  for (const change of [{status:'pending'},{sameOriginal:false},{receiptOriginalInstanceMatched:false},{consumed:false},{revoked:true}])
    assert.throws(()=>runner.verifyPrivate(diagnostic(),{status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false,...change}));
  for (const change of [{retained:false},{ackAccepted:0},{expired:true},{cleared:true}])
    assert.throws(()=>runner.verifyPrivate(diagnostic(change),{status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false}));
});

test('正式received与下一native poll诊断可双向乱序，但两份原事实齐全才通过', async () => {
  const received={status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false};
  const waiting={available:false,unknown:false},ready={...diagnostic(),available:true,unknown:false};
  for(const observations of [
    [[waiting,received],[ready,received]],
    [[ready,{...received,status:'pending'}],[ready,received]],
  ]) {
    let now=0,reads=0;
    const result=await runner.waitFor(async()=>{const [cpp,receipt]=observations[reads++];return runner.privateObservationReady(cpp,receipt);},10,{now:()=>now,sleep:async()=>{now+=1;}});
    assert.equal(result,true);assert.equal(reads,2);
  }
});

test('正式received不能补造永远缺失的本地诊断，等原deadline明确失败', async () => {
  let now=0;
  const received={status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false};
  await assert.rejects(runner.waitFor(async()=>runner.privateObservationReady({available:false,unknown:false},received),3,{now:()=>now,sleep:async()=>{now+=1;}}),/phase_timeout/);
  assert.equal(runner.privateObservationReady({...diagnostic({ackAttempts:0,ackAccepted:0}),available:true},received),false);
  assert.equal(runner.privateObservationReady({...diagnostic(),available:true},null),false);
});

test('先收到原服务ACK读回再消费下一native stderr JSON，不伪造本地留存', () => {
  const stream=runner.contextObserver(),received={status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false};
  assert.equal(runner.privateObservationReady(stream.facts(),received),false);
  stream.push(Buffer.from(JSON.stringify(diagnostic({retained:false,ackAttempts:0,ackAccepted:0}))+'\n'));
  assert.equal(runner.privateObservationReady(stream.facts(),received),false);
  const nextPoll=Buffer.from(JSON.stringify(diagnostic())+'\n');stream.push(nextPoll.subarray(0,12));
  assert.equal(runner.privateObservationReady(stream.facts(),received),false);stream.push(nextPoll.subarray(12));
  assert.equal(runner.privateObservationReady(stream.facts(),received),true);
});

test('坏诊断和正式原ACK绑定错误立即拒绝，不因另一通道尚在pending容错', () => {
  const received={status:'received',sameOriginal:true,receiptOriginalInstanceMatched:true,consumed:true,revoked:false};
  for(const change of [{unknown:true},{cleared:true},{expired:true},{ended:true}])assert.throws(()=>runner.privateObservationReady({...diagnostic(),available:true,...change},{...received,status:'pending'}));
  for(const change of [{sameOriginal:false},{receiptOriginalInstanceMatched:false},{consumed:false},{revoked:true}])assert.throws(()=>runner.privateObservationReady({available:false,unknown:false},{...received,...change}));
});

test('原canEscalate非true不得点击；正式可用后只发送一次intent', async () => {
  let now=0,reads=0,clicks=0;
  await runner.escalateWhenReady(async()=>({canEscalate:++reads===3}),async()=>{clicks++;},10,{now:()=>now,sleep:async()=>{now+=1;}});
  assert.equal(reads,3);assert.equal(clicks,1);
  now=0;clicks=0;
  await assert.rejects(runner.escalateWhenReady(async()=>({canEscalate:'true'}),async()=>{clicks++;},3,{now:()=>now,sleep:async()=>{now+=1;}}),/phase_timeout/);
  assert.equal(clicks,0);
});

test('真实模型必须首轮且完成及连续可播放均关联同原response', () => {
  const before={completed:0,captureClosed:true,activeReply:false,nonSilent:0};
  const after={completed:1,activeReply:false,nonSilent:2,completedID:'reply',playableID:'reply'};
  assert.equal(runner.verifyModelRound(before,after),true);
  for(const change of [{playableID:'old'},{completedID:''},{nonSilent:1},{activeReply:true},{completed:2}])
    assert.throws(()=>runner.verifyModelRound(before,{...after,...change}));
  assert.throws(()=>runner.verifyModelRound({...before,completed:1},after));
});

test('双端PCM需实际非静音增长，连接或旧计数不够', () => {
  assert.equal(runner.verifyAudioPair([media(),media()],[media({frames:120,nonSilentFrames:22}),media({frames:125,nonSilentFrames:23})]),true);
  assert.throws(()=>runner.verifyAudioPair([media(),media()],[media({frames:120}),media({frames:125,nonSilentFrames:23})]));
  assert.throws(()=>runner.verifyAudioPair([media(),media()],[media({session:'replacement',frames:120,nonSilentFrames:22}),media({frames:125,nonSilentFrames:23})]));
});

test('保持需两端实际停Sender/输出且稳定非静音计数，恢复仍同Peer', () => {
  const before=[media(),media()];
  const held=before.map(v=>media({held:true,inputAttached:false,outputEnabled:false,frames:v.frames+10}));
  const later=held.map(v=>({...v,frames:v.frames+10}));
  assert.equal(runner.verifyHeldPair(before,held,later),true);
  assert.throws(()=>runner.verifyHeldPair(before,held,[{...later[0],nonSilentFrames:21},later[1]]));
  assert.throws(()=>runner.verifyHeldPair(before,[{...held[0],inputAttached:true},held[1]],later));
  assert.equal(runner.verifyResumedPair(before,later,before.map(v=>({...v,frames:150,nonSilentFrames:25}))),true);
  assert.throws(()=>runner.verifyResumedPair(before,later,[media({peerEpoch:'2',frames:150,nonSilentFrames:25}),media({frames:150,nonSilentFrames:25})]));
});

test('保持窗口帧增长依据真实产品Worklet，关闭输出仍渲染静音并上报原计数', () => {
  const source=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/human-voice-media.js'),'utf8');
  let Processor;class Base {constructor(){this.facts=[];this.port={postMessage:value=>this.facts.push(value)};}}
  vm.runInNewContext(source,{AudioWorkletProcessor:Base,sampleRate:48000,TextEncoder,registerProcessor:(_,value)=>{Processor=value;}});
  const scope={sessionId:'human-leg',routeGeneration:'1',peerEpoch:'1'},processor=new Processor({processorOptions:scope});
  processor.port.onmessage({data:{kind:'humanVoiceOutputGate',...scope,ticket:'1',enabled:false}});
  const output=new Float32Array(128);processor.process([[new Float32Array(128).fill(0.5)]],[[output]]);
  const first=processor.facts.at(-1);assert.equal(first.enabled,false);assert.equal(first.frames,1);assert.equal(first.nonSilentFrames,0);
  for(let i=0;i<127;i++)processor.process([[new Float32Array(128).fill(0.5)]],[[output]]);
  const later=processor.facts.at(-1);assert.equal(later.kind,'humanVoiceOutputProgress');assert.equal(later.frames,128);assert.equal(later.nonSilentFrames,0);assert.equal(output.every(v=>v===0),true);
  const snapshot=value=>media({frames:value.frames,nonSilentFrames:value.nonSilentFrames,inputAttached:false,outputEnabled:false,held:true});
  assert.equal(runner.verifyHeldPair([media(),media()],[snapshot(first),snapshot(first)],[snapshot(later),snapshot(later)]),true);
});

test('正式控制需本轮新唯一completed+code200+实际门控回执', () => {
  const receipt=controlReceipt();
  assert.equal(runner.verifyControl([receipt],'hold',1),true);
  assert.throws(()=>runner.verifyControl([receipt],'hold',2));
  assert.throws(()=>runner.verifyControl([receipt,receipt],'hold',1));
  assert.throws(()=>runner.verifyControl([{...receipt,state:'unknown'}],'hold',1));
});

test('正式ACK先到的原Pending→实际门控Completed必须按同token接受', () => {
  const pending=controlReceipt({state:'pending',actualApplied:false,actualHeld:false,receiptIndex:2});
  const completed=controlReceipt({receiptIndex:3});
  assert.equal(runner.verifyControl([pending,completed],'hold',1),true);
  assert.equal(runner.verifyControl([controlReceipt({state:'pending',code:0,revision:0,actualApplied:true,receiptIndex:2}),completed],'hold',1),true);
});

test('本轮控制不能忽略Unknown/Rejected、多个token、重复终态或迟到旧token', () => {
  const pending=controlReceipt({state:'pending',actualApplied:false,actualHeld:false,receiptIndex:2}),completed=controlReceipt({receiptIndex:3});
  for(const changes of [
    [controlReceipt({state:'unknown',receiptIndex:2}),completed],
    [controlReceipt({state:'rejected',code:409,reason:'staleRevision',receiptIndex:2}),completed],
    [{...pending,controlToken:'another'},completed],
    [completed,controlReceipt({receiptIndex:4})],
    [completed,{...pending,receiptIndex:4}],
    [controlReceipt({receiptIndex:1}),pending,completed],
    [{...pending,operation:'resume'},completed],
  ])assert.throws(()=>runner.verifyControl(changes,'hold',1));
  assert.equal(runner.verifyControl([pending,{...pending,receiptIndex:3},controlReceipt({receiptIndex:4})],'hold',1),true);
});

test('预算耗尽或取消立即失败，不因等待条件稍后为真冒成功', async () => {
  let now=0,calls=0;
  await assert.rejects(runner.waitFor(async()=>{calls++;return false;},5,{now:()=>now,sleep:async()=>{now+=5;}}),/phase_timeout/);
  assert.equal(calls,1);
  await assert.rejects(runner.waitFor(async()=>true,5,{now:()=>0,sleep:async()=>{},cancelled:()=>true}),/run_interrupted/);
});

test('资源登记精确原PID/start/exe/cwd/argv，PID复用拒绝signal', async () => {
  const child=new EventEmitter();child.pid=22;child.stdout=new PassThrough();child.stderr=new PassThrough();
  child.stdout.resume();child.stderr.resume(); // 消费模拟流才能有真实end，不用手工emit造流终态。
  child.spawnfile='/bin/device';child.spawnargs=['/bin/device','19001'];let signals=0;child.kill=()=>{signals++;};
  const original={pid:22,start:'100',executable:'/bin/device',cwd:'/task',argv:['/bin/device','19001']};
  const held=runner.trackChild(child,'/bin/device','/task',['/bin/device','19001']);
  child.emit('spawn');await held.spawned;
  runner.captureChild(held,()=>original);
  await assert.rejects(runner.releaseChild(held,{fingerprint:()=>({...original,start:'101'}),sleep:async()=>{}}),/child_identity_changed/);
  assert.equal(signals,0);
  child.stdout.end();child.stderr.end();child.emit('close',0,null);
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(held.closed,true);
  assert.equal(await runner.releaseChild(held),true);
});

test('即使close已到，缺双流end仍不能报告清理成功', async () => {
  await assert.rejects(runner.releaseChild({closed:true,stdoutEnded:true,stderrEnded:false}),/child_stream_end_missing/);
});

test('原精确child退出后不再发送signal或借PID消失补close', async () => {
  let signals=0;
  const held={closed:true,stdoutEnded:true,stderrEnded:true,child:{kill(){signals++;}}};
  assert.equal(await runner.releaseChild(held),true);assert.equal(signals,0);
});

test('清理只终止已登记原child，必须等原close和双流end', async () => {
  const child=new EventEmitter();child.pid=23;child.stdout=new PassThrough();child.stderr=new PassThrough();child.stdout.resume();child.stderr.resume();
  const record={pid:23,start:'200',executable:'/bin/device',cwd:'/task',argv:['/bin/device']},signals=[];
  child.kill=signal=>{signals.push(signal);child.stdout.end();child.stderr.end();setImmediate(()=>child.emit('close',null,signal));return true;};
  const held=runner.trackChild(child,'/bin/device','/task',['/bin/device']);child.emit('spawn');await held.spawned;runner.captureChild(held,()=>record);
  assert.equal(await runner.releaseChild(held,{fingerprint:()=>record}),true);assert.deepEqual(signals,['SIGTERM']);assert.equal(held.closed,true);assert.equal(held.stdoutEnded&&held.stderrEnded,true);
});

test('缺正式只读来源或Outbox端口在spawn前明确拒绝', async () => {
  await assert.rejects(runner.requireObservationPorts({resolveSource:async()=>({})}),/formal_observation_unavailable/);
  await assert.rejects(runner.requireObservationPorts({readReceipt:async()=>({})}),/formal_observation_unavailable/);
  await assert.rejects(runner.run({},undefined),/formal_observation_unavailable/);
  await assert.rejects(runner.run({},{resolveSource:async()=>{},readReceipt:async()=>{},observationWindow:{createdAfter:Date.now()-1,createdBefore:Date.now()+60000}},{offline:true}),/offline_run_not_supported/);
});

test('宿主固定时间窗必需且有界、失效不能重建或扩为历史读取', async () => {
  const original={createdAfter:1000,createdBefore:181000};
  assert.deepEqual(runner.validateObservationWindow(original,1001),original);
  assert.equal(Object.isFrozen(runner.validateObservationWindow(original,1001)),true);
  for(const changed of [undefined,{...original,createdBefore:181001},{...original,createdAfter:1002},{...original,identity:'private'}])assert.throws(()=>runner.validateObservationWindow(changed,1001));
  assert.throws(()=>runner.validateObservationWindow(original,181000));
  await assert.rejects(runner.requireObservationPorts({resolveSource:async()=>{},readReceipt:async()=>{}}),/formal_observation_window_invalid/);
});

test('普通本人self严格核原owner企业和client角色，不以管理员代签', () => {
  const original={userID:'42',tenants:[{tenantCode:'company',status:1,isTenantOwner:false,roles:[{code:'client',status:1,isFullAuth:false}]}]};
  assert.equal(runner.verifyOrdinarySelf(original,'company','42'),true);
  assert.throws(()=>runner.verifyOrdinarySelf({...original,userID:'43'},'company','42'));
  assert.throws(()=>runner.verifyOrdinarySelf(original,'other','42'));
  for(const change of [{isTenantOwner:true},{roles:[{code:'admin',status:1}]},{status:0}])
    assert.throws(()=>runner.verifyOrdinarySelf({...original,tenants:[{...original.tenants[0],...change}]},'company','42'));
});

test('正式HTTP只读self与Consent创建白名单，网络失败无重试或正文输出', async () => {
  let calls=0;
  const transport=async(url,options)=>{calls++;assert.equal(url,'http://127.0.0.1:19000/api/v1/im/context-consents/create');assert.equal(options.redirect,'error');assert.equal(options.headers.token,'private-token');return new Response(JSON.stringify({code:200,data:{consent:{scope:'completedText'}}}));};
  const api={base:'http://127.0.0.1:19000',app:'app',token:'private-token'};
  assert.deepEqual(await runner.apiRequest(api,'company','im/context-consents/create',{},transport),{consent:{scope:'completedText'}});
  assert.equal(calls,1);
  await assert.rejects(runner.apiRequest(api,'company','database/write',{},transport),/api_route_forbidden/);
  assert.equal(calls,1);
  await assert.rejects(runner.apiRequest(api,'company','system/user/self/get-one',{},async()=>{calls++;throw Error('private response');}));
  assert.equal(calls,2);
});

test('HTTP响应超限、business失败及分片UTF8均严格处理', async () => {
  const api={base:'http://127.0.0.1:19000',app:'app',token:'private'};
  await assert.rejects(runner.apiRequest(api,'company','im/context-consents/create',{},async()=>new Response('x'.repeat(65537))),/api_response_limit/);
  await assert.rejects(runner.apiRequest(api,'company','im/context-consents/create',{},async()=>new Response(JSON.stringify({code:403,data:{}}))),/api_rejected/);
  const bytes=Buffer.from(JSON.stringify({code:200,data:{name:'你好'}})),mid=bytes.indexOf(Buffer.from('你好'))+1;
  const response=new Response(new ReadableStream({start(c){c.enqueue(bytes.subarray(0,mid));c.enqueue(bytes.subarray(mid));c.close();}}));
  assert.equal((await runner.apiRequest(api,'company','system/user/self/get-one',{},async()=>response)).name,'你好');
});

test('浏览器CLI响应限量解包装，不原样反射字符串', () => {
  assert.deepEqual(runner.parseJSON(JSON.stringify(JSON.stringify({enabled:true}))),{enabled:true});
  for(const raw of ['null','[]','"private"','x'.repeat(65537)])assert.throws(()=>runner.parseJSON(raw));
});

test('页面观察器只旁观实际output事件，原函数仍只调用一次且返回不变', () => {
  let called=0;
  const media={scope:{sessionId:'human',routeGeneration:'1',peerEpoch:'1'},publish(){called++;return 'original';}};
  const context=vm.createContext({duplexHumanMedia:media});vm.runInContext('('+runner.installPageObservation.toString()+')()',context);
  assert.equal(media.publish({kind:'humanVoiceOutputGated',enabled:true,sessionId:'human',routeGeneration:'1',peerEpoch:'1'}),'original');
  assert.equal(called,1);assert.equal(context.__urSharedHandoffObservation.outputEnabled,true);
  media.publish({kind:'humanVoiceOutputGated',enabled:false,sessionId:'other',routeGeneration:'1',peerEpoch:'1'});
  assert.equal(called,2);assert.equal(context.__urSharedHandoffObservation.outputEnabled,true);
  assert.throws(()=>vm.runInContext('('+runner.installPageObservation.toString()+')()',context));
});

// actualObservationPage加载正式页面和正式Human统计实现；只模拟平台对象，不启动采集、Peer或浏览器。
function actualObservationPage(scriptPaths=['human-voice-media.js','app.js'],native='') {
  class OfflineAudioContext { constructor(){this.state='running';this.sampleRate=48000;} }
  class OfflineStream {
    constructor(){this.track={readyState:'live',enabled:true};}
    getAudioTracks(){return [this.track];} getVideoTracks(){return [];} getTracks(){return [this.track];}
  }
  class OfflineSource { constructor(stream,context){this.mediaStream=stream;this.context=context;} }
  const elements=new Map();
  const context=vm.createContext({TextEncoder,URLSearchParams,WebSocket:{OPEN:1},
    AudioContext:OfflineAudioContext,MediaStream:OfflineStream,MediaStreamAudioSourceNode:OfflineSource,
    UrInputOwnershipBoot:'1',UrInputOwnershipSampleRate:48000,UrHumanVoiceEnabled:true,
    document:{getElementById(id){if(!elements.has(id))elements.set(id,{disabled:true,value:'',textContent:'',addEventListener(){}});return elements.get(id);}},
    window:{addEventListener(){}}});
  if(native)vm.runInContext(native,context);
  for(const file of scriptPaths)vm.runInContext(fs.readFileSync(path.join(__dirname,'../adapters/desktop/web',file),'utf8'),context);
  const read=()=>vm.runInContext('('+runner.readPageFacts.toString()+')()',context);
  const initialize=()=>vm.runInContext("socket={readyState:1};context=new AudioContext();stream=new MediaStream();source=new MediaStreamAudioSourceNode(stream,context);duplexHumanMedia=new UrHumanVoiceMedia({stream,source,context,emit(){}});",context);
  return {context,elements,read,initialize};
}

// runWithClosedChild执行正式run控制流，仅替换原外部IO为内存端口；没有真实进程、文件、页面或HTTP。
// model分支在最后一次业务采样后关闭原child，用以证明catch不能为取证另发eval或创建页面。
// fractionalBudget仅把可控单调时钟推进至剩余不足15秒，验证正式browser提交的原timeout选项。
async function runWithClosedChild({flag='1',early=false,invalidLast=false,fractionalBudget=false}={}) {
  const filename=path.join(__dirname,'shared_handoff_live.cjs'),binary='/offline/device',input='/offline/input',wave='/offline/audio.wav';
  const audio=Buffer.alloc(44);audio.write('RIFF');audio.write('WAVE',8);
  const data={tenantCode:'company',ownerUserId:'42',serviceId:'support',caller:{productID:'p',deviceName:'a',secret:'synthetic'},human:{productID:'p',deviceName:'b',secret:'synthetic'},api:{base:'http://127.0.0.1:19000',app:'43',token:'synthetic'},expiresAt:Date.now()+60000};
  const files=new Map([[binary,Buffer.from('offline device')],[input,Buffer.from(JSON.stringify(data))],[wave,audio]]),children=[],calls=[],pages=[],sessions=[];
  let closed=false,report=null,modelSamples=0; // 固定模型前采样计数，只控制测试外部端口的退出时点。
  let clockReads=0; // 首次固定deadline基点，后续保留半毫秒余数；不改变宿主观测时间窗。
  const clock={performance:{now:()=>clockReads++===0?0:45000.5}};
  // closeChildren只派发受控原ChildProcess终态与实际模拟流end，不用PID缺失伪造close。
  const closeChildren=async()=>{if(closed)return;closed=true;for(const child of children){child.stdout.end();child.stderr.end();}await new Promise(resolve=>setImmediate(resolve));for(const child of children)child.emit('close',1,null);};
  const io={...fs,lstatSync(file){if(!files.has(file))throw Error('offline_file_missing');return {uid:process.getuid(),mode:0o600,size:files.get(file).length,isFile:()=>true,isSymbolicLink:()=>false};},statSync:()=>({isFile:()=>true}),
    readFileSync(file,encoding){let bytes=files.get(file);if(!bytes&&file.startsWith('/proc/')){const child=children.find(v=>file.startsWith('/proc/'+v.pid+'/'));assert.ok(child,'only synthetic original process may be inspected');if(file.endsWith('/stat'))bytes=Buffer.from(child.pid+' (device) '+['R','1',...Array(17).fill('0'),'123'].join(' '));else if(file.endsWith('/cmdline'))bytes=Buffer.from([binary,...child.args,''].join('\0'));}assert.ok(bytes,'no external file read');return encoding?bytes.toString(encoding):bytes;},
    readlinkSync(file){assert.ok(file.startsWith('/proc/'));return file.endsWith('/exe')?binary:path.resolve(__dirname,'../../..');}};
  const processes={spawn(_binary,args){const child=new EventEmitter();child.pid=90000+children.length;child.args=args;child.stdout=new PassThrough();child.stderr=new PassThrough();child.stdout.resume();child.stderr.resume();child.kill=()=>{throw Error('closed fixture must not receive signals');};children.push(child);setImmediate(()=>{child.emit('spawn');child.stdout.write('http://127.0.0.1:'+args[0]+'/\n');});return child;},
    execFile(command,args,_options,callback){calls.push({command,args,afterClose:closed,timeout:_options.timeout});void(async()=>{
      const name=args[1];let index=sessions.indexOf(name);if(index<0){sessions.push(name);index=sessions.length-1;}
      const action=args[2]==='--headed'?args[3]:args[2],script=args.at(-1);
      if(action==='open'){const page=actualObservationPage();page.initialize();page.context.document.getElementById('sendText').disabled=index!==0;pages[index]=page;return '{}';}
      if(action==='click'&&early&&pages.length===2){await closeChildren();return '{}';}
      if(action==='eval'){
        if(closed)throw Error('diagnostic must not relaunch a stopped browser');
        const page=pages[index];
        if(script.includes('readPageFacts')&&index===0&&vm.runInContext('!!globalThis.__urSharedHandoffObservation',page.context)){
          modelSamples++;vm.runInContext('duplexHumanMedia.close()',page.context);
          // 第三次返回畸形纯getter值，仅验证缓存不能被无效后继样本覆盖；不制造新的停止首因。
          if(invalidLast&&modelSamples===3)vm.runInContext("UrHumanVoiceMedia.prototype.audioSnapshot=()=>({known:true,cause:'SECRET'})",page.context);
          const result=await vm.runInContext(script,page.context);if(!invalidLast||modelSamples===3)await closeChildren();return JSON.stringify(result);
        }
        return JSON.stringify(await vm.runInContext(script,page.context))||'{}';
      }
      return '{}';
    })().then(value=>callback(null,value),error=>callback(error,''));}};
  const loaded={exports:{}},nativeRequire=require('node:module').createRequire(filename);
  const context=vm.createContext({module:loaded,exports:loaded.exports,__dirname,Buffer,console,process,setTimeout,clearTimeout,setImmediate,URL,Response,AbortSignal,
    fetch:async()=>new Response(JSON.stringify({code:200,data:{userID:'42',tenants:[{tenantCode:'company',status:1,isTenantOwner:false,roles:[{code:'client',status:1,isFullAuth:false}]}]}})),
    require:name=>name==='node:fs'?io:name==='node:child_process'?processes:name==='node:perf_hooks'&&fractionalBudget?clock:nativeRequire(name)});
  vm.runInContext(fs.readFileSync(filename,'utf8'),context,{filename});
  const env={UR_SHARED_HANDOFF_LIVE:'1',AGENT_BROWSER_HEADED:'1',DISPLAY:':offline',XAUTHORITY:'/offline/auth',UR_SHARED_HANDOFF_BIN:binary,UR_SHARED_HANDOFF_BIN_SHA256:createHash('sha256').update(files.get(binary)).digest('hex'),UR_SHARED_HANDOFF_INPUT:input,UR_SHARED_HANDOFF_PORT_FIRST:'19001',UR_SHARED_HANDOFF_PORT_SECOND:'19002',UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:'19003',UR_SHARED_HANDOFF_AUDIO_FIXTURE:wave,UR_SHARED_HANDOFF_AUDIO_SHA256:createHash('sha256').update(audio).digest('hex'),...(flag===null?{}:{UR_DEVICE_CONTEXT_DIAGNOSTIC:flag})};
  const observer={resolveSource:async()=>{throw Error('model failure must precede source IO');},readReceipt:async()=>{throw Error('no receipt IO');},observationWindow:{createdAfter:Date.now()-1,createdBefore:Date.now()+60000}};
  const error=await loaded.exports.run(env,observer,{report:value=>{report=value;}}).catch(error=>error);
  return {calls,children,error,report:JSON.parse(JSON.stringify(report)),closed};
}

test('正式browser剩余不足15秒的timeout仍为整数毫秒，Node拒绝分数时未spawn',async()=>{
  assert.throws(()=>nativeExecFile(process.execPath,[],{timeout:14500.5},()=>assert.fail('非法timeout不得触发子进程回调')),{name:'RangeError',code:'ERR_OUT_OF_RANGE'});
  for(const fractionalBudget of [false,true]){
    const result=await runWithClosedChild({early:true,fractionalBudget});
    const timeouts=result.calls.filter(v=>!v.afterClose).map(v=>v.timeout);
    assert.ok(timeouts.length>0,'必须检查正式run实际提交的browser选项');
    for(const timeout of timeouts){assert.ok(Number.isInteger(timeout),'实际browser timeout必须为整数毫秒');assert.ok(timeout>=1&&timeout<=15000);}
    assert.equal(timeouts.some(timeout=>timeout<15000),fractionalBudget);
    assert.equal(result.report.failureCode,'device_ended_early');
    assert.equal(result.report.cleanup.devicesClosed,true);
  }
});

test('正式run在child已close时catch零新增eval，仅保存最后业务样本并明确非故障现场',async()=>{
  const result=await runWithClosedChild();
  assert.equal(result.error.message,'shared_handoff_live_failed');assert.equal(result.report.failureCode,'device_ended_early');
  assert.equal(result.closed,true);assert.equal(result.children.length,2);
  assert.deepEqual(result.calls.filter(v=>v.afterClose).map(v=>v.args.at(-1)),['close','close']);
  assert.deepEqual(result.report.humanAudioObservation,{sampling:'lastBusinessSample',caller:{sampled:true,last:{known:true,cause:'explicitClose'}},human:{sampled:false,last:null}});
  assert.equal(result.report.cleanup.devicesClosed,true);assert.equal(result.report.passed,false);
});

test('正式run缺原业务样本保持null，诊断未精确开启保留默认报告且catch不执行eval',async()=>{
  const early=await runWithClosedChild({early:true});
  assert.deepEqual(early.report.humanAudioObservation,{sampling:'lastBusinessSample',caller:{sampled:false,last:null},human:{sampled:false,last:null}});
  for(const flag of [null,'0','01']){const result=await runWithClosedChild({flag});assert.equal(Object.hasOwn(result.report,'humanAudioObservation'),false);assert.equal(result.calls.some(v=>v.afterClose&&v.args.includes('eval')),false);}
});

test('正式run保留最后有效业务样本，后继非法getter与finally不能覆盖或泄漏自由cause',async()=>{
  const result=await runWithClosedChild({invalidLast:true});
  assert.equal(result.report.failureCode,'device_ended_early');
  assert.equal(result.report.humanAudioObservation.sampling,'lastBusinessSample');
  assert.deepEqual(result.report.humanAudioObservation.caller,{sampled:true,last:{known:true,cause:'explicitClose'}});
  assert.equal(JSON.stringify(result.report).includes('SECRET'),false);
  assert.equal(result.calls.some(v=>v.afterClose&&v.args.includes('eval')),false);
});

test('readPageFacts默认形状不变，精确true仅在原业务eval附带纯停止getter',async()=>{
  const page=actualObservationPage();page.initialize();vm.runInContext('('+runner.installPageObservation.toString()+')(true)',page.context);
  assert.equal(Object.hasOwn(await page.read(),'audioSnapshot'),false);
  const read=flag=>vm.runInContext('('+runner.readPageFacts.toString()+')('+JSON.stringify(flag)+',('+runner.readHumanAudioSnapshot.toString()+'))',page.context);
  assert.equal(Object.hasOwn(await read('1'),'audioSnapshot'),false);
  assert.deepEqual(JSON.parse(JSON.stringify((await read(true)).audioSnapshot)),{known:false,cause:'unknown'});
});

test('正式main实际HTML按原脚本顺序加载输入门控，activate才可进入受控bootstrap探针', async () => {
  const main=fs.readFileSync(path.join(__dirname,'../adapters/desktop/shared_handoff_main.cpp'),'utf8');
  // 仅解码正式main固定C++字面量；不插入测试自造脚本或把平台探针当真实配对成功。
  const insertion=/\.insert\(marker,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\);/.exec(main);
  assert.ok(insertion,'正式HTML装配必须可核验');
  const html=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/index.html'),'utf8').replace('  <script src="/app.js"',insertion[1].match(/"(?:[^"\\]|\\.)*"/g).map(v=>JSON.parse(v)).join('')+'  <script src="/app.js"');
  const scripts=[...html.matchAll(/<script src="\/([^"\s]+)"(?: defer)?><\/script>/g)].map(v=>v[1]);
  const nativeMatch=/assets\["\/shared-handoff-native.js"\]\s*=([\s\S]*?);\n/.exec(main);
  assert.ok(nativeMatch);const literals=nativeMatch[1].match(/"(?:[^"\\]|\\.)*"/g).map(v=>JSON.parse(v));
  assert.equal(literals.length,11);assert.ok(literals[0].endsWith("UrInputOwnershipBoot='"));
  // 同原native合法装配组合：AI端可显式临时借用，纯受话端不能同时启用临时AI恢复。
  for(const [receiver,temporary,endpoint] of [[false,false,false],[false,true,false],
    [false,true,true],[true,false,false],[true,false,true]]) {
    const native=literals[0]+'1'+literals[1]+literals[2]+(receiver?literals[3]:literals[4])+
      (receiver&&endpoint?literals[5]:literals[6])+(temporary?literals[7]:literals[8])+
      (endpoint?literals[9]:literals[10]);
    const page=actualObservationPage(scripts.filter(v=>v!=='shared-handoff-native.js'),native);
    assert.equal(vm.runInContext('globalThis.UrTemporaryCalls===true',page.context),temporary);
    assert.equal(vm.runInContext('globalThis.UrHumanDialEnabled===true',page.context),receiver&&endpoint);
    assert.equal(vm.runInContext('globalThis.UrHumanEndpointSource===true',page.context),endpoint);
    let calls=0;page.context.fetch=async()=>{calls++;throw Error('controlled_pairing_probe');};
    await vm.runInContext('activate()',page.context);
    assert.equal(calls,1,'正式main缺脚本时原guard在bootstrap前拒绝，探针次数为0');
    assert.equal(vm.runInContext('typeof UrInputOwnershipGate',page.context),'function');
    assert.equal((await page.read()).activationFailure.stage,'pairing');
  }
  assert.equal(scripts.filter(v=>v==='input-ownership.js').length,1);
  assert.ok(scripts.indexOf('input-ownership.js')<scripts.indexOf('human-voice-media.js')&&scripts.indexOf('human-voice-media.js')<scripts.indexOf('app.js'));
  assert.equal(fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/index.html'),'utf8').includes('input-ownership.js'),false);
});

test('实际页面与实际Human statistics在尚无Peer时可读，初始化前后有固定就绪事实', async () => {
  const page=actualObservationPage(),before=await page.read();
  assert.equal(before.ready,false);assert.equal(before.bridgeOpen,false);assert.equal(before.audioRunning,false);
  assert.equal(before.terminated,false);assert.equal(before.humanMediaPresent,false);
  page.initialize();const after=await page.read();
  assert.equal(after.ready,true);assert.equal(after.bridgeOpen,true);assert.equal(after.audioRunning,true);
  assert.equal(after.humanMediaPresent,true);assert.equal(after.media.connected,false);
  assert.equal(after.media.frames,0);assert.equal(after.media.nonSilentFrames,0);
  assert.equal(after.established,false);page.elements.get('sendText').disabled=false;
  assert.equal((await page.read()).established,true);
});

test('就绪诊断闭集区分桥、音频、原AI建立，不降低原ready判据或反射页面自由字段', () => {
  const value={ready:false,bridgeOpen:false,audioRunning:false,terminated:false,established:false,humanMediaPresent:false};
  assert.equal(runner.projectReadyObservation().stage,'unobserved');
  assert.equal(runner.projectReadyObservation([value,value]).stage,'waitingBridge');
  assert.equal(runner.projectReadyObservation([value,{...value,terminated:true}]).stage,'pageTerminated');
  const bridge={...value,bridgeOpen:true};
  assert.equal(runner.projectReadyObservation([bridge,bridge]).stage,'waitingAudio');
  const ready={...bridge,audioRunning:true,ready:true,humanMediaPresent:true};
  assert.equal(runner.projectReadyObservation([ready,ready]).stage,'waitingAISession');
  const last=runner.projectReadyObservation([{...ready,established:true,privateText:'SECRET'},ready]);
  assert.equal(last.stage,'ready');assert.equal(last.caller.observed,true);
  assert.equal(JSON.stringify(last).includes('SECRET'),false);
  // 安装子阶段不改变原就绪事实，缺媒体对象也不能被诊断层偷换为新的准入条件。
  for(const step of ['wait','installCaller','installHuman','installed'])
    assert.equal(runner.projectReadyObservation([{...ready,established:true,humanMediaPresent:false},ready],step).step,step);
  assert.throws(()=>runner.projectReadyObservation([ready,ready],'SECRET'));
  for(const bad of [[],[ready],[ready,ready,ready],[{...ready,ready:'true'},ready],[{...ready,bridgeOpen:false},ready]])
    assert.throws(()=>runner.projectReadyObservation(bad));
});

test('公开报告只保留最后成功原页面就绪诊断，不声称清理后DOMnull就是原ready根因', async () => {
  const page=actualObservationPage();page.initialize();const original=await page.read();
  const observation=runner.projectReadyObservation([original,original]);
  page.context.document.getElementById=()=>null;
  await assert.rejects(page.read());
  const report=runner.projectReport({phase:'ready',readyObservation:observation,offline:true});
  assert.equal(report.readyObservation.stage,'waitingAISession');
  assert.equal(report.readyObservation.caller.terminated,false);
  assert.equal(report.actualModelRun,false);assert.equal(report.actualCPP,false);
  const withExtras={...observation,stage:'SECRET',secret:'SECRET',caller:{...observation.caller,token:'SECRET'}};
  assert.equal(JSON.stringify(runner.projectReport({phase:'ready',readyObservation:withExtras})).includes('SECRET'),false);
  assert.equal(runner.projectReport({readyObservation:{...observation,caller:{observed:false}}}).readyObservation.stage,'unobserved');
});

test('实际Human观察器安装保留原publish返回及精确Peer门控事实', async () => {
  const page=actualObservationPage();page.initialize();
  vm.runInContext('('+runner.installPageObservation.toString()+')()',page.context);
  assert.equal((await page.read()).media.outputEnabled,false);
  vm.runInContext("duplexHumanMedia.scope={sessionId:'original',routeGeneration:1,peerEpoch:'original'};duplexHumanMedia.publish({kind:'humanVoiceOutputGated',sessionId:'wrong',routeGeneration:1,peerEpoch:'original',enabled:true});",page.context);
  assert.equal((await page.read()).media.outputEnabled,false);
  vm.runInContext("duplexHumanMedia.publish({kind:'humanVoiceOutputGated',sessionId:'original',routeGeneration:1,peerEpoch:'original',enabled:true});",page.context);
  assert.equal((await page.read()).media.outputEnabled,true);
  assert.throws(()=>vm.runInContext('('+runner.installPageObservation.toString()+')()',page.context),/observer_duplicate/);
});

test('原Human纯getter默认关闭，观察器显式开启后只返回闭集且无驱动副作用',async()=>{
  const page=actualObservationPage();page.initialize();
  const read=()=>vm.runInContext('('+runner.readHumanAudioSnapshot.toString()+')()',page.context);
  assert.equal(read(),null);
  vm.runInContext('('+runner.installPageObservation.toString()+')(true)',page.context);
  assert.deepEqual(JSON.parse(JSON.stringify(read())),{known:false,cause:'unknown'});
  vm.runInContext('duplexHumanMedia.close()',page.context);
  assert.deepEqual(JSON.parse(JSON.stringify(read())),{known:true,cause:'explicitClose'});
  assert.equal(vm.runInContext('terminated',page.context),false);
});
test('停止快照严格拒绝未知类型/额外字段/访问器，不执行恶意getter或输出正文',()=>{
  let reads=0;
  for(const value of [null,{},[],{known:true,cause:'SECRET'},{known:true,cause:'unknown'},
    {known:false,cause:'processorError'},{known:'true',cause:'peerFailed'},
    {known:true,cause:'peerFailed',private:'SECRET'},
    {get known(){reads++;throw Error('SECRET');},cause:'peerFailed'}])
    assert.equal(runner.projectHumanAudioSnapshot(value),null);
  assert.equal(reads,0);
  assert.deepEqual(runner.projectHumanAudioSnapshot({known:false,cause:'unknown'}),{known:false,cause:'unknown'});
  assert.deepEqual(runner.projectHumanAudioSnapshot({known:true,cause:'peerDisconnected'}),{known:true,cause:'peerDisconnected'});
  const page=actualObservationPage();page.initialize();
  Object.defineProperty(page.context.UrHumanVoiceMedia.prototype,'audioSnapshot',{get(){reads++;throw Error('SECRET');}});
  assert.equal(vm.runInContext('('+runner.readHumanAudioSnapshot.toString()+')()',page.context),null);assert.equal(reads,0);
});
test('业务失败只复制已存停止样本，不覆盖原原因或制造sampled，finally不改首因',async()=>{
  let code=null;const observation=runner.cachedHumanAudioObservation([{known:true,cause:'remoteSinkRejected'},null]);
  await assert.rejects(runner.captureFailure(async()=>{throw Error('device_ended_early');},v=>{code=v;}),/device_ended_early/);
  const report=runner.projectReport({phase:'audio',failureCode:code,humanAudioObservation:observation,cleanup:{devicesClosed:true,browsersClosed:true}});
  assert.equal(report.humanAudioObservation.sampling,'lastBusinessSample');assert.equal(report.failureCode,'device_ended_early');
  assert.deepEqual(report.humanAudioObservation.caller,{sampled:true,last:{known:true,cause:'remoteSinkRejected'}});
  assert.deepEqual(report.humanAudioObservation.human,{sampled:false,last:null});
  assert.equal(JSON.stringify(report).includes('SECRET'),false);
  assert.equal(Object.hasOwn(runner.projectReport({phase:'audio'}),'humanAudioObservation'),false);
  let getters=0;
  const unsafe={sampling:'lastBusinessSample',get caller(){getters++;throw Error('SECRET');},human:{sampled:true,last:{known:true,cause:'peerFailed'}}};
  const sanitized=runner.projectReport({phase:'audio',humanAudioObservation:unsafe});
  assert.equal(getters,0);assert.deepEqual(sanitized.humanAudioObservation.caller,{sampled:false,last:null});
  const invalid=runner.cachedHumanAudioObservation([{known:true,cause:'SECRET'},null]);
  assert.deepEqual(invalid,{sampling:'lastBusinessSample',caller:{sampled:false,last:null},human:{sampled:false,last:null}});
});

test('实际activate首次本地配对初始化失败可读固定类别，后继close不覆盖原原因', async () => {
  const page=actualObservationPage();
  // 缺原InputOwnershipGate使正式activate在本地配对阶段拒绝；不连接网络或伪造媒体。
  await vm.runInContext('activate()',page.context);
  const original=await page.read();
  assert.equal(original.terminated,true);
  assert.deepEqual(JSON.parse(JSON.stringify(original.activationFailure)),{reason:'activationFailed',stage:'pairing',category:'initialization'});
  vm.runInContext("release('本机连接已关闭，麦克风已释放。')",page.context);
  assert.deepEqual(JSON.parse(JSON.stringify((await page.read()).activationFailure)),{reason:'activationFailed',stage:'pairing',category:'initialization'});
});

test('激活失败诊断仅承认原固定UI文本，不回显自由正文或伪造细分根因', async () => {
  const page=actualObservationPage();
  for(const [text,expected] of [
    ['麦克风授权失败（NotAllowedError）；请检查浏览器权限及本机设备进程。',{reason:'activationFailed',stage:'microphonePermission',category:'NotAllowedError'}],
    ['WebRTC音频端点失败（NotSupportedError）；请检查浏览器权限及本机设备进程。',{reason:'activationFailed',stage:'rtcEndpoint',category:'NotSupportedError'}],
    ['音频处理器加载失败（初始化失败）；请检查浏览器权限及本机设备进程。',{reason:'activationFailed',stage:'audioProcessor',category:'initialization'}],
    ['本地媒体报文非法，已停止采集与播放。',{reason:'invalidLocalMedia',stage:'unknown',category:'none'}],
    ['SECRET 自由模型回复/令牌',{reason:'unknown',stage:'unknown',category:'none'}],
  ]) {
    page.context.document.getElementById('status').textContent=text;vm.runInContext('terminated=true',page.context);
    const failure=(await page.read()).activationFailure;
    assert.deepEqual(JSON.parse(JSON.stringify(failure)),expected);
    assert.equal(JSON.stringify(failure).includes('SECRET'),false);
  }
});

test('不可恢复页面终止立即拒绝等待，不延长预算或重新点击激活', async () => {
  let reads=0,sleeps=0;
  const value={ready:false,bridgeOpen:false,audioRunning:false,terminated:true,established:false,humanMediaPresent:false};
  await assert.rejects(runner.waitFor(async()=>runner.requireReadyPages([value,value]),100,{now:()=>0,sleep:async()=>{sleeps++;}}),/page_activation_terminated/);
  assert.equal(sleeps,0);
  await assert.rejects(runner.waitFor(async()=>{reads++;return runner.requireReadyPages([value,{...value,terminated:false}]);},100,{now:()=>0,sleep:async()=>{sleeps++;}}),/page_activation_terminated/);
  assert.equal(reads,1);assert.equal(sleeps,0);
});

test('激活报告闭集保留原六阶段六类别，不反射自由status或客户端伪根因', async () => {
  const page=actualObservationPage(),stages={'本地配对':'pairing','麦克风授权':'microphonePermission','WebRTC音频端点':'rtcEndpoint','音频上下文':'audioContext','音频处理器加载':'audioProcessor','音频上下文启动':'audioResume'};
  for(const [label,stage] of Object.entries(stages))for(const category of ['NotAllowedError','NotFoundError','NotSupportedError','SecurityError','AbortError','初始化失败']) {
    vm.runInContext('terminated=true',page.context);
    page.context.document.getElementById('status').textContent=`${label}失败（${category}）；请检查浏览器权限及本机设备进程。`;
    const snapshot=await page.read(),observation=runner.projectActivationObservation([snapshot,snapshot]);
    assert.deepEqual(observation.caller,{reason:'activationFailed',stage,category:category==='初始化失败'?'initialization':category});
    assert.deepEqual(runner.projectReport({activationObservation:observation}).activationObservation,observation);
  }
  assert.deepEqual(runner.projectActivationObservation(),{caller:null,human:null});
  for(const failure of [
    {reason:'SECRET',stage:'unknown',category:'none'},
    {reason:'activationFailed',stage:'pairing',category:'PRIVATE browser error'},
    {reason:'deviceError',stage:'pairing',category:'initialization'},
    {reason:'none',stage:'unknown',category:'none',status:'SECRET'},
  ])assert.throws(()=>runner.projectActivationObservation([{activationFailure:failure},{activationFailure:failure}]));
  // 活页面的模型status不是初始化故障；只输出none，不导出自由正文。
  vm.runInContext('terminated=false',page.context);page.context.document.getElementById('status').textContent='SECRET';
  const snapshot=await page.read();assert.deepEqual(JSON.parse(JSON.stringify(snapshot.activationFailure)),{reason:'none',stage:'unknown',category:'none'});
});

test('公开报告只含有限事实，不输出正文/身份/摘要，offline不冒真实模型', () => {
  const report=runner.projectReport({phase:'private',passed:false,offline:true,modelCompleted:true,cppStarted:2,privateReceived:false,cleanup:{devicesClosed:true,browsersClosed:true},secret:'SECRET',sourceId:'SOURCE',hash:'HASH'});
  assert.equal(report.actualModelRun,false);assert.equal(report.actualCPP,false);
  for (const value of ['SECRET','SOURCE','HASH'])assert.equal(JSON.stringify(report).includes(value),false);
});

test('真实入口的私有输入、产物pin、回环端口不能宽松接收', () => {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'shared-handoff-fixture-'));
  const binary=path.join(directory,'device'),input=path.join(directory,'input.json'),wave=path.join(directory,'audio.wav');
  const audio=Buffer.alloc(44);audio.write('RIFF');audio.write('WAVE',8);
  fs.writeFileSync(binary,'offline fixture');fs.writeFileSync(wave,audio);
  const data={tenantCode:'company',ownerUserId:'42',serviceId:'support',caller:{productID:'p',deviceName:'a',secret:'private'},human:{productID:'p',deviceName:'b',secret:'private'},api:{base:'http://127.0.0.1:19000',app:'43',token:'private'},expiresAt:Date.now()+60000};
  const save=()=>fs.writeFileSync(input,JSON.stringify(data),{mode:0o600});save();
  const env={UR_SHARED_HANDOFF_LIVE:'1',AGENT_BROWSER_HEADED:'1',DISPLAY:':offline',XAUTHORITY:'/offline/auth',UR_SHARED_HANDOFF_BIN:binary,UR_SHARED_HANDOFF_BIN_SHA256:createHash('sha256').update(fs.readFileSync(binary)).digest('hex'),UR_SHARED_HANDOFF_INPUT:input,UR_SHARED_HANDOFF_PORT_FIRST:'19001',UR_SHARED_HANDOFF_PORT_SECOND:'19002',UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:'19003',UR_SHARED_HANDOFF_AUDIO_FIXTURE:wave,UR_SHARED_HANDOFF_AUDIO_SHA256:createHash('sha256').update(audio).digest('hex')};
  try {
    assert.equal(runner.validateEnvironment(env).data.ownerUserId,'42');
    for(const changed of [{UR_SHARED_HANDOFF_BIN_SHA256:'a'.repeat(64)},{UR_MQTT_PORT:'19003.5'},{UR_SHARED_HANDOFF_INPUT:path.relative(process.cwd(),input)},{UR_SHARED_HANDOFF_LIVE:'0'},{UR_SHARED_HANDOFF_PORT_SECOND:'19001'}])assert.throws(()=>runner.validateEnvironment({...env,...changed}));
    data.api.base='http://127.0.0.1:99999';save();assert.throws(()=>runner.validateEnvironment(env));data.api.base='http://127.0.0.1:19000';save();
    for(const invalid of ['core','0','01','9007199254740992',42]){data.api.app=invalid;save();assert.throws(()=>runner.validateEnvironment(env),'普通app必须保持原正十进制安全整数，不回退默认0');}data.api.app='43';save();
    for(const invalid of ['0','01','9007199254740992',42]){data.ownerUserId=invalid;save();assert.throws(()=>runner.validateEnvironment(env),'本人ID必须保持原正十进制安全整数字符串');}data.ownerUserId='42';save();
    fs.chmodSync(input,0o644);assert.throws(()=>runner.validateEnvironment(env));
  } finally {fs.rmSync(directory,{recursive:true,force:true});} // 仅本用例mkdtemp原目录，不碰共享资源。
});

test('只读宿主调用拒绝时不重试，已过原预算不执行外部IO', async () => {
  let calls=0;await assert.rejects(runner.budgetCall(async()=>{calls++;throw Error('observer failed');},performance.now()+100),/observer failed/);assert.equal(calls,1);
  await assert.rejects(runner.budgetCall(async()=>{calls++;},performance.now()-1),/phase_timeout/);assert.equal(calls,1);
  await assert.rejects(runner.budgetCall(async()=>new Promise(()=>{}),performance.now()+5),/phase_timeout/);
});

// 挂断旁证只取正式readPageFacts已有字段；页面对象来自原actualObservationPage，不手写成功状态代替真实投影。
test('挂断九布尔从正式readPageFacts产生，不复制身份正文或媒体计数',async()=>{
  const page=actualObservationPage();page.initialize();const raw=await page.read(),state=runner.createHangupObservation();state.begin();state.observe(0,raw);
  const sample=JSON.parse(JSON.stringify(state.snapshot().caller));
  assert.deepEqual(sample,{terminated:false,released:false,bridgeOpen:true,audioRunning:true,humanMediaPresent:true,connected:false,inputAttached:false,outputEnabled:false,held:false});
  assert.equal(Object.keys(sample).length,9);for(const key of ['session','peerEpoch','peerFingerprint','frames','nonSilentFrames','completedID'])assert.equal(Object.hasOwn(sample,key),false);
  // 修改的是合成页面原状态，不伪造正式sessionEnded或focus释放；readPageFacts必须保留真实“不完整释放”。
  vm.runInContext('terminated=true',page.context);const after=await page.read();assert.equal(after.released,false);state.observe(1,after);assert.equal(state.snapshot().human.terminated,true);assert.equal(state.snapshot().human.released,false);
});
test('挂断投影拒绝阶段和flags非法组合，不能凭completed或清理事实补通过',()=>{
  const base=runner.createHangupObservation().snapshot();
  for(const stage of ['notStarted','pageRelease','deviceClose','completed'])for(const pageReleaseWaitPassed of [false,true])for(const deviceCloseWaitPassed of [false,true]){
    const value={...base,stage,pageReleaseWaitPassed,deviceCloseWaitPassed};
    const valid=pageReleaseWaitPassed===['deviceClose','completed'].includes(stage)&&deviceCloseWaitPassed===(stage==='completed');
    const result=runner.projectHangupObservation(value);assert.equal(result.unknown,!valid);if(valid)assert.equal(result.stage,stage);
  }
  for(const patch of [{stage:'private'},{unknown:'false'},{sampling:'cleanup'},{pageReleaseWaitPassed:1},{deviceCloseWaitPassed:null}])assert.equal(runner.projectHangupObservation({...base,...patch}).unknown,true);
});
test('挂断投影拒getter和额外Symbol，不执行私有访问器；样本只接受原九布尔',async()=>{
  const page=actualObservationPage();page.initialize();const raw=await page.read(),state=runner.createHangupObservation();state.begin();state.observe(0,raw);const good=state.snapshot();let calls=0;
  for(const field of ['stage','caller','unknown']){const bad={...good};Object.defineProperty(bad,field,{get(){calls++;throw Error('synthetic-private');}});assert.equal(runner.projectHangupObservation(bad).unknown,true);}
  const extra={...good};extra[Symbol('synthetic-private')]=true;assert.equal(runner.projectHangupObservation(extra).unknown,true);
  const sample={...good.caller};sample[Symbol('synthetic-private')]=true;assert.equal(runner.projectHangupObservation({...good,caller:sample}).unknown,true);
  const bad={...raw};Object.defineProperty(bad,'media',{get(){calls++;throw Error('synthetic-private');}});state.observe(0,bad);assert.equal(calls,0);assert.equal(state.snapshot().unknown,true);assert.equal(state.snapshot().caller.terminated,false);
});
test('pagesReleased与最后缓存只记录原位置，不调用页面／媒体／进程资源或在finally补采',async()=>{
  const page=actualObservationPage();page.initialize();const raw=await page.read(),state=runner.createHangupObservation();state.begin();state.observe(0,raw);const before=await page.read();
  state.pagesReleased();assert.equal(state.snapshot().stage,'deviceClose');assert.equal(state.snapshot().deviceCloseWaitPassed,false);assert.deepEqual(await page.read(),before);
  state.completed();vm.runInContext('terminated=true',page.context);state.observe(0,await page.read());assert.equal(state.snapshot().caller.terminated,false);
  const missing=runner.createHangupObservation();missing.begin();missing.pagesReleased();assert.equal(missing.snapshot().caller,null);assert.equal(missing.snapshot().human,null);
});
test('挂断诊断严格默认关闭，失败finally仍不新增eval或采样',async()=>{
  // 原runWithClosedChild执行正式run及原finally，只将外部IO设为受控端口；无真实环境。
  for(const flag of [null,'0','01']){const result=await runWithClosedChild({flag});assert.equal(Object.hasOwn(result.report,'hangupObservation'),false);assert.equal(result.calls.some(v=>v.afterClose&&v.args.includes('eval')),false);}
  const state=runner.createHangupObservation();state.begin();const report=runner.projectReport({phase:'hangup',failureCode:'device_close_timeout',hangupObservation:state.snapshot()});assert.equal(report.failureCode,'device_close_timeout');assert.equal(report.hangupObservation.stage,'pageRelease');
});
