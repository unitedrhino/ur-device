// 文件说明：真实UDP页面的回复控制回归；控制目标来自C++投影，不借尚未开始或迟到的播放对象授权。
'use strict';
const test = require('node:test'),assert = require('node:assert/strict'),vm = require('node:vm'),fs = require('node:fs'),path = require('node:path');
// page 为原脚本提供最小本地桥和按钮，保留实际incoming、send与点击处理。
function page() {
  const elements = new Map(),sent = [],handlers = new Map();
  const context = vm.createContext({TextEncoder,WebSocket:{OPEN:1},window:{addEventListener(){}},
    document:{getElementById(id){if(!elements.has(id))elements.set(id,{addEventListener:(event,fn)=>handlers.set(id+':'+event,fn)});return elements.get(id);}}});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/app.js'),'utf8'),context);
  context.emitRaw = value => sent.push(JSON.parse(value));
  vm.runInContext('socket={readyState:1,bufferedAmount:0,send:emitRaw}',context);
  return {sent,incoming:context.incoming,click:()=>handlers.get('cancel:click')(),set:code=>vm.runInContext(code,context)};
}
// state 是原可信本地桥的核心投影，不属于云端Topic信封或设备自报授权。
const state = responseId => ({kind:'state',established:true,interactionMode:'fullDuplex',text:'已建立',responseId});
test('工具等待尚无播放时UDP取消原核心回复', () => {
  const p = page();p.incoming(state('current'));p.click();
  assert.deepEqual(p.sent,[{kind:'cancel',id:'current'}]);
});
test('新回复与旧播放ID不同，退休核心目标后不能复活旧播放取消资格', () => {
  const p = page();p.set('responseId="old-playback"');p.incoming(state('new-core'));p.click();
  p.incoming(state(''));p.click();assert.deepEqual(p.sent,[{kind:'cancel',id:'new-core'}]);
});
test('未知和非法UDP控制字段严格拒绝，旧未投影宿主保留原播放取消', () => {
  const p = page();
  for(const bad of [{...state('r'),responseId:1},{...state('r'),responseId:'r\n'},
    {...state('r'),private:'not-a-projection'}]) assert.throws(()=>p.incoming(bad));
  p.set('responseId="legacy-playback"');p.incoming({kind:'state',established:true,interactionMode:'halfDuplex',text:'已建立'});
  p.click();assert.deepEqual(p.sent,[{kind:'cancel',id:'legacy-playback'}]);
});
