// 文件说明：隔离联调的精确子进程收尾；PID消失不等于close，身份变化绝不发送信号。
'use strict';
const assert=require('node:assert/strict');
const {isDeepStrictEqual}=require('node:util');

// release 逐次核验原PID/start/exe/cwd/argv；退出到close的窗口仅等待原close，不补发信号。
// ports提供实际fingerprint与有界waitClose，原TERM/KILL两段预算不增加；返回true必须有原close。
async function release(held,ports){
  if(!held||held.closed)return true;
  assert.ok(held.record,'owned_child_identity_unknown');
  const {pid,...expected}=held.record; // PID仅寻址，其他原始字段全部参与身份比较。
  for(const [signal,ms]of [['SIGTERM',2500],['SIGKILL',5000]]){
    if(held.closed)return true;
    const current=ports.fingerprint(pid,held.record.start);
    if(current!==null){
      assert.ok(isDeepStrictEqual(current,expected),'owned_child_identity_changed');
      held.child.kill(signal);
    }
    // 原进程已退出但stdio尚未close时不能当作新身份，也不能把PID消失直接记为成功。
    if(await ports.waitClose(held,ms)){
      assert.equal(held.closed,true,'owned_child_close_missing');
      return true;
    }
    if(current===null)throw Error('owned_child_close_missing');
  }
  throw Error('owned_child_close_missing');
}
module.exports={release};
