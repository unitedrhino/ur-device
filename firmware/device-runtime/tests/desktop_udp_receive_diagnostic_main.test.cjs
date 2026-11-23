// 文件说明：实物socket诊断必须在首次泵接收前明确开启；结构检查不替代真实UDP与ICMP回归。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/shared_handoff_main.cpp'), 'utf8');

test('原Main仅显式诊断时在实际socket首次接收前开启旁证', () => {
    const enable = source.indexOf('socket.enable_receive_failure_diagnostics()');
    assert.ok(enable > source.indexOf('UdpSocket socket;'));
    assert.ok(enable < source.indexOf('SharedHandoffHost host'));
    assert.ok(enable < source.indexOf('host.poll('));
    assert.match(source.slice(source.lastIndexOf('if (context_diagnostic)', enable), enable), /^if \(context_diagnostic\) \{/);
    assert.equal(source.match(/socket\.enable_receive_failure_diagnostics\(\)/g).length, 1);
});

test('退出独立输出原首因而不覆盖voiceMediaFailure或输出系统正文', () => {
    const exit = source.indexOf('if (!context_diagnostic || exit_diagnostic_printed)');
    const read = source.indexOf('socket.receive_failure_diagnostic()');
    assert.ok(read > exit);
    assert.ok(source.indexOf('voiceMediaFailureDiagnostic') < read);
    assert.ok(source.indexOf('udpSocketReceiveFailureDiagnostic') > read);
    assert.match(source.slice(read, source.indexOf('while (!done)', read)), /string_view\("unknown"\)/);
    assert.doesNotMatch(source.slice(read, source.indexOf('while (!done)', read)), /strerror|errno|peer_address|local_address|payload/);
});
