// 文件说明：已接管目标的真实控制恢复验收；本机双false与原MQTT证据分别验证，不借初建源自动采集规则。
'use strict';
const assert=require('node:assert/strict');
const endpoint=require('./browser_ai_endpoint_live.cjs');

// projectPublicRecoveryFacts是持久报告的唯一出口；原wire/私有绑定只在RAM核对，不传播身份或授权字段。
function projectPublicRecoveryFacts(facts) {
  const booleans=['originalSessionPreserved','endpointAdvancedOnce','oldActualFullGroupReleased','freshActualFullGroupAcquired','noAutomaticCapture','userStopPreserved','actualCaptureStopped','actualOutputStopped','originalRecoveryDeadlineExpired','reconnectAfterExpiryRejected','originalDeadlinePreserved','uniqueNetworkTimeout','sameOriginalRecoveryScopeVerified','noLateInputOrAcquire','strictNewPhysicalEpoch','firstCapabilityWithoutInventedBaseline','originalSnapshotPreserved','oldOriginalFocusReleased','secondActualBaselineVerified','freshOriginalFocusAcquired','oldInputNotReplayed','capabilityAcknowledgementsVerified','recoveredSourceSecondTransferVerified'];
  const result={};
  for(const key of booleans)if(Object.hasOwn(facts,key)){assert.equal(typeof facts[key],'boolean');result[key]=facts[key];}
  for(const key of ['recoveryMilliseconds','expiredMilliseconds'])if(Object.hasOwn(facts,key)){assert.ok(Number.isFinite(facts[key])&&facts[key]>=0);result[key]=facts[key];}
  return result;
}

// targetControlDisconnect实际双false后返回，允许原预算内唯一只读lost检查，不向浏览器注入私有账本。
async function targetControlDisconnect() {
  const fact=globalThis.__urAIControl,started=performance.now(),epoch=aiControlState.endpointEpoch;
  fact.lossInput=fact.lastInput;fact.lossPeer=useWebRTC?rtcPeer:null;fact.lossPeerRevision=useWebRTC?rtcPeerRevision:'';
  const wait=async(condition,limit)=>{while(!condition()){if(performance.now()-started>=limit)throw Error('target_control_window_timeout');await new Promise(resolve=>setTimeout(resolve,20));}};
  if(!send({kind:'controlDisconnect'}))throw Error('disconnect_send_failed');
  await wait(()=>aiControlState?.state==='disconnected'&&metrics.captureOpen===false&&responseId==='',5000);
  globalThis.__urTargetRecoveryWindow={started,epoch};
  return {actualCaptureClosed:true,actualOutputStopped:true,originalSessionPreserved:aiControlState.originalSessionPreserved,endpointEpoch:aiControlState.endpointEpoch};
}

// targetControlReconnect沿用原断连起点；只读checkpoint耗时同样计入15秒，不创建第二预算或自动采集。
async function targetControlReconnect(variant) {
  const {started,epoch}=globalThis.__urTargetRecoveryWindow;
  const wait=async(condition,limit)=>{while(!condition()){if(performance.now()-started>=limit)throw Error('target_control_window_timeout');await new Promise(resolve=>setTimeout(resolve,20));}};
  if(variant==='targetRecoveryExpired') {
    await wait(()=>terminated&&context?.state==='closed'&&stream.getAudioTracks().every(x=>x.readyState==='ended'),20000);
    return {expired:true,reconnectAfterExpiryRejected:send({kind:'controlReconnect'})===false,elapsed:performance.now()-started};
  }
  if(!send({kind:'controlReconnect'}))throw Error('reconnect_send_failed');
  await wait(()=>aiControlState?.state==='ready'&&aiControlState.endpointEpoch===epoch+1&&aiEndpointState?.phase===7&&metrics.focusReceipts.filter(x=>x.operation==='acquire'&&x.code===200).length===2,14000);
  return {expired:false,elapsed:performance.now()-started};
}

