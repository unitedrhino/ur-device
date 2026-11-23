// 文件说明：测试专用透明MQTT门铃观察器；原字节转发与有界原事件重投，不构造事件或平台回执。
'use strict';
const assert=require('node:assert/strict'),net=require('node:net'),{createHash}=require('node:crypto');
const {frameLength,maxPacket}=require('../tests/mqtt_execute_delay.cjs');

// eventMessage 只读取精确配置的物模型Topic，CONNECT及无关业务正文不进入观察账本。
function eventMessage(packet,topic){
  if((packet[0]>>4)!==3)return null;
  const qos=(packet[0]>>1)&3;assert.ok(qos<=2);
  let cursor=1;while(packet[cursor++]&128){}
  assert.ok(cursor+2<=packet.length);const length=packet.readUInt16BE(cursor);cursor+=2;
  assert.ok(length>0&&cursor+length+(qos?2:0)<=packet.length);
  const expected=Buffer.from(topic);
  if(!packet.subarray(cursor,cursor+length).equals(expected))return null;
  cursor+=length+(qos?2:0);assert.equal(packet[0]&1,0);assert.ok(packet.length-cursor<=65536);
  const body=packet.subarray(cursor),message=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(body));
  assert.ok(message&&typeof message==='object'&&!Array.isArray(message));
  assert.ok(typeof message.msgToken==='string'&&message.msgToken.length>0&&message.msgToken.length<=128);
  return {message,digest:createHash('sha256').update(body).digest('hex')};
}

// EventLedger 有界保存原事件关联；固定计数不输出设备标识、事件token、Topic或正文。
class EventLedger{
  constructor(event){this.event=event;this.records=new Map();this.failed=false;}
  observe(packet,topic,direction){
    const parsed=eventMessage(packet,topic);if(!parsed)return;
    const {message,digest}=parsed;
    if(direction==='up'){
      assert.equal(message.method,'eventPost');assert.equal(message.eventID,this.event);
      const old=this.records.get(message.msgToken);
      if(old){assert.equal(old.digest,digest);assert.ok(++old.duplicates<=8);return;}
      assert.ok(this.records.size<64);this.records.set(message.msgToken,{digest,duplicates:0,code:null,replies:0});
    }else{
      assert.equal(message.method,'eventReply');
      const old=this.records.get(message.msgToken);assert.ok(old,'original_event_required');
      assert.ok(Number.isSafeInteger(message.code)&&message.code>0&&message.code<=2147483647);
      if(old.code!==null)assert.equal(old.code,message.code);
      assert.ok(++old.replies<=9);old.code=message.code;
    }
  }
  facts(){const rows=[...this.records.values()];return {submitted:rows.length,replied:rows.filter(v=>v.code!==null).length,
    success:rows.filter(v=>v.code===200).length,rejected:rows.filter(v=>v.code!==null&&v.code!==200).length,
    duplicateReplies:rows.reduce((sum,v)=>sum+Math.max(0,v.replies-1),0),failed:this.failed};}
}

// createObserver 只允许原设备事件的两条回环路由；不读取鉴权、不跨连接重投，close等待实际socket退出。
async function createObserver({targetPort,product,device,event}){
  assert.ok(Number.isSafeInteger(targetPort)&&targetPort>=1024&&targetPort<=65535);
  for(const value of [product,device,event])assert.match(value,/^[A-Za-z0-9_-]{1,64}$/);
  const ledger=new EventLedger(event),sockets=new Set();let used=false,closing=false,closePromise,original,connection,replayed=false;
  const server=net.createServer(client=>{
    if(used||closing){ledger.failed=true;client.destroy();return;}used=true;
    const upstream=net.connect({host:'127.0.0.1',port:targetPort});sockets.add(client);sockets.add(upstream);
    connection=upstream;
    const fail=()=>{if(!closing)ledger.failed=true;client.destroy();upstream.destroy();};
    for(const socket of [client,upstream]){socket.on('error',fail);socket.once('close',()=>sockets.delete(socket));}
    client.once('close',()=>upstream.destroy());upstream.once('close',()=>client.destroy());
    // pipe原样处理背压；观察缓冲只读原字节，失败只能使本轮FAIL，不修改或补造平台回执。
    client.pipe(upstream);upstream.pipe(client);
    for(const [socket,direction]of [[client,'up'],[upstream,'down']]){
      let buffered=Buffer.alloc(0);const topic=`$thing/${direction}/event/${product}/${device}`;
      socket.on('data',chunk=>{try{assert.ok(buffered.length+chunk.length<=maxPacket);buffered=Buffer.concat([buffered,chunk]);
        for(;;){const length=frameLength(buffered);if(!length)break;
          const packet=buffered.subarray(0,length);ledger.observe(packet,topic,direction);
          if(direction==='up'){const parsed=eventMessage(packet,topic);
            if(parsed)original={packet:Buffer.from(packet),token:parsed.message.msgToken};}
          buffered=buffered.subarray(length);}
      }catch{fail();}});
    }
  });
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
  server.on('error',()=>{ledger.failed=true;});
  return {port:server.address().port,facts:()=>({...ledger.facts(),connectionsClosed:sockets.size===0}),
    // replayLatestOriginal 仅在原事件已收到成功回执后，同连接重发一次原字节；不改token、正文或MQTT头。
    replayLatestOriginal(){assert.ok(!closing&&!replayed&&connection&&!connection.destroyed&&original,'original_live_connection_required');
      assert.equal(ledger.records.get(original.token)?.code,200,'original_success_reply_required');
      replayed=true;ledger.observe(original.packet,`$thing/up/event/${product}/${device}`,'up');
      connection.write(original.packet);},
    close(){if(closePromise)return closePromise;closing=true;
      const stopped=[...sockets].map(socket=>new Promise(resolve=>socket.closed?resolve():socket.once('close',resolve)));
      const listener=new Promise(resolve=>server.close(resolve));for(const socket of sockets)socket.destroy();
      closePromise=Promise.all([listener,...stopped]).then(()=>{});return closePromise;}};
}
module.exports={eventMessage,EventLedger,createObserver};
