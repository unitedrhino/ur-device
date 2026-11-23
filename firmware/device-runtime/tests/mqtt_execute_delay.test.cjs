// 文件说明：MQTT延迟桥的有界解析与实际回环网络单测；合成报文不作为真实模型或设备验收。
'use strict';
const test = require('node:test'),assert = require('node:assert/strict'),net = require('node:net');
const {once} = require('node:events');
const {frameLength,executeIdentity,createExecuteDelay,maxPacket} = require('./mqtt_execute_delay.cjs');
const topic = '$thing/down/message/synthetic-product/synthetic-device';
// publish 只组装离线合成PUBLISH，实际E2E不得调用此函数生成授权或平台消息。
function publish(value, flags = 0x30, actualTopic = topic) {
  const name = Buffer.from(actualTopic),payload = Buffer.from(JSON.stringify(value));
  const body = Buffer.alloc(2 + name.length + ((flags & 6) ? 2 : 0) + payload.length);
  body.writeUInt16BE(name.length);name.copy(body,2);payload.copy(body,body.length - payload.length);
  const length = [];let size = body.length;
  do {let digit = size % 128;size = Math.floor(size / 128);if(size) digit |= 128;length.push(digit);} while(size);
  return Buffer.concat([Buffer.from([flags,...length]),body]);
}
// execute 是独立单测身份，不携带真实令牌或设备凭据。
function execute() {
  return {method:'toolCallExecute',msgToken:'synthetic-token',sessionId:'synthetic-session',messageSeq:3,
    data:{name:'set_local_display_theme',respId:'synthetic-response',toolCallId:'synthetic-call'}};
}
test('逐字节拆帧、可变长度、多个包和非法预算均有明确结果', () => {
  const packet = publish(execute());
  for(let index = 0;index < packet.length;index++) assert.equal(frameLength(packet.subarray(0,index)),0);
  assert.equal(frameLength(packet),packet.length);
  assert.equal(frameLength(Buffer.concat([packet,packet])),packet.length);
  assert.throws(() => frameLength(Buffer.from([0x30,0xff,0xff,0xff,0xff])));
  assert.throws(() => frameLength(Buffer.from([0x30,0xff,0xff,0x7f])));
  assert.equal(maxPacket,1048576);
});
test('只识别原设备Topic和审核工具，保留原身份且不接受损坏PUBLISH', () => {
  const identity = executeIdentity(publish(execute()),topic);
  assert.equal(identity.response,'synthetic-response');
  assert.equal(executeIdentity(publish(execute(),0x31),topic),null,'retained不能冒充本次原工具');
  assert.equal(executeIdentity(publish(execute(),0x30,topic+'/other'),topic),null);
  assert.equal(executeIdentity(publish({...execute(),method:'respDone'}),topic),null);
  assert.equal(executeIdentity(Buffer.from([0xd0,0]),topic),null);
  assert.throws(() => executeIdentity(publish(execute(),0x34),topic));
  assert.throws(() => executeIdentity(Buffer.from([0x30,1,0]),topic));
  assert.throws(() => executeIdentity(publish({...execute(),sessionId:null}),topic));
});
// eventually 只为实际socket到达/关闭设独立有界等待，时间本身不构成通过事实。
async function eventually(predicate) {
  const until = Date.now() + 2000;
  while(!predicate()) {assert.ok(Date.now() < until,'socket_fact_timeout');await new Promise(resolve => setTimeout(resolve,5));}
}
test('实际回环只延迟原Execute，取消帧不阻塞，原scope才能原字节放行且清理完整', async t => {
  let source,received = Buffer.alloc(0);
  const broker = net.createServer(socket => {source = socket;socket.on('data',bytes => {received = Buffer.concat([received,bytes]);});});
  broker.listen(0,'127.0.0.1');await once(broker,'listening');
  t.after(async() => {if(source) source.destroy();await new Promise(resolve => broker.close(resolve));});
  const proxy = await createExecuteDelay({targetPort:broker.address().port,topic});
  t.after(() => proxy.close());
  const device = net.connect({host:'127.0.0.1',port:proxy.port});t.after(() => device.destroy());
  let downstream = Buffer.alloc(0);device.on('data',bytes => {downstream = Buffer.concat([downstream,bytes]);});
  await once(device,'connect');await eventually(() => !!source);
  const upstream = Buffer.from([0xc0,0]);device.write(upstream);
  await eventually(() => received.length === upstream.length);assert.deepEqual(received,upstream);
  const original = publish(execute()),cancelled = publish({method:'respDone',data:{status:'cancelled'}});
  source.write(original.subarray(0,3));source.write(Buffer.concat([original.subarray(3),cancelled]));
  await eventually(() => proxy.facts().held && downstream.length === cancelled.length);
  assert.deepEqual(downstream,cancelled);
  assert.throws(() => proxy.release('wrong-session','synthetic-response'));
  assert.throws(() => proxy.release('synthetic-session','wrong-response'));
  assert.equal(proxy.release('synthetic-session','synthetic-response'),true);
  await eventually(() => downstream.length === cancelled.length + original.length);
  assert.deepEqual(downstream,Buffer.concat([cancelled,original]));
  assert.throws(() => proxy.release('synthetic-session','synthetic-response'));
  await Promise.all([proxy.close(),proxy.close()]);
  assert.deepEqual(proxy.facts(),{held:true,releasedOriginal:true,failed:false,duplicates:0,forwarded:1,connectionsClosed:true});
  assert.equal(proxy.identity(),null,'清理后不能继续读取原scope或跨连接恢复授权');
});
test('只准回环固定目标和精确Topic，未知环境不启动监听', async() => {
  for(const value of [{targetPort:0,topic},{targetPort:1883,topic:topic+'/#'},
    {targetPort:1883,topic:'$thing/up/message/p/d'}]) await assert.rejects(createExecuteDelay(value));
});