// verifyTargetRecovery保留全部历史acquire/release，固定旧revision之后只有一次实际fresh组取得，仍等待本人新start。
function verifyTargetRecovery(before,stopped,after,variant) {
  assert.ok(['targetRecoveryActive','targetRecoveryStopped'].includes(variant));
  assert.equal(before.control?.state,'ready');assert.equal(before.control.originalSessionPreserved,true);
  assert.equal(before.captureOpen,variant==='targetRecoveryActive');assert.equal(before.targetPhase,7);
  assert.equal(stopped.actualCaptureClosed,true);assert.equal(stopped.actualOutputStopped,true);assert.equal(stopped.originalSessionPreserved,true);
  assert.equal(stopped.endpointEpoch,before.control.endpointEpoch);assert.equal(stopped.expired,false);
  assert.ok(Number.isFinite(stopped.elapsed)&&stopped.elapsed>=0&&stopped.elapsed<15000);
  assert.equal(after.control?.state,'ready');assert.equal(after.control.endpointEpoch,before.control.endpointEpoch+1);
  assert.equal(after.control.originalSessionPreserved,true);assert.ok(Number.isSafeInteger(after.control.messageWatermark)&&after.control.messageWatermark>=before.control.messageWatermark);
  assert.equal(after.targetPhase,7);assert.equal(after.captureOpen,false);assert.equal(after.outputStopped,true);
  assert.equal(after.terminated,false);assert.equal(after.contextRunning,true);assert.equal(after.tracksEnded,false);assert.equal(after.contextClosed,false);
  for(const key of ['starts','recognized','completed','fixtureEnded','configurations'])assert.equal(after[key],before[key]);
  assert.equal(after.inputID,before.inputID); // 真新输入尚未由本人发起，恢复本身不能换ID重采。
  const old=endpoint.successfulFocus(before,'acquire');assert.equal(old.length,1);
  const acquired=endpoint.successfulFocus(after,'acquire'),released=endpoint.successfulFocus(after,'release');
  assert.equal(acquired.length,2);assert.equal(released.length,1);
  assert.deepEqual(acquired[0],old[0]);assert.ok(released[0].receiptIndex>old[0].receiptIndex&&released[0].revision>old[0].revision);
  assert.ok(acquired[1].receiptIndex>released[0].receiptIndex&&acquired[1].revision>released[0].revision);
  return {originalSessionPreserved:true,endpointAdvancedOnce:true,oldActualFullGroupReleased:true,freshActualFullGroupAcquired:true,noAutomaticCapture:true,userStopPreserved:variant==='targetRecoveryStopped',recoveryMilliseconds:stopped.elapsed};
}

// verifyExpired只认真实驱动关闭及原预算终止；未交付的release不伪造成功，后继权限须由wire证明保持失效。
function verifyExpired(before,stopped,after) {
  assert.equal(before.captureOpen,true);assert.equal(stopped.expired,true);assert.equal(stopped.actualCaptureClosed,true);assert.equal(stopped.actualOutputStopped,true);
  assert.ok(stopped.elapsed>=15000&&stopped.elapsed<20000);assert.equal(stopped.reconnectAfterExpiryRejected,true);
  // release将线程意图置null；真实轨/上下文终止独立证明停采，不能补造false。
  assert.ok(after.captureOpen===false||after.captureOpen===null);assert.equal(after.outputStopped,true);assert.equal(after.terminated,true);assert.equal(after.tracksEnded,true);assert.equal(after.contextClosed,true);
  assert.equal(after.starts,before.starts);assert.equal(after.recognized,before.recognized);assert.equal(after.completed,before.completed);
  assert.equal(endpoint.successfulFocus(after,'acquire').length,1);
  return {actualCaptureStopped:true,actualOutputStopped:true,originalRecoveryDeadlineExpired:true,noAutomaticCapture:true,reconnectAfterExpiryRejected:true,expiredMilliseconds:stopped.elapsed};
}

