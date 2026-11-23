// 文件说明：真正Shared native入口严格开关，固定无凭据环境，合法值不冒称MQTT广告/硬件验证。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
const binary=process.env.TASK143_HANDOFF_BINARY; // 必须由CMake本轮目标装配，不能skip或找旧二进制。
assert.ok(binary,'缺少本轮Shared native二进制');
// 闭集配置只检查签名/监听前原拒绝边界；不继承身份或真实模型环境。
for(const [name,value,human,expected] of [
 ['default','',false,'invalid_device_environment'],
 ['zero','0',false,'invalid_device_environment'],
 ['enabled','1',false,'invalid_device_environment'],
 ['boolean','true',false,'invalid_udp_reply_start_receipt_environment'],
 ['two','2',false,'invalid_udp_reply_start_receipt_environment'],
 ['human-default','',true,'invalid_device_environment'],
 ['human-zero','0',true,'invalid_device_environment'],
 ['human-one','1',true,'invalid_udp_reply_start_receipt_environment'],
]) test(name,()=>{
 const child=spawnSync(binary,['19998',...(human?['--human']:[])],{
  env:{PATH:'/usr/bin:/bin',UR_DEVICE_UDP_REPLY_START_RECEIPT:value},encoding:'utf8',timeout:3000});
 assert.ifError(child.error);assert.equal(child.signal,null);assert.equal(child.status,2);
 assert.equal(child.stdout,'');assert.equal(child.stderr,expected+'\n');
});