// delayFixture 只为故障回归创建独立回环端点；测试结束按精确对象回收，不复用真实设备连接。
async function delayFixture(t) {
  let source;
  const broker = net.createServer(socket => {source = socket;});
  broker.listen(0,'127.0.0.1');await once(broker,'listening');
  t.after(async() => {if(source) source.destroy();await new Promise(resolve => broker.close(resolve));});
  const proxy = await createExecuteDelay({targetPort:broker.address().port,topic});
  t.after(() => proxy.close());
  const device = net.connect({host:'127.0.0.1',port:proxy.port});t.after(() => device.destroy());
  let received = Buffer.alloc(0);
  device.on('data',bytes => {received = Buffer.concat([received,bytes]);});
  await once(device,'connect');await eventually(() => !!source);
  return {proxy,device,source,received:() => received};
}
test('原Execute重复只保存一次，超过预算或同token改正文均失败关闭', async t => {
  for(const changed of [false,true]) await t.test(changed ? '同token变更' : '重复预算',async t => {
    const {proxy,source,received} = await delayFixture(t);
    source.write(publish(execute()));await eventually(() => proxy.facts().held);
    if(changed) source.write(publish({...execute(),messageSeq:4}));
    else source.write(Buffer.concat(Array.from({length:9},() => publish(execute()))));
    await eventually(() => proxy.facts().failed && proxy.facts().connectionsClosed);
    assert.equal(received().length,0,'不能在错误路径投递待取消的Execute');
    assert.throws(() => proxy.release('synthetic-session','synthetic-response'));
  });
});
test('持有原Execute后断连或关闭均不补投，不残留可用授权', async t => {
  for(const disconnected of [false,true]) await t.test(disconnected ? '原端断连' : '主动关闭',async t => {
    const {proxy,source,device,received} = await delayFixture(t);
    source.write(publish(execute()));await eventually(() => proxy.facts().held);
    if(disconnected) {device.destroy();await eventually(() => proxy.facts().connectionsClosed);}
    else await proxy.close();
    assert.equal(received().length,0);
    assert.throws(() => proxy.release('synthetic-session','synthetic-response'));
    await proxy.close();assert.equal(proxy.identity(),null);
    assert.equal(proxy.facts().releasedOriginal,false);
  });
});
test('下行畸形长度主动失败并释放双向连接', async t => {
  const {proxy,source,received} = await delayFixture(t);
  source.write(Buffer.from([0x30,0xff,0xff,0xff,0xff]));
  await eventually(() => proxy.facts().failed && proxy.facts().connectionsClosed);
  assert.equal(received().length,0);
});
