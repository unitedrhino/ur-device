// 文件说明：合成MQTT有界编码的离线反例，不代替真实C++进程和Chromium停轨执行。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {mqttPacket,mqttText,capabilityReply,failureFacts}=require('./browser_cpp_ack_release_live.cjs');
test('MQTT剩余长度及UTF8前缀是真实协议编码，不把JSON伪装成裸TCP',()=>{
  assert.deepEqual(mqttPacket(0x20,Buffer.from([0,0])),Buffer.from([0x20,2,0,0]));
  assert.deepEqual(mqttPacket(0x30,Buffer.alloc(128)).subarray(0,3),Buffer.from([0x30,0x80,1]));
  assert.deepEqual(mqttText('中').subarray(0,2),Buffer.from([0,3]));
  assert.throws(()=>mqttPacket(0x30,Buffer.alloc(65537)));
});
test('失败事实只保留闭集媒体状态，不透传自由错误或设备身份',()=>{
  assert.deepEqual(failureFacts({phase:1,contextState:'running',terminated:false,trackCount:1,socketState:1,secret:'must-not-project'}),{phase:1,contextState:'running',terminated:false,trackCount:1,socketState:1});
  assert.deepEqual(failureFacts({phase:NaN,contextState:'private',terminated:'yes',trackCount:100,socketState:-1}),{phase:null,contextState:null,terminated:null,trackCount:null,socketState:null});
});
test('监听能力必须应用层确认原token和revision，不能只有MQTT PUBACK',()=>{
  assert.deepEqual(capabilityReply({method:'capabilityReport',msgToken:'original-capability',params:{revision:3}}),{method:'requestAck',msgToken:'original-capability',code:200,data:{requestMethod:'capabilityReport',result:{acceptedRevision:3}}});
  for(const revision of [0,-1,1.5,Number.MAX_SAFE_INTEGER+1,undefined])assert.throws(()=>capabilityReply({method:'capabilityReport',msgToken:'original-capability',params:{revision}}));
  assert.throws(()=>capabilityReply({method:'other',msgToken:'original-capability',params:{revision:3}}));
});
