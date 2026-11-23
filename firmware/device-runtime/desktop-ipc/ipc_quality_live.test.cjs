// 文件说明：S19实际验收器的原计划关联、有限MQTT帧与脱敏回归；合成报文不算后端或实际媒体通过。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const {planObserver, frames, qualityButtonSelector, run} = require('./ipc_quality_live.cjs');

// fixture仅构造公开合成报文，覆盖同一原计划的五个阶段，不包含真实身份或认证材料。
function fixture(n = 1, track = 'audio-1', profile = 'low') {
  const token = `original-${n}`, report = `observed-${n}`, state = profile === 'audioOnly' ? 'paused' : 'active';
  const envelope = {sessionId:'synthetic-session'};
  return [
    ['up',{...envelope,msgToken:token,method:'mediaUpdate',params:{operation:'setQuality',expectedRevision:n-1,trackIds:[track],profile}}],
    ['down',{...envelope,msgToken:token,method:'mediaUpdated',code:200,data:{state:'pending',revision:n,mediaRevision:1,tracks:[{trackId:track,state:'active'}]}}],
    ['up',{...envelope,msgToken:report,method:'trackState',data:{requestToken:token,trackId:track,state,revision:n,mediaRevision:1}}],
    ['down',{...envelope,msgToken:report,method:'requestAck',code:200,data:{requestMethod:'trackState',result:{trackId:track,state,mediaRevision:1}}}],
    ['down',{...envelope,msgToken:`terminal-${n}`,method:'mediaUpdated',eventSeq:n+3,data:{requestToken:token,state:'applied',revision:n,mediaRevision:1,tracks:[{trackId:track,state}]}}]
  ];
}
// observeAll逐条提交真实取样器同样的方向/正文接口，不直接写内部通过状态。
function observeAll(o, rows) {for (const row of rows) o.observe(...row);}
test('八个原计划完整逐轨关联，终态与ACK任意顺序，重复帧不重复完成', () => {
  const o = planObserver();
  for (let n = 1; n <= 8; n++) {
    const track = n < 4 ? 'audio-1' : 'video-1', profile = n === 7 ? 'audioOnly' : 'standard';
    o.begin('synthetic-session',track,profile); const rows = fixture(n,track,profile);
    observeAll(o,rows.slice(0,3)); assert.equal(o.snapshot().completed,false);
    observeAll(o,[rows[4],rows[4]]); assert.equal(o.snapshot().completed,false);
    observeAll(o,[rows[3],...rows]); assert.equal(o.snapshot().completed,true);
    assert.equal(o.snapshot().revision,n); assert.equal(o.snapshot().error,false);
  }
  assert.equal(o.snapshot().records.length,8); assert.throws(() => o.begin('synthetic-session','audio-1','low'));
});
test('乱腿和无关ACK不完成计划；新计划内原字节迟到DUP保持幂等', () => {
  const o=planObserver(), rows=fixture();o.begin('synthetic-session','audio-1','low');
  observeAll(o,rows.map(([d,m])=>[d,{...m,sessionId:'other-session'}]));assert.equal(o.snapshot().revision,0);
  o.observe('down',{method:'requestAck',sessionId:'synthetic-session',msgToken:'unrelated',code:200,data:{requestMethod:'mediaState'}});
  observeAll(o,rows);o.begin('synthetic-session','audio-1','low');observeAll(o,rows);
  assert.equal(o.snapshot().error,false);assert.equal(o.snapshot().completed,false);observeAll(o,fixture(2));assert.equal(o.snapshot().revision,2);
});
test('失败或错误关联不冒充质量成功', () => {
  for (const [index,mutate] of [
    [0,m=>{m.code=null;}], [0,m=>{m.params.expectedRevision=1;}], [0,m=>{m.params.trackIds=['video-1'];}],
    [0,m=>{m.params.profile='high';}], [0,m=>{m.params.consentIds=[];}],
    [1,m=>{m.msgToken='another';}], [1,m=>{m.code=409;}], [1,m=>{m.data.state='applied';}],
    [2,m=>{m.code=200;}], [2,m=>{m.data.revision=2;}], [2,m=>{m.data.mediaRevision=2;}], [2,m=>{m.data.state='failed';}],
    [3,m=>{m.msgToken='another';}], [3,m=>{m.data.result.trackId='video-1';}], [3,m=>{m.data.result.state='paused';}],
    [4,m=>{m.data.requestToken='another';}], [4,m=>{m.data.state='failed';}], [4,m=>{m.eventSeq=0;}],
    [4,m=>{m.data.tracks[0].state='paused';}]
  ]) {
    const o=planObserver(), rows=fixture();o.begin('synthetic-session','audio-1','low');mutate(rows[index][1]);observeAll(o,rows);
    assert.equal(o.snapshot().error,true);assert.equal(o.snapshot().completed,false);
  }
});
test('未受理不能报实态，修改同token原字节以及超预算均拒绝', () => {
  for (const index of [1,2,3,4]) {const o=planObserver();o.begin('synthetic-session','audio-1','low');o.observe(...fixture()[index]);assert.equal(o.snapshot().error,true);}
  const o=planObserver(), rows=fixture();o.begin('synthetic-session','audio-1','low');o.observe(...rows[0]);
  assert.throws(()=>o.begin('synthetic-session','audio-1','low'));rows[0][1].params.profile='high';o.observe(...rows[0]);assert.equal(o.snapshot().error,true);
});
test('安全快照不包含原身份或请求token，调用者不能改内部记录', () => {
  const o=planObserver();o.begin('synthetic-session','audio-1','low');observeAll(o,fixture());
  const s=o.snapshot();assert.equal(/synthetic-session|original-|observed-|terminal-/.test(JSON.stringify(s)),false);
  s.records[0].profile='changed';assert.equal(o.snapshot().records[0].profile,'low');
});
test('同腿后继旧轨控制code回执不混入已完成质量计划，携质量关联的伪code仍失败', () => {
  const o=planObserver();o.begin('synthetic-session','audio-1','low');observeAll(o,fixture());
  o.observe('up',{sessionId:'synthetic-session',method:'trackState',msgToken:'old-control',code:200,
    data:{trackId:'video-1',state:'paused',mediaRevision:1}});
  assert.equal(o.snapshot().error,false);assert.equal(o.snapshot().revision,1);
  const p=planObserver(), rows=fixture();p.begin('synthetic-session','audio-1','low');observeAll(p,rows.slice(0,2));
  rows[2][1].code=200;p.observe(...rows[2]);assert.equal(p.snapshot().error,true);
});
test('终态共享水位不能回退或复用，同token pending不能携别的请求关联', () => {
  const o=planObserver();o.begin('synthetic-session','audio-1','low');observeAll(o,fixture());
  o.begin('synthetic-session','audio-1','low');const rows=fixture(2);rows[4][1].eventSeq=4;observeAll(o,rows);assert.equal(o.snapshot().error,true);
  const p=planObserver();p.begin('synthetic-session','audio-1','low');const bad=fixture();bad[1][1].data.requestToken='another';observeAll(p,bad);
  assert.equal(p.snapshot().error,true);
});
test('真实点击定位只接受原DOM有限序号，不生成文本伪类或允许注入', () => {
  assert.equal(qualityButtonSelector(2,3),'#views > section:nth-child(2) > button:nth-of-type(3)');
  for(const pair of [[0,1],[4,1],[1,0],[1,9],[1.5,2],['2',1],[1,'1);click'],[null,1]])assert.throws(()=>qualityButtonSelector(...pair));
});

