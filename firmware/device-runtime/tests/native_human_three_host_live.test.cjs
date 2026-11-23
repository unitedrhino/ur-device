// 文件说明：纯Human三宿主驱动的离线合同；不启动C++、浏览器、后端或模型，不冒称实际媒体通过。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const driver = require('./native_human_three_host_live.cjs');
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {EventEmitter}=require('node:events');
const proof = (stage) => ({stage,sameOriginal:true,currentOwners:true,wholeFocus:true,
  sourceReleased:stage==='committed',sourceRetained:stage!=='committed',remoteRetained:true,
  targetActual:stage!=='aborted',targetGateOpen:stage==='committed',imCommitted:stage==='committed',
  targetReleased:stage==='aborted',unknown:false});
test('缺显式真实入口在触碰资源前拒绝', async () => {
  await assert.rejects(driver.run({}, null), /explicit_live_opt_in_required/);
});
test('ready必须目标完整actual但业务门关闭，源和远端保持', () => {
  assert.equal(driver.verifyTransferProof(proof('ready'),'ready'),true);
  for (const key of ['sameOriginal','currentOwners','wholeFocus','sourceRetained','remoteRetained','targetActual'])
    assert.throws(()=>driver.verifyTransferProof({...proof('ready'),[key]:false},'ready'));
  for (const key of ['sourceReleased','targetGateOpen','imCommitted','targetReleased','unknown'])
    assert.throws(()=>driver.verifyTransferProof({...proof('ready'),[key]:true},'ready'));
});
test('commit不可用单独信令冒充源release与IM提交', () => {
  assert.equal(driver.verifyTransferProof(proof('committed'),'committed'),true);
  for(const key of ['sourceReleased','remoteRetained','targetActual','targetGateOpen','imCommitted'])
    assert.throws(()=>driver.verifyTransferProof({...proof('committed'),[key]:false},'committed'));
  assert.throws(()=>driver.verifyTransferProof({...proof('committed'),extra:'privacy-sentinel'},'committed'));
});
test('abort必须目标实际释放，原两腿不被恢复或关闭', () => {
  assert.equal(driver.verifyTransferProof(proof('aborted'),'aborted'),true);
  assert.throws(()=>driver.verifyTransferProof({...proof('aborted'),targetReleased:false},'aborted'));
  assert.throws(()=>driver.verifyTransferProof({...proof('aborted'),targetGateOpen:true},'aborted'));
});
test('脱敏报告不保存身份、自由异常或私有正文', () => {
  const report=driver.projectReport({phase:'privacy-sentinel',error:'secret',passed:true,rounds:2,
    devicesClosed:false,browsersClosed:true});
  assert.equal(report.passed,false);
  assert.equal(report.phase,'unknown');
  assert.equal(JSON.stringify(report).includes('sentinel'),false);
  assert.equal(JSON.stringify(report).includes('secret'),false);
});
test('固定产物与三个不同设备私有输入在资源前核验，非法形状不得启栈', t => {
  const parent=path.resolve(__dirname,'../../../.temp/device-runtime');
  fs.mkdirSync(parent,{recursive:true});
  const dir=fs.mkdtempSync(path.join(parent,'ur-human-three-'));
  t.after(()=>fs.rmSync(dir,{recursive:true,force:true}));
  const binary=path.join(dir,'device'),audio=path.join(dir,'audio.wav'),input=path.join(dir,'input.json');
  fs.writeFileSync(binary,'component-not-executable');
  const wav=Buffer.alloc(44);wav.write('RIFF');wav.write('WAVE',8);fs.writeFileSync(audio,wav);
  const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  const data={source:{productID:'product',deviceName:'source',secret:'private-sentinel'},
    remote:{productID:'product',deviceName:'remote',secret:'private-sentinel'},
    target:{productID:'product',deviceName:'target',secret:'private-sentinel'},ports:[19001,19002,19003]};
  const env={UR_NATIVE_HUMAN_THREE_HOST_LIVE:'1',AGENT_BROWSER_HEADED:'1',DISPLAY:':test',XAUTHORITY:path.join(dir,'authority'),
    UR_SHARED_HANDOFF_BIN:binary,UR_SHARED_HANDOFF_BIN_SHA256:hash(binary),UR_NATIVE_HUMAN_THREE_HOST_INPUT:input,
    UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:'18830',UR_SHARED_HANDOFF_AUDIO_FIXTURE:audio,UR_SHARED_HANDOFF_AUDIO_SHA256:hash(audio)};
  const write=value=>{fs.writeFileSync(input,JSON.stringify(value),{mode:0o600});fs.chmodSync(input,0o600);};
  write(data);assert.equal(driver.validateEnvironment(env).binary,binary);
  for(const invalid of [{...data,target:data.source},{...data,ports:[19001,19001,19003]},
    {...data,extra:'privacy-sentinel'},{...data,target:{...data.target,deviceName:'wrong/identity'}}]) {
    write(invalid);assert.throws(()=>driver.validateEnvironment(env));
  }
  write(data);fs.chmodSync(input,0o644);assert.throws(()=>driver.validateEnvironment(env));
  fs.chmodSync(input,0o600);
  assert.throws(()=>driver.validateEnvironment({...env,UR_SHARED_HANDOFF_BIN_SHA256:'0'.repeat(64)}));
  fs.appendFileSync(audio,'changed');assert.throws(()=>driver.validateEnvironment(env));
});

