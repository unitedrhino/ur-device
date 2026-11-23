// 文件说明：纯Human接续只读观察器的离线合同；SQL/Redis与Runtime输入均为明确fixture，不注册实网适配。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const observer=require('./native_human_transfer_observer.cjs');
const scope={tenantCode:'tenant',source:'device:p:source',remote:'device:p:remote',target:'device:p:target',createdAfter:100000,createdBefore:280000};
const request={stage:'ready',attempt:1,source:'p:source',target:'p:target',sourceSessionId:'source-session',targetSessionId:'candidate-session',createdAfter:100000,createdBefore:280000};
const key='x'.repeat(32);
// witness只表示当前固定sample的fixture，不代表真实Runtime拥有许可。
function witness(kind='snapshot',sample=1,overrides={}) {
  return {join:observer.join(key,scope.tenantCode,request.sourceSessionId,request.targetSessionId),kind,sample,observedAt:101000,validUntil:102000,
    stage:'ready',sameOriginal:true,currentOwners:true,wholeFocus:true,sourceReleased:false,sourceRetained:true,remoteRetained:true,
    targetActual:true,targetGateOpen:false,imCommitted:false,targetReleased:false,unknown:false,...overrides};
}
test('owner与大整数原文无损，非owner不安全数不能默默舍入',()=>{
  assert.equal(observer.parsePrivate('{"ownerUserId":9223372036854775807}').ownerUserId,'9223372036854775807');
  assert.throws(()=>observer.parsePrivate('{"revision":9007199254740992}'));
  assert.throws(()=>observer.parsePrivate('{"ownerUserId":1,"ownerUserId":2}'));
});
test('Redis数组hash保持Go HTML和U2028转义，不排序对象替代',()=>{
  const encoded='["a\\u003cb\\u003e\\u0026\\u2028","z"]';
  assert.equal(observer.goHash('a<b>&\u2028','z'),crypto.createHash('sha256').update(encoded).digest('hex'));
});
test('只读查询固定本轮来源/时间且ID转text，不允许注入或自由查询',()=>{
  const sql=observer.query(scope,request);
  assert.match(sql,/BEGIN READ ONLY/);assert.match(sql,/statement_timeout='2000ms'/);
  assert.match(sql,/owner_user_id::text/);assert.match(sql,/LIMIT 2/);assert.match(sql,/ROLLBACK/);
  assert.throws(()=>observer.query(scope,{...request,sourceSessionId:"bad' OR true"}));
  assert.throws(()=>observer.query(scope,{...request,target:'p:remote'}));
});
test('最新unknown拒回捞旧ready，更高snapshot可恢复；receipt不作为当前许可',()=>{
  const c=observer.createWitnessStore();c.push(JSON.stringify(witness('receipt',1)));
  assert.equal(c.current(witness().join,'ready',101100),null);
  c.push(JSON.stringify(witness('snapshot',2)));
  assert.ok(c.current(witness().join,'ready',101100));
  c.push(JSON.stringify(witness('snapshot',3,{stage:'unknown',unknown:true,currentOwners:false})));
  assert.equal(c.current(witness().join,'ready',101100),null);
  c.push(JSON.stringify(witness('snapshot',4)));
  assert.ok(c.current(witness().join,'ready',101100));
});
test('低sample迟到不覆盖，同sample异字节永久拒，过期及错join拒',()=>{
  const c=observer.createWitnessStore();c.push(JSON.stringify(witness('receipt',1)));c.push(JSON.stringify(witness('snapshot',3)));
  c.push(JSON.stringify(witness('snapshot',2,{stage:'unknown',unknown:true})));
  assert.ok(c.current(witness().join,'ready',101100));
  assert.equal(c.current('0'.repeat(64),'ready',101100),null);
  assert.equal(c.current(witness().join,'ready',102000),null);
  c.push(JSON.stringify(witness('snapshot',3,{targetGateOpen:true})));
  assert.equal(c.current(witness().join,'ready',101100),null);
  c.push(JSON.stringify(witness('snapshot',4)));
  assert.equal(c.current(witness().join,'ready',101100),null);
});

