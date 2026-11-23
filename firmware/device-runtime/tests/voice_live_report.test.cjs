// 文件说明：逐轮真实验收报告的失败保留与正向回归；不访问网络或供应商。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { VoiceReport } = require('./voice_live_report.cjs');

// complete 提供一轮已解码且具有三类计时的确定性诊断事件。
function complete(report, round) {
  report.accept({ event: 'virtualPlaybackSaved', samples: 3200, round });
  report.accept({ event: 'roundCompleted', round, latencyBasis: 'lastNonSilentSentFrame',
    firstTextLatencyMs: 100, firstNetworkLatencyMs: 200, firstPlayableLatencyMs: 240 });
}

test('文字提交与语音末帧采用显式独立基准，不能混入同一验收', () => {
  for (const basis of ['textSubmitted', 'lastNonSilentSentFrame']) {
    const report = new VoiceReport(1, undefined, 'textSubmitted');
    report.accept({ event: 'virtualPlaybackSaved', samples: 3200, round: 1 });
    report.accept({ event: 'roundCompleted', round: 1, latencyBasis: basis,
      firstTextLatencyMs: 100, firstNetworkLatencyMs: 200, firstPlayableLatencyMs: 240 });
    assert.equal(report.finish(0).passed, basis === 'textSubmitted');
    assert.equal(report.finish(0).latencyBasis, 'textSubmitted');
  }
  assert.throws(() => new VoiceReport(1, undefined, 'unknown'));
});

test('每轮独立验收，第二轮缺少动作不能被首轮多次动作掩盖', () => {
  const report = new VoiceReport(2, 50);
  for (let i = 0; i < 2; i++) report.accept({ event: 'softwareActionCompleted', code: 200, volume: 50 });
  complete(report, 1);
  complete(report, 2);
  assert.equal(report.finish(0).passed, false);
});
test('通知必须配对，且实际动作及解码音频同时通过', () => {
  const report = new VoiceReport(1, 50);
  report.accept({ event: 'softwareActionCompleted', code: 200, volume: 50 });
  report.accept({ event: 'content', method: 'toolCallStart' });
  report.accept({ event: 'content', method: 'toolCallResult' });
  complete(report, 1);
  assert.equal(report.finish(0, true).passed, true);
  assert.equal(report.finish(1, true).passed, false);
});
test('无通知不伪造工具通知通过，普通语音仍可独立验收', () => {
  const report = new VoiceReport(1);
  complete(report, 1);
  assert.equal(report.finish(0).passed, true);
  assert.equal(report.finish(0, true).passed, false);
});
test('少轮、重复轮次及无效配置不通过', () => {
  const report = new VoiceReport(2);
  complete(report, 1);
  assert.equal(report.finish(0).passed, false);
  complete(report, 1);
  assert.equal(report.finish(0).passed, false);
  assert.throws(() => new VoiceReport(0));
});

test('错误动作、缺少音频、无效计时及孤立通知均失败', () => {
  for (const kind of ['wrong-volume', 'failed-action', 'no-audio', 'invalid-timing', 'unpaired']) {
    const report = new VoiceReport(1, 50);
    report.accept({ event: 'softwareActionCompleted', code: kind === 'failed-action' ? 500 : 200,
      volume: kind === 'wrong-volume' ? 40 : 50 });
    report.accept({ event: 'content', method: 'toolCallStart' });
    if (kind !== 'unpaired') report.accept({ event: 'content', method: 'toolCallResult' });
    if (kind !== 'no-audio') report.accept({ event: 'virtualPlaybackSaved', samples: 3200, round: 1 });
    report.accept({ event: 'roundCompleted', round: 1, latencyBasis: 'lastNonSilentSentFrame',
      firstTextLatencyMs: 100, firstNetworkLatencyMs: 200, firstPlayableLatencyMs: kind === 'invalid-timing' ? null : 240 });
    assert.equal(report.finish(0, true).passed, false, kind);
  }
});
