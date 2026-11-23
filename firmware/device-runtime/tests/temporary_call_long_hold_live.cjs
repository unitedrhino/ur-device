// 文件说明：显式长人际保持的原生语音验收；原180秒/WAV/双端媒体/恢复/释放合同不变，不发静音保活。
'use strict';
const assert=require('node:assert/strict'),{performance}=require('node:perf_hooks');
const native=require('./temporary_call_voice_live.cjs'),shared=require('./shared_handoff_live.cjs');
// holdMilliseconds固定大于原30秒空闲期限；不开放任意超时参数来绕过原驱动总预算。
const holdMilliseconds=35000;
// exactData只读取自身数据字段；访问器、额外字段或继承值不能冒充实际闭集观察。
function exactData(value,keys){assert.ok(value&&typeof value==='object'&&!Array.isArray(value),'provider_lifetime_invalid');const fields=Object.getOwnPropertyDescriptors(value);assert.equal(Reflect.ownKeys(fields).length,keys.length,'provider_lifetime_invalid');assert.ok(keys.every(key=>Object.hasOwn(fields,key)&&Object.hasOwn(fields[key],'value')),'provider_lifetime_invalid');return Object.fromEntries(keys.map(key=>[key,fields[key].value]));}
// verifyProvider只接受可信原来源join的唯一实例和有序事实；unknown/二次连接/替换实例/提前关闭均拒绝。
function verifyProvider(value,stage,generation){
 const data=exactData(value,['join','records']);assert.match(data.join,/^[a-f0-9]{64}$/,'provider_join_unknown');assert.ok(Array.isArray(data.records)&&data.records.length<=32,'provider_lifetime_invalid');
 assert.ok(['held','resumed'].includes(stage));const expected=stage==='held'?['connected','held']:['connected','held','resumed'];assert.equal(data.records.length,expected.length,'provider_lifetime_unconfirmed');
 let actual=null;for(let index=0;index<data.records.length;index++){const row=exactData(data.records[index],['kind','provider','stage','join','generation']);assert.equal(row.kind,'providerLifetime');assert.equal(row.provider,'doubao');assert.equal(row.join,data.join,'provider_scope_changed');assert.match(row.generation,/^[a-f0-9]{32}$/,'provider_generation_unknown');assert.equal(row.stage,expected[index],'provider_lifetime_unconfirmed');if(actual===null)actual=row.generation;assert.equal(row.generation,actual,'provider_instance_changed');}
 if(generation!==undefined)assert.equal(actual,generation,'provider_instance_changed');return actual;
}
// validateLongHold仅正常显式恢复variant可验同WS恢复；本人stop原负向仍走原独立驱动。
function validateLongHold(env,observer){assert.equal(env.UR_TEMPORARY_NATIVE_LONG_HOLD_LIVE,'1','long_hold_opt_in_required');assert.equal(env.UR_TEMPORARY_CALL_VARIANT,'resume','long_hold_resume_required');assert.ok(observer&&typeof observer.readProviderLifetime==='function','original_provider_observer_required');return holdMilliseconds;}
// holdVerified在原截止内持续核双端进程、连接、采集/播放和正式暂停证明；可控时钟仅供离线测试。
async function holdVerified(context,readProvider,clock={now:()=>performance.now(),sleep:ms=>new Promise(resolve=>setTimeout(resolve,ms))}){
 const started=clock.now();assert.ok(Number.isFinite(started)&&started+holdMilliseconds<context.deadline,'long_hold_budget_insufficient');context.guard();
 const generation=verifyProvider(await readProvider(context.sourceSessionId),'held');const initial=await context.readPair();assert.equal(initial.length,2);let last=initial;
 do{context.guard();assert.ok(clock.now()<context.deadline,'long_hold_deadline');await context.verifyHuman();const current=await context.readPair();assert.equal(current.length,2);assert.ok(current.every(x=>x.media.connected===true&&x.media.inputAttached===true&&x.media.outputEnabled===true&&x.media.held===false),'long_hold_human_media_closed');for(let i=0;i<2;i++){for(const key of ['session','peerEpoch','peerFingerprint'])assert.equal(current[i].media[key],initial[i].media[key],'long_hold_peer_changed');assert.ok(current[i].media.frames>=last[i].media.frames&&current[i].media.nonSilentFrames>=last[i].media.nonSilentFrames,'long_hold_media_counter_reset');}last=current;assert.ok(last[0].captureClosed===true&&last[0].activeReply===false,'long_hold_ai_input_not_paused');if(initial[0].voice)assert.equal(last[0].voice?.userStops,initial[0].voice.userStops,'long_hold_user_stop');verifyProvider(await readProvider(context.sourceSessionId),'held',generation);
   if(clock.now()-started>=holdMilliseconds)break;await clock.sleep(Math.min(1000,holdMilliseconds-(clock.now()-started)));
 }while(true);
 context.guard();assert.ok(clock.now()<context.deadline,'long_hold_deadline');shared.verifyAudioPair(initial.map(x=>x.media),last.map(x=>x.media));return {generation,elapsedMs:Math.floor(clock.now()-started)};
}
// run只装配原真实驱动的可选观察hook；不注入替代网络、Provider或假页面，finally仍归原驱动。
async function run(env=process.env,observer,options={}){
 validateLongHold(env,observer);assert.equal(options.holdOriginal,undefined,'long_hold_hook_override');assert.equal(options.verifyOriginalProvider,undefined,'long_hold_hook_override');let held=null,verified=false;
 const result=await native.run(env,observer,{...options,holdOriginal:async context=>{held=await holdVerified(context,id=>shared.budgetCall(()=>observer.readProviderLifetime({sourceSessionId:id}),context.deadline));},verifyOriginalProvider:async context=>{context.guard();assert.ok(held,'long_hold_not_observed');verifyProvider(await shared.budgetCall(()=>observer.readProviderLifetime({sourceSessionId:context.sourceSessionId}),context.deadline),'resumed',held.generation);verified=true;},report:report=>{const projected={...report,longHoldMs:held?.elapsedMs??0,sameOriginalProviderVerified:verified};if(options.report)options.report(projected);}});
 assert.ok(verified&&held?.elapsedMs>=holdMilliseconds,'long_hold_unconfirmed');return {...result,longHoldMs:held.elapsedMs,sameOriginalProviderVerified:true};
}
module.exports={holdMilliseconds,exactData,verifyProvider,validateLongHold,holdVerified,run};
// 不允许CLI从自由模块提供Provider事实；正式owned入口注入同一实际日志观察器。
if(require.main===module)run().catch(()=>{console.error(JSON.stringify({passed:false,reason:'native_long_hold_failed'}));process.exitCode=1;});
