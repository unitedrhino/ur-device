// 文件说明：纯Human三腿换端的私有只读核验；固定SQL/Redis出处与当前Runtime快照相交，不创建媒体或授权。
// 本模块不连接数据库、不加载自由配置、不执行命令；隔离宿主仅提供已经核实归属的只读传输与原管道裁剪。
'use strict';
const assert=require('node:assert/strict'),crypto=require('node:crypto');
// 业务与运行时字段固定闭集；同sample矛盾永久拒绝，unknown快照则允许后继真实快照恢复。
const booleans=['sameOriginal','currentOwners','wholeFocus','sourceReleased','sourceRetained','remoteRetained',
  'targetActual','targetGateOpen','imCommitted','targetReleased','unknown'];
const witnessKeys=['join','kind','sample','observedAt','validUntil','stage',...booleans];
const ownerKeys=new Set(['ownerUserId','creatorUserId','originalCallerUserId','originalCalleeUserId','conversationId']);
// exact/requireKeys限制私有容器；未知字段不能隐含额外来源。
function exact(value,keys){return value!==null&&typeof value==='object'&&!Array.isArray(value)&&Object.keys(value).length===keys.length&&keys.every(key=>Object.hasOwn(value,key));}
function requireKeys(value,keys){assert.ok(exact(value,keys),'observation_invalid');}
// allowed限定Go omitempty容器的合法可选字段，不从未知扩展猜测新权限。
function allowed(value,required,optional=[]){assert.ok(value&&typeof value==='object'&&!Array.isArray(value)&&required.every(key=>Object.hasOwn(value,key))&&Object.keys(value).every(key=>required.includes(key)||optional.includes(key)),'observation_invalid');}
// parsePrivate仅为本观察器的有界JSON读取保留owner十进制原文，拒绝重复键/非安全修订与深嵌套；不是公共序列化替代品。
function parsePrivate(raw,limit=1048576,topLevel){
  assert.ok(typeof raw==='string'&&Buffer.byteLength(raw)>0&&Buffer.byteLength(raw)<=limit,'observation_invalid');
  let at=0,nodes=0;
  const whitespace=()=>{while(/[\x20\t\r\n]/.test(raw[at]||'x'))at++;};
  const string=()=>{const start=at++;while(at<raw.length){if(raw[at]==='\\'){at+=2;continue;}if(raw[at++]==='"')return JSON.parse(raw.slice(start,at));}throw Error('observation_invalid');};
  const value=(depth=0,key='')=>{
    assert.ok(depth<=24&&++nodes<=30000,'observation_invalid');whitespace();const first=raw[at];
    if(first==='"')return string();
    if(first==='{'){at++;const result=Object.create(null);whitespace();if(raw[at]==='}'){at++;return result;}
      while(true){whitespace();assert.equal(raw[at],'"','observation_invalid');const field=string();assert.ok(!Object.hasOwn(result,field),'observation_invalid');whitespace();assert.equal(raw[at++],':','observation_invalid');whitespace();const start=at;result[field]=value(depth+1,field);if(depth===0&&topLevel)topLevel.set(field,raw.slice(start,at));whitespace();const end=raw[at++];if(end==='}')return result;assert.equal(end,',','observation_invalid');}}
    if(first==='['){at++;const result=[];whitespace();if(raw[at]===']'){at++;return result;}while(true){result.push(value(depth+1));whitespace();const end=raw[at++];if(end===']')return result;assert.equal(end,',','observation_invalid');}}
    for(const [literal,result] of [['true',true],['false',false],['null',null]])if(raw.startsWith(literal,at)){at+=literal.length;return result;}
    const token=/^-?(?:0|[1-9][0-9]*)/.exec(raw.slice(at));assert.ok(token,'observation_invalid');at+=token[0].length;
    if(ownerKeys.has(key)){assert.match(token[0],/^(?:0|[1-9][0-9]*)$/,'observation_invalid');return token[0];}
    const number=Number(token[0]);assert.ok(Number.isSafeInteger(number),'observation_invalid');return number;
  };
  const result=value();whitespace();assert.equal(at,raw.length,'observation_invalid');return result;
}
// goHash严格复用Go JSON字符串数组顺序/HTML及U2028转义，不能用排序对象替代私有账本键。
function goJSON(value){return JSON.stringify(value).replace(/</g,'\\u003c').replace(/>/g,'\\u003e').replace(/&/g,'\\u0026').replace(/\u2028/g,'\\u2028').replace(/\u2029/g,'\\u2029');}
function goHash(...parts){assert.ok(parts.every(part=>typeof part==='string'),'observation_invalid');return crypto.createHash('sha256').update(goJSON(parts)).digest('hex');}
// rowID保留Go []any中owner的原数字字节，不把int64先转Number或转成带引号字符串计算SQL主键。
function rowID(tenant,owner,session,handoff){assert.ok(decimal(owner),'observation_invalid');return crypto.createHash('sha256').update('['+goJSON(tenant)+','+owner+','+goJSON(session)+','+goJSON(handoff)+']').digest('hex');}
// join用与原producer相同的独立域和BE64 UTF8长度，摘要不携带原身份也不授予许可。
function join(key,tenant,source,target){assert.ok(typeof key==='string'&&Buffer.byteLength(key)>=32&&Buffer.byteLength(key)<=256,'observation_invalid');const digest=crypto.createHmac('sha256',key);digest.update('human-handoff-v1\0');for(const field of [tenant,source,target]){assert.ok(typeof field==='string'&&field.length>0,'observation_invalid');const bytes=Buffer.from(field),length=Buffer.alloc(8);length.writeBigUInt64BE(BigInt(bytes.length));digest.update(length);digest.update(bytes);}return digest.digest('hex');}
// createWitnessStore只消费宿主从原日志content精确裁出的17字段JSON，不接自由日志；receipt只记录完成，不提供当前许可。
function createWitnessStore(){
  const joins=new Map();let invalid=false,total=0;
  return {push(raw){
    if(invalid)return;
    try{const record=parsePrivate(raw,4096);requireKeys(record,witnessKeys);
      assert.match(record.join,/^[a-f0-9]{64}$/,'observation_invalid');assert.ok(['snapshot','receipt'].includes(record.kind)&&['ready','aborted','committed','unknown'].includes(record.stage),'observation_invalid');
      assert.ok(Number.isSafeInteger(record.sample)&&record.sample>0&&record.sample<=512&&Number.isSafeInteger(record.observedAt)&&record.observedAt>0&&Number.isSafeInteger(record.validUntil)&&record.validUntil>=record.observedAt&&record.validUntil<=record.observedAt+2000,'observation_invalid');
      for(const key of booleans)assert.equal(typeof record[key],'boolean','observation_invalid');
      if(!record.unknown){assert.ok(record.stage!=='unknown'&&record.sameOriginal&&record.currentOwners&&record.wholeFocus&&record.remoteRetained&&record.validUntil>record.observedAt,'observation_invalid');
        const committed=record.stage==='committed',aborted=record.stage==='aborted';
        for(const [key,expected] of Object.entries({sourceReleased:committed,sourceRetained:!committed,targetActual:!aborted,targetGateOpen:committed,imCommitted:committed,targetReleased:aborted}))assert.equal(record[key],expected,'observation_invalid');}
      let state=joins.get(record.join);if(!state){assert.ok(joins.size<3,'observation_invalid');state={last:0,lastObserved:0,seen:new Map(),receipts:new Map(),snapshot:null};joins.set(record.join,state);}
      if(state.seen.has(record.sample)){assert.equal(state.seen.get(record.sample),raw,'observation_invalid');return;}
      if(record.sample<state.last)return; // 锁外合法迟到：不覆盖已见最新快照或制造授权。
      assert.ok(record.observedAt>=state.lastObserved,'observation_invalid');
      total+=Buffer.byteLength(raw);assert.ok(total<=2097152,'observation_invalid');
      state.seen.set(record.sample,raw);state.last=record.sample;state.lastObserved=record.observedAt;
      if(record.kind==='snapshot')state.snapshot=record;
      else{assert.ok(!record.unknown&&record.stage!=='unknown','observation_invalid');const prior=state.receipts.get(record.stage);assert.ok(!prior,'observation_invalid');state.receipts.set(record.stage,record);}
    }catch{invalid=true;}
  },current(expected,stage,now){
    if(invalid)return null;const state=joins.get(expected),latest=state?.snapshot,receipt=state?.receipts.get(stage);
    if(!latest||!receipt||latest.unknown||latest.stage!==stage||now<latest.observedAt||now>=latest.validUntil||now<receipt.observedAt)return null;
    for(const key of booleans)if(latest[key]!==receipt[key])return null;
    return {...latest};
  }};
}
// validate限制本轮三设备与时间窗，不接受任意角色、企业或期限扩张。
function validate(scope){requireKeys(scope,['tenantCode','source','remote','target','createdAfter','createdBefore']);
  assert.match(scope.tenantCode,/^[A-Za-z0-9_-]{1,50}$/,'observation_invalid');
  for(const key of ['source','remote','target'])assert.match(scope[key],/^device:[A-Za-z0-9_-]{1,64}:[A-Za-z0-9_-]{1,64}$/,'observation_invalid');
  assert.equal(new Set([scope.source,scope.remote,scope.target]).size,3,'observation_invalid');
  assert.ok(Number.isSafeInteger(scope.createdAfter)&&scope.createdAfter>0&&Number.isSafeInteger(scope.createdBefore)&&scope.createdBefore>scope.createdAfter&&scope.createdBefore-scope.createdAfter<=180000,'observation_invalid');}