// verifyTargetRecoveryWire验证只读原连接观察器的严格关联；原标识仅在RAM比较，输出只有证明布尔。
// witness由中央同连接observer提供，不允许页面自己回报true或缺原字段时补默认。
function verifyTargetRecoveryWire(witness,variant) {
  assert.ok(Array.isArray(witness?.records));witness=deriveTargetRecoveryWire(witness,variant);
  assert.ok(witness&&typeof witness==='object'&&!Array.isArray(witness));
  for(const key of ['sessionId','oldInputId','oldInstance'])assert.ok(typeof witness[key]==='string'&&witness[key].length>0);
  for(const key of ['oldConnectionEpoch','oldEndpointEpoch','capabilityRevision'])assert.ok(Number.isSafeInteger(witness[key])&&witness[key]>0);
  assert.equal(witness.originalDeadlinePreserved,true);assert.equal(witness.originalHandoffImmutable,true);assert.equal(witness.oldInputNotReplayed,true);
  if(variant==='targetRecoveryExpired') {
    assert.equal(witness.terminalReason,'networkTimeout');assert.equal(witness.terminalCount,1);assert.equal(witness.newInputCount,0);assert.equal(witness.freshAcquireCount,0);
    return {originalDeadlinePreserved:true,uniqueNetworkTimeout:true,sameOriginalRecoveryScopeVerified:true,noLateInputOrAcquire:true};
  }
  const {firstReport,snapshot,oldAcquire,oldRelease,secondReport,freshAcquire,freshReceipt}=witness;
  for(const value of [firstReport,snapshot,oldAcquire,oldRelease,secondReport,freshAcquire,freshReceipt])assert.ok(value&&typeof value==='object'&&!Array.isArray(value));
  assert.ok(Number.isSafeInteger(firstReport.connectionEpoch)&&firstReport.connectionEpoch>witness.oldConnectionEpoch);
  assert.ok(typeof firstReport.instance==='string'&&firstReport.instance&&firstReport.instance!==witness.oldInstance);
  assert.equal(firstReport.capabilityRevision,witness.capabilityRevision);assert.equal(firstReport.baseline,null);
  assert.equal(snapshot.sessionId,witness.sessionId);assert.equal(snapshot.endpointEpoch,witness.oldEndpointEpoch+1);assert.equal(snapshot.capabilityRevision,witness.capabilityRevision);
  assert.equal(snapshot.tracksPreserved,true);assert.equal(snapshot.mediaRevisionPreserved,true);assert.equal(snapshot.messageWatermarkNotRegressed,true);assert.equal(snapshot.eventWatermarkNotRegressed,true);
  assert.ok(typeof oldAcquire.focusId==='string'&&oldAcquire.focusId);assert.ok(typeof oldRelease.token==='string'&&oldRelease.token);
  assert.equal(oldRelease.focusId,oldAcquire.focusId);assert.equal(oldRelease.sessionId,witness.sessionId);assert.equal(oldRelease.instance,firstReport.instance);assert.equal(oldRelease.operation,'release');assert.equal(oldRelease.code,200);
  assert.deepEqual(oldRelease.resources,['microphone','speaker']);assert.equal(oldRelease.state,'released');assert.ok(Number.isSafeInteger(oldRelease.revision)&&oldRelease.revision>oldAcquire.revision);
  assert.equal(secondReport.capabilityRevision,firstReport.capabilityRevision);assert.equal(secondReport.instance,firstReport.instance);assert.equal(secondReport.connectionEpoch,firstReport.connectionEpoch);assert.equal(secondReport.sameDeclarationExceptBaseline,true);
  assert.equal(secondReport.baseline?.revision,oldRelease.revision);assert.deepEqual(secondReport.baseline?.resources,['microphone','speaker']);assert.equal(secondReport.baseline?.state,'idle');
  assert.ok(typeof freshAcquire.focusId==='string'&&freshAcquire.focusId&&freshAcquire.focusId!==oldAcquire.focusId);
  assert.ok(typeof freshAcquire.token==='string'&&freshAcquire.token&&freshAcquire.token!==oldRelease.token);
  assert.equal(freshAcquire.sessionId,witness.sessionId);assert.equal(freshAcquire.expectedRevision,oldRelease.revision);assert.deepEqual(freshAcquire.resources,['microphone','speaker']);
  assert.equal(freshReceipt.focusId,freshAcquire.focusId);assert.equal(freshReceipt.token,freshAcquire.token);assert.equal(freshReceipt.code,200);assert.equal(freshReceipt.state,'acquired');assert.ok(Number.isSafeInteger(freshReceipt.revision)&&freshReceipt.revision>oldRelease.revision);
  const sequence=[firstReport.ordinal,snapshot.ordinal,oldRelease.ordinal,secondReport.ordinal,freshAcquire.ordinal,freshReceipt.ordinal];
  for(let i=0;i<sequence.length;i++)assert.ok(Number.isSafeInteger(sequence[i])&&sequence[i]>0&&(i===0||sequence[i]>sequence[i-1]));
  assert.equal(witness.noInputBeforeExplicitStart,true);
  return {strictNewPhysicalEpoch:true,firstCapabilityWithoutInventedBaseline:true,originalSnapshotPreserved:true,oldOriginalFocusReleased:true,secondActualBaselineVerified:true,freshOriginalFocusAcquired:true,noAutomaticCapture:true,originalDeadlinePreserved:true,oldInputNotReplayed:true,capabilityAcknowledgementsVerified:true};
}

// canonical只用于同caprev原结构比较；对象键顺序不影响合同，数组顺序与实际值不擅自归一。
function canonical(value) {if(Array.isArray(value))return value.map(canonical);if(value&&typeof value==='object')return Object.fromEntries(Object.keys(value).sort().map(key=>[key,canonical(value[key])]));return value;}

