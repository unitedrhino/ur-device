// 文件说明：真实浏览器测试取消时只回收原持有的设备子进程；运行中error不是close，失败不能冒成功。
'use strict';
const assert = require('node:assert/strict');

// create 锁存bus的SIGTERM/SIGINT并绑定唯一ChildProcess；返回取消、信号失败事实和原finally的精确回收入口。
function create(bus) {
  // sent只表示该句柄接受过TERM，closed只由真实close产生；两者不能互相替代。
  let child = null, spawned = false, closed = false, startupFailed = false;
  let sent = false, cancelled = false, disposed = false, operationFailed = false;
  // stopChild 不信号尚未spawn或已close实例；信号失败允许原finally重试，不续期或换PID。
  const stopChild = () => {
    if (disposed || !child || !spawned || closed || startupFailed || sent) return false;
    try {
      sent = child.kill('SIGTERM') === true;
      if (!sent) operationFailed = true;
      return sent;
    } catch {
      operationFailed = true;
      return false;
    }
  };
  // onSignal 在spawn前只锁存取消，spawn后仅转发原实例，重复信号不重复发送成功TERM。
  const onSignal = () => { cancelled = true; stopChild(); };
  bus.on('SIGTERM', onSignal);
  bus.on('SIGINT', onSignal);
  // attach 只能绑定原ChildProcess一次；启动失败无活实例，运行中error仍保留原精确清理能力。
  function attach(value) {
    assert.equal(child, null);
    child = value;
    child.once('spawn', () => { spawned = true; if (cancelled) stopChild(); });
    child.on('error', () => {
      if (!spawned) startupFailed = true;
      else { operationFailed = true; sent = false; }
    });
    child.once('close', () => { closed = true; });
  }
  // finish 供调用方实际close或同步spawn失败后释放入口监听，不伪造设备退出或修改原预算。
  function finish() {
    disposed = true;
    bus.removeListener('SIGTERM', onSignal);
    bus.removeListener('SIGINT', onSignal);
  }
  return {attach, stopChild, finish, get cancelled() { return cancelled; }, get operationFailed() { return operationFailed; }};
}
module.exports = {create};
