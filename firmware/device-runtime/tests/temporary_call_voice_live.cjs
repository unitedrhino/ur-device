// 文件说明：原生Realtime语音先行临时通话驱动；复用原接续契约与真实WAV/CPP，不实现NativeText或替代模型。
'use strict';
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
const {spawn,execFile}=require('node:child_process'),{randomUUID}=require('node:crypto'),{performance}=require('node:perf_hooks');
const shared=require('./shared_handoff_live.cjs'),temporary=require('./temporary_call_live.cjs'),control=require('./browser_ai_control_live.cjs');
const {prepareBrowserClick,verifyBrowserClick}=require('./browser_click.cjs'); // 原真实坐标点击前确认视口与命中。
const {receipt,releasedAndIdle,verifyProof,verifySource,verifyNewRound,verifyStopped,installTemporaryObservation,installHumanReleaseObservation,closeOwnedBrowsers}=temporary;
const root=path.resolve(__dirname,'../../..'); // 实际工作区与原私有输入作用域不变。

// validateNativeVoice 精确要求已授权语音链路和非空固定语料关键词；未声明模式不猜半/全双工。
function validateNativeVoice(env){
 assert.equal(env.UR_TEMPORARY_NATIVE_VOICE_LIVE,'1','native_voice_opt_in_required');
 const mode=env.UR_TEMPORARY_NATIVE_INTERACTION_MODE;
 assert.ok(['halfDuplex','fullDuplex'].includes(mode),'native_actual_mode_required');
 // 本地策略独立于预期模式；只转发本人显式0/1，不因想验收half而擅改能力或AEC。
 assert.ok(env.UR_DEVICE_HALF_DUPLEX_ONLY===undefined||['0','1'].includes(env.UR_DEVICE_HALF_DUPLEX_ONLY),'native_half_duplex_policy_invalid');
 const keywords=(env.UR_BROWSER_EXPECT_ASR_KEYWORDS||'').split(',').filter(Boolean);
 assert.ok(keywords.length>0&&keywords.length<=10&&keywords.every(value=>value.length<=64&&!/[\r\n\x00]/.test(value)),'native_fixed_keywords_required');
 return {mode,keywords};
}

// verifyNativeMode 核原sessionCreated实际回写；可选观察回调在断言前只投影闭集模式，失败报告也保留实际协商事实。
function verifyNativeMode(value,expected,onObserved){const actual=['halfDuplex','fullDuplex'].includes(value?.interactionMode)?value.interactionMode:'unknown';if(typeof onObserved==='function')onObserved(actual);assert.ok(['halfDuplex','fullDuplex'].includes(expected));assert.equal(actual,expected,'native_negotiated_mode_mismatch');return true;}

// installNativeObservation 只旁观真实本人Stop投递与原控制观察，不额外消费者、假ACK或采集门赋值。
function installNativeObservation(){
 if(globalThis.__urTemporaryNativeVoice)throw Error('native_voice_observer_duplicate');
 globalThis.__urTemporaryNativeVoice={userStops:0,dialAttempts:0,dialSubmitted:0};
 const original=send;send=function(value){const result=original.apply(this,arguments);if(value?.kind==='stop')++globalThis.__urTemporaryNativeVoice.userStops;if(value?.kind==='handoffControl'&&value.operation==='dial'){++globalThis.__urTemporaryNativeVoice.dialAttempts;if(result===true)++globalThis.__urTemporaryNativeVoice.dialSubmitted;}return result;};
}

// nativeSnapshot 搭载同次原临时getter与控制音频getter；输入ID仅内存比较，不写报告或假造新代次。
function snapshotExpression(includeVoice=true,observeStop=false){const base=includeVoice?'(async()=>{const base=await '+temporary.snapshotExpression()+';const voice=await ('+control.readFacts.toString()+')();return {...base,interactionMode:metrics.interactionMode,voice:{...voice,lastInput:globalThis.__urAIControl.lastInput,userStops:globalThis.__urTemporaryNativeVoice.userStops}};})()':temporary.snapshotExpression();return observeStop?'(async()=>{const base=await '+base+';return {...base,audioSnapshot:('+shared.readHumanAudioSnapshot.toString()+')()};})()':base;}

