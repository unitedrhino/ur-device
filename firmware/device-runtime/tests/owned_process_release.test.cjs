// 文件说明：精确收尾的身份与exit/close边界回归；真实子进程不接模型或共享环境。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {spawn}=require('node:child_process');
const {release}=require('./owned_process_release.cjs');

// fixture 给确定性分支保存信号和等待事实；不模拟真实PID释放成功。
function fixture(){
  const current={start:'10',executable:'/owned/bin',cwd:'/owned',argv:['/owned/bin']};
  const signals=[],waits=[];
  const held={closed:false,record:{pid:12,...current},child:{kill:signal=>signals.push(signal)}};
  const ports={fingerprint:()=>current,waitClose:async(value,ms)=>{waits.push(ms);value.closed=true;return true;}};
  return {current,held,ports,signals,waits};
}
test('原close已经观察后不查询身份或发送信号',async()=>{
  assert.equal(await release({closed:true},{fingerprint:()=>assert.fail()}),true);
});
test('原身份匹配才发送TERM并等待实际close',async()=>{
  const f=fixture();assert.equal(await release(f.held,f.ports),true);
  assert.deepEqual(f.signals,['SIGTERM']);assert.deepEqual(f.waits,[2500]);
});
test('PID消失只等待原close，未close仍失败且不发送信号',async()=>{
  const f=fixture();f.ports.fingerprint=()=>null;f.ports.waitClose=async()=>false;
  await assert.rejects(release(f.held,f.ports),/owned_child_close_missing/);
  assert.deepEqual(f.signals,[]);assert.equal(f.held.closed,false);
});
test('PID复用、原启动时刻异常及错误的close自报均拒绝',async()=>{
  const f=fixture();f.ports.fingerprint=()=>({...f.current,start:'11'});
  await assert.rejects(release(f.held,f.ports),/owned_child_identity_changed/);
  assert.deepEqual(f.signals,[]);
  f.ports.fingerprint=()=>{throw Error('original_start_changed');};
  await assert.rejects(release(f.held,f.ports),/original_start_changed/);
  f.ports.fingerprint=()=>null;f.ports.waitClose=async()=>true;
  await assert.rejects(release(f.held,f.ports),/owned_child_close_missing/);
  assert.deepEqual(f.signals,[]);
});
test('第一段超时后核原身份，再KILL且原close才成功',async()=>{
  const f=fixture();let count=0;
  f.ports.waitClose=async(value,ms)=>{f.waits.push(ms);if(++count===2){value.closed=true;return true;}return false;};
  assert.equal(await release(f.held,f.ports),true);
  assert.deepEqual(f.signals,['SIGTERM','SIGKILL']);assert.deepEqual(f.waits,[2500,5000]);
});
test('实际exit到stdio close窗口不误报失败、不发送任何信号',async()=>{
  const child=spawn(process.execPath,['-e','process.exit(0)'],{env:{PATH:process.env.PATH},stdio:['ignore','pipe','pipe']});
  const signals=[]; // 本用例在原exit回调中调用release，真实PID不可再发信号。
  const held={closed:false,record:{pid:12,start:'10'},child:{kill:signal=>signals.push(signal)}};
  const done=new Promise(resolve=>child.once('close',()=>{held.closed=true;resolve();}));
  const ports={fingerprint:()=>null,waitClose:async(_,ms)=>{
    let timer;try{return await Promise.race([done.then(()=>true),new Promise(resolve=>{timer=setTimeout(()=>resolve(false),ms);})]);}finally{clearTimeout(timer);}
  }};
  try{
    await new Promise((resolve,reject)=>{child.once('error',reject);child.once('exit',()=>{
      assert.equal(held.closed,false);release(held,ports).then(resolve,reject);
    });});
    assert.equal(held.closed,true);assert.equal(child.exitCode,0);assert.deepEqual(signals,[]);
  }finally{
    if(child.exitCode===null&&child.signalCode===null)child.kill('SIGTERM');
    await done;
  }
});
