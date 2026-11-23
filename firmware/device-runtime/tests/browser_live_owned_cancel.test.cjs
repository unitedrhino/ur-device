// 文件说明：单设备精确取消的普通Node回归；仅模拟child事件和运行标准库子进程，不需要后端、浏览器或媒体。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {spawn} = require('node:child_process');
const {create} = require('./browser_live_owned_cancel.cjs');

// fixture 分离spawn/error/close与kill接受事实，不能把发TERM或error冒充实际退出。
function fixture() {
  const child = new EventEmitter(), signals = [];
  child.kill = signal => { signals.push(signal); return true; };
  return {child, signals};
}
test('取消早于spawn只锁存，重复信号只TERM一次，close后不再发送', () => {
  const bus = new EventEmitter(), {child, signals} = fixture(), owned = create(bus);
  bus.emit('SIGTERM'); owned.attach(child); assert.equal(signals.length, 0);
  child.emit('spawn'); bus.emit('SIGINT'); bus.emit('SIGTERM'); owned.stopChild();
  assert.deepEqual(signals, ['SIGTERM']); child.emit('close', 1, null); owned.stopChild();
  assert.equal(signals.length, 1); assert.equal(owned.cancelled, true);
  owned.finish(); owned.finish(); assert.equal(bus.listenerCount('SIGTERM'), 0); assert.equal(bus.listenerCount('SIGINT'), 0);
});
test('运行中error不是close，信号失败后原finally仍可精确回收且不能成功', () => {
  const bus = new EventEmitter(), {child, signals} = fixture(), owned = create(bus);
  owned.attach(child); child.emit('spawn'); child.emit('error', Error('controlled_running_error'));
  owned.stopChild(); assert.deepEqual(signals, ['SIGTERM']); assert.equal(owned.operationFailed, true);
  child.emit('error', Error('controlled_async_signal_failure')); owned.stopChild(); assert.equal(signals.length, 2);
  child.emit('close', 1, null); owned.stopChild(); assert.equal(signals.length, 2); owned.finish();
});
test('kill返回false或抛错不永久失去清理，不绑定替换实例', () => {
  const bus = new EventEmitter(), {child} = fixture(), owned = create(bus); let attempts = 0;
  child.kill = () => { ++attempts; if (attempts === 1) return false; if (attempts === 2) throw Error('controlled_signal'); return true; };
  owned.attach(child); assert.throws(() => owned.attach(fixture().child)); child.emit('spawn');
  owned.stopChild(); owned.stopChild(); owned.stopChild(); owned.stopChild();
  assert.equal(attempts, 3); assert.equal(owned.operationFailed, true); child.emit('close', 1, null); owned.finish();
});
test('启动error不信号未知实例，启动失败监听清理幂等', () => {
  const bus = new EventEmitter(), {child, signals} = fixture(), owned = create(bus);
  owned.attach(child); child.emit('error', Error('controlled_spawn_failure')); bus.emit('SIGTERM'); owned.stopChild();
  assert.equal(signals.length, 0); child.emit('close', -2, null); owned.finish();
  assert.equal(bus.listenerCount('SIGTERM'), 0);
  const early = create(bus); early.finish(); assert.equal(bus.listenerCount('SIGINT'), 0);
});
test('真实Node启动失败error后必须等待close，取消的已spawn子进程也实际close', async () => {
  const failed = spawn('/__task143_owned_nonexistent_executable__', [], {stdio: 'ignore'}), events = [];
  failed.once('error', () => events.push('error'));
  await new Promise(resolve => failed.once('close', () => { events.push('close'); resolve(); }));
  assert.deepEqual(events, ['error', 'close']);
  const child = spawn(process.execPath, ['-e', 'setInterval(()=>{},10000)'], {stdio: 'ignore'});
  const bus = new EventEmitter(), owned = create(bus);
  owned.attach(child);
  const ended = new Promise(resolve => child.once('close', (code, signal) => resolve({code, signal})));
  try {
    await new Promise((resolve, reject) => { child.once('spawn', resolve); child.once('error', reject); });
    bus.emit('SIGTERM'); const terminal = await ended; assert.equal(terminal.signal, 'SIGTERM');
    assert.equal(owned.cancelled, true); assert.equal(owned.operationFailed, false);
  } finally { owned.stopChild(); await ended; owned.finish(); }
});
