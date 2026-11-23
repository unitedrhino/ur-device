// 文件说明：以原页面release和原readFacts复现终态capture指标与过期验证的契约冲突；无网络或模型。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../../..'); // root仅定位当前worktree原验证叶。
test('原页面release的null保留且v3仅根据真实终态停止证明通过',async()=>{
 const elements=new Map(),page=vm.createContext({TextEncoder,document:{getElementById(id){if(!elements.has(id))elements.set(id,{addEventListener(){}});return elements.get(id);}},window:{addEventListener(){}}});
 vm.runInContext(fs.readFileSync(path.join(root,'firmware/device-runtime/adapters/desktop/web/app.js'),'utf8'),page);
 vm.runInContext(`globalThis.__urAIControl={starts:1,stops:0,configurations:0,fixtureEnded:0,recognized:1,keywords:[],recognizedText:''};
 const originalTrack={readyState:'live',stop(){this.readyState='ended';}};
 stream={getTracks:()=>[originalTrack],getAudioTracks:()=>[originalTrack]};
 context={state:'running',close(){this.state='closed';return Promise.resolve();}};
 metrics.captureOpen=false;metrics.completedResponses=1;
 release('fixed');`,page);
 const control=require(path.join(root,'firmware/device-runtime/tests/browser_ai_control_live.cjs'));
 const after=await vm.runInContext('('+control.readFacts.toString()+')()',page);
 Object.assign(after,vm.runInContext('({tracksEnded:stream.getAudioTracks().every(x=>x.readyState==="ended"),contextClosed:context.state==="closed",focusReceipts:[{receiptIndex:1,revision:1,operation:"acquire",state:"acquired",code:200}]})',page));
 assert.equal(after.captureOpen,null);assert.equal(after.terminated,true);assert.equal(after.tracksEnded,true);assert.equal(after.contextClosed,true);
 const before={captureOpen:true,starts:1,recognized:1,completed:1},stopped={expired:true,actualCaptureClosed:true,actualOutputStopped:true,elapsed:15001,reconnectAfterExpiryRejected:true};
 require('./browser_ai_endpoint_target_recovery_live.cjs').verifyExpired(before,stopped,after);
});

// expiredFixture只提供完整合法终态事实，关键反例逐项破坏而不放宽原到期或媒体条件。
function expiredFixture(){return {before:{captureOpen:true,starts:1,recognized:1,completed:1},stopped:{expired:true,actualCaptureClosed:true,actualOutputStopped:true,elapsed:15000,reconnectAfterExpiryRejected:true},after:{captureOpen:null,outputStopped:true,terminated:true,tracksEnded:true,contextClosed:true,starts:1,recognized:1,completed:1,focusReceipts:[{receiptIndex:1,revision:1,operation:'acquire',state:'acquired',code:200}]}};}
// invalidFacts覆盖非法采集意图、实际停轨缺失、计数变化、唯一授权和原15至20秒边界。
const invalidFacts=[['before_capture',x=>x.before.captureOpen=false],['expired',x=>x.stopped.expired=false],['stopped_capture',x=>x.stopped.actualCaptureClosed=false],['stopped_output',x=>x.stopped.actualOutputStopped=false],['early',x=>x.stopped.elapsed=14999],['late',x=>x.stopped.elapsed=20000],['elapsed_unknown',x=>x.stopped.elapsed=NaN],['reconnect',x=>x.stopped.reconnectAfterExpiryRejected=false],['capture_true',x=>x.after.captureOpen=true],['capture_missing',x=>delete x.after.captureOpen],['capture_type',x=>x.after.captureOpen=0],['output',x=>x.after.outputStopped=false],['terminated',x=>x.after.terminated=false],['tracks',x=>x.after.tracksEnded=false],['context',x=>x.after.contextClosed=false],['starts',x=>x.after.starts++],['recognized',x=>x.after.recognized++],['completed',x=>x.after.completed++],['acquire',x=>x.after.focusReceipts=[]]];
for(const [name,mutate]of invalidFacts)test('终态过期验证仍严格拒绝 '+name,()=>{const x=expiredFixture();mutate(x);assert.throws(()=>require('./browser_ai_endpoint_target_recovery_live.cjs').verifyExpired(x.before,x.stopped,x.after),assert.AssertionError);});
test('原合法false与null只在真实停止全部成立且原期限内通过',()=>{for(const captureOpen of [false,null])for(const elapsed of [15000,19999]){const x=expiredFixture();x.after.captureOpen=captureOpen;x.stopped.elapsed=elapsed;const out=require('./browser_ai_endpoint_target_recovery_live.cjs').verifyExpired(x.before,x.stopped,x.after);assert.equal(out.actualCaptureStopped,true);assert.equal(out.actualOutputStopped,true);assert.equal(out.expiredMilliseconds,elapsed);assert.equal(x.after.captureOpen,captureOpen);}});
