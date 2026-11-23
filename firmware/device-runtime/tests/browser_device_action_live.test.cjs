// 文件说明：五轮真实设备动作验收判据的离线反例，不以这些替身结果替代正式MCP或真实媒体。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict'), vm = require('node:vm');
const {volumes,installActionObserver,actionDiagnostic,verifyActionRound} = require('./browser_device_action_live.cjs');
// round提供最小成功事实：原工具启动→实际驱动应用→原结果→同回复真实播放完成。
function round() {
  const before = {submitted:0,accepted:0,completed:0,cancelled:0,completedID:'',actions:[],tools:[]};
  const after = {overflow:false,submitted:1,accepted:1,completed:1,cancelled:0,completedID:'reply',active:false,
    captureOpen:false,pending:0,playableResponse:'reply',consumedResponse:'reply',voicedFrames:2,
    actions:[{code:200,volume:40,sequence:2}],
    tools:[{id:'tool',response:'reply',state:'started',name:'device_action_send',sequence:1},
      {id:'tool',response:'reply',state:'succeeded',sequence:3}]};
  return {before,after};
}
test('五轮增益固定且每轮真的变化', () => {
  assert.deepEqual(volumes,[40,65,20,80,40]);assert.ok(Object.isFrozen(volumes));
  assert.ok(volumes.every((value,index) => index === 0 || value !== volumes[index - 1]));
});
test('设备实际动作、原MCP回复、真实连续消费三者齐全才通过，核验不改原事实', () => {
  const f = round(), copy = JSON.stringify(f), result = verifyActionRound(f.before,f.after,40);
  assert.equal(result.passed,true);assert.equal(result.actualDriverApplied,true);assert.equal(JSON.stringify(f),copy);
  assert.ok(!JSON.stringify(result).includes('reply'));assert.ok(!JSON.stringify(result).includes('tool'));
});
test('允许模型先读取物模型，但不接受另一服务同后缀或无实际驱动的工具成功', () => {
  const f = round(); f.after.tools.unshift({id:'model',response:'reply',state:'started',name:'get_thing_model',sequence:0},
    {id:'model',response:'reply',state:'succeeded',sequence:0});
  assert.equal(verifyActionRound(f.before,f.after,40).passed,true);
  f.after.tools[2].name = 'other';assert.throws(() => verifyActionRound(f.before,f.after,40));
});
test('设备完成通知可先于MCP启动通知到达，工具自身仍须启动和终态完整', () => {
  const f = round();f.after.actions[0].sequence = 0;
  assert.equal(verifyActionRound(f.before,f.after,40).passed,true);
  f.after.tools.reverse();assert.throws(() => verifyActionRound(f.before,f.after,40));
});
test('每个通过条件都有失败反例', () => {
  for (const change of [
    f => f.after.overflow = true, f => f.after.submitted = 2, f => f.after.accepted = 0,
    f => f.after.completed = 2, f => f.after.completedID = '', f => f.after.cancelled = 1,
    f => f.after.active = true, f => f.after.captureOpen = true, f => f.after.pending = 1,
    f => f.after.actions = [], f => f.after.actions[0].code = 504, f => f.after.actions[0].volume = 65,
    f => f.after.tools[0].response = 'other',
    f => f.after.tools[1].state = 'unknown', f => f.after.tools[1].id = 'orphan',
    f => f.after.tools.pop(), f => f.after.tools.push({...f.after.tools[1]}),
    f => f.after.tools.push({...f.after.tools[0]}), f => f.after.playableResponse = 'old',
    f => f.after.consumedResponse = 'old', f => f.after.voicedFrames = 1,
    f => f.before.actions = [{volume:40}],
  ]) {const f = round();change(f);assert.throws(() => verifyActionRound(f.before,f.after,40));}
});
test('透明观察器不重发原事件，不保留参数、私钥或自由文本，并有界拒绝溢出', () => {
  const context = {count:0,incoming:() => ++context.count};vm.createContext(context);
  vm.runInContext('(' + installActionObserver.toString() + ')()',context);
  const event = {kind:'toolStatus',state:'started',name:'untrusted.device_action_send',id:'tool',responseId:'reply',private:'never-retain'};
  assert.equal(context.incoming(event),1);assert.equal(context.count,1);
  assert.equal(context.__urDeviceAction.tools[0].name,'other');
  assert.ok(!JSON.stringify(context.__urDeviceAction).includes('never-retain'));
  for (let i = 0; i < 100; ++i) context.incoming(event);
  assert.equal(context.__urDeviceAction.tools.length,64);assert.equal(context.__urDeviceAction.overflow,true);
});
test('失败诊断不反射恶意值，空ID不伪造同回复事实且长度有界', () => {
  const privateValue = {secret:'do-not-log'}, f = round();
  f.after.submitted = privateValue;f.after.active = privateValue;f.after.completedID = '';
  f.after.actions = Array(80).fill({code:privateValue,volume:privateValue});
  f.after.tools = Array(150).fill({id:privateValue,name:privateValue,state:privateValue,response:''});
  const value = actionDiagnostic(f.after);assert.equal(value.submitted,null);assert.equal(value.active,null);
  assert.equal(value.actions.length,32);assert.equal(value.tools.length,64);
  assert.ok(value.tools.every(tool => !tool.sameReply));assert.ok(!JSON.stringify(value).includes('do-not-log'));
});
