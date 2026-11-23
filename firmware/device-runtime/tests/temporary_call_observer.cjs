// 文件说明：临时人际通话真实驱动的只读观察；SQL核原出处/实际焦点回执，私有生命周期事实独立核RAM对象。
// 执行边界：宿主注入已核归属的read/readWitness；本模块不执行Docker、自由脚本、API或生产写入。
'use strict';
const assert=require('node:assert/strict'),{createHmac}=require('node:crypto');
// exact 只接闭集自有数据字段，拒绝访问器、继承身份及额外Symbol。
function exact(value,keys){assert.ok(value&&Object.getPrototypeOf(value)===Object.prototype&&Reflect.ownKeys(value).length===keys.length,'observation_invalid');const descriptors=Object.getOwnPropertyDescriptors(value);
  assert.ok(keys.every(key=>Object.hasOwn(descriptors,key)&&Object.hasOwn(descriptors[key],'value')),'observation_invalid');}
// id/integer/subject 限定宿主冻结身份，禁自由SQL片段及不安全整数。
function id(value,max=128){assert.ok(typeof value==='string'&&value.length>0&&value.length<=max&&/^[A-Za-z0-9_.-]+$/.test(value),'scope_invalid');return value;}
function integer(value){assert.ok(Number.isSafeInteger(value)&&value>0,'scope_invalid');return value;}
function subject(value){assert.ok(typeof value==='string'&&/^device:[A-Za-z0-9_-]{1,128}:[A-Za-z0-9_-]{1,128}$/.test(value),'scope_invalid');return value;}
// validate 固定企业、普通主人、双端身份及最长180秒窗；目标关联仍由真实拨号与双方媒体scope验证，不伪造native SQL腿。
function validate(scope){exact(scope,['database','tenantCode','ownerUserId','subject','targetSubject','createdAfter','createdBefore']);assert.match(scope.database,/^[A-Za-z][A-Za-z0-9_]{0,62}$/,'scope_invalid');id(scope.tenantCode,50);integer(scope.ownerUserId);
  subject(scope.subject);subject(scope.targetSubject);assert.notEqual(scope.subject,scope.targetSubject,'scope_invalid');integer(scope.createdAfter);integer(scope.createdBefore);assert.ok(scope.createdBefore>scope.createdAfter&&scope.createdBefore-scope.createdAfter<=180000,'scope_invalid');}