// wireRecords只接透明观察器完整原消息；串行物理连接编号不是登录epoch，原绑定由独立SQL只读proof提供。
function wireRecords(witness) {
  assert.equal(witness.failed,false);assert.equal(witness.overflow,false);assert.ok(Array.isArray(witness.records)&&witness.records.length>0&&witness.records.length<=192);
  const seen=new Map(),records=[];
  for(const [index,record]of witness.records.entries()) {
    assert.ok(Number.isSafeInteger(record.connection)&&record.connection>0&&record.connection<=8);assert.ok(['up','down'].includes(record.direction));assert.ok(record.message&&typeof record.message==='object'&&!Array.isArray(record.message));
    const m=record.message;assert.ok(typeof m.method==='string'&&typeof m.msgToken==='string'&&m.msgToken);assert.ok(Object.keys(m).every(key=>['msgToken','method','sessionId','params','data','code','eventSeq','messageSeq'].includes(key)));
    const key=JSON.stringify([record.connection,record.direction,m.method,m.msgToken]),body=JSON.stringify(canonical(m));
    if(seen.has(key)){assert.equal(seen.get(key),body);continue;}seen.set(key,body);records.push({...record,ordinal:index+1});
  }
  return records;
}

// deriveTargetRecoveryWire逐字节来源的MSG只留RAM：原token/session/full2/CAS、两cap和真实快照不能靠UI数字推断。
function deriveTargetRecoveryWire(witness,variant) {
  const records=wireRecords(witness),proof=witness.bindingProof;
  assert.ok(proof&&typeof proof==='object');for(const key of ['oldConnectionEpoch','newConnectionEpoch'])if(variant!=='targetRecoveryExpired'||key==='oldConnectionEpoch')assert.ok(Number.isSafeInteger(proof[key])&&proof[key]>0);
  assert.ok(typeof proof.oldInstance==='string'&&proof.oldInstance);assert.equal(proof.recoveryDeadlinePreserved,true);assert.equal(proof.originalHandoffImmutable,true);
  const one=values=>{assert.equal(values.length,1);return values[0];},full=value=>{assert.ok(Array.isArray(value)&&value.length===2);assert.deepEqual([...value].sort(),['microphone','speaker']);return [...value].sort();};
  const established=one(records.filter(x=>x.direction==='down'&&x.message.method==='sessionEstablished'));
  const session=established.message.sessionId,data=established.message.data;assert.ok(typeof session==='string'&&session);assert.ok(Number.isSafeInteger(data?.endpointEpoch)&&data.endpointEpoch>=2);assert.ok(Number.isSafeInteger(data.capabilityRevision)&&data.capabilityRevision>0);
  const initial=one(records.filter(x=>x.direction==='down'&&x.message.method==='focusControl'&&x.message.params?.operation==='acquire'&&x.connection===established.connection));
  assert.equal(initial.message.sessionId,session);full(initial.message.params.resources);
  const receiptFor=command=>one(records.filter(x=>x.direction==='up'&&x.message.method==='focusState'&&x.connection===command.connection&&x.message.msgToken===command.message.msgToken));
  const initialReceipt=receiptFor(initial);assert.equal(initialReceipt.message.code,200);assert.equal(initialReceipt.message.data?.state,'acquired');assert.equal(initialReceipt.message.data.focusId,initial.message.params.focusId);full(initialReceipt.message.data.resources);
  assert.equal(initialReceipt.message.sessionId,session);assert.equal(initialReceipt.message.data.revision,initial.message.params.expectedRevision+1);
  const starts=records.filter(x=>x.direction==='up'&&x.message.method==='audioStart'&&x.message.sessionId===session&&x.connection===established.connection);assert.ok(starts.length>0);const oldInput=starts.at(-1).message.params?.inputId;assert.ok(typeof oldInput==='string'&&oldInput);
  const common={sessionId:session,oldInputId:oldInput,oldInstance:proof.oldInstance,oldConnectionEpoch:proof.oldConnectionEpoch,oldEndpointEpoch:data.endpointEpoch,capabilityRevision:data.capabilityRevision,originalDeadlinePreserved:true,originalHandoffImmutable:true,oldInputNotReplayed:true};
  if(variant==='targetRecoveryExpired') {
    for(const key of ['originalModelPreserved','originalCreationBindingPreserved','expiredTerminalVerified'])assert.equal(proof[key],true);
    assert.equal(records.filter(x=>x.direction==='up'&&x.message.method==='capabilityReport'&&x.connection>established.connection).length,0);
    const after=records.filter(x=>x.ordinal>starts.at(-1).ordinal);assert.equal(after.filter(x=>x.direction==='up'&&x.message.method==='audioStart').length,0);
    assert.equal(after.filter(x=>x.direction==='down'&&x.message.method==='focusControl'&&x.message.params?.operation==='acquire').length,0);
    return {...common,terminalReason:proof.terminalReason,terminalCount:proof.terminalCount,newInputCount:0,freshAcquireCount:0};
  }
  assert.ok(proof.newConnectionEpoch>proof.oldConnectionEpoch);assert.ok(typeof proof.newInstance==='string'&&proof.newInstance&&proof.newInstance!==proof.oldInstance);
  const reports=records.filter(x=>x.direction==='up'&&x.message.method==='capabilityReport'&&x.connection>established.connection);assert.equal(reports.length,2);const [first,second]=reports;assert.equal(first.connection,second.connection);
  // 两份能力均须原token成功回执；只见上行声明不能称平台已接受真实baseline。
  const capabilityAck=report=>{const ack=one(records.filter(x=>x.direction==='down'&&x.connection===report.connection&&x.message.method==='requestAck'&&x.message.msgToken===report.message.msgToken));assert.equal(ack.message.sessionId??'','');assert.equal(ack.message.code,200);assert.equal(ack.message.data?.requestMethod,'capabilityReport');assert.equal(ack.message.data.result?.acceptedRevision,report.message.params.revision);assert.ok(ack.ordinal>report.ordinal);return ack;};
  const firstAck=capabilityAck(first),secondAck=capabilityAck(second);
  const cap=first.message.params;assert.ok(cap&&cap.revision===data.capabilityRevision);for(const feature of ['audio','sessionSync','localAccept','aiEndpointHandoff','aiEndpointControlSync'])assert.ok(cap.features?.includes(feature));assert.equal(Object.hasOwn(cap.controlCapabilities,'baseline'),false);
  const sync=one(records.filter(x=>x.direction==='up'&&x.message.method==='sessionSync'&&x.connection===first.connection));assert.equal(sync.message.sessionId,session);assert.equal(sync.message.params?.endpointEpoch,data.endpointEpoch);assert.equal(sync.message.params.mediaRevision,data.mediaRevision);
  const snap=one(records.filter(x=>x.direction==='down'&&x.message.method==='sessionSnapshot'&&x.connection===first.connection&&x.message.msgToken===sync.message.msgToken));assert.equal(snap.message.code,200);assert.equal(snap.message.sessionId,session);const sd=snap.message.data;
  assert.equal(sd?.state,'established');assert.equal(sd.resumeAllowed,true);assert.equal(sd.eventWatermark,snap.message.eventSeq);assert.equal(sd.controls?.ai?.inputState,'paused');assert.equal(sd.controls.ai.inputId,oldInput);assert.deepEqual(sd.controls.ai.activeRespIds,[]);
  const invite=one(records.filter(x=>x.direction==='down'&&x.message.method==='sessionInvite'&&x.message.sessionId===session)),tracks=invite.message.params?.tracks;assert.ok(Array.isArray(tracks)&&tracks.length===2);assert.ok(Array.isArray(sd.tracks)&&sd.tracks.length===2);
  const track=value=>canonical(Object.fromEntries(['trackId','kind','sourceId','direction','codec'].map(key=>[key,value[key]])));
  assert.deepEqual(sd.tracks.map(track),tracks.map(track));assert.ok(sd.tracks.every(x=>x.state==='paused'));assert.deepEqual(sd.controls.mediaPlan.tracks,sd.tracks);assert.equal(sd.controls.mediaPlan.revision,sd.mediaRevision);
  const release=one(records.filter(x=>x.direction==='down'&&x.message.method==='focusControl'&&x.connection===first.connection&&x.message.params?.operation==='release'&&x.message.params.focusId===initial.message.params.focusId));const rr=receiptFor(release);
  assert.equal(release.message.params.expectedRevision,initialReceipt.message.data.revision);assert.equal(sd.controls?.focus?.focusId,initial.message.params.focusId);assert.equal(sd.controls.focus.state,'acquired');assert.equal(sd.controls.focus.revision,initialReceipt.message.data.revision);full(sd.controls.focus.resources);
  assert.equal(release.message.sessionId,session);assert.equal(rr.message.sessionId,session);assert.equal(rr.message.code,200);assert.equal(rr.message.data?.state,'released');assert.equal(rr.message.data.focusId,release.message.params.focusId);assert.equal(rr.message.data.revision,release.message.params.expectedRevision+1);full(rr.message.data.resources);full(release.message.params.resources);
  const fresh=one(records.filter(x=>x.direction==='down'&&x.message.method==='focusControl'&&x.connection===first.connection&&x.message.params?.operation==='acquire'));const fr=receiptFor(fresh);assert.equal(fr.message.sessionId,session);assert.equal(fr.message.code,200);assert.equal(fr.message.data?.state,'acquired');assert.equal(fr.message.data.focusId,fresh.message.params.focusId);assert.equal(fr.message.data.revision,fresh.message.params.expectedRevision+1);full(fr.message.data.resources);full(fresh.message.params.resources);
  assert.ok(firstAck.ordinal<sync.ordinal&&secondAck.ordinal<fresh.ordinal);
  assert.ok(Number.isSafeInteger(fresh.message.params.expiresAt)&&fresh.message.params.expiresAt<=initial.message.params.expiresAt);
  const baseline=second.message.params?.controlCapabilities?.baseline;const strip=value=>{const c=structuredClone(value);delete c.controlCapabilities.baseline;return canonical(c);};assert.deepEqual(strip(second.message.params),strip(cap));
  assert.equal(records.filter(x=>x.direction==='up'&&x.message.method==='audioStart'&&x.connection===first.connection).length,0);
  return {...common,noInputBeforeExplicitStart:true,firstReport:{connectionEpoch:proof.newConnectionEpoch,instance:proof.newInstance,capabilityRevision:cap.revision,baseline:null,ordinal:first.ordinal},snapshot:{sessionId:session,endpointEpoch:sd.endpointEpoch,capabilityRevision:sd.capabilityRevision,tracksPreserved:true,mediaRevisionPreserved:sd.mediaRevision===data.mediaRevision,messageWatermarkNotRegressed:sd.messageWatermark>=sync.message.params.lastMessageSeq,eventWatermarkNotRegressed:sd.eventWatermark>=sync.message.params.lastEventSeq,ordinal:snap.ordinal},oldAcquire:{focusId:initial.message.params.focusId,revision:initialReceipt.message.data.revision},oldRelease:{focusId:release.message.params.focusId,token:release.message.msgToken,sessionId:session,instance:proof.newInstance,operation:'release',code:rr.message.code,state:rr.message.data.state,resources:full(rr.message.data.resources),revision:rr.message.data.revision,ordinal:rr.ordinal},secondReport:{capabilityRevision:second.message.params.revision,instance:proof.newInstance,connectionEpoch:proof.newConnectionEpoch,sameDeclarationExceptBaseline:true,baseline:{state:baseline?.state,revision:baseline?.revision,resources:baseline?.resources?[...baseline.resources].sort():null},ordinal:second.ordinal},freshAcquire:{focusId:fresh.message.params.focusId,token:fresh.message.msgToken,sessionId:session,expectedRevision:fresh.message.params.expectedRevision,resources:full(fresh.message.params.resources),ordinal:fresh.ordinal},freshReceipt:{focusId:fr.message.data.focusId,token:fr.message.msgToken,code:fr.message.code,state:fr.message.data.state,revision:fr.message.data.revision,ordinal:fr.ordinal}};
}

