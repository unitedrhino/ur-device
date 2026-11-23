// 文件说明：独立三台C++设备的单次转接验收；平台绑定由外层正式读回核验，本入口不调用API或自授授权。
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {spawn,execFile}=require('node:child_process');
const {randomUUID}=require('node:crypto');
const {performance}=require('node:perf_hooks'); // 总观察期限只用单调时钟，墙钟调整不能续期。
const {childClose,observeOriginalSpawn,deviceFailureObservation,sameOriginal}=require('./device_pair_live.cjs');
const callBootObserver=require('./call_boot_observer.cjs'); // 默认关闭，只读原启动回调与缓存，不提供成功判据。

// selectScenario 必须明确选择一个场景，不能省略后静默运行另一条真实样本。
function selectScenario(value) {
  assert.ok(['success','reject','cancel','timeout'].includes(value),'scenario_invalid');return value;
}
// validateConfig 仅校验受控环境的三身份/目标/端口；普通主人、当前tenant及IM绑定必须由外层核实并归档。
function validateConfig(env) {
  assert.equal(env.UR_VIDEO_TRANSFER_LIVE,'1');assert.equal(env.AGENT_BROWSER_HEADED,'1');
  const scenario=selectScenario(env.UR_VIDEO_TRANSFER_SCENARIO);
  const identities=JSON.parse(env.UR_VIDEO_TRANSFER_IDENTITIES),ports=JSON.parse(env.UR_VIDEO_TRANSFER_PORTS);
  assert.ok(Array.isArray(identities)&&identities.length===3&&Array.isArray(ports)&&ports.length===3);
  for(const identity of identities) {
    assert.ok(identity&&Object.getPrototypeOf(identity)===Object.prototype&&
      Object.keys(identity).sort().join(',')==='deviceName,productID,secret');
    for(const field of ['productID','deviceName']) assert.ok(typeof identity[field]==='string'&&
      identity[field].length>0&&identity[field].length<=64&&!/[\/+#\x00-\x1f\x7f]/.test(identity[field]));
    assert.ok(!identity.productID.includes(':')&&typeof identity.secret==='string'&&
      identity.secret.length>0&&identity.secret.length<=4096&&!identity.secret.includes('\0'));
  }
  const targets=identities.map(value=>`${value.productID}:${value.deviceName}`);
  assert.ok(targets.every(value=>value.length<=128));assert.equal(new Set(targets).size,3);
  assert.ok(ports.every(value=>Number.isInteger(value)&&value>=1024&&value<=65535)&&new Set(ports).size===3);
  assert.ok(typeof env.UR_MQTT_PORT==='string'&&/^[1-9][0-9]{0,4}$/.test(env.UR_MQTT_PORT));
  const mqttPort=Number(env.UR_MQTT_PORT);assert.ok(mqttPort<=65535&&!ports.includes(mqttPort));
  const binary=env.UR_VIDEO_DEVICE_BIN;assert.ok(typeof binary==='string'&&path.isAbsolute(binary));
  return {scenario,identities,ports,mqttPort,binary};
}
// mediaGrowth 原腿/Peer/原发送轨必须保持，且两次实际媒体读数均有效；单个connected或黑画面不算通过。
function mediaGrowth(before,after) {
  sameOriginal(before,after);assert.equal(after.trackFingerprint,before.trackFingerprint);
  for(const value of [before,after]) {
    assert.equal(value.peerPresent,true);assert.equal(value.capturePresent,true);
    assert.equal(value.outputEnabled,true);assert.equal(value.actualHeld,false);assert.equal(value.videoPresented,true);
    for(const field of ['trackFingerprint','pixelFingerprint'])
      assert.ok(typeof value[field]==='string'&&value[field].length>0&&value[field].length<=128);
    for(const field of ['audioPackets','videoPackets','outAudioPackets','outVideoPackets','videoFrames','audioNonSilentBlocks','pixelSum'])
      assert.ok(Number.isSafeInteger(value[field])&&value[field]>=0&&value[field]<=10000000);
    assert.ok(value.audioNonSilentBlocks>0&&value.videoFrames>=2&&value.pixelSum>1000);
  }
  for(const field of ['audioPackets','videoPackets','outAudioPackets','outVideoPackets','videoFrames','audioNonSilentBlocks'])
    assert.ok(after[field]>before[field]);
  assert.notEqual(after.pixelFingerprint,before.pixelFingerprint);
}
// transferResult 验证同原token/腿的唯一受理与独立结果；撤候选还必须有另一个原200ACK。
function transferResult(receipts,baseline,scenario,token) {
  selectScenario(scenario);assert.ok(typeof token==='string'&&token.length>0);
  assert.ok(Array.isArray(receipts)&&receipts.length<=24);
  const matches=receipts.filter(value=>value?.sessionId===baseline.sessionId&&value.controlToken===token);
  assert.equal(matches.length,1);const value=matches[0];
  assert.equal(value.code,200);assert.equal(value.candidatePresent,true);
  assert.ok(Number.isSafeInteger(value.ackRevision)&&value.ackRevision>0&&
    Number.isSafeInteger(value.resultRevision)&&value.resultRevision>value.ackRevision);
  assert.equal(value.state,scenario==='success'?'transferred':scenario==='cancel'?'cancelled':'failed');
  assert.equal(value.reason,scenario==='success'?'':scenario==='cancel'?'candidateCancelled':
    scenario==='reject'?'transferRejected':'transferTimeout');
  if(scenario==='cancel') {
    assert.equal(value.cancelState,'completed');assert.equal(value.cancelCode,200);assert.equal(value.cancelReason,'');
    assert.ok(typeof value.cancelToken==='string'&&value.cancelToken&&value.cancelToken!==token);
  } else {
    assert.equal(value.cancelToken,'');assert.equal(value.cancelState,'idle');assert.equal(value.cancelCode,0);
  }
  return value;
}
// releasedMedia 必须收录原Peer与原轨实际终态，不把清空页面引用当成停止采集。
function releasedMedia(value) {
  assert.equal(value.peerPresent,false);assert.equal(value.capturePresent,false);assert.equal(value.outputEnabled,false);
  assert.equal(value.releaseFacts?.originalPeerClosed,true);assert.equal(value.releaseFacts?.originalTracksEnded,true);
}
// browserEnvironment 浏览器只驱动媒体，不继承设备密钥或成组身份；原C++启动环境保持不变。
function browserEnvironment(env){const value={...env};
  for(const key of ['UR_VIDEO_TRANSFER_IDENTITIES','UR_DEVICE_SECRET','UR_VIDEO_PAIR_FIRST','UR_VIDEO_PAIR_SECOND'])delete value[key];
  return value;}
// browser 只驱动本次具名会话；条件观察不排入超过CLI 30秒IPC期限的daemon等待。
function browser(session,env,...args) {
  if(args[0]==='wait'&&args[1]==='--fn'&&args.length===3)
    return waitCondition(session,env,args[2]);
  return browserCommand(session,env,40000,args[0],...args);
}
// browserCommand 保留原固定操作分类；短eval仍归属原wait，不回显CLI正文或自由异常。
function browserCommand(session,env,timeout,operation,...args) {
  return new Promise((resolve,reject)=>execFile('agent-browser',['--session',session,...args],
    {env:browserEnvironment(env),timeout,maxBuffer:262144},
    (error,output)=>{if(!error)return resolve(output.trim());
      const category=error.code==='ERR_CHILD_PROCESS_STDIO_MAXBUFFER'?'output_limit':
        error.code==='ETIMEDOUT'||error.killed===true?'deadline':
        ['ENOENT','EACCES','EPERM'].includes(error.code)?'spawn_failed':
        Number.isInteger(error.code)?'command_exit':'unknown'; // 原错误正文/CLI输出不进入诊断。
      reject(Object.assign(Error('browser_command_failed'),{browserOperation:
        ['open','fill','click','wait','eval','close'].includes(operation)?operation:'unknown',browserFailure:category}));}));
}
// waitCondition 在原55秒总期限内只重读固定布尔条件；异常/非法结果立即失败，不重发业务操作。
async function waitCondition(session,env,expression) {
  const failure=category=>Object.assign(Error('browser_command_failed'),
    {browserOperation:'wait',browserFailure:category});
  if(typeof expression!=='string'||!expression||expression.length>16384)throw failure('invalid_result');
  const deadline=performance.now()+55000;
  for(;;){
    const remaining=deadline-performance.now();if(remaining<=0)throw failure('deadline');
    const output=await browserCommand(session,env,Math.min(25000,Math.ceil(remaining)),
      'wait','eval','!!('+expression+')');
    if(performance.now()>=deadline)throw failure('deadline');
    let result;try{result=JSON.parse(output);}catch{throw failure('invalid_result');}
    if(typeof result!=='boolean')throw failure('invalid_result');
    if(result)return '';
    await new Promise(resolve=>setTimeout(resolve,Math.min(100,deadline-performance.now())));
  }
}
// failureDiagnosticFacts 固定白名单投影；未采到与实际false分开，非法/越界值保持null而非猜实态。
function failureDiagnosticFacts(raw) {
  const present=raw&&typeof raw==='object'&&raw.diagnosticSnapshot===true; // 只识别原只读采样标记。
  const value=present?raw:{},boolean=key=>typeof value[key]==='boolean'?value[key]:null; // 明确未知布尔。
  const count=(source,key,max=10000000)=>Number.isSafeInteger(source[key])&&source[key]>=0&&source[key]<=max?source[key]:null; // 固定计数上界。
  const choice=(source,key,values)=>values.includes(source[key])?source[key]:null; // 不保留自由枚举或正文。
  const latest=present&&value.latest&&typeof value.latest==='object'?value.latest:null; // 无投影不伪造空ACK。
  const reasons=['','staleRevision','invalidState','rejected','candidateCancelled','transferRejected','transferTimeout',
    'transferMediaFailed','transferUnavailable','transferRevisionChanged','transferPermissionRevoked']; // 原13字段理由集合。
  const code=(source,key)=>[0,200].includes(source[key])||Number.isSafeInteger(source[key])&&source[key]>=400&&source[key]<=599?source[key]:null; // 合法已知ACK码。
  return {sampled:!!present,state:choice(value,'state',['idle','connecting','available','inviting','ringing','negotiating','connected']),
    closed:boolean('closed'),socketOpen:boolean('socketOpen'),peerPresent:boolean('peerPresent'),capturePresent:boolean('capturePresent'),
    outputEnabled:boolean('outputEnabled'),actualHeld:boolean('actualHeld'),transferDisabled:boolean('transferDisabled'),
    transferIntent:choice(value,'transferIntent',['','transfer','cancelTransfer']),latestPresent:boolean('latestPresent'),
    receiptCount:count(value,'receiptCount',24),overflow:boolean('overflow'),audioPackets:count(value,'audioPackets'),
    videoPackets:count(value,'videoPackets'),videoFrames:count(value,'videoFrames'),audioNonSilentBlocks:count(value,'audioNonSilentBlocks'),
    latest:latest?{state:choice(latest,'state',['pending','transferring','transferred','failed','cancelled','rejected','unknown']),
      code:code(latest,'code'),cancelState:choice(latest,'cancelState',['idle','pending','completed','rejected','unknown']),
      cancelCode:code(latest,'cancelCode'),ackRevision:count(latest,'ackRevision',9007199254740991),
      resultRevision:count(latest,'resultRevision',9007199254740991),reason:choice(latest,'reason',reasons),
      candidatePresent:typeof latest.candidatePresent==='boolean'?latest.candidatePresent:null,
      sessionMatched:typeof latest.sessionMatched==='boolean'?latest.sessionMatched:null,
      controlTokenPresent:typeof latest.controlTokenPresent==='boolean'?latest.controlTokenPresent:null}:null};
}
// failureClass 不读取自由异常正文，只区分浏览器原固定类别、断言、解码与未知。
function failureClass(error) {
  const operation=['open','fill','click','wait','eval','close','unknown'].includes(error?.browserOperation)?error.browserOperation:null;
  const category=['output_limit','deadline','spawn_failed','command_exit','invalid_result','unknown'].includes(error?.browserFailure)?error.browserFailure:null;
  return {failureType:operation?'browser_command':error?.code==='ERR_ASSERTION'?'condition_failed':
    error?.name==='SyntaxError'?'invalid_browser_result':'unknown',browserOperation:operation,browserFailure:category};
}
// failureSnapshot 仅原页面内读固定状态/计数与关联布尔，session/token/target及自由正文不离开页面。
const failureSnapshot=`(()=>({diagnosticSnapshot:true,state,closed,socketOpen:socket?.readyState===WebSocket.OPEN,
  peerPresent:!!peer,capturePresent:!!capture,outputEnabled,actualHeld,transferDisabled:item('transfer').disabled,
  transferIntent,latestPresent:!!latestTransfer,receiptCount:transferReceipts.length,overflow:transferOverflow,
  audioPackets:reports.audioPackets,videoPackets:reports.videoPackets,videoFrames:reports.videoFrames,audioNonSilentBlocks,
  latest:latestTransfer?{state:latestTransfer.state,code:latestTransfer.code,cancelState:latestTransfer.cancelState,
    cancelCode:latestTransfer.cancelCode,ackRevision:latestTransfer.ackRevision,resultRevision:latestTransfer.resultRevision,
    reason:latestTransfer.reason,candidatePresent:latestTransfer.candidatePresent,
    sessionMatched:latestTransfer.sessionId===callSession,controlTokenPresent:!!latestTransfer.controlToken}:null}))()`;
// decoded 只在内存解开CLI包装，不将原对象转入日志。
function decoded(value) {
  let result=JSON.parse(value);
  while(typeof result==='string'&&/^[{[\"]/.test(result)){try{result=JSON.parse(result);}catch{break;}}
  return result;
}
// bootDevice 仅同形复用原pair启动窗，spawn后立即登记，双流原end/close由共享原观察器收录。
async function bootDevice(config,index,env,register) {
  const identity=config.identities[index],port=config.ports[index];
  const child=spawn(config.binary,[String(port)],{env:{...env,UR_DEVICE_PRODUCT:identity.productID,
    UR_DEVICE_NAME:identity.deviceName,UR_DEVICE_SECRET:identity.secret,UR_MQTT_HOST:'127.0.0.1',
    UR_MQTT_PORT:String(config.mqttPort),UR_MQTT_CA:''},stdio:['ignore','pipe','pipe']});
  const lifetime=childClose(child),observation=deviceFailureObservation(child.stderr);
  const record={child,...lifetime,facts:()=>({...lifetime.facts(),stderr:observation.facts()})};
  register(record);child.stdout.on('data',()=>{});child.stderr.on('data',()=>{});
  await new Promise((resolve,reject)=>{
    let prefix='',settled=false;
    const finish=error=>{if(settled)return;settled=true;clearTimeout(timer);
      child.stdout.removeListener('data',data);child.removeListener('error',failed);child.removeListener('close',closed);
      error?reject(error):resolve();};
    const data=bytes=>{prefix=(prefix+bytes.toString()).slice(-1024);
      if(prefix.includes(`http://127.0.0.1:${port}/`))finish();};
    const failed=()=>finish(Error('device_spawn_failed')),closed=()=>finish(Error('device_closed_before_bind'));
    const timer=setTimeout(()=>finish(Error('device_start_timeout')),10000);
    child.stdout.on('data',data);child.once('error',failed);child.once('close',closed);
  });
  return record;
}
// installOrderObserver 仅旁观原两类宿主消息与释放前实态，不改原参数/次数/返回/异常，最多16项。
const installOrderObserver=`(()=>{
  const original=incoming,records=[];let overflow=false;
  globalThis.__transferOrder=()=>({records:records.map(value=>({...value})),overflow});
  incoming=function(value){
    try{
      if(value?.kind==='callStop'||(value?.kind==='callTransferReceipt'&&value.state==='transferred')){
        if(records.length===16)overflow=true;
        else records.push({kind:value.kind,peerConnected:!!peer&&peer.connectionState==='connected',
          captureLive:!!capture&&capture.getTracks().length===2&&capture.getTracks().every(track=>track.readyState==='live'),
          outputEnabled:outputEnabled===true});
      }
    }catch{overflow=true;}
    return original.apply(this,arguments);
  };return true;
})()`;
// rememberMedia 仅保留原对象的只读终态观察，挂断后仍可核原轨而不复活或操纵媒体。
const rememberMedia=`(()=>{
  const original=peer,tracks=capture.getTracks();
  if(!original||tracks.length!==2)throw Error('original_media_unknown');
  globalThis.__transferRelease=()=>({originalPeerClosed:original.connectionState==='closed',
    originalTracksEnded:tracks.every(track=>track.readyState==='ended')});return true;
})()`;
// snapshot 读取真实Peer/RTP、PCM与呈现像素；标识只用于内存关联，不输出正文或设备身份。
const snapshot=`(async()=>{
  const metrics=controlMetrics(),base={...metrics,state,peerPresent:!!peer,capturePresent:!!capture,
    order:typeof __transferOrder==='function'?__transferOrder():null,
    releaseFacts:typeof __transferRelease==='function'?__transferRelease():null};
  if(!peer||!capture)return base;
  const stats=await peer.getStats();let audioPackets=0,videoPackets=0,outAudioPackets=0,outVideoPackets=0,videoFrames=0;
  const seen=new Set();stats.forEach(value=>{
    if(!['inbound-rtp','outbound-rtp'].includes(value.type)||!['audio','video'].includes(value.kind))return;
    const count=value.type==='inbound-rtp'?value.packetsReceived:value.packetsSent;
    if(!Number.isSafeInteger(count)||count<0||count>10000000)throw Error('stat_unknown');
    seen.add(value.type+':'+value.kind);
    if(value.type==='inbound-rtp'){if(value.kind==='audio')audioPackets+=count;
      else{videoPackets+=count;videoFrames+=value.framesDecoded||0;}}
    else if(value.kind==='audio')outAudioPackets+=count;else outVideoPackets+=count;
  });if(seen.size!==4)throw Error('stat_unknown');
  const tracks=capture.getTracks(),senders=peer.getSenders();
  const hash=async bytes=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',
    typeof bytes==='string'?new TextEncoder().encode(bytes):bytes)),
    value=>value.toString(16).padStart(2,'0')).join('');
  const canvas=document.createElement('canvas');canvas.width=32;canvas.height=24;
  const context=canvas.getContext('2d');context.drawImage(item('remote'),0,0,32,24);
  const pixels=context.getImageData(0,0,32,24).data;let pixelSum=0;
  for(let index=0;index<pixels.length;index+=4)pixelSum+=pixels[index]+pixels[index+1]+pixels[index+2];
  return {...base,peerFingerprint:await hash(peer.localDescription.sdp),
    trackFingerprint:await hash(tracks.map(track=>track.kind+':'+track.id).sort().join('|')),
    pixelFingerprint:await hash(pixels),videoPresented:!item('remote').paused&&item('remote').videoWidth>0&&item('remote').style.visibility!=='hidden',
    connected:peer.connectionState==='connected',sameOriginalTracks:tracks.length===2&&senders.length===2&&
      tracks.every(track=>track.readyState==='live')&&senders.every(sender=>tracks.includes(sender.originalCallTrack||sender.track)),
    audioPackets,videoPackets,outAudioPackets,outVideoPackets,videoFrames,pixelSum};
})()`;
// readPair 两次原页读数只用于有限媒体比较，不用页面点击状态替代实际媒体。
async function readPair(sessions,env) {return Promise.all(sessions.map(async session=>decoded(await browser(session,env,'eval',snapshot))));}
// waitMedia 原固定实际媒体门槛，不放宽1500ms显示、2秒门控或750ms稳定窗口。
async function waitMedia(session,env) {
  await browser(session,env,'wait','--fn','state==="connected" && peer?.connectionState==="connected" && outputEnabled && audioNonSilentBlocks>0 && reports.videoFrames>=2 && item("remote").videoWidth>0');
}
// stableSample 复用原750ms稳定观察，不通过裸等待造成功；随后仍须双向真实媒体增长。
async function stableSample(session,env) {
  return decoded(await browser(session,env,'eval',`(async()=>{await new Promise(resolve=>setTimeout(resolve,750));return await ${snapshot};})()`));
}
// releaseOrder 仅投影宿主通知顺序，契约未保证跨通知同序，不借此判定sessionEnded或新腿媒体许可。
function releaseOrder(order) {
  if(!order||order.overflow!==false||!Array.isArray(order.records)||order.records.length>16)return {sampled:false};
  const transferred=order.records.findIndex(value=>value.kind==='callTransferReceipt');
  const stopped=order.records.findIndex(value=>value.kind==='callStop');
  const record=order.records[transferred];
  return {sampled:true,transferredObserved:transferred>=0,hostStopObserved:stopped>=0,
    notificationBeforeStop:transferred>=0&&stopped>=0?transferred<stopped:null,
    peerLiveAtNotification:record&&typeof record.peerConnected==='boolean'?record.peerConnected:null,
    captureLiveAtNotification:record&&typeof record.captureLive==='boolean'?record.captureLive:null,
    outputAtNotification:record&&typeof record.outputEnabled==='boolean'?record.outputEnabled:null};
}
// run 只通过真实界面发起转接/撤候选，D3必须显式接听；外层负责最新产物与真实绑定准入。
async function run({onSpawn}={}) {
  let config;
  let bootDiagnostics=false; // 显式诊断增加click前准备命令，不冒认原无observer同负载。
  try{bootDiagnostics=callBootObserver.enabled(process.env);assert.ok(onSpawn===undefined||typeof onSpawn==='function');config=validateConfig(process.env);
    const stat=fs.statSync(config.binary);assert.ok(stat.isFile()&&(stat.mode&0o100));}
  catch{throw Error('transfer_preflight_failed');}
  // 条件总观察仍55秒，单个CLI动作保持25秒，避免30秒IPC重投；原45秒候选不延长。
  const env={...process.env,AGENT_BROWSER_DEFAULT_TIMEOUT:'25000',AGENT_BROWSER_ARGS:[process.env.AGENT_BROWSER_ARGS||'--no-sandbox',
    '--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required'].join(',')};
  const sessions=[0,1,2].map(index=>`transfer-${index}-${randomUUID()}`),children=[],opened=[];
  let stage='boot',substep='device_boot',primaryFailure,result,orderFacts;
  try {
    for(let index=0;index<3;++index)await bootDevice(config,index,env,record=>{children.push(record);observeOriginalSpawn(record.child,onSpawn);});
    for(let index=0;index<3;++index){opened.push(sessions[index]);
      await browser(sessions[index],env,'open',`http://127.0.0.1:${config.ports[index]}/`);
      if(bootDiagnostics)await browser(sessions[index],env,'eval',callBootObserver.installSource);
      await browser(sessions[index],env,'click','#activate');await browser(sessions[index],env,'wait','--fn','state==="available"');}
    stage='original_call';substep='original_call';
    await browser(sessions[0],env,'fill','#target',`${config.identities[1].productID}:${config.identities[1].deviceName}`);
    await browser(sessions[0],env,'click','#dial');await browser(sessions[1],env,'wait','--fn','state==="ringing"');
    await browser(sessions[1],env,'click','#accept');
    for(const session of sessions.slice(0,2))await waitMedia(session,env);
    const baseline=await readPair(sessions.slice(0,2),env);
    const original=await Promise.all(sessions.slice(0,2).map(session=>stableSample(session,env)));
    original.forEach((value,index)=>mediaGrowth(baseline[index],value));
    for(const session of sessions.slice(0,2))await browser(session,env,'eval',rememberMedia);
    await browser(sessions[0],env,'eval',installOrderObserver);
    stage='transfer_acceptance';substep='transfer_target_fill';
    await browser(sessions[0],env,'fill','#transferTarget',`${config.identities[2].productID}:${config.identities[2].deviceName}`);
    substep='transfer_click';
    await browser(sessions[0],env,'click','#transfer');
    substep='transfer_ack_wait';
    await browser(sessions[0],env,'wait','--fn','latestTransfer?.state==="transferring" && latestTransfer.code===200 && latestTransfer.candidatePresent');
    substep='transfer_ack_read';
    const accepted=decoded(await browser(sessions[0],env,'eval','({...latestTransfer})'));
    substep='transfer_ack_binding';
    assert.equal(accepted.sessionId,baseline[0].sessionId);assert.ok(accepted.controlToken);
    substep='candidate_ringing_wait';
    await browser(sessions[2],env,'wait','--fn','state==="ringing"');
    substep='candidate_preaccept_read';
    const candidateBefore=decoded(await browser(sessions[2],env,'eval','({peer:!!peer,capture:!!capture})'));
    substep='candidate_preaccept_media';
    assert.equal(candidateBefore.peer,false);assert.equal(candidateBefore.capture,false);
    substep='original_pending_media_read';
    const pending=await Promise.all(sessions.slice(0,2).map(session=>stableSample(session,env)));
    substep='original_pending_media_growth';
    pending.forEach((value,index)=>mediaGrowth(original[index],value));
    stage='candidate_'+config.scenario;substep='candidate_choice';
    if(config.scenario==='success')await browser(sessions[2],env,'click','#accept');
    else if(config.scenario==='reject')await browser(sessions[2],env,'click','#reject');
    else if(config.scenario==='cancel')await browser(sessions[0],env,'click','#cancelTransfer');
    // timeout绝不发送reject/cancel；真实45秒候选事件未到或核心Unknown均失败，不伪造超时结果。
    const terminal=config.scenario==='success'?'transferred':config.scenario==='cancel'?'cancelled':'failed';
    substep='transfer_terminal_wait';
    await browser(sessions[0],env,'wait','--fn',`transferReceipts.some(value=>value.controlToken===${JSON.stringify(accepted.controlToken)} && value.state===${JSON.stringify(terminal)}${config.scenario==='cancel'?' && value.cancelState==="completed" && value.cancelCode===200':''})`);
    stage='actual_switch';substep='actual_switch';
    const caller=decoded(await browser(sessions[0],env,'eval',snapshot));
    assert.equal(caller.transferOverflow,false);transferResult(caller.transferReceipts,baseline[0],config.scenario,accepted.controlToken);
    if(config.scenario==='success') {
      await browser(sessions[0],env,'wait','--fn','state==="available" && !peer && !capture');
      for(const session of sessions.slice(1))await waitMedia(session,env);
      const first=await readPair(sessions.slice(1),env),second=await Promise.all(sessions.slice(1).map(session=>stableSample(session,env)));
      mediaGrowth(pending[1],first[0]);mediaGrowth(first[0],second[0]);mediaGrowth(first[1],second[1]);
      await browser(sessions[2],env,'eval',rememberMedia);
      assert.notEqual(first[1].sessionId,baseline[0].sessionId);assert.notEqual(first[1].sessionId,baseline[1].sessionId);
      const released=decoded(await browser(sessions[0],env,'eval',snapshot));
      releasedMedia(released);
      orderFacts=releaseOrder(released.order);
    } else {
      await browser(sessions[2],env,'wait','--fn','state==="available" && !peer && !capture');
      const preserved=await Promise.all(sessions.slice(0,2).map(session=>stableSample(session,env)));
      preserved.forEach((value,index)=>mediaGrowth(pending[index],value));
    }
    stage='hangup';substep='hangup';await browser(sessions[config.scenario==='success'?1:0],env,'click','#hangup');
    for(const session of sessions)await browser(session,env,'wait','--fn','state==="available" && !peer && !capture && !outputEnabled');
    const finished=await readPair(config.scenario==='success'?sessions:sessions.slice(0,2),env);
    finished.forEach(releasedMedia);
    assert.ok(children.every(child=>!child.exited()));
    result={result:'pass',scenario:config.scenario,threeIndependentDevices:true,candidateCaptureAbsentBeforeAccept:true,
      originalPeerRetained:true,actualBidirectionalMediaVerified:true,mediaReleased:true,
      ...(orderFacts?{notificationOrder:orderFacts}:{})};
  }catch(error){
    // 固定首次失败先留存；仅opened原页面各读一次，不开页、不重试业务、不因采样/收尾覆盖原因。
    const bootObservations=bootDiagnostics?await Promise.all(opened.map(async(session,index)=>{try{return {index,...await callBootObserver.readOriginalCache(session,env)};}catch{return {index,...callBootObserver.unknown()};}})):null; // 原缓存不可读保持unknown，不重开页或造回执。
    const facts={kind:'transfer_failure',stage,substep,...failureClass(error),...(bootDiagnostics?{bootObservations}:{}),pages:await Promise.all(opened.map(async(session,index)=>{
      if(bootDiagnostics&&stage==='boot')return {index,...failureDiagnosticFacts(null)}; // 原daemon可能已退出；此显式诊断分支不借CLI ensure_daemon重新创建失败后的页面。
      try{return {index,...failureDiagnosticFacts(decoded(await browser(session,env,'eval',failureSnapshot)))};}
      catch{return {index,...failureDiagnosticFacts(null)};}
    }))};
    primaryFailure=Object.assign(Error(`transfer_${stage}_failed`),{facts});
    try{console.error(JSON.stringify(facts));}catch{/* 诊断出口不能改变原业务错误或收尾。 */}
    throw primaryFailure;
  }
  finally {
    let cleanupFailure='none';
    try {
      const closures=await Promise.allSettled(opened.map(session=>browser(session,env,'close')));
      for(const child of children)if(!child.exited())child.terminated=child.child.kill('SIGTERM');
      const ended=await Promise.all(children.map(child=>child.wait()));
      ended.forEach((value,index)=>assert.ok(value.code===0||(children[index].terminated&&value.signal==='SIGTERM')));
      assert.ok(closures.every(value=>value.status==='fulfilled'));
      assert.ok(children.every(child=>{const facts=child.facts();return facts.closed&&facts.stdoutEnded&&facts.stderrEnded;}));
      if(result){result.childrenClosed=true;result.browserSessionsClosed=true;}
    }catch{cleanupFailure='unknown';if(!primaryFailure)throw Error('transfer_cleanup_failed');}
    finally{try{console.error(JSON.stringify({kind:'transfer_cleanup',failure:cleanupFailure,children:children.map(child=>child.facts())}));}catch{}}
  }
  return result;
}
// 仅显式CLI进入正式流程；加载或未开关时不得启动进程、浏览器或调用API。
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(error=>{
  console.error(/^transfer_[a-z_]+_failed$/.test(error.message)?error.message:'transfer_preflight_or_cleanup_failed');process.exitCode=1;
});
module.exports={selectScenario,validateConfig,mediaGrowth,transferResult,releasedMedia,releaseOrder,installOrderObserver,rememberMedia,run,browserEnvironment,failureDiagnosticFacts,failureClass,waitCondition};
