// 文件说明：正式历史E2E入口纯函数回归；不启动后端、浏览器、ffmpeg或生成伪造媒体证据。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {fixturePlan, validateConfiguration, openCommand, playbackCommand, controlFactReady, mediaPass, pausePass} = require('./record_live.cjs');

// environment仅为前置校验语料，所有身份为占位值，不包含真实凭据。
function environment() {
  return {UR_RECORD_LIVE: '1', UR_RECORD_BASE_URL: 'http://127.0.0.1:12345', UR_RECORD_DEVICE_PORT: '23456',
    UR_RECORD_DEVICE_BIN: '/task/device_record', UR_RECORD_FILE: '/task/.temp/archive.mp4', UR_RECORD_ENABLED: '1',
    UR_RECORD_AUDIO: '1', UR_RECORD_DURATION_MS: '30000', UR_RECORD_MAX_BYTES: '8388608', UR_RECORD_LEASE_MS: '60000',
    UR_RECORD_START_TIME_MS: '1900000000000', UR_RECORD_APP_TOKEN: '<private-token>', UR_RECORD_TENANT: 'test', UR_RECORD_APP_ID: 'demo',
    UR_DEVICE_PRODUCT: 'p', UR_DEVICE_NAME: 'd', UR_DEVICE_SECRET: '<private-secret>', UR_RECORD_CAMERA_ID: 'front',
    UR_RECORD_ID: 'archive', UR_RECORD_VERSION: 'v1', UR_RECORD_CONTENT_REF: 'record:archive-v1'};
}
test('真实入口要求显式隔离和固定整份格式，副作用前拒绝缺配置', () => {
  const env = environment(); assert.deepEqual(validateConfiguration(env, () => true), {port: 23456, start: 1900000000000});
  for (const patch of [{UR_RECORD_LIVE: '0'}, {UR_RECORD_BASE_URL: 'https://production.invalid'}, {UR_RECORD_AUDIO: '0'},
    {UR_RECORD_DURATION_MS: '29000'}, {UR_RECORD_MAX_BYTES: '8388609'}, {UR_RECORD_LEASE_MS: '0'}, {UR_RECORD_FILE: 'relative.mp4'},
    {UR_RECORD_START_TIME_MS: '9007199254740992'}, {UR_RECORD_APP_TOKEN: ''}])
    assert.throws(() => validateConfiguration({...env, ...patch}, () => true));
  assert.throws(() => validateConfiguration(env, () => false));
});
test('素材生成计划固定H264+Opus而非实时源，-n禁止覆盖，输出仅任务.temp', () => {
  const cwd = '/workspace', plan = fixturePlan('/workspace/.temp/record-stage/archive.mp4', cwd);
  assert.ok(plan.ffmpeg.includes('libx264') && plan.ffmpeg.includes('libopus') && plan.ffmpeg.includes('-n'));
  assert.equal(plan.ffmpeg.includes('libvpx'), false); assert.equal(plan.ffmpeg.includes('aac'), false);
  assert.ok(plan.ffmpeg.includes('sine=frequency=880:sample_rate=48000:duration=30'));
  const markers = plan.ffmpeg[plan.ffmpeg.indexOf('-vf') + 1];
  assert.equal((markers.match(/enable=/g) || []).length, 5); assert.ok(markers.includes('floor(t)'));
  assert.throws(() => fixturePlan('/outside/archive.mp4', cwd));
  assert.throws(() => fixturePlan('/workspace/.temp/../outside.mp4', cwd));
  assert.throws(() => fixturePlan('/workspace/.temp/record/archive.webm', cwd));
});
test('open只带真实用户意图和整份clip，版本/ref/用户不成为自报许可', () => {
  const command = openCommand(environment(), 'original');
  assert.deepEqual(command, {msgToken: 'original', method: 'interaction.open', target: {productID: 'p', deviceName: 'd'},
    params: {purpose: 'recordPlayback', cameraId: 'front', recordingId: 'archive', clip: {startMs: 0, endMs: 30000}}});
  assert.equal(JSON.stringify(command).includes('contentRef'), false); assert.equal(JSON.stringify(command).includes('userId'), false);
});
test('seek始终带原当前m及原Actual水位，不提前跳代或混用用户位置', () => {
  const command = playbackCommand('original', 'session', 'player', 4, 2, 'seek', 20000);
  assert.deepEqual(command.params.arguments, {operation: 'seek', expectedRevision: 4, playbackId: 'player', mediaRevision: 2, positionMs: 20000});
  assert.throws(() => playbackCommand('original', 'session', 'player', 4, 0, 'seek', 20000));
  assert.throws(() => playbackCommand('original', 'session', 'player', 4, 2, 'play', 20000));
  assert.throws(() => playbackCommand('original', 'session', 'player', 4, 2, 'seek', 30001));
});
test('真实媒体验收同时要求像素秒标记和非静音PCM，不用RTP/握手替代', () => {
  const actual = {connected: true, frames: 3, pixels: 1000, width: 320, audioSamples: 5000, peak: 0.1, run: 3, markerSecond: 12};
  assert.equal(mediaPass(actual, 10, 15), true);
  for (const patch of [{frames: 0}, {pixels: 0}, {audioSamples: 0}, {peak: 0}, {run: 0}, {markerSecond: 3}, {markerSecond: null}, {connected: false}])
    assert.equal(mediaPass({...actual, ...patch}, 10, 15), false);
});
test('暂停稳定窗口必须源实际paused/位置不进展且视频音频RTP均停', () => {
  const before = {frames: 3, packets: 10, audioPackets: 20}, source = {state: 'paused', positionMs: 12000, frames: 100};
  assert.equal(pausePass(before, before, source, source), true);
  assert.equal(pausePass(before, {...before, audioPackets: 21}, source, source), false);
  assert.equal(pausePass(before, {...before, frames: 4}, source, source), false);
  assert.equal(pausePass(before, before, source, {...source, state: 'playing'}), false);
  assert.equal(pausePass(before, before, source, {...source, positionMs: 13000}), false);
  assert.equal(pausePass(before, before, source, {...source, frames: 101}), false);
});
test('新Peer实际播放不等于APP已收到playing事实，seeking不能成为暂停前置', () => {
  const seeking = {playbackId: 'player', mediaRevision: 2, revision: 3, state: 'seeking'};
  assert.equal(controlFactReady(seeking, 'player', 2, 'pause'), false);
  assert.equal(controlFactReady({...seeking, state: 'playing', revision: 4}, 'player', 2, 'pause'), true);
  assert.equal(controlFactReady({...seeking, state: 'playing'}, 'other', 2, 'pause'), false);
  assert.equal(controlFactReady({...seeking, state: 'playing'}, 'player', 1, 'pause'), false);
});
test('控制前置严格区分操作状态及非法水位，不用终态或旧媒体代次恢复播放', () => {
  const loaded = {playbackId: 'player', mediaRevision: 1, revision: 1, state: 'loaded'};
  assert.equal(controlFactReady(loaded, 'player', 1, 'play'), true);
  assert.equal(controlFactReady(loaded, 'player', 1, 'seek'), true);
  assert.equal(controlFactReady(loaded, 'player', 1, 'pause'), false);
  for (const state of ['ended', 'failed', 'stopped', 'seeking', 'unknown'])
    assert.equal(controlFactReady({...loaded, state}, 'player', 1, 'play'), false);
  for (const revision of [-1, 0, 1.5, Number.MAX_SAFE_INTEGER + 1])
    assert.equal(controlFactReady({...loaded, revision}, 'player', 1, 'play'), false);
  assert.equal(controlFactReady(loaded, 'player', 1, 'unsupported'), false);
});
