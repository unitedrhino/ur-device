// 文件说明：UDP首因只读投影的拆包、严格契约、有界终态及隐私回归；不启动网络或证明实际播放。
'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const {limits, reasons, project, create} = require('./udp_socket_failure_projection.cjs');
const env = {UR_DEVICE_CONTEXT_DIAGNOSTIC: '1'};
// wire 只生成正式两字段行，不补身份、地址或系统错误正文。
const wire = reason => JSON.stringify({kind: 'udpSocketReceiveFailureDiagnostic', reason});
// observe 由测试显式提交Buffer，返回只读事实，不模拟子进程或实际UDP。
function observe(reason) {
    const view = create(env);
    view.push(Buffer.from(wire(reason) + '\n'));
    return view;
}

test('所有闭集合reason唯一可投影，未知字符串及额外/缺字段拒绝', () => {
    for (const reason of reasons) assert.deepEqual(project(wire(reason)),
        {kind: 'udpSocketReceiveFailureDiagnostic', reason});
    for (const value of ['timeout', '', null, 0, {}, []]) assert.equal(project(wire(value)), null);
    for (const line of ['{}', 'null', '[]', '{', wire('peerRefused') + 'junk',
        '{"kind":"udpSocketReceiveFailureDiagnostic"}',
        '{"kind":"other","reason":"peerRefused"}',
        '{"kind":"udpSocketReceiveFailureDiagnostic","reason":"peerRefused","address":"private"}'])
        assert.equal(project(line), null);
});

test('重复及转义同名字段拒绝，不能last-key-wins或提升包装', () => {
    for (const line of [
        '{"kind":"udpSocketReceiveFailureDiagnostic","reason":"peerRefused","reason":"unknown"}',
        '{"kind":"udpSocketReceiveFailureDiagnostic","reason":"peerRefused","\\u0072eason":"peerRefused"}',
        JSON.stringify({content: wire('peerRefused')}), 'prefix ' + wire('peerRefused')])
        assert.equal(project(line), null);
});

test('默认关闭，环境只精确1开启，不自动处理父环境', () => {
    for (const config of [undefined, {}, {UR_DEVICE_CONTEXT_DIAGNOSTIC: true},
        {UR_DEVICE_CONTEXT_DIAGNOSTIC: 'true'}, {UR_DEVICE_CONTEXT_DIAGNOSTIC: '0'}]) {
        const view = create(config); view.push(Buffer.from(wire('peerRefused') + '\n'));
        assert.equal(view.statistics().enabled, false); assert.equal(view.statistics().observed, false);
    }
});

test('逐字节拆包、CRLF和实际end尾行均来自同一原行', () => {
    for (const ending of ['\n', '\r\n', '']) {
        const view = create(env), bytes = Buffer.from(wire('peerRefused') + ending);
        const original = Buffer.from(bytes);
        for (const byte of bytes) view.push(Buffer.from([byte]));
        if (!ending) assert.equal(view.statistics().observed, false);
        view.end();
        assert.equal(view.statistics().reason, 'peerRefused'); assert.equal(view.statistics().ended, true);
        assert.deepEqual(bytes, original);
    }
});

test('未观察不是没有失败，unknown与实际ended独立', () => {
    const missing = create(env); missing.end();
    assert.equal(missing.statistics().observed, false); assert.equal(missing.statistics().reason, null);
    assert.equal(missing.statistics().unknown, false); assert.equal(missing.statistics().ended, true);
    const explicit = observe('unknown');
    assert.equal(explicit.statistics().observed, true); assert.equal(explicit.statistics().unknown, true);
    assert.equal(explicit.statistics().ended, false); assert.equal(explicit.statistics().reason, 'unknown');
});

test('重复同首因与矛盾首因不覆盖原事实，并拒绝可靠reason投影', () => {
    for (const second of ['peerRefused', 'peerReset']) {
        const view = observe('peerRefused'); view.push(Buffer.from(wire(second) + '\n'));
        const facts = view.statistics();
        assert.equal(facts.observed, true); assert.equal(facts.reason, null); assert.equal(facts.consistent, false);
        assert.equal(facts.duplicates, second === 'peerRefused' ? 1 : 0);
        assert.equal(facts.contradictions, second === 'peerReset' ? 1 : 0);
    }
});