// packet编码有限MQTT QoS1合成帧，用于TCP拆包/粘包回归；不连接Broker或写任何实际消息。
function packet(topic, body, flags = 0x32) {
  const t=Buffer.from(topic),b=Buffer.from(typeof body==='string'?body:JSON.stringify(body)),data=Buffer.alloc(4+t.length+b.length);
  data.writeUInt16BE(t.length);t.copy(data,2);data.writeUInt16BE(1,t.length+2);b.copy(data,t.length+4);
  const size=[];let n=data.length;do{let v=n%128;n=Math.floor(n/128);if(n)v|=128;size.push(v);}while(n);
  return Buffer.concat([Buffer.from([flags,...size]),data]);
}
test('MQTT逐字节拆包/多帧粘包只读原字节，忽略其它Topic和控制帧', () => {
  const rows=[], topic='$thing/up/signaling/example/device', p=packet(topic,fixture()[0][1]),copy=Buffer.from(p);
  const take=frames('up',topic,(...row)=>rows.push(row),()=>assert.fail('unexpected_frame_failure'));
  for(const byte of p)take(Buffer.from([byte]));take(Buffer.concat([Buffer.from([0xd0,0]),packet('other',{ignored:true}),p]));
  assert.equal(rows.length,2);assert.equal(rows[0][0],'up');assert.deepEqual(rows[0][1],fixture()[0][1]);assert.deepEqual(p,copy);
});
test('MQTT重复JSON键/非法UTF8/畸形长度/QoS/取样预算明确失败且不再取样', () => {
  const topic='$thing/up/signaling/example/device';
  for(const input of [packet(topic,'{"a":1,"a":2}'),packet(topic,Buffer.from([0xff]).toString('latin1')).map((v,i,a)=>i===a.length-2?0xff:v),
    Buffer.from([0x30,0xff,0xff,0xff,0xff]),Buffer.from([0x30,0xff,0xff,0x7f]),Buffer.from([0x32,1,0]),packet(topic,{},0x36),Buffer.alloc(131073)]) {
    let failed=0,seen=0;const take=frames('up',topic,()=>seen++,()=>failed++);take(input);take(packet(topic,{}));
    assert.equal(failed,1);assert.equal(seen,0);
  }
  let failed=0,seen=0;const take=frames('up',topic,()=>seen++,()=>failed++);for(let n=0;n<257;n++)take(packet(topic,{}));
  assert.equal(seen,256);assert.equal(failed,1);
});
test('真实质量验收缺少显式开关时不创建代理或设备', async () => {
  const original=process.env.UR_IPC_QUALITY_LIVE;delete process.env.UR_IPC_QUALITY_LIVE;
  try {await assert.rejects(run(),/explicit_quality_live_required/);}finally{if(original!==undefined)process.env.UR_IPC_QUALITY_LIVE=original;}
});