// bounded 保持既有DB内2秒及500ms锁截止，SQL仅经宿主原stdin只读入口；不是通用SQL执行器。
function bounded(sql){assert.ok(!/\b(UPDATE|INSERT|DELETE|CREATE|DROP|ALTER|TRUNCATE|COPY|CALL)\b/i.test(sql),'readonly_required');return "BEGIN READ ONLY;\nSET LOCAL statement_timeout='2000ms';\nSET LOCAL lock_timeout='500ms';\n"+sql+'\nROLLBACK;\n';}
// window 固定进程创建窗，不在轮询时扩展或拿历史完成轮替代本轮。
function window(scope,alias){return `${alias}.created_time BETWEEN to_timestamp(${scope.createdAfter}/1000.0) AND to_timestamp(${scope.createdBefore}/1000.0)`;}
// sourceQuery 支持恢复后的第二轮；连续已完成文本只在DB核验非空，正文永不进入投影。
function sourceQuery(scope,turns){validate(scope);assert.ok(turns===1||turns===2,'source_invalid');const [,product,device]=scope.subject.split(':');return bounded(`WITH source AS (
SELECT s.id,s.deadline,s.resource_id,s.snapshot::jsonb snap FROM public.im_signaling_sessions s
WHERE s.tenant_code='${scope.tenantCode}' AND s.subject='${scope.subject}' AND s.purpose='aiConversation'
AND s.state='established' AND s.resource_id=s.id AND ${window(scope,'s')}
AND s.lease_until>floor(extract(epoch FROM clock_timestamp())*1000) AND s.deadline>floor(extract(epoch FROM clock_timestamp())*1000)
AND s.snapshot::jsonb->'binding'->>'subject'=s.subject AND s.snapshot::jsonb->'binding'->>'userId'='${scope.ownerUserId}'
AND EXISTS(SELECT 1 FROM public.im_signaling_operations op WHERE op.tenant_code=s.tenant_code AND op.subject=s.subject AND op.session_id=s.id
AND op.route='signaling' AND op.method='sessionCreate' AND op.status='completed' AND ${window(scope,'op')}
AND op.response::jsonb->>'method'='sessionCreated' AND op.response::jsonb->>'sessionId'=s.id) LIMIT 2), candidates AS (
SELECT source.id,source.deadline,source.snap,
EXISTS(SELECT 1 FROM public.ai_device_session_bindings b JOIN public.ai_sessions a ON a.session_id=b.session_id
WHERE b.session_id=source.resource_id AND b.tenant_code='${scope.tenantCode}' AND b.user_id=${scope.ownerUserId}
AND b.channel_type='device' AND b.channel_id='${product}:${device}' AND b.channel IN ('voice','text')
AND a.tenant_code=b.tenant_code AND a.user_id=b.user_id AND a.channel=b.channel AND a.session_kind='device' AND a.deleted_time=0
AND a.tool_id=0 AND a.app_id=0 AND a.project_id=0 AND a.scope_hash='' AND a.parent_session_id='' AND a.parent_summary_id=0
AND COALESCE(a.parent_snapshot,'')='' AND a.fork_request_hash=''
AND (SELECT count(*) FROM public.ai_session_summaries t WHERE t.session_id=a.session_id AND t.tenant_code=a.tenant_code AND t.user_id=a.user_id
AND t.channel=a.channel AND t.deleted_time=0 AND t.turn_index BETWEEN 1 AND ${turns} AND t.context_completed=true
AND char_length(t.user_input)>0 AND char_length(t.ai_response)>0 AND ${window(scope,'t')})=${turns}
AND (SELECT count(DISTINCT t.turn_index) FROM public.ai_session_summaries t WHERE t.session_id=a.session_id AND t.tenant_code=a.tenant_code
AND t.deleted_time=0 AND t.turn_index BETWEEN 1 AND ${turns})=${turns}
AND (SELECT count(*) FROM public.ai_session_summaries t WHERE t.session_id=a.session_id AND t.tenant_code=a.tenant_code AND t.deleted_time=0)=${turns})
AND EXISTS(SELECT 1 FROM public.dm_device_info d WHERE d.tenant_code='${scope.tenantCode}' AND d.product_id='${product}' AND d.device_name='${device}'
AND d.user_id=${scope.ownerUserId} AND d.deleted_time=0 AND d.is_online=1 AND d.is_enable=1
AND floor(extract(epoch FROM d.last_login)*1000)::text=source.snap->'binding'->>'connectionEpoch') AS valid FROM source)
SELECT COALESCE(json_agg(jsonb_build_object('sourceSessionId',id,'expiresAt',deadline,'valid',valid AND
(NOT (snap ? 'nativeTemporaryBinding') OR NOT (snap ? 'controlBinding') AND NOT (snap ? 'endpointBinding')))
|| CASE WHEN snap ? 'nativeTemporaryBinding' THEN jsonb_build_object('nativeBinding',snap->'nativeTemporaryBinding') ELSE '{}'::jsonb END),'[]'::json) FROM candidates;`);}
// parse 有界响应只输出固定错误；数据库/日志原文及私有身份不跟异常泄漏。
function parse(bytes,max=16384){assert.ok(Buffer.isBuffer(bytes)&&bytes.length<=max,'observation_invalid');try{return JSON.parse(bytes.toString().trim());}catch{throw Error('observation_invalid');}}
// sourceProjection 多候选拒绝，不选最新；可选rememberBinding只向可信宿主私有pin传四字段，不扩公开source返回值。
// 原来源固定后第二轮不能改会话、期限或私有来源；未知NB不按旧CB/EB回退。
function sourceProjection(bytes,turns,rememberBinding){assert.ok(turns===1||turns===2);const rows=parse(bytes);assert.ok(Array.isArray(rows)&&rows.length<=2,'source_invalid');if(!rows.length)return null;assert.equal(rows.length,1,'source_ambiguous');const value=rows[0],native=Object.hasOwn(value,'nativeBinding');exact(value,['sourceSessionId','expiresAt','valid',...(native?['nativeBinding']:[])]);id(value.sourceSessionId,64);integer(value.expiresAt);assert.equal(typeof value.valid,'boolean','source_invalid');
  if(native){const binding=value.nativeBinding;exact(binding,['connectionEpoch','connectionInstance','capabilityRevision','capabilityHash']);integer(binding.connectionEpoch);integer(binding.capabilityRevision);assert.match(binding.connectionInstance||'',/^[a-f0-9]{32}$/,'source_invalid');assert.match(binding.capabilityHash||'',/^[a-f0-9]{64}$/,'source_invalid');}
  if(!value.valid)return null;if(rememberBinding)rememberBinding(native?{...value.nativeBinding}:null);
  return {sourceSessionId:value.sourceSessionId,expiresAt:value.expiresAt,firstTurnIndex:1,lastTurnIndex:turns};}