// fixture只构造原Go存储形状；所有读取均留在内存，不向真实入口注册SQL/Redis或假Runtime。
function fixture(input=request,stage='ready',callID='original-call') {
  // fixture用十进制文本构造身份，再按Go int64字段输出数字token；被测parser不能靠字符串fixture绕过真实数值形状。
  const wire=value=>JSON.stringify(value).replace(/"(ownerUserId|creatorUserId|originalCallerUserId|originalCalleeUserId|conversationId)":"([0-9]+)"/g,'"$1":$2');
  const ref=(subject,sessionId,index)=>({kind:'native',sessionId,subject,ownerUserId:'1',connectionEpoch:index,
    connectionInstance:String(index).repeat(32),focusId:'focus-'+sessionId,deadline:250000,ownerNode:'node',ownerEpoch:1,connectionOwnerEpoch:1});
  const origin={callId:callID,handoffId:'handoff-'+input.attempt,generation:input.targetSessionId,prepareToken:'prepare-'+input.attempt,
    source:ref('device:'+input.source,input.sourceSessionId,input.attempt===3?4:1),retained:ref(scope.remote,'remote-session',2),
    target:ref('device:'+input.target,input.targetSessionId,input.attempt+2),callRevision:input.attempt===3?4:input.attempt,endpointRevision:(input.attempt-1)*3+1,startedAt:100500,operationDeadline:130500,callDeadline:250000};
  const leg=ref=>({subject:ref.subject,sessionId:ref.sessionId,ownerUserId:ref.ownerUserId,connectionEpoch:ref.connectionEpoch,
    connectionInstance:ref.connectionInstance,endpointRevision:0});
  const creation={callId:origin.callId,tenantCode:scope.tenantCode,creatorUserId:'1',creatorSubject:scope.source,
    originalCallerSubject:scope.source,originalCallerUserId:'1',originalCalleeSubject:scope.remote,originalCalleeUserId:'1',conversationId:'0',createdAt:100};
  const sourceLeg={...leg(origin.source),endpointRevision:origin.endpointRevision-1};
  const prior={creation,caller:input.attempt===3?leg(origin.retained):sourceLeg,callee:input.attempt===3?sourceLeg:leg(origin.retained),callDeadline:250000,version:input.attempt};
  const state={handoffId:origin.handoffId,state:stage,revision:origin.endpointRevision+(stage==='ready'?1:2),
    ...(stage==='aborted'?{reason:'userCancelled'}:{candidateSessionId:origin.generation})};
  const encode=value=>Buffer.from(JSON.stringify(value)).toString('base64');
  const envelope=(method,ref,data,eventSeq)=>({msgToken:'event-'+method,method,sessionId:ref.sessionId,...(eventSeq?{eventSeq}:{}),data});
  const event=(method,ref,data,seq)=>({subject:ref.subject,payload:encode(envelope(method,ref,data,seq))});
  const prepare={msgToken:origin.prepareToken,method:'endpointState',sessionId:origin.source.sessionId,code:200,
    data:{state:'preparing',revision:origin.endpointRevision,handoffId:origin.handoffId}};
  const terminalToken=stage==='aborted'?'abort-token':'commit-token';
  const reply={msgToken:terminalToken,method:'endpointState',sessionId:origin.source.sessionId,code:200,data:state};
  const events=stage==='aborted'?
    [event('endpointState',origin.source,state,5),event('endpointState',origin.target,state,3),event('sessionEnded',origin.target,{state:'ended',reason:'userCancelled'},4)]:
    [event('endpointState',origin.source,state,5),event('sessionEnded',origin.source,{state:'ended',reason:'transferred'},6),
      event('callState',origin.retained,{state:'active',revision:origin.callRevision+1,requestToken:origin.prepareToken},5),
      event('sessionEstablished',origin.target,{state:'established',mediaRevision:1},3),event('endpointState',origin.target,state,4)];
  const prepared=JSON.stringify({reply:encode(reply),events});
  const current=structuredClone(prior);
  if(stage==='committed'){current.caller=leg(origin.retained);current.callee={...leg(origin.target),endpointRevision:origin.endpointRevision+2};current.version++;}
  if(stage==='aborted'){(input.attempt===3?current.callee:current.caller).endpointRevision=origin.endpointRevision+2;current.version++;}
  const redisEvents=events.map(event=>({Subject:event.subject,Payload:event.payload}));
  const digest=crypto.createHash('sha256').update(wire({state:current,events:redisEvents})).digest('hex');
  const prepareFixed={fingerprint:'e'.repeat(64),payload:encode(prepare)},terminalFixed={fingerprint:'f'.repeat(64),payload:encode(reply)};
  const endpoint={handoffId:origin.handoffId,sourceSessionId:origin.source.sessionId,sourceEpoch:origin.source.connectionEpoch,retainedEpoch:origin.retained.connectionEpoch,targetEpoch:origin.target.connectionEpoch,
    sourceInstance:origin.source.connectionInstance,retainedInstance:origin.retained.connectionInstance,targetInstance:origin.target.connectionInstance,
    startedAt:origin.startedAt,expiresAt:origin.operationDeadline,state,replies:{[observer.goHash(origin.prepareToken)]:prepareFixed,...(stage==='ready'?{}:{[observer.goHash(terminalToken)]:terminalFixed})}};
  const record={fingerprint:'f'.repeat(64),generation:origin.generation,targetKey:'im:call:user:'+origin.target.subject,
    status:stage==='ready'?'reserved':stage==='aborted'?'released':'committed',endpoint,humanState:prior,
    ...(stage==='committed'?{humanCommittedState:current,humanBoundDigest:digest,events:redisEvents}:{}),
    ...(stage==='aborted'?{humanReleasedState:current,humanBoundDigest:digest,events:redisEvents}:{})};
  const originalPolicy={...endpoint,state:{state:'preparing',revision:origin.endpointRevision,handoffId:origin.handoffId},replies:{[observer.goHash(origin.prepareToken)]:prepareFixed}};
  const initialFingerprint=observer.goHash(scope.tenantCode,'1',origin.callId,origin.source.subject,origin.target.subject,String(origin.callRevision-1),origin.prepareToken);
  record.fingerprint=observer.goHash(observer.goHash(initialFingerprint,'humanLegs',wire(prior)),'endpoint',JSON.stringify(originalPolicy));
  const actual=(ref,state)=>({original:{msgToken:'original-'+ref.sessionId,method:'focusControl',sessionId:ref.sessionId,eventSeq:1,
    params:{operation:'acquire',expectedRevision:0,focusId:ref.focusId,resources:['microphone','speaker'],priority:'call',resumePolicy:'never',expiresAt:ref.deadline}},
    actual:{state,focusId:ref.focusId,resources:['microphone','speaker'],revision:1}});
  const refs=[origin.source,origin.retained,origin.target],states=[stage==='committed'?'released':'acquired','acquired',stage==='aborted'?'released':'acquired'];
  const row={id:crypto.createHash('sha256').update(JSON.stringify([scope.tenantCode,1,origin.source.sessionId,origin.handoffId])).digest('hex'),tenantCode:scope.tenantCode,subject:origin.source.subject,ownerUserId:'1',sourceSessionId:origin.source.sessionId,
    callId:origin.callId,handoffId:origin.handoffId,generation:origin.generation,ownerNode:'node',ownerEpoch:1,revision:3,
    state:stage==='ready'?'targetReady':stage,scope:wire(origin),activeKey:stage==='ready'?observer.goHash(scope.tenantCode,origin.callId):null,retainUntil:stage==='ready'?0:800000,sourceReleaseRecorded:stage==='committed',
    operationDeadline:origin.operationDeadline,callDeadline:origin.callDeadline,commitToken:stage==='committed'?terminalToken:'',commitFingerprint:stage==='committed'?'c'.repeat(64):'',
    preparedResult:stage==='committed'?prepared:null,result:stage==='committed'?JSON.stringify({imCommitDigest:digest,preparedDigest:crypto.createHash('sha256').update(prepared).digest('hex')}):
      stage==='aborted'?JSON.stringify({token:terminalToken,fingerprint:'b'.repeat(64),targetReady:true,reply:encode(reply),events,imDigest:digest}):null,
    sessions:refs.map((ref,index)=>({id:ref.sessionId,subject:ref.subject,purpose:'humanCall',state:states[index]==='released'?'ended':'established',ownerNode:'node',ownerEpoch:1,leaseUntil:110000,deadline:250000,resourceId:'nativeFocus:'+ref.focusId,
      snapshot:wire({nativeFocus:{sessionId:ref.sessionId,subject:ref.subject,callId:origin.callId,ownerUserId:'1',connectionEpoch:ref.connectionEpoch,connectionInstance:ref.connectionInstance,focusId:ref.focusId,deadline:250000},closing:states[index]==='released'})})),
    connections:refs.map((ref,index)=>({subject:ref.subject,connectionEpoch:ref.connectionEpoch,state:'ready',ownerNode:'node',ownerEpoch:1,leaseUntil:110000,
      snapshot:wire({resourceRevision:1,baseline:{ownerUserId:'1',resources:['microphone','speaker']},leases:{[ref.focusId]:actual(ref,states[index])}})}))};
  const redis=[ [origin.callId,'connected','voice',current.caller.subject,current.callee.subject,String(stage==='committed'?origin.callRevision+1:origin.callRevision),wire(creation),wire(current)],
    stage==='committed'?null:origin.callId,origin.callId,stage==='ready'?origin.generation:stage==='committed'?origin.callId:null,
    [wire(record),stage==='ready'?origin.generation:null,stage==='ready'?record.targetKey:null],
    [wire(record),JSON.stringify(prepareFixed),...(stage==='ready'?[]:[JSON.stringify(terminalFixed)])]];
  const fact={...witness(),join:observer.join(key,scope.tenantCode,input.sourceSessionId,input.targetSessionId),stage,
    sourceReleased:stage==='committed',sourceRetained:stage!=='committed',targetActual:stage!=='aborted',targetGateOpen:stage==='committed',
    imCommitted:stage==='committed',targetReleased:stage==='aborted'};
  return {origin,row,redis,fact};
}
// harness仅把原模块生成的查询送进内存fixture，并核查第二次Redis读取确实使用完全相同的固定批次。
function harness(data=fixture()) {
  let sqlCalls=0,redisCalls=0,lastBatch=null,now=101100;
  const value={data,witnesses:null,changeSecondRedis:false};
  value.port=observer.createObserver(scope,{traceKey:key,now:()=>now,
    readSql:async statement=>{sqlCalls++;assert.match(statement,/BEGIN READ ONLY/);return JSON.stringify([value.data.row]);},
    readRedis:async batch=>{redisCalls++;if(redisCalls%2===1)lastBatch=JSON.stringify(batch);else assert.equal(JSON.stringify(batch),lastBatch);
      const reply=structuredClone(value.data.redis);if(value.changeSecondRedis&&redisCalls%2===0)reply[0][5]='999';if(value.expireSecondRedis&&redisCalls%2===0)now=102000;return reply;},
    readWitness:async input=>{assert.equal(input.join,value.data.fact.join);return value.witnesses||[JSON.stringify({...value.data.fact,kind:'receipt',sample:1}),JSON.stringify({...value.data.fact,kind:'snapshot',sample:2})];}});
  value.counts=()=>({sqlCalls,redisCalls});value.setNow=value=>{now=value;};return value;
}
test('实际组装的ready需SQL三原锚+Redis原具体两腿+独立候选+新鲜Runtime，缺一不可',async()=>{
  const f=harness(),result=await f.port.readTransfer(request);
  assert.deepEqual(result,{stage:'ready',...Object.fromEntries(Object.keys(witness()).filter(key=>!['join','kind','sample','observedAt','validUntil','stage'].includes(key)).map(key=>[key,witness()[key]]))});
  assert.deepEqual(f.counts(),{sqlCalls:1,redisCalls:2});
});
test('当前snapshot缺失/unknown/过期/错误HMAC返回null，receipt不能代替当前许可',async()=>{
  for(const rows of [[witness('receipt',1)],[witness('receipt',1),witness('snapshot',2,{unknown:true,stage:'unknown'})],
    [witness('receipt',1),witness('snapshot',2,{validUntil:101050})]]){
    const f=harness();f.witnesses=rows.map(JSON.stringify);assert.equal(await f.port.readTransfer(request),null);
  }
});
test('错HMAC与坏JSON永久拒绝，不能下一次用正确marker擦除；最新unknown可正常恢复',async()=>{
  for(const bad of [JSON.stringify(witness('snapshot',2,{join:'0'.repeat(64)})),'{"join":']){
    const f=harness();f.witnesses=[bad];await assert.rejects(f.port.readTransfer(request),/native_human_observation_invalid/);
    f.witnesses=null;await assert.rejects(f.port.readTransfer(request),/native_human_observation_invalid/);
  }
  const f=harness();assert.ok(await f.port.readTransfer(request));
  f.witnesses=[JSON.stringify(witness('snapshot',3,{stage:'unknown',unknown:true,currentOwners:false}))];
  assert.equal(await f.port.readTransfer(request),null);
  f.witnesses=[JSON.stringify(witness('snapshot',4))];assert.ok(await f.port.readTransfer(request));
});
test('SQL/Redis每个来源字段单因素篡改拒绝，不把历史同owner当当前原腿',async t=>{
  const changes={rowOwner:f=>{f.row.ownerUserId='2';},scopeOwner:f=>{const o=JSON.parse(f.row.scope);o.source.ownerUserId='2';f.row.scope=JSON.stringify(o);},
    originalInstance:f=>{const o=JSON.parse(f.row.scope);o.target.connectionInstance='f'.repeat(32);f.row.scope=JSON.stringify(o);},
    deadline:f=>{f.row.callDeadline++;},sqlState:f=>{f.row.state='bogus';},sourceReleased:f=>{f.row.sourceReleaseRecorded=true;},
    rowID:f=>{f.row.id='0'.repeat(64);},activeKey:f=>{f.row.activeKey='0'.repeat(64);},
    missingAnchor:f=>{f.row.sessions.pop();},anchorScope:f=>{const s=JSON.parse(f.row.sessions[0].snapshot);s.nativeFocus.callId='other';f.row.sessions[0].snapshot=JSON.stringify(s);},
    ownerFence:f=>{f.row.connections[0].ownerEpoch++;},leaseExpiry:f=>{f.row.sessions[0].leaseUntil=101000;},
    actualShape:f=>{const s=JSON.parse(f.row.connections[2].snapshot);s.leases[f.origin.target.focusId].actual.resources=['speaker'];f.row.connections[2].snapshot=JSON.stringify(s);},
    pending:f=>{const s=JSON.parse(f.row.connections[0].snapshot);s.pending={};f.row.connections[0].snapshot=JSON.stringify(s);},
    actualRevision:f=>{const s=JSON.parse(f.row.connections[0].snapshot);s.resourceRevision=0;f.row.connections[0].snapshot=JSON.stringify(s);},
    remoteBusy:f=>{f.redis[2]='other';},targetBusy:f=>{f.redis[3]='other';},callState:f=>{f.redis[0][1]='ended';},
    callerRole:f=>{f.redis[0][3]=scope.target;},legInstance:f=>{const l=JSON.parse(f.redis[0][7]);l.caller.connectionInstance='f'.repeat(32);f.redis[0][7]=JSON.stringify(l);},
    generation:f=>{const r=JSON.parse(f.redis[4][0]);r.generation='other';f.redis[4][0]=JSON.stringify(r);},
    endpointIndex:f=>{const e=JSON.parse(f.redis[5][0]);e.targetEpoch++;f.redis[5][0]=JSON.stringify(e);},
    firstFingerprint:f=>{const r=JSON.parse(f.redis[4][0]);r.fingerprint='0'.repeat(64);f.redis[4][0]=f.redis[5][0]=JSON.stringify(r);},
    prepareReply:f=>{const e=JSON.parse(f.redis[5][1]);e.payload=Buffer.from('{}').toString('base64');f.redis[5][1]=JSON.stringify(e);}};
  for(const [name,change] of Object.entries(changes))await t.test(name,async()=>{const data=fixture();change(data);const f=harness(data);await assert.rejects(f.port.readTransfer(request),/native_human_observation_invalid/);});
});
test('原Scope在首ready后不可替换，Redis读取期间改变不能拼接成通过',async()=>{
  const f=harness();assert.ok(await f.port.readTransfer(request));
  const origin=JSON.parse(f.data.row.scope);origin.prepareToken='another';f.data.row.scope=JSON.stringify(origin);
  await assert.rejects(f.port.readTransfer(request),/native_human_observation_invalid/);
  const changing=harness();changing.changeSecondRedis=true;await assert.rejects(changing.port.readTransfer(request),/native_human_observation_invalid/);
});
test('同一原来源ready后才可核commit/abort，摘要取原Prepared UTF8字节且事件payload严格base64',async()=>{
  for(const stage of ['committed','aborted']){
    const f=harness();await f.port.readTransfer(request);f.data=fixture(request,stage);
    f.witnesses=[JSON.stringify({...f.data.fact,kind:'receipt',sample:3}),JSON.stringify({...f.data.fact,kind:'snapshot',sample:4})];
    const result=await f.port.readTransfer({...request,stage});assert.equal(result.stage,stage);assert.equal(result.unknown,false);
  }
  for(const mutate of [f=>{f.row.preparedResult+=' ';},f=>{const p=JSON.parse(f.row.preparedResult);p.events[0].payload+='=';f.row.preparedResult=JSON.stringify(p);},
    f=>{const result=JSON.parse(f.row.result);result.imCommitDigest='b'.repeat(64);f.row.result=JSON.stringify(result);},
    f=>{const p=JSON.parse(f.row.preparedResult);[p.events[0],p.events[1]]=[p.events[1],p.events[0]];f.row.preparedResult=JSON.stringify(p);}]){
    const f=harness();await f.port.readTransfer(request);f.data=fixture(request,'committed');mutate(f.data);
    await assert.rejects(f.port.readTransfer({...request,stage:'committed'}),/native_human_observation_invalid/);
  }
});
test('Runtime所有布尔单因素与metadata畸形拒绝，重复相同帧幂等且不耗唯一容量',async t=>{
  const facts=witness();
  for(const key of Object.keys(facts).filter(key=>typeof facts[key]==='boolean'))await t.test(key,()=>{
    const c=observer.createWitnessStore();c.push(JSON.stringify(witness('receipt',1)));c.push(JSON.stringify(witness('snapshot',2,{[key]:!facts[key]})));
    assert.equal(c.current(facts.join,'ready',101100),null);
  });
  for(const overrides of [{sample:0},{sample:513},{sample:1.5},{kind:'anything'},{stage:'preparing'},
    {observedAt:0},{validUntil:103001},{validUntil:100999},{extra:'privacy-sentinel'}])await t.test(JSON.stringify(overrides),()=>{
    const c=observer.createWitnessStore();c.push(JSON.stringify(witness('receipt',1)));c.push(JSON.stringify(witness('snapshot',2,overrides)));
    assert.equal(c.current(facts.join,'ready',101100),null);
    c.push(JSON.stringify(witness('snapshot',3)));assert.equal(c.current(facts.join,'ready',101100),null);
  });
  const c=observer.createWitnessStore(),raw=JSON.stringify(witness('snapshot',2));c.push(JSON.stringify(witness('receipt',1)));
  for(let i=0;i<5000;i++)c.push(raw);assert.ok(c.current(facts.join,'ready',101100));
  c.push(JSON.stringify(witness('snapshot',3,{observedAt:100999,validUntil:101999})));
  assert.equal(c.current(facts.join,'ready',101100),null);
});
test('合法在途SQL阶段返回null，不把prepared/sourceReleased猜为committed',async()=>{
  for(const state of ['preparing','sourceReleasing','sourceReleased','commitUnknown','aborting','unknown']){
    const f=harness();f.data.row.state=state;assert.equal(await f.port.readTransfer(request),null);
  }
});
test('正式顺序完成Abort与首次提交后才允许callee作为新源，原创建历史不改',async()=>{
  const f=harness();await f.port.readTransfer(request);
  f.data=fixture(request,'aborted');f.witnesses=[JSON.stringify({...f.data.fact,kind:'receipt',sample:3}),JSON.stringify({...f.data.fact,kind:'snapshot',sample:4})];
  assert.equal((await f.port.readTransfer({...request,stage:'aborted'})).stage,'aborted');
  const second={...request,attempt:2,targetSessionId:'second-target'};
  f.data=fixture(second);f.witnesses=null;assert.equal((await f.port.readTransfer(second)).stage,'ready');
  f.data=fixture(second,'committed');f.witnesses=[JSON.stringify({...f.data.fact,kind:'receipt',sample:3}),JSON.stringify({...f.data.fact,kind:'snapshot',sample:4})];
  assert.equal((await f.port.readTransfer({...second,stage:'committed'})).stage,'committed');
  const third={...request,attempt:3,source:'p:target',target:'p:source',sourceSessionId:'second-target',targetSessionId:'new-source'};
  f.data=fixture(third);f.witnesses=null;assert.equal((await f.port.readTransfer(third)).stage,'ready');
  const skipped=harness(fixture(third));await assert.rejects(skipped.port.readTransfer(third),/native_human_observation_invalid/);
});
test('无损owner原文参与SQL与Redis键，不能数字舍入碰撞',()=>{
  const data=fixture(),large='9223372036854775807',origin=observer.parsePrivate(data.row.scope);origin.source.ownerUserId=large;
  const batch=observer.redisBatch(scope,origin,data.row);
  assert.equal(batch[5].key,'im:call:endpoint-handoff:'+observer.goHash(scope.tenantCode,large,origin.source.subject,origin.source.sessionId));
  assert.notEqual(batch[5].key,'im:call:endpoint-handoff:'+observer.goHash(scope.tenantCode,String(Number(large)),origin.source.subject,origin.source.sessionId));
});
test('读取故障不透传私有异常；同observer并发拒绝且不破坏原在途读取',async()=>{
  const data=fixture();let unlock,entered;
  const reached=new Promise(resolve=>{entered=resolve;}),gate=new Promise(resolve=>{unlock=resolve;});
  const port=observer.createObserver(scope,{traceKey:key,now:()=>101100,readSql:async()=>{entered();await gate;return JSON.stringify([data.row]);},readRedis:async()=>data.redis,
    readWitness:async()=>[JSON.stringify(witness('receipt',1)),JSON.stringify(witness('snapshot',2))]});
  const pending=port.readTransfer(request);await reached;await assert.rejects(port.readTransfer(request),error=>error.message==='native_human_observation_invalid');unlock();assert.ok(await pending);
  const failed=observer.createObserver(scope,{traceKey:key,now:()=>101100,readSql:async()=>{throw Error('private-token-sentinel');},readRedis:async()=>[],readWitness:async()=>[]});
  await assert.rejects(failed.readTransfer(request),error=>error.message==='native_human_observation_invalid'&&!JSON.stringify(error).includes('private-token-sentinel'));
});
test('最后Redis读回期间短票到期不能沿早先snapshot返回当前许可',async()=>{
  const f=harness();f.expireSecondRedis=true;assert.equal(await f.port.readTransfer(request),null);
});
test('后继attempt不能把同设备同会话号的新Call伪作原通话连续换端',async()=>{
  const f=harness();await f.port.readTransfer(request);f.data=fixture(request,'aborted');
  f.witnesses=[JSON.stringify({...f.data.fact,kind:'receipt',sample:3}),JSON.stringify({...f.data.fact,kind:'snapshot',sample:4})];
  await f.port.readTransfer({...request,stage:'aborted'});
  const second={...request,attempt:2,targetSessionId:'second-target'};
  f.data=fixture(second,'ready','other-call');f.witnesses=null;
  await assert.rejects(f.port.readTransfer(second),/native_human_observation_invalid/);
});
test('构造后替换外部密钥/来源不能改变本轮固定只读关联',async()=>{
  const data=fixture(),input={...scope};
  const transport={traceKey:key,now:()=>101100,readSql:async()=>JSON.stringify([data.row]),readRedis:async()=>data.redis,
    readWitness:async expected=>{assert.equal(expected.join,witness().join);return [JSON.stringify(witness('receipt',1)),JSON.stringify(witness('snapshot',2))];}};
  const port=observer.createObserver(input,transport);input.source='device:p:other';transport.traceKey='z'.repeat(32);
  transport.readSql=async()=>{throw Error('must-not-call-replaced-port');};
  assert.equal((await port.readTransfer(request)).stage,'ready');
});
