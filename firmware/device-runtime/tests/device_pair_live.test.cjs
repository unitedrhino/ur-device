// 文件说明：正式两C++设备控制验收的固定证据门槛与真实child close合同，不启动API/媒体/浏览器。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {EventEmitter}=require('node:events');
const {selectScenario,sameOriginal,heldPair,resumedPair,originalControl,childClose,childExitFacts,observeOriginalSpawn,controlFailureSnapshot,run}=require('./device_pair_live.cjs');

// baseline 公开合成原腿与Peer标识，仅用于证明不同对象不能补作原控制成功。
function baseline(index) {
  return {sessionId:`leg-${index}`,peerEpoch:'1',peerFingerprint:`fixed-peer-${index}`,
    connected:true,sameOriginalTracks:true};
}
// held 提供两次实际固定观察，缺任一指标必须拒绝，fixture不生成设备ACK。
function held(index) {
  const first={...baseline(index),actualHeld:true,detachedSenders:2,outputEnabled:false,
    videoPaused:true,videoHidden:true,audioZeroBlocks:100,audioNonSilentBlocks:20,
    audioPackets:100,videoPackets:200,outAudioPackets:90,outVideoPackets:180,videoFrames:30};
  return {first,second:{...first,audioZeroBlocks:150}};
}
// resumed 原媒体重新收发的观测，不以平台active或点击字段替代实际增量。
function resumed(index) {
  return {...held(index).second,actualHeld:false,detachedSenders:0,outputEnabled:true,
    videoPaused:false,videoHidden:false,audioPackets:120,videoPackets:220,
    outAudioPackets:110,outVideoPackets:200,videoFrames:40,audioNonSilentBlocks:40};
}

