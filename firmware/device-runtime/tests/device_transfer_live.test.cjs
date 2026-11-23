// 文件说明：三设备转接验收入口的离线准入与固定事实合同，不启动浏览器、后端或设备。
'use strict';
// 浏览器只提供媒体驱动，不得继承测试进程携带的设备凭据。
require('node:test')('浏览器环境排除设备凭据且保留显示配置，不修改原设备环境',()=>{
  const assert=require('node:assert/strict'),{browserEnvironment}=require('./device_transfer_live.cjs');
  const original={DISPLAY:':fixture',AGENT_BROWSER_ARGS:'--fixture',UR_VIDEO_TRANSFER_IDENTITIES:'PRIVATE-IDENTITIES',
    UR_DEVICE_SECRET:'PRIVATE-SECRET',UR_VIDEO_PAIR_FIRST:'PRIVATE-FIRST',UR_VIDEO_PAIR_SECOND:'PRIVATE-SECOND'};
  const projected=browserEnvironment(original);
  for(const key of ['UR_VIDEO_TRANSFER_IDENTITIES','UR_DEVICE_SECRET','UR_VIDEO_PAIR_FIRST','UR_VIDEO_PAIR_SECOND'])assert.ok(!Object.hasOwn(projected,key));
  assert.equal(projected.DISPLAY,original.DISPLAY);assert.equal(projected.AGENT_BROWSER_ARGS,original.AGENT_BROWSER_ARGS);
  assert.equal(original.UR_VIDEO_TRANSFER_IDENTITIES,'PRIVATE-IDENTITIES');assert.equal(original.UR_DEVICE_SECRET,'PRIVATE-SECRET');
});
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const {EventEmitter}=require('node:events');
const {childClose}=require('./device_pair_live.cjs');
const {validateConfig,selectScenario,mediaGrowth,transferResult,releasedMedia,installOrderObserver,releaseOrder,rememberMedia}=require('./device_transfer_live.cjs');

// environment 仅使用公开合成身份，真实凭据只能由外层受控环境注入。
function environment() {
  return {UR_VIDEO_TRANSFER_LIVE:'1',UR_VIDEO_TRANSFER_SCENARIO:'success',AGENT_BROWSER_HEADED:'1',
    UR_VIDEO_DEVICE_BIN:'/public/fixture',UR_MQTT_PORT:'1883',UR_VIDEO_TRANSFER_PORTS:'[28001,28002,28003]',
    UR_VIDEO_TRANSFER_IDENTITIES:JSON.stringify([1,2,3].map(value=>({productID:'p',deviceName:'d'+value,secret:'public-fixture'})))};
}

test('validator selects exactly one scenario and three unique identities and loopback ports',()=>{
  for(const scenario of ['success','reject','cancel','timeout']) {
    const config=validateConfig({...environment(),UR_VIDEO_TRANSFER_SCENARIO:scenario});
    assert.equal(config.scenario,scenario);assert.equal(config.identities.length,3);assert.equal(config.ports.length,3);
  }
  for(const value of [undefined,null,'','default','SUCCESS',1]) assert.throws(()=>selectScenario(value));
});

test('invalid admission, identity, capability and port facts fail before any child or browser action',()=>{
  for(const patch of [{UR_VIDEO_TRANSFER_LIVE:'0'},{AGENT_BROWSER_HEADED:'0'},
    {UR_VIDEO_TRANSFER_PORTS:'[28001,28001,28003]'},{UR_VIDEO_TRANSFER_PORTS:'[28001,28002]'},
    {UR_VIDEO_TRANSFER_PORTS:'[1883,28002,28003]'},{UR_VIDEO_TRANSFER_PORTS:'[28001,"28002",28003]'},
    {UR_VIDEO_TRANSFER_PORTS:'[1023,28002,28003]'},{UR_MQTT_PORT:'01883'},{UR_VIDEO_DEVICE_BIN:'relative'},
    {UR_VIDEO_TRANSFER_IDENTITIES:'{}'},{UR_VIDEO_TRANSFER_IDENTITIES:'not-json'}])
    assert.throws(()=>validateConfig({...environment(),...patch}));
  const identities=JSON.parse(environment().UR_VIDEO_TRANSFER_IDENTITIES);
  for(const third of [{...identities[0]}, {...identities[2],secret:''},
    {...identities[2],deviceName:'d/3'}, {...identities[2],owner:true}, {...identities[2],productID:'p:other'}])
    assert.throws(()=>validateConfig({...environment(),UR_VIDEO_TRANSFER_IDENTITIES:JSON.stringify([identities[0],identities[1],third])}));
});