// verifyRecoveredSourceTransfer第二跳只释放恢复后的当前完整组；旧release不能再次计作当前源释放。
function verifyRecoveredSourceTransfer(before,after,target) {
  const acquires=endpoint.successfulFocus(before,'acquire'),oldReleases=endpoint.successfulFocus(before,'release');assert.equal(acquires.length,2);assert.equal(oldReleases.length,1);
  assert.ok(oldReleases[0].receiptIndex<acquires[1].receiptIndex);
  assert.equal(after.terminated,true);assert.equal(after.tracksEnded,true);assert.equal(after.contextClosed,true);
  const releases=endpoint.successfulFocus(after,'release',acquires[1].receiptIndex);assert.equal(releases.length,1);assert.ok(releases[0].revision>acquires[1].revision);
  assert.equal(endpoint.successfulFocus(after,'release').length,2);assert.equal(target.targetPhase,7);assert.equal(target.captureOpen,false);assert.equal(target.terminated,false);assert.equal(target.contextRunning,true);assert.equal(endpoint.successfulFocus(target,'acquire').length,1);
  return {sourceHadFullGroup:true,sourceFreshOriginalFullGroupReleased:true,targetActualFullGroupAcquired:true,sourceActualTracksStopped:true,sourceActualOutputClosed:true};
}

