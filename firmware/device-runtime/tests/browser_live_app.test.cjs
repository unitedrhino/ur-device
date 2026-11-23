// 文件说明：原设备页固定Live分派与默认隔离回归；隔离VM不代表实际浏览器解码或平台E2E。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');
// page 保持原AI宿主，不给新Live构造云身份、权限、真实媒体或成功握手。
function page() {
  const elements = new Map();
  const context = vm.createContext({document: {getElementById(id) {
    if (!elements.has(id)) elements.set(id, {textContent: '', addEventListener() {}});
    return elements.get(id);
  }}, window: {addEventListener() {}}});
  vm.runInContext(source, context);
  return {context, transcript: () => elements.get('transcript')?.textContent};
}
test('only explicit host consumes fixed live commands and AI content remains independent', () => {
  const {context, transcript} = page(); const consumed = [];
  context.__live = {handle(value) { consumed.push(value.kind); return true; }};
  vm.runInContext('desktopLive=__live', context);
  for (const kind of ['liveLaneGate', 'liveBegin', 'liveAnswer', 'liveTrackControl', 'liveClose', 'liveVolume'])
    context.dispatchLocalMessage({kind});
  assert.equal(consumed.length, 6);
  context.dispatchLocalMessage({kind: 'text', text: 'original-ai'});
  assert.equal(transcript(), 'original-ai'); assert.equal(consumed.length, 6);
});
test('default host does not enable live and forged downstream actual cannot grant output', () => {
  const {context} = page();
  assert.throws(() => context.dispatchLocalMessage({kind: 'liveBegin'}), /unknown_message/);
  const consumed = [];
  context.__live = {handle(value) { consumed.push(value.kind); return true; }};
  vm.runInContext('desktopLive=__live', context);
  for (const kind of ['liveTrackFact', 'liveConnected', 'trackState', 'liveReady'])
    assert.throws(() => context.dispatchLocalMessage({kind}), /unknown_message/);
  assert.equal(consumed.length, 0);
});
test('invalid original live command is never redirected to file handler or silently acknowledged', () => {
  const {context} = page(); let files = 0;
  context.__live = {handle() { return false; }};
  context.__files = {handle() { ++files; return true; }};
  vm.runInContext('desktopLive=__live;desktopFiles=__files', context);
  assert.throws(() => context.dispatchLocalMessage({kind: 'liveVolume', percent: 999}), /invalid_live_command/);
  assert.equal(files, 0);
  context.dispatchLocalMessage({kind: 'fileMask'}); assert.equal(files, 1);
});
