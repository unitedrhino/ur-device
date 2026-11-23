// 文件说明：真实门控验收器的离线负例；不能用握手、静音原素材或停止采集冒充双向门控成功。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {validate} = require('./audio_gate_live.cjs');
// fixture 是显式验收器输入夹具，不冒称实际浏览器执行结果。
function fixture() {
  return [{kind: 'gateComplete', frames: [20, 20, 20], captureFrames: [20, 20, 20]},
    {failure: false, played: [20, 20, 20], nonSilent: [20, 0, 20], capture: [20, 20, 20],
      peak: [0.25, 0, 0.25], gates: [{ticket: '1', enabled: false}, {ticket: '2', enabled: true}]}];
}
test('完整原C++事实及独立输出图才可通过', () => { assert.doesNotThrow(() => validate(...fixture())); });
test('缺失或矛盾媒体事实全部拒绝', () => {
  const mutations = [
    ([c]) => { c.kind = 'probeReady'; },
    ([c]) => { c.frames[1] = 0; },
    ([c]) => { c.captureFrames[1] = 0; },
    ([c]) => { c.captureFrames = []; },
    ([,e]) => { e.capture[1] = 0; },
    ([,e]) => { e.capture[1] = NaN; },
    ([,e]) => { e.played[2] = 0; },
    ([,e]) => { e.nonSilent = [0, 0, 0]; },
    ([,e]) => { e.nonSilent[1] = 1; },
    ([,e]) => { e.peak[1] = 0.01; },
    ([,e]) => { e.peak[2] = 0; },
    ([,e]) => { e.peak[0] = Infinity; },
    ([,e]) => { e.peak = []; },
    ([,e]) => { e.gates[1].ticket = '1'; },
    ([,e]) => { e.failure = true; }
  ];
  for (const mutate of mutations) {
    const input = fixture(); mutate(input);
    assert.throws(() => validate(...input));
  }
});

// focusFixture 只给验收器提供显式输入，不作为实际资源执行证据。
function focusFixture() {
  const input = fixture();
  input[0].focusStates = ['acquired', 'suspended', 'restored', 'released'];
  input[0].focusRevision = 4;
  input[1].gates = [false, true, false, true, false].map((enabled, index) => ({ticket: String(index + 1), enabled}));
  input[1].releasePeak = 0;
  return input;
}
test('共享焦点必须四个实际水位及最终原释放静音', () => {
  assert.doesNotThrow(() => validate(...focusFixture(), true));
  for (const mutate of [
    ([c]) => { c.focusStates.pop(); },
    ([c]) => { c.focusStates[1] = 'restored'; },
    ([c]) => { c.focusRevision = 3; },
    ([,e]) => { e.gates.pop(); },
    ([,e]) => { e.gates[4].enabled = true; },
    ([,e]) => { delete e.releasePeak; },
    ([,e]) => { e.releasePeak = 0.01; },
  ]) {
    const input = focusFixture(); mutate(input);
    assert.throws(() => validate(...input, true));
  }
});
