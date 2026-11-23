// 文件说明：正式主题末端的离线DOM边界测试；模拟帧/样式不代替真实浏览器可见副作用验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 使用可控帧与定时端口运行真实生产类，不依赖sleep或网络。
function fixture() {
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/client-theme.js'), 'utf8'), context);
  const frames = new Map(), timers = new Map(), emitted = [];
  const element = {style: {}, dataset: {}};
  let sequence = 0, visible = true, mismatch = false;
  const display = new context.UrClientThemeDisplay({element,
    emit: value => emitted.push(JSON.parse(JSON.stringify(value))),
    setTimer: (fn, milliseconds) => { assert.equal(milliseconds, 1500); timers.set(++sequence, fn); return sequence; },
    clearTimer: id => timers.delete(id),
    scheduleFrame: fn => { frames.set(++sequence, fn); return sequence; },
    cancelFrame: id => frames.delete(id),
    readStyle: () => ({backgroundColor: mismatch ? 'wrong' : element.style.backgroundColor, color: element.style.color}),
    isVisible: () => visible,
  });
  return {display, element, frames, timers, emitted,
    frame: () => { const fn = frames.values().next().value; assert.ok(fn); fn(); },
    timeout: () => { const fn = timers.values().next().value; assert.ok(fn); fn(); },
    hidden: () => { visible = false; },
    mismatch: () => { mismatch = true; },
  };
}
// command 只包含本地票据与审核枚举，不包含平台身份、参数正文或执行令牌。
function command(themeTicket = '1', theme = 'dark') { return {kind: 'clientThemeSet', themeTicket, theme}; }

test('真实样式应用后仍等待绘制读回，原票据实际一致才成功', () => {
  const f = fixture();
  assert.equal(f.display.ready(), true);
  assert.deepEqual(f.emitted, [{kind: 'clientThemeReady', schemaRevision: 1}]);
  assert.equal(f.display.handle(command()), true);
  assert.equal(f.element.style.backgroundColor, 'rgb(15, 23, 42)');
  assert.equal(f.emitted.length, 1);
  f.frame();
  assert.deepEqual(f.emitted[1], {kind: 'clientThemeActual', themeTicket: '1', status: 'succeeded', theme: 'dark'});
  assert.equal(f.frames.size, 0);
  assert.equal(f.timers.size, 0);
});

test('重复票据只重放同原结果，参数冲突及退回票据不能再次应用', () => {
  const f = fixture();
  assert.equal(f.display.handle(command('2')), true);
  assert.equal(f.display.handle(command('2')), true);
  assert.equal(f.frames.size, 1);
  f.frame();
  assert.equal(f.display.handle(command('2')), true);
  assert.deepEqual(f.emitted[1], f.emitted[0]);
  assert.equal(f.display.handle(command('2', 'light')), false);
  assert.equal(f.display.handle(command('1', 'light')), false);
  assert.equal(f.frames.size, 0);
});

test('脚本或额外身份/令牌、非法主题及非规范票据都在副作用前拒绝', () => {
  const f = fixture();
  for (const value of [{...command(), executionToken: 'synthetic'}, {...command(), script: 'synthetic'},
    command('01'), command('0'), command('18446744073709551616'), command('1', 'arbitrary-css')])
    assert.equal(f.display.handle(value), false);
  assert.deepEqual(f.element.style, {});
  assert.equal(f.emitted.length, 0);
});

test('页面不可见或实际颜色错配只能unknown', () => {
  for (const vary of ['hidden', 'mismatch']) {
    const f = fixture();
    f[vary]();
    assert.equal(f.display.handle(command()), true);
    f.frame();
    assert.deepEqual(f.emitted[0], {kind: 'clientThemeActual', themeTicket: '1', status: 'unknown', theme: ''});
  }
});

test('局部超时只固定unknown，迟到帧不能补成功或重发副作用', () => {
  const f = fixture();
  assert.equal(f.display.handle(command()), true);
  const late = f.frames.values().next().value;
  f.timeout();
  assert.equal(f.emitted[0].status, 'unknown');
  late();
  assert.equal(f.emitted.length, 1);
  assert.equal(f.display.handle(command()), true);
  assert.equal(f.emitted[1].status, 'unknown');
  assert.equal(f.frames.size, 0);
});

test('原页面关闭清理等待，不输出结果或承接新页面执行', () => {
  const f = fixture();
  assert.equal(f.display.handle(command()), true);
  const late = f.frames.values().next().value;
  f.display.close();
  late();
  assert.equal(f.emitted.length, 0);
  assert.equal(f.frames.size, 0);
  assert.equal(f.timers.size, 0);
  assert.equal(f.display.handle(command('2')), false);
});