// verifyNativeRound 复用原解码/识别/同回复终态标准；全双工不以用户Stop完成轮次，半双工保留原start/stop。
function verifyNativeRound(before,after,mode){
 verifyNativeMode(before,mode);verifyNativeMode(after,mode);control.verifyAudio(before.voice,after.voice,'udp');
 assert.equal(after.voice.fixtureEnded,before.voice.fixtureEnded+1,'native_fixture_not_completed');
 assert.equal(after.voice.starts,before.voice.starts+1,'native_new_start_unconfirmed');
 assert.ok(typeof after.voice.lastInput==='string'&&after.voice.lastInput!==''&&after.voice.lastInput!==before.voice.lastInput,'native_old_input_reused');
 assert.equal(after.voice.captureOpen,mode==='fullDuplex','native_capture_policy_mismatch');
 assert.equal(after.captureClosed,mode!=='fullDuplex','native_capture_fact_mismatch');
 assert.equal(after.voice.userStops,before.voice.userStops+(mode==='halfDuplex'?1:0),'native_user_stop_policy_mismatch');
 return true;
}

// projectNativeTimingRound 接收轮号1/2及该轮原getter事实，返回脱敏同钟时间差或不可测原因；不借页面耗时补测。
function projectNativeTimingRound(round,voice){
 assert.ok(Number.isInteger(round)&&round>=1&&round<=2,'native_timing_round_invalid');
 assert.ok(voice&&typeof voice==='object'&&!Array.isArray(voice),'native_timing_facts_invalid');
 return {round,stage:round===1?'initial':'restored',...control.verifyReplyTiming(voice,'udp')};
}

// verifyNativeSuspended 全双工只能由正式平台屏障暂停；本人Stop不能伪造连续输入场景。
function verifyNativeSuspended(before,after,mode){
 verifyNativeMode(before,mode);verifyNativeMode(after,mode);assert.equal(after.voice.captureOpen,false,'native_platform_capture_not_closed');assert.equal(after.captureClosed,true);
 assert.equal(after.voice.userStops,before.voice.userStops,'native_user_stop_replaced_platform_pause');
 if(mode==='fullDuplex')assert.equal(before.voice.captureOpen,true,'native_full_duplex_not_continuous');
 return true;
}

