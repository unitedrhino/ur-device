// 文件说明：直接执行正式网页连接回调，验证会议/人际同Peer五秒自然窗口，不模拟媒体恢复成功。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');

// fixture 从正式源码取回调与释放函数；可控时钟/定时器只用于确定性边界，不修改被测逻辑。
function fixture(human=false,talk=!human) {
  const source=fs.readFileSync(path.join(__dirname,'web/app.js'),'utf8');
  const release=source.slice(source.indexOf('function release(session) {'),source.indexOf('// stopped '));
  const handler=source.slice(source.indexOf('  view.peer.onconnectionstatechange = () => {'),source.indexOf('  await view.peer.setLocalDescription'));
  assert.ok(release.length>100&&handler.length>100,'formal_browser_handler_required');
  const timers=new Map(),allTimers=new Map(),sent=[];
  let now=100,sequence=0;
  const capture={enabled:false,readyState:'live',stop(){this.readyState='ended';}},remote={readyState:'live',stop(){this.readyState='ended';}};
  const peer={connectionState:'connected',closed:false,close(){this.closed=true;this.connectionState='closed';}};
  const view={peer,stream:{getTracks:()=>[capture]},remoteTrack:remote,connected:true,active:true,closed:false,human,talk,
    mediaRevision:1,releaseId:'1'};
  const context=vm.createContext({view,human,talk,value:{sessionId:'public-fixture'},views:new Map([['public-fixture',view]]),
    invitations:new Map(),retiringViews:new Map(),closed:false,performance:{now:()=>now},
    send:value=>{sent.push(value);return true;},setTimeout:(callback,milliseconds)=>{
      const timer=++sequence;const record={callback,milliseconds,at:now};timers.set(timer,record);allTimers.set(timer,record);return timer;
    },clearTimeout:timer=>timers.delete(timer)});
  vm.runInContext(release+'\n'+handler,context);
  return {view,peer,capture,remote,timers,allTimers,sent,
    state(value){peer.connectionState=value;peer.onconnectionstatechange();},
    at(value){now=value;},fire(timer){allTimers.get(timer).callback();},release(){vm.runInContext('release("public-fixture")',context);}};
}

for(const human of [false,true]) {
  const mode=human?'人际':'会议';
  test(`${mode}实际disconnected先保留原资源并通知HAL暂断，五秒固定截止`,()=>{
    const f=fixture(human);f.state('disconnected');
    assert.equal(f.view.closed,false,'natural_disconnect_must_not_release_immediately');
    assert.equal(f.capture.readyState,'live');assert.equal(f.capture.enabled,false);assert.equal(f.remote.readyState,'live');
    assert.equal(f.timers.size,1);assert.equal([...f.timers.values()][0].milliseconds,5000);
    assert.equal(f.sent.length,1);assert.equal(f.sent[0].kind,human?'callConnectivity':'ipcConnectivity');
    assert.equal(f.sent[0].state,'disconnected');assert.equal(f.sent[0].releaseId,'1');
    f.at(1000);f.state('disconnected');assert.equal(f.timers.size,1);assert.equal([...f.timers.values()][0].at,100);
  });
  test(`${mode}截止前原Peer恢复不重复握手或重开原本静音的采集`,()=>{
    const f=fixture(human),original=f.peer;f.state('disconnected');const timer=[...f.timers.keys()][0];
    f.at(5099);f.state('connected');assert.equal(f.view.closed,false);assert.equal(f.peer,original);assert.equal(f.capture.enabled,false);
    assert.equal(f.timers.size,0);assert.equal(f.sent.length,2);assert.equal(f.sent[1].kind,human?'callConnectivity':'ipcConnectivity');
    assert.equal(f.sent[1].state,'connected');assert.equal(f.sent[1].releaseId,'1');
    f.fire(timer);assert.equal(f.view.closed,false,'cancelled_timer_must_not_end_recovered_peer');
  });
  test(`${mode}精确五秒或更晚的连接回调不得绕过已到期窗口`,()=>{
    for(const at of [5100,5101]) {
      const f=fixture(human);f.state('disconnected');f.at(at);f.state('connected');
      assert.equal(f.view.closed,true);assert.equal(f.capture.readyState,'ended');
      assert.equal(f.sent.at(-1).kind,human?'callFailure':'ipcFailure');
    }
  });
  test(`${mode}超时、failed与closed均正式停采集且只发一次失败`,()=>{
    for(const cause of ['timeout','failed','closed']) {
      const f=fixture(human);f.state('disconnected');const timer=[...f.timers.keys()][0];
      if(cause==='timeout'){f.at(5100);f.fire(timer);}else f.state(cause);
      assert.equal(f.view.closed,true);assert.equal(f.capture.readyState,'ended');assert.equal(f.remote.readyState,'ended');
      const count=f.sent.length;f.fire(timer);assert.equal(f.sent.length,count);assert.equal(f.timers.size,0);
    }
  });
  test(`${mode}显式释放取消自然窗口且旧回调不再发送失败`,()=>{
    const f=fixture(human);f.state('disconnected');const timer=[...f.timers.keys()][0];f.release();
    assert.equal(f.timers.size,0);const count=f.sent.length;f.fire(timer);assert.equal(f.sent.length,count);
  });
  test(`${mode}未建立媒体不得借自然窗口等待或获得许可`,()=>{
    const f=fixture(human);f.view.active=false;f.state('disconnected');assert.equal(f.view.closed,true);assert.equal(f.timers.size,0);
  });
}

test('纯IPC预览保留原下一代次重协商，不借会议自然窗口跳过平台许可',()=>{
  const f=fixture(false,false);f.state('disconnected');
  assert.equal(f.view.closed,false);assert.equal(f.view.recovering,true);assert.equal(f.view.connected,false);
  assert.equal(f.timers.size,0);assert.equal(f.sent[0].kind,'ipcDisconnected');assert.equal(f.sent[0].mediaRevision,1);
});

module.exports={fixture};
