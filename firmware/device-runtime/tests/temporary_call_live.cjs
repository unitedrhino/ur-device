// 文件说明：临时人际通话保留原AI的独立真实驱动；复用共享宿主的身份、产物、PCM与精确进程清理，不套用永久移交Consent。
'use strict';
const assert=require('node:assert/strict'),path=require('node:path');
const {spawn,execFile}=require('node:child_process'),{randomUUID}=require('node:crypto'),{performance}=require('node:perf_hooks');
const shared=require('./shared_handoff_live.cjs');
const root=path.resolve(__dirname,'../../..'); // 仅当前工作区，私有输入仍由隔离宿主交付。
// receipt 校验原HAL已验证并投影的Focus回执；同阶段重复、拒绝或旧修订不能被成功事实遮盖。
function receipt(rows,operation,afterIndex,minRevision=0){
  assert.ok(Array.isArray(rows)&&rows.length<=32&&['suspend','release','restore'].includes(operation));let previous=0;
  for(const row of rows){assert.deepEqual(Object.keys(row).sort(),['code','operation','receiptIndex','revision','state']);
    assert.ok(['acquire','suspend','release','restore'].includes(row.operation)&&Number.isSafeInteger(row.receiptIndex)&&row.receiptIndex>previous&&Number.isSafeInteger(row.revision)&&row.revision>=0);
    previous=row.receiptIndex;assert.ok((row.code===200&&row.state===({acquire:'acquired',suspend:'suspended',release:'released',restore:'restored'})[row.operation])||(Number.isInteger(row.code)&&row.code>=400&&row.code<=599&&row.state==='denied'));
  }
  assert.ok(Number.isSafeInteger(afterIndex)&&afterIndex>=0&&Number.isSafeInteger(minRevision)&&minRevision>=0);
  const current=rows.filter(row=>row.receiptIndex>afterIndex&&row.operation===operation);
  assert.equal(current.length,1,'temporary_receipt_not_unique');const value=current[0];
  assert.ok(value.code===200&&value.revision>minRevision,'temporary_receipt_unconfirmed');return value;
}
// releasedAndIdle 只接受人际原Peer及输入输出实际释放；恢复按钮可用不等于采集已恢复。
function releasedAndIdle(value){assert.ok(value&&value.humanReleased===true&&value.humanInputClosed===true&&value.humanOutputClosed===true&&value.humanStoppedConfirmed===true&&value.captureClosed===true&&value.activeReply===false,'temporary_release_unconfirmed');
  assert.ok(value.temporaryState&&typeof value.temporaryState.canResumeAI==='boolean'&&value.temporaryState.canResumeAI===value.canResumeAI&&value.temporaryState.restoring===false,'temporary_hal_state_unknown');return true;}