// verifyRecoveredTransferWire关联原fresh实际取得ACK、原CAS及有序释放；上一旧focus或任意更高revision不得冒充第二跳。
function verifyRecoveredTransferWire(witness) {
  const records=wireRecords(witness);assert.equal(witness.bindingProof?.originalHandoffImmutable,true);
  const first=records.find(x=>x.direction==='up'&&x.message.method==='sessionSync');assert.ok(first);
  assert.ok(typeof first.message.sessionId==='string'&&first.message.sessionId);
  const acquire=records.filter(x=>x.direction==='down'&&x.connection===first.connection&&x.message.method==='focusControl'&&x.message.params?.operation==='acquire');assert.equal(acquire.length,1);const original=acquire[0];
  assert.ok(typeof original.message.params.focusId==='string'&&original.message.params.focusId);assert.ok(Number.isSafeInteger(original.message.params.expectedRevision)&&original.message.params.expectedRevision>=0);
  assert.equal(original.message.sessionId,first.message.sessionId);
  const obtained=records.filter(x=>x.direction==='up'&&x.connection===original.connection&&x.message.method==='focusState'&&x.message.msgToken===original.message.msgToken);assert.equal(obtained.length,1);const actual=obtained[0];
  assert.equal(actual.message.sessionId,original.message.sessionId);assert.equal(actual.message.code,200);assert.equal(actual.message.data?.state,'acquired');assert.equal(actual.message.data.focusId,original.message.params.focusId);assert.equal(actual.message.data.revision,original.message.params.expectedRevision+1);
  assert.ok(Number.isSafeInteger(actual.message.data.revision));
  assert.deepEqual([...original.message.params.resources].sort(),['microphone','speaker']);assert.deepEqual([...actual.message.data.resources].sort(),['microphone','speaker']);assert.ok(actual.ordinal>original.ordinal);
  const release=records.filter(x=>x.direction==='down'&&x.connection===first.connection&&x.message.method==='focusControl'&&x.message.params?.operation==='release'&&x.message.params.focusId===original.message.params.focusId);assert.equal(release.length,1);const command=release[0];
  const receipt=records.filter(x=>x.direction==='up'&&x.connection===command.connection&&x.message.method==='focusState'&&x.message.msgToken===command.message.msgToken);assert.equal(receipt.length,1);const reply=receipt[0];
  assert.equal(command.message.sessionId,original.message.sessionId);assert.equal(command.message.params.expectedRevision,actual.message.data.revision);assert.ok(command.ordinal>actual.ordinal);assert.equal(reply.message.sessionId,command.message.sessionId);assert.equal(reply.message.code,200);assert.equal(reply.message.data?.state,'released');assert.equal(reply.message.data.focusId,original.message.params.focusId);assert.equal(reply.message.data.revision,command.message.params.expectedRevision+1);
  assert.deepEqual([...command.message.params.resources].sort(),['microphone','speaker']);assert.deepEqual([...reply.message.data.resources].sort(),['microphone','speaker']);assert.ok(command.ordinal>original.ordinal&&reply.ordinal>command.ordinal);
  return {freshOriginalFocusReleased:true,sourceSocketIsRecoveredSocket:true,originalHandoffImmutable:true};
}