// sample 仅模拟固定只读事实；测试不会生成ACK、修改真实轨或播放媒体。
function sample(index=0) {
  return {state:'connected',sessionId:'leg-'+index,peerEpoch:'1',peerFingerprint:'peer-'+index,
    trackFingerprint:'tracks-'+index,pixelFingerprint:'pixels-'+index+'-10',videoPresented:true,
    connected:true,sameOriginalTracks:true,peerPresent:true,capturePresent:true,
    outputEnabled:true,actualHeld:false,audioPackets:10,videoPackets:10,outAudioPackets:10,outVideoPackets:10,
    videoFrames:10,audioNonSilentBlocks:10,pixelSum:4000,transferOverflow:false,transferReceipts:[]};
}
// growth 必须来自两次原对象读数，不以连接枚举或单张像素替代真实媒体增量。
function growth(index=0) {
  return {...sample(index),audioPackets:20,videoPackets:20,outAudioPackets:20,outVideoPackets:20,
    videoFrames:20,audioNonSilentBlocks:20,pixelFingerprint:'pixels-'+index+'-20'};
}
// receipt 原转接与撤候选的关联仅在内存中校验，日志不得保留token或原腿ID。
function receipt(overrides={}) {
  return {kind:'callTransferReceipt',sessionId:'leg-0',controlToken:'original-transfer',state:'transferred',
    code:200,ackRevision:1,resultRevision:2,reason:'',candidatePresent:true,cancelToken:'',
    cancelState:'idle',cancelCode:0,cancelReason:'',...overrides};
}

test('actual media growth requires original leg, Peer, tracks, both directions, non-silent PCM and decoded pixels',()=>{
  mediaGrowth(sample(),growth());
  for(const key of ['sessionId','peerEpoch','peerFingerprint','trackFingerprint'])
    assert.throws(()=>mediaGrowth(sample(),{...growth(),[key]:'different'}));
  for(const key of ['audioPackets','videoPackets','outAudioPackets','outVideoPackets','videoFrames','audioNonSilentBlocks'])
    assert.throws(()=>mediaGrowth(sample(),{...growth(),[key]:10}));
  for(const patch of [{pixelSum:0},{outputEnabled:false},{connected:false},{actualHeld:true},
    {sameOriginalTracks:false},{audioPackets:NaN},{capturePresent:false},{videoPresented:false},
    {trackFingerprint:undefined},{pixelFingerprint:undefined},{pixelFingerprint:'pixels-0-10'}])
    assert.throws(()=>mediaGrowth(sample(),{...growth(),...patch}));
});

test('result requires one original200 acceptance plus independent terminal and cancellation ACK; unknown cannot pass',()=>{
  for(const [scenario,value] of [['success',receipt()],['reject',receipt({state:'failed',reason:'transferRejected'})],
    ['timeout',receipt({state:'failed',reason:'transferTimeout'})],['cancel',receipt({state:'cancelled',reason:'candidateCancelled',
      cancelToken:'original-cancel',cancelState:'completed',cancelCode:200})]])
    assert.equal(transferResult([value],sample(),scenario,'original-transfer').state,value.state);
  for(const patch of [{state:'unknown'},{code:0},{code:409},{resultRevision:0},{sessionId:'different'},
    {controlToken:'different'},{candidatePresent:false}])
    assert.throws(()=>transferResult([receipt(patch)],sample(),'success','original-transfer'));
  assert.throws(()=>transferResult([receipt(),receipt()],sample(),'success','original-transfer'));
  for(const patch of [{cancelState:'unknown'},{cancelCode:0},{cancelToken:'original-transfer'},
    {reason:'transferTimeout'},{state:'transferring'}])
    assert.throws(()=>transferResult([receipt({state:'cancelled',reason:'candidateCancelled',cancelToken:'original-cancel',
      cancelState:'completed',cancelCode:200,...patch})],sample(),'cancel','original-transfer'));
});