// verifyProof 核原模型停流屏障及四个实际焦点回执；空输入没有audioStop ACK，不得虚构第五回执。
function verifyProof(value,stage){
  const fields=['sameOriginal','sameOriginalModel','sameOriginalRuntime','modelRetained','modelStopRequired','modelStopCompleted','modelStoppedAck','suspendedAck','humanAcquiredAck','humanReleasedAck','restoredAck','resumeAllowed','stoppedIntent','revoked','pendingRestore'];
  assert.ok(value&&typeof value==='object'&&!Array.isArray(value)&&Reflect.ownKeys(value).length===fields.length,'temporary_proof_invalid');
  const descriptors=Object.getOwnPropertyDescriptors(value);assert.ok(fields.every(key=>Object.hasOwn(descriptors,key)&&Object.hasOwn(descriptors[key],'value')&&typeof descriptors[key].value==='boolean'),'temporary_proof_invalid');
  value=Object.fromEntries(fields.map(key=>[key,descriptors[key].value])); // 只核自有数据，getter、继承字段与额外Symbol不能冒宿主事实。
  assert.ok(value.sameOriginal&&value.sameOriginalModel&&value.sameOriginalRuntime&&value.modelRetained&&!value.revoked,'temporary_scope_unconfirmed');
  assert.ok(['human','idle','restored','stopped','stopRejected'].includes(stage));
  assert.ok(value.modelStopCompleted&&value.modelStoppedAck===value.modelStopRequired,'temporary_model_stop_unconfirmed');
  assert.ok(value.suspendedAck&&value.humanAcquiredAck,'temporary_acquire_unconfirmed');
  if(stage!=='human')assert.ok(value.humanReleasedAck,'temporary_release_unconfirmed');
  if(stage==='idle')assert.ok(!value.restoredAck&&value.resumeAllowed&&!value.stoppedIntent&&!value.pendingRestore,'automatic_restore_or_denial');
  if(stage==='restored')assert.ok(value.restoredAck&&!value.stoppedIntent&&!value.pendingRestore,'temporary_restore_unconfirmed');
  if(stage==='stopped'||stage==='stopRejected')assert.ok(value.stoppedIntent&&!value.restoredAck&&!value.resumeAllowed&&!value.pendingRestore,'stopped_restore_permitted');return true;
}
// verifySource 保持原四字段闭集、同时间窗及真实完成轮次，不把同sid字符串当模型/runtime不变证明。
function verifySource(value,completedReplies,now=Date.now()){
  assert.ok(value&&Object.keys(value).length===4&&['sourceSessionId','expiresAt','firstTurnIndex','lastTurnIndex'].every(key=>Object.hasOwn(value,key)),'source_unconfirmed');
  assert.ok(typeof value.sourceSessionId==='string'&&Buffer.byteLength(value.sourceSessionId)>0&&Buffer.byteLength(value.sourceSessionId)<=64&&!/[\x00-\x1f\x7f\ud800-\udfff]/u.test(value.sourceSessionId)&&Number.isSafeInteger(value.expiresAt)&&value.expiresAt>now&&value.firstTurnIndex===1&&value.lastTurnIndex===completedReplies,'source_unconfirmed');return value;
}
// verifyNewRound 恢复必须原AI会话、新回复完成且同回复实际有声，不能借历史PCM或另一模型完成凑通过。
function verifyNewRound(originalSession,before,after){assert.equal(after.aiSession,originalSession,'original_ai_changed');
  assert.ok(before.captureClosed&&!before.activeReply&&after.completed===before.completed+1&&!after.activeReply&&after.nonSilent>=before.nonSilent+2&&after.completedID&&after.completedID!==before.completedID&&after.playableID===after.completedID,'temporary_audio_unconfirmed');return true;}
// verifyStopped 本人stop后不得出现restore成功、采集自动启动或新回复；拒绝也不能被错误当成恢复成功。
function verifyStopped(before,after){releasedAndIdle(after);assert.equal(after.canResumeAI,false,'stopped_resume_allowed');
  assert.equal(after.completed,before.completed,'stopped_reply_created');assert.equal(after.nonSilent,before.nonSilent,'stopped_audio_played');
  assert.ok(!after.focusReceipts.some(row=>row.receiptIndex>before.focusIndex&&row.operation==='restore'&&row.code===200),'stopped_restore_accepted');return true;}
