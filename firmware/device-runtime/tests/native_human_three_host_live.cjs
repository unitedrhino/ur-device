// 文件说明：显式启用的三C++纯Human音频接续驱动；原端保留、撤候选、两次实际移交及退出清理。
// 复用共享宿主的进程身份/浏览器PCM判据；平台只读旁证必须由隔离宿主注入，不从页面推导授权。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const {spawn,execFile} = require('node:child_process');
const {randomUUID,createHash} = require('node:crypto');
const {performance} = require('node:perf_hooks');
const shared = require('./shared_handoff_live.cjs');
const {prepareBrowserClick,verifyBrowserClick}=require('./browser_click.cjs');
const root = path.resolve(__dirname,'../../..'); // 只在任务worktree启动原签收设备产品。
// proofKeys只保留原平台释放/提交/门控的闭集合，不包含身份或自由原因。
const proofKeys=['stage','sameOriginal','currentOwners','wholeFocus','sourceReleased','sourceRetained',
  'remoteRetained','targetActual','targetGateOpen','imCommitted','targetReleased','unknown'];
// exact仅接纳原闭集合自有字段；未知私有内容不进入报告。
function exact(value,keys) {return value!==null&&typeof value==='object'&&!Array.isArray(value)&&
  Object.keys(value).length===keys.length&&keys.every(key=>Object.hasOwn(value,key));}
// verifyTransferProof区分ready的资源就绪与业务门开放；平台事实不能由信令成功或单侧PCM补造。
function verifyTransferProof(value,stage) {
  assert.ok(['ready','committed','aborted'].includes(stage)&&exact(value,proofKeys),'transfer_proof_invalid');
  assert.equal(value.stage,stage,'transfer_proof_invalid');
  for(const key of proofKeys.slice(1))assert.equal(typeof value[key],'boolean','transfer_proof_invalid');
  for(const key of ['sameOriginal','currentOwners','wholeFocus','remoteRetained'])assert.equal(value[key],true,'transfer_proof_unconfirmed');
  assert.equal(value.unknown,false,'transfer_proof_unconfirmed');
  const committed=stage==='committed',aborted=stage==='aborted';
  for(const [key,expected] of Object.entries({sourceReleased:committed,sourceRetained:!committed,
    targetActual:!aborted,targetGateOpen:committed,imCommitted:committed,targetReleased:aborted}))
    assert.equal(value[key],expected,'transfer_proof_unconfirmed');
  return true;
}
// bootProjection只读启动角色、步骤及原页面布尔/固定激活枚举；未知自由异常或正文不可进入报告。
function bootProjection(value){
  const role=['source','remote','target'].includes(value?.role)?value.role:'unknown';
  const steps=['none','spawn','capture','readyEndpoint','openBrowser','activate','pageReady','installObservation','complete'];
  const step=steps.includes(value?.step)?value.step:'unknown';let page=null;
  if(value?.page){const raw=value.page;page={};for(const key of ['ready','bridgeOpen','audioRunning','terminated'])page[key]=typeof raw[key]==='boolean'?raw[key]:null;
    page.activationFailure=null;try{page.activationFailure=shared.projectActivationObservation([raw,raw]).caller;}catch{/* 缺失/畸形固定激活事实仍缺失，不用status正文补值。 */}}
  return {role,step,failureCode:shared.projectFailureCode({message:value?.failureCode}),page};
}
// operationProjection只保本轮角色/尝试序号及固定业务步骤，不记录DOM选择器、设备身份或参数。
function operationProjection(value){const steps=['input','boot','dialReady','dialTargetKind','dialTarget','dialSend','inviteReady','inviteAccept','initialAudio','prepareReady','originalAudio',
  'prepareTarget','prepareSend','candidateReady','readyBaseline','readyProof','abortSend','commitSend','terminalProof','targetExit','sourceExit','retainedAudio','committedAudio','hangup','finalExit','completed'];
  return {role:['source','remote','target'].includes(value?.role)?value.role:'unknown',attempt:Number.isInteger(value?.attempt)&&value.attempt>=0&&value.attempt<=3?value.attempt:null,
    step:steps.includes(value?.step)?value.step:'unknown'};}