// scheduler仅模拟底层进程/页面/时钟；同一runOwned业务步骤仍实际执行，结果强制offline而非真实通过。
function scheduler(failure='') {
  let now=1000,pid=100,attempt=0,pending=null,clockBack=false;
  const roles=['source','remote','target'],children=[],sessions=new Map(),events=[],released=[],closedBrowsers=[],launchArgs=[],protocolAtObservation=[];
  const config={binary:'/fixture/device',audio:'/fixture/voice.wav',data:{ports:[19001,19002,19003]}};
  for(const role of roles)config.data[role]={productID:'p',deviceName:role,secret:'not-real'};
  const fail=point=>{events.push(point);if(failure===point)throw Error('fixture_failure');};
  const latest=role=>children.filter(child=>child.role===role).at(-1);
  const finish=child=>{if(child.finished){events.push('duplicateExit:'+child.role);return;}child.finished=true;
    child.stdout.emit('end');child.stderr.emit('end');child.emit('close',0,null);events.push('exit:'+child.role+':'+child.pid);};
  const sample=child=>{
    child.pageSamples=(child.pageSamples||0)+1;
    child.frames+=3;if(child.active)child.sound+=2;
    if(failure==='candidateLeak'&&child.candidate)child.sound++;
    return {ready:failure!=='pageReady:'+child.role&&failure!=='terminated:'+child.role&&!(failure==='pageReadyOnce:'+child.role&&child.pageSamples<3),bridgeOpen:true,audioRunning:failure!=='pageReady:'+child.role,terminated:failure==='terminated:'+child.role,
      activationFailure:{reason:'none',stage:'unknown',category:'none'},completed:0,canEscalate:!child.sessionID&&failure!=='dialNotReady'&&failure!=='protocolNotReady:'+child.role&&!(failure==='protocolReadyOnce:'+child.role&&child.pageSamples<3),canAccept:pending?.target===child.role&&failure!=='inviteNotReady',
      released:child.finished,endpoint:{canPrepare:child.active,canCommit:pending?.source===child.role},
      media:{session:child.sessionID,peerEpoch:String(child.pid),peerFingerprint:'a'.repeat(64),connected:!!child.sessionID,
        frames:child.frames,nonSilentFrames:child.sound,inputAttached:!!child.sessionID,
        outputEnabled:!!child.sessionID,held:false}};
  };
  const ports={now:()=>now,wall:()=>100000+now,sleep:async ms=>{now+=ms;if(failure==='clockRollback'&&!clockBack){now-=1000;clockBack=true;}},
    budget:async work=>work(),captureChild:held=>{fail('capture:'+held.child.role);held.record={pid:held.child.pid};},
    releaseChild:async held=>{released.push(held.child.pid);finish(held.child);if(failure==='releaseFailure')throw Error('fixture_cleanup_failure');},
    spawn(binary,args,options){
      const role=options.env.UR_DEVICE_NAME;fail('boot:'+role);
      assert.equal(binary,config.binary);assert.deepEqual(args,[String(config.data.ports[roles.indexOf(role)]),'--human']);
      assert.equal(options.env.UR_DEVICE_HUMAN_ENDPOINT_SOURCE,'1');
      assert.ok(!latest(role)||latest(role).finished,'restarted_live_handle');
      const child=new EventEmitter();child.role=role;child.pid=++pid;child.frames=20;child.sound=0;child.sessionID='';
      child.stdout=new EventEmitter();child.stderr=new EventEmitter();child.stderr.resume=()=>{};children.push(child);
      queueMicrotask(()=>{if(failure==='spawnError:'+role){child.emit('error',Error('privacy-sentinel'));finish(child);return;}
        child.emit('spawn');child.stdout.emit('data',Buffer.from('http://127.0.0.1:'+args[0]+'/\n'));});return child;
    },
    execFile(command,args,options,callback){
      try {
        assert.equal(command,'agent-browser');
        const session=args[1],action=args[2]==='close'?'close':args[3],selector=args[4],value=args[5];
        if(action==='open') {const port=Number(new URL(selector).port),role=roles[config.data.ports.indexOf(port)];sessions.set(session,latest(role));fail('open:'+role);
          if(children.filter(value=>value.role===role).length>1)fail('openAgain:'+role);}
        const child=sessions.get(session);assert.ok(child);
        if(action==='close'){closedBrowsers.push(child.pid);if(failure==='browserCloseFailure')throw Error('fixture_cleanup_failure');callback(null,'');return;}
        launchArgs.push(options.env.AGENT_BROWSER_ARGS);
        if(action==='eval') {
          if(selector.includes('function installPageObservation'))protocolAtObservation.push({role:child.role,pid:child.pid,ready:failure!=='protocolReadyOnce:'+child.role||child.pageSamples>=3});
          if(selector.includes('function prepareBrowserClick')){const vm=require('node:vm');
            const button={disabled:false,getBoundingClientRect(){return {left:0,right:100,top:child.outsideControls?900:10,bottom:child.outsideControls?930:40,width:100,height:30};},
              scrollIntoView(){child.outsideControls=false;},contains(){return false;},click(){assert.fail('must-not-dom-click');}};
            const facts=vm.runInNewContext(selector,{innerWidth:800,innerHeight:600,document:{querySelector:()=>button,elementFromPoint:()=>button}});callback(null,JSON.stringify(facts));return;
          }
          callback(null,selector.startsWith('(async()=>')?JSON.stringify(sample(child)):'{}');return;}
        assert.ok(!child.finished,'using_closed_handle');
        if(action==='click'&&selector==='#activate')fail('activate:'+child.role);
        if(action==='fill'){child[selector]=value;if(failure==='offscreenControls')child.outsideControls=true;}
        if(action==='click'&&child.outsideControls){callback(null,'{}');return;} // CLI可成功返回但离屏坐标没有派发原app handler。
        if(action==='click'&&selector==='#handoffEscalate') {
          if(failure==='protocolReadyOnce:remote')assert.ok(latest('remote').pageSamples>=3,'remote_protocol_not_ready_before_dial');
          fail('dial');pending={source:child.role,target:'remote',initial:true};
        }
        if(action==='click'&&selector==='#endpointSourcePrepare') {
          attempt++;fail('prepare:'+attempt);
          pending={source:child.role,target:child['#endpointSourceTarget'].split(':').at(-1)};
        }
        if(action==='click'&&selector==='#handoffAccept') {
          fail('accept:'+child.role);assert.equal(pending.target,child.role);
          child.sessionID='session-'+child.role+'-'+child.pid;child.candidate=!pending.initial;
          if(child.candidate&&failure==='initialCandidateLeak')child.sound=5;
          if(pending.initial){child.active=true;const source=latest(pending.source);source.active=true;source.sessionID='session-source-'+source.pid;pending=null;}
        }
        if(action==='click'&&selector==='#endpointSourceAbort') {
          fail('abort:'+attempt);const target=latest(pending.target);finish(target);
        }
        if(action==='click'&&selector==='#endpointSourceCommit') {
          fail('commit:'+attempt);const target=latest(pending.target);target.active=true;target.candidate=false;
          child.active=false;finish(child);if(failure==='duplicateExit')child.emit('close',0,null);
        }
        if(action==='click'&&selector==='#handoffHangup') {
          fail('hangup');finish(child);finish(latest('remote'));
        }
        callback(null,'{}');
      } catch(error){callback(error);}
    }};
  const observer={observationWindow:{createdAfter:100001,createdBefore:280001},async readTransfer(request){
    fail(request.stage+':'+request.attempt);
    assert.equal(request.source,'p:'+pending.source);assert.equal(request.target,'p:'+pending.target);
    assert.equal(request.sourceSessionId,latest(pending.source).sessionID);
    assert.equal(request.targetSessionId,latest(pending.target).sessionID);
    if(failure==='unknownProof')return {...proof(request.stage),unknown:true};
    if(failure==='earlyGate')return {...proof(request.stage),targetGateOpen:true};
    if(failure==='timeout')return null;
    return proof(request.stage);
  }};
  return {config,ports,observer,children,events,released,closedBrowsers,launchArgs,protocolAtObservation,clock:()=>now};
}
test('所有纯Human初启及候选重启须先等原能力ACK，桥和音频ready不能让同伴提前拨号',async t=>{
  for(const role of ['source','remote','target'])await t.test(role,async()=>{
    const f=scheduler('protocolReadyOnce:'+role);let result,error;
    try{result=await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.equal(error,undefined,'应等待原Human可用事实，不得在被叫尚未协议就绪时点击拨号');
    assert.equal(result.fixturePassed,true);assert.equal(result.actualCPP,false);
    const observations=f.protocolAtObservation.filter(value=>value.role===role);assert.ok(observations.length>0);
    assert.ok(observations.every(value=>value.ready),'页面观察只能装在原ACK之后，重启不能复用旧进程就绪');
    assert.deepEqual(f.released,f.children.map(child=>child.pid));
  });
});
test('任一Human未获原能力ACK在原总预算内超时，不拨号且清理全部已建角色',async t=>{
  for(const role of ['source','remote','target'])await t.test(role,async()=>{
    const f=scheduler('protocolNotReady:'+role);let error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.phase,'boot');assert.equal(error.facts.boot.role,role);
    assert.equal(error.facts.boot.step,'pageReady');assert.equal(error.facts.failureCode,'phase_timeout');
    assert.equal(error.facts.lastPageFacts[role].ready,true);assert.equal(error.facts.lastPageFacts[role].canEscalate,false);
    assert.equal(f.events.includes('dial'),false);assert.equal(error.facts.cleanup.devicesClosed,true);assert.equal(error.facts.cleanup.browsersClosed,true);
    assert.deepEqual(f.released,f.children.map(child=>child.pid));
  });
});
test('同run业务Abort后重启T、S退出后重启且callee成为第二源，始终不冒实际C++', async()=>{
  const f=scheduler();let result;
  try{result=await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(error){assert.fail(JSON.stringify({phase:error.facts?.phase,cause:error.cause?.message,events:f.events}));}
  assert.equal(result.fixturePassed,true);assert.equal(result.passed,false);assert.equal(result.actualCPP,false);
  assert.equal(result.evidence,'offline');assert.equal(result.readyTargetInputObserved,true);
  assert.deepEqual(f.children.map(child=>child.role),['source','remote','target','target','source']);
  assert.equal(new Set(f.children.map(child=>child.pid)).size,5);
  assert.deepEqual(f.events.filter(event=>/^(prepare|abort|commit):/.test(event)),['prepare:1','abort:1','prepare:2','commit:2','prepare:3','commit:3']);
  assert.deepEqual(f.released,f.children.map(child=>child.pid));
  assert.equal(new Set(f.closedBrowsers).size,5);
  assert.ok(f.events.indexOf('exit:source:101')<f.events.lastIndexOf('boot:source'));
  assert.ok(f.events.indexOf('exit:target:103')<f.events.lastIndexOf('boot:target'));
});
test('每阶段故障只清本轮已创建资源，未知proof/时钟回退/超时/候选漏流不冒通过',async t=>{
  for(const point of ['boot:source','boot:remote','boot:target','dial','accept:remote','prepare:1','ready:1',
    'abort:1','prepare:2','commit:2','prepare:3','commit:3','hangup','unknownProof','earlyGate','timeout','clockRollback','candidateLeak','initialCandidateLeak','duplicateExit'])
    await t.test(point,async()=>{
      const f=scheduler(point);let error;
      try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
      assert.ok(error,'fixture_should_fail');assert.equal(error.facts?.passed,false);assert.equal(error.facts?.actualCPP,false);
      assert.deepEqual(f.released,f.children.map(child=>child.pid));
      assert.equal(new Set(f.released).size,f.children.length);
      assert.ok(f.children.every(child=>child.finished));
      assert.ok(f.closedBrowsers.every(pid=>f.children.some(child=>child.pid===pid)));
    });
});
test('清理失败不能因业务已完成转为通过或遗漏其他原进程',async t=>{
  for(const point of ['browserCloseFailure','releaseFailure'])await t.test(point,async()=>{
    const f=scheduler(point);let error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.fixturePassed,false);assert.equal(error.facts.passed,false);
    assert.equal(error.facts.cleanup[point==='releaseFailure'?'devicesClosed':'browsersClosed'],false);
    assert.deepEqual(f.released,f.children.map(child=>child.pid));
  });
});
test('boot每角色启动throw或卡ready都保留闭集步骤及实际已创建/退出数量',async t=>{
  for(const role of ['source','remote','target'])for(const kind of ['boot','pageReady'])await t.test(kind+':'+role,async()=>{
    const f=scheduler(kind+':'+role);let error;try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.boot.role,role);assert.equal(error.facts.boot.step,kind==='boot'?'spawn':'pageReady');
    assert.equal(error.facts.boot.failureCode,kind==='boot'?'unknown':'phase_timeout');
    assert.equal(error.facts.deviceProcesses.createdCount,f.children.length);assert.equal(error.facts.deviceProcesses.spawnedCount,f.children.length);
    assert.equal(error.facts.deviceProcesses.closedCount,f.children.length);assert.equal(error.facts.deviceProcesses.stdoutEndedCount,f.children.length);assert.equal(error.facts.deviceProcesses.stderrEndedCount,f.children.length);
    if(kind==='pageReady')assert.deepEqual(error.facts.boot.page,{ready:false,bridgeOpen:true,audioRunning:false,terminated:false,activationFailure:{reason:'none',stage:'unknown',category:'none'}});
  });
});
test('boot投影拒绝自由分类/角色/页面正文，缺actual计数不得补零称成功',()=>{
  const value=driver.projectReport({boot:{role:'privacy-sentinel',step:'private-url',failureCode:'private-error',page:{ready:true,bridgeOpen:true,audioRunning:true,terminated:false,status:'private-text',activationFailure:{reason:'private'}}}});
  assert.equal(value.boot.role,'unknown');assert.equal(value.boot.step,'unknown');assert.equal(value.boot.failureCode,'unknown');assert.equal(value.boot.page.activationFailure,null);
  assert.deepEqual(value.deviceProcesses,{createdCount:null,spawnedCount:null,closedCount:null,stdoutEndedCount:null,stderrEndedCount:null});
  assert.equal(JSON.stringify(value).includes('private'),false);assert.equal(JSON.stringify(value).includes('sentinel'),false);
});
test('每角色capture/open/activate异常均保步骤，spawn确认不与capture记录混淆',async t=>{
  for(const role of ['source','remote','target'])for(const kind of ['capture','open','activate'])await t.test(kind+':'+role,async()=>{
    const f=scheduler(kind+':'+role);let error;try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.boot.role,role);assert.equal(error.facts.boot.step,kind==='open'?'openBrowser':kind);
    assert.equal(error.facts.boot.failureCode,kind==='capture'?'unknown':'browser_command_failed');assert.equal(error.facts.deviceProcesses.spawnedCount,f.children.length);
    assert.equal(error.facts.deviceProcesses.closedCount,f.children.length);assert.equal(error.facts.deviceProcesses.stdoutEndedCount,f.children.length);assert.equal(error.facts.deviceProcesses.stderrEndedCount,f.children.length);
  });
});
test('原child创建后spawn失败独立计数，真实close不冒充spawn成功',async t=>{
  for(const role of ['source','remote','target'])await t.test(role,async()=>{const f=scheduler('spawnError:'+role);let error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}assert.ok(error);
    assert.equal(error.facts.boot.role,role);assert.equal(error.facts.boot.step,'spawn');assert.equal(error.facts.boot.failureCode,'child_spawn_failed');
    assert.equal(error.facts.deviceProcesses.createdCount,f.children.length);assert.equal(error.facts.deviceProcesses.spawnedCount,f.children.length-1);
    assert.equal(error.facts.deviceProcesses.closedCount,f.children.length);assert.equal(JSON.stringify(error.facts).includes('privacy-sentinel'),false);
  });
});
test('Abort后T重启和首次commit后S重启失败仍保启动分类，不借外层phase丢失首因',async t=>{
  for(const role of ['target','source'])await t.test(role,async()=>{const f=scheduler('openAgain:'+role);let error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}assert.ok(error);
    assert.equal(error.facts.boot.role,role);assert.equal(error.facts.boot.step,'openBrowser');assert.equal(error.facts.boot.failureCode,'browser_command_failed');
    assert.equal(error.facts.deviceProcesses.closedCount,f.children.length);
  });
});
test('真实driver传给execFile的Chromium参数按本机CLI逗号/换行规则拆成五个独立flag',async()=>{
  const f=scheduler();await driver.runOfflineFixture(f.config,f.observer,f.ports);assert.ok(f.launchArgs.length>0);
  for(const value of f.launchArgs)assert.deepEqual(value.split(/[,\n]/),['--no-sandbox','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream',
    '--use-file-for-fake-audio-capture=/fixture/voice.wav','--autoplay-policy=no-user-gesture-required']);
});
test('boot已终止页面冻结事实后立即拒，普通暂未ready仍按原预算等待恢复',async()=>{
  const failed=scheduler('terminated:source');let error;try{await driver.runOfflineFixture(failed.config,failed.observer,failed.ports);}catch(value){error=value;}
  assert.ok(error);assert.equal(error.facts.boot.failureCode,'page_activation_terminated');assert.equal(error.facts.boot.page.terminated,true);assert.ok(failed.clock()<2000);
  assert.equal(error.facts.cleanup.devicesClosed,true);assert.equal(error.facts.cleanup.browsersClosed,true);
  const delayed=scheduler('pageReadyOnce:source'),result=await driver.runOfflineFixture(delayed.config,delayed.observer,delayed.ports);
  assert.equal(result.fixturePassed,true);assert.ok(delayed.clock()>1000);
});
test('全流程失败冻结原role/attempt/步骤和页布尔，cleanup不得补采或覆盖业务事实',async t=>{
  for(const [point,role,attempt,step]of [['dial','source',0,'dialSend'],['ready:1','source',1,'readyProof'],['abort:1','source',1,'abortSend'],
    ['commit:2','source',2,'commitSend'],['prepare:3','target',3,'prepareSend'],['hangup','source',3,'hangup']])await t.test(point,async()=>{
    const f=scheduler(point);let before,error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports,{onBeforeCleanup:value=>{before=structuredClone(value);assert.ok(f.children.some(child=>!child.finished));}});}catch(value){error=value;}
    assert.ok(error);assert.deepEqual(error.facts.lastOperation,{role,attempt,step});assert.equal(error.facts.failureCode,step==='readyProof'?'unknown':'browser_command_failed');
    assert.deepEqual(error.facts.lastPageFacts,before.lastPageFacts);assert.deepEqual(error.facts.lastOperation,before.lastOperation);
    assert.equal(typeof error.facts.lastPageFacts.source.canEscalate,'boolean');assert.equal(error.facts.cleanup.devicesClosed,true);
  });
});
test('全流程投影只保闭集操作和页事实，未知或缺样本不补成功且无ID/自由正文',()=>{
  const result=driver.projectReport({lastOperation:{role:'private-role',attempt:999,step:'private-url'},lastPageFacts:{source:{ready:true,status:'private-body',
    canEscalate:false,canAccept:true,endpoint:{canPrepare:true,sessionId:'private-id'},media:{connected:false,session:'private-session'}}},failureCode:'private-error'});
  assert.deepEqual(result.lastOperation,{role:'unknown',attempt:null,step:'unknown'});assert.equal(result.lastPageFacts.remote,null);
  assert.equal(result.lastPageFacts.source.ready,true);assert.equal(result.lastPageFacts.source.audioRunning,null);
  assert.equal(JSON.stringify(result).includes('private'),false);
});
test('cleanup前诊断回调抛错不跳过任何释放、不覆盖原首因，缺页仍null',async()=>{
  for(const point of ['','dial','boot:source']){const f=scheduler(point);let error;
    try{await driver.runOfflineFixture(f.config,f.observer,f.ports,{onBeforeCleanup(){throw Error('private-diagnostic-error');}});}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.diagnosticFailed,true);assert.equal(error.facts.fixturePassed,false);
    assert.equal(error.facts.failureCode,point==='dial'?'browser_command_failed':'unknown');
    if(point==='boot:source')assert.deepEqual(error.facts.lastPageFacts,{source:null,remote:null,target:null});
    assert.deepEqual(f.released,f.children.map(child=>child.pid));assert.equal(error.facts.cleanup.devicesClosed,true);assert.equal(error.facts.cleanup.browsersClosed,true);
    assert.equal(JSON.stringify(error.facts).includes('private'),false);
  }
});
test('三个页面ready不代替source协议就绪或remote可接听，固定步骤明确区分两种等待',async()=>{
  for(const [point,role,step,flag]of [['dialNotReady','source','boot','canEscalate'],['inviteNotReady','remote','inviteReady','canAccept']]){
    const f=scheduler(point);let error;try{await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(value){error=value;}
    assert.ok(error);assert.equal(error.facts.failureCode,'phase_timeout');assert.deepEqual(error.facts.lastOperation,{role,attempt:0,step});
    assert.equal(error.facts.lastPageFacts[role].ready,true);assert.equal(error.facts.lastPageFacts[role][flag],false);assert.equal(error.facts.cleanup.devicesClosed,true);
  }
});
test('诊断端口意外返回拒绝Promise也不得阻塞清理或产生未处理自由异常',async()=>{
  const f=scheduler('dial');let error;try{await driver.runOfflineFixture(f.config,f.observer,f.ports,{async onBeforeCleanup(){throw Error('private-async-error');}});}catch(value){error=value;}
  assert.ok(error);assert.equal(error.facts.diagnosticFailed,true);assert.equal(error.facts.failureCode,'browser_command_failed');
  assert.deepEqual(f.released,f.children.map(child=>child.pid));assert.equal(JSON.stringify(error.facts).includes('private'),false);
});
test('原真实driver对离屏坐标先执行原geometry准备再CLI点击，不能把CLI成功当dial已派发',async()=>{
  const f=scheduler('offscreenControls');let result;try{result=await driver.runOfflineFixture(f.config,f.observer,f.ports);}catch(error){assert.fail(JSON.stringify({phase:error.facts?.phase,lastOperation:error.facts?.lastOperation}));}
  assert.equal(result.fixturePassed,true);assert.ok(f.events.includes('dial'));assert.equal(result.completedTransfers,2);
  assert.ok(result.clickObservations.some(value=>value.role==='source'&&value.step==='dialSend'&&value.scrolled===true&&value.hit===true));
  assert.ok(result.clickObservations.length<=32);assert.equal(JSON.stringify(result.clickObservations).includes('selector'),false);
});
