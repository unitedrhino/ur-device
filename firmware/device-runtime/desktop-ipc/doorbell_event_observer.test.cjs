// 文件说明：透明门铃观察器的离线报文及回环传输单测；模拟对端不是正式Broker或后端验收。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),net=require('node:net');
const {eventMessage,EventLedger,createObserver}=require('./doorbell_event_observer.cjs');
const up='$thing/up/event/p/d',down='$thing/down/event/p/d';
// packet 构造纯离线MQTT夹具；不用于真实设备或平台链路。
function packet(topic,body){const route=Buffer.from(topic),payload=Buffer.from(JSON.stringify(body));
  const length=Buffer.alloc(2);length.writeUInt16BE(route.length);let size=2+route.length+payload.length,remaining=[];
  do{let byte=size%128;size=Math.floor(size/128);if(size)byte|=128;remaining.push(byte);}while(size);
  return Buffer.concat([Buffer.from([0x30,...remaining]),length,route,payload]);}
const request={method:'eventPost',msgToken:'original',eventID:'Doorbell',timestamp:1,params:{}};
const reply={method:'eventReply',msgToken:'original',code:200};

test('只关联原事件和原回执，重复不累计，非成功ACK与超时明确分开',()=>{
  const ledger=new EventLedger('Doorbell');ledger.observe(packet(up,request),up,'up');
  assert.deepEqual(ledger.facts(),{submitted:1,replied:0,success:0,rejected:0,duplicateReplies:0,failed:false});
  ledger.observe(packet(up,request),up,'up');ledger.observe(packet(down,{...reply,code:100000}),down,'down');
  ledger.observe(packet(down,{...reply,code:100000}),down,'down');
  assert.deepEqual(ledger.facts(),{submitted:1,replied:1,success:0,rejected:1,duplicateReplies:1,failed:false});
  assert.throws(()=>ledger.observe(packet(down,reply),down,'down'));
});

test('无关Topic与CONNECT不解析正文；错token、变参、错事件和有界重复均拒绝',()=>{
  assert.equal(eventMessage(Buffer.from([0x10,0]),up),null);
  assert.equal(eventMessage(packet('$thing/up/action/p/d',{}),up),null);
  const ledger=new EventLedger('Doorbell');assert.throws(()=>ledger.observe(packet(down,reply),down,'down'));
  assert.throws(()=>ledger.observe(packet(up,{...request,eventID:'other'}),up,'up'));
  ledger.observe(packet(up,request),up,'up');
  assert.throws(()=>ledger.observe(packet(up,{...request,timestamp:2}),up,'up'));
  for(let count=0;count<8;count++)ledger.observe(packet(up,request),up,'up');
  assert.throws(()=>ledger.observe(packet(up,request),up,'up'));
});

test('实际回环双向字节透明、拆帧可观察、close回收所有socket且可重复',async()=>{
  const original=packet(up,request),answer=packet(down,reply);let serverSocket,packets=0;
  const target=net.createServer(socket=>{serverSocket=socket;let bytes=Buffer.alloc(0);
    socket.on('data',chunk=>{bytes=Buffer.concat([bytes,chunk]);if(bytes.length===original.length){assert.deepEqual(bytes,original);bytes=Buffer.alloc(0);packets++;socket.write(answer);}});});
  await new Promise(resolve=>target.listen(0,'127.0.0.1',resolve));
  const observer=await createObserver({targetPort:target.address().port,product:'p',device:'d',event:'Doorbell'});
  const client=net.connect({host:'127.0.0.1',port:observer.port});client.on('error',()=>{});
  try{
    assert.throws(()=>observer.replayLatestOriginal());
    const received=new Promise((resolve,reject)=>{let bytes=Buffer.alloc(0);const timer=setTimeout(()=>reject(Error('loopback_timeout')),3000);
      client.on('data',chunk=>{bytes=Buffer.concat([bytes,chunk]);if(bytes.length===answer.length){clearTimeout(timer);resolve(bytes);}});});
    await new Promise(resolve=>client.once('connect',resolve));client.write(original.subarray(0,2));client.write(original.subarray(2));
    assert.deepEqual(await received,answer);assert.equal(observer.facts().success,1);assert.equal(observer.facts().failed,false);
    const duplicate=new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('replay_timeout')),3000);
      client.once('data',bytes=>{clearTimeout(timer);resolve(bytes);});});
    observer.replayLatestOriginal();assert.throws(()=>observer.replayLatestOriginal());
    assert.deepEqual(await duplicate,answer);assert.equal(packets,2);assert.equal(observer.facts().submitted,1);
    assert.equal(observer.facts().duplicateReplies,1);
    await observer.close();await observer.close();assert.equal(observer.facts().connectionsClosed,true);
    assert.throws(()=>observer.replayLatestOriginal());
  }finally{client.destroy();serverSocket?.destroy();await observer.close();await new Promise(resolve=>target.close(resolve));}
});

test('原事件尚无回执或被明确拒绝时不得重投',async()=>{
  for(const code of [null,100000]){
    let serverSocket,seen;const observed=new Promise(resolve=>{seen=resolve;});
    const target=net.createServer(socket=>{serverSocket=socket;socket.once('data',()=>{
      if(code!==null)socket.write(packet(down,{...reply,code}));seen();});});
    await new Promise(resolve=>target.listen(0,'127.0.0.1',resolve));
    const observer=await createObserver({targetPort:target.address().port,product:'p',device:'d',event:'Doorbell'});
    const client=net.connect({host:'127.0.0.1',port:observer.port});client.on('error',()=>{});
    try{
      const answered=code===null?Promise.resolve():new Promise(resolve=>client.once('data',resolve));
      await new Promise(resolve=>client.once('connect',resolve));client.write(packet(up,request));
      await observed;await answered;assert.equal(observer.facts().submitted,1);
      assert.throws(()=>observer.replayLatestOriginal(),/original_success_reply_required/);
      assert.equal(observer.facts().duplicateReplies,0);
    }finally{client.destroy();serverSocket?.destroy();await observer.close();await new Promise(resolve=>target.close(resolve));}
  }
});
