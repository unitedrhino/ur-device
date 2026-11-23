// 文件说明：实际宿主启动复现模式选择的离线校验；不启动浏览器或假报实际驱动成功。
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {startupModes}=require('./voice_focus_startup_live.cjs');
const {readStartupTiming}=require('./voice_focus_startup_live.cjs');
const vm=require('node:vm');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {createHash}=require('node:crypto');
const {startupTraceRequested,createStartupTrace}=require('./voice_focus_startup_live.cjs');
const {readDeviceStartupFailure,readStartupFailureFacts}=require('./voice_focus_startup_live.cjs');
const {startupConnectFailureRequested,readStartupConnectFailure}=require('./voice_focus_startup_live.cjs');
test('专用连接拒绝必须精确optin，默认关闭且诊断字段不反射自由stdout',()=>{
  assert.equal(startupConnectFailureRequested(undefined),false);assert.equal(startupConnectFailureRequested(''),false);
  assert.equal(startupConnectFailureRequested('1'),true);
  for(const value of ['0','true',1,true,'other'])assert.throws(()=>startupConnectFailureRequested(value));
  assert.deepEqual(readStartupConnectFailure('private-pairing-url\n',''),{failureRecords:0,runtimeFailure:null});
  assert.ok(!JSON.stringify(readStartupConnectFailure('private-pairing-url\n','')).includes('private-pairing'));
});
// connectFailureHarness 运行原sample控制流的离线端口夹具，不创建监听、浏览器或实际设备进程。
async function connectFailureHarness(diagnostic, nativeOutput, overrides={}) {
  const {EventEmitter}=require('node:events'),{PassThrough}=require('node:stream'); // 原进程事件接口，不伪造实际端云成功。
  const calls=[],child=new EventEmitter(),fixtureState={connections:0,capability:null,failed:false};
  let brokerClosed=false,browserClosed=false,spawnEnvironment=null;
  child.stdout=new PassThrough();child.stderr=new PassThrough();child.kill=()=>{throw Error('unexpected_kill');};
  const reservation={listen(port,host,callback){callback();},address(){return {port:39868};},close(callback){callback();}};
  const childProcess={spawn(file,args,options){spawnEnvironment=options.env;queueMicrotask(()=>child.stdout.write('http://127.0.0.1:39868\n'));return child;},
    execFile(file,args,options,callback){calls.push(args);const command=args[args.indexOf('--json')+1];
      if(command==='click'){fixtureState.connections=overrides.connections||0;
        child.stdout.write(nativeOutput);child.stdout.end();child.stderr.end();
        child.emit('exit',overrides.exitCode??1);child.emit('close',overrides.exitCode??1);}
      if(command==='close')browserClosed=true;
      callback(null,JSON.stringify({success:true,data:{result:[]}}),'');}};
  // execFile真实promisify合同返回stdout/stderr对象；夹具不能用普通单值Promise改变原调用形状。
  childProcess.execFile[require('node:util').promisify.custom]=(file,args,options)=>new Promise((resolve,reject)=>
    childProcess.execFile(file,args,options,(error,stdout,stderr)=>error?reject(error):resolve({stdout,stderr})));
  const source=fs.readFileSync(path.join(__dirname,'voice_focus_startup_live.cjs'),'utf8'); // 原维护来源，不改sample函数体作为替代实现。
  const realm={require:name=>name==='node:net'?{createServer:()=>reservation}:name==='node:child_process'?childProcess:
      name.startsWith('./')?require(path.join(__dirname,name)):require(name),module:{exports:{}},__dirname,__filename:path.join(__dirname,'voice_focus_startup_live.cjs'),
    Buffer,console,process:{pid:12345,env:{}},queueMicrotask,setTimeout,clearTimeout,setImmediate,
    fixturePort:{state:fixtureState,port:21889,close:async()=>{brokerClosed=true;}}};
  vm.createContext(realm);vm.runInContext(source,realm);
  vm.runInContext('fixture=async()=>fixturePort;wait=async predicate=>{for(let i=0;i<20;++i){if(predicate())return;await new Promise(setImmediate);}throw Error("fixture_wait_failed");};',realm);
  let result=null,error=null;
  try{result=await vm.runInContext(`sample('default',false,{bytes:0},${JSON.stringify(diagnostic)})`,realm);}catch(caught){error=caught;}
  return {result,error,calls,spawnEnvironment,brokerClosed,browserClosed};
}
test('原sample专用同步connect拒绝先真实配对activate，再要求唯一首因且释放原端口',async()=>{
  const fact={kind:'runtimeFailureDiagnostic',runtime:'mqttConnect',udp:'none',pcm:'none',observation:1};
  const observed=await connectFailureHarness('1','host_failure='+JSON.stringify(fact)+'\n');
  assert.equal(observed.error,null);assert.equal(observed.result.exitCode,1);assert.equal(observed.result.connections,0);
  assert.deepEqual(JSON.parse(JSON.stringify(observed.result.runtimeFailure)),fact);
  assert.equal(observed.result.failureRecords,1);assert.equal(observed.result.childClosed,true);assert.equal(observed.result.stdoutEnded,true);
  assert.deepEqual(JSON.parse(JSON.stringify(observed.result.cleanup)),{childClosed:true,brokerClosed:true,browserClosed:true});
  assert.equal(observed.brokerClosed,true);assert.equal(observed.browserClosed,true);
  assert.equal(observed.spawnEnvironment.UR_DEVICE_MESSAGE_DIAGNOSTICS,'1');
  assert.match(observed.spawnEnvironment.UR_MQTT_CA,/startup-mqtt-connect-reject\.absent\.pem$/);
  assert.deepEqual(observed.calls.map(args=>args[args.indexOf('--json')+1]),['open','snapshot','eval','click','close']);
});
test('原sample诊断off或非法值仍同exit1且无首因，缺失重复错误首因及意外连接严格拒绝',async()=>{
  for(const diagnostic of ['', 'true']){
    const result=await connectFailureHarness(diagnostic,'');assert.equal(result.error,null);
    assert.equal(result.result.runtimeFailure,null);assert.equal(result.result.failureRecords,0);assert.equal(result.result.exitCode,1);
    assert.equal(result.spawnEnvironment.UR_DEVICE_MESSAGE_DIAGNOSTICS,diagnostic);
  }
  const fact='host_failure='+JSON.stringify({kind:'runtimeFailureDiagnostic',runtime:'mqttConnect',udp:'none',pcm:'none',observation:1})+'\n';
  for(const [diagnostic,output,overrides] of [['1','',{}],['1',fact+fact,{}],['1',fact.replace('mqttConnect','mqttPoll'),{}],
    ['',fact,{}],['1',fact,{connections:1}],['1',fact,{exitCode:0}]]){
    const result=await connectFailureHarness(diagnostic,output,overrides);assert.ok(result.error);
    assert.equal(result.brokerClosed,true);assert.equal(result.browserClosed,true);
  }
});
test('设备启动失败只投影正式固定校验枚举，不反射任意stderr或身份',()=>{
  for(const reason of ['invalid_media_transport','invalid_output_focus','invalid_file_catalog','invalid_device_environment','browser_bind_failed'])
    assert.deepEqual(readDeviceStartupFailure(reason+'\n'),{kind:'startupValidationFailed',reason});
  assert.deepEqual(readDeviceStartupFailure('runtime_failed state=4 voice=2\n'),{kind:'runtimeFailed',state:4,voice:2});
  for(const value of [null,'secret','invalid_file_catalog credential=secret','browser_bind_failed_secret',
    'runtime_failed state=65 voice=2','runtime_failed state=4 voice=9007199254740992',
    'x'.repeat(4097)+'\nbrowser_bind_failed'])
    assert.equal(readDeviceStartupFailure(value),null);
  assert.deepEqual(readStartupFailureFacts(),{phase:'start',diagnostic:null});
});
test('缺省完整七条含原文件与新接收，不因选项静默减少范围',()=>{
  assert.deepEqual(startupModes(),['default','focus','unknown','media','mediaUnknown','live','liveUnknown']);
});
test('精确单一路径可复现，非法和重复模式在副作用前拒绝',()=>{
  assert.deepEqual(startupModes('media'),['media']);
  assert.deepEqual(startupModes('live,liveUnknown'),['live','liveUnknown']);
  for(const value of ['media,media','live,','unknown-mode',','])assert.throws(()=>startupModes(value));
});
test('时序诊断只读原页面并精确停止，不切焦点或帧上下文',()=>{
  let stops=0;
  const page={stopFocusStartupTimingObservation:()=>++stops,
    focusStartupTimingFacts:{supported:true,entries:[],truncated:false}};
  page.top=page.self=page;
  const result=vm.runInNewContext('('+readStartupTiming.toString()+')()',
    {globalThis:page,document:{querySelectorAll:name=>{assert.equal(name,'iframe');return {length:0};}}});
  assert.equal(stops,1);
  assert.equal(result.topLevelDocument,true);
  assert.equal(result.iframeCount,0);
  assert.equal(result.timing.supported,true);
});
test('未知时序或停止异常不伪造性能事实，iframe计数有界',()=>{
  const result=vm.runInNewContext('('+readStartupTiming.toString()+')()',
    {globalThis:{top:{},self:{},stopFocusStartupTimingObservation:()=>{throw Error('private');}},
      document:{querySelectorAll:()=>({length:100})}});
  assert.equal(result.topLevelDocument,false);
  assert.equal(result.iframeCount,32);
  assert.equal(result.timing,null);
  assert.ok(!JSON.stringify(result).includes('private'));
});
test('时序读取只投影固定数值，不输出自由属性，超过32条明确截断',()=>{
  const entries=Array.from({length:35},()=>({startOffsetMs:12,durationMs:60,name:'private',attribution:['private']}));
  const result=vm.runInNewContext('('+readStartupTiming.toString()+')()',
    {globalThis:{focusStartupTimingFacts:{supported:true,entries,truncated:false,secret:'private'}},
      document:{querySelectorAll:()=>({length:0})}});
  assert.equal(result.timing.entries.length,32);
  assert.equal(result.timing.truncated,true);
  assert.deepEqual(Object.keys(result.timing).sort(),['entries','supported','truncated']);
  assert.ok(!JSON.stringify(result).includes('private'));
});
test('非法时序数值或getter失败为未知，不能冒认无主线程阻塞',()=>{
  for(const entry of [{startOffsetMs:-1,durationMs:60},{startOffsetMs:0,durationMs:Infinity},null]) {
    const result=vm.runInNewContext('('+readStartupTiming.toString()+')()',
      {globalThis:{focusStartupTimingFacts:{supported:true,entries:[entry],truncated:false}},
        document:{querySelectorAll:()=>({length:0})}});
    assert.equal(result.timing,null);
  }
});

