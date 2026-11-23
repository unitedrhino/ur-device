// 文件说明：临时恢复驱动的离线严格判据回归；固定事实只是单测输入，不能代替真实HAL/SQL或模型音频。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm'),runner=require('./temporary_call_live.cjs');
const fs=require('node:fs'),path=require('node:path'); // 只加载正式Human模块复现真实close，不启动浏览器或媒体。
// focus 构造原页面五字段回执形状，不构造设备或云端回执。
function focus(operation,receiptIndex=1){return {operation,state:{suspend:'suspended',release:'released',restore:'restored'}[operation],code:200,revision:receiptIndex,receiptIndex};}
// idle 构造人际资源结束且AI未自动采集的只读判据。
function idle(extra={}){return {humanReleased:true,humanInputClosed:true,humanOutputClosed:true,humanStoppedConfirmed:true,captureClosed:true,inputAttached:false,outputEnabled:false,activeReply:false,canResumeAI:true,temporaryState:{canResumeAI:extra.canResumeAI??true,restoring:false},completed:1,nonSilent:4,completedID:'old',focusIndex:3,focusReceipts:[],...extra};}
// proof 构造宿主服务端观察的严格闭集；默认表示尚未请求restore的已释放阶段。
function proof(extra={}){return {sameOriginal:true,sameOriginalModel:true,sameOriginalRuntime:true,modelRetained:true,modelStopRequired:true,modelStopCompleted:true,modelStoppedAck:true,suspendedAck:true,humanAcquiredAck:true,humanReleasedAck:true,restoredAck:false,resumeAllowed:true,stoppedIntent:false,revoked:false,pendingRestore:false,...extra};}
test('五阶段服务端证明不能被页面可恢复或本地发送受理代替',()=>{
  assert.equal(runner.verifyProof(proof(),'idle'),true);assert.equal(runner.verifyProof(proof({restoredAck:true}),'restored'),true);
  for(const extra of [{sameOriginal:false},{sameOriginalModel:false},{sameOriginalRuntime:false},{modelRetained:false},{revoked:true},{modelStoppedAck:false},{suspendedAck:false},{humanAcquiredAck:false},{humanReleasedAck:false},{restoredAck:true}])assert.throws(()=>runner.verifyProof(proof(extra),'idle'));
  assert.throws(()=>runner.verifyProof({...proof(),text:'private'},'idle'));assert.throws(()=>runner.verifyProof(proof({humanReleasedAck:'true'}),'idle'));
  let reads=0;const getter=proof();Object.defineProperty(getter,'modelRetained',{get(){reads++;return true;}});assert.throws(()=>runner.verifyProof(getter,'idle'));assert.equal(reads,0);
  const symbol=proof();symbol[Symbol('hidden')]=true;assert.throws(()=>runner.verifyProof(symbol,'idle'));
  const inherited=proof();delete inherited.sameOriginal;Object.setPrototypeOf(inherited,{sameOriginal:true});assert.throws(()=>runner.verifyProof(inherited,'idle'));
});
test('无运行输入只完成停流屏障，不虚构停止ACK；有输入时仍须真实ACK',()=>{
  assert.equal(runner.verifyProof(proof({modelStopRequired:false,modelStoppedAck:false}),'idle'),true);
  for(const extra of [{modelStopCompleted:false},{modelStoppedAck:false},{modelStopRequired:false},{modelStopRequired:false,modelStoppedAck:false,modelStopCompleted:false}])
    assert.throws(()=>runner.verifyProof(proof(extra),'idle'),/temporary_model_stop_unconfirmed/);
  const missing=proof();delete missing.modelStopRequired;
  assert.throws(()=>runner.verifyProof(missing,'idle'),/temporary_proof_invalid/);
});
test('挂起与恢复ACK必须唯一新回执且revision前进，拒绝不能被后继成功覆盖',()=>{
  assert.equal(runner.receipt([focus('suspend',2)],'suspend',1,1).state,'suspended');
  for(const rows of [[focus('suspend',1)],[focus('suspend',2),focus('suspend',3)],[{...focus('suspend',2),code:409,state:'denied'}],[{...focus('suspend',2),state:'restored'}],[{...focus('suspend',2),payload:'private'}]])assert.throws(()=>runner.receipt(rows,'suspend',1));
});
test('人际Peer未关、Sender未脱离、输出未停或AI自动采集均不能恢复',()=>{
  assert.equal(runner.releasedAndIdle(idle()),true);
  for(const extra of [{humanReleased:false},{humanInputClosed:false},{humanOutputClosed:false},{humanStoppedConfirmed:false},{captureClosed:false},{activeReply:true},{captureClosed:'true'},{temporaryState:null},{temporaryState:{canResumeAI:true,restoring:true}},{temporaryState:{canResumeAI:false,restoring:false}}])assert.throws(()=>runner.releasedAndIdle(idle(extra)));
});
test('恢复需同原AI的新完成回复和同回复有声，历史PCM与替换模型拒绝',()=>{
  const next={...idle(),aiSession:'original',completed:2,nonSilent:6,completedID:'new',playableID:'new'};
  assert.equal(runner.verifyNewRound('original',idle(),next),true);
  for(const extra of [{aiSession:'replacement'},{completed:1},{nonSilent:4},{completedID:'old'},{playableID:'old'},{activeReply:true}])assert.throws(()=>runner.verifyNewRound('original',idle(),{...next,...extra}));
});
test('本人stop使恢复无资格且不能有成功restore或新音频',()=>{
  const stopped=idle({canResumeAI:false});assert.equal(runner.verifyStopped(stopped,stopped),true);
  assert.equal(runner.verifyProof(proof({resumeAllowed:false,stoppedIntent:true}),'stopped'),true);
  for(const extra of [{canResumeAI:true},{completed:2},{nonSilent:5},{focusReceipts:[focus('restore',4)]}])assert.throws(()=>runner.verifyStopped(stopped,{...stopped,...extra}));
  assert.throws(()=>runner.verifyProof(proof(),'stopped'));assert.throws(()=>runner.verifyProof(proof({resumeAllowed:false,stoppedIntent:true,restoredAck:true}),'stopped'));
  assert.throws(()=>runner.verifyProof(proof({resumeAllowed:false,stoppedIntent:true,pendingRestore:true}),'stopRejected'));
  assert.equal(runner.verifyProof(proof({resumeAllowed:false,stoppedIntent:true}),'stopRejected'),true);
});
test('原source四字段、轮数和有效截止严格核，不接受同sid加伪造正文或过期',()=>{
  const original={sourceSessionId:'original',expiresAt:200,firstTurnIndex:1,lastTurnIndex:2};assert.equal(runner.verifySource(original,2,100),original);
  for(const extra of [{lastTurnIndex:1},{firstTurnIndex:0},{expiresAt:100},{expiresAt:NaN},{sourceSessionId:'bad\n'},{sourceSessionId:''},{extra:'private'}])assert.throws(()=>runner.verifySource({...original,...extra},2,100));
});
test('页面读取沿原导出getter和真实临时布尔，不从中文文案推造恢复事实',()=>{
  const text=runner.snapshotExpression();assert.ok(!text.includes('temporaryStatus'));assert.ok(text.includes('temporaryResumeAllowed'));assert.doesNotThrow(()=>new vm.Script(text));
});
test('旁观实际incoming/send完整保留原返回及异常，不把投递成功冒native或云端接受',()=>{
  const failure=Error('original'),ctx={incoming(value){if(value.fail)throw failure;return 7;},send(value){if(value.fail)throw failure;return value.accepted;}};vm.createContext(ctx);
  vm.runInContext('('+runner.installTemporaryObservation.toString()+')()',ctx);
  assert.equal(vm.runInContext('incoming({kind:"temporaryCallState",canResumeAI:false,restoring:true})',ctx),7);
  assert.equal(ctx.__urTemporaryCallObservation.state.restoring,true);
  assert.equal(vm.runInContext('send({kind:"start",accepted:false})',ctx),false);assert.equal(ctx.__urTemporaryCallObservation.accepted,0);
  assert.equal(vm.runInContext('send({kind:"start",accepted:true})',ctx),true);assert.equal(ctx.__urTemporaryCallObservation.accepted,1);
  assert.throws(()=>vm.runInContext('send({kind:"start",fail:true})',ctx),error=>error===failure);
  assert.throws(()=>vm.runInContext('incoming({kind:"temporaryCallState",fail:true})',ctx),error=>error===failure);
  assert.equal(ctx.__urTemporaryCallObservation.starts,2);
});
test('缺显式模式或正式观察器在任何资源动作前拒绝，不用mock回退',async()=>{
  await assert.rejects(runner.run({}),/temporary_opt_in_required/);
  await assert.rejects(runner.run({UR_TEMPORARY_CALL_LIVE:'1',UR_TEMPORARY_CALL_VARIANT:'resume'}),/formal_temporary_observer_required/);
  await assert.rejects(runner.run({UR_TEMPORARY_CALL_LIVE:'1',UR_TEMPORARY_CALL_VARIANT:'other'}),/temporary_variant_required/);
});
test('真实Human.close保留旧outputGated=true，但完成Peer/节点释放后应承认关闭',()=>{
  const context=vm.createContext({});vm.runInContext(fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/human-voice-media.js'),'utf8'),context);
  const published=[],media=new context.UrHumanVoiceMedia({emit:value=>published.push(value)}),scope={sessionId:'human',routeGeneration:'1',peerEpoch:'1'};
  media.scope=scope;media.peer={connectionState:'connected',close(){this.connectionState='closed';}};
  let disconnected=false;media.node={port:{postMessage(){},close(){}},disconnect(){disconnected=true;}};
  media.receiverSink={paused:false,srcObject:{},pause(){this.paused=true;}};
  context.duplexHumanMedia=media;vm.runInContext('('+require('./shared_handoff_live.cjs').installPageObservation.toString()+')()',context);
  vm.runInContext('('+runner.installHumanReleaseObservation.toString()+')()',context);
  media.publish({kind:'humanVoiceOutputGated',enabled:true,...scope});media.close();
  assert.equal(context.__urSharedHandoffObservation.outputEnabled,true);assert.equal(disconnected,true);
  assert.ok(published.some(value=>value.kind==='humanVoiceStopped'));assert.equal(media.peer.connectionState,'closed');
  assert.equal(context.__urTemporaryHumanRelease.scope.sessionId,scope.sessionId);
  assert.equal(runner.releasedAndIdle(idle({inputAttached:true,outputEnabled:true,humanInputClosed:true,humanOutputClosed:true,humanStoppedConfirmed:true})),true);
  // 原节点disconnect实际抛错时close不发布Stopped，不能仅closed布尔补造输出释放。
  const failed=new context.UrHumanVoiceMedia({emit:value=>published.push(value)});failed.scope=scope;failed.peer={connectionState:'connected',close(){this.connectionState='closed';}};
  failed.node={port:{postMessage(){},close(){}},disconnect(){throw Error('disconnect_failed');}};
  const failedContext=vm.createContext({duplexHumanMedia:failed});vm.runInContext('('+runner.installHumanReleaseObservation.toString()+')()',failedContext);failed.close();
  assert.equal(failedContext.__urTemporaryHumanRelease.scope,null);assert.throws(()=>runner.releasedAndIdle(idle({humanStoppedConfirmed:false,humanOutputClosed:false})));
});
test('关闭一个本人浏览器失败仍继续另一端，失败不能被两个已打开数量覆盖',async()=>{
  const seen=[];assert.equal(await runner.closeOwnedBrowsers([0,1],async index=>{seen.push(index);if(index===0)throw Error('close_failed');}),false);assert.deepEqual(seen,[0,1]);
  assert.equal(await runner.closeOwnedBrowsers([0,1],async index=>index===0?false:true),false);
});
test('两个本人浏览器关闭成功才通过；不允许陌生index或重复session清理',async()=>{
  const seen=[];assert.equal(await runner.closeOwnedBrowsers([0,1],async index=>{seen.push(index);}),true);assert.deepEqual(seen,[0,1]);
  await assert.rejects(runner.closeOwnedBrowsers([0,2],async()=>true));await assert.rejects(runner.closeOwnedBrowsers([0,0],async()=>true));
});