test('cleared page references cannot replace actual original Peer closed and both original tracks ended',()=>{
  const value={peerPresent:false,capturePresent:false,outputEnabled:false,
    releaseFacts:{originalPeerClosed:true,originalTracksEnded:true}};
  releasedMedia(value);
  for(const patch of [{peerPresent:true},{capturePresent:true},{outputEnabled:true},{releaseFacts:null},
    {releaseFacts:{originalPeerClosed:false,originalTracksEnded:true}},
    {releaseFacts:{originalPeerClosed:true,originalTracksEnded:false}}])
    assert.throws(()=>releasedMedia({...value,...patch}));
  const original={connectionState:'connected'},tracks=[{readyState:'live'},{readyState:'live'}];
  const sandbox={peer:original,capture:{getTracks:()=>tracks}};vm.runInNewContext(rememberMedia,sandbox);
  sandbox.peer=null;sandbox.capture=null;assert.equal(sandbox.__transferRelease().originalTracksEnded,false);
  original.connectionState='closed';tracks.forEach(track=>{track.readyState='ended';});
  assert.equal(sandbox.__transferRelease().originalPeerClosed,true);assert.equal(sandbox.__transferRelease().originalTracksEnded,true);
});

test('module import and flag-off CLI perform no spawn, browser or filesystem preflight',async()=>{
  const source=fs.readFileSync(require.resolve('./device_transfer_live.cjs'),'utf8');
  let effects=0;
  const module={exports:{}},request=name=>name==='node:child_process'?{
    spawn:()=>{effects++;throw Error('unexpected');},execFile:()=>{effects++;throw Error('unexpected');}}:
    name==='node:fs'?{statSync:()=>{effects++;throw Error('unexpected');}}:require(name);
  request.main={};
  const sandbox={module,require:request,process:{env:{}},console:{log(){},error(){}},setTimeout,clearTimeout,Buffer};
  vm.runInNewContext(source,sandbox);assert.equal(effects,0);
  await assert.rejects(module.exports.run(),/transfer_preflight_failed/);assert.equal(effects,0);
  const cli={...sandbox,module:{exports:{}},process:{env:{}}};
  request.main=cli.module;vm.runInNewContext(source,cli);await new Promise(resolve=>setImmediate(resolve));
  assert.equal(effects,0);assert.equal(cli.process.exitCode,1);
});

test('passive incoming observer preserves original call, receiver, arguments, return and exception without imposing cross-notification order',()=>{
  let calls=0,receiver,args;
  const sentinel={},failure=Error('public-original-error'),sandbox={peer:{connectionState:'connected'},
    capture:{getTracks:()=>[{readyState:'live'},{readyState:'live'}]},outputEnabled:true,
    incoming:function(...values){calls++;receiver=this;args=values;if(values[0].throws)throw failure;return sentinel;}};
  vm.runInNewContext(installOrderObserver,sandbox);
  const context={},value={kind:'callTransferReceipt',state:'transferred'};
  assert.equal(sandbox.incoming.call(context,value,'original-extra'),sentinel);
  assert.equal(calls,1);assert.equal(receiver,context);assert.deepEqual(args,[value,'original-extra']);
  assert.throws(()=>sandbox.incoming({kind:'callStop',throws:true}),error=>error===failure);assert.equal(calls,2);
  const observed=releaseOrder(sandbox.__transferOrder());assert.equal(observed.notificationBeforeStop,true);
  sandbox.capture.getTracks=()=>{throw Error('public-observation-error');};
  assert.equal(sandbox.incoming({kind:'callStop'}),sentinel);assert.equal(calls,3);
  assert.deepEqual(releaseOrder(sandbox.__transferOrder()),{sampled:false});
  assert.equal(releaseOrder({overflow:false,records:[{kind:'callStop'},{kind:'callTransferReceipt'}]}).notificationBeforeStop,false);
  assert.equal(releaseOrder({overflow:false,records:[]}).notificationBeforeStop,null);
});