// validateRequest把attempt与原三角色固定，第二次callee成为新源不会借用历史caller身份。
function validateRequest(scope,request){validate(scope);requireKeys(request,['stage','attempt','source','target','sourceSessionId','targetSessionId','createdAfter','createdBefore']);
  assert.ok(['ready','aborted','committed'].includes(request.stage)&&[1,2,3].includes(request.attempt),'observation_invalid');
  assert.equal(request.source,(request.attempt===3?scope.target:scope.source).slice(7),'observation_invalid');
  assert.equal(request.target,(request.attempt===3?scope.source:scope.target).slice(7),'observation_invalid');
  assert.equal(request.createdAfter,scope.createdAfter,'observation_invalid');assert.equal(request.createdBefore,scope.createdBefore,'observation_invalid');
  for(const key of ['sourceSessionId','targetSessionId'])assert.match(request[key],/^[A-Za-z0-9_-]{1,64}$/,'observation_invalid');
  assert.notEqual(request.sourceSessionId,request.targetSessionId,'observation_invalid');}
// query仅生成固定PG只读观察语句；owner/行ID按文本投影，内部JSON由parsePrivate无损读取，不在生产repo增加方言。
function query(scope,request){validateRequest(scope,request);const subject='device:'+request.source,target='device:'+request.target;
  return `BEGIN READ ONLY;\nSET LOCAL statement_timeout='2000ms';\nSET LOCAL lock_timeout='500ms';\nWITH selected AS (
SELECT h.* FROM im_native_human_handoffs h WHERE tenant_code='${scope.tenantCode}' AND source_session_id='${request.sourceSessionId}' AND generation='${request.targetSessionId}'
AND subject='${subject}' AND scope::jsonb->'target'->>'subject'='${target}' AND scope::jsonb->'retained'->>'subject'='${scope.remote}'
AND (scope::jsonb->>'startedAt')::bigint>=${scope.createdAfter} AND (scope::jsonb->>'startedAt')::bigint<${scope.createdBefore} LIMIT 2
) SELECT COALESCE(json_agg(json_build_object('id',h.id::text,'tenantCode',h.tenant_code,'subject',h.subject,'ownerUserId',h.owner_user_id::text,
'sourceSessionId',h.source_session_id,'callId',h.call_id,'handoffId',h.handoff_id,'generation',h.generation,'ownerNode',h.owner_node,'ownerEpoch',h.owner_epoch,
'revision',h.revision,'state',h.state,'scope',h.scope,'activeKey',h.active_key,'retainUntil',h.retain_until,'sourceReleaseRecorded',h.source_release_recorded,'operationDeadline',h.operation_deadline,'callDeadline',h.call_deadline,
'commitToken',h.commit_token,'commitFingerprint',h.commit_fingerprint,'preparedResult',h.prepared_result,'result',h.result,
'sessions',(SELECT COALESCE(json_agg(json_build_object('id',s.id::text,'subject',s.subject,'purpose',s.purpose,'state',s.state,'ownerNode',s.owner_node,'ownerEpoch',s.owner_epoch,'leaseUntil',s.lease_until,'deadline',s.deadline,'resourceId',s.resource_id,'snapshot',s.snapshot)),'[]'::json)
FROM im_signaling_sessions s WHERE s.tenant_code=h.tenant_code AND EXISTS(SELECT 1 FROM jsonb_each(h.scope::jsonb) ref WHERE ref.key IN ('source','retained','target') AND s.id=ref.value->>'sessionId' AND s.subject=ref.value->>'subject')),
'connections',(SELECT COALESCE(json_agg(json_build_object('subject',c.subject,'connectionEpoch',c.connection_epoch,'state',c.state,'ownerNode',c.owner_node,'ownerEpoch',c.owner_epoch,'leaseUntil',c.lease_until,'snapshot',c.snapshot)),'[]'::json)
FROM im_signaling_focus_connections c WHERE c.tenant_code=h.tenant_code AND EXISTS(SELECT 1 FROM jsonb_each(h.scope::jsonb) ref WHERE ref.key IN ('source','retained','target') AND c.subject=ref.value->>'subject' AND c.connection_epoch=(ref.value->>'connectionEpoch')::bigint))
)),'[]'::json)::text FROM selected h;\nROLLBACK;\n`;
}
// base64严格解Go []byte容器，摘要始终取原UTF8容器字节，不重新序列化PreparedResult。
function base64(value){assert.ok(typeof value==='string'&&value.length>0&&value.length<=32768,'observation_invalid');const bytes=Buffer.from(value,'base64');assert.equal(bytes.toString('base64'),value,'observation_invalid');assert.equal(Buffer.from(bytes.toString('utf8')).equals(bytes),true,'observation_invalid');return bytes.toString('utf8');}
// pair只接受原完整mic/speaker，不将额外组、重复项或未知部分资源视为整组。
function pair(resources){return Array.isArray(resources)&&resources.length===2&&new Set(resources).size===2&&resources.includes('microphone')&&resources.includes('speaker');}
// validID/decimal检查原私有标识和int64文本；修订/时间仍必须遵循MaxSafeInteger。
function validID(value,max=128){return typeof value==='string'&&value.length>0&&Buffer.byteLength(value)<=max&&!/[\x00-\x20\x7f]/.test(value);}
function decimal(value){return typeof value==='string'&&/^[1-9][0-9]*$/.test(value)&&BigInt(value)<=9223372036854775807n;}
function safe(value,positive=true){return Number.isSafeInteger(value)&&(positive?value>0:value>=0);}
// readScope固定行与三原来源；任一旧记录/未知扩展不能获得新观察许可。
function readScope(scope,request,row,now){
  const origin=parsePrivate(row.scope,16384);requireKeys(origin,['callId','handoffId','generation','prepareToken','source','retained','target','callRevision','endpointRevision','startedAt','operationDeadline','callDeadline']);
  assert.equal(row.tenantCode,scope.tenantCode,'observation_invalid');assert.equal(row.subject,'device:'+request.source,'observation_invalid');
  assert.equal(row.sourceSessionId,request.sourceSessionId,'observation_invalid');assert.equal(row.generation,request.targetSessionId,'observation_invalid');
  assert.equal(origin.source.sessionId,request.sourceSessionId,'observation_invalid');assert.equal(origin.target.sessionId,request.targetSessionId,'observation_invalid');
  assert.equal(origin.source.subject,'device:'+request.source,'observation_invalid');assert.equal(origin.target.subject,'device:'+request.target,'observation_invalid');assert.equal(origin.retained.subject,scope.remote,'observation_invalid');
  for(const key of ['callId','handoffId','generation','operationDeadline','callDeadline'])assert.equal(row[key],origin[key],'observation_invalid');
  assert.ok(decimal(row.ownerUserId)&&row.ownerUserId===origin.source.ownerUserId&&row.id===rowID(scope.tenantCode,row.ownerUserId,row.sourceSessionId,row.handoffId)&&safe(row.revision)&&safe(row.ownerEpoch)&&row.ownerNode===origin.source.ownerNode&&row.ownerEpoch===origin.source.ownerEpoch,'observation_invalid');
  assert.ok(validID(origin.prepareToken)&&safe(origin.callRevision)&&safe(origin.endpointRevision)&&origin.startedAt>=scope.createdAfter&&origin.startedAt<scope.createdBefore&&now>=origin.startedAt&&now<origin.callDeadline&&origin.operationDeadline===origin.startedAt+30000,'observation_invalid');
  if(request.stage!=='aborted')assert.ok(now<origin.operationDeadline,'observation_invalid');
  for(const ref of [origin.source,origin.retained,origin.target]){
    requireKeys(ref,['kind','sessionId','subject','ownerUserId','connectionEpoch','connectionInstance','focusId','deadline','ownerNode','ownerEpoch','connectionOwnerEpoch']);
    assert.ok(ref.kind==='native'&&validID(ref.sessionId,64)&&validID(ref.subject,260)&&decimal(ref.ownerUserId)&&BigInt(ref.ownerUserId)<=BigInt(Number.MAX_SAFE_INTEGER)&&safe(ref.connectionEpoch)&&/^[a-f0-9]{32}$/.test(ref.connectionInstance)&&validID(ref.focusId)&&ref.deadline>=origin.callDeadline&&safe(ref.deadline)&&safe(ref.ownerEpoch)&&safe(ref.connectionOwnerEpoch)&&validID(ref.ownerNode),'observation_invalid');
  }
  assert.equal(origin.target.deadline,origin.callDeadline,'observation_invalid');assert.equal(origin.source.ownerUserId,origin.target.ownerUserId,'observation_invalid');
  const terminal=['committed','aborted','failed'].includes(row.state);
  assert.ok(terminal?row.activeKey===null&&safe(row.retainUntil)&&row.retainUntil>now:row.activeKey===goHash(scope.tenantCode,origin.callId)&&row.retainUntil===0,'observation_invalid');
  assert.ok(origin.callRevision<=Number.MAX_SAFE_INTEGER-2&&origin.endpointRevision<=Number.MAX_SAFE_INTEGER-2,'observation_invalid');
  for(const key of ['subject','sessionId'])assert.equal(new Set([origin.source,origin.retained,origin.target].map(ref=>ref[key])).size,3,'observation_invalid');return origin;
}
// resourceActual只读原专用锚和原连接实际全组；released允许旧租期到期，但不能把到期自动解释为released。
function resourceActual(row,origin,ref,state,now){
  const sessions=row.sessions.filter(value=>value.id===ref.sessionId&&value.subject===ref.subject),connections=row.connections.filter(value=>value.subject===ref.subject&&value.connectionEpoch===ref.connectionEpoch);
  assert.equal(sessions.length,1,'observation_invalid');assert.equal(connections.length,1,'observation_invalid');
  const session=sessions[0],connection=connections[0],stored=parsePrivate(session.snapshot,16384),snapshot=parsePrivate(connection.snapshot,524288);
  requireKeys(stored,['nativeFocus','closing']);
  const expected={sessionId:ref.sessionId,subject:ref.subject,callId:origin.callId,ownerUserId:ref.ownerUserId,connectionEpoch:ref.connectionEpoch,connectionInstance:ref.connectionInstance,focusId:ref.focusId,deadline:ref.deadline};
  assert.deepEqual({...stored.nativeFocus},expected,'observation_invalid');
  assert.ok(session.purpose==='humanCall'&&session.resourceId==='nativeFocus:'+ref.focusId&&session.ownerNode===ref.ownerNode&&session.ownerEpoch===ref.ownerEpoch&&session.deadline===ref.deadline&&connection.ownerNode===ref.ownerNode&&connection.ownerEpoch===ref.connectionOwnerEpoch&&snapshot.baseline?.ownerUserId===ref.ownerUserId&&pair(snapshot.baseline.resources)&&!snapshot.pending,'observation_invalid');
  if(state!=='released')assert.ok(!stored.closing&&session.state==='established'&&session.leaseUntil>now&&session.deadline>now&&connection.state==='ready'&&connection.leaseUntil>now,'observation_invalid');
  else assert.equal(stored.closing,true,'observation_invalid');
  const lease=snapshot.leases?.[ref.focusId];assert.ok(lease,'observation_invalid');
  const original=lease.original,actual=lease.actual;
  requireKeys(original,['msgToken','method','sessionId','eventSeq','params']);requireKeys(original.params,['operation','expectedRevision','focusId','resources','priority','resumePolicy','expiresAt']);
  assert.ok(safe(original.eventSeq)&&safe(original.params.expectedRevision,false)&&original.params.priority==='call'&&original.params.resumePolicy==='never','observation_invalid');
  assert.ok(original&&original.method==='focusControl'&&original.sessionId===ref.sessionId&&validID(original.msgToken)&&original.params?.operation==='acquire'&&original.params.focusId===ref.focusId&&pair(original.params.resources)&&original.params.expiresAt===ref.deadline,'observation_invalid');
  assert.ok(actual?.state===state&&actual.focusId===ref.focusId&&pair(actual.resources)&&safe(actual.revision)&&actual.revision<=snapshot.resourceRevision&&!actual.reason,'observation_invalid');
  if(state!=='released')assert.ok(original.params.expiresAt>now,'observation_invalid');return true;
}
// redisBatch只生成原固定键/字段，不提供SCAN/HGETALL或写命令；原token均先从已核Scope/固定结果取得。
function redisBatch(scope,origin,row){
  const terminal=row.state==='aborted'?parsePrivate(row.result,24000).token:row.commitToken;
  const endpoint='im:call:endpoint-handoff:'+goHash(scope.tenantCode,origin.source.ownerUserId,origin.source.subject,origin.source.sessionId);
  return [
    {command:'HMGET',key:'im:call:'+origin.callId,fields:['call_id','state','call_type','caller_id','callee_id','native_revision','native_human_creation','native_human_legs']},
    ...[origin.source,origin.retained,origin.target].map(ref=>({command:'GET',key:'im:call:user:'+ref.subject})),
    {command:'HMGET',key:'im:call:transfer-candidate:'+goHash(scope.tenantCode,origin.callId),fields:[goHash(origin.source.subject,origin.prepareToken),'_active','_target']},
    {command:'HMGET',key:endpoint,fields:['h:'+goHash(origin.handoffId),'o:'+goHash(origin.prepareToken),...(terminal?['o:'+goHash(terminal)]:[])]}
  ];
}
// legMatches核具体原腿，不从caller角色或同账号推断授权。
function legMatches(leg,ref){return exact(leg,['subject','sessionId','ownerUserId','connectionEpoch','connectionInstance','endpointRevision'])&&leg.subject===ref.subject&&leg.sessionId===ref.sessionId&&leg.ownerUserId===ref.ownerUserId&&leg.connectionEpoch===ref.connectionEpoch&&leg.connectionInstance===ref.connectionInstance&&safe(leg.endpointRevision,false);}
// validateRedis将原IM当前腿、候选和endpoint索引相交；状态历史本身不能开放业务媒体。
function validateRedis(scope,origin,row,stage,reply){
  assert.ok(Array.isArray(reply)&&reply.length===6&&Array.isArray(reply[0])&&reply[0].length===8&&Array.isArray(reply[4])&&reply[4].length===3&&Array.isArray(reply[5])&&reply[5].length>1,'observation_invalid');
  for(const item of reply)for(const value of Array.isArray(item)?item:[item])assert.ok(value===null||typeof value==='string'&&Buffer.byteLength(value)<=65536,'observation_invalid');
  const [call,sBusy,rBusy,tBusy,candidate,index]=reply;
  assert.equal(index.length,stage==='ready'?2:3,'observation_invalid');
  assert.ok(call[0]===origin.callId&&call[1]==='connected'&&call[2]==='voice'&&rBusy===origin.callId,'observation_invalid');
  const rawFields=new Map(),creation=parsePrivate(call[6],8192),legs=parsePrivate(call[7],16384),record=parsePrivate(candidate[0],65536,rawFields);
  assert.equal(index[0],candidate[0],'observation_invalid'); // 原h字段冻结整个candidate原字节，不是endpoint子对象。
  const endpoint=record.endpoint;
  allowed(record,['fingerprint','generation','targetKey','status','endpoint','humanState'],['events','humanCommittedState','humanReleasedState','humanBoundDigest']);
  requireKeys(endpoint,['handoffId','sourceSessionId','sourceEpoch','retainedEpoch','targetEpoch','sourceInstance','retainedInstance','targetInstance','startedAt','expiresAt','state','replies']);
  requireKeys(creation,['callId','tenantCode','creatorUserId','creatorSubject','originalCallerSubject','originalCallerUserId','originalCalleeSubject','originalCalleeUserId','conversationId','createdAt']);
  requireKeys(legs,['creation','caller','callee','callDeadline','version']);assert.deepEqual(legs.creation,creation,'observation_invalid');
  assert.ok(creation.callId===origin.callId&&creation.tenantCode===scope.tenantCode&&creation.createdAt*1000>=scope.createdAfter-1000&&creation.createdAt*1000<scope.createdBefore&&decimal(creation.creatorUserId)&&creation.creatorSubject===scope.source&&creation.creatorSubject===creation.originalCallerSubject&&creation.creatorUserId===creation.originalCallerUserId&&creation.originalCallerUserId===origin.source.ownerUserId&&creation.originalCalleeSubject===scope.remote&&creation.originalCalleeUserId===origin.retained.ownerUserId&&legs.callDeadline===origin.callDeadline&&safe(legs.version),'observation_invalid');
  assert.equal(call[3],legs.caller.subject,'observation_invalid');assert.equal(call[4],legs.callee.subject,'observation_invalid');
  assert.ok(record.generation===origin.generation&&record.targetKey==='im:call:user:'+origin.target.subject&&endpoint&&/^[a-f0-9]{64}$/.test(record.fingerprint),'observation_invalid');
  assert.ok(endpoint.handoffId===origin.handoffId&&endpoint.sourceSessionId===origin.source.sessionId&&endpoint.sourceEpoch===origin.source.connectionEpoch&&endpoint.retainedEpoch===origin.retained.connectionEpoch&&endpoint.targetEpoch===origin.target.connectionEpoch&&endpoint.sourceInstance===origin.source.connectionInstance&&endpoint.retainedInstance===origin.retained.connectionInstance&&endpoint.targetInstance===origin.target.connectionInstance&&endpoint.startedAt===origin.startedAt&&endpoint.expiresAt===origin.operationDeadline&&endpoint.state?.state===stage,'observation_invalid');
  assert.ok(record.humanState&&record.humanState.creation&&JSON.stringify(record.humanState.creation)===JSON.stringify(creation)&&record.humanState.callDeadline===origin.callDeadline,'observation_invalid');
  requireKeys(record.humanState,['creation','caller','callee','callDeadline','version']);assert.ok(safe(record.humanState.version),'observation_invalid');
  const before=[record.humanState.caller,record.humanState.callee];assert.ok(before.some(leg=>legMatches(leg,origin.source))&&before.some(leg=>legMatches(leg,origin.retained)),'observation_invalid');
  const prepareReply=parsePrivate(index[1],8192),prepare=parsePrivate(base64(prepareReply.payload),4096);
  requireKeys(prepareReply,['fingerprint','payload']);assert.match(prepareReply.fingerprint,/^[a-f0-9]{64}$/,'observation_invalid');
  assert.deepEqual(endpoint.replies?.[goHash(origin.prepareToken)],prepareReply,'observation_invalid');
  assert.ok(prepare.method==='endpointState'&&prepare.msgToken===origin.prepareToken&&prepare.sessionId===origin.source.sessionId&&prepare.code===200&&prepare.data?.state==='preparing'&&prepare.data.handoffId===origin.handoffId,'observation_invalid');
  assert.equal(prepare.data.revision,origin.endpointRevision,'observation_invalid');
  // 固定首次reserve的三段指纹；policy按原Go结构字段序和仅首次prepare回复构造，不拿当前ready/终态替换初始记录。
  const initialPolicy={handoffId:endpoint.handoffId,sourceSessionId:endpoint.sourceSessionId,sourceEpoch:endpoint.sourceEpoch,
    retainedEpoch:endpoint.retainedEpoch,targetEpoch:endpoint.targetEpoch,sourceInstance:endpoint.sourceInstance,retainedInstance:endpoint.retainedInstance,targetInstance:endpoint.targetInstance,
    startedAt:endpoint.startedAt,expiresAt:endpoint.expiresAt,state:{state:'preparing',revision:origin.endpointRevision,handoffId:origin.handoffId},replies:{[goHash(origin.prepareToken)]:prepareReply}};
  const initial=goHash(scope.tenantCode,origin.source.ownerUserId,origin.callId,origin.source.subject,origin.target.subject,String(origin.callRevision-1),origin.prepareToken);
  assert.equal(record.fingerprint,goHash(goHash(initial,'humanLegs',rawFields.get('humanState')),'endpoint',goJSON(initialPolicy)),'observation_invalid');
  if(stage==='ready')assert.ok(sBusy===origin.callId&&tBusy===origin.generation&&record.status==='reserved'&&candidate[1]===origin.generation&&candidate[2]===record.targetKey&&!record.humanBoundDigest&&!record.humanCommittedState&&!record.humanReleasedState&&[legs.caller,legs.callee].some(leg=>legMatches(leg,origin.source))&&[legs.caller,legs.callee].some(leg=>legMatches(leg,origin.retained)),'observation_invalid');
  else{
    assert.ok(candidate[1]===null&&candidate[2]===null&&/^[a-f0-9]{64}$/.test(record.humanBoundDigest),'observation_invalid');
    if(stage==='committed')assert.ok(record.status==='committed'&&sBusy!==origin.callId&&tBusy===origin.callId&&legMatches(legs.caller,origin.retained)&&legMatches(legs.callee,origin.target)&&JSON.stringify(record.humanCommittedState)===JSON.stringify(legs)&&!record.humanReleasedState,'observation_invalid');
    else assert.ok(record.status==='released'&&sBusy===origin.callId&&tBusy!==origin.generation&&JSON.stringify(record.humanReleasedState)===JSON.stringify(legs)&&!record.humanCommittedState&&[legs.caller,legs.callee].some(leg=>legMatches(leg,origin.source))&&[legs.caller,legs.callee].some(leg=>legMatches(leg,origin.retained)),'observation_invalid');
    const terminal=stage==='aborted'?parsePrivate(row.result,24000).token:row.commitToken,terminalReply=parsePrivate(index[2],8192);
    requireKeys(terminalReply,['fingerprint','payload']);assert.match(terminalReply.fingerprint,/^[a-f0-9]{64}$/,'observation_invalid');
    assert.deepEqual(endpoint.replies?.[goHash(terminal)],terminalReply,'observation_invalid');
    const fixed=parsePrivate(stage==='aborted'?row.result:row.preparedResult,24000);
    assert.equal(terminalReply.payload,fixed.reply,'observation_invalid');
    assert.ok(Array.isArray(record.events)&&record.events.length===fixed.events.length,'observation_invalid');
    record.events.forEach((event,index)=>{requireKeys(event,['Subject','Payload']);assert.equal(event.Subject,fixed.events[index].subject,'observation_invalid');assert.equal(event.Payload,fixed.events[index].payload,'observation_invalid');});
    const stateField=stage==='committed'?'humanCommittedState':'humanReleasedState';
    assert.equal(legs.version,record.humanState.version+1,'observation_invalid');
    if(stage==='committed'){assert.equal(legs.callee.endpointRevision,origin.endpointRevision+2,'observation_invalid');assert.equal(call[5],String(origin.callRevision+1),'observation_invalid');}
    else {assert.equal([legs.caller,legs.callee].find(leg=>leg.subject===origin.source.subject).endpointRevision,origin.endpointRevision+2,'observation_invalid');assert.equal(call[5],String(origin.callRevision),'observation_invalid');}
    const digest=crypto.createHash('sha256').update('{"state":'+rawFields.get(stateField)+',"events":'+rawFields.get('events')+'}').digest('hex');
    assert.equal(digest,record.humanBoundDigest,'observation_invalid'); // 原Go容器子字节不经Number/重序列化，保持int64与字段顺序。
  }
  return {record,legs};
}
// checkEvents严格解原Go字节容器并核顺序与修订；原JSON文本用于摘要，不经JSON.stringify重签。
function checkEvents(container,origin,stage,row){
  const prepared=parsePrivate(container,24000),aborted=stage==='aborted';
  requireKeys(prepared,aborted?['token','fingerprint','targetReady','reply','events','imDigest']:['reply','events']);
  const canonicalEvents=prepared.events?.map(event=>({subject:event.subject,payload:event.payload}));
  const canonical=aborted?{token:prepared.token,fingerprint:prepared.fingerprint,targetReady:prepared.targetReady,reply:prepared.reply,events:canonicalEvents,imDigest:prepared.imDigest}:{reply:prepared.reply,events:canonicalEvents};
  assert.equal(container,goJSON(canonical),'observation_invalid'); // Go容器规范字节必须一致；摘要仍核原传入UTF8，不签重排后的替身。
  const reply=parsePrivate(base64(prepared.reply),4096),state=reply.data;
  assert.ok(reply.method==='endpointState'&&reply.sessionId===origin.source.sessionId&&reply.code===200&&state?.state===stage&&state.handoffId===origin.handoffId&&state.revision===origin.endpointRevision+2,'observation_invalid');
  assert.equal(reply.msgToken,aborted?prepared.token:row.commitToken,'observation_invalid');
  if(aborted)assert.ok(prepared.targetReady===true&&state.reason==='userCancelled'&&!state.candidateSessionId&&/^[a-f0-9]{64}$/.test(prepared.fingerprint)&&/^[a-f0-9]{64}$/.test(prepared.imDigest),'observation_invalid');
  else assert.equal(state.candidateSessionId,origin.generation,'observation_invalid');
  const refs=aborted?[origin.source,origin.target,origin.target]:[origin.source,origin.source,origin.retained,origin.target,origin.target];
  const methods=aborted?['endpointState','endpointState','sessionEnded']:['endpointState','sessionEnded','callState','sessionEstablished','endpointState'];
  assert.ok(Array.isArray(prepared.events)&&prepared.events.length===refs.length,'observation_invalid');const seq=[];
  prepared.events.forEach((event,index)=>{requireKeys(event,['subject','payload']);const message=parsePrivate(base64(event.payload),4096);assert.ok(event.subject===refs[index].subject&&message.sessionId===refs[index].sessionId&&message.method===methods[index]&&safe(message.eventSeq)&&!Object.hasOwn(message,'code')&&!Object.hasOwn(message,'params')&&!Object.hasOwn(message,'messageSeq'),'observation_invalid');seq.push(message.eventSeq);
    if(message.method==='endpointState')assert.deepEqual(message.data,state,'observation_invalid');
    if(message.method==='sessionEnded')assert.deepEqual({...message.data},{state:'ended',reason:aborted?'userCancelled':'transferred'},'observation_invalid');
    if(message.method==='sessionEstablished')assert.deepEqual({...message.data},{state:'established',mediaRevision:1},'observation_invalid');
    if(message.method==='callState')assert.ok(message.data.state==='active'&&message.data.revision===origin.callRevision+1&&message.data.requestToken===origin.prepareToken,'observation_invalid');
  });
  assert.ok(aborted?seq[2]===seq[1]+1:seq[1]===seq[0]+1&&seq[4]===seq[3]+1,'observation_invalid');return prepared;
}
// createObserver固定本轮范围和原始字节pin；每次读取SQL与Redis当前事实，未知快照不捞历史成功，不执行任何写入。
function createObserver(input,transport){
  validate(input);requireKeys(transport,['readSql','readRedis','readWitness','traceKey','now']);
  for(const name of ['readSql','readRedis','readWitness','now'])assert.equal(typeof transport[name],'function','observation_invalid');
  join(transport.traceKey,input.tenantCode,'validation-source','validation-target');
  transport=Object.freeze({...transport}); // 本轮密钥和只读端口固定，不追随调用方后续替换。
  const scope=Object.freeze({...input}),pins=new Map(),store=createWitnessStore();let lastNow=0,reading=false,tainted=false;
  return {observationWindow:Object.freeze({createdAfter:scope.createdAfter,createdBefore:scope.createdBefore}),async readTransfer(request){
    if(reading||tainted)throw Error('native_human_observation_invalid');
    reading=true;
    try{
      validateRequest(scope,request);request=Object.freeze({...request});const now=transport.now();assert.ok(safe(now)&&now>=lastNow&&now>=scope.createdAfter&&now<scope.createdBefore,'observation_invalid');lastNow=now;
      if(request.attempt>1)assert.equal(pins.get(request.attempt-1)?.terminal,request.attempt===2?'aborted':'committed','observation_invalid');
      const rows=parsePrivate(await transport.readSql(query(scope,request)),2097152);assert.ok(Array.isArray(rows)&&rows.length<=2,'observation_invalid');if(!rows.length)return null;assert.equal(rows.length,1,'observation_invalid');
      const row=rows[0],origin=readScope(scope,request,row,now),pin=pins.get(request.attempt);
      if(pin)assert.equal(pin.scope,row.scope,'observation_invalid');else{
        assert.equal(request.stage,'ready','observation_invalid');
        if(request.attempt>1){const prior=pins.get(request.attempt-1),previous=prior.origin;
          assert.ok(origin.callId===previous.callId&&origin.callDeadline===previous.callDeadline&&origin.startedAt>=previous.startedAt&&origin.handoffId!==previous.handoffId&&origin.generation!==previous.generation,'observation_invalid');
          assert.equal(JSON.stringify(origin.retained),JSON.stringify(previous.retained),'observation_invalid');
          assert.equal(JSON.stringify(origin.source),JSON.stringify(request.attempt===2?previous.source:previous.target),'observation_invalid');
          assert.equal(origin.callRevision,previous.callRevision+(prior.terminal==='aborted'?1:2),'observation_invalid');
        }
        pins.set(request.attempt,{scope:row.scope,origin,terminal:null,terminalLegs:null});
      }
      const expectedState={ready:'targetReady',committed:'committed',aborted:'aborted'}[request.stage];
      if(row.state!==expectedState){assert.ok(['preparing','targetReady','sourceReleasing','sourceReleased','commitUnknown','aborting','unknown'].includes(row.state),'observation_invalid');return null;}
      assert.equal(row.sourceReleaseRecorded,request.stage==='committed','observation_invalid');
      assert.ok(Array.isArray(row.sessions)&&row.sessions.length===3&&Array.isArray(row.connections)&&row.connections.length===3,'observation_invalid');
      for(const [name,state] of [['source',request.stage==='committed'?'released':'acquired'],['retained','acquired'],['target',request.stage==='aborted'?'released':'acquired']])resourceActual(row,origin,origin[name],state,now);
      const batch=redisBatch(scope,origin,row),redis=await transport.readRedis(batch),{record,legs}=validateRedis(scope,origin,row,request.stage,redis);
      if(request.attempt>1)assert.equal(JSON.stringify(record.humanState),pins.get(request.attempt-1).terminalLegs,'observation_invalid');
      if(request.stage==='committed'){
        checkEvents(row.preparedResult,origin,request.stage,row);const bound=parsePrivate(row.result,256);requireKeys(bound,['imCommitDigest','preparedDigest']);
        assert.equal(row.result,goJSON({imCommitDigest:bound.imCommitDigest,preparedDigest:bound.preparedDigest}),'observation_invalid');
        assert.ok(bound.imCommitDigest===record.humanBoundDigest&&/^[a-f0-9]{64}$/.test(bound.imCommitDigest)&&bound.preparedDigest===crypto.createHash('sha256').update(row.preparedResult,'utf8').digest('hex'),'observation_invalid');
      }else if(request.stage==='aborted'){
        const fixed=checkEvents(row.result,origin,request.stage,row);assert.equal(fixed.imDigest,record.humanBoundDigest,'observation_invalid');
      }else assert.ok(row.preparedResult===null&&row.result===null&&row.commitToken===''&&row.commitFingerprint==='','observation_invalid');
      const expected=join(transport.traceKey,scope.tenantCode,origin.source.sessionId,origin.target.sessionId),witnesses=await transport.readWitness({join:expected});
      assert.ok(Array.isArray(witnesses)&&witnesses.length<=512&&witnesses.every(value=>typeof value==='string'),'observation_invalid');
      try{for(const raw of witnesses){assert.equal(parsePrivate(raw,4096).join,expected,'observation_invalid');store.push(raw);}}catch{tainted=true;throw Error('observation_invalid');}
      const after=transport.now();assert.ok(after>=now&&after<scope.createdBefore,'observation_invalid');lastNow=after;
      const current=store.current(expected,request.stage,after);if(!current)return null;
      assert.ok(current.validUntil<=origin.callDeadline,'observation_invalid');
      const repeat=await transport.readRedis(batch);assert.equal(JSON.stringify(redis),JSON.stringify(repeat),'observation_invalid');
      const completedAt=transport.now();assert.ok(completedAt>=after&&completedAt<scope.createdBefore,'observation_invalid');lastNow=completedAt;
      if(completedAt>=current.validUntil||completedAt>=origin.callDeadline||request.stage!=='aborted'&&completedAt>=origin.operationDeadline)return null;
      const result={stage:current.stage};for(const key of booleans)result[key]=current[key];
      if(request.stage!=='ready'){pins.get(request.attempt).terminal=request.stage;pins.get(request.attempt).terminalLegs=JSON.stringify(legs);}return result;
    }catch{throw Error('native_human_observation_invalid');}finally{reading=false;}
  }};
}
module.exports={parsePrivate,goHash,join,createWitnessStore,query,redisBatch,createObserver};
