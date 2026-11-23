// 文件说明：正式IPC网页的已消费名册展示测试；无网络、Peer或采集，也不代表真实端云验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 运行正式入口；媒体副作用若被意外调用必须使测试立即失败。
function fixture() {
  const sent = [];
  const elements = new Map();
  const element = () => ({children: [], textContent: '', append(value) { this.children.push(value); }, remove() {}});
  const context = vm.createContext({TextEncoder, WebSocket:{OPEN:1}, testSocket:{readyState:1,bufferedAmount:0,send(body){sent.push(JSON.parse(body));}},
    document: {createElement: element, getElementById(id) {
      if (!elements.has(id)) elements.set(id, element()); return elements.get(id);
    }}, window: {addEventListener() {}},
    navigator: {mediaDevices: {getUserMedia() { assert.fail('名册不得采集媒体'); }}},
    RTCPeerConnection: class { constructor() { assert.fail('名册不得创建Peer'); } },
    testElement: element,
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'web/app.js'), 'utf8'), context);
  vm.runInContext(`views.set('leg', {active:true,talk:true,closed:false,element:testElement(),
    peer:{close(){throw Error('名册不得关闭Peer');}},stream:null});`, context);
  vm.runInContext('socket=testSocket;',context);
  return {context,sent,
    async incoming(value) { context.value = value; await vm.runInContext('incoming(value)', context); },
    view() { return vm.runInContext("views.get('leg')", context); },
    set(field, value) { context.field = field; context.value = value;
      vm.runInContext("views.get('leg')[field]=value", context); },
  };
}
// notice 只构造本地显示shape；不是直接伪造平台participantChanged以绕过C++接收。
function notice(sequence = 3, participants = [{participantId:'101', tracks:[]}]) {
  return {kind:'ipcParticipants',sessionId:'leg',eventSeq:sequence,participants};
}
// track 提供公共完整元数据，任何展示也不意味着已订阅或获得话权。
function track(id = 't') {
  return {trackId:id,kind:'audio',sourceId:'mic',direction:'recvonly',codec:'opus',state:'active'};
}
test('加入、替换、离开显示仅原腿，JSON对象不保留可变别名', async () => {
  const f = fixture(), original = notice(3, [{participantId:'<img>😀',tracks:[track()]}]);
  await f.incoming(original);
  assert.equal(f.view().rosterSequence, 3);
  assert.equal(f.view().rosterElement.textContent.includes('<img>😀'), true);
  original.participants[0].tracks[0].state = 'stopped';
  assert.equal(f.view().participants[0].tracks[0].state, 'active');
  await f.incoming(notice(7));
  assert.equal(f.view().participants[0].participantId, '101');
  await f.incoming(notice(8, []));
  assert.equal(f.view().participants.length, 0);
  assert.equal(f.view().element.children.length, 1);
  assert.equal(f.view().closed, false);
});