// installTemporaryObservation 旁观原唯一incoming/send，完整保留原返回及异常；不补ACK、不接第二消息消费者或改变门控。
function installTemporaryObservation(){
  if(globalThis.__urTemporaryCallObservation)throw Error('temporary_observer_duplicate');const observation={state:null,starts:0,accepted:0};globalThis.__urTemporaryCallObservation=observation;
  const originalIncoming=incoming;incoming=function(value){const result=originalIncoming.apply(this,arguments);if(value?.kind==='temporaryCallState')observation.state={canResumeAI:value.canResumeAI,restoring:value.restoring};return result;};
  const originalSend=send;send=function(value){const result=originalSend.apply(this,arguments);if(value?.kind==='start'){observation.starts++;if(result===true)observation.accepted++;}return result;};
}
// installHumanReleaseObservation 只旁观原close已确认的Stopped发布；旧OutputGated不是停止事实，也不直接改缓存为false。
function installHumanReleaseObservation(){
  if(globalThis.__urTemporaryHumanRelease)throw Error('human_release_observer_duplicate');const observed={scope:null};globalThis.__urTemporaryHumanRelease=observed;
  const original=duplexHumanMedia.publish;duplexHumanMedia.publish=function(value){const result=original.apply(this,arguments);
    if(value?.kind==='humanVoiceStopped'&&this.closed===true&&this.peer?.connectionState==='closed'&&this.scope&&['sessionId','routeGeneration','peerEpoch'].every(key=>value[key]===this.scope[key]))observed.scope={...this.scope};return result;};
}
// closeOwnedBrowsers 逐个关闭本入口登记的session；一端异常仍清另一端，任一拒绝不能被数量检查覆盖。
async function closeOwnedBrowsers(opened,close){assert.ok(Array.isArray(opened)&&new Set(opened).size===opened.length&&opened.every(index=>index===0||index===1)&&typeof close==='function');let success=true;
  for(const index of opened)try{if(await close(index)===false)success=false;}catch{success=false;}return success;
}
// readTemporaryFacts 搭载一次原页面读取，只读HAL已有投影与原Peer，不伪造ACK或修改采集门。
async function readTemporaryFacts(){const base=await READ_SHARED();const human=duplexHumanMedia,peer=human?.peer;
  const released=globalThis.__urTemporaryHumanRelease?.scope,stopped=!!released&&!!human?.scope&&['sessionId','routeGeneration','peerEpoch'].every(key=>released[key]===human.scope[key]);
  return {...base,focusIndex:metrics.focusReceiptIndex,focusReceipts:metrics.focusReceipts.map(value=>({...value})),
    canResumeAI:temporaryResumeAllowed,temporaryState:globalThis.__urTemporaryCallObservation?.state||null,
    inputAttempts:{starts:globalThis.__urTemporaryCallObservation?.starts||0,accepted:globalThis.__urTemporaryCallObservation?.accepted||0},
    humanReleased:!!peer&&peer.connectionState==='closed',humanInputClosed:human?.closed===true&&peer?.connectionState==='closed',humanStoppedConfirmed:stopped,
    humanOutputClosed:stopped&&human.closed===true&&(!human.receiverSink||(human.receiverSink.paused===true&&human.receiverSink.srcObject===null)),inputAttached:base.media.inputAttached,outputEnabled:base.media.outputEnabled};}