// executeRecovery只使用本人已有页面入口；无线旁证不可用就失败，不以本机计数代替协议验收。
async function executeRecovery(settings,devices,progress,journal,witnessPort,checkpointPort) {
  assert.equal(typeof witnessPort,'function');assert.equal(typeof checkpointPort,'function');const [,b,c]=devices;const variant=settings.variant;
  if(variant!=='targetRecoveryStopped'){await b.invoke('click','#start');await b.wait('metrics.captureOpen === true');}
  const before=await b.facts();await checkpointPort('B','before');progress('target_control_disconnect');
  const loss=await b.evaluate('('+targetControlDisconnect.toString()+')()');await checkpointPort('B','lost');
  // 重连沿原恢复起点与期限执行，仅把失败阶段与断连动作区分。
  progress('target_control_reconnect');
  const stopped={...loss,...await b.evaluate('('+targetControlReconnect.toString()+')('+JSON.stringify(variant)+')')};const after=await b.facts();
  await checkpointPort('B',variant==='targetRecoveryExpired'?'ended':'after');
  progress('target_recovery_wire');const wire=verifyTargetRecoveryWire(await witnessPort('B',variant),variant);
  journal.recoveries=[{variant,...projectPublicRecoveryFacts({... (variant==='targetRecoveryExpired'?verifyExpired(before,stopped,after):verifyTargetRecovery(before,stopped,after,variant)),...wire})}];
  if(variant==='targetRecoveryExpired') {
    // 清理前保存当前HAL与原wire同scope证明；不拿cleanup后状态或页面自报终态替代正式原旁证。
    journal.expiredRetirementBeforeCleanup={variant,role:'B',scenarioComplete:true,...journal.recoveries[0],
      originalInputPreserved:typeof before.inputID==='string'&&before.inputID.length>0&&after.inputID===before.inputID,
      originalEndpointPreserved:Number.isSafeInteger(before.control?.endpointEpoch)&&before.control.endpointEpoch>0&&after.control?.endpointEpoch===before.control.endpointEpoch,
      terminated:after.terminated,tracksEnded:after.tracksEnded,contextClosed:after.contextClosed,
      successfulAcquires:endpoint.successfulFocus(after,'acquire').length};
    return;
  }
  if(variant==='targetRecoveryStopped'){
    await b.invoke('wait','500');const settled=await b.facts();assert.equal(settled.captureOpen,false);assert.equal(settled.starts,before.starts);assert.equal(settled.captured,after.captured);return;
  }
  progress('target_explicit_new_input');const recovered=await b.audioRound(false),newInput=await b.facts();assert.notEqual(newInput.inputID,before.inputID);assert.equal(newInput.starts,after.starts+1);
  journal.rounds.push({round:'target_recovered_audio',...recovered});
  progress('recovered_source_prepare');await b.invoke('click','#start');await b.wait('metrics.captureOpen === true');const sourceBefore=await b.facts();
  await b.evaluate('globalThis.__urEndpointPrepareAt=performance.now();({submitted:true})');await b.invoke('fill','#endpointSourceTarget',c.target);await b.invoke('click','#endpointSourcePrepare');await b.wait('aiEndpointSourceState?.state === "preparing"');await c.wait('aiEndpointState?.phase === 2 && !element("endpointAccept").disabled');
  await c.invoke('click','#endpointAccept');await b.wait('aiEndpointSourceState?.state === "ready" && !element("endpointSourceCommit").disabled');await c.wait('aiEndpointState?.phase === 4 && !terminated && context?.state === "running"');
  const ready=endpoint.verifyCandidateReady(await b.facts(),await c.facts()),preserved=endpoint.verifyPreserved(sourceBefore,await b.facts(),await c.facts());
  progress('recovered_source_precommit_audio');journal.rounds.push({round:'recovered_source_precommit_audio',...await b.audioRound(true)});
  await b.invoke('click','#start');await b.wait('metrics.captureOpen === true');const atCommit=await b.facts();progress('recovered_source_commit');await b.invoke('click','#endpointSourceCommit');
  await c.wait('aiEndpointState?.phase === 7 && metrics.focusReceipts.some(x=>x.operation === "acquire" && x.code === 200)');await b.wait('terminated && context?.state === "closed" && stream.getAudioTracks().every(x=>x.readyState === "ended") && metrics.focusReceipts.filter(x=>x.operation === "release" && x.code === 200).length === 2');
  journal.transfers.push({...ready,...preserved,...verifyRecoveredSourceTransfer(atCommit,await b.facts(),await c.facts())});
  progress('recovered_target_audio');journal.rounds.push({round:'recovered_target_audio',...await c.audioRound(false)});
  const next=verifyRecoveredTransferWire(await witnessPort('B','recoveredSourceTransferred'));assert.equal(next.freshOriginalFocusReleased,true);
  journal.recoveries[0].recoveredSourceSecondTransferVerified=true;
}

// run复用原三设备生命周期；新模式缺只读wire端口时立即拒绝，旧commit/abort/timeout不能误走新路径。
async function run(env=process.env,deps={}) {
  assert.ok(['targetRecoveryActive','targetRecoveryStopped','targetRecoveryExpired'].includes(env.UR_AI_ENDPOINT_VARIANT));assert.equal(typeof deps.targetRecoveryWitness,'function');assert.equal(typeof deps.targetRecoveryCheckpoint,'function');
  return endpoint.run(env,{...deps,afterBootstrap:(settings,devices,progress,journal)=>executeRecovery(settings,devices,progress,journal,deps.targetRecoveryWitness,deps.targetRecoveryCheckpoint)});
}
module.exports={projectPublicRecoveryFacts,targetControlDisconnect,targetControlReconnect,verifyTargetRecovery,verifyExpired,verifyTargetRecoveryWire,deriveTargetRecoveryWire,wireRecords,verifyRecoveredSourceTransfer,verifyRecoveredTransferWire,executeRecovery,run};
