// 文件说明：执行真实C++进程的目标恢复装配拒绝路径；不联网、不提供凭据、不以网页替代HAL。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
const binary = process.env.UR_DEVICE_BROWSER_TEST_BINARY;
// invalid 各例只提供安全开关，启动必须在签名、监听和能力报告之前拒绝。
for (const [name, values] of [
  ['非法零值', {UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC: '0'}],
  ['非法布尔正文', {UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC: 'true'}],
  ['缺少本人接听候选HAL', {UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC: '1', UR_DEVICE_CONTROL_SYNC: '1'}],
  ['缺少控制恢复能力', {UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC: '1', UR_DEVICE_AI_ENDPOINT_LISTEN: '1'}],
  ['None没有完整音频HAL', {UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC: '1', UR_DEVICE_AI_ENDPOINT_LISTEN: '1',
    UR_DEVICE_CONTROL_SYNC: '1', UR_MEDIA_TRANSPORT: 'none'}],
]) {
  test(name, {skip: !binary}, () => {
    const child = spawnSync(binary, ['19998'], {env: {PATH: process.env.PATH, ...values},
      encoding: 'utf8', timeout: 3000});
    assert.ifError(child.error);
    assert.equal(child.status, 2);
    assert.equal(child.stdout, '');
    assert.equal(child.stderr, 'invalid_ai_endpoint_control_sync_configuration\n');
  });
}
