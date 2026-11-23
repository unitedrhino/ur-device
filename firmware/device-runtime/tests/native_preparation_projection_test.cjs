// 文件说明：临时资源准备失败旁证的闭集与脱敏回归，不把原因标签解释为授权或重试许可。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {project, reasons} = require('./native_preparation_projection.cjs');
const marker = 'UR_NATIVE_TEMPORARY_PREPARATION_DIAGNOSTIC'; // 与后台独立marker固定匹配。

test('全部允许原因仅保留两个固定字段', () => {
    assert.equal(reasons.length, 17);
    for (const reason of reasons) assert.deepEqual(project(JSON.stringify({marker, reason})), {marker, reason});
});
test('原JSON日志只消费真实error正文，不接受其他级别', () => {
    const content = JSON.stringify({marker, reason: 'aiRetainWatermark'});
    assert.deepEqual(project(JSON.stringify({level: 'error', content, caller: 'private'})),
        {marker, reason: 'aiRetainWatermark'});
    for (const level of ['info', 'debug', null, 1]) assert.equal(project(JSON.stringify({level, content})), null);
});
test('额外身份与未知原因整条拒绝', () => {
    for (const value of [{marker, reason: 'private-token'}, {marker, reason: 'aiRetainPending', sessionId: 'private'},
        {marker: 'other', reason: 'aiRetainPending'}, {reason: 'aiRetainPending'}, []]) {
        assert.equal(project(JSON.stringify(value)), null);
    }
});
test('内外层重复键包括转义重名均拒绝', () => {
    for (const line of [`{"marker":"${marker}","reason":"aiRetainPending","reason":"humanLeaseInvalid"}`,
        `{"marker":"${marker}","reason":"aiRetainPending","\\u0072eason":"humanLeaseInvalid"}`,
        '{"level":"info","level":"error","content":"{}"}']) assert.equal(project(line), null);
});
test('不转换未知对象或执行代理陷阱', () => {
    let touched = false;
    const value = new Proxy({}, {get() {touched = true; throw Error('private');}});
    assert.equal(project(value), null);
    assert.equal(touched, false);
});
test('畸形、超长与自由前缀不进入报告', () => {
    const body = JSON.stringify({marker, reason: 'aiRetainPending'});
    for (const line of ['private ' + body, body + '{}', 'x'.repeat(16385), body.slice(0, -1), 'null']) {
        assert.equal(project(line), null);
    }
});
