// 文件说明：目标恢复驱动的原wire/HAL关联与严格失败判据；合成夹具不代表真实模型或现场恢复通过。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm');
const live=require('./browser_ai_endpoint_target_recovery_live.cjs');

// witness构造与正式契约一致的合成SIG原消息，私有出处另给可控SQL只读proof。
function witness() {
  const resources=['microphone','speaker'],tracks=[{trackId:'in',kind:'audio',sourceId:'mic',direction:'sendonly',codec:'opus',state:'requested'},{trackId:'out',kind:'audio',sourceId:'speaker',direction:'recvonly',codec:'opus',state:'requested'}];
  const cap={revision:7,purposes:['aiConversation'],features:['audio','sessionSync','localAccept','aiEndpointHandoff','aiEndpointControlSync'],controlCapabilities:{operations:['focusControl'],focusResources:resources}};
  const focus=(operation,id,revision)=>({operation,focusId:id,expectedRevision:revision,resources,priority:'conversation',resumePolicy:'explicit',...(operation==='acquire'?{expiresAt:100000}:{})});
  const records=[],add=(connection,direction,message)=>records.push({connection,direction,message});
  add(1,'down',{method:'sessionInvite',msgToken:'invite',sessionId:'public',eventSeq:1,params:{tracks}});
  add(1,'down',{method:'sessionEstablished',msgToken:'established',sessionId:'public',eventSeq:2,data:{state:'established',mediaRevision:1,endpointEpoch:2,capabilityRevision:7}});
  add(1,'down',{method:'focusControl',msgToken:'old-acquire',sessionId:'public',eventSeq:3,params:focus('acquire','old-focus',0)});
  add(1,'up',{method:'focusState',msgToken:'old-acquire',sessionId:'public',code:200,data:{state:'acquired',focusId:'old-focus',revision:1,resources}});
  add(1,'up',{method:'audioStart',msgToken:'old-input-token',sessionId:'public',params:{inputId:'old-input',firstPacketSeq:128}});
  add(2,'up',{method:'capabilityReport',msgToken:'cap-one',params:cap});
  add(2,'up',{method:'sessionSync',msgToken:'sync',sessionId:'public',params:{lastEventSeq:6,lastMessageSeq:12,endpointEpoch:2,mediaRevision:1}});
  const paused=tracks.map(x=>({...x,state:'paused'}));
  add(2,'down',{method:'sessionSnapshot',msgToken:'sync',sessionId:'public',code:200,eventSeq:9,data:{state:'established',resumeAllowed:true,revision:11,eventWatermark:9,messageWatermark:13,endpointEpoch:3,capabilityRevision:7,mediaRevision:1,tracks:paused,controls:{ai:{inputState:'paused',inputId:'old-input',activeRespIds:[]},focus:{state:'acquired',focusId:'old-focus',revision:1,resources},mediaPlan:{revision:1,tracks:paused}}}});
  add(2,'down',{method:'focusControl',msgToken:'old-release',sessionId:'public',eventSeq:10,params:focus('release','old-focus',1)});
  add(2,'up',{method:'focusState',msgToken:'old-release',sessionId:'public',code:200,data:{state:'released',focusId:'old-focus',revision:2,resources}});
  add(2,'up',{method:'capabilityReport',msgToken:'cap-two',params:{...structuredClone(cap),controlCapabilities:{...structuredClone(cap.controlCapabilities),baseline:{state:'idle',revision:2,resources}}}});
  add(2,'down',{method:'focusControl',msgToken:'fresh-acquire',sessionId:'public',eventSeq:11,params:focus('acquire','fresh-focus',2)});
  add(2,'up',{method:'focusState',msgToken:'fresh-acquire',sessionId:'public',code:200,data:{state:'acquired',focusId:'fresh-focus',revision:3,resources}});
  // 在原请求之后插入正式能力成功回执；其他测试用语义记录索引避免回执造成误指向。
  for(const token of ['cap-one','cap-two']){const index=records.findIndex(x=>x.message.msgToken===token);records.splice(index+1,0,{connection:2,direction:'down',message:{method:'requestAck',msgToken:token,code:200,data:{requestMethod:'capabilityReport',result:{acceptedRevision:7}}}});}
  return {records,failed:false,overflow:false,bindingProof:{oldConnectionEpoch:100,newConnectionEpoch:105,oldInstance:'old-instance',newInstance:'new-instance',recoveryDeadlinePreserved:true,originalHandoffImmutable:true}};
}
// originalRecords保留原流程语义位置，不让新增能力ACK改变原畸形字段用例所指向的消息。
function originalRecords(witness){return witness.records.filter(x=>x.message.method!=='requestAck');}
// facts只描述原页面HAL观察，回执索引/全局revision均真实语义但此处为独立可控夹具。
function facts() {
  const receipt=(operation,revision)=>({operation,state:operation==='acquire'?'acquired':'released',code:200,revision,receiptIndex:revision});
  const before={control:{state:'ready',endpointEpoch:2,messageWatermark:12,originalSessionPreserved:true},captureOpen:true,outputStopped:true,targetPhase:7,terminated:false,contextRunning:true,tracksEnded:false,contextClosed:false,starts:2,stops:1,recognized:1,completed:1,fixtureEnded:1,configurations:0,inputID:'old-input',focusReceipts:[receipt('acquire',1)]};
  return {before,stopped:{actualCaptureClosed:true,actualOutputStopped:true,originalSessionPreserved:true,endpointEpoch:2,expired:false,elapsed:2300},after:{...structuredClone(before),captureOpen:false,control:{...before.control,endpointEpoch:3,messageWatermark:13},focusReceipts:[receipt('acquire',1),receipt('release',2),receipt('acquire',3)]}};
}
test('真实wire形状须与HAL分开核旧全组释放、两cap及独立fresh取得',()=>{
  const wire=live.verifyTargetRecoveryWire(witness(),'targetRecoveryActive'),f=facts();assert.equal(wire.secondActualBaselineVerified,true);assert.equal(live.verifyTargetRecovery(f.before,f.stopped,f.after,'targetRecoveryActive').noAutomaticCapture,true);
  assert.equal(JSON.stringify(wire).includes('old-input'),false);assert.equal(JSON.stringify(wire).includes('old-instance'),false);
});
test('每个原token/session/资源/CAS/绑定/预算字段缺失或错误都不能用success bool兜底',()=>{
  const mutations=[
    x=>x.failed=true,x=>x.overflow=true,x=>delete x.bindingProof,
    x=>x.bindingProof.newConnectionEpoch=100,x=>x.bindingProof.newInstance=x.bindingProof.oldInstance,
    x=>x.bindingProof.recoveryDeadlinePreserved=false,x=>x.bindingProof.originalHandoffImmutable=false,
    x=>x.records[5].message.params.controlCapabilities.baseline={state:'idle',revision:0,resources:['microphone','speaker']},
    x=>x.records[7].message.msgToken='wrong',x=>x.records[7].message.sessionId='wrong',x=>x.records[7].message.data.controls.ai.activeRespIds=['old-reply'],
    x=>x.records[7].message.data.endpointEpoch=2,x=>x.records[7].message.data.messageWatermark=11,x=>x.records[7].message.data.tracks[0].trackId='different',
    x=>x.records[8].message.params.focusId='different',x=>x.records[9].message.msgToken='wrong',x=>x.records[9].message.data.revision=1,
    x=>x.records[10].message.params.revision=8,x=>x.records[10].message.params.features.push('invented'),x=>x.records[10].message.params.controlCapabilities.baseline.revision=0,
    x=>x.records[11].message.params.focusId='old-focus',x=>x.records[11].message.params.expectedRevision=1,x=>x.records[11].message.params.expiresAt=100001,
    x=>x.records[12].message.code=409,x=>x.records[12].message.data.resources=['microphone'],x=>x.records[12].message.msgToken='wrong',
    x=>x.records.push({connection:2,direction:'up',message:{method:'audioStart',msgToken:'auto',sessionId:'public',params:{inputId:'old-input'}}}),
    x=>x.records.splice(8,1),x=>[x.records[9],x.records[10]]=[x.records[10],x.records[9]],x=>x.records[6].message.version=2
  ];
  for(const [index,change]of mutations.entries()){const x=witness(),acks=x.records.filter(x=>x.message.method==='requestAck');x.records=originalRecords(x);change(x);for(const ack of acks){const request=x.records.findIndex(p=>p.message.msgToken===ack.message.msgToken);if(request>=0)x.records.splice(request+1,0,ack);}assert.throws(()=>live.verifyTargetRecoveryWire(x,'targetRecoveryActive'),'negative '+index);}
});
test('原字节同token重投只读去重，冲突重投必须失败',()=>{
  const x=witness();x.records.push(structuredClone(x.records.at(-1)));assert.equal(live.verifyTargetRecoveryWire(x,'targetRecoveryActive').freshOriginalFocusAcquired,true);
  x.records.at(-1).message.data.revision=4;assert.throws(()=>live.verifyTargetRecoveryWire(x,'targetRecoveryActive'));
});
test('两份能力ACK必须原token/空会话/acceptedRevision且不能用错误或缺失回执推进恢复',()=>{
  for(const change of [x=>x.records.splice(6,1),x=>x.records[6].message.msgToken='wrong',x=>x.records[6].message.sessionId='public',x=>x.records[6].message.code=409,x=>x.records[6].message.data.requestMethod='audioStart',x=>x.records[6].message.data.result.acceptedRevision=8,x=>x.records.splice(12,1),x=>x.records[12].message.data.result.acceptedRevision=0]){const x=witness();change(x);assert.throws(()=>live.verifyTargetRecoveryWire(x,'targetRecoveryActive'));}
});
test('持久报告投影只允许有限布尔和延迟，不传播原身份/嵌套token或自由文本',()=>{
  const facts={...live.deriveTargetRecoveryWire(witness(),'targetRecoveryActive'),originalSessionPreserved:true,recoveryMilliseconds:123,token:'private-token',nested:{token:'hidden'},reason:'private-text'};
  assert.deepEqual(live.projectPublicRecoveryFacts(facts),{originalSessionPreserved:true,originalDeadlinePreserved:true,oldInputNotReplayed:true,recoveryMilliseconds:123});
  assert.throws(()=>live.projectPublicRecoveryFacts({noAutomaticCapture:'true'}));assert.throws(()=>live.projectPublicRecoveryFacts({recoveryMilliseconds:-1}));
  assert.throws(()=>live.verifyTargetRecoveryWire({...facts,capabilityAcknowledgementsVerified:true},'targetRecoveryActive'));
});
test('失效只允许原networkTimeout唯一终态且无新cap/输入/取得',()=>{
  const x=witness();x.records=x.records.slice(0,5);Object.assign(x.bindingProof,{terminalReason:'networkTimeout',terminalCount:1,originalModelPreserved:true,originalCreationBindingPreserved:true,expiredTerminalVerified:true});assert.equal(live.verifyTargetRecoveryWire(x,'targetRecoveryExpired').uniqueNetworkTimeout,true);
  for(const change of [p=>p.bindingProof.terminalCount=2,p=>p.bindingProof.terminalReason='mediaFailed',p=>p.records.push({connection:2,direction:'up',message:{method:'capabilityReport',msgToken:'late',params:{}}}),p=>p.records.push({connection:2,direction:'down',message:{method:'focusControl',msgToken:'late-focus',sessionId:'public',params:{operation:'acquire'}}})]){const p=structuredClone(x);change(p);assert.throws(()=>live.verifyTargetRecoveryWire(p,'targetRecoveryExpired'));}
});
test('HAL不得自动采集/重放旧回复/造release/改变原媒体上下文，用户已停止维持关闭',()=>{
  const f=facts();for(const change of [p=>p.captureOpen=true,p=>p.starts++,p=>p.recognized++,p=>p.completed++,p=>p.configurations++,p=>p.focusReceipts.splice(1,1),p=>p.focusReceipts[2].revision=2,p=>p.inputID='auto-input',p=>p.contextClosed=true,p=>p.control.endpointEpoch=4]){const after=structuredClone(f.after);change(after);assert.throws(()=>live.verifyTargetRecovery(f.before,f.stopped,after,'targetRecoveryActive'));}
  assert.throws(()=>live.verifyTargetRecovery(f.before,{...f.stopped,elapsed:15000},f.after,'targetRecoveryActive'));
  assert.equal(live.verifyTargetRecovery({...f.before,captureOpen:false},f.stopped,f.after,'targetRecoveryStopped').userStopPreserved,true);
});
test('恢复后第二跳必须当前fresh原lease释放而不是借旧release完成',()=>{
  const f=facts(),after={...structuredClone(f.after),terminated:true,tracksEnded:true,contextClosed:true};after.focusReceipts.push({operation:'release',state:'released',code:200,revision:4,receiptIndex:4});
  const target={targetPhase:7,captureOpen:false,terminated:false,contextRunning:true,focusReceipts:[{operation:'acquire',state:'acquired',code:200,revision:1,receiptIndex:1}]};
  assert.equal(live.verifyRecoveredSourceTransfer(f.after,after,target).sourceFreshOriginalFullGroupReleased,true);after.focusReceipts.pop();assert.throws(()=>live.verifyRecoveredSourceTransfer(f.after,after,target));
});
test('恢复后的原新socket第二跳release必须原freshId/token/全组对应，旧release不够',()=>{
  const w=witness();w.records.push({connection:2,direction:'down',message:{method:'focusControl',msgToken:'source-release',sessionId:'public',eventSeq:14,params:{operation:'release',focusId:'fresh-focus',expectedRevision:3,resources:['microphone','speaker'],priority:'conversation',resumePolicy:'explicit'}}});w.records.push({connection:2,direction:'up',message:{method:'focusState',msgToken:'source-release',sessionId:'public',code:200,data:{state:'released',focusId:'fresh-focus',revision:4,resources:['microphone','speaker']}}});
  assert.equal(live.verifyRecoveredTransferWire(w).freshOriginalFocusReleased,true);
  for(const mutate of [x=>x.records.pop(),x=>x.records.at(-1).message.msgToken='wrong',x=>x.records.at(-1).message.data.focusId='old-focus',x=>x.records.at(-1).connection=1,x=>x.records.at(-1).message.data.revision=3]){const x=structuredClone(w);mutate(x);assert.throws(()=>live.verifyRecoveredTransferWire(x));}
});
test('第二跳释放必须沿原fresh实际取得ACK的完整作用域、基线、CAS和严格次序',()=>{
  const w=witness();w.records.push({connection:2,direction:'down',message:{method:'focusControl',msgToken:'source-release',sessionId:'public',eventSeq:14,params:{operation:'release',focusId:'fresh-focus',expectedRevision:3,resources:['microphone','speaker']}}},{connection:2,direction:'up',message:{method:'focusState',msgToken:'source-release',sessionId:'public',code:200,data:{state:'released',focusId:'fresh-focus',revision:4,resources:['microphone','speaker']}}});
  const command=x=>x.records.find(p=>p.message.msgToken==='fresh-acquire'&&p.direction==='down'),receipt=x=>x.records.find(p=>p.message.msgToken==='fresh-acquire'&&p.direction==='up');
  for(const mutate of [
    x=>x.records.splice(x.records.indexOf(receipt(x)),1),
    x=>{x.records.at(-2).message.params.expectedRevision=100;x.records.at(-1).message.data.revision=101;},
    x=>receipt(x).message.msgToken='wrong',x=>receipt(x).message.sessionId='wrong',x=>receipt(x).message.code=409,
    x=>receipt(x).message.data.state='released',x=>receipt(x).message.data.focusId='wrong',x=>receipt(x).message.data.revision=4,
    x=>command(x).message.params.expectedRevision=1,x=>receipt(x).message.data.resources=['microphone'],x=>command(x).message.params.resources=['speaker'],
    x=>receipt(x).direction='down',x=>receipt(x).connection=1,
    x=>{const a=x.records.indexOf(command(x)),r=x.records.indexOf(receipt(x));[x.records[a],x.records[r]]=[x.records[r],x.records[a]];},
    x=>{const r=x.records.indexOf(receipt(x)),release=x.records.length-2;[x.records[r],x.records[release]]=[x.records[release],x.records[r]];},
    x=>x.records.find(p=>p.message.method==='sessionSync').message.sessionId='wrong',
    x=>{command(x).message.params.expectedRevision=null;receipt(x).message.data.revision=1;x.records.at(-2).message.params.expectedRevision=1;x.records.at(-1).message.data.revision=2;},
    x=>{command(x).message.params.expectedRevision=-1;receipt(x).message.data.revision=0;x.records.at(-2).message.params.expectedRevision=0;x.records.at(-1).message.data.revision=1;},
    x=>{command(x).message.params.expectedRevision=Number.MAX_SAFE_INTEGER;receipt(x).message.data.revision=Number.MAX_SAFE_INTEGER+1;x.records.at(-2).message.params.expectedRevision=Number.MAX_SAFE_INTEGER+1;x.records.at(-1).message.data.revision=Number.MAX_SAFE_INTEGER+2;},
    x=>{command(x).message.params.focusId='';receipt(x).message.data.focusId='';x.records.at(-2).message.params.focusId='';x.records.at(-1).message.data.focusId='';},
    x=>{for(const record of x.records)delete record.message.sessionId;}
  ]){const x=structuredClone(w);mutate(x);assert.throws(()=>live.verifyRecoveredTransferWire(x));}
});
test('分段lost checkpoint不重置浏览器原恢复起点，14秒之后仍不能无限等',async()=>{
  const context=vm.createContext({__urTargetRecoveryWindow:{started:0,epoch:2},performance:{now:()=>14000},send:()=>true,aiControlState:{state:'synchronizing'},aiEndpointState:{phase:6},metrics:{focusReceipts:[]},setTimeout:fn=>{fn();}});
  await assert.rejects(vm.runInContext('('+live.targetControlReconnect.toString()+')("targetRecoveryActive")',context));
});
test('缺原只读wire/checkpoint入口时拒新variant，不会静默回退旧自动恢复',async()=>{
  await assert.rejects(live.run({UR_AI_ENDPOINT_VARIANT:'targetRecoveryActive'},{}));await assert.rejects(live.run({UR_AI_ENDPOINT_VARIANT:'commit'},{}));
});
for (const failedStep of ['disconnect', 'reconnect']) test('恢复失败准确保留原'+failedStep+'阶段和异常，不执行后继动作',async()=>{
  const f=facts(),calls=[],failure=Error('fixed_test_failure');let phase;
  const port={async facts(){calls.push('facts');return {...f.before,captureOpen:false};},
    async evaluate(script){const step=script.includes('targetControlDisconnect')?'disconnect':'reconnect';calls.push(step);if(step===failedStep)throw failure;return {...f.stopped,expired:undefined,elapsed:undefined};}};
  await assert.rejects(live.executeRecovery({variant:'targetRecoveryStopped'},[{},port,{}],value=>{phase=value;},
    {rounds:[],transfers:[]},async()=>{calls.push('wire');},async(_role,value)=>calls.push(value)),error=>error===failure);
  assert.equal(phase,failedStep==='disconnect'?'target_control_disconnect':'target_control_reconnect');
  assert.deepEqual(calls,failedStep==='disconnect'?['facts','before','disconnect']:['facts','before','disconnect','lost','reconnect']);
});
test('本人已停止的真实编排顺序必须before/lost/after再wire，不自动新start或第二跳',async()=>{
  const f=facts();f.before.captureOpen=false;const calls=[],port={
    async facts(){return structuredClone(calls.includes('reconnected')?f.after:f.before);},
    async evaluate(script){if(script.includes('targetControlDisconnect')){calls.push('disconnected');return {...f.stopped,expired:undefined,elapsed:undefined};}calls.push('reconnected');return {expired:false,elapsed:2300};},
    async invoke(kind,selector){calls.push(kind+':'+selector);},async wait(){throw Error('unexpected_wait');}
  },journal={rounds:[],transfers:[]};
  await live.executeRecovery({variant:'targetRecoveryStopped'},[{},port,{}],()=>{},journal,async()=>{calls.push('wire');return witness();},async(_role,phase)=>calls.push(phase));
  assert.deepEqual(calls,['before','disconnected','lost','reconnected','after','wire','wait:500']);assert.equal(journal.recoveries[0].userStopPreserved,true);assert.equal(journal.rounds.length,0);
});
test('已过期编排只read ended，无reconnect成功或新输入，报告不含RAM原标识',async()=>{
  const f=facts(),calls=[],expired={...f.after,control:structuredClone(f.before.control),terminated:true,contextClosed:true,contextRunning:false,tracksEnded:true,focusReceipts:structuredClone(f.before.focusReceipts)};
  const port={async facts(){return structuredClone(calls.includes('expired')?expired:f.before);},async invoke(kind,selector){calls.push(kind+':'+selector);},async wait(){calls.push('capture');},async evaluate(script){if(script.includes('targetControlDisconnect')){calls.push('disconnected');return {...f.stopped,expired:undefined,elapsed:undefined};}calls.push('expired');return {expired:true,reconnectAfterExpiryRejected:true,elapsed:16000};}};
  const raw=witness();raw.records=raw.records.slice(0,5);Object.assign(raw.bindingProof,{terminalReason:'networkTimeout',terminalCount:1,originalModelPreserved:true,originalCreationBindingPreserved:true,expiredTerminalVerified:true});const journal={rounds:[],transfers:[]};
  await live.executeRecovery({variant:'targetRecoveryExpired'},[{},port,{}],()=>{},journal,async()=>{calls.push('wire');return raw;},async(_role,phase)=>calls.push(phase));
  assert.deepEqual(calls,['click:#start','capture','before','disconnected','lost','expired','ended','wire']);assert.equal(journal.recoveries[0].uniqueNetworkTimeout,true);assert.equal(journal.rounds.length,0);assert.equal(journal.transfers.length,0);
  assert.equal(journal.expiredRetirementBeforeCleanup.sameOriginalRecoveryScopeVerified,true);
  for(const key of ['originalInputPreserved','originalEndpointPreserved','terminated','tracksEnded','contextClosed'])assert.equal(journal.expiredRetirementBeforeCleanup[key],true);
  assert.equal(journal.expiredRetirementBeforeCleanup.expiredMilliseconds,16000);assert.equal(journal.expiredRetirementBeforeCleanup.successfulAcquires,1);
  for(const privateValue of ['public','old-instance','old-input','old-focus','old-acquire'])assert.equal(JSON.stringify(journal).includes(privateValue),false);
});
test('active恢复后仅本人新start可继续且任何旧input复用先失败，不执行后继移交',async()=>{
  const f=facts(),calls=[],port={async facts(){return structuredClone(calls.includes('reconnected')?f.after:f.before);},async invoke(kind,selector){calls.push(kind+':'+selector);},async wait(){calls.push('capture');},async evaluate(script){if(script.includes('targetControlDisconnect')){calls.push('disconnected');return {...f.stopped,expired:undefined,elapsed:undefined};}calls.push('reconnected');return {expired:false,elapsed:2300};},async audioRound(){calls.push('new-round');return {actualDecoded:true};}};
  const journal={rounds:[],transfers:[]};await assert.rejects(live.executeRecovery({variant:'targetRecoveryActive'},[{},port,{}],()=>{},journal,async()=>witness(),async(_role,phase)=>calls.push(phase)));
  assert.deepEqual(calls,['click:#start','capture','before','disconnected','lost','reconnected','after','new-round']);assert.equal(journal.recoveries[0].noAutomaticCapture,true);assert.equal(journal.rounds.length,0);assert.equal(journal.transfers.length,0);
});

// 过期SQL旁证须沿原bindingProof产生；未知/旧scope不能由页面布尔或终态文字补齐。
test('Expired原模型创建绑定及终态scope旁证缺失或否定全部拒绝',()=>{
  const x=witness();x.records=x.records.slice(0,5);Object.assign(x.bindingProof,{terminalReason:'networkTimeout',terminalCount:1,originalModelPreserved:true,originalCreationBindingPreserved:true,expiredTerminalVerified:true});
  for(const key of ['originalModelPreserved','originalCreationBindingPreserved','expiredTerminalVerified'])for(const mode of ['missing','false']){
    const p=structuredClone(x);if(mode==='missing')delete p.bindingProof[key];else p.bindingProof[key]=false;
    assert.throws(()=>live.verifyTargetRecoveryWire(p,'targetRecoveryExpired'),key+':'+mode);
  }
  for(const change of [p=>p.bindingProof.originalHandoffImmutable=false,p=>p.bindingProof.recoveryDeadlinePreserved=false,
    p=>p.records[3].message.sessionId='other-session']){
    const p=structuredClone(x);change(p);assert.throws(()=>live.verifyTargetRecoveryWire(p,'targetRecoveryExpired'));
  }
});