// syncNotice 仅是共享C++状态投影，不是平台快照，也不能作为安装名册的依据。
function syncNotice(state='ready', eventSeq=0) {
  return {kind:'ipcSyncState',sessionId:'leg',state,eventSeq};
}
test('完整名册查询按钮只发本腿意图，Pending重复点击不产生请求或媒体', async()=>{
  const f=fixture(); await f.incoming(syncNotice());
  assert.equal(f.view().syncButton.disabled,false);
  f.view().syncButton.onclick(); f.view().syncButton.onclick();
  assert.deepEqual(f.sent,[{kind:'ipcSync',sessionId:'leg'}]);
  await f.incoming(syncNotice('pending'));
  assert.equal(f.view().syncButton.disabled,true);
  assert.equal(f.view().closed,false);
});
test('查询状态仅显示实际状态和水位，成功不伪造全量名册或Peer', async()=>{
  const f=fixture(); await f.incoming(notice(3));
  for(const state of ['pending','succeeded','failed','unknown','cancelled','rejected','ready']) await f.incoming(syncNotice(state,4));
  assert.equal(f.view().syncState,'ready');
  assert.equal(f.view().syncSequence,4);
  assert.equal(f.view().rosterSequence,3);
  assert.equal(f.view().participants.length,1);
  assert.equal(f.view().syncStatus.textContent.includes('恢复'),false);
  assert.equal(f.view().closed,false);
});
test('无能力、错腿、非Active、预览和终态不开放名册查询', async()=>{
  for(const [field,value] of [['active',false],['talk',false],['closed',true]]) {
    const f=fixture(); f.set(field,value); await f.incoming(syncNotice());
    assert.equal(f.view().syncButton,undefined); assert.equal(f.sent.length,0);
  }
  const f=fixture(); await f.incoming({...syncNotice(),sessionId:'other'});
  assert.equal(f.view().syncButton,undefined);
  await f.incoming(syncNotice('unavailable')); assert.equal(f.view().syncButton,undefined);
});
test('非法同步状态和多余字段不影响主通话或旧展示', async()=>{
  const f=fixture(); await f.incoming(syncNotice());
  for(const value of [{...syncNotice(),token:'forbidden'},syncNotice('established'),syncNotice('ready',-1),
    syncNotice('ready',1.5),syncNotice('ready',Number.MAX_SAFE_INTEGER+1),{...syncNotice(),eventSeq:undefined}]) await f.incoming(value);
  assert.equal(f.view().syncState,'ready'); assert.equal(f.view().syncSequence,0);
  assert.equal(f.view().closed,false); assert.equal(f.sent.length,0);
});
test('迟到低水位状态不回退Pending或实际名册，释放后旧按钮不发意图', async()=>{
  const f=fixture(); await f.incoming(syncNotice('succeeded',9));
  await f.incoming(syncNotice('pending',8)); assert.equal(f.view().syncState,'succeeded');
  const button=f.view().syncButton; f.set('closed',true); button.onclick();
  assert.equal(f.sent.length,0);
});
test('相同和较小水位不改新成员，别腿不能命中', async () => {
  const f = fixture(); await f.incoming(notice(10));
  for (const value of [notice(10, []),notice(9, []), {...notice(11, []),sessionId:'other'}]) await f.incoming(value);
  assert.equal(f.view().rosterSequence, 10);
  assert.equal(f.view().participants.length, 1);
});
test('未Active、关闭、纯预览拒绝旁路，不关闭主会话', async () => {
  for (const [field, value] of [['active',false], ['closed',true], ['talk',false]]) {
    const f = fixture(); f.set(field,value); await f.incoming(notice());
    assert.equal(f.view().rosterSequence, undefined);
    assert.equal(f.view().element.children.length, 0);
  }
});
test('非法成员与额外字段整条拒绝，不产生半个展示或终止Peer', async () => {
  const f = fixture(); await f.incoming(notice());
  const invalid = [null,{}, {participantId:'101',tracks:null}, {participantId:'',tracks:[]},
    {participantId:'101\0hidden',tracks:[]},{participantId:'\ud800',tracks:[]},
    {participantId:'a'.repeat(129),tracks:[]},{participantId:'界'.repeat(43),tracks:[]},
    {participantId:'101',tracks:[],token:'forbidden'}];
  for (const member of invalid) await f.incoming(notice(4, [member]));
  for (const value of [{...notice(4),secret:'forbidden'}, {...notice(4),eventSeq:0},
    {...notice(4),eventSeq:1.5}, {...notice(4),eventSeq:Number.MAX_SAFE_INTEGER+1},
    {...notice(4),participants:null},notice(4,[{participantId:'101',tracks:[]},{participantId:'101',tracks:[]}])]) await f.incoming(value);
  assert.equal(f.view().rosterSequence, 3);
  assert.equal(f.view().participants.length, 1);
  assert.equal(f.view().closed, false);
});
test('轨字段与跨成员重复严格拒绝，合法轨仅显示不执行驱动', async () => {
  const f = fixture();
  for (const value of [null,{}, {...track(),state:'unknown'}, {...track(),direction:'other'},
    {...track(),kind:'other'}, {...track(),codec:''}, {...track(),extra:true}]) {
    await f.incoming(notice(3,[{participantId:'101',tracks:[value]}]));
    assert.equal(f.view().rosterSequence, undefined);
  }
  await f.incoming(notice(3,[{participantId:'101',tracks:[track()]},{participantId:'102',tracks:[track()]}]));
  assert.equal(f.view().rosterSequence, undefined);
  await f.incoming(notice(3,[{participantId:'101',tracks:[track()]}]));
  assert.equal(f.view().participants[0].tracks[0].trackId,'t');
  assert.equal(f.view().closed,false);
});
test('32成员及8轨边界合法，超界不替换原展示', async () => {
  const f = fixture();
  const members = Array.from({length:32},(_,index)=>({participantId:String(index),tracks:[]}));
  await f.incoming(notice(3,members)); assert.equal(f.view().participants.length,32);
  await f.incoming(notice(4,[...members,{participantId:'overflow',tracks:[]} ]));
  assert.equal(f.view().rosterSequence,3);
  await f.incoming(notice(4,[{participantId:'101',tracks:Array.from({length:8},(_,index)=>track(String(index)))}]));
  assert.equal(f.view().participants[0].tracks.length,8);
  await f.incoming(notice(5,[{participantId:'101',tracks:Array.from({length:9},(_,index)=>track(String(index)))}]));
  assert.equal(f.view().rosterSequence,4);
});
