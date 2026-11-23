// 文件说明：原生语音接续的严格离线判据；执行正式页面getter，不将模拟事实声称真实API验收。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm');
const runner=require('./temporary_call_voice_live.cjs');
const shared=require('./shared_handoff_live.cjs'); // 原闭集异常投影；新驱动不维护第二份自由异常分类。
// pair 提供同回复完整UDP解码和真实新输入的单测事实，错误字段由各回归用例独立覆盖。
function pair(mode){const before={interactionMode:mode,captureClosed:true,voice:{completed:0,completedID:'',voiced:0,recognized:0,fixtureEnded:0,starts:0,lastInput:'',userStops:0}};
 const after={interactionMode:mode,captureClosed:mode==='halfDuplex',voice:{completed:1,completedID:'reply',playableID:'reply',voiced:2,recognized:1,recognizedSameReply:true,keywordMatched:true,outputStopped:true,network:2,decoded:2,received:0,missingPackets:0,observedComplete:true,completedReplyMediaFinal:true,replyTiming:null,fixtureEnded:1,starts:1,lastInput:'input',userStops:mode==='halfDuplex'?1:0,captureOpen:mode==='fullDuplex'}};
 return {before,after};}
// proof 使用原temporary严格闭集，只有全双工额外要求实际活动输入Stop ACK。
function proof(){return {sameOriginal:true,sameOriginalModel:true,sameOriginalRuntime:true,modelRetained:true,modelStopRequired:true,modelStopCompleted:true,modelStoppedAck:true,suspendedAck:true,humanAcquiredAck:true,humanReleasedAck:true,restoredAck:false,resumeAllowed:true,stoppedIntent:false,revoked:false,pendingRestore:false};}
test('原生语音两轮分别投影同C++时钟延迟，不把整轮耗时当首音',()=>{
 // first/second分别是初轮与恢复轮的私有同钟样本；原标识只作脱敏哨兵。
 const first=runner.projectNativeTimingRound(1,{replyTiming:{lastVoiceSentMs:1000,firstTextMs:1100,firstNetworkMs:1250,firstPlayableMs:1270},completedID:'private-reply',lastInput:'private-input'});
 const second=runner.projectNativeTimingRound(2,{replyTiming:{lastVoiceSentMs:9000,firstTextMs:9400,firstNetworkMs:9500,firstPlayableMs:9520}});
 assert.equal(first.round,1);assert.equal(first.stage,'initial');assert.equal(second.stage,'restored');
 assert.equal(first.firstTextMilliseconds,100);assert.equal(first.firstNetworkAudioMilliseconds,250);assert.equal(first.firstPlayableAudioMilliseconds,270);
 assert.equal(second.firstPlayableAudioMilliseconds,520);assert.equal(first.clock,'cpp_monotonic_ms');
 assert.equal(first.physicalSpeakerOnsetMeasured,false);assert.equal(first.primaryMeasured,true);
 assert.ok(!JSON.stringify(first).includes('private-'));assert.equal(first.firstPlayableAudioMilliseconds,270);
});
test('缺计时或合法重叠明确不可测，畸形计时拒绝而不补网页时间',()=>{
 // absent/overlap分别验证缺锚与合法实时重叠，不推定网络或设备播放失败。
 const absent=runner.projectNativeTimingRound(1,{replyTiming:null});
 assert.equal(absent.primaryMeasured,false);assert.equal(absent.firstPlayableAudioMilliseconds,null);
 const overlap=runner.projectNativeTimingRound(2,{replyTiming:{lastVoiceSentMs:200,firstTextMs:150,firstNetworkMs:160,firstPlayableMs:180}});
 assert.equal(overlap.primaryMeasured,false);assert.equal(overlap.unavailable.firstPlayableAudioMilliseconds,'overlapped_response');
 for(const round of [0,3,NaN,1.5])assert.throws(()=>runner.projectNativeTimingRound(round,{}));
 assert.throws(()=>runner.projectNativeTimingRound(1,{replyTiming:{lastVoiceSentMs:1,firstTextMs:2,firstNetworkMs:4,firstPlayableMs:3}}));
});
test('缺原生开关、模式、语料或正式观察器均在资源启动前拒绝',async()=>{
 const valid={UR_TEMPORARY_NATIVE_VOICE_LIVE:'1',UR_TEMPORARY_NATIVE_INTERACTION_MODE:'fullDuplex',UR_BROWSER_EXPECT_ASR_KEYWORDS:'天气'};
 assert.deepEqual(runner.validateNativeVoice(valid),{mode:'fullDuplex',keywords:['天气']});
 for(const value of ['0','1'])assert.deepEqual(runner.validateNativeVoice({...valid,UR_DEVICE_HALF_DUPLEX_ONLY:value}),{mode:'fullDuplex',keywords:['天气']});
 for(const value of ['','true',' 1','2'])assert.throws(()=>runner.validateNativeVoice({...valid,UR_DEVICE_HALF_DUPLEX_ONLY:value}),/native_half_duplex_policy_invalid/);
 for(const extra of [{UR_TEMPORARY_NATIVE_VOICE_LIVE:''},{UR_TEMPORARY_NATIVE_INTERACTION_MODE:'auto'},{UR_BROWSER_EXPECT_ASR_KEYWORDS:''},{UR_BROWSER_EXPECT_ASR_KEYWORDS:'私有\n正文'}])assert.throws(()=>runner.validateNativeVoice({...valid,...extra}));
 await assert.rejects(runner.run({}),/native_voice_opt_in_required/);
 await assert.rejects(runner.run({...valid,UR_TEMPORARY_CALL_LIVE:'1',UR_TEMPORARY_CALL_VARIANT:'resume'}),/formal_temporary_observer_required/);
});
test('半双工和全双工都以实际协商模式而非环境标签验收',()=>{
 for(const mode of ['halfDuplex','fullDuplex']){const {before,after}=pair(mode);assert.equal(runner.verifyNativeRound(before,after,mode),true);
 assert.throws(()=>runner.verifyNativeRound(before,{...after,interactionMode:mode==='halfDuplex'?'fullDuplex':'halfDuplex'},mode),/native_negotiated_mode_mismatch/);}
});
test('实际新输入及fixture完成不能被同旧输入或只有开始按钮替代',()=>{
 const {before,after}=pair('halfDuplex');
 for(const change of [{starts:0},{lastInput:''},{fixtureEnded:0},{userStops:0}])assert.throws(()=>runner.verifyNativeRound(before,{...after,voice:{...after.voice,...change}},'halfDuplex'));
 const restoredBefore={...before,voice:{...before.voice,starts:1,lastInput:'old',fixtureEnded:1,userStops:1}};
 const restoredAfter={...after,voice:{...after.voice,starts:2,lastInput:'new',fixtureEnded:2,userStops:2}};
 assert.equal(runner.verifyNativeRound(restoredBefore,restoredAfter,'halfDuplex'),true);
 assert.throws(()=>runner.verifyNativeRound(restoredBefore,{...restoredAfter,voice:{...restoredAfter.voice,lastInput:'old'}},'halfDuplex'),/native_old_input_reused/);
});
test('保留原识别关键词、回复关联、完整解码与实际有声门禁',()=>{
 const {before,after}=pair('fullDuplex');
 for(const change of [{keywordMatched:false},{recognizedSameReply:false},{playableID:'old'},{voiced:1},{network:0},{decoded:1},{missingPackets:1},{observedComplete:false},{completedReplyMediaFinal:false},{outputStopped:false}])assert.throws(()=>runner.verifyNativeRound(before,{...after,voice:{...after.voice,...change}},'fullDuplex'));
});
test('全双工连续输入不能用用户stop或提前关闭采集冒充',()=>{
 const {before,after}=pair('fullDuplex');
 assert.throws(()=>runner.verifyNativeRound(before,{...after,voice:{...after.voice,userStops:1}},'fullDuplex'),/native_user_stop_policy_mismatch/);
 assert.throws(()=>runner.verifyNativeRound(before,{...after,voice:{...after.voice,captureOpen:false}},'fullDuplex'),/native_capture_policy_mismatch/);
 const suspended={...after,captureClosed:true,voice:{...after.voice,captureOpen:false}};
 assert.equal(runner.verifyNativeSuspended(after,suspended,'fullDuplex'),true);
 assert.throws(()=>runner.verifyNativeSuspended(after,{...suspended,voice:{...suspended.voice,userStops:1}},'fullDuplex'),/native_user_stop_replaced_platform_pause/);
 assert.throws(()=>runner.verifyNativeSuspended(suspended,suspended,'fullDuplex'),/native_full_duplex_not_continuous/);
});
test('全双工暂停要求真实输入停止ACK，半双工无活动输入遵循原合同',()=>{
 assert.equal(runner.verifyNativeProof(proof(),'idle','fullDuplex'),true);
 const inactive={...proof(),modelStopRequired:false,modelStoppedAck:false};
 assert.equal(runner.verifyNativeProof(inactive,'idle','halfDuplex'),true);
 assert.throws(()=>runner.verifyNativeProof(inactive,'idle','fullDuplex'),/native_continuous_input_stop_ack_missing/);
});
test('本人stop旁观保留原返回与异常、不改投递并拒绝重复安装',()=>{
 const failure=Error('original'),ctx=vm.createContext({send(value){if(value.fail)throw failure;return value.accepted;}});
 vm.runInContext('('+runner.installNativeObservation.toString()+')()',ctx);
 assert.equal(vm.runInContext('send({kind:"stop",accepted:false})',ctx),false);
 assert.equal(ctx.__urTemporaryNativeVoice.userStops,1);
 assert.equal(vm.runInContext('send({kind:"start",accepted:true})',ctx),true);
 assert.equal(ctx.__urTemporaryNativeVoice.userStops,1);
 assert.throws(()=>vm.runInContext('send({kind:"stop",fail:true})',ctx),value=>value===failure);
 assert.throws(()=>vm.runInContext('('+runner.installNativeObservation.toString()+')()',ctx),/native_voice_observer_duplicate/);
});
test('真实导出getter在页面现有全局形状运行并读取实际metrics模式',async()=>{
 // 与app.js一致的全局字段形状；执行实际共享/临时/音频getter，不替换函数或编造snapshot结果。
 const ctx=vm.createContext({duplexHumanMedia:null,stream:null,socket:{readyState:1},WebSocket:{OPEN:1},context:{state:'running'},terminated:false,
 document:{getElementById(){return {disabled:false};}},responseId:'',completedResponseId:'reply',handoffState:null,humanControlState:null,temporaryResumeAllowed:false,
 metrics:{interactionMode:'fullDuplex',captureOpen:true,completedResponses:1,consumedNonSilent:2,lastPlayableConsumedResponseId:'reply',humanControlReceiptIndex:0,humanControlReceipts:[],focusReceiptIndex:0,focusReceipts:[],captured:4,lastNetworkPackets:2,lastDecodedFrames:2,lastNonSilentFrames:2,consumed:2,lastMissingPackets:0,lastObservedComplete:true,lastAudioState:'completed',lastMediaFinalResponseId:'reply',lastReplyTiming:null},
 useWebRTC:false,rtcPeer:null,aiControlState:null,currentReplyConsumed:0,currentReplyConsecutiveConsumedNonSilent:0,pendingPlayback:new Map(),
 __urAIControl:{starts:1,stops:0,configurations:1,fixtureEnded:1,lossPeer:null,lossPeerRevision:0,lossInput:'',lastInput:'new',recognized:1,recognizedID:'reply',recognizedText:'天气',keywords:['天气']},__urTemporaryNativeVoice:{userStops:0},__urControlPlay(){}});
 const actual=await vm.runInContext(runner.snapshotExpression(),ctx);
 assert.equal(actual.interactionMode,'fullDuplex');assert.equal(actual.voice.captureOpen,true);assert.equal(actual.captureClosed,false);
 assert.equal(actual.voice.starts,1);assert.equal(actual.voice.lastInput,'new');assert.equal(actual.voice.keywordMatched,true);assert.equal(actual.voice.recognizedSameReply,true);assert.equal(actual.voice.control,null);
 // 实际getter协商full与预期half不符仍必须拒绝，但失败报告在finally可看到实际闭集，而非null或预期替身。
 let observed='unknown';assert.throws(()=>runner.verifyNativeMode(actual,'halfDuplex',mode=>{observed=mode;}),/native_negotiated_mode_mismatch/);assert.equal(observed,'fullDuplex');
 ctx.metrics.interactionMode='halfDuplex';ctx.metrics.captureOpen=false;
 const next=await vm.runInContext(runner.snapshotExpression(),ctx);assert.equal(next.interactionMode,'halfDuplex');assert.equal(next.voice.captureOpen,false);assert.equal(next.captureClosed,true);
 // 接听端仅装原Human观察器，不存在AI控制状态；不得让双端ready被私有AI getter阻断。
 delete ctx.__urAIControl;delete ctx.__urTemporaryNativeVoice;
 const human=await vm.runInContext(runner.snapshotExpression(false),ctx);assert.equal(human.ready,true);assert.equal(Object.hasOwn(human,'voice'),false);
 // 接听端诊断搭载同一次原getter；默认无额外字段，显式开启才读取已有纯首因方法。
 class StopFacts{statistics(){return {};}audioSnapshot(){return {known:true,cause:'peerFailed'};}}
 ctx.duplexHumanMedia=new StopFacts();const diagnosed=await vm.runInContext(runner.snapshotExpression(false,true),ctx);assert.deepEqual(JSON.parse(JSON.stringify(diagnosed.audioSnapshot)),{known:true,cause:'peerFailed'});
});
test('未知或私有模式仅投影unknown，失败仍保留原断言且不泄露报文字段',()=>{
 for(const interactionMode of [undefined,null,'私有正文','auto',{},42]){const report={interactionMode:'unknown'};
 assert.throws(()=>runner.verifyNativeMode({interactionMode},'halfDuplex',mode=>{report.interactionMode=mode;}),/native_negotiated_mode_mismatch/);assert.deepEqual(report,{interactionMode:'unknown'});}
 let actual='unknown';assert.equal(runner.verifyNativeMode({interactionMode:'halfDuplex'},'halfDuplex',mode=>{actual=mode;}),true);assert.equal(actual,'halfDuplex');
});
test('浏览器最后操作只暴露固定方向、操作和selector，不报告eval/目标/地址',()=>{
 assert.deepEqual(runner.projectBrowserOperation(0,'select','#handoffTargetKind'),{side:'caller',operation:'select',selector:'#handoffTargetKind'});
 for(const value of ['私有目标','http://private','globalThis.privateToken'])assert.deepEqual(runner.projectBrowserOperation(1,'eval',value),{side:'human',operation:'eval',selector:'unknown'});
 assert.deepEqual(runner.projectBrowserOperation(4,'private','private'),{side:'unknown',operation:'unknown',selector:'unknown'});
});
test('dial快照读取正式布尔全局和固定控件，不把full采集开放当拒绝原因',()=>{
 const ctx=vm.createContext({terminated:false,handoffState:{canEscalate:true},metrics:{captureOpen:true},responseId:'',document:{getElementById(){return {disabled:false};}}});
 const value=vm.runInContext('('+runner.readDialControls.toString()+')()',ctx);
 assert.deepEqual(JSON.parse(JSON.stringify(value)),{terminated:false,canEscalate:true,dialEnabled:true,targetKindEnabled:true,targetEnabled:true,captureOpen:true,activeReply:false});
 ctx.handoffState.canEscalate=false;assert.equal(vm.runInContext('('+runner.readDialControls.toString()+')()',ctx).canEscalate,false);
});
test('拨号旁观只记录原本地发送返回和实际宿主状态，不读取目标或伪造平台ACK',()=>{
 const original=Error('original_send_failure');const ctx=vm.createContext({handoffState:{state:0,canEscalate:true},send(value){if(value.fail)throw original;return value.accepted;}});
 vm.runInContext('('+runner.installNativeObservation.toString()+')()',ctx);
 const read=()=>JSON.parse(JSON.stringify(vm.runInContext('('+runner.readDialProgress.toString()+')()',ctx)));
 assert.deepEqual(read(),{state:0,canEscalate:true,dialAttempts:0,dialSubmitted:0});
 assert.equal(vm.runInContext('send({kind:"handoffControl",operation:"dial",accepted:false,targetId:"private"})',ctx),false);
 assert.equal(vm.runInContext('send({kind:"handoffControl",operation:"dial",accepted:true,targetId:"private"})',ctx),true);
 ctx.handoffState={state:1,canEscalate:false};assert.deepEqual(read(),{state:1,canEscalate:false,dialAttempts:2,dialSubmitted:1});
 assert.throws(()=>vm.runInContext('send({kind:"handoffControl",operation:"dial",fail:true})',ctx),value=>value===original);
 assert.equal(read().dialAttempts,2);ctx.handoffState={state:'private',canEscalate:false};assert.equal(read().state,null);
 assert.equal(JSON.stringify(read()).includes('private'),false);
});
test('拨号操作耗时只保留闭集命令和单调时间，不泄露脚本或自由异常',()=>{
 assert.deepEqual(runner.projectDialTiming(0,'click','#handoffEscalate',123.8,false),{side:'caller',operation:'click',selector:'#handoffEscalate',elapsedMs:123,failed:false});
 for(const elapsed of [NaN,Infinity,-1])assert.deepEqual(runner.projectDialTiming(1,'eval','private script',elapsed,true),{side:'human',operation:'eval',selector:'unknown',elapsedMs:null,failed:true});
});
test('进程退出在谓词catch外保留首因，不等待总截止或再读取已结束页面',async()=>{
 const original=Error('device_ended_early');let reads=0;
 await assert.rejects(shared.waitFor(()=>runner.guardedRead(()=>{throw original;},()=>{++reads;try{throw Error('local');}catch{return false;}}),performance.now()+10000),error=>error===original);
 assert.equal(reads,0);assert.equal(runner.guardedRead(()=>{},()=>true),true);
});
test('首个原异常必须穿过实际finally，报告/清理状态与统一失败断言不能覆盖它',async()=>{
 const original=Error('browser_command_failed');let failureRaised=false,failureCode=null,cleaned=false,reported;
 await assert.rejects((async()=>{try{await shared.captureFailure(async()=>{throw original;},code=>{failureRaised=true;failureCode=code;});}finally{cleaned=true;runner.finishNativeReport({passed:false,failureCode,cleanup:{devicesClosed:cleaned}},failureRaised,report=>{reported=report;throw Error('report_failed');});}})(),value=>value===original);
 assert.equal(cleaned,true);assert.equal(reported.cleanup.devicesClosed,true);assert.equal(reported.failureCode,'browser_command_failed');
 assert.throws(()=>runner.finishNativeReport({passed:false},false,()=>{}),/temporary_call_live_failed/);
});
test('异常闭集沿原descriptor读取，不执行自由message getter或输出原私有正文',async()=>{
 let reads=0;const original={};Object.defineProperty(original,'message',{get(){++reads;return '私有正文';}});let code;
 await assert.rejects(shared.captureFailure(async()=>{throw original;},value=>{code=value;}),value=>value===original);
 assert.equal(reads,0);assert.equal(code,'unknown');assert.equal(shared.projectFailureCode(Error('私有正文')),'unknown');
});