test('offline import is inert and formal selector rejects unknown or non-string attempts',()=>{
  assert.equal(selectScenario(undefined),'default');
  assert.equal(selectScenario('hold_resume'),'hold_resume');
  for(const value of ['unknown',3,null,'HOLD_RESUME']) assert.throws(()=>selectScenario(value));
});
test('same original session, Peer epoch and SDP fingerprint all must remain unchanged',()=>{
  sameOriginal(baseline(0),baseline(0));
  for(const field of ['sessionId','peerEpoch','peerFingerprint'])
    assert.throws(()=>sameOriginal(baseline(0),{...baseline(0),[field]:'different'}));
  assert.throws(()=>sameOriginal(baseline(0),{...baseline(0),connected:false}));
  assert.throws(()=>sameOriginal(baseline(0),{...baseline(0),sameOriginalTracks:false}));
});
test('hold requires both original devices, detached senders, actual zero blocks and stable RTP',()=>{
  heldPair([baseline(0),baseline(1)],[held(0),held(1)]);
  for(const [field,value] of [['actualHeld',false],['detachedSenders',1],['outputEnabled',true],
    ['videoPaused',false],['videoHidden',false]]) {
    const pair=[held(0),held(1)];pair[1].first[field]=value;
    assert.throws(()=>heldPair([baseline(0),baseline(1)],pair));
  }
  for(const field of ['audioPackets','videoPackets','outAudioPackets','outVideoPackets','audioNonSilentBlocks']) {
    const pair=[held(0),held(1)];++pair[1].second[field];
    assert.throws(()=>heldPair([baseline(0),baseline(1)],pair));
  }
  const pair=[held(0),held(1)];pair[0].second.audioZeroBlocks=pair[0].first.audioZeroBlocks;
  assert.throws(()=>heldPair([baseline(0),baseline(1)],pair));
});
test('resume requires original tracks and actual media in both directions, not just active state',()=>{
  resumedPair([baseline(0),baseline(1)],[held(0),held(1)],[resumed(0),resumed(1)]);
  for(const field of ['audioPackets','videoFrames','outAudioPackets','outVideoPackets','audioNonSilentBlocks']) {
    const values=[resumed(0),resumed(1)];values[1][field]=held(1).second[field];
    assert.throws(()=>resumedPair([baseline(0),baseline(1)],[held(0),held(1)],values));
  }
  const values=[resumed(0),resumed(1)];values[0].detachedSenders=1;
  assert.throws(()=>resumedPair([baseline(0),baseline(1)],[held(0),held(1)],values));
});
test('completed receipt must be unique original200 and actual; Pending or Unknown cannot pass',()=>{
  const value={operation:'hold',state:'completed',code:200,actualApplied:true,actualHeld:true,
    controlToken:'synthetic-original',revision:1};
  assert.equal(originalControl([value],'hold'),value);
  assert.throws(()=>originalControl([value,value],'hold'));
  for(const changed of [{state:'pending'},{state:'unknown'},{code:409},{actualApplied:false},
    {actualHeld:false},{revision:0},{controlToken:''}])
    assert.throws(()=>originalControl([{...value,...changed}],'hold'));
});
test('kill submission and exit event do not replace the exact original child close',async()=>{
  const child=new EventEmitter(), lifetime=childClose(child);
  let settled=false;
  const waiting=lifetime.wait().then(value=>{settled=true;return value;});
  child.emit('exit',0,null);await Promise.resolve();
  assert.equal(lifetime.exited(),false);assert.equal(settled,false);
  child.emit('close',0,null);
  assert.deepEqual(await waiting,{code:0,signal:null});
  assert.equal(lifetime.exited(),true);
  child.emit('close',1,'SIGTERM');
  assert.deepEqual(await lifetime.wait(),{code:0,signal:null});
});
test('只读spawn观察器收到唯一原child，返回值不替换原句柄',()=>{
  const child=new EventEmitter();let calls=0;
  assert.equal(observeOriginalSpawn(child,observed=>{assert.equal(observed,child);calls++;return {fake:true};}),true);
  assert.equal(calls,1);assert.equal(observeOriginalSpawn(child,undefined),true);
});
test('观察异常不改变原close/end事件，仍必须等原child close',async()=>{
  const child=new EventEmitter(),stdout=new EventEmitter(),stderr=new EventEmitter();child.stdout=stdout;child.stderr=stderr;
  const lifetime=childClose(child);let ends=0;stdout.once('end',()=>ends++);stderr.once('end',()=>ends++);
  assert.equal(observeOriginalSpawn(child,()=>{throw Error('private_fixture');}),false);
  assert.equal(lifetime.exited(),false);stdout.emit('end');stderr.emit('end');child.emit('exit',0);
  assert.equal(ends,2);assert.equal(lifetime.exited(),false);child.emit('close',0,null);
  assert.deepEqual(await lifetime.wait(),{code:0,signal:null});
});
test('非法观察器在spawn和正式输入前拒绝，不提供spawn替身入口',async()=>{
  for(const onSpawn of [null,1,'fake',{}])await assert.rejects(run({onSpawn}),/spawn_observer_invalid/);
});
test('退出投影仅真实close/双流end及有界码和信号，无自由原报文',async()=>{
  const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();
  const lifetime=childClose(child);
  assert.equal(lifetime.facts().closed,false);child.emit('exit',1,'SIGTERM');
  assert.equal(lifetime.facts().closed,false);child.stdout.emit('end');
  assert.equal(lifetime.facts().stderrEnded,false);child.stderr.emit('end');child.emit('close',1,null);
  assert.deepEqual(lifetime.facts(),{closed:true,exitCode:1,signal:'none',stdoutEnded:true,stderrEnded:true});
  child.emit('close',0,'SIGKILL');assert.equal(lifetime.facts().exitCode,1);
  for(const code of [-1,NaN,Infinity,256,'1'])assert.equal(childExitFacts({code,signal:'private-signal'},{}).exitCode,null);
  assert.equal(childExitFacts({code:1,signal:'private-signal'},{}).signal,'unknown');
});
test('原run启动业务失败不得被finally原child非零close覆盖（离线VM，不启动资源）',async()=>{
  const vm=require('node:vm'),fs=require('node:fs');
  const env={UR_VIDEO_PAIR_LIVE:'1',UR_VIDEO_PAIR_SCENARIO:'hold_resume',AGENT_BROWSER_HEADED:'1',
    UR_VIDEO_DEVICE_BIN:'/public/fixture',UR_VIDEO_PAIR_FIRST:JSON.stringify({productID:'p1',deviceName:'d1',secret:'public-fixture'}),
    UR_VIDEO_PAIR_SECOND:JSON.stringify({productID:'p2',deviceName:'d2',secret:'public-fixture'}),
    UR_MQTT_PORT:'1883',UR_VIDEO_PAIR_PORT_FIRST:'27891',UR_VIDEO_PAIR_PORT_SECOND:'27892'};
  const diagnostics=[];
  const sandbox={require:name=>name==='node:fs'?{statSync:()=>({isFile:()=>true})}:require(name),
    module:{exports:{}},process:{env},console:{error:value=>diagnostics.push(JSON.parse(value)),log:()=>{}},setTimeout,clearTimeout,
    fixtureBoot:async(_binary,_port,_identity,_mqtt,register)=>{
      const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();
      const lifetime=childClose(child);register({child,...lifetime});
      child.stdout.emit('end');child.stderr.emit('end');child.emit('close',1,null);
      throw Error('public-original-boot-failure');
    },fixtureBrowser:async()=>JSON.stringify({connected:false,available:false,peer:false,capture:false})};
  const source=fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8');
  vm.runInNewContext(source+'\nbootDevice=fixtureBoot;browser=fixtureBrowser;',sandbox);
  await assert.rejects(sandbox.module.exports.run(),/pair_boot_failed/);
  const report=diagnostics.find(value=>value.kind==='pair_cleanup');
  assert.deepEqual(report,{kind:'pair_cleanup',failure:'device_exit_failed',
    children:[{closed:true,exitCode:1,signal:'none',stdoutEnded:true,stderrEnded:true}]});
  sandbox.console.error=()=>{throw Error('private-sink');};
  await assert.rejects(sandbox.module.exports.run(),/pair_boot_failed/);
  // 无业务失败时原非零退出仍不得成功；仅公开fixture接管VM边界，不注入实际run。
  env.UR_VIDEO_PAIR_SCENARIO='reject';
  sandbox.fixtureBoot=async(_binary,_port,_identity,_mqtt,register)=>{
    const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();
    const lifetime=childClose(child);child.kill=()=>{child.stdout.emit('end');child.stderr.emit('end');child.emit('close',1,null);return true;};
    register({child,...lifetime});
  };
  vm.runInNewContext('bootDevice=fixtureBoot;',sandbox);
  await assert.rejects(sandbox.module.exports.run(),/device_exit_failed/);
});
test('原holdResume失败须保留固定audio_baseline断点及收尾前脱敏旁证（离线VM）',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),diagnostics=[];
  const snapshot={actualHeld:false,outputEnabled:false,detachedSenders:0,videoPaused:true,videoHidden:true,
    audioZeroBlocks:0,audioNonSilentBlocks:0,audioPackets:0,videoPackets:0,outAudioPackets:0,outVideoPackets:0,videoFrames:0,
    sessionId:'private-session-fixture',peerFingerprint:'private-sdp-fixture',receipts:[]};
  const sandbox={require,module:{exports:{}},console:{error:value=>diagnostics.push(JSON.parse(value))},process,setTimeout,clearTimeout,
    fixtureBrowser:async(_session,_env,operation)=>{if(operation==='eval')return JSON.stringify(snapshot);throw Error('private-browser-body-fixture');}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8')+
    '\nbrowser=fixtureBrowser;module.exports.holdResumeFixture=holdResume;',sandbox);
  await assert.rejects(sandbox.module.exports.holdResumeFixture(['public-caller','public-callee'],{}));
  const report=diagnostics.find(value=>value.kind==='pair_control_failure');
  assert.ok(report,'必须保留原失败step，不以总hold_resume_cancel代替');
  assert.equal(report.step,'audio_baseline');assert.equal(JSON.stringify(report).includes('private'),false);
});
// failureFacts 用公开原Peer合成值验证纯投影，不代表媒体、ACK或设备端云事实。
function failureFacts(){return {...baseline(0),...held(0).second,receipts:[]};}
test('失败实态只投有界原字段，缺失/越界整体未知而不是补零',()=>{
  const source=failureFacts(),f=controlFailureSnapshot(source,baseline(0));
  assert.equal(f.sampled,true);assert.equal(f.comparisonKnown,true);assert.equal(f.matchedOriginal,true);
  assert.equal(JSON.stringify(f).includes('leg-'),false);assert.equal(Object.hasOwn(f,'peerFingerprint'),false);
  for(const key of ['audioZeroBlocks','audioNonSilentBlocks','audioPackets','videoPackets','outAudioPackets','outVideoPackets','videoFrames','peerEpoch']){
    const missing={...source};delete missing[key];assert.deepEqual(controlFailureSnapshot(missing,baseline(0)),{sampled:false});
  }
  for(const n of [-1,NaN,Infinity,10000001,'1'])assert.deepEqual(controlFailureSnapshot({...source,audioPackets:n}),{sampled:false});
  assert.equal(controlFailureSnapshot(source).comparisonKnown,false);
  assert.equal(controlFailureSnapshot({...source,peerEpoch:'different'},baseline(0)).matchedOriginal,false);
});
test('pending/unknown无ACK原code0合法但不冒完成，未知原token只投比较未知',()=>{
  const receipt={operation:'hold',state:'pending',code:0,revision:0,sessionId:'leg-0',controlToken:'private-token',actualApplied:false,actualHeld:false};
  for(const state of ['pending','unknown']){
    const source={...failureFacts(),receipts:[{...receipt,state}]},f=controlFailureSnapshot(source,baseline(0));
    assert.equal(f.sampled,true);assert.equal(f.receipts[0].code,0);assert.equal(f.receipts[0].comparisonKnown,false);
    assert.equal(JSON.stringify(f).includes('private'),false);
  }
  const source={...failureFacts(),receipts:[{...receipt,state:'completed',code:200,revision:1,actualApplied:true}]};
  assert.equal(controlFailureSnapshot(source,baseline(0),{hold:'private-token'}).receipts[0].matchedOriginal,true);
  assert.equal(controlFailureSnapshot(source,baseline(0),{hold:'other'}).receipts[0].matchedOriginal,false);
  for(const change of [{state:'completed'},{state:'rejected'},{revision:1},{code:undefined},{code:-1},{state:'free'}])
    assert.deepEqual(controlFailureSnapshot({...failureFacts(),receipts:[{...receipt,...change}]}),{sampled:false});
  assert.deepEqual(controlFailureSnapshot({...failureFacts(),receipts:Array(25).fill(receipt)}),{sampled:false});
});
test('八个原控制边界均保留first failure，失败失读不借成功baseline，sink失败隔离',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),source=fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8');
  for(const wanted of ['audio_baseline','hold_actual','hold_receipt','hold_stability','resume_actual','resume_receipt','resume_stability','cancel_rejected']){
    let mode='active',failed=false,diagnosticBoundary=false,reads=0;const firstFailure=Error('public-original-failure'),diagnostics=[];
    // receipt 仅公开可信合同形状；不存在网络、设备、ACK或媒体注入入口。
    const receipt=operation=>({operation,state:'completed',code:200,revision:operation==='hold'?1:2,
      sessionId:'leg-0',controlToken:'private-'+operation,actualApplied:true,actualHeld:operation==='hold'});
    const current=()=>({...((mode==='resumed')?resumed(0):held(0).second),
      ...(mode==='active'?{actualHeld:false,detachedSenders:0,outputEnabled:true,videoPaused:false,videoHidden:false}:{}),
      receipts:mode==='active'?[]:mode==='held'?[receipt('hold')]:[receipt('hold'),receipt('resume')]});
    const sandbox={require,module:{exports:{}},process,setTimeout,clearTimeout,console:{error:value=>diagnostics.push(JSON.parse(value))},
      fixtureBrowser:async(_session,_env,operation,...args)=>{
        const expression=args.at(-1)||'';
        const at=operation==='click'?(args[0]==='#hold'?'hold_actual':args[0]==='#resume'?'resume_actual':'cancel_rejected'):
          operation==='wait'&&expression.includes('controlReceipts')?(expression.includes('"hold"')?'hold_receipt':'resume_receipt'):
          operation==='wait'&&mode==='active'?'audio_baseline':
          operation==='eval'&&expression.includes('setTimeout(resolve,750)')?'hold_stability':
          operation==='eval'&&mode==='resumed'?'resume_stability':null;
        if(!failed&&at===wanted){failed=true;queueMicrotask(()=>{diagnosticBoundary=true;});throw firstFailure;}
        if(diagnosticBoundary&&operation==='eval'){reads++;if(wanted==='resume_stability')throw Error('public-read-failure');}
        if(operation==='click'){mode=args[0]==='#hold'?'held':args[0]==='#resume'?'resumed':mode;return '';}
        if(operation==='eval'&&expression.includes('setTimeout(resolve,750)')){
          const value=current();return JSON.stringify({first:{...value,audioZeroBlocks:100},second:value});
        }
        return operation==='eval'?JSON.stringify(current()):'';
      }};
    vm.runInNewContext(source+'\nbrowser=fixtureBrowser;module.exports.holdResumeFixture=holdResume;',sandbox);
    await assert.rejects(sandbox.module.exports.holdResumeFixture(['public-caller','public-callee'],{}),error=>error===firstFailure);
    const report=diagnostics[0];assert.equal(report.step,wanted);assert.equal(reads,2);
    assert.equal(JSON.stringify(report).includes('private'),false);
    if(wanted==='resume_stability')assert.ok(report.snapshots.every(s=>s.sampled===false));
    if(wanted==='audio_baseline'){
      failed=false;diagnosticBoundary=false;mode='active';sandbox.console.error=()=>{throw Error('public-sink-failure');};
      await assert.rejects(sandbox.module.exports.holdResumeFixture(['public-caller','public-callee'],{}),error=>error===firstFailure);
    }
  }
});
test('失败getStats字段缺失必须未知；原业务统计语义不因诊断改写',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),module={exports:{}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8')+
    '\nmodule.exports.scriptsFixture={business:controlSnapshot,diagnostic:controlDiagnosticSnapshot};',{require,module,process,console,setTimeout,clearTimeout});
  const scripts=module.exports.scriptsFixture,track={};
  // scopeFixture 只执行原只读表达式的公开getStats形状；不接浏览器、Peer网络或媒体驱动。
  const scopeFixture=values=>({peer:{getStats:async()=>values,connectionState:'connected',localDescription:{sdp:'public-fixture'},
    getSenders:()=>[{track},{track}]},capture:{getTracks:()=>[track,track]},controlMetrics:failureFacts,
    item:()=>({paused:true,style:{visibility:'hidden'}}),reports:{videoFrames:30},crypto:require('node:crypto').webcrypto,TextEncoder});
  const values=['inbound-rtp','outbound-rtp'].flatMap(type=>['audio','video'].map(kind=>({type,kind,
    ...(type==='inbound-rtp'?{packetsReceived:10}:{packetsSent:10})})));
  const known=await vm.runInNewContext(scripts.diagnostic,scopeFixture(values));assert.equal(known.audioPackets,10);
  for(const fields of [values.slice(1),[{...values[0],packetsReceived:undefined},...values.slice(1)],
    [{...values[0],packetsReceived:-1},...values.slice(1)]])
    await assert.rejects(vm.runInNewContext(scripts.diagnostic,scopeFixture(fields)),/diagnostic_stat_unknown/);
  const original=await vm.runInNewContext(scripts.business,scopeFixture([{...values[0],packetsReceived:undefined},...values.slice(1)]));
  assert.equal(original.audioPackets,0);
});