test('notification observer is bounded and never records free platform fields or identifiers',()=>{
  let calls=0;const sandbox={peer:null,capture:null,outputEnabled:false,incoming:()=>{calls++;}};
  vm.runInNewContext(installOrderObserver,sandbox);
  for(let index=0;index<20;++index)sandbox.incoming({kind:'callStop',secret:'private-synthetic-body'});
  assert.equal(calls,20);const facts=sandbox.__transferOrder();assert.equal(facts.records.length,16);assert.equal(facts.overflow,true);
  assert.ok(!JSON.stringify(facts).includes('private-synthetic-body'));
});

// runFixture 只验证真实入口的命令分支与收尾顺序，受控I/O不是MQTT/浏览器/三方实际验收。
function runFixture(scenario,{failWait=false,badExit=false,diagnosticFailure=false,bootDiagnostics=false,bootWaitFailure=false,cacheFailure=false}={}) {
  const commands=[],children=[],diagnostics=[],cacheCalls=[],counts=[0,0,0];let terminal=false,hungup=false;
  const env={...environment(),UR_VIDEO_TRANSFER_SCENARIO:scenario};
  if(bootDiagnostics)env.UR_CALL_BOOT_DIAGNOSTICS='1'; // 公开离线开关仅驱动真实诊断分支。
  const sandbox={module:{exports:{}},process:{env},setTimeout,clearTimeout,Buffer,
    require:name=>name==='node:fs'?{statSync:()=>({isFile:()=>true,mode:0o700})}:name==='./call_boot_observer.cjs'?{
      ...require(name),readOriginalCache:async(session)=>{cacheCalls.push(session);if(cacheFailure)throw Error('PRIVATE-CACHE');return require(name).unknown();}}:require(name),
    console:{log(){},error:value=>diagnostics.push(JSON.parse(value))},
    // fixtureBoot 沿共享原close/end合同登记合成child，不向实际run开放spawn替身参数。
    fixtureBoot:async(_config,_index,_env,register)=>{
      const child=new EventEmitter();child.stdout=new EventEmitter();child.stderr=new EventEmitter();
      const lifetime=childClose(child);
      child.kill=()=>{child.stdout.emit('end');child.stderr.emit('end');child.emit('close',badExit?1:0,null);return true;};
      children.push(child);register({child,...lifetime,...(bootWaitFailure?{facts:()=>({...lifetime.facts(),stderr:{seen:true,reason:'call_actual_control',unknown:false}})}:{})}); // 公开原固定失败夹具只核保留，不冒认真实C++运行。
    },
    // fixtureBrowser 只返回公开合成原读数，用于验证是否走错UI操作或错误地接受cleanup失败。
    fixtureBrowser:async(session,_env,...args)=>{
      const index=Number(/^transfer-([0-2])-/.exec(session)[1]);commands.push({index,args});
      if((failWait&&args[0]==='wait'&&args[2].includes('latestTransfer'))||(bootWaitFailure&&args[0]==='wait'&&args[2]==='state==="available"'))throw Object.assign(Error('PRIVATE-WAIT-BODY'),
        {browserOperation:'wait',browserFailure:'deadline',code:'ETIMEDOUT'});
      if(args[0]==='wait'&&args[2].includes('transferReceipts.some'))terminal=true;
      if(args[0]==='click'&&args[1]==='#hangup')hungup=true;
      if(args[0]!=='eval')return '';
      if(args[1].includes('diagnosticSnapshot')) {
        if(diagnosticFailure)throw Error('PRIVATE-SNAPSHOT-BODY');
        return JSON.stringify({diagnosticSnapshot:true,state:index===2?'available':'connected',closed:false,
          peerPresent:index!==2,capturePresent:index!==2,outputEnabled:index!==2,
          latest:{state:'unknown',code:0,candidatePresent:false,sessionMatched:true,controlTokenPresent:true},
          PRIVATE:'PRIVATE-SECRET-TOKEN-ADDRESS'});
      }
      if(args[1]===installOrderObserver||args[1]===rememberMedia||args[1]===require('./call_boot_observer.cjs').installSource)return 'true';
      if(args[1]==='({...latestTransfer})')return JSON.stringify(receipt({state:'transferring',resultRevision:0}));
      if(args[1]==='({peer:!!peer,capture:!!capture})')return '{"peer":false,"capture":false}';
      const count=++counts[index]*10;
      const facts={...sample(index),audioPackets:count,videoPackets:count,outAudioPackets:count,
        outVideoPackets:count,videoFrames:count,audioNonSilentBlocks:count,pixelFingerprint:'pixels-'+index+'-'+count};
      if(index===0&&terminal){
        const result=scenario==='success'?receipt():scenario==='cancel'?receipt({state:'cancelled',reason:'candidateCancelled',
          cancelToken:'original-cancel',cancelState:'completed',cancelCode:200}):receipt({state:'failed',
            reason:scenario==='reject'?'transferRejected':'transferTimeout'});
        facts.transferReceipts=[result];
        if(scenario==='success')Object.assign(facts,{peerPresent:false,capturePresent:false,outputEnabled:false,
          releaseFacts:{originalPeerClosed:true,originalTracksEnded:true},
          order:{overflow:false,records:[{kind:'callStop'},{kind:'callTransferReceipt',peerConnected:false,captureLive:false,outputEnabled:false}]}});
      }
      if(hungup)Object.assign(facts,{peerPresent:false,capturePresent:false,outputEnabled:false,
        releaseFacts:{originalPeerClosed:true,originalTracksEnded:true}});
      return JSON.stringify(facts);
    }};
  const source=fs.readFileSync(require.resolve('./device_transfer_live.cjs'),'utf8');
  vm.runInNewContext(source+'\nbootDevice=fixtureBoot;browser=fixtureBrowser;',sandbox);
  return {run:sandbox.module.exports.run,commands,children,diagnostics,cacheCalls};
}