// ackSQL 四种原完整组实际回执独立核验；AI终止release绝不能当人际release。
function ackSQL(operation,state,focus){return `((SELECT count(*) FROM jsonb_each(COALESCE(fs->'temporaryCommands','{}'::jsonb)) c
WHERE c.value->>'callSessionId'=origin->>'callSessionId' AND c.value->>'operation'='${operation}' AND COALESCE((c.value->>'releaseAI')::boolean,false)=false)=1
AND (SELECT count(*) FROM jsonb_each(COALESCE(fs->'temporaryCommands','{}'::jsonb)) c
WHERE c.value->>'callSessionId'=origin->>'callSessionId' AND c.value->>'token'=c.key AND c.value->>'operation'='${operation}'
AND COALESCE((c.value->>'releaseAI')::boolean,false)=false AND c.value->'actual'->>'state'='${state}'
AND c.value->'actual'->>'focusId'=origin->>'${focus}' AND (c.value->'actual'->>'revision')::bigint>0
AND c.value->'actual'->'resources' @> '["microphone","speaker"]'::jsonb AND jsonb_array_length(c.value->'actual'->'resources')=2)=1)`;}
// nativeBindingSQL 只识别初建独立临时来源；CB/EB同时存在、结构损坏或与原代次不同均拒绝，不能兜底扩权。
// capHash只作原私有来源冻结值，实际当前能力与主人仍由正式Go短票/媒体围栏核权，SQL不代替DMS。
function nativeBindingSQL(){return `(CASE WHEN jsonb_typeof(model->'nativeTemporaryBinding')='object' THEN
NOT (model ? 'controlBinding') AND NOT (model ? 'endpointBinding')
AND (SELECT count(*) FROM jsonb_object_keys(model->'nativeTemporaryBinding'))=4
AND NOT EXISTS(SELECT 1 FROM jsonb_object_keys(model->'nativeTemporaryBinding') k WHERE k NOT IN ('connectionEpoch','connectionInstance','capabilityRevision','capabilityHash'))
AND jsonb_typeof(model->'nativeTemporaryBinding'->'connectionEpoch')='number'
AND jsonb_typeof(model->'nativeTemporaryBinding'->'capabilityRevision')='number'
AND model->'nativeTemporaryBinding'->>'connectionEpoch'=origin->>'connectionEpoch'
AND model->'nativeTemporaryBinding'->>'connectionInstance'=origin->>'connectionInstance'
AND model->'nativeTemporaryBinding'->>'capabilityRevision'=origin->>'capabilityRevision'
AND model->'nativeTemporaryBinding'->>'capabilityHash' ~ '^[a-f0-9]{64}$'
AND origin->>'endpointEpoch'='1'
AND model->'binding'->>'subject'=origin->>'subject'
AND model->'binding'->>'userId'=origin->>'ownerUserId'
AND model->'binding'->>'connectionEpoch'=origin->>'connectionEpoch'
ELSE false END)`;}
// proofQuery 仅选原出处的身份键与布尔；NB追加私有capHash供原有pin冻结，不改变公开15bool或旧15项出处。
// 无模型正文/焦点报文/媒体地址，RAM同一性不由SQL推导。
function proofQuery(scope,source){validate(scope);id(source,64);const [,product,device]=scope.subject.split(':');return bounded(`WITH retained AS (
SELECT s.id,s.state,s.owner_node,s.deadline,s.lease_until,s.snapshot::jsonb model,f.snapshot::jsonb fs,f.state connection_state,f.lease_until connection_lease,
f.connection_epoch,tc.key call_session,tc.value fact,tc.value->'origin' origin
FROM public.im_signaling_sessions s JOIN public.im_signaling_focus_connections f ON f.tenant_code=s.tenant_code AND f.subject=s.subject
CROSS JOIN LATERAL jsonb_each(COALESCE(f.snapshot::jsonb->'temporaryCalls','{}'::jsonb)) tc
WHERE s.tenant_code='${scope.tenantCode}' AND s.id='${source}' AND s.subject='${scope.subject}' AND s.purpose='aiConversation'
AND ${window(scope,'s')} AND tc.value->'origin'->>'modelSessionId'=s.id LIMIT 2), facts AS (
SELECT *, ${ackSQL('suspend','suspended','aiFocusId')} suspended,${ackSQL('acquire','acquired','callFocusId')} acquired,
${ackSQL('release','released','callFocusId')} released,${ackSQL('restore','restored','aiFocusId')} restored FROM retained)
SELECT COALESCE(json_agg(json_build_object(
'origin',jsonb_build_array(origin->>'modelSessionId',origin->>'modelSubject',origin->>'aiSessionId',origin->>'subject',origin->>'ownerUserId',origin->>'connectionEpoch',origin->>'connectionInstance',origin->>'capabilityRevision',origin->>'endpointEpoch',origin->>'callId',origin->>'callSessionId',origin->>'aiFocusId',origin->>'callFocusId',origin->>'leaseExpiresAt',origin->>'callExpiresAt') || CASE WHEN model ? 'nativeTemporaryBinding' THEN jsonb_build_array(model->'nativeTemporaryBinding'->>'capabilityHash') ELSE '[]'::jsonb END,
'sameOriginal',COALESCE(origin->>'modelSessionId'=id AND origin->>'modelSubject'='${scope.subject}' AND origin->>'aiSessionId'=id AND origin->>'subject'='${scope.subject}'
AND origin->>'ownerUserId'='${scope.ownerUserId}' AND origin->>'callSessionId'=call_session AND origin->>'connectionEpoch'=connection_epoch::text
AND origin->>'connectionInstance' ~ '^[a-f0-9]{32}$' AND (origin->>'capabilityRevision')::bigint>0
AND CASE WHEN model ? 'nativeTemporaryBinding' THEN ${nativeBindingSQL()} ELSE
origin->>'connectionEpoch'=COALESCE(model->'controlBinding'->>'connectionEpoch',model->'endpointBinding'->>'connectionEpoch')
AND origin->>'connectionInstance'=COALESCE(model->'controlBinding'->>'connectionInstance',model->'endpointBinding'->>'connectionInstance')
AND origin->>'capabilityRevision'=COALESCE(model->'controlBinding'->>'capabilityRevision',model->'endpointBinding'->>'capabilityRevision') END
AND origin->'originalAI'=fs->'leases'->(origin->>'aiFocusId')->'original',false),
'modelRetained',state='established' AND lease_until>floor(extract(epoch FROM clock_timestamp())*1000) AND deadline>floor(extract(epoch FROM clock_timestamp())*1000),
'suspendedAck',suspended,'humanAcquiredAck',acquired,'humanReleasedAck',released,'restoredAck',restored,
'resumeAllowed',fact->>'phase'='awaitExplicitResume' AND COALESCE((fact->>'callEnded')::boolean,false) AND NOT COALESCE((fact->>'stopped')::boolean,false)
AND released AND connection_state='ready' AND connection_lease>floor(extract(epoch FROM clock_timestamp())*1000) AND fs->'pending' IS NULL,
'stoppedIntent',COALESCE((fact->>'stopped')::boolean,false),'revoked',connection_state<>'ready' OR connection_lease<=floor(extract(epoch FROM clock_timestamp())*1000)
OR NOT EXISTS(SELECT 1 FROM public.dm_device_info d WHERE d.tenant_code='${scope.tenantCode}' AND d.product_id='${product}' AND d.device_name='${device}'
AND d.user_id=${scope.ownerUserId} AND d.deleted_time=0 AND d.is_online=1 AND d.is_enable=1
AND floor(extract(epoch FROM d.last_login)*1000)::text=origin->>'connectionEpoch'),
'pendingRestore',COALESCE(fact->>'phase'='restoringAI' OR fs->'pending'->'temporaryCall'->>'operation'='restore',false),
'restoreConsumed',restored AND fact->>'phase'='resumed' AND COALESCE((fact->>'callEnded')::boolean,false) AND NOT COALESCE((fact->>'stopped')::boolean,false)
AND fs->'pending' IS NULL AND connection_state='ready' AND connection_lease>floor(extract(epoch FROM clock_timestamp())*1000)
AND fs->'leases'->(origin->>'aiFocusId')->'actual'->>'state'='restored'
AND fs->'leases'->(origin->>'aiFocusId')->'actual'->>'revision'=fs->>'resourceRevision'
AND EXISTS(SELECT 1 FROM jsonb_each(COALESCE(fs->'temporaryCommands','{}'::jsonb)) c WHERE c.value->>'callSessionId'=origin->>'callSessionId'
AND c.value->>'operation'='restore' AND c.value->>'restoreToken'=fact->>'restoreToken' AND c.value->>'restoreFingerprint'=fact->>'restoreFingerprint'
AND c.value->'actual'=fs->'leases'->(origin->>'aiFocusId')->'actual')
AND EXISTS(SELECT 1 FROM public.im_signaling_operations op WHERE op.tenant_code='${scope.tenantCode}' AND op.subject='${scope.subject}' AND op.session_id=facts.id
AND op.method='audioStart' AND op.route='signaling' AND op.status='completed' AND op.token=fact->>'restoreToken' AND op.fingerprint=fact->>'restoreFingerprint')
)),'[]'::json) FROM facts;`);}
// proofProjection 保留唯一原fact键在私有内存；缺值、空ACK与歧义不升级成功。
function proofProjection(bytes){const rows=parse(bytes);assert.ok(Array.isArray(rows)&&rows.length<=2,'proof_invalid');if(!rows.length)return null;assert.equal(rows.length,1,'proof_ambiguous');const value=rows[0];exact(value,['origin','sameOriginal','modelRetained','suspendedAck','humanAcquiredAck','humanReleasedAck','restoredAck','resumeAllowed','stoppedIntent','revoked','pendingRestore','restoreConsumed']);
  assert.ok(Array.isArray(value.origin)&&[15,16].includes(value.origin.length)&&value.origin.every(part=>typeof part==='string'&&part.length>0&&part.length<=260&&!/[\x00-\x1f\x7f]/.test(part)),'proof_invalid');
  if(value.origin.length===16)assert.ok(/^[a-f0-9]{64}$/.test(value.origin[15])&&value.origin[8]==='1','proof_invalid');
  for(const key of Object.keys(value).filter(key=>key!=='origin'))assert.equal(typeof value[key],'boolean','proof_invalid');return value;}
