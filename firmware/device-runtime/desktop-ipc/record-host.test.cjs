// 文件说明：历史专用宿主页面离线回归；网络和档案源显式替身，不称真实协议或媒体验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture运行正式host脚本，源替身只记录调用，不提供媒体成功／权限事实。
async function fixture(options = {}) {
  const messages = [], sources = [], sockets = [], handlers = {};
  const elements = new Map();
  for (const id of ['record-status', 'record-session', 'record-media', 'record-close'])
    elements.set(id, {textContent: '', addEventListener(name, handler) { handlers[`${id}:${name}`] = handler; }});
  class Socket {
    static OPEN = 1;
    constructor(url) { this.url = url; this.readyState = 1; sockets.push(this); }
    send(value) { messages.push(JSON.parse(value)); }
  }
  class Source {
    constructor(options) { this.options = options; this.incomingCalls = []; this.closed = 0; this.binding = null; sources.push(this); }
    async incoming(message) { this.incomingCalls.push(message); return options.accept !== false; }
    async close(value) { this.closed++; this.lastClose = value; }
  }
  const context = vm.createContext({document: {getElementById: id => elements.get(id)},
    WebSocket: Socket, UrRecordSource: Source, location: {host: '127.0.0.1:12345'},
    fetch: async route => ({ok: options.bootstrap !== false, json: async () => ({token: options.token || 'a'.repeat(64)})}),
    addEventListener(name, handler) { handlers[name] = handler; },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'record-host.js'), 'utf8'), context);
  await new Promise(resolve => setImmediate(resolve));
  return {context, messages, sources, sockets, handlers, elements,
    async incoming(message) { await sockets[0].onmessage({data: JSON.stringify(message)}); },
  };
}
test('同源bootstrap后只发送ready，不自行授信或发布自由平台方法', async () => {
  const f = await fixture(); f.sockets[0].onopen();
  assert.deepEqual(f.messages, [{kind: 'ready'}]);
  assert.match(f.sockets[0].url, /^ws:\/\/127\.0\.0\.1:12345\/bridge\?token=[a-f0-9]{64}$/);
  assert.equal(f.sources[0].incomingCalls.length, 0); assert.equal(f.context.recordHostState, null);
});
test('配对失败或非法原token不建立WebSocket或档案源', async () => {
  for (const options of [{bootstrap: false}, {token: 'invalid'}]) {
    const f = await fixture(options);
    assert.equal(f.sockets.length, 0); assert.equal(f.sources.length, 0);
  }
});
test('驱动命令准确交独立源，页面不自动play或生成新m', async () => {
  const f = await fixture(); const message = {kind: 'recordPeerBegin', sessionId: 's', playbackId: 'p', mediaRevision: 2};
  await f.incoming(message);
  assert.deepEqual(JSON.parse(JSON.stringify(f.sources[0].incomingCalls)), [message]);
  assert.equal(f.messages.length, 0);
});
test('核心投影与档案Actual分开，额外权限字段不能被界面接纳', async () => {
  const f = await fixture(); const projection = {kind: 'recordSessionState', phase: 'active', state: 'paused', revision: 4, mediaRevision: 2, released: false, releaseSubmitted: false};
  await f.incoming(projection); assert.equal(f.context.recordHostState.state, 'paused');
  assert.equal(f.sources[0].incomingCalls.length, 0); assert.equal(f.messages.length, 0);
  await f.incoming({...projection, permission: true});
  assert.equal(f.context.recordHostState.permission, undefined);
  assert.equal(f.elements.get('record-status').textContent, '本地连接已关闭');
});
test('失联关闭原源但不伪造平台Released，迟到UI不自动换会话', async () => {
  const f = await fixture(); f.sources[0].binding = {sessionId: 's', playbackId: 'p'};
  await f.sockets[0].onclose(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.sources[0].closed, 1);
  assert.deepEqual(JSON.parse(JSON.stringify(f.sources[0].lastClose)), {kind: 'recordClose', sessionId: 's', playbackId: 'p', releaseId: '1'});
  assert.equal(f.messages.length, 0);
});
test('退出按钮仅发严格shutdown，不改媒体或平台授权', async () => {
  const f = await fixture(); f.handlers['record-close:click']();
  assert.deepEqual(f.messages, [{kind: 'shutdown'}]); assert.equal(f.sources[0].closed, 0);
});