// snapshotExpression 使用原导出的页面getter，保持同次观察；不在实际入口开放注入假页面事实。
function snapshotExpression(){return '('+readTemporaryFacts.toString().replace('READ_SHARED()', '('+shared.readPageFacts.toString()+')()')+')()';}
// run 沿原两具名浏览器/两真实CPP生命周期；正常与stop负向为独立模式，各自finally释放原资源。
async function run(env=process.env,observer,options={}){
  assert.equal(env.UR_TEMPORARY_CALL_LIVE,'1','temporary_opt_in_required');const mode=env.UR_TEMPORARY_CALL_VARIANT;
  assert.ok(['resume','stop'].includes(mode),'temporary_variant_required');assert.ok(observer&&typeof observer.resolveSource==='function'&&typeof observer.readTemporaryProof==='function','formal_temporary_observer_required');
  assert.equal(env.SAAS_RESOURCE_GUARD_HELD,'1','actual_resource_guard_required');
  const window=shared.validateObservationWindow(observer.observationWindow),config=shared.validateEnvironment(env);
  const deadline=performance.now()+Math.min(180000,window.createdBefore-Date.now()),sessions=['temporary-ai-'+randomUUID(),'temporary-human-'+randomUUID()],children=[],opened=[];
  let cancelled=false,phase='input',passed=false;const cleanup={devicesClosed:false,browsersClosed:false};
  const cancel=()=>{cancelled=true;};process.on('SIGTERM',cancel);process.on('SIGINT',cancel);
  const guard=()=>{assert.ok(!cancelled&&performance.now()<deadline,'run_interrupted');assert.ok(!children[0]?.closed,'device_ended_early');};
  const browser=(index,...args)=>new Promise((resolve,reject)=>{guard();execFile('agent-browser',['--session',sessions[index],'--headed',...args],{env:{...env,AGENT_BROWSER_ARGS:['--no-sandbox','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream','--use-file-for-fake-audio-capture='+env.UR_SHARED_HANDOFF_AUDIO_FIXTURE,'--autoplay-policy=no-user-gesture-required'].join(',')},timeout:Math.max(1,Math.floor(Math.min(15000,deadline-performance.now()))),maxBuffer:65536},(error,stdout)=>error?reject(Error('browser_command_failed')):resolve(stdout.trim()));});
  const snapshot=async index=>shared.parseJSON(await browser(index,'eval',snapshotExpression())),wait=read=>shared.waitFor(read,deadline,{cancelled:()=>cancelled});
  const source=async completedReplies=>{const identity={tenantCode:config.data.tenantCode,ownerUserId:config.data.ownerUserId,subject:'device:'+config.data.caller.productID+':'+config.data.caller.deviceName,...window,deviceEstablished:true,completedReplies};
    return wait(async()=>{const value=await shared.budgetCall(()=>observer.resolveSource(identity),deadline);return value===null?false:verifySource(value,completedReplies);});};
  // proof 只在同一时间窗核原来源和两个实际物理端点；具体SQL/RPC观察由隔离宿主提供，驱动不复制后台协议。
  const proof=async(originalSession,stage)=>wait(async()=>{const value=await shared.budgetCall(()=>observer.readTemporaryProof({sourceSessionId:originalSession,subject:'device:'+config.data.caller.productID+':'+config.data.caller.deviceName,targetSubject:'device:'+config.data.human.productID+':'+config.data.human.deviceName,...window}),deadline);return value===null?false:verifyProof(value,stage);});
  try{
    shared.verifyOrdinarySelf(await shared.budgetCall(()=>shared.apiRequest(config.data.api,config.data.tenantCode,'system/user/self/get-one',{withTenant:true}),deadline),config.data.tenantCode,config.data.ownerUserId);
    phase='boot';for(let index=0;index<2;index++){const device=index?config.data.human:config.data.caller,args=[String(config.ports[index]),...(index?['--human']:[])];
      const child=spawn(config.binary,args,{cwd:root,env:{...env,UR_DEVICE_TEMPORARY_CALL:index?'':'1',UR_DEVICE_PRODUCT:device.productID,UR_DEVICE_NAME:device.deviceName,UR_DEVICE_SECRET:device.secret},stdio:['ignore','pipe','pipe']});
      const held=shared.trackChild(child,config.binary,root,[config.binary,...args]);children.push(held);child.stderr.on('data',()=>{});let ready=false,pending='';child.stdout.on('data',bytes=>{pending+=bytes.toString();if(pending.length>8192){pending='';return;}let end;while((end=pending.indexOf('\n'))>=0){if(pending.slice(0,end)==='http://127.0.0.1:'+config.ports[index]+'/')ready=true;pending=pending.slice(end+1);}});
      await shared.budgetCall(()=>held.spawned,deadline);shared.captureChild(held);if(options.onSpawn)await shared.budgetCall(()=>options.onSpawn(child,held.record),deadline);await wait(()=>ready);opened.push(index);if(options.onBrowserSession)await shared.budgetCall(()=>options.onBrowserSession(sessions[index]),deadline);await browser(index,'open','http://127.0.0.1:'+config.ports[index]+'/');if(index===0)await browser(index,'eval','('+installTemporaryObservation.toString()+')()');await browser(index,'click','#activate');}
    phase='ready';await wait(async()=>shared.requireReadyPages(await Promise.all([snapshot(0),snapshot(1)])));for(let i=0;i<2;i++)await browser(i,'eval','('+shared.installPageObservation.toString()+')()');await browser(0,'eval','('+installHumanReleaseObservation.toString()+')()');
    phase='aiAudio';await wait(async()=>{const v=await snapshot(0);return v.captureClosed&&!v.activeReply;});const initial=await snapshot(0);
    await browser(0,'fill','#textInput',shared.fixedInput);await browser(0,'check','#textAudio');await browser(0,'click','#sendText');await wait(async()=>{try{return shared.verifyModelRound(initial,await snapshot(0));}catch{return false;}});const original=await source(1),beforeCall=await snapshot(0);
    phase='dial';await browser(0,'select','#handoffTargetKind','device');await browser(0,'fill','#handoffTarget',config.data.human.productID+':'+config.data.human.deviceName);await browser(0,'uncheck','#handoffShareContext');await browser(0,'click','#handoffEscalate');
    await wait(async()=>shared.parseJSON(await browser(1,'eval','JSON.stringify({enabled:!document.getElementById("handoffAccept").disabled})')).enabled);await browser(1,'click','#handoffAccept');
    phase='suspended';await wait(async()=>{try{return receipt((await snapshot(0)).focusReceipts,'suspend',beforeCall.focusIndex);}catch{return false;}});
    phase='humanAudio';const first=await wait(async()=>{const v=await Promise.all([snapshot(0),snapshot(1)]);return v.every(x=>x.media.connected&&x.media.inputAttached&&x.media.outputEnabled)&&v.map(x=>x.media);});await wait(async()=>{try{return shared.verifyAudioPair(first,(await Promise.all([snapshot(0),snapshot(1)])).map(x=>x.media));}catch{return false;}});
    await proof(original.sourceSessionId,'human');
    if(mode==='stop'){phase='userStop';await browser(0,'click','#stop');}
    phase='humanEnd';const beforeEnd=await snapshot(0);await browser(0,'click','#handoffHangup');await wait(async()=>{try{const v=await snapshot(0);receipt(v.focusReceipts,'release',beforeEnd.focusIndex);return releasedAndIdle(v);}catch{return false;}});
    await wait(async()=>{const v=await snapshot(1);return v.released;});await shared.budgetCall(()=>children[1].originalClose.wait(),deadline);assert.ok(children[1].closed&&children[1].code===0,'human_exit_failed');
    const idle=await snapshot(0);releasedAndIdle(idle);await proof(original.sourceSessionId,mode==='stop'?'stopped':'idle');releasedAndIdle(await snapshot(0));
    if(mode==='stop'){phase='stopNoRestore';verifyStopped(idle,await snapshot(0));const delivered=shared.parseJSON(await browser(0,'eval','JSON.stringify({accepted:send({kind:"start"})})'));assert.equal(delivered.accepted,true,'new_input_not_delivered');
      // send只证原本地WebSocket成功投递，不称云端接收或native拒绝ACK；原Stopped/无Pending及持续关闭共同证明不恢复。
      await proof(original.sourceSessionId,'stopRejected');const sent=await snapshot(0);assert.equal(sent.inputAttempts.starts,idle.inputAttempts.starts+1);assert.equal(sent.inputAttempts.accepted,idle.inputAttempts.accepted+1);verifyStopped(idle,sent);
      await new Promise(resolve=>setTimeout(resolve,400));verifyStopped(idle,await snapshot(0));await proof(original.sourceSessionId,'stopRejected');}
    else{phase='explicitInput';assert.equal(idle.canResumeAI,true,'resume_not_available');await browser(0,'click','#temporaryResumeAI');
      await wait(async()=>{try{return receipt((await snapshot(0)).focusReceipts,'restore',idle.focusIndex);}catch{return false;}});
      await proof(original.sourceSessionId,'restored');phase='restoredAudio';await wait(async()=>{const v=await snapshot(0);try{return verifyNewRound(original.sourceSessionId,idle,{...v,aiSession:original.sourceSessionId});}catch{return false;}});const restored=await source(2);assert.equal(restored.sourceSessionId,original.sourceSessionId,'original_ai_changed');}
    passed=true;phase='completed';
  }finally{
    cleanup.browsersClosed=await closeOwnedBrowsers(opened,index=>new Promise((resolve,reject)=>execFile('agent-browser',['--session',sessions[index],'close'],{env,timeout:10000,maxBuffer:4096},e=>e?reject(Error('browser_close_failed')):resolve())));
    let devicesClosed=true;for(const held of children)try{await shared.releaseChild(held);}catch{devicesClosed=false;}cleanup.devicesClosed=devicesClosed;
    process.removeListener('SIGTERM',cancel);process.removeListener('SIGINT',cancel);const report={passed:passed&&cleanup.devicesClosed&&cleanup.browsersClosed,mode,phase,cleanup,actualModelRun:passed,actualCPP:children.length===2,nativeRejectionAckVerified:false,stopLocalPolicyNoRestoreVerified:passed&&mode==='stop'};if(options.report)options.report(report);assert.ok(report.passed,'temporary_call_live_failed');
  }
  return {passed:true,mode,phase,cleanup};
}
module.exports={receipt,releasedAndIdle,verifyProof,verifySource,verifyNewRound,verifyStopped,installTemporaryObservation,installHumanReleaseObservation,closeOwnedBrowsers,snapshotExpression,run};
// CLI缺正式来源observer时明确拒绝，不从自由模块或历史报告加载替代事实。
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(()=>{console.error(JSON.stringify({passed:false,reason:'temporary_call_live_failed'}));process.exitCode=1;});
