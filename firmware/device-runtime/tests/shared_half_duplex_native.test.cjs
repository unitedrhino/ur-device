// 文件说明：半双工策略只在native启动读取；非法配置在身份签名/监听前拒绝，不借测试凭据连接平台。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),{spawnSync}=require('node:child_process');
const binary=process.env.TASK143_HANDOFF_BINARY; // CMake本轮正式目标；禁止静默skip或搜索旧产物。
assert.ok(binary,'缺少本轮Shared native二进制');
for(const [name,value,expected] of [['missing',undefined,'invalid_device_environment'],['zero','0','invalid_device_environment'],['one','1','invalid_device_environment'],['empty','','invalid_half_duplex_only_environment'],['word','true','invalid_half_duplex_only_environment'],['space',' 1','invalid_half_duplex_only_environment'],['two','2','invalid_half_duplex_only_environment']])test(name,()=>{
 const env={PATH:'/usr/bin:/bin'};if(value!==undefined)env.UR_DEVICE_HALF_DUPLEX_ONLY=value;
 const child=spawnSync(binary,['19998'],{env,encoding:'utf8',timeout:3000});
 assert.ifError(child.error);assert.equal(child.signal,null);assert.equal(child.status,2);assert.equal(child.stdout,'');assert.equal(child.stderr,expected+'\n');
});