// audioScope 只给原失败表达式提供公开RTC统计及轨对象，不创建音源、浏览器或真实Peer。
function audioScope(){
  const local={kind:'audio',id:'private-local',enabled:true,muted:false,readyState:'live'};
  const remote={kind:'audio',id:'private-remote',enabled:true,muted:false,readyState:'live'},video={kind:'video'};
  const stats=['inbound-rtp','outbound-rtp'].flatMap(type=>['audio','video'].map(kind=>({type,kind,
    ...(type==='inbound-rtp'?{packetsReceived:10}:{packetsSent:10}),
    ...(type==='inbound-rtp'&&kind==='audio'?{trackIdentifier:remote.id,totalAudioEnergy:0.25,audioLevel:0.5}:{})})));
  stats.push({type:'media-source',kind:'audio',trackIdentifier:local.id,totalAudioEnergy:1,audioLevel:0.75});
  let reads=0;
  const element={paused:true,style:{visibility:'hidden'},srcObject:{getAudioTracks:()=>[remote]}};
  const scope={peer:{getStats:async()=>{reads++;return stats;},connectionState:'connected',localDescription:{sdp:'public'},
    getSenders:()=>[{track:local},{track:video}],getReceivers:()=>[{track:remote}]},
    capture:{getTracks:()=>[local,video]},controlMetrics:failureFacts,item:()=>element,reports:{videoFrames:30},
    crypto:require('node:crypto').webcrypto,TextEncoder,callAudio:{state:'running'},remoteAudioSource:{}};
  return {scope,stats,local,remote,element,reads:()=>reads};
}
test('原audio_baseline失败一次真实getStats表达式采样原轨音频来源/入站能量且不泄露ID',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),diagnostics=[],pages=[audioScope(),audioScope()];
  const originalError=Error('private-first-failure');
  const sandbox={require,module:{exports:{}},process,setTimeout,clearTimeout,console:{error:value=>diagnostics.push(JSON.parse(value))},
    fixtureBrowser:async(session,_env,operation,expression)=>{if(operation==='wait')throw originalError;
      return JSON.stringify(await vm.runInNewContext(expression,pages[session==='caller'?0:1].scope));}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8')+
    '\nbrowser=fixtureBrowser;module.exports.holdResumeFixture=holdResume;',sandbox);
  await assert.rejects(sandbox.module.exports.holdResumeFixture(['caller','callee'],{}),error=>error===originalError);
  assert.equal(diagnostics[0].step,'audio_baseline');assert.ok(pages.every(page=>page.reads()===1));
  for(const value of diagnostics[0].snapshots){assert.equal(value.sampled,true);assert.deepEqual(value.audioFacts,
    {sourceEnergy:1,sourceLevel:0.75,inboundEnergy:0.25,inboundLevel:0.5,
      localTrackEnabled:true,localTrackMuted:false,localTrackState:'live',remoteTrackEnabled:true,
      remoteTrackMuted:false,remoteTrackState:'live',audioContextState:'running',remoteSourcePresent:true,
      remoteElementBound:true,remoteElementPaused:true});}
  assert.equal(JSON.stringify(diagnostics).includes('private'),false);
});
test('音频来源必须唯一关联原轨；缺失/重复/非法能量为null，元素绑定不是仅非空',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),module={exports:{}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8')+
    '\nmodule.exports.scriptFixture=controlDiagnosticSnapshot;',{require,module,process,console,setTimeout,clearTimeout});
  for(const mutation of ['missing','duplicate','other','invalid','elementOther','tracksAmbiguous']){
    const page=audioScope();
    if(mutation==='missing')for(const value of page.stats){delete value.totalAudioEnergy;delete value.audioLevel;}
    if(mutation==='duplicate')page.stats.push({...page.stats.at(-1)});
    if(mutation==='other')page.stats.at(-1).trackIdentifier='private-other';
    if(mutation==='invalid'){page.stats.at(-1).totalAudioEnergy=Infinity;page.stats.at(-1).audioLevel=-1;}
    if(mutation==='elementOther')page.element.srcObject={getAudioTracks:()=>[{...page.remote}]};
    if(mutation==='tracksAmbiguous')page.scope.capture.getTracks=()=>[page.local,{...page.local}];
    const raw=await vm.runInNewContext(module.exports.scriptFixture,page.scope),facts=controlFailureSnapshot(raw).audioFacts;
    assert.ok(facts,'原失败表达式必须产生固定音频事实');
    if(mutation==='elementOther')assert.equal(facts.remoteElementBound,false);
    else{assert.equal(facts.sourceEnergy,null);assert.equal(facts.sourceLevel,null);}
    assert.equal(page.reads(),1);assert.equal(JSON.stringify(facts).includes('private'),false);
  }
  const facts=controlFailureSnapshot({...failureFacts(),audioFacts:{sourceEnergy:-1,inboundEnergy:'secret',
    audioContextState:'private-error',localTrackState:'private-name',remoteElementBound:'yes',raw:'private-body'}}).audioFacts;
  assert.ok(Object.values(facts).every(value=>value===null));
});
test('原boot失败与原close各保留09dea固定stderr marker，不新增浏览器命令或覆盖first（离线VM）',async()=>{
  const vm=require('node:vm'),fs=require('node:fs'),children=[],commands=[],reports=[];
  const env={UR_VIDEO_PAIR_LIVE:'1',UR_VIDEO_PAIR_SCENARIO:'hold_resume',AGENT_BROWSER_HEADED:'1',UR_VIDEO_DEVICE_BIN:'/public/binary',
    UR_VIDEO_PAIR_FIRST:JSON.stringify({productID:'p1',deviceName:'d1',secret:'public'}),UR_VIDEO_PAIR_SECOND:JSON.stringify({productID:'p2',deviceName:'d2',secret:'public'}),
    UR_MQTT_PORT:'1883',UR_VIDEO_PAIR_PORT_FIRST:'27891',UR_VIDEO_PAIR_PORT_SECOND:'27892'};
  const fakeSpawn=(_binary,args)=>{const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();children.push(child);
    child.kill=()=>{child.stdout.emit('end');child.stderr.emit('end');child.emit('close',null,'SIGTERM');return true;};
    queueMicrotask(()=>{child.emit('spawn');child.stdout.emit('data',Buffer.from('http://127.0.0.1:'+args[0]+'/\n'));});return child;};
  const sandbox={require:name=>name==='node:fs'?{statSync:()=>({isFile:()=>true})}:name==='node:child_process'?{...require(name),spawn:fakeSpawn}:require(name),
    module:{exports:{}},process:{env},console:{error:value=>reports.push(JSON.parse(value)),log:()=>{}},setTimeout,clearTimeout,TextDecoder,Buffer,
    fixtureBrowser:async(_session,_env,command)=>{commands.push(command);if(command==='open'){
      children[0].stderr.emit('data',Buffer.from('private-library-line\ncall_runtime_failed state=2\ncall_failed reason=none\ncall_runtime_diagnostic failure=10 capabilityCode=409 capabilityReason=3\n'));
      children[0].stdout.emit('end');children[0].stderr.emit('end');children[0].emit('close',1,null);throw Error('private-first-failure');}
      if(command==='eval')return JSON.stringify({connected:false,available:false,peer:false,capture:false});return '';}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./device_pair_live.cjs'),'utf8')+'\nbrowser=fixtureBrowser;',sandbox);
  await assert.rejects(sandbox.module.exports.run(),/pair_boot_failed/);
  const expected={seen:true,reason:'none',previousState:2,runtimeDiagnostic:{failure:10,capabilityCode:409,capabilityReason:3},unknown:false,truncated:false,records:3};
  assert.deepEqual(reports.find(value=>value.kind==='pair_boot_failure')?.children[0].stderr,expected);
  assert.deepEqual(reports.find(value=>value.kind==='pair_cleanup').children[0].stderr,expected);
  assert.deepEqual(commands,['open','eval','eval','close','close']);assert.equal(JSON.stringify(reports).includes('private'),false);
});
test('stderr增量UTF8/分片固定投影；自由行丢弃，越界/非法marker未知，原data/end参数不变',()=>{
  const {deviceFailureObservation}=require('./device_pair_live.cjs');
  const stream=new EventEmitter(),seen=[],observation=deviceFailureObservation(stream);
  stream.on('data',value=>seen.push(value));const all=Buffer.from('自由私有正文\ncall_failed reason=call_actual_control\n');
  for(let i=0;i<all.length;i++){const bytes=all.subarray(i,i+1);stream.emit('data',bytes);assert.equal(seen.at(-1),bytes);}
  stream.emit('end');assert.deepEqual(observation.facts(),{seen:true,reason:'call_actual_control',previousState:null,runtimeDiagnostic:null,unknown:false,truncated:false,records:1});
  for(const bytes of [Buffer.from('call_failed reason=private-error\n'),Buffer.from('call_runtime_failed state=14\n'),Buffer.from([0xc3,0x28]),Buffer.alloc(4097,65),Buffer.alloc(1025,65)]){
    const input=new EventEmitter(),value=deviceFailureObservation(input);input.emit('data',bytes);input.emit('end');
    assert.equal(value.facts().unknown,true);assert.equal(JSON.stringify(value.facts()).includes('private'),false);
  }
  const duplicate=new EventEmitter(),value=deviceFailureObservation(duplicate);duplicate.emit('data',Buffer.from('call_failed reason=browser_bridge\ncall_failed reason=device_start\ncall_runtime_failed state=1\n'));
  duplicate.emit('end');assert.equal(value.facts().reason,'browser_bridge');assert.equal(value.facts().previousState,1);assert.equal(value.facts().records,2);assert.equal(value.facts().unknown,true);
  const total=new EventEmitter(),bounded=deviceFailureObservation(total);for(let i=0;i<17;i++)total.emit('data',Buffer.alloc(4096,10));
  total.emit('end');assert.equal(bounded.facts().truncated,true);
});

// 新诊断沿原stderr读取，不新增设备命令；枚举仅证明原失败快照，不代表已定位业务根因。
test('runtime固定诊断完整枚举与ACK边界，零code仅允许None，原data对象不变',()=>{
  const {deviceFailureObservation}=require('./device_pair_live.cjs');
  for(let failure=0;failure<=14;failure++)for(let capabilityReason=0;capabilityReason<=4;capabilityReason++){
    for(const capabilityCode of capabilityReason===0?[0,201,409,599]:[201,409,599]){
      const input=new EventEmitter(),seen=[],observation=deviceFailureObservation(input);
      input.on('data',bytes=>seen.push(bytes));const bytes=Buffer.from(`call_runtime_diagnostic failure=${failure} capabilityCode=${capabilityCode} capabilityReason=${capabilityReason}\n`);
      input.emit('data',bytes);input.emit('end');assert.equal(seen[0],bytes);
      assert.deepEqual(observation.facts(),{seen:true,reason:null,previousState:null,
        runtimeDiagnostic:{failure,capabilityCode,capabilityReason},unknown:false,truncated:false,records:1});
      observation.facts().runtimeDiagnostic.failure=99;assert.equal(observation.facts().runtimeDiagnostic.failure,failure);
    }
  }
});

// 字段顺序、十进制表示和整行内容必须精确，禁止把未知值、自由后缀或正文存入诊断。
test('runtime诊断非法枚举/ACK/零code关联/未知字段拒绝，不保存自由正文',()=>{
  const {deviceFailureObservation}=require('./device_pair_live.cjs');
  const valid='call_runtime_diagnostic failure=10 capabilityCode=409 capabilityReason=3';
  const invalid=[valid.replace('failure=10','failure=15'),valid.replace('failure=10','failure=-1'),
    valid.replace('failure=10','failure=010'),valid.replace('capabilityReason=3','capabilityReason=5'),
    ...[1,100,199,200,600].map(code=>valid.replace('capabilityCode=409',`capabilityCode=${code}`)),
    valid.replace('capabilityCode=409','capabilityCode=0409'),valid.replace('capabilityCode=409','capabilityCode=0'),
    valid+' private-secret',valid+' extra=private-secret',valid.replace('capabilityReason=3','capabilityReason=private-secret'),
    'call_runtime_diagnostic capabilityCode=409 failure=10 capabilityReason=3','call_runtime_diagnostic failure=10'];
  for(const line of invalid){const input=new EventEmitter(),observation=deviceFailureObservation(input);
    input.emit('data',Buffer.from(line+'\n'));input.emit('end');const facts=observation.facts();
    assert.equal(facts.runtimeDiagnostic,null);assert.equal(facts.unknown,true);assert.equal(facts.records,0);
    assert.equal(JSON.stringify(facts).includes('private'),false);
  }
});

// 原end才提交末行；close不是end，重复记录不覆盖首次快照，超长流只停诊断。
test('runtime诊断分片CRLF/末行/end/重复/超长保持有界且三种marker共存',()=>{
  const {deviceFailureObservation}=require('./device_pair_live.cjs');
  const marker='call_runtime_diagnostic failure=10 capabilityCode=409 capabilityReason=3';
  const input=new EventEmitter(),observation=deviceFailureObservation(input);
  const bytes=Buffer.from('私有库正文\ncall_failed reason=none\ncall_runtime_failed state=2\n'+marker+'\r');
  for(let i=0;i<bytes.length;i++)input.emit('data',bytes.subarray(i,i+1));
  input.emit('close');assert.equal(observation.facts().runtimeDiagnostic,null);
  input.emit('end');assert.deepEqual(observation.facts(),{seen:true,reason:'none',previousState:2,
    runtimeDiagnostic:{failure:10,capabilityCode:409,capabilityReason:3},unknown:false,truncated:false,records:3});
  input.emit('data',Buffer.from(marker.replace('failure=10','failure=1')+'\n'));input.emit('end');
  assert.equal(observation.facts().runtimeDiagnostic.failure,10);assert.equal(observation.facts().unknown,false);
  const duplicate=new EventEmitter(),first=deviceFailureObservation(duplicate);
  duplicate.emit('data',Buffer.from(marker+'\n'+marker.replace('failure=10','failure=1')+'\n'));duplicate.emit('end');
  assert.equal(first.facts().runtimeDiagnostic.failure,10);assert.equal(first.facts().records,1);assert.equal(first.facts().unknown,true);
  for(const oversized of [Buffer.from(marker+' '.repeat(1025)+'\n'),Buffer.alloc(4097,65)]){
    const stream=new EventEmitter(),value=deviceFailureObservation(stream);stream.emit('data',oversized);stream.emit('end');
    assert.equal(value.facts().runtimeDiagnostic,null);assert.equal(value.facts().unknown,true);assert.equal(value.facts().truncated,true);
  }
});