// verifyNativeProof 仅全双工另核原活动输入真实停止ACK；其余字段仍由唯一temporary.verifyProof维护。
function verifyNativeProof(value,stage,mode){verifyProof(value,stage);if(mode==='fullDuplex')assert.ok(value.modelStopRequired&&value.modelStoppedAck,'native_continuous_input_stop_ack_missing');return true;}
// projectBrowserOperation 只保留固定操作与UI控件；eval正文、填写目标、地址和参数均不进入报告。
function projectBrowserOperation(index,operation,selector){return {side:index===0?'caller':index===1?'human':'unknown',operation:['open','eval','click','select','fill','uncheck'].includes(operation)?operation:'unknown',selector:['#activate','#start','#stop','#temporaryResumeAI','#handoffTargetKind','#handoffTarget','#handoffShareContext','#handoffEscalate','#handoffAccept','#handoffHangup'].includes(selector)?selector:'unknown'};}
// readDialControls 读取正式页面固定控制实态；只输出布尔，不读目标、分享正文或status文案。
function readDialControls(){return {terminated:terminated===true,canEscalate:handoffState?.canEscalate===true,dialEnabled:document.getElementById('handoffEscalate')?.disabled===false,targetKindEnabled:document.getElementById('handoffTargetKind')?.disabled===false,targetEnabled:document.getElementById('handoffTarget')?.disabled===false,captureOpen:metrics.captureOpen===true,activeReply:responseId!==''};}
// readDialProgress 只旁观原页面发送返回与宿主状态；本地投递不冒充MQTT发布或平台创建应答。
function readDialProgress(){const observed=globalThis.__urTemporaryNativeVoice;return {state:Number.isInteger(handoffState?.state)&&handoffState.state>=0&&handoffState.state<=10?handoffState.state:null,canEscalate:handoffState?.canEscalate===true,dialAttempts:observed.dialAttempts,dialSubmitted:observed.dialSubmitted};}
// projectDialTiming 只记录原浏览器命令的单调耗时与固定结果，不读取目标、脚本或自由异常正文。
function projectDialTiming(index,operation,selector,elapsed,failed){return {...projectBrowserOperation(index,operation,selector),elapsedMs:Number.isFinite(elapsed)&&elapsed>=0?Math.floor(elapsed):null,failed:failed===true};}
// guardedRead 在业务谓词的局部catch外检查进程生命，防止致命退出被吞成继续等待的false。
function guardedRead(guard,read){guard();return read();}
// finishNativeReport 在资源finally后报告闭集结果；已有原异常时报告失败不能再次覆盖它。
function finishNativeReport(report,failureRaised,reporter){try{if(reporter)reporter(report);}catch(error){if(!failureRaised)throw error;}if(!failureRaised)assert.ok(report.passed,'temporary_call_live_failed');}
// run 沿原两具名浏览器/两真实CPP生命周期；正常与stop负向为独立模式，各自finally释放原资源。
async function run(env=process.env,observer,options={}){
  const voiceSettings=validateNativeVoice(env);const interactionMode=voiceSettings.mode;
  assert.equal(env.UR_TEMPORARY_CALL_LIVE,'1','temporary_opt_in_required');const mode=env.UR_TEMPORARY_CALL_VARIANT;
  assert.ok(['resume','stop'].includes(mode),'temporary_variant_required');assert.ok(observer&&typeof observer.resolveSource==='function'&&typeof observer.readTemporaryProof==='function','formal_temporary_observer_required');
  assert.equal(env.SAAS_RESOURCE_GUARD_HELD,'1','actual_resource_guard_required');
  const window=shared.validateObservationWindow(observer.observationWindow),config=shared.validateEnvironment(env);
  const voiceBytes=fs.readFileSync(env.UR_SHARED_HANDOFF_AUDIO_FIXTURE);assert.ok(voiceBytes.length>44&&voiceBytes.length<=320044,'native_fixed_wave_invalid');
  const deadline=performance.now()+Math.min(180000,window.createdBefore-Date.now()),sessions=['temporary-ai-'+randomUUID(),'temporary-human-'+randomUUID()],children=[],opened=[];
  // voiceTimings最多保存两条已验证原语音轮；不保留回复标识或在finally用清理后状态补造计时。
  let cancelled=false,phase='input',passed=false,verifiedVoiceRounds=0,actualInteractionMode='unknown',failureRaised=false,failureCode=null,lastBrowserOperation=projectBrowserOperation(),dialObservation=null,dialProgress=null,dialTimingOverflow=false;const dialTimings=[],clickObservations=[],voiceTimings=[],cleanup={devicesClosed:false,browsersClosed:false};
  const cancel=()=>{cancelled=true;};process.on('SIGTERM',cancel);process.on('SIGINT',cancel);
  const guard=()=>{assert.ok(!cancelled&&performance.now()<deadline,'run_interrupted');assert.ok(!children[0]?.closed,'device_ended_early');if(['dial','suspended','humanAudio'].includes(phase))assert.ok(!children[1]?.closed,'device_ended_early');};
  const executeBrowser=(index,...args)=>new Promise((resolve,reject)=>{guard();lastBrowserOperation=projectBrowserOperation(index,args[0],args[1]);const started=performance.now(),observeDial=phase==='dial';execFile('agent-browser',['--session',sessions[index],'--headed',...args],{env:{...env,AGENT_BROWSER_ARGS:['--no-sandbox','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream','--use-file-for-fake-audio-capture='+env.UR_SHARED_HANDOFF_AUDIO_FIXTURE,'--autoplay-policy=no-user-gesture-required'].join(',')},timeout:Math.max(1,Math.floor(Math.min(15000,deadline-performance.now()))),maxBuffer:65536},(error,stdout)=>{if(observeDial){if(dialTimings.length<32)dialTimings.push(projectDialTiming(index,args[0],args[1],performance.now()-started,!!error));else dialTimingOverflow=true;}return error?reject(Error('browser_command_failed')):resolve(stdout.trim());});});
  const browser=async(index,...args)=>{if(args[0]==='click'){const facts=shared.parseJSON(await executeBrowser(index,'eval','('+prepareBrowserClick.toString()+')('+JSON.stringify(args[1])+')'));verifyBrowserClick(facts);assert.ok(clickObservations.length<32,'browser_click_observation_limit');clickObservations.push({...projectBrowserOperation(index,args[0],args[1]),...facts});}return executeBrowser(index,...args);};
  const observeStop=env.UR_DEVICE_CONTEXT_DIAGNOSTIC==='1',audioSamples=[null,null]; // 仅原业务getter缓存，失败后不补读或猜停止原因。
  const snapshot=async index=>{const value=shared.parseJSON(await browser(index,'eval',snapshotExpression(index===0,observeStop)));if(observeStop){const sample=shared.projectHumanAudioSnapshot(value.audioSnapshot);if(sample)audioSamples[index]=sample;}return value;},wait=read=>shared.waitFor(()=>guardedRead(guard,read),deadline,{cancelled:()=>cancelled});
  const source=async completedReplies=>{const identity={tenantCode:config.data.tenantCode,ownerUserId:config.data.ownerUserId,subject:'device:'+config.data.caller.productID+':'+config.data.caller.deviceName,...window,deviceEstablished:true,completedReplies};
    return wait(async()=>{const value=await shared.budgetCall(()=>observer.resolveSource(identity),deadline);return value===null?false:verifySource(value,completedReplies);});};
  // proof 只在同一时间窗核原来源和两个实际物理端点；具体SQL/RPC观察由隔离宿主提供，驱动不复制后台协议。
  const proof=async(originalSession,stage)=>wait(async()=>{const value=await shared.budgetCall(()=>observer.readTemporaryProof({sourceSessionId:originalSession,subject:'device:'+config.data.caller.productID+':'+config.data.caller.deviceName,targetSubject:'device:'+config.data.human.productID+':'+config.data.human.deviceName,...window}),deadline);return value===null?false:verifyNativeProof(value,stage,interactionMode);});
  try{await shared.captureFailure(async()=>{
    shared.verifyOrdinarySelf(await shared.budgetCall(()=>shared.apiRequest(config.data.api,config.data.tenantCode,'system/user/self/get-one',{withTenant:true}),deadline),config.data.tenantCode,config.data.ownerUserId);
    phase='boot';for(let index=0;index<2;index++){const device=index?config.data.human:config.data.caller,args=[String(config.ports[index]),...(index?['--human']:[])];
      const child=spawn(config.binary,args,{cwd:root,env:{...env,UR_DEVICE_TEMPORARY_CALL:index?'':'1',UR_DEVICE_PRODUCT:device.productID,UR_DEVICE_NAME:device.deviceName,UR_DEVICE_SECRET:device.secret},stdio:['ignore','pipe','pipe']});
      const held=shared.trackChild(child,config.binary,root,[config.binary,...args]);children.push(held);child.stderr.on('data',()=>{});let ready=false,pending='';child.stdout.on('data',bytes=>{pending+=bytes.toString();if(pending.length>8192){pending='';return;}let end;while((end=pending.indexOf('\n'))>=0){if(pending.slice(0,end)==='http://127.0.0.1:'+config.ports[index]+'/')ready=true;pending=pending.slice(end+1);}});
      await shared.budgetCall(()=>held.spawned,deadline);shared.captureChild(held);if(options.onSpawn)await shared.budgetCall(()=>options.onSpawn(child,held.record),deadline);await wait(()=>ready);opened.push(index);if(options.onBrowserSession)await shared.budgetCall(()=>options.onBrowserSession(sessions[index]),deadline);await browser(index,'open','http://127.0.0.1:'+config.ports[index]+'/');if(index===0){await browser(index,'eval','('+installTemporaryObservation.toString()+')()');await browser(index,'eval','('+control.installObserver.toString()+')()');await browser(index,'eval','('+installNativeObservation.toString()+')()');}await browser(index,'click','#activate');}
    phase='ready';await wait(async()=>shared.requireReadyPages(await Promise.all([snapshot(0),snapshot(1)])));for(let i=0;i<2;i++)await browser(i,'eval','('+shared.installPageObservation.toString()+')('+String(observeStop)+')');await browser(0,'eval','('+installHumanReleaseObservation.toString()+')()');
    const initial=await snapshot(0);verifyNativeMode(initial,interactionMode,actual=>{actualInteractionMode=actual;});
    await browser(0,'eval','globalThis.__urAIControl.keywords='+JSON.stringify(voiceSettings.keywords)+';globalThis.__urFixtureChunks=[];true');
    const encoded=voiceBytes.toString('base64');for(let offset=0;offset<encoded.length;offset+=24000)await browser(0,'eval','globalThis.__urFixtureChunks.push('+JSON.stringify(encoded.slice(offset,offset+24000))+');true');
    await browser(0,'eval','('+control.installFixture.toString()+')()');
    // audioRound沿既有endpoint驱动顺序；全双工不以本人Stop伪造平台暂停，恢复按钮本身已发送新Start。
    const audioRound=async(before,startAlreadyRequested)=>{if(!startAlreadyRequested)await browser(0,'click','#start');await wait(async()=>(await snapshot(0)).voice.captureOpen===true);
      await browser(0,'eval','globalThis.__urControlPlay();true');await wait(async()=>(await snapshot(0)).voice.fixtureEnded===before.voice.fixtureEnded+1);
      if(interactionMode==='halfDuplex')await browser(0,'click','#stop');
      const after=await wait(async()=>{const value=await snapshot(0);try{return verifyNativeRound(before,value,interactionMode)&&value;}catch{return false;}});
      voiceTimings.push(projectNativeTimingRound(verifiedVoiceRounds+1,after.voice));
      ++verifiedVoiceRounds;return after;};
    phase='aiAudio';await audioRound(initial,false);const original=await source(1),beforeCall=await snapshot(0);
    phase='dial';dialObservation=shared.parseJSON(await browser(0,'eval','('+readDialControls.toString()+')()'));await browser(0,'select','#handoffTargetKind','device');await browser(0,'fill','#handoffTarget',config.data.human.productID+':'+config.data.human.deviceName);await browser(0,'uncheck','#handoffShareContext');await browser(0,'click','#handoffEscalate');
    await wait(async()=>{dialProgress=shared.parseJSON(await browser(0,'eval','('+readDialProgress.toString()+')()'));assert.equal(dialProgress.dialAttempts,1,'dial_local_not_attempted');assert.equal(dialProgress.dialSubmitted,1,'dial_local_not_submitted');return shared.parseJSON(await browser(1,'eval','JSON.stringify({enabled:!document.getElementById("handoffAccept").disabled})')).enabled;});await browser(1,'click','#handoffAccept');
    phase='suspended';await wait(async()=>{try{const values=await Promise.all([snapshot(0),snapshot(1)]);return receipt(values[0].focusReceipts,'suspend',beforeCall.focusIndex);}catch{return false;}});
    verifyNativeSuspended(beforeCall,await snapshot(0),interactionMode);
    phase='humanAudio';const first=await wait(async()=>{const v=await Promise.all([snapshot(0),snapshot(1)]);return v.every(x=>x.media.connected&&x.media.inputAttached&&x.media.outputEnabled)&&v.map(x=>x.media);});await wait(async()=>{try{return shared.verifyAudioPair(first,(await Promise.all([snapshot(0),snapshot(1)])).map(x=>x.media));}catch{return false;}});
    await proof(original.sourceSessionId,'human');
    // 独立长保持验收可在原人际实态已通过后观察；缺省无等待、不改变原180秒截止或媒体门控。
    if(options.holdOriginal)await options.holdOriginal({sourceSessionId:original.sourceSessionId,deadline,guard,readPair:()=>Promise.all([snapshot(0),snapshot(1)]),verifyHuman:()=>proof(original.sourceSessionId,'human')});
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
      await proof(original.sourceSessionId,'restored');phase='restoredAudio';const voiced=await audioRound(idle,true);const restored=await source(2);assert.equal(restored.sourceSessionId,original.sourceSessionId,'original_ai_changed');verifyNewRound(original.sourceSessionId,idle,{...voiced,aiSession:restored.sourceSessionId});}
    // 原恢复音频及来源均已确认后才允许核对原Provider实例；观察不代替既有解码或恢复判据。
    if(options.verifyOriginalProvider)await options.verifyOriginalProvider({sourceSessionId:original.sourceSessionId,deadline,guard});
    passed=true;phase='completed';
  },code=>{failureRaised=true;failureCode=code;});
  }finally{
    cleanup.browsersClosed=await closeOwnedBrowsers(opened,index=>new Promise((resolve,reject)=>execFile('agent-browser',['--session',sessions[index],'close'],{env,timeout:10000,maxBuffer:4096},e=>e?reject(Error('browser_close_failed')):resolve())));
    let devicesClosed=true;for(const held of children)try{await shared.releaseChild(held);}catch{devicesClosed=false;}cleanup.devicesClosed=devicesClosed;
    process.removeListener('SIGTERM',cancel);process.removeListener('SIGINT',cancel);const report={passed:passed&&cleanup.devicesClosed&&cleanup.browsersClosed,inputMode:'voiceFirst',expectedInteractionMode:interactionMode,interactionMode:actualInteractionMode,verifiedVoiceRounds,voiceTimings,mode,phase,failureCode,lastBrowserOperation,dialObservation,dialProgress,dialTimings,dialTimingOverflow,clickObservations,...(observeStop?{humanAudioObservation:shared.cachedHumanAudioObservation(audioSamples)}:{}),cleanup,actualModelRun:passed,actualCPP:children.length===2,nativeRejectionAckVerified:false,stopLocalPolicyNoRestoreVerified:passed&&mode==='stop'};finishNativeReport(report,failureRaised,options.report);
  }
  return {passed:true,inputMode:'voiceFirst',interactionMode:actualInteractionMode,verifiedVoiceRounds,voiceTimings,mode,phase,cleanup};
}

module.exports={validateNativeVoice,verifyNativeMode,installNativeObservation,snapshotExpression,verifyNativeRound,projectNativeTimingRound,verifyNativeSuspended,verifyNativeProof,projectBrowserOperation,readDialControls,readDialProgress,projectDialTiming,guardedRead,finishNativeReport,run};
// CLI不能从任意模块注入模型/SQL事实；缺正式原observer在启动资源前拒绝。
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(()=>{console.error(JSON.stringify({passed:false,reason:'temporary_native_voice_failed'}));process.exitCode=1;});