test('默认关闭不增加安装或缓存，显式诊断只在原open之后click之前安装',async()=>{const observer=require('./call_boot_observer.cjs'),plain=runFixture('success');await plain.run();assert.equal(plain.commands.some(v=>v.args[1]===observer.installSource),false);assert.equal(plain.cacheCalls.length,0);const selected=runFixture('success',{bootDiagnostics:true});await selected.run();assert.equal(selected.commands.filter(v=>v.args[1]===observer.installSource).length,3);assert.equal(selected.cacheCalls.length,0);for(let index=0;index<3;index++){const commands=selected.commands.filter(v=>v.index===index);assert.equal(commands[0].args[0],'open');assert.equal(commands[1].args[1],observer.installSource);assert.deepEqual(commands[2].args,['click','#activate']);assert.equal(commands[3].args[0],'wait');}});
test('实际run首端boot失败仅读一次原缓存，cache失败隔离不覆盖首失败/原收尾',async()=>{const selected=runFixture('success',{bootDiagnostics:true,bootWaitFailure:true,cacheFailure:true});let first;await assert.rejects(selected.run(),error=>{first=error;return error.message==='transfer_boot_failed';});assert.equal(first.facts.browserFailure,'deadline');assert.equal(first.facts.bootObservations.length,1);assert.equal(first.facts.bootObservations[0].sampled,false);assert.equal(selected.cacheCalls.length,1);assert.equal(selected.commands.filter(v=>v.args[0]==='open').length,1);assert.equal(selected.commands.filter(v=>v.args[0]==='close').length,1);assert.equal(JSON.stringify(first.facts).includes('PRIVATE'),false);assert.ok(selected.diagnostics.at(-1).children.every(c=>c.closed&&c.stdoutEnded&&c.stderrEnded));});
test('显式boot诊断只读缓存且不再eval/open/wait，默认与非boot保持原失败采样',async()=>{const observer=require('./call_boot_observer.cjs'),diagnostic=runFixture('success',{bootDiagnostics:true,bootWaitFailure:true});let first;await assert.rejects(diagnostic.run(),error=>{first=error;return error.message==='transfer_boot_failed';});assert.equal(first.facts.pages[0].sampled,false);assert.equal(first.facts.pages[0].closed,null);assert.equal(first.facts.pages[0].peerPresent,null);assert.equal(diagnostic.commands.filter(v=>v.args[0]==='eval').length,1);assert.equal(diagnostic.commands.find(v=>v.args[0]==='eval').args[1],observer.installSource);assert.equal(diagnostic.commands.filter(v=>v.args[0]==='wait').length,1);assert.equal(diagnostic.commands.filter(v=>v.args[0]==='open').length,1);assert.equal(diagnostic.diagnostics.at(-1).children[0].stderr.reason,'call_actual_control');for(const options of [{bootWaitFailure:true},{bootDiagnostics:true,failWait:true}]){const old=runFixture('success',options);await assert.rejects(old.run());assert.ok(old.commands.some(v=>v.args[0]==='eval'&&v.args[1].includes('diagnosticSnapshot')));}});
test('真实Node CLI默认关闭只拒准入，不进入spawn或浏览器，模块导入无副作用',()=>{const cp=require('node:child_process'),file=require.resolve('./device_transfer_live.cjs');const imported=cp.spawnSync(process.execPath,['-e','require('+JSON.stringify(file)+')'],{env:{PATH:process.env.PATH},encoding:'utf8',timeout:5000});assert.equal(imported.status,0);assert.equal(imported.stdout,'');assert.equal(imported.stderr,'');const cli=cp.spawnSync(process.execPath,[file],{env:{PATH:process.env.PATH},encoding:'utf8',timeout:5000});assert.equal(cli.status,1);assert.equal(cli.stdout,'');assert.equal(cli.stderr.trim(),'transfer_preflight_failed');});