// join 与Go ailifetime.Join完全相同的域及uint64字节长度；独立取证密钥不进入输出。
function join(key,tenant,source){assert.ok(typeof key==='string'&&Buffer.byteLength(key)>=32&&Buffer.byteLength(key)<=256,'trace_key_required');const mac=createHmac('sha256',key).update('s18-ai-lifetime-v1\0');for(const part of [tenant,source]){const bytes=Buffer.from(part),size=Buffer.alloc(8);size.writeBigUInt64BE(BigInt(bytes.length));mac.update(size).update(bytes);}return mac.digest('hex');}
// witnessProjection 只接宿主本轮原日志裁剪的闭集payload；禁止最新代次猜测及跨进程时间排序。
// 返回恢复计数供同observer前后比较，计数不是供应商WebSocket实例或硬件验收证据。
function witnessProjection(bytes,expectedJoin,restoreConsumed){assert.match(expectedJoin,/^[a-f0-9]{64}$/);const rows=parse(bytes,65536);assert.ok(Array.isArray(rows)&&rows.length<=128,'runtime_invalid');if(!rows.length)return null;
  const im=[],chat=[],audio=[];
  for(const row of rows){assert.ok(row&&['temporaryRuntimeWitness','aiLifetimeWitness'].includes(row.marker),'runtime_invalid');assert.ok(row.join===expectedJoin,'runtime_scope_changed');
    if(row.marker==='temporaryRuntimeWitness'){exact(row,['marker','stage','join','generation','sameRuntime','sameConnection','alive','closeObserved','stopRequired','stopCompleted','stopAck']);assert.ok(['created','paused','resumed','retired'].includes(row.stage)&&Number.isSafeInteger(row.generation)&&row.generation>0&&row.generation<=512,'runtime_invalid');
      for(const key of ['sameRuntime','sameConnection','alive','closeObserved','stopRequired','stopCompleted','stopAck'])assert.equal(typeof row[key],'boolean','runtime_invalid');im.push(row);
    }else{exact(row,row.kind==='chat'?['marker','kind','stage','join','generation','audioGeneration']:['marker','kind','stage','join','generation']);assert.ok(['chat','audio'].includes(row.kind)&&['bound','turnStopped','resumed','unbound','retired','audioClosed','unknown'].includes(row.stage)&&/^[a-f0-9]{32}$/.test(row.generation),'runtime_invalid');if(row.kind==='chat')assert.match(row.audioGeneration,/^[a-f0-9]{32}$/,'runtime_pair_unknown');(row.kind==='chat'?chat:audio).push(row);}}
  if(!im.length||!chat.length||!audio.length)return null;
  assert.ok(new Set(im.map(row=>row.generation)).size===1&&new Set(chat.map(row=>row.generation)).size===1&&new Set(audio.map(row=>row.generation)).size===1,'runtime_ambiguous');
  assert.ok(im.length<=4&&new Set(im.map(row=>row.stage)).size===im.length&&chat.length<=33&&audio.length<=33,'runtime_invalid');
  assert.ok(im[0].stage==='created'&&chat[0].stage==='bound'&&audio[0].stage==='bound','runtime_initial_missing');
  assert.ok(!im[0].stopRequired&&!im[0].stopCompleted&&!im[0].stopAck&&chat.filter(row=>row.stage==='bound').length===1&&audio.filter(row=>row.stage==='bound').length===1,'runtime_initial_invalid');
  assert.ok(!im.some(row=>row.stage==='retired'||!row.sameRuntime||!row.sameConnection||!row.alive||row.closeObserved)&&![...chat,...audio].some(row=>['unbound','retired','audioClosed','unknown'].includes(row.stage)),'runtime_not_retained');
  assert.ok(chat.every(row=>row.audioGeneration===audio[0].generation),'runtime_pair_changed');
  const paused=im.find(row=>row.stage==='paused');if(!paused)return null;
  assert.ok(paused.stopCompleted&&paused.stopAck===paused.stopRequired,'runtime_stop_unconfirmed');
  const resumed=im.find(row=>row.stage==='resumed');if(restoreConsumed&&!resumed)return null;
  if(resumed)assert.ok(im.indexOf(resumed)>im.indexOf(paused)&&resumed.stopRequired===paused.stopRequired&&resumed.stopCompleted&&resumed.stopAck===paused.stopAck,'runtime_resume_invalid');
  return {sameOriginalModel:true,sameOriginalRuntime:true,modelStopRequired:paused.stopRequired,modelStopCompleted:paused.stopCompleted,modelStoppedAck:paused.stopAck,
    resumeCounts:{chat:chat.filter(row=>row.stage==='resumed').length,audio:audio.filter(row=>row.stage==='resumed').length}};
}
// createObserver 由宿主注入实际只读/诊断入口；缺证明返回未就绪，执行错误仅固定码。
function createObserver(original,adapter){validate(original);exact(adapter,['read','readWitness','traceKey']);assert.ok(typeof adapter.read==='function'&&typeof adapter.readWitness==='function','formal_observer_required');const {read:readEntry,readWitness,traceKey}=adapter;join(traceKey,original.tenantCode,'validation');const scope=Object.freeze({...original});let pinnedSource=null,pinnedOrigin=null,baseline=null,pinnedBinding;
  const read=async query=>{try{return await readEntry(scope,query);}catch{throw Error('readonly_observation_failed');}};
  return {observationWindow:Object.freeze({createdAfter:scope.createdAfter,createdBefore:scope.createdBefore}),
    async resolveSource(input){exact(input,['tenantCode','ownerUserId','subject','createdAfter','createdBefore','deviceEstablished','completedReplies']);for(const key of ['tenantCode','subject','createdAfter','createdBefore'])assert.ok(input[key]===scope[key],'scope_changed');assert.ok(input.ownerUserId===String(scope.ownerUserId)&&input.deviceEstablished===true,'scope_changed');let binding;const value=sourceProjection(await read(sourceQuery(scope,input.completedReplies)),input.completedReplies,actual=>{binding=actual;});
      if(value){if(pinnedSource){assert.ok(pinnedSource.sourceSessionId===value.sourceSessionId&&pinnedSource.expiresAt===value.expiresAt,'source_changed');assert.deepEqual(binding,pinnedBinding,'origin_changed');}else{assert.equal(input.completedReplies,1,'first_turn_required');pinnedSource=value;pinnedBinding=binding;}}return value;},
    async readTemporaryProof(input){exact(input,['sourceSessionId','subject','targetSubject','createdAfter','createdBefore']);for(const key of ['subject','targetSubject','createdAfter','createdBefore'])assert.ok(input[key]===scope[key],'scope_changed');assert.ok(pinnedSource&&input.sourceSessionId===pinnedSource.sourceSessionId,'source_unconfirmed');const value=proofProjection(await read(proofQuery(scope,input.sourceSessionId)));if(!value)return null;
      // 首次source(1)在拨号前已经冻结完整四字段，首human proof不能将当时已被替换的Hash当基线。
      const origin=JSON.stringify(value.origin);if(pinnedOrigin)assert.ok(origin===pinnedOrigin,'origin_changed');else pinnedOrigin=origin;
      const sameBinding=pinnedBinding===null?value.origin.length===15:!!pinnedBinding&&value.origin.length===16&&String(pinnedBinding.connectionEpoch)===value.origin[5]&&pinnedBinding.connectionInstance===value.origin[6]&&String(pinnedBinding.capabilityRevision)===value.origin[7]&&pinnedBinding.capabilityHash===value.origin[15];
      value.sameOriginal=value.sameOriginal&&sameBinding;
      let bytes;try{bytes=await readWitness(scope,{join:join(traceKey,scope.tenantCode,input.sourceSessionId)});}catch{throw Error('runtime_observation_failed');}
      const runtime=witnessProjection(bytes,join(traceKey,scope.tenantCode,input.sourceSessionId),value.restoreConsumed);if(!runtime)return null;
      if(!baseline){assert.ok(!value.restoreConsumed,'runtime_baseline_required');baseline=runtime.resumeCounts;}
      if(value.restoreConsumed&&(runtime.resumeCounts.chat<=baseline.chat||runtime.resumeCounts.audio<=baseline.audio))return null;
      if(value.stoppedIntent)assert.ok(runtime.resumeCounts.chat===baseline.chat&&runtime.resumeCounts.audio===baseline.audio,'stopped_runtime_resumed');
      const {origin:_,restoreConsumed:consumed,...sql}=value,{resumeCounts:counts,...facts}=runtime;return {...sql,...facts};}
  };}
module.exports={validate,sourceQuery,sourceProjection,proofQuery,proofProjection,join,witnessProjection,createObserver};