// traceFixture 仅构造临时私有文件和CLI返回夹具，不启动浏览器或以合成trace证明E2E。
function traceFixture(t, overrides={}, mode='live') {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'startup-trace-unit-'));
  t.after(()=>fs.rmSync(directory,{recursive:true,force:true}));
  const calls=[];
  const privatePath=path.join(directory,`voice-focus-startup-trace-${process.pid}-${mode}.json`);
  const bytes=JSON.stringify({traceEvents:[{name:'private',args:{url:'private',credential:'private'}}]});
  const browser=async args=>{
    calls.push(args);
    if(args[1]==='start')return {started:true};
    fs.writeFileSync(privatePath,bytes);
    return {path:privatePath,eventCount:1};
  };
  const trace=createStartupTrace(mode,{requested:true,directory,pid:process.pid,browser,...overrides});
  return {trace,calls,privatePath,bytes,directory};
}

test('trace严格显式准入：必须诊断开启且仅文件或Live原模式，无自由值',()=>{
  assert.equal(startupTraceRequested(undefined,undefined,['default']),false);
  assert.equal(startupTraceRequested('',undefined,['default']),false);
  assert.equal(startupTraceRequested('1','1',['live','liveUnknown']),true);
  assert.equal(startupTraceRequested('1','1',['media','mediaUnknown']),true);
  assert.equal(startupTraceRequested('1','1',['media','live']),true);
  for(const modes of [['default'],['live','focus'],['media','unknown'],[],['other']])
    assert.throws(()=>startupTraceRequested('1','1',modes));
  for(const value of ['0','true','2'])assert.throws(()=>startupTraceRequested(value,'1',['live']));
  assert.throws(()=>startupTraceRequested('1',undefined,['live']),{code:'ERR_STARTUP_TRACE'});
});
test('文件原启动取证只复用同一profiler预算与私有mode文件，不创建Live帧或改变原paint',async t=>{
  for(const mode of ['media','mediaUnknown']) {
    const {trace,calls,privatePath}=traceFixture(t,{},mode);
    assert.equal(await trace.start(),true);assert.equal(await trace.stop(),true);
    assert.equal(trace.facts.complete,true);
    assert.equal(calls[1][2],privatePath);
    assert.ok(privatePath.endsWith('-'+mode+'.json'));
    assert.equal(fs.statSync(privatePath).mode&0o777,0o600);
  }
});
test('默认trace不调用CLI、不创建路径，固定报告不含自由正文',async()=>{
  let calls=0;
  const trace=createStartupTrace('default',{requested:false,browser:()=>++calls});
  await trace.start(); await trace.stop();
  assert.equal(calls,0);
  assert.equal(trace.facts.requested,false);
  assert.equal(trace.facts.privateFileVerified,false);
});
test('原native profiler命令固定categories，实际600文件与计数/hash核验才完成',async t=>{
  const {trace,calls,privatePath,bytes}=traceFixture(t);
  assert.equal(await trace.start(),true);
  assert.equal(fs.statSync(privatePath).mode&0o777,0o600);
  assert.equal(await trace.stop(),true);
  assert.deepEqual(calls,[['profiler','start','--categories','devtools.timeline,renderer.scheduler,toplevel,cc,viz'],['profiler','stop',privatePath]]);
  assert.equal(trace.facts.started,true); assert.equal(trace.facts.stopped,true);
  assert.equal(trace.facts.privateFileVerified,true); assert.equal(trace.facts.complete,true);
  assert.equal(trace.facts.eventCount,1); assert.equal(trace.facts.bytes,Buffer.byteLength(bytes));
  assert.equal(trace.facts.sha256,createHash('sha256').update(bytes).digest('hex'));
  assert.ok(!JSON.stringify(trace.facts).includes('"private"'));
  assert.deepEqual(Object.keys(trace.facts).sort(),['bytes','complete','eventCount','eventsDropped','oversized','privateFileVerified','requested','sha256','startFailed','started','stopFailed','stopped']);
});
test('已存在路径或非法mode/PID在任何CLI前失败且保留原字节',async t=>{
  const {trace,calls,privatePath}=traceFixture(t);
  fs.writeFileSync(privatePath,'retained',{mode:0o600});
  assert.equal(await trace.start(),false); await trace.stop();
  assert.equal(calls.length,0); assert.equal(fs.readFileSync(privatePath,'utf8'),'retained');
  assert.equal(trace.facts.startFailed,true);
  for(const mode of ['default','live/other','../live'])assert.throws(()=>createStartupTrace(mode,{requested:true}));
  assert.throws(()=>createStartupTrace('live',{requested:true,pid:-1}));
});
test('start异常固定失败，stop尽力一次隔离异常，不打印私有错误',async t=>{
  const calls=[];
  const {trace}=traceFixture(t,{browser:async args=>{calls.push(args[1]);throw Error('private_body');}});
  assert.equal(await trace.start(),false);
  assert.equal(await trace.stop(),false);
  assert.deepEqual(calls,['start','stop']);
  assert.equal(trace.facts.startFailed,true); assert.equal(trace.facts.stopFailed,true);
  assert.ok(!JSON.stringify(trace.facts).includes('private_body'));
});
test('start不确认或stop仅命令成功没有原文件，不冒报完成',async t=>{
  for(const startResult of [{started:false},{started:true}]) {
    const {trace}=traceFixture(t,{browser:async args=>args[1]==='start'?startResult:{eventCount:0,path:'other'}});
    assert.equal(await trace.start(),startResult.started);
    assert.equal(await trace.stop(),false);
    assert.equal(trace.facts.privateFileVerified,false); assert.equal(trace.facts.complete,false);
  }
});
test('stop失败不抛出覆盖原业务异常，重复/并发cleanup只有一次命令',async t=>{
  const calls=[];
  const {trace}=traceFixture(t,{browser:async args=>{calls.push(args[1]);if(args[1]==='start')return {started:true};throw Error('private');}});
  await Promise.all([trace.start(),trace.start()]);
  const original=Error('original_business');
  let caught;
  try {throw original;}catch(error){await Promise.all([trace.stop(),trace.stop()]);caught=error;}
  await trace.stop();
  assert.equal(caught,original); assert.deepEqual(calls,['start','stop']);
  assert.equal(trace.facts.stopFailed,true);
});
test('原native输出path/事件数量不符或截断为诊断失败',async t=>{
  for(const result of [{eventCount:2},{eventCount:1,path:'other'},{eventCount:1,warning:'private'}]) {
    let fixture;
    fixture=traceFixture(t,{browser:async args=>{
      if(args[1]==='start')return {started:true};
      fs.writeFileSync(fixture.privatePath,fixture.bytes);
      return {path:fixture.privatePath,...result};
    }});
    await fixture.trace.start(); assert.equal(await fixture.trace.stop(),false);
    assert.equal(fixture.trace.facts.complete,false);
    assert.ok(!JSON.stringify(fixture.trace.facts).includes('"private"'));
  }
});
test('非法JSON、空events、非600或原inode被替换都不冒报文件核验成功',async t=>{
  for(const kind of ['json','empty','mode','inode']) {
    let fixture;
    fixture=traceFixture(t,{browser:async args=>{
      if(args[1]==='start')return {started:true};
      if(kind==='inode'){fs.unlinkSync(fixture.privatePath);fs.writeFileSync(fixture.privatePath,fixture.bytes,{mode:0o600});}
      else fs.writeFileSync(fixture.privatePath,kind==='json'?'not-json':kind==='empty'?'{"traceEvents":[]}':fixture.bytes);
      if(kind==='mode')fs.chmodSync(fixture.privatePath,0o644);
      return {path:fixture.privatePath,eventCount:kind==='empty'?0:1};
    }});
    await fixture.trace.start(); assert.equal(await fixture.trace.stop(),false);
    assert.equal(fixture.trace.facts.privateFileVerified,false);
    assert.equal(fs.existsSync(fixture.privatePath),true);
  }
});
test('总文件超过64MiB保留失败不读取正文、不删除；跨模式共享预算',async t=>{
  let fixture;
  const budget={bytes:0};
  fixture=traceFixture(t,{budget,browser:async args=>{
    if(args[1]==='start')return {started:true};
    fs.truncateSync(fixture.privatePath,64*1024*1024+1);
    return {path:fixture.privatePath,eventCount:1};
  }});
  await fixture.trace.start(); assert.equal(await fixture.trace.stop(),false);
  assert.equal(fixture.trace.facts.oversized,true); assert.equal(fixture.trace.facts.sha256,null);
  assert.equal(fs.statSync(fixture.privatePath).size,64*1024*1024+1);
  const second=traceFixture(t,{budget:{bytes:64*1024*1024}});
  await second.trace.start(); assert.equal(await second.trace.stop(),false);
  assert.equal(second.trace.facts.oversized,true);
});
test('stop在start前为幂等无副作用，不能在stop后又开启新的trace',async t=>{
  const {trace,calls}=traceFixture(t);
  assert.equal(await trace.stop(),false);
  assert.equal(await trace.start(),false);
  assert.equal(calls.length,0);
});
test('成功stop重复/并发仅一次，start未确认即使文件有效仍不冒认完整',async t=>{
  const successful=traceFixture(t);
  await successful.trace.start();
  assert.deepEqual(await Promise.all([successful.trace.stop(),successful.trace.stop()]),[true,true]);
  assert.equal(await successful.trace.stop(),true);
  assert.equal(successful.calls.length,2);
  let failed;
  failed=traceFixture(t,{browser:async args=>{
    if(args[1]==='start')return {started:false};
    fs.writeFileSync(failed.privatePath,failed.bytes);
    return {path:failed.privatePath,eventCount:1};
  }});
  assert.equal(await failed.trace.start(),false);
  assert.equal(await failed.trace.stop(),false);
  assert.equal(failed.trace.facts.privateFileVerified,true);
  assert.equal(failed.trace.facts.started,false); assert.equal(failed.trace.facts.complete,false);
});
test('runner保留原启动断言和具名cleanup，短trace停止先于后续事实/transform复核',()=>{
  const source=fs.readFileSync(require.resolve('./voice_focus_startup_live.cjs'),'utf8');
  const window=source.slice(source.indexOf('if(traceRequested&&!(await trace.start()))'),source.indexOf('phase = `${mode}:facts`;'));
  assert.ok(window.includes("await browser(['click', '#activate'])"));
  assert.ok(window.includes('assert.equal(terminal.code, 1)'));
  assert.ok(window.includes('assert.equal(broker.state.connections, 0)'));
  assert.ok(window.includes('assert.ok(broker.state.capability && !broker.state.failed && !terminal)'));
  assert.ok(window.includes('finally {\n      if(traceRequested)await trace.stop()'));
  assert.ok(source.indexOf('if(traceRequested)await trace.stop(); // 包括start')<source.indexOf('videoTransform:await desktopLive.probeTransform'));
  assert.ok(source.includes('await wait(() => terminal, 7000).finally(() => clearTimeout(timer))'));
  assert.ok(source.includes("await browser(['close'])"));
  assert.ok(!source.includes('bringToFront')&&!source.includes('HeadlessExperimental.beginFrame'));
});