test('production run command branches send only intended UI choices; timeout sends no synthetic reject or cancel',async()=>{
  for(const scenario of ['success','reject','cancel','timeout']) {
    const fixture=runFixture(scenario),result=await fixture.run();
    assert.equal(result.result,'pass');assert.equal(result.childrenClosed,true);
    const clicked=(index,id)=>fixture.commands.filter(value=>value.index===index&&value.args[0]==='click'&&value.args[1]===id).length;
    assert.equal(clicked(0,'#transfer'),1);assert.equal(clicked(1,'#accept'),1);
    assert.equal(clicked(2,'#accept'),scenario==='success'?1:0);
    assert.equal(clicked(2,'#reject'),scenario==='reject'?1:0);
    assert.equal(clicked(0,'#cancelTransfer'),scenario==='cancel'?1:0);
    assert.equal(clicked(scenario==='success'?1:0,'#hangup'),1);
    assert.equal(fixture.commands.filter(value=>value.args[0]==='close').length,3);
    assert.equal(fixture.children.length,3);
    for(const value of fixture.commands.filter(value=>value.args[0]==='eval')) {
      assert.ok(!/\.click\(|incoming\s*\(\s*\{|actualHeld\s*=|outputEnabled\s*=\s*true/.test(value.args[1]));
    }
    if(scenario==='success')assert.equal(result.notificationOrder.notificationBeforeStop,false,
      '通知顺序仅旁证，不凭不同顺序否认正式终态');
  }
});

test('original failure remains primary while every exact child close and stream end is collected; cleanup failure cannot pass',async()=>{
  const failed=runFixture('success',{failWait:true,badExit:true});
  await assert.rejects(failed.run(),/transfer_transfer_acceptance_failed/);
  const report=failed.diagnostics.at(-1);assert.equal(report.failure,'unknown');assert.equal(report.children.length,3);
  assert.ok(report.children.every(value=>value.closed&&value.stdoutEnded&&value.stderrEnded&&value.exitCode===1));
  await assert.rejects(runFixture('reject',{badExit:true}).run(),/transfer_cleanup_failed/);
});

test('首次业务子步骤和固定浏览器失败分类在cleanup前采样，收尾不能覆盖原失败',async()=>{
  const fixture=runFixture('success',{failWait:true,badExit:true});let caught;
  await assert.rejects(fixture.run(),error=>{caught=error;return error.message==='transfer_transfer_acceptance_failed';});
  const facts=caught.facts;
  assert.equal(facts.stage,'transfer_acceptance');assert.equal(facts.substep,'transfer_ack_wait');
  assert.equal(facts.failureType,'browser_command');assert.equal(facts.browserOperation,'wait');
  assert.equal(facts.browserFailure,'deadline');assert.equal(facts.pages.length,3);
  assert.equal(facts.pages[0].sampled,true);assert.equal(facts.pages[0].latest.state,'unknown');
  assert.equal(facts.pages[2].peerPresent,false);
  assert.ok(!JSON.stringify(facts).includes('PRIVATE'));
  const reads=fixture.commands.filter(value=>value.args[0]==='eval'&&value.args[1].includes('diagnosticSnapshot'));
  assert.equal(reads.length,3);const firstClose=fixture.commands.findIndex(value=>value.args[0]==='close');
  assert.ok(reads.every(value=>fixture.commands.indexOf(value)<firstClose));
  assert.equal(fixture.diagnostics.at(-1).failure,'unknown');
  assert.equal(fixture.diagnostics.find(value=>value.kind==='transfer_failure').substep,'transfer_ack_wait');
});

test('采样失败是未采到而非页面false，固定投影截断非法与自由字段',async()=>{
  const fixture=runFixture('reject',{failWait:true,diagnosticFailure:true});let caught;
  await assert.rejects(fixture.run(),error=>{caught=error;return error.message==='transfer_transfer_acceptance_failed';});
  assert.ok(caught.facts.pages.every(page=>page.sampled===false&&page.closed===null&&page.peerPresent===null));
  const {failureDiagnosticFacts}=require('./device_transfer_live.cjs');
  const value=failureDiagnosticFacts({diagnosticSnapshot:true,state:'PRIVATE-FREE-TEXT',closed:'false',
    receiptCount:25,audioPackets:Infinity,latest:{state:'PRIVATE-STATE',code:999,reason:'PRIVATE-ERROR',
      candidatePresent:'yes',controlToken:'PRIVATE-TOKEN'},secret:'PRIVATE-SECRET'});
  assert.equal(value.sampled,true);assert.equal(value.state,null);assert.equal(value.closed,null);
  assert.equal(value.receiptCount,null);assert.equal(value.audioPackets,null);assert.equal(value.latest.state,null);
  assert.equal(value.latest.reason,null);assert.equal(value.latest.code,null);assert.equal(value.latest.candidatePresent,null);
  assert.ok(!JSON.stringify(value).includes('PRIVATE'));
  assert.equal(failureDiagnosticFacts(null).sampled,false);
});

test('真实browser包装保留固定操作与有界失败类别，不回显CLI正文或自由错误',async()=>{
  const source=fs.readFileSync(require.resolve('./device_transfer_live.cjs'),'utf8');
  for(const [error,expected]of [[{code:'ETIMEDOUT'},'deadline'],[{killed:true},'deadline'],
    [{code:'ERR_CHILD_PROCESS_STDIO_MAXBUFFER',killed:true},'output_limit'],[{code:'ENOENT'},'spawn_failed'],
    [{code:1},'command_exit'],[{code:'PRIVATE-CODE'},'unknown']]) {
    let commands=0;const module={exports:{}},sandbox={module,process:{env:{}},setTimeout,clearTimeout,Buffer,
      require:name=>name==='node:child_process'?{execFile:(_binary,args,_options,callback)=>{
        commands++;assert.equal(args[2],'eval');assert.equal(args[3],'!!(true)');
        assert.ok(_options.timeout<=25000);callback(Object.assign(Error('PRIVATE-ERROR'),error),'PRIVATE-CLI-BODY');}}:require(name)};
    vm.runInNewContext(source+'\nmodule.exports.testBrowser=browser;',sandbox);
    await assert.rejects(module.exports.testBrowser('transfer-0-public',{},'wait','--fn','true'),failure=>{
      assert.equal(failure.message,'browser_command_failed');assert.equal(failure.browserOperation,'wait');
      assert.equal(failure.browserFailure,expected);assert.ok(!JSON.stringify(failure).includes('PRIVATE'));return true;
    });assert.equal(commands,1); // 分类绝不重试原命令。
  }
  const {failureClass}=require('./device_transfer_live.cjs');
  assert.deepEqual(failureClass({code:'ERR_ASSERTION',message:'PRIVATE'}),
    {failureType:'condition_failed',browserOperation:null,browserFailure:null});
  assert.deepEqual(failureClass({browserOperation:'PRIVATE',browserFailure:'PRIVATE'}),
    {failureType:'unknown',browserOperation:null,browserFailure:null});
});

// conditionFixture 用可控单调时钟执行实际browser入口，不借长等待或真实页面凑边界。
function conditionFixture(values,{advance=0,error=null}={}) {
  const source=fs.readFileSync(require.resolve('./device_transfer_live.cjs'),'utf8');
  const module={exports:{}},commands=[];let now=0;
  const sandbox={module,process:{env:{}},Buffer,clearTimeout,
    setTimeout:(callback,ms)=>{now+=ms;queueMicrotask(callback);},
    require:name=>name==='node:perf_hooks'?{performance:{now:()=>now}}:
      name==='node:child_process'?{execFile:(_binary,args,options,callback)=>{
        commands.push({args,timeout:options.timeout});now+=advance;
        callback(error,JSON.stringify(values.length?values.shift():false));}}:require(name)};
  vm.runInNewContext(source+'\nmodule.exports.testBrowser=browser;',sandbox);
  return {run:()=>module.exports.testBrowser('public-condition-session',{},'wait','--fn','window.ready===true'),commands,now:()=>now};
}

test('55秒观察只用短eval，false只重读同条件，不排入daemon长wait或业务操作',async()=>{
  const fixture=conditionFixture([false,false,true]);await fixture.run();
  assert.equal(fixture.commands.length,3);
  for(const command of fixture.commands){assert.equal(command.args[2],'eval');
    assert.equal(command.args[3],'!!(window.ready===true)');assert.ok(command.timeout<=25000);}
  assert.equal(fixture.now(),200);
});

test('同55秒截止缩短最后命令预算，不越期限追加求值或重发click',async()=>{
  const fixture=conditionFixture([false],{advance:24000});
  await assert.rejects(fixture.run(),error=>error.browserOperation==='wait'&&error.browserFailure==='deadline');
  assert.equal(fixture.commands.length,3);assert.equal(fixture.commands[2].timeout,6800);
  assert.equal(fixture.now(),72200); // 原命令若无视timeout晚回，不能以晚true越过原期限。
});

test('非法条件结果与表达式/SDK错误立即失败，不能吞成false继续等待',async()=>{
  for(const raw of ['true',null,{},1]){const fixture=conditionFixture([raw]);
    await assert.rejects(fixture.run(),error=>error.browserOperation==='wait'&&error.browserFailure==='invalid_result');
    assert.equal(fixture.commands.length,1);}
  const fixture=conditionFixture([],{error:Object.assign(Error('PRIVATE-SDK-ERROR'),{code:1})});
  await assert.rejects(fixture.run(),error=>error.browserOperation==='wait'&&error.browserFailure==='command_exit');
  assert.equal(fixture.commands.length,1);
});

test('晚返回true不延长55秒期限，及时true只完成原观察',async()=>{
  const late=conditionFixture([true],{advance:55000});
  await assert.rejects(late.run(),error=>error.browserFailure==='deadline');assert.equal(late.commands.length,1);
  const normal=conditionFixture([true],{advance:54999});await normal.run();assert.equal(normal.commands.length,1);
});