test('畸形UTF8整行丢弃，换行后独立恢复，不接替换字符', () => {
    const view = create(env);
    view.push(Buffer.from([0xff])); view.push(Buffer.from(wire('peerRefused') + '\n'));
    assert.equal(view.statistics().observed, false); assert.equal(view.statistics().invalidChunks, 1);
    view.push(Buffer.from(wire('unreachable') + '\n'));
    assert.equal(view.statistics().reason, 'unreachable'); assert.equal(view.statistics().truncated, true);
});

test('超长行不接纳残尾，实际新行仍可投影', () => {
    const view = create(env);
    view.push(Buffer.from('x'.repeat(limits.lineBytes + 1)));
    view.push(Buffer.from(wire('peerRefused') + '\n'));
    assert.equal(view.statistics().observed, false);
    view.push(Buffer.from(wire('permissionDenied') + '\n'));
    assert.equal(view.statistics().reason, 'permissionDenied'); assert.equal(view.statistics().oversizedLines, 1);
});

test('记录、行和总字节预算耗尽不伪造无失败或未知，后续不观察', () => {
    const records = create(env);
    records.push(Buffer.from((wire('peerRefused') + '\n').repeat(limits.records + 2)));
    assert.equal(records.statistics().records, limits.records); assert.equal(records.statistics().truncated, true);
    const lines = create(env); lines.push(Buffer.from('\n'.repeat(limits.lines + 1)));
    lines.push(Buffer.from(wire('peerRefused') + '\n'));
    assert.equal(lines.statistics().lines, limits.lines); assert.equal(lines.statistics().observed, false);
    assert.equal(lines.statistics().truncated, true);
    const total = create(env); total.push(Buffer.alloc(limits.totalBytes + 1, 120));
    total.push(Buffer.from(wire('peerRefused') + '\n'));
    assert.equal(total.statistics().bytes, limits.totalBytes); assert.equal(total.statistics().observed, false);
    assert.equal(total.statistics().truncated, true); assert.equal(total.statistics().unknown, false);
});

test('错误输入对象不执行转换，close不伪造end，结束后push不重新接纳', () => {
    let called = false;
    const view = create(env); view.push({toString() {called = true; throw Error('private');}});
    assert.equal(called, false); assert.equal(view.statistics().invalidChunks, 1);
    view.close(); view.push(Buffer.from(wire('peerRefused') + '\n')); view.end();
    assert.equal(view.statistics().closed, true); assert.equal(view.statistics().ended, false);
    assert.equal(view.statistics().observed, false);
    const ended = observe('peerRefused'); ended.end(); ended.push(Buffer.from(wire('peerReset') + '\n'));
    assert.equal(ended.statistics().reason, 'peerRefused'); assert.equal(ended.statistics().afterEndPushes, 1);
});

test('未知Proxy也不得执行原型陷阱或把异常传播到业务pipe', () => {
    let called = false;
    const bytes = new Proxy({}, {getPrototypeOf() {called = true; throw Error('private-sentinel');}});
    const view = create(env);
    assert.doesNotThrow(() => view.push(bytes));
    assert.equal(called, false); assert.equal(view.statistics().invalidChunks, 1);
    assert.equal(view.statistics().observed, false);
});

test('隐私哨兵不进入投影/统计，快照独立且不证明采集或播放', () => {
    const sentinel = 'PRIVATE_TOKEN_ADDRESS_CONTENT';
    const view = create(env);
    view.push(Buffer.from(sentinel + '\n' + JSON.stringify({kind: 'udpSocketReceiveFailureDiagnostic',
        reason: 'peerRefused', secret: sentinel}) + '\n' + wire('peerRefused') + '\n'));
    const facts = view.statistics(); facts.reason = sentinel;
    const serialized = JSON.stringify(view.statistics());
    assert.equal(serialized.includes(sentinel), false); assert.equal(view.statistics().reason, 'peerRefused');
    assert.equal(Object.keys(view.statistics()).some(key => /play|capture|address|lineText|payload/i.test(key)), false);
});
