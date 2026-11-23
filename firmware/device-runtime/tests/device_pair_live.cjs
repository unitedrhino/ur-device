// 文件说明：显式真实后端验收；两台 C++ 设备各自持有 MQTT 身份，浏览器只充当可替换的媒体驱动。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {spawn, execFile} = require('node:child_process');
const {randomUUID} = require('node:crypto');

// browser 仅驱动本测试创建的浏览器会话，报错时不输出页面中的设备凭据。
function browser(session, env, ...args) {
  return new Promise((resolve, reject) => execFile('agent-browser', ['--session', session, ...args],
    {env, timeout: args[0] === 'wait' ? 65000 : 40000, maxBuffer: 262144},
    (error, output) => error ? reject(Error(`browser_${args[0]}_failed`)) :
      resolve(output.trim())));
}

// decoded 解开浏览器 CLI 的 JSON 包装，不把任意页面内容原样打印到日志。
function decoded(value) {
  let result = JSON.parse(value);
  while (typeof result === 'string' && /^[{[\"]/.test(result)) {
    try { result = JSON.parse(result); } catch { break; }
  }
  return result;
}
// selectScenario 精确限定正式单次场景，非法选择在spawn或API之前失败，不静默跑默认样本。
function selectScenario(value) {
  if (value===undefined || value==='') return 'default';
  assert.ok(['reject','disconnect_caller','disconnect_callee','hold_resume'].includes(value), 'scenario_invalid');
  return value;
}

// sameOriginal 固定原会话、Peer代次和SDP摘要，不能用新的连通Peer补作原保持/恢复。
function sameOriginal(baseline, current) {
  assert.ok(baseline.sessionId && baseline.peerEpoch && baseline.peerFingerprint);
  assert.equal(current.sessionId, baseline.sessionId);
  assert.equal(current.peerEpoch, baseline.peerEpoch);
  assert.equal(current.peerFingerprint, baseline.peerFingerprint);
  assert.equal(current.connected, true);
  assert.equal(current.sameOriginalTracks, true);
}

// heldPair 原两个端各自Actual停轨与末端归零齐全，再核已稳定窗口的双向RTP没有继续增长。
function heldPair(baselines, samples) {
  assert.equal(samples.length, 2);
  samples.forEach((sample, index) => {
    sameOriginal(baselines[index], sample.first);
    sameOriginal(baselines[index], sample.second);
    assert.equal(sample.first.actualHeld, true);
    assert.equal(sample.first.detachedSenders, 2);
    assert.equal(sample.first.outputEnabled, false);
    assert.equal(sample.first.videoPaused, true);
    assert.equal(sample.first.videoHidden, true);
    assert.ok(sample.second.audioZeroBlocks > sample.first.audioZeroBlocks);
    assert.equal(sample.second.audioNonSilentBlocks, sample.first.audioNonSilentBlocks);
    for (const key of ['audioPackets','videoPackets','outAudioPackets','outVideoPackets'])
      assert.equal(sample.second[key], sample.first[key]);
  });
}

// resumedPair 同原Peer显式恢复原轨并实际收发媒体，平台active或恢复点击不能代替这些事实。
function resumedPair(baselines, held, resumed) {
  assert.equal(resumed.length, 2);
  resumed.forEach((value, index) => {
    sameOriginal(baselines[index], value);
    assert.equal(value.actualHeld, false);
    assert.equal(value.detachedSenders, 0);
    assert.equal(value.outputEnabled, true);
    assert.equal(value.videoPaused, false);
    assert.equal(value.videoHidden, false);
    assert.ok(value.audioPackets > held[index].second.audioPackets);
    assert.ok(value.videoFrames > held[index].second.videoFrames);
    assert.ok(value.outAudioPackets > held[index].second.outAudioPackets);
    assert.ok(value.outVideoPackets > held[index].second.outVideoPackets);
    assert.ok(value.audioNonSilentBlocks > held[index].second.audioNonSilentBlocks);
  });
}

// originalControl 只收原操作200/Completed与本端Actual，拒绝与未知不能被非空收据解释成成功。
function originalControl(receipts, operation) {
  const matches = receipts.filter(value => value.operation === operation && value.state === 'completed');
  assert.equal(matches.length, 1);
  const value = matches[0];
  assert.equal(value.code, 200);
  assert.equal(value.actualApplied, true);
  assert.equal(value.actualHeld, operation === 'hold');
  assert.ok(value.controlToken && Number.isSafeInteger(value.revision) && value.revision > 0);
  return value;
}

// childClose 保留真实终态事件Promise，不能用kill发送、PID消失或超时猜进程已经退出。
function childClose(child) {
  let result;
  // streams 只旁观原流end，关闭句柄或exit不能替代双流消费完成。
  const streams={stdoutEnded:false,stderrEnded:false};
  child.stdout?.once('end',()=>{streams.stdoutEnded=true;});
  child.stderr?.once('end',()=>{streams.stderrEnded=true;});
  const closed = new Promise(resolve => child.once('close', (code, signal) => {
    result = {code, signal}; resolve(result);
  }));
  return {exited: () => !!result, facts:()=>childExitFacts(result,streams), wait: async () => {
    if (result) return result;
    let timer;
    try {
      return await Promise.race([closed, new Promise((_, reject) => {
        timer=setTimeout(()=>reject(Error('device_close_timeout')),10000);
      })]);
    } finally { clearTimeout(timer); }
  }};
}

// childExitFacts 仅投真实close的有限退出码/信号与双流end；未知信号不输出自由值。
function childExitFacts(result,streams) {
  const exitCode=Number.isSafeInteger(result?.code)&&result.code>=0&&result.code<=255?result.code:null;
  const signal=result?.signal===null?'none':['SIGTERM','SIGKILL','SIGINT','SIGABRT','SIGSEGV'].includes(result?.signal)?result.signal:'unknown';
  return {closed:!!result,exitCode,signal,stdoutEnded:streams.stdoutEnded===true,
    stderrEnded:streams.stderrEnded===true};
}

// observeOriginalSpawn 仅旁观原真实ChildProcess；观察器异常不改变spawn、ACK、媒体判据或原finally收尾。
function observeOriginalSpawn(child, observer) {
  if (observer === undefined) return true;
  try { observer(child); return true; } catch { return false; }
}

// deviceFailureObservation 只旁观原设备的固定stderr marker；有界UTF8解析不改变原流、退出或业务判据。
function deviceFailureObservation(stream) {
  const reasons=['none','control_receipt','browser_bridge','browser_message','duplicate_ready','device_start',
    'before_start','call_dial','call_accept','call_reject','call_offer','call_connected','call_track',
    'control_rejected_delivery','call_hangup','call_actual_control','call_output_acquire','track_ack_delivery','state_delivery','media_command'];
  const state={seen:false,reason:null,previousState:null,runtimeDiagnostic:null,unknown:false,truncated:false,records:0};
  let pending='',bytesSeen=0,stopped=false,ended=false;
  const decoder=new (require('node:util').TextDecoder)('utf-8',{fatal:true}); // 原分片跨UTF8字符时保留解码器状态。
  // stop 超限或非法UTF8仅停止诊断，不截断、暂停或销毁原stderr。
  const stop=()=>{state.unknown=true;state.truncated=true;stopped=true;pending='';};
  // line 只记录完整固定marker；库自由行丢弃，貌似marker但非法字段保留未知。
  const line=value=>{
    if(value.endsWith('\r'))value=value.slice(0,-1);
    if(Buffer.byteLength(value,'utf8')>1024){stop();return;}
    let field,parsed;
    if(value.startsWith('call_failed reason=')){
      field='reason';parsed=value.slice(19);if(!reasons.includes(parsed)){state.unknown=true;return;}
    }else if(value.startsWith('call_runtime_failed state=')){
      field='previousState';const number=value.slice(26);
      if(!/^(0|[1-9]|1[0-3])$/.test(number)){state.unknown=true;return;}parsed=Number(number);
    }else if(value.startsWith('call_runtime_diagnostic')){
      // 只接纳原header固定枚举和非成功ACK码；零code没有ACK拒绝原因，不保存自由后缀。
      const match=/^call_runtime_diagnostic failure=(0|[1-9]|1[0-4]) capabilityCode=(0|[2-5][0-9]{2}) capabilityReason=([0-4])$/.exec(value);
      if(!match){state.unknown=true;return;}
      const failure=Number(match[1]),capabilityCode=Number(match[2]),capabilityReason=Number(match[3]);
      if(capabilityCode===200||(capabilityCode===0&&capabilityReason!==0)){state.unknown=true;return;}
      field='runtimeDiagnostic';parsed={failure,capabilityCode,capabilityReason};
    }else return;
    if(state.records===3){state.unknown=true;state.truncated=true;return;}
    if(state[field]!==null){state.unknown=true;return;}
    state.seen=true;state.records++;state[field]=parsed;
  };
  // consume 仅接收原Buffer分片；异常隔离且无自由报文持久化，原其他data监听仍收到同一对象。
  const consume=bytes=>{if(stopped||ended)return;try{
    if(!Buffer.isBuffer(bytes)&&!(bytes instanceof Uint8Array)){stop();return;}
    bytesSeen+=bytes.byteLength;if(bytes.byteLength>4096||bytesSeen>65536){stop();return;}
    pending+=decoder.decode(bytes,{stream:true});
    for(let index;(index=pending.indexOf('\n'))>=0&&!stopped;){line(pending.slice(0,index));pending=pending.slice(index+1);}
    if(!stopped&&Buffer.byteLength(pending,'utf8')>1024)stop();
  }catch{stop();}};
  // finish 只在原end刷新完整末行，缺尾UTF8明确未知；不制造原end/close或额外等待。
  const finish=()=>{if(ended)return;ended=true;if(stopped)return;try{pending+=decoder.decode();if(pending)line(pending);pending='';}catch{stop();}};
  stream.on('data',consume);stream.once('end',finish);
  // facts 返回固定独立快照，旁观者不能经嵌套对象修改首次已采集的原失败事实。
  return {facts:()=>({...state,runtimeDiagnostic:state.runtimeDiagnostic?{...state.runtimeDiagnostic}:null,
    unknown:state.unknown||!state.seen})};
}

// bootDevice 为每台设备提供独立环境和回环桥，并要求其绑定成功后才继续。
async function bootDevice(binary, port, identity, mqttPort, register) {
  const env = {...process.env, UR_DEVICE_PRODUCT: identity.productID,
    UR_DEVICE_NAME: identity.deviceName, UR_DEVICE_SECRET: identity.secret,
    UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: String(mqttPort), UR_MQTT_CA: ''};
  const child = spawn(binary, [String(port)], {env, stdio: ['ignore', 'pipe', 'pipe']});
  const lifetime = childClose(child);
  const observation=deviceFailureObservation(child.stderr); // 原每个子进程独立有限证据，不读设备自由stderr。
  const record = {child, ...lifetime,facts:()=>({...lifetime.facts(),stderr:observation.facts()})};
  register(record); // spawn之后首个同步边界即登记，启动失败仍沿原child等待精确close。
    child.stderr.on('data', () => {}); // 持续消费原stderr；诊断截断不会停止原业务流。
  child.stdout.on('data', () => {}); // 启动观察移除后仍消费双流，不保存正文。
  await new Promise((resolve, reject) => {
    let prefix = ''; // 最多1024字符启动窗，正式循环不收集stdout正文。
    const finish = error => {
      clearTimeout(timer); child.stdout.removeListener('data', data);
      child.removeListener('close', ended); child.removeListener('error', failed);
      error ? reject(error) : resolve();
    };
    const ended = () => finish(Error('device_exited_before_bind'));
    const failed = () => finish(Error('device_spawn_failed'));
    const data = bytes => {
      prefix = (prefix + bytes.toString()).slice(-1024);
      if (prefix.includes(`http://127.0.0.1:${port}/`)) {
        finish();
      }
    };
    const timer = setTimeout(() => finish(Error('device_start_timeout')), 10000);
    child.once('error', failed); child.once('close', ended); child.stdout.on('data', data);
  });
  return record;
}

// audioFailureFacts 固定投射失败音频旁证；缺失、越界或自由值均为null，不推断有声或成功。
function audioFailureFacts(value) {
  const facts={}; // 数值仅为原统计的有限非负能量/归一化电平，不输出轨ID或统计对象。
  for(const key of ['sourceEnergy','sourceLevel','inboundEnergy','inboundLevel']){
    const number=value?.[key],limit=key.endsWith('Level')?1:10000000;
    facts[key]=typeof number==='number'&&Number.isFinite(number)&&number>=0&&number<=limit?number:null;
  }
  for(const key of ['localTrackEnabled','localTrackMuted','remoteTrackEnabled','remoteTrackMuted',
    'remoteSourcePresent','remoteElementBound','remoteElementPaused'])
    facts[key]=typeof value?.[key]==='boolean'?value[key]:null;
  for(const key of ['localTrackState','remoteTrackState'])
    facts[key]=['live','ended'].includes(value?.[key])?value[key]:null;
  facts.audioContextState=['running','suspended','closed','interrupted'].includes(value?.audioContextState)?value.audioContextState:null;
  return facts;
}

// controlSnapshot 只读原网页固定事实与原SDP摘要；新增音频旁证仅在原失败采样中读取，不注入媒体。
const controlSnapshotBody = `
  const stats=await peer.getStats();let audioPackets=0,videoPackets=0,outAudioPackets=0,outVideoPackets=0;
  const packetFields=new Set();
  stats.forEach(value=>{
    if(diagnostic&&['inbound-rtp','outbound-rtp'].includes(value.type)&&['audio','video'].includes(value.kind)){
      const count=value.type==='inbound-rtp'?value.packetsReceived:value.packetsSent;
      if(!Number.isSafeInteger(count)||count<0||count>10000000)throw Error('diagnostic_stat_unknown');
      packetFields.add(value.type+':'+value.kind);
    }
    if(value.type==='inbound-rtp'){if(value.kind==='audio')audioPackets+=value.packetsReceived||0;
      if(value.kind==='video')videoPackets+=value.packetsReceived||0;}
    if(value.type==='outbound-rtp'){if(value.kind==='audio')outAudioPackets+=value.packetsSent||0;
      if(value.kind==='video')outVideoPackets+=value.packetsSent||0;}
  });
  if(diagnostic&&packetFields.size!==4)throw Error('diagnostic_stat_unknown');
  const digest=await crypto.subtle.digest('SHA-256',new TextEncoder().encode(peer.localDescription.sdp));
  const peerFingerprint=Array.from(new Uint8Array(digest),value=>value.toString(16).padStart(2,'0')).join('');
  const tracks=capture.getTracks(),senders=peer.getSenders(),metrics=controlMetrics();
  let audioFacts;
  if(diagnostic){
    ${audioFailureFacts.toString()}
    // 原轨必须唯一；同kind不能代替trackIdentifier关联，重复来源不求和或猜第一项。
    const localTracks=tracks.filter(track=>track.kind==='audio');
    const remoteTracks=typeof peer.getReceivers==='function'?peer.getReceivers().map(receiver=>receiver.track).filter(track=>track?.kind==='audio'):[];
    const local=localTracks.length===1?localTracks[0]:null,remote=remoteTracks.length===1?remoteTracks[0]:null;
    const sources=[],inbounds=[];
    stats.forEach(value=>{
      if(local&&typeof local.id==='string'&&local.id&&value.type==='media-source'&&value.kind==='audio'&&value.trackIdentifier===local.id)sources.push(value);
      if(remote&&typeof remote.id==='string'&&remote.id&&value.type==='inbound-rtp'&&value.kind==='audio'&&value.trackIdentifier===remote.id)inbounds.push(value);
    });
    const source=sources.length===1?sources[0]:null,inbound=inbounds.length===1?inbounds[0]:null;
    const element=item('remoteAudio');let remoteElementBound=null;
    if(remote&&element){
      if(element.srcObject===null)remoteElementBound=false;
      else if(typeof element.srcObject?.getAudioTracks==='function'){
        const bound=element.srcObject.getAudioTracks();remoteElementBound=bound.length===1&&bound[0]===remote;
      }
    }
    audioFacts=audioFailureFacts({sourceEnergy:source?.totalAudioEnergy,sourceLevel:source?.audioLevel,
      inboundEnergy:inbound?.totalAudioEnergy,inboundLevel:inbound?.audioLevel,
      localTrackEnabled:local?.enabled,localTrackMuted:local?.muted,localTrackState:local?.readyState,
      remoteTrackEnabled:remote?.enabled,remoteTrackMuted:remote?.muted,remoteTrackState:remote?.readyState,
      audioContextState:typeof callAudio==='undefined'?null:callAudio?.state,
      remoteSourcePresent:typeof remoteAudioSource==='undefined'?null:remoteAudioSource!==null,
      remoteElementBound,remoteElementPaused:element?.paused});
  }
  return {...metrics,peerFingerprint,connected:peer.connectionState==='connected',
    sameOriginalTracks:tracks.length===2&&senders.length===2&&senders.every(sender=>
      tracks.includes(sender.originalCallTrack||sender.track)),
    detachedSenders:senders.filter(sender=>sender.track===null).length,
    videoPaused:item('remote').paused,videoHidden:item('remote').style.visibility==='hidden',
    audioPackets,videoPackets,outAudioPackets,outVideoPackets,videoFrames:reports.videoFrames,
    ...(diagnostic?{audioFacts}:{})};
`;
// controlSnapshot 保持原业务统计语义；controlDiagnosticSnapshot仅失败读取时额外拒绝缺失的RTP原字段。
const controlSnapshot=`(async()=>{const diagnostic=false;${controlSnapshotBody}})()`;
const controlDiagnosticSnapshot=`(async()=>{const diagnostic=true;${controlSnapshotBody}})()`;

// holdResume 操作仅走原UI，两个端分别证明实际门控，平台四向gate不替本端生成完成事实。
async function holdResume(sessions, env) {
  // step 仅标记原执行边界；baselines/tokens留闭包，失败旁证不能输出原身份或摘要。
  let step='audio_baseline',baselines;
  const tokens={};
  try {
  for (const session of sessions)
    await browser(session,env,'wait','--fn','outputEnabled && audioNonSilentBlocks > 0');
  baselines=await Promise.all(sessions.map(session=>browser(session,env,'eval',controlSnapshot).then(decoded)));
  step='hold_actual';
  await browser(sessions[0],env,'click','#hold');
  for (const session of sessions)
    await browser(session,env,'wait','--fn','actualHeld && !outputEnabled && peer.getSenders().every(sender=>sender.track===null)');
  step='hold_receipt';
  await browser(sessions[0],env,'wait','--fn',
    'controlReceipts.some(value=>value.operation==="hold" && value.state==="completed" && value.code===200 && value.actualApplied)');
  // 固定两段750ms媒体观察用于排除已在途RTP，不以裸等待作为业务成功条件。
  step='hold_stability';
  const samples=await Promise.all(sessions.map(session=>browser(session,env,'eval',`(async()=>{
    await new Promise(resolve=>setTimeout(resolve,750));const first=await ${controlSnapshot};
    await new Promise(resolve=>setTimeout(resolve,750));const second=await ${controlSnapshot};
    return {first,second};})()`).then(decoded)));
  heldPair(baselines,samples);
  const heldReceipt=originalControl(samples[0].second.receipts,'hold');
  tokens.hold=heldReceipt.controlToken;
  step='resume_actual';
  await browser(sessions[0],env,'click','#resume');
  for (let index=0;index<sessions.length;++index) {
    const sample=samples[index].second;
    await browser(sessions[index],env,'wait','--fn',`!actualHeld && outputEnabled &&
      reports.audioPackets>${sample.audioPackets} && reports.videoFrames>${sample.videoFrames} &&
      audioNonSilentBlocks>${sample.audioNonSilentBlocks}`);
  }
  step='resume_receipt';
  await browser(sessions[0],env,'wait','--fn',
    'controlReceipts.some(value=>value.operation==="resume" && value.state==="completed" && value.code===200 && value.actualApplied)');
  step='resume_stability';
  const resumed=await Promise.all(sessions.map(session=>browser(session,env,'eval',controlSnapshot).then(decoded)));
  resumedPair(baselines,samples,resumed);
  const resumedReceipt=originalControl(resumed[0].receipts,'resume');
  tokens.resume=resumedReceipt.controlToken;
  assert.notEqual(resumedReceipt.controlToken,heldReceipt.controlToken);
  assert.equal(resumedReceipt.revision,heldReceipt.revision+1);
  step='cancel_rejected';
  await browser(sessions[0],env,'click','#cancel');
  await browser(sessions[0],env,'wait','--fn',
    'controlReceipts.some(value=>value.operation==="cancel" && value.state==="rejected" && value.code===409 && value.reason==="invalidState")');
  const rejected=await Promise.all(sessions.map(session=>browser(session,env,'eval',controlSnapshot).then(decoded)));
  rejected.forEach((value,index)=>{sameOriginal(baselines[index],value);assert.equal(value.actualHeld,false);
    assert.equal(value.outputEnabled,true);assert.equal(value.detachedSenders,0);});
  return {holdCompleted:true,resumeCompleted:true,connectedCancelRejected:true,
    bothOriginalPeersRetained:true,bothActualGatesVerified:true};
  } catch(error) {
    // 失败后只并行读两页一次，原CLI有界；不追加业务等待、重用旧事实或吞掉第一失败。
    try {
      const snapshots=await Promise.all(sessions.map(async(session,index)=>{
        let facts;try{facts=controlFailureSnapshot(decoded(await browser(session,env,'eval',controlDiagnosticSnapshot)),baselines?.[index],tokens);}
        catch{facts={sampled:false};}
        return {side:index===0?'caller':'callee',...facts};
      }));
      console.error(JSON.stringify({kind:'pair_control_failure',step,snapshots}));
    }catch{ /* 诊断读取或sink异常不能覆盖原失败，也不能改变成功判据。 */ }
    throw error;
  }
}

// controlFailureSnapshot 严格投射原实态；缺失/越界整体未知，身份、token与SDP仅在闭包内比较。
function controlFailureSnapshot(value,baseline,tokens={}) {
  try {
    const facts={sampled:true};
    for(const key of ['actualHeld','outputEnabled','videoPaused','videoHidden','connected','sameOriginalTracks']){
      assert.equal(typeof value[key],'boolean');facts[key]=value[key];
    }
    assert.ok(Number.isSafeInteger(value.detachedSenders)&&value.detachedSenders>=0&&value.detachedSenders<=2);
    facts.detachedSenders=value.detachedSenders;
    for(const key of ['audioZeroBlocks','audioNonSilentBlocks','audioPackets','videoPackets','outAudioPackets','outVideoPackets','videoFrames']){
      assert.ok(Number.isSafeInteger(value[key])&&value[key]>=0&&value[key]<=10000000);facts[key]=value[key];
    }
    assert.ok(Array.isArray(value.receipts)&&value.receipts.length<=24);
    const original=value=>value&&['sessionId','peerEpoch','peerFingerprint'].every(key=>
      typeof value[key]==='string'&&value[key].length>0&&value[key].length<=128);
    assert.ok(original(value));
    facts.comparisonKnown=!!original(baseline)&&!!original(value);
    facts.matchedOriginal=facts.comparisonKnown&&value.connected&&value.sameOriginalTracks&&
      ['sessionId','peerEpoch','peerFingerprint'].every(key=>value[key]===baseline[key]);
    facts.receipts=value.receipts.map(receipt=>{
      assert.ok(['cancel','hold','resume'].includes(receipt.operation)&&['pending','completed','rejected','unknown'].includes(receipt.state));
      assert.ok(Number.isSafeInteger(receipt.code)&&((receipt.code>=100&&receipt.code<=599)||
        (receipt.code===0&&['pending','unknown'].includes(receipt.state))));
      assert.ok(receipt.state!=='completed'||receipt.code===200);
      assert.ok(Number.isSafeInteger(receipt.revision)&&receipt.revision>=0);
      assert.ok(receipt.code!==0||receipt.revision===0);
      assert.equal(typeof receipt.actualApplied,'boolean');assert.equal(typeof receipt.actualHeld,'boolean');
      assert.ok(typeof receipt.sessionId==='string'&&receipt.sessionId.length>0&&receipt.sessionId.length<=64&&
        typeof receipt.controlToken==='string'&&receipt.controlToken.length>0&&receipt.controlToken.length<=128);
      const comparisonKnown=facts.comparisonKnown&&typeof tokens[receipt.operation]==='string';
      return {operation:receipt.operation,state:receipt.state,code:receipt.code,revision:receipt.revision,
        actualApplied:receipt.actualApplied,actualHeld:receipt.actualHeld,comparisonKnown,
        matchedOriginal:comparisonKnown&&facts.matchedOriginal&&receipt.sessionId===baseline.sessionId&&receipt.controlToken===tokens[receipt.operation]};
    });
    facts.audioFacts=audioFailureFacts(value.audioFacts);
    return facts;
  }catch{return {sampled:false};}
}

// mediaResult 检查双向音频收包和实际视频解码及像素，而非仅检查 WebRTC 状态。
async function mediaResult(session, env) {
  await browser(session, env, 'wait', '--fn',
    'state === "connected" && reports.audioPackets > 3 && reports.videoFrames >= 2 && item("remote").videoWidth > 0');
  const report = decoded(await browser(session, env, 'eval', `(async()=>{
    const canvas=document.createElement('canvas');canvas.width=32;canvas.height=24;
    const context=canvas.getContext('2d');context.drawImage(item('remote'),0,0,32,24);
    const pixels=context.getImageData(0,0,32,24).data;let sum=0;
    for(let i=0;i<pixels.length;i+=4)sum+=pixels[i]+pixels[i+1]+pixels[i+2];
    return JSON.stringify({audioPackets:reports.audioPackets,videoFrames:reports.videoFrames,pixelSum:sum});
  })()`));
  assert.ok(report.audioPackets > 3 && report.videoFrames >= 2 && report.pixelSum > 1000);
  return report;
}

// run 覆盖呼叫、接听、实际双向音视频、轨控制、挂断和媒体资源释放。
// run 可选onSpawn只读观察原child；不提供spawn替换入口，缺省CLI行为与预算保持原样。
async function run({onSpawn} = {}) {
  assert.ok(onSpawn === undefined || typeof onSpawn === 'function', 'spawn_observer_invalid');
  assert.equal(process.env.UR_VIDEO_PAIR_LIVE, '1');
  const scenario=selectScenario(process.env.UR_VIDEO_PAIR_SCENARIO);
  if (scenario==='hold_resume') assert.equal(process.env.AGENT_BROWSER_HEADED,'1','actual_display_required');
  const binary = process.env.UR_VIDEO_DEVICE_BIN;
  const first = JSON.parse(process.env.UR_VIDEO_PAIR_FIRST || '{}');
  const second = JSON.parse(process.env.UR_VIDEO_PAIR_SECOND || '{}');
  const mqttPort = Number(process.env.UR_MQTT_PORT);
  const ports = [Number(process.env.UR_VIDEO_PAIR_PORT_FIRST), Number(process.env.UR_VIDEO_PAIR_PORT_SECOND)];
  assert.ok(binary && fs.statSync(binary).isFile());
  assert.ok(first.productID && first.deviceName && first.secret && second.productID && second.deviceName && second.secret);
  assert.notEqual(`${first.productID}:${first.deviceName}`, `${second.productID}:${second.deviceName}`);
  assert.ok(Number.isInteger(mqttPort) && mqttPort > 0);
  assert.ok(ports.every(port => Number.isInteger(port) && port >= 1024 && port <= 65535) && ports[0] !== ports[1]);
  const env = {...process.env, AGENT_BROWSER_ARGS: [process.env.AGENT_BROWSER_ARGS || '--no-sandbox',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
    '--autoplay-policy=no-user-gesture-required'].join(',')};
  const sessions = [`pair-caller-${randomUUID()}`, `pair-callee-${randomUUID()}`];
  const children = [];
  let stage = 'boot';
  // primaryFailure 保留业务失败阶段；收尾失败另作固定投影，不能覆盖首个失败。
  let primaryFailure=null;
  try {
    await bootDevice(binary, ports[0], first, mqttPort, child=>{
      children.push(child); observeOriginalSpawn(child.child, onSpawn);
    });
    await bootDevice(binary, ports[1], second, mqttPort, child=>{
      children.push(child); observeOriginalSpawn(child.child, onSpawn);
    });
    for (let i = 0; i < 2; i++) {
      await browser(sessions[i], env, 'open', `http://127.0.0.1:${ports[i]}/`);
      await browser(sessions[i], env, 'click', '#activate');
      await browser(sessions[i], env, 'wait', '--fn', 'state === "available"');
    }
    stage = 'dial';
    await browser(sessions[0], env, 'fill', '#target', `${second.productID}:${second.deviceName}`);
    await browser(sessions[0], env, 'click', '#dial');
    await browser(sessions[0], env, 'wait', '--fn', 'state === "inviting" || state === "negotiating"');
    await browser(sessions[1], env, 'wait', '--fn', 'state === "ringing"');
    if (process.env.UR_VIDEO_PAIR_SCENARIO === 'reject') {
      stage = 'reject';
      const rejectedAt = Date.now();
      await browser(sessions[1], env, 'click', '#reject');
      for (const session of sessions) {
        await browser(session, env, 'wait', '--fn', 'state === "available" && !peer && !capture');
      }
      assert.ok(children.every(child => !child.exited()));
      return {result:'pass',scenario:'cpp_device_pair_reject',
        elapsedMs:Date.now()-rejectedAt,mediaReleased:true};
    }
    stage = 'accept';
    await browser(sessions[1], env, 'click', '#accept');
    await browser(sessions[0], env, 'wait', '--fn', 'state === "connected" && peer?.connectionState === "connected"');
    await browser(sessions[1], env, 'wait', '--fn', 'state === "connected" && peer?.connectionState === "connected"');
    stage = 'decoded_media';
    const caller = await mediaResult(sessions[0], env);
    const callee = await mediaResult(sessions[1], env);
    let controls;
    if (process.env.UR_VIDEO_PAIR_SCENARIO === 'hold_resume') {
      stage='hold_resume_cancel'; controls=await holdResume(sessions,env);
    }
    if (process.env.UR_VIDEO_PAIR_SCENARIO === 'disconnect_caller' ||
        process.env.UR_VIDEO_PAIR_SCENARIO === 'disconnect_callee') {
      stage = 'abrupt_disconnect';
      const disconnectedIndex = process.env.UR_VIDEO_PAIR_SCENARIO === 'disconnect_caller' ? 0 : 1;
      const survivorIndex = disconnectedIndex === 0 ? 1 : 0;
      children[disconnectedIndex].child.kill('SIGKILL');
      await children[disconnectedIndex].wait();
      await browser(sessions[survivorIndex], env, 'wait', '--fn',
        'state === "available" && peer === null && capture === null');
      return {result:'pass',scenario:process.env.UR_VIDEO_PAIR_SCENARIO,
        callerAudioPackets:caller.audioPackets,callerVideoFrames:caller.videoFrames,
        calleeAudioPackets:callee.audioPackets,calleeVideoFrames:callee.videoFrames,
        survivingMediaReleased:true,disconnectedDeviceKilled:true};
    }
    stage = 'track_control';
    await browser(sessions[0], env, 'click', '#mute');
    await browser(sessions[0], env, 'wait', '--fn', 'trackPending === false && capture.getAudioTracks()[0].enabled === false');
    await browser(sessions[0], env, 'click', '#mute');
    await browser(sessions[0], env, 'wait', '--fn', 'trackPending === false && capture.getAudioTracks()[0].enabled === true');
    await browser(sessions[1], env, 'click', '#camera');
    await browser(sessions[1], env, 'wait', '--fn', 'trackPending === false && capture.getVideoTracks()[0].enabled === false');
    await browser(sessions[1], env, 'click', '#camera');
    await browser(sessions[1], env, 'wait', '--fn', 'trackPending === false && capture.getVideoTracks()[0].enabled === true');
    stage = 'hangup';
    await browser(sessions[0], env, 'click', '#hangup');
    for (const session of sessions)
      await browser(session, env, 'wait', '--fn', 'state === "available" && peer === null && capture === null');
    assert.ok(children.every(child => !child.exited()));
    return {result:'pass', scenario:controls?'cpp_device_pair_hold_resume':'cpp_device_pair',
      callerAudioPackets:caller.audioPackets, callerVideoFrames:caller.videoFrames,
      calleeAudioPackets:callee.audioPackets, calleeVideoFrames:callee.videoFrames,
      mediaReleased:true,...(controls||{})};
  } catch (error) {
    for (const session of sessions) {
      try {
        const snapshot = decoded(await browser(session, env, 'eval',
          'JSON.stringify({connected:state==="connected",available:state==="available",peer:!!peer,capture:!!capture})'));
        console.error(JSON.stringify({session:session.includes('caller')?'caller':'callee',snapshot}));
      } catch { /* 浏览器可能已被设备故障关闭。 */ }
    }
    if(stage==='boot')try{console.error(JSON.stringify({kind:'pair_boot_failure',children:children.map(child=>child.facts())}));}
    catch{ /* 固定诊断sink异常不覆盖原boot失败，不能生成退出或成功事实。 */ }
    primaryFailure=Error(`pair_${stage}_failed`);
    throw primaryFailure; // 错误只保留固定阶段，不回显浏览器正文、标识或断言原值。
  } finally {
    let cleanupFailure='none';
    try {
    const closures=await Promise.allSettled(sessions.map(session => browser(session, env, 'close')));
    for (const child of children) if (!child.exited()) child.terminated=child.child.kill('SIGTERM');
    const ended=await Promise.all(children.map(child=>child.wait()));
    ended.forEach((result,index)=>{
      const deliberatelyKilled=(scenario==='disconnect_caller'&&index===0)||
        (scenario==='disconnect_callee'&&index===1);
      assert.ok(result.code===0 || (children[index].terminated&&result.signal==='SIGTERM') ||
        (deliberatelyKilled&&result.signal==='SIGKILL'), 'device_exit_failed');
    });
    assert.ok(closures.every(value=>value.status==='fulfilled'), 'browser_close_unknown');
    } catch(error) {
      const message=Object.getOwnPropertyDescriptor(error,'message')?.value;
      cleanupFailure=['device_exit_failed','device_close_timeout','browser_close_unknown'].includes(message)?message:'unknown';
      if(!primaryFailure)throw error;
    } finally {
      try { console.error(JSON.stringify({kind:'pair_cleanup',failure:cleanupFailure,
        children:children.map(child=>child.facts())})); } catch { /* 诊断sink失败不得覆盖原业务或收尾异常。 */ }
    }
  }
}
// 仅显式CLI执行正式入口，离线单测加载不会启动设备或浏览器。
if (require.main===module) run().then(value=>console.log(JSON.stringify({...value,
  childrenClosed:true,browserSessionsClosed:true}))).catch(error => {
    console.error(/^pair_[a-z_]+_failed$/.test(error.message)?error.message:'pair_preflight_or_cleanup_failed');
    process.exitCode = 1;
  });
module.exports={selectScenario,sameOriginal,heldPair,resumedPair,originalControl,childClose,childExitFacts,observeOriginalSpawn,controlFailureSnapshot,run,deviceFailureObservation};
