// 文件说明：桌面独立线程取证器的隐私、实例与生命周期回归；不调用真实模型或改变设备状态。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
// load 每次从正式模块读取，不以测试内替身代替被测观察器。
const load = () => require('./thread_progress_observer.cjs');
// fact 构造固定诊断合同，计数故意非事务一致以验证旁观者不伪造严格顺序。
function fact() {
  const names = ['callbacksEntered','callbacksLocked','callbacksExited','rejected','enqueueOverflow',
    'mqttPollEntered','mqttPollExited','takeEntered','takeExited','loopsEntered','loopsExited',
    'runtimeEntered','runtimeExited','diagnosticAttempts','diagnosticSent','diagnosticFailed',
    'messageValid','messageEnqueued','messageTaken','signalingValid','signalingEnqueued','signalingTaken',
    'otherValid','otherEnqueued','otherTaken'];
  return {kind:'threadProgress',version:1,nonTransactional:true,callbackStage:'entered',hostStage:'runtime_poll',
    counters:Object.fromEntries(names.map((name,index)=>[name,index])),overflow:false};
}
// fixture 仅在内存模拟原回环读取，不保存令牌或向设备注入业务消息。
function fixture(extra = {}) {
  const calls=[], emitted=[];
  const options={env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port:27897,originalAlive:()=>true,
    emit:value=>emitted.push(value),request:async(port,path,token)=>{
      calls.push({port,path,token}); return path==='/bootstrap' ? JSON.stringify({token:'a'.repeat(64)}) : JSON.stringify(fact());
    },...extra};
  return {...load().create(options),calls,emitted};
}
test('严格接受固定25项无事务顺序的诊断；返回独立投影',()=>{
  const raw=fact(), result=load().project(JSON.stringify(raw)); assert.deepEqual(result,raw);
  result.counters.callbacksEntered=999; assert.equal(raw.counters.callbacksEntered,0);
  for (const stage of ['unknown','idle','shutdown','stopped']) {
    assert.ok(load().project(JSON.stringify({...raw,hostStage:stage})));
  }
});
test('正文身份额外字段、重复键、错枚举和越界均整项拒绝',()=>{
  const raw=fact();
  for (const value of [{...raw,privateIdentity:'secret'}, {...raw,version:2}, {...raw,nonTransactional:false},
    {...raw,callbackStage:'payload'}, {...raw,counters:{...raw.counters,messageTaken:-1}},
    {...raw,counters:{...raw.counters,messageTaken:4294967296}},
    {...raw,counters:{...raw.counters,messageTaken:'1'}},
    {...raw,counters:{...raw.counters,privateText:'secret'}}]) assert.equal(load().project(JSON.stringify(value)),null);
  assert.equal(load().project(JSON.stringify(raw).replace('"version":1','"version":1,"version":1')),null);
  assert.equal(load().project(JSON.stringify(raw).replace('"messageTaken":18','"messageTaken":18,"messageTaken":18')),null);
  assert.equal(load().project('x'.repeat(8193)),null);
});
test('默认关闭和非精确1不读取HTTP、不查询实例、不安装资源',async()=>{
  for (const env of [{},{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'true'}]) {
    const observer=fixture({env,originalAlive:()=>{throw Error('must not query');}});
    await observer.sample(); await observer.close(); assert.deepEqual(observer.calls,[]); assert.deepEqual(observer.emitted,[]);
    assert.equal(observer.statistics().enabled,false);
  }
});
test('只读bootstrap及固定GET投影，不输出原令牌和地址',async()=>{
  const observer=fixture(); await observer.sample(); await observer.sample(); await observer.close();
  assert.equal(observer.calls.length,3); assert.deepEqual(observer.calls.map(v=>v.path),['/bootstrap','/diagnostics/progress','/diagnostics/progress']);
  assert.equal(observer.calls[0].token,null); assert.equal(observer.calls[1].token,'a'.repeat(64));
  assert.equal(observer.emitted.length,2); assert.equal(observer.emitted[0].sampled,true);
  const json=JSON.stringify(observer.emitted); assert.ok(!json.includes('a'.repeat(64))); assert.ok(!json.includes('27897'));
});
test('原实例退出或读取期间被换代时只报未知，不沿用上次状态',async()=>{
  let alive=true;
  const observer=fixture({originalAlive:()=>alive}); await observer.sample(); alive=false; await observer.sample();
  assert.equal(observer.emitted[0].sampled,true); assert.deepEqual(observer.emitted[1],{diagnostic:'threadProgress',sample:2,sampled:false,
    observation:{stage:'identity_before',reason:'identity_unconfirmed',httpStatus:null,bodyBytes:null,
      headersObserved:null,endObserved:null,bodyComplete:null}});
  const replaced=fixture({originalAlive:()=>alive,request:async()=>{alive=false; return JSON.stringify({token:'a'.repeat(64)});}});
  alive=true; await replaced.sample(); assert.equal(replaced.emitted[0].sampled,false); assert.equal(replaced.statistics().sampled,0);
});
test('bootstrap额外字段、错误及畸形读取不保留旧采样；sink失败不污染业务',async()=>{
  for(const body of ['{}',JSON.stringify({token:'a'.repeat(64),privateText:'secret'}),'not json']) {
    const observer=fixture({request:async()=>body}); await observer.sample(); assert.equal(observer.emitted[0].sampled,false);
    assert.equal(observer.statistics().sampled,0);
  }
  const observer=fixture({request:async()=>{throw Error('private detail');},emit:async()=>{throw Error('private sink');}});
  await assert.doesNotReject(observer.sample()); await observer.close(); assert.equal(observer.statistics().sinkFailures,1);
  assert.ok(!JSON.stringify(observer.statistics()).includes('private'));
});
test('并发采样不叠加请求，close中止并等待原请求，不发后继HTTP',async()=>{
  let release, calls=0;
  const observer=fixture({request:async(port,path,token,signal)=>{
    calls++; return new Promise(resolve=>{release=()=>resolve('{}'); signal.addEventListener('abort',release,{once:true});});
  }});
  const first=observer.sample(); await observer.sample(); assert.equal(calls,1);
  await observer.close(); await first; assert.equal(observer.statistics().active,false); assert.equal(observer.emitted.length,0);
  await observer.sample(); assert.equal(calls,1);
});
test('96次预算耗尽不再读取或隐式重试；无效端口提前拒绝',async()=>{
  const observer=fixture(); for(let i=0;i<100;i++) await observer.sample();
  assert.equal(observer.emitted.length,96); assert.equal(observer.calls.length,97); assert.equal(observer.statistics().capacityReached,true);
  for(const port of [0,65536,'27897',null]) assert.throws(()=>fixture({port}),/thread_progress_port_invalid/);
});
// serve 使用独立内核分配回环端口，并在测试结束等待server真实close，不接管任务业务监听。
async function serve(t,handler) {
  const http=require('node:http'), server=http.createServer(handler);
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
  t.after(()=>new Promise(resolve=>{server.close(resolve);server.closeAllConnections();}));
  return server.address().port;
}
test('真实HTTP只发送同源GET及原Bearer，完整解析固定诊断',async t=>{
  const calls=[];
  const port=await serve(t,(request,response)=>{
    calls.push({method:request.method,path:request.url,headers:request.headers});
    response.writeHead(200,{'Content-Type':'application/json'});
    response.end(request.url==='/bootstrap'?JSON.stringify({token:'a'.repeat(64)}):JSON.stringify(fact()));
  });
  const emitted=[], observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
    originalAlive:()=>true,emit:value=>emitted.push(value)});
  await observer.sample();await observer.close(); assert.equal(emitted[0].sampled,true);
  assert.equal(calls.length,2);
  for(const call of calls){assert.equal(call.method,'GET');assert.equal(call.headers.host,'127.0.0.1:'+port);
    assert.equal(call.headers.origin,'http://127.0.0.1:'+port);assert.equal(call.headers['sec-fetch-site'],'same-origin');}
  assert.equal(calls[0].headers.authorization,undefined);assert.equal(calls[1].headers.authorization,'Bearer '+'a'.repeat(64));
});
test('真实HTTP拒绝跳转、错误类型、超限正文和非法UTF8',async t=>{
  let mode=0, count=0;
  const port=await serve(t,(request,response)=>{
    count++;
    if(mode===0){response.writeHead(302,{Location:'/do-not-follow'});response.end();}
    if(mode===1){response.writeHead(200,{'Content-Type':'text/plain'});response.end('{}');}
    if(mode===2){response.writeHead(200,{'Content-Type':'application/json'});response.end('x'.repeat(8193));}
    if(mode===3){response.writeHead(200,{'Content-Type':'application/json'});response.end(Buffer.from([0xff]));}
  });
  for(mode=0;mode<4;mode++) {
    const observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
      originalAlive:()=>true,emit:value=>assert.equal(value.sampled,false)});
    await observer.sample();await observer.close();
  }
  assert.equal(count,4,'任何跳转或失败都不追加隐藏HTTP重试');
});
test('真实在途HTTP被close取消，诊断完成后不残留连接',async t=>{
  let started; const accepted=new Promise(resolve=>{started=resolve;});
  const port=await serve(t,()=>started());
  const observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
    originalAlive:()=>true,emit:()=>assert.fail('关闭后不再发采样')});
  const sample=observer.sample();await accepted;await observer.close();await sample;
  assert.equal(observer.statistics().active,false);
});
test('原实例未知时sink同步重入也不能产生第二个采样',async()=>{
  let observer, emitted=0;
  observer=fixture({originalAlive:()=>false,emit:()=>{if(++emitted===1)observer.sample();}});
  await observer.sample();await observer.close();
  assert.equal(observer.statistics().attempted,1);assert.equal(emitted,1);
});
test('异步sink等待close不能与原采样循环互等，缺失sink完成明确计数',async()=>{
  let observer, completed=false;
  observer=fixture({emit:async()=>{await observer.close();completed=true;}});
  const sampling=observer.sample();
  let timer;
  const settled=await Promise.race([sampling.then(()=>true),new Promise(resolve=>{timer=setTimeout(()=>resolve(false),100);})]);
  clearTimeout(timer);assert.equal(settled,true,'sink不能让原观察器自等待');
  await new Promise(resolve=>setImmediate(resolve));assert.equal(completed,true);
  assert.equal(observer.statistics().pendingSinks,0);
});
test('缺失分类：未发GET和读取后实例未知仅报告identity_unconfirmed',async()=>{
  const absent=fixture({originalAlive:()=>false});await absent.sample();await absent.close();
  assert.equal(absent.calls.length,0);
  assert.deepEqual(absent.emitted[0].observation,{stage:'identity_before',reason:'identity_unconfirmed',
    httpStatus:null,bodyBytes:null,headersObserved:null,endObserved:null,bodyComplete:null});
  let checks=0;
  const replaced=fixture({originalAlive:()=>++checks<3});await replaced.sample();await replaced.close();
  assert.equal(replaced.calls.length,2);assert.equal(replaced.emitted[0].sampled,false);
  assert.equal(replaced.emitted[0].observation.stage,'identity_after');
  assert.equal(replaced.emitted[0].observation.reason,'identity_unconfirmed');
  const failed=fixture({originalAlive:()=>{throw Error('private fingerprint detail');}});
  await failed.sample();assert.equal(failed.emitted[0].observation.reason,'identity_unconfirmed');
  assert.ok(!JSON.stringify(failed.emitted).includes('private'));
  let middleChecks=0;
  const middle=fixture({originalAlive:()=>++middleChecks===1});await middle.sample();await middle.close();
  assert.equal(middle.calls.length,1);assert.equal(middle.emitted[0].observation.stage,'identity_after_bootstrap');
  assert.equal(middle.emitted[0].observation.reason,'identity_unconfirmed');
});
test('缺失分类：真实HTTP状态类型超限UTF8只输出有界固定事实',async t=>{
  let mode=0,count=0;
  const port=await serve(t,(request,response)=>{
    count++;
    if(mode===0){response.writeHead(403,{'Content-Type':'application/json'});response.end('private body');}
    if(mode===1){response.writeHead(200,{'Content-Type':'text/plain'});response.end('private body');}
    if(mode===2){response.writeHead(200,{'Content-Type':'application/json'});response.end('x'.repeat(9000));}
    if(mode===3){response.writeHead(200,{'Content-Type':'application/json'});response.end(Buffer.from([0xff]));}
  });
  const expected=['http_status','content_type','oversized','invalid_utf8'];
  for(mode=0;mode<4;mode++) {
    const emitted=[],observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
      originalAlive:()=>true,emit:value=>emitted.push(value)});
    await observer.sample();await observer.close();
    const value=emitted[0].observation;
    assert.equal(value.stage,'bootstrap');assert.equal(value.reason,expected[mode]);
    assert.equal(value.httpStatus,mode===0?403:200);assert.equal(value.headersObserved,true);
    if(mode<2){assert.equal(value.bodyBytes,0);assert.equal(value.endObserved,false);assert.equal(value.bodyComplete,false);}
    if(mode===2){assert.equal(value.bodyBytes,8193);assert.equal(value.bodyComplete,false);}
    if(mode===3){assert.equal(value.bodyBytes,1);assert.equal(value.endObserved,true);assert.equal(value.bodyComplete,true);}
    const json=JSON.stringify(emitted);assert.ok(!json.includes('private'));assert.ok(!json.includes(String(port)));
  }
  assert.equal(count,4);
});
test('缺失分类：真实1500ms截止不追加GET，状态正文未知保留null',async t=>{
  let count=0;const port=await serve(t,()=>{count++;});const emitted=[];
  const observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
    originalAlive:()=>true,emit:value=>emitted.push(value)});
  const start=performance.now();await observer.sample();await observer.close();
  assert.ok(performance.now()-start>=1400);assert.equal(count,1);
  assert.deepEqual(emitted[0].observation,{stage:'bootstrap',reason:'deadline',httpStatus:null,bodyBytes:null,
    headersObserved:false,endObserved:false,bodyComplete:false});
});
test('缺失分类：readHTTP可选metadata失败不改变原字符串合同与隐私',async t=>{
  let count=0;const port=await serve(t,(request,response)=>{count++;response.writeHead(200,{'Content-Type':'application/json'});response.end('{}');});
  const values=[],controller=new AbortController();
  assert.equal(await load().readHTTP(port,'/bootstrap',null,controller.signal,value=>{values.push(value);throw Error('private sink');}),'{}');
  assert.deepEqual(values.at(-1),{reason:'none',httpStatus:200,bodyBytes:2,headersObserved:true,endObserved:true,bodyComplete:true});
  assert.equal(count,1);assert.ok(!JSON.stringify(values).includes('private'));
  assert.equal(await load().readHTTP(port,'/bootstrap',null,controller.signal,async()=>{throw Error('private async sink');}),'{}');
  assert.equal(count,2);await new Promise(resolve=>setImmediate(resolve));
});
test('缺失分类：真实完整正文后projection拒绝不伪造传输失败',async t=>{
  let count=0;
  const port=await serve(t,(request,response)=>{count++;response.writeHead(200,{'Content-Type':'application/json'});
    response.end(request.url==='/bootstrap'?JSON.stringify({token:'a'.repeat(64)}):JSON.stringify({...fact(),privateText:'secret'}));});
  const emitted=[],observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
    originalAlive:()=>true,emit:value=>emitted.push(value)});
  await observer.sample();await observer.close();assert.equal(count,2);
  const value=emitted[0];assert.equal(value.sampled,false);assert.equal(value.observation.stage,'projection');
  assert.equal(value.observation.reason,'shape_rejected');assert.equal(value.observation.httpStatus,200);
  assert.equal(value.observation.endObserved,true);assert.equal(value.observation.bodyComplete,true);
  assert.ok(!JSON.stringify(emitted).includes('secret'));assert.ok(!JSON.stringify(emitted).includes('privateText'));
});
test('缺失分类：真实响应中断保持已观察字段，非HTTP错误文本不透出',async t=>{
  let count=0;
  const port=await serve(t,(request,response)=>{count++;response.writeHead(200,{'Content-Type':'application/json'});
    response.write('{');setImmediate(()=>response.destroy());});
  const emitted=[],observer=load().create({env:{UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS:'1'},port,
    originalAlive:()=>true,emit:value=>emitted.push(value)});
  await observer.sample();await observer.close();assert.equal(count,1);
  const value=emitted[0].observation;assert.equal(value.reason,'transport');assert.equal(value.stage,'bootstrap');
  assert.equal(value.httpStatus,200);assert.equal(value.headersObserved,true);assert.equal(value.bodyBytes,1);
  assert.equal(value.endObserved,false);assert.equal(value.bodyComplete,false);
});