// pageProjection保原业务读页中的有限布尔；页面ready不是canEscalate/云端许可，缺样本始终null。
function pageProjection(value){if(!value||typeof value!=='object'||Array.isArray(value))return null;const result={};
  for(const key of ['ready','bridgeOpen','audioRunning','terminated','established','canEscalate','canAccept','canCancel','sessionBound','canHold','canResume','captureClosed','released'])result[key]=typeof value[key]==='boolean'?value[key]:null;
  result.activationFailure=bootProjection({page:value}).page.activationFailure;
  result.endpoint=Object.fromEntries(['canPrepare','canCommit','canAbort'].map(key=>[key,typeof value.endpoint?.[key]==='boolean'?value.endpoint[key]:null]));
  result.media=Object.fromEntries(['connected','inputAttached','outputEnabled','held'].map(key=>[key,typeof value.media?.[key]==='boolean'?value.media[key]:null]));return result;}
// projectReport只保留固定阶段/实际完成轮数及清理；自由异常、标识、SDP与凭据不落盘。
function projectReport(value) {
  const phases=['input','boot','dial','audio','prepare','ready','abort','commit','hangup','completed'];
  const cleaned=value.devicesClosed===true&&value.browsersClosed===true;
  return {scenario:'cpp_pure_human_three_host',phase:phases.includes(value.phase)?value.phase:'unknown',
    passed:value.offline!==true&&value.passed===true&&cleaned&&value.diagnosticFailed!==true&&value.rounds===2&&value.aborted===true,
    ...(value.offline===true?{evidence:'offline',fixturePassed:value.passed===true&&cleaned&&value.diagnosticFailed!==true&&value.rounds===2&&value.aborted===true}:{}),
    actualCPP:value.actualCPP===true,actualModelRun:false,completedTransfers:Number.isInteger(value.rounds)?Math.max(0,Math.min(2,value.rounds)):0,
    readyTargetInputObserved:value.readyTargetInputObserved===true,
    abortVerified:value.aborted===true,cleanup:{devicesClosed:value.devicesClosed===true,browsersClosed:value.browsersClosed===true},
    boot:bootProjection(value.boot),diagnosticFailed:value.diagnosticFailed===true,failureCode:value.passed===true&&cleaned&&value.diagnosticFailed!==true?null:shared.projectFailureCode({message:value.passed===true&&!cleaned?'cleanup_failed':value.failureCode}),lastOperation:operationProjection(value.lastOperation),
    lastPageFacts:Object.fromEntries(['source','remote','target'].map(role=>[role,pageProjection(value.lastPageFacts?.[role])])),
    clickObservations:Array.isArray(value.clickObservations)?value.clickObservations.slice(0,32).map(item=>({...operationProjection(item),
      ...Object.fromEntries(['present','enabled','inViewport','hit','scrolled'].map(key=>[key,typeof item[key]==='boolean'?item[key]:null]))})):[],
    deviceProcesses:Object.fromEntries(['createdCount','spawnedCount','closedCount','stdoutEndedCount','stderrEndedCount'].map(key=>[key,
      Number.isInteger(value.deviceProcesses?.[key])&&value.deviceProcesses[key]>=0&&value.deviceProcesses[key]<=5?value.deviceProcesses[key]:null]))};
}
// validateEnvironment在任何spawn前核显式开关/普通私有文件/固定ELF与WAV；不创建环境或修改生产配置。
function validateEnvironment(env) {
  assert.equal(env.UR_NATIVE_HUMAN_THREE_HOST_LIVE,'1','explicit_live_opt_in_required');
  assert.equal(env.AGENT_BROWSER_HEADED,'1','headed_browser_required');
  assert.ok(env.DISPLAY&&path.isAbsolute(env.XAUTHORITY||''),'headed_browser_required');
  const binary=env.UR_SHARED_HANDOFF_BIN;
  assert.ok(path.isAbsolute(binary||'')&&!fs.lstatSync(binary).isSymbolicLink()&&fs.statSync(binary).isFile(),'binary_pin_invalid');
  assert.ok(/^[a-f0-9]{64}$/.test(env.UR_SHARED_HANDOFF_BIN_SHA256||'')&&
    createHash('sha256').update(fs.readFileSync(binary)).digest('hex')===env.UR_SHARED_HANDOFF_BIN_SHA256,'binary_pin_invalid');
  const file=env.UR_NATIVE_HUMAN_THREE_HOST_INPUT;
  assert.ok(path.isAbsolute(file||''),'private_input_invalid');
  const stat=fs.lstatSync(file);
  assert.ok(stat.isFile()&&!stat.isSymbolicLink()&&stat.uid===process.getuid()&&(stat.mode&0o777)===0o600&&stat.size>0&&stat.size<=16384,'private_input_invalid');
  const data=JSON.parse(fs.readFileSync(file,'utf8'));
  assert.ok(exact(data,['source','remote','target','ports']),'private_input_invalid');
  const identities=new Set();
  for(const role of ['source','remote','target']) {
    const device=data[role];
    assert.ok(exact(device,['productID','deviceName','secret']),'private_input_invalid');
    for(const key of ['productID','deviceName'])assert.ok(typeof device[key]==='string'&&/^[A-Za-z0-9_-]{1,64}$/.test(device[key]),'private_input_invalid');
    assert.ok(typeof device.secret==='string'&&device.secret.length>0&&Buffer.byteLength(device.secret)<=1024&&!/[\x00-\x1f\x7f]/.test(device.secret),'private_input_invalid');
    identities.add(device.productID+':'+device.deviceName);
  }
  assert.equal(identities.size,3,'distinct_devices_required');
  assert.ok(Array.isArray(data.ports)&&data.ports.length===3&&new Set(data.ports).size===3&&
    data.ports.every(port=>Number.isInteger(port)&&port>=1024&&port<=65535),'bridge_ports_invalid');
  assert.ok(env.UR_MQTT_HOST==='127.0.0.1'&&/^[1-9][0-9]*$/.test(env.UR_MQTT_PORT||'')&&Number(env.UR_MQTT_PORT)>=1024&&Number(env.UR_MQTT_PORT)<=65535,'mqtt_loopback_required');
  const audio=env.UR_SHARED_HANDOFF_AUDIO_FIXTURE;
  assert.ok(path.isAbsolute(audio||'')&&!/[\r\n,]/.test(audio)&&/^[a-f0-9]{64}$/.test(env.UR_SHARED_HANDOFF_AUDIO_SHA256||''),'audio_pin_invalid');
  const wav=fs.readFileSync(audio);
  assert.ok(wav.length>=44&&wav.length<=10485760&&wav.subarray(0,4).toString()==='RIFF'&&wav.subarray(8,12).toString()==='WAVE'&&
    createHash('sha256').update(wav).digest('hex')===env.UR_SHARED_HANDOFF_AUDIO_SHA256,'audio_pin_invalid');
  return {binary,data,audio};
}
// run在一个原180秒预算执行真实三Host；缺只读平台证明在spawn前拒绝，离线测试不能调用本入口制造通过。
function createLivePorts() {
  return {spawn,execFile,captureChild:shared.captureChild,releaseChild:shared.releaseChild,
    now:()=>performance.now(),wall:()=>Date.now(),sleep:ms=>new Promise(resolve=>setTimeout(resolve,ms)),budget:shared.budgetCall};
}
// runOwned只持有一个业务顺序；离线端口只能走独立测试入口，正式入口不接受外来端口。
async function runOwned(config,env,observer,options,io,offline=false) {
  assert.ok(observer&&typeof observer.readTransfer==='function','formal_observation_unavailable');
  const window=shared.validateObservationWindow(observer.observationWindow,io.wall());
  const deadline=io.now()+Math.min(180000,window.createdBefore-io.wall());
  const roles=['source','remote','target'],current=new Map(),all=[],opened=new Set();
  let phase='input',rounds=0,aborted=false,passed=false,cancelled=false,readyTargetInputObserved=false,duplicateClose=false;
  let spawnedCount=0; // 只数原spawn已确认事件，不把后继capture失败混作未启动设备。
  let bootInProgress=false; // 两次重启仍属于原boot子步骤，失败不能因外层phase已是abort/commit而丢分类。
  const bootFacts={role:'unknown',step:'none',failureCode:'unknown',page:null}; // 只保最近启动角色，不持有页面正文或设备身份。
  const lastPageFacts={source:null,remote:null,target:null};let lastOperation={role:'unknown',attempt:0,step:'input'},failureCode='unknown',business=null;
  const clickObservations=[]; // 原五次进程流程最多32次点击，只保固定几何事实，不记录selector/参数或身份。
  // operation不推进业务，只标识当前原等待/动作；finally不更新它，不倒灌清理后的页面。
  const operation=(role,attempt,step)=>{lastOperation={role,attempt,step};};
  const freezeBusiness=()=>({failureCode,clickObservations:[...clickObservations],lastOperation:operationProjection(lastOperation),lastPageFacts:Object.fromEntries(roles.map(role=>[role,pageProjection(lastPageFacts[role])]))});
  let fixtureFailure; // 仅离线回归保留原断言cause；正式报告绝不包含自由异常。
  const interrupted=()=>{cancelled=true;};
  process.on('SIGTERM',interrupted);process.on('SIGINT',interrupted);
  // guard只检查原预算和精确当前进程；结束中的旧源由明确waitClose处理，不借任意PID。
  let lastNow=io.now(),lastWall=io.wall(); // 同一总预算的观测时钟，不因prepare或重启重新计时。
  const guard=()=>{const now=io.now(),wall=io.wall();assert.ok(!cancelled&&!duplicateClose&&now>=lastNow&&wall>=lastWall&&now<deadline,'run_interrupted');lastNow=now;lastWall=wall;};
  const wait=read=>shared.waitFor(async()=>{guard();const value=await read();guard();return value;},deadline,
    {cancelled:()=>cancelled,now:io.now,sleep:io.sleep});
  // 本机CLI按逗号/换行拆Chromium参数；与既有共享入口一致显式装假麦克风/原WAV及播放策略。
  const executeBrowser=(held,...args)=>new Promise((resolve,reject)=>{
    guard();io.execFile('agent-browser',['--session',held.session,'--headed',...args],{
      env:{...env,AGENT_BROWSER_ARGS:['--no-sandbox','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream',
        '--use-file-for-fake-audio-capture='+config.audio,'--autoplay-policy=no-user-gesture-required'].join(',')},timeout:Math.max(1,Math.min(10000,deadline-io.now())),maxBuffer:65536},
      (error,stdout)=>error?reject(Error('browser_command_failed')):resolve(stdout.trim()));
  });
  // 坐标点击前复用原固定目标几何核验；CLI成功不代表本人按钮handler已触发，不重发业务动作。
  const browser=async(held,...args)=>{
    if(args[0]==='click'){
      const facts=shared.parseJSON(await executeBrowser(held,'eval','('+prepareBrowserClick.toString()+')('+JSON.stringify(args[1])+')'));
      // 投影先冻结再核验，失败几何仍可区分；报告不持有原页面对象或自由字段。
      if(clickObservations.length<32)clickObservations.push({...operationProjection({...lastOperation,role:held.role}),
        ...Object.fromEntries(['present','enabled','inViewport','hit','scrolled'].map(key=>[key,typeof facts?.[key]==='boolean'?facts[key]:null]))});
      verifyBrowserClick(facts);
    }
    return executeBrowser(held,...args);
  };
  // snapshot只读正式页面/PCM与C++投影，不从DOM按钮填入revision、permit或session。
  const snapshot=async held=>{const value=shared.parseJSON(await browser(held,'eval',
    '(async()=>({...await ('+shared.readPageFacts.toString()+')(),endpoint:aiEndpointSourceState?{...aiEndpointSourceState}:null,'+
    'canAccept:handoffState?.canAccept===true,canCancel:handoffState?.canCancel===true,'+
    'sessionBound:typeof duplexHumanMedia!=="undefined"&&typeof duplexHumanMedia?.scope?.sessionId==="string"&&duplexHumanMedia.scope.sessionId.length>0}))()'));lastPageFacts[held.role]=pageProjection(value);return value;};
  const closeBrowser=async held=>{
    if(!opened.has(held))return;
    await new Promise((resolve,reject)=>io.execFile('agent-browser',['--session',held.session,'close'],{env,timeout:10000,maxBuffer:4096},error=>error?reject(Error('browser_command_failed')):resolve()));
    opened.delete(held);
  };
  // boot每次均为显式纯Human；同设备再次作为候选只在旧实际进程已关闭后启动新原实例。
  const boot=async (role,attempt=0)=>{
    operation(role,attempt,'boot');
    bootInProgress=true;
    bootFacts.role=role;bootFacts.step='spawn';bootFacts.failureCode='unknown';bootFacts.page=null;
    const old=current.get(role);
    assert.ok(!old||old.closed,'old_device_still_running');
    if(old)await closeBrowser(old);
    const index=roles.indexOf(role),device=config.data[role],args=[String(config.data.ports[index]),'--human'];
    const child=io.spawn(config.binary,args,{cwd:root,env:{...env,UR_DEVICE_PRODUCT:device.productID,UR_DEVICE_NAME:device.deviceName,
      UR_DEVICE_SECRET:device.secret,UR_DEVICE_HUMAN_ENDPOINT_SOURCE:'1',UR_DEVICE_CONTEXT_DIAGNOSTIC:'1'},stdio:['ignore','pipe','pipe']});
    const held=shared.trackChild(child,config.binary,root,[config.binary,...args]);
    let closeCount=0; // 原ChildProcess仅有一次close；重复通知不能再次成为后继源或新实例许可。
    child.on('close',()=>{if(++closeCount>1)duplicateClose=true;});
    held.role=role;held.session='pure-human-'+randomUUID();all.push(held);current.set(role,held);
    let ready=false,pending='',overflow=false;
    child.stdout.on('data',bytes=>{if(overflow)return;pending+=bytes.toString('utf8');if(pending.length>8192){overflow=true;pending='';return;}
      let end;while((end=pending.indexOf('\n'))>=0){if(pending.slice(0,end)==='http://127.0.0.1:'+config.data.ports[index]+'/')ready=true;pending=pending.slice(end+1);}});
    child.stderr.resume(); // 不保存自由库输出、标识或密钥；只读原ChildProcess关闭/双流事实。
    await io.budget(()=>held.spawned,deadline);++spawnedCount;bootFacts.step='capture';io.captureChild(held);
    if(options.onSpawn)await io.budget(()=>options.onSpawn(child,held.record),deadline);
    bootFacts.step='readyEndpoint';await wait(()=>{assert.ok(!held.closed&&!overflow,'device_ended_early');return ready;});
    opened.add(held);if(options.onBrowserSession)await io.budget(()=>options.onBrowserSession(held.session),deadline);
    bootFacts.step='openBrowser';await browser(held,'open','http://127.0.0.1:'+config.data.ports[index]+'/');bootFacts.step='activate';await browser(held,'click','#activate');
    bootFacts.step='pageReady';await wait(async()=>{const value=await snapshot(held);bootFacts.page=bootProjection({page:value}).page;
      // 与原共享入口一致：终止页面不可恢复，冻结闭集事实后立即失败，不消耗剩余180秒假等就绪。
      // 三角色均为显式Human源；canEscalate来自原C++能力ACK后的Ready/Available，桥和音频就绪不能替代它。
      assert.ok(!value.terminated,'page_activation_terminated');assert.equal(value.completed,0,'pure_human_created_ai');return value.ready&&value.canEscalate;});
    bootFacts.step='installObservation';await browser(held,'eval','('+shared.installPageObservation.toString()+')()');bootFacts.step='complete';bootInProgress=false;return held;
  };
  // audioPair要求同原Peer上双方新增连续有声PCM，不能用ICE/历史计数替代。
  const audioPair=async pair=>{
    const initial=await wait(async()=>{const values=await Promise.all(pair.map(snapshot));
      return values.every(value=>value.media.connected&&value.media.inputAttached&&value.media.outputEnabled)&&values.map(value=>value.media);});
    await wait(async()=>{try{return shared.verifyAudioPair(initial,(await Promise.all(pair.map(snapshot))).map(value=>value.media));}catch{return false;}});
    return initial;
  };
  // proof绑定本轮原腿与候选；私有标识只传给正式只读observer，不落公共report。
  const proof=async (stage,attempt,source,target,sourceSession,targetSession)=>{
    const value=await io.budget(()=>observer.readTransfer({stage,attempt,sourceSessionId:sourceSession,
      targetSessionId:targetSession,source:config.data[source].productID+':'+config.data[source].deviceName,
      target:config.data[target].productID+':'+config.data[target].deviceName,...window}),deadline);
    if(value===null)return false;
    return verifyTransferProof(value,stage);
  };
  const waitClose=async held=>{
    await io.budget(()=>held.originalClose.wait(),deadline);
    assert.ok(held.closed&&held.code===0&&held.stdoutEnded&&held.stderrEnded,'device_exit_failed');
  };
  try {
    phase='boot';for(const role of roles)await boot(role);
    const source=current.get('source'),remote=current.get('remote');
    phase='dial';operation('source',0,'dialReady');await wait(async()=>(await snapshot(source)).canEscalate);
    operation('source',0,'dialTargetKind');await browser(source,'select','#handoffTargetKind','device');operation('source',0,'dialTarget');await browser(source,'fill','#handoffTarget',config.data.remote.productID+':'+config.data.remote.deviceName);
    operation('source',0,'dialSend');await browser(source,'click','#handoffEscalate');operation('remote',0,'inviteReady');await wait(async()=>{
      // canCancel/sessionBound仅是原C++绑定状态旁证，不把点击成功猜成创建ACK；缺失仍null。
      await snapshot(source);return (await snapshot(remote)).canAccept;
    });operation('remote',0,'inviteAccept');await browser(remote,'click','#handoffAccept');
    phase='audio';operation('source',0,'initialAudio');await audioPair([source,remote]);
    // 先撤一次已ready候选，再两次提交；撤候选不得中断原S/R或替用户恢复输入。
    for(const [attempt,sourceRole,targetRole,abort] of [[1,'source','target',true],[2,'source','target',false],[3,'target','source',false]]) {
      const origin=current.get(sourceRole),other=current.get('remote');
      let target=current.get(targetRole);if(target.closed)target=await boot(targetRole,attempt);
      phase='prepare';operation(sourceRole,attempt,'prepareReady');await wait(async()=>(await snapshot(origin)).endpoint?.canPrepare===true);
      operation(sourceRole,attempt,'originalAudio');const original=await audioPair([origin,other]),sourceSession=original[0].session;
      operation(sourceRole,attempt,'prepareTarget');await browser(origin,'fill','#endpointSourceTarget','device:'+config.data[targetRole].productID+':'+config.data[targetRole].deviceName);
      operation(sourceRole,attempt,'prepareSend');await browser(origin,'click','#endpointSourcePrepare');operation(targetRole,attempt,'inviteReady');await wait(async()=>(await snapshot(target)).canAccept);operation(targetRole,attempt,'inviteAccept');await browser(target,'click','#handoffAccept');
      phase='ready';operation(sourceRole,attempt,'candidateReady');const targetSession=await wait(async()=>{const values=await Promise.all([snapshot(origin),snapshot(other),snapshot(target)]);
        shared.verifyAudioPair(original,values.slice(0,2).map(value=>value.media));
        return values[0].endpoint?.canCommit===true&&values[2].media.session;});
      // 完整Focus可实际采集；先固定当前候选计数，再在有界观察窗联合核平台门关闭与原双端持续有声。
      operation(targetRole,attempt,'readyBaseline');const candidateBaseline=(await snapshot(target)).media,readyAt=io.now();
      // 候选均来自新进程/新UrHumanVoiceMedia/新Worklet，计数从零开始；首次已消费有声也必须拒绝。
      assert.equal(candidateBaseline.nonSilentFrames,0,'candidate_audio_before_commit');
      readyTargetInputObserved ||= candidateBaseline.inputAttached===true;
      operation(sourceRole,attempt,'readyProof');await wait(async()=>{
        const values=await Promise.all([snapshot(origin),snapshot(other),snapshot(target)]);
        shared.verifyAudioPair(original,values.slice(0,2).map(value=>value.media));
        const candidate=values[2].media;
        for(const key of ['session','peerEpoch','peerFingerprint'])assert.equal(candidate[key],candidateBaseline[key],'candidate_peer_changed');
        assert.equal(candidate.nonSilentFrames,candidateBaseline.nonSilentFrames,'candidate_audio_before_commit');
        readyTargetInputObserved ||= candidate.inputAttached===true;
        const confirmed=await proof('ready',attempt,sourceRole,targetRole,sourceSession,targetSession);
        return confirmed&&io.now()-readyAt>=100;
      });
      phase=abort?'abort':'commit';operation(sourceRole,attempt,abort?'abortSend':'commitSend');await browser(origin,'click',abort?'#endpointSourceAbort':'#endpointSourceCommit');
      operation(sourceRole,attempt,'terminalProof');await wait(()=>proof(abort?'aborted':'committed',attempt,sourceRole,targetRole,sourceSession,targetSession));
      if(abort){operation(targetRole,attempt,'targetExit');await waitClose(target);operation(sourceRole,attempt,'retainedAudio');await audioPair([origin,other]);aborted=true;}
      else{operation(sourceRole,attempt,'sourceExit');await waitClose(origin);operation(targetRole,attempt,'committedAudio');await audioPair([target,other]);rounds++;}
    }
    phase='hangup';const finalSource=current.get('source'),finalRemote=current.get('remote');
    operation('source',3,'hangup');await browser(finalSource,'click','#handoffHangup');operation('source',3,'finalExit');
    await wait(async()=>(await Promise.all([snapshot(finalSource),snapshot(finalRemote)])).every(value=>value.released));
    await Promise.all([waitClose(finalSource),waitClose(finalRemote)]);passed=true;phase='completed';operation('source',3,'completed');
  } catch(error) {
    failureCode=shared.projectFailureCode(error);business=freezeBusiness();
    if(bootInProgress)bootFacts.failureCode=shared.projectFailureCode(error);
    if(offline)fixtureFailure=error;
    throw error;
  } finally {
    business ||= freezeBusiness();
    // 只读同步旁证在任何cleanup前冻结；回调异常不改变业务/清理，不补发CLI或刷新预算。
    let diagnosticFailed=false;
    if(options.onBeforeCleanup)try{const result=options.onBeforeCleanup(structuredClone(business));
      // 原端口仅同步内存投影；异步返回也拒绝且吸收拒绝，不能拖住真实清理或泄露自由异常。
      if(result&&typeof result.then==='function'){diagnosticFailed=true;Promise.resolve(result).catch(()=>{});}
    }catch{diagnosticFailed=true;/* 不泄露异常、不覆盖原业务首因，继续所有真实资源释放。 */}
    let browsersClosed=true,devicesClosed=true;
    for(const held of [...opened])try{await closeBrowser(held);}catch{browsersClosed=false;}
    for(const held of all)try{await io.releaseChild(held);}catch{devicesClosed=false;}
    process.removeListener('SIGTERM',interrupted);process.removeListener('SIGINT',interrupted);
    const deviceProcesses={createdCount:all.length,spawnedCount,closedCount:all.filter(held=>held.closed===true).length,
      stdoutEndedCount:all.filter(held=>held.stdoutEnded===true).length,stderrEndedCount:all.filter(held=>held.stderrEnded===true).length};
    const report=projectReport({phase,passed,rounds,aborted,offline,readyTargetInputObserved,actualCPP:!offline&&all.filter(held=>held.record).length>=3,devicesClosed,browsersClosed,boot:bootFacts,deviceProcesses,...business,diagnosticFailed});
    if(options.report)options.report(report);
    if(!(offline?report.fixturePassed:report.passed)){const error=Error('native_human_three_host_failed');error.facts=report;if(offline)error.cause=fixtureFailure;throw error;}
  }
  return projectReport({phase,passed,rounds,aborted,offline,readyTargetInputObserved,actualCPP:!offline,devicesClosed:true,browsersClosed:true,boot:bootFacts,...business,
    deviceProcesses:{createdCount:all.length,spawnedCount,closedCount:all.filter(held=>held.closed===true).length,stdoutEndedCount:all.filter(held=>held.stdoutEnded===true).length,stderrEndedCount:all.filter(held=>held.stderrEnded===true).length}});
}
// run正式入口总是自行创建真实适配；options不携带端口，不能通过环境或调用参数换成fixture。
async function run(env=process.env,observer,options={}) {
  return runOwned(validateEnvironment(env),env,observer,options,createLivePorts());
}
// runOfflineFixture只供有界离线调度，明确标记actualCPP=false/真实passed=false，不能作为实网签收。
async function runOfflineFixture(config,observer,ports,options={}) {
  return runOwned(config,{},observer,options,ports,true);
}
module.exports={verifyTransferProof,projectReport,validateEnvironment,run,runOfflineFixture};
// CLI默认无正式observer，明确拒绝；宿主必须在隔离入口绑定当前平台证明与实际产品签收。
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(error=>{
  console.error(JSON.stringify(error.facts||{passed:false,phase:'input',reason:'formal_observation_unavailable'}));process.exitCode=1;
});
