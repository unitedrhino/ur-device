// 文件说明：组合预览验收的显式配置与真实解码判据回归；合成数值不冒充真实WebRTC运行。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {previewCount, decodedVideo} = require('./talk_preview_live.cjs');

test('三预览组合必须明确选择，拼错或部分观看者不能冒充组合覆盖', () => {
  for (const value of [undefined, '0']) assert.equal(previewCount(value), 0);
  assert.equal(previewCount('3'), 3);
  for (const value of [null, '', 0, 3, '1', '2', '4', '03', 'true']) assert.throws(() => previewCount(value));
});

test('预览要求实际非黑视频解码，建立连接和收到包不足以证明可播放', () => {
  const good = {connected: true, active: true, audio: false, frames: 2, packets: 3, pixels: 12};
  assert.equal(decodedVideo(good), true);
  for (const value of [null, undefined, {}, ...[
    {connected: false}, {connected: 1}, {active: false}, {audio: true}, {audio: undefined},
    {frames: 0}, {frames: 1}, {frames: NaN}, {frames: '2'}, {frames: 2.5},
    {packets: 0}, {packets: -1}, {packets: Infinity}, {pixels: 0}, {pixels: NaN}, {pixels: '12'}
  ].map(change => ({...good, ...change}))]) assert.equal(decodedVideo(value), false);
});
