// 文件说明：执行真实WebRTC验收器的语料安装脚本，验证Peer重建仍取同一有界源；不替代真实媒体与模型验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {fixtureScript} = require('./browser_webrtc_live.cjs');

// fixture构造可控媒体端口；原源和语料源身份独立，重建Peer取正式页面stream。
function fixture(options = {}) {
  const original = {kind: 'audio', enabled: false, stopped: false, stop() { this.stopped = true; }};
  const injected = {kind: 'audio', enabled: true, stopped: false, stop() { this.stopped = true; }};
  const originalStream = {getAudioTracks: () => [original], getTracks: () => [original]};
  const destinationStream = {getAudioTracks: () => [injected], getTracks: () => [injected]};
  const sources = [];
  const beds = [];
  const sender = {track: original, async replaceTrack(track) {
    if (options.rejectReplace) throw Error('fixture_replace_failed');
    this.track = track;
  }};
  const context = {
    currentTime: 5,
    async decodeAudioData() { return {numberOfChannels: options.channels ?? 1, duration: options.duration ?? 2}; },
    createMediaStreamDestination() { return {stream: destinationStream}; },
    createBufferSource() {
      const source = {connect(target) { this.target = target; }, disconnect() { this.disconnected = true; },
        start(when) { this.startedAt = when; }};
      sources.push(source); return source;
    },
    createConstantSource() {
      const bed = {offset: {value: 1}, connect() {}, start() {}, stop() { this.stopped = true; },
        disconnect() { this.disconnected = true; }};
      beds.push(bed); return bed;
    }
  };
  const page = vm.createContext({context, stream: originalStream, window: {__urFixtureChunks: ['AA==']},
    rtcPeer: {getSenders: () => options.noSender ? [] : [sender]}, Uint8Array,
    atob: text => Buffer.from(text, 'base64').toString('binary')});
  return {page, sender, original, injected, originalStream, destinationStream, sources, beds};
}

test('正式语料安装后Peer重建取同一语料轨，旧采集源停止而不重新取麦克风', async () => {
  const value = fixture();
  assert.equal(await vm.runInContext(fixtureScript(false), value.page), 'fixture_ready');
  assert.equal(value.sender.track, value.injected);
  assert.equal(value.page.stream.getAudioTracks()[0], value.injected,
    '正式rtcCreatePeer读取stream，不能在新Peer恢复成fake-GUM');
  assert.equal(value.original.stopped, true);
  assert.equal(value.injected.enabled, false, '安装不能越过核心输入许可');
  value.page.window.__urPlayFixture();
  assert.equal(value.sources[0].startedAt, 5.05);
  value.sources[0].onended();
  assert.equal(value.sources[0].disconnected, true);
  assert.equal(value.page.window.__urFixtureEnded, 1);
  value.page.stream.getTracks().forEach(track => track.stop());
  assert.equal(value.injected.stopped, true, '正式release持有当前语料流并能释放');
});

test('替换失败保留原页面源，释放新轨和静音床且不开放输入', async () => {
  const value = fixture({rejectReplace: true});
  await assert.rejects(vm.runInContext(fixtureScript(true), value.page), /fixture_replace_failed/);
  assert.equal(value.page.stream, value.originalStream);
  assert.equal(value.sender.track, value.original);
  assert.equal(value.original.stopped, false);
  assert.equal(value.injected.stopped, true);
  assert.equal(value.beds[0].stopped, true);
  assert.equal(value.beds[0].disconnected, true);
  assert.equal(value.page.window.__urPlayFixture, undefined);
});

test('原单声道与十秒边界不放宽，无合法sender不得替换正式页面源', async () => {
  for (const options of [{channels: 2}, {duration: 0}, {duration: 10.01}, {noSender: true}]) {
    const value = fixture(options);
    await assert.rejects(vm.runInContext(fixtureScript(false), value.page), /invalid_fixture|missing_sender/);
    assert.equal(value.page.stream, value.originalStream);
    assert.equal(value.original.stopped, false);
    assert.equal(value.sender.track, value.original);
  }
  const boundary = fixture({duration: 10});
  assert.equal(await vm.runInContext(fixtureScript(true), boundary.page), 'fixture_ready');
  assert.equal(boundary.page.stream, boundary.destinationStream);
  assert.equal(boundary.beds[0].offset.value, 0);
  assert.equal(boundary.injected.enabled, false);
});
