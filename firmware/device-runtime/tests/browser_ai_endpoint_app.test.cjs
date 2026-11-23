// 文件说明：执行正式页面的AI候选投影与本人接听；纯DOM测试不能替代实际端云媒体验证。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/app.js'),'utf8');
// page 不提供摄像头、网络或麦克风；固定本人操作只允许原已配对send。
function page(enabled=true){
  const elements=new Map(),sent=[];
  const context=vm.createContext({UrAIEndpointListen:enabled,TextEncoder,
    document:{getElementById(id){if(!elements.has(id))elements.set(id,{handlers:{},addEventListener(name,fn){this.handlers[name]=fn;}});return elements.get(id);}},
    window:{addEventListener(){}}});
  vm.runInContext(source,context);
  context.__send=value=>{sent.push(JSON.parse(JSON.stringify(value)));return true;};
  vm.runInContext('send=globalThis.__send',context);
  return {elements,sent,receive:value=>context.incoming(value),click:id=>elements.get(id).handlers.click()};
}
// fact 只有闭集本地阶段和本人的按钮资格，平台身份永不进入页面。
const fact=(phase=2)=>({kind:'endpointState',phase,canAccept:phase===2,canReject:phase===2});
test('本人显式接听和拒接只发固定单字段，不从邀请自动开麦',()=>{
  const p=page();p.receive(fact());assert.deepEqual(p.sent,[]);
  p.click('endpointAccept');p.click('endpointReject');
  assert.deepEqual(p.sent,[{kind:'endpointAccept'},{kind:'endpointReject'}]);
  p.receive(fact(6));p.click('endpointAccept');assert.equal(p.sent.length,2);
});
test('未装配、自由字段、错误阶段和伪造按钮资格拒绝',()=>{
  assert.throws(()=>page(false).receive(fact()));
  for(const value of [{...fact(),sessionId:'untrusted'}, {...fact(),phase:10}, {...fact(),phase:2.5},
    {...fact(),phase:'2'},{...fact(),canAccept:'true'},{...fact(1),canAccept:true}])
    assert.throws(()=>page().receive(value));
});
test('准备和双提交不是实际Active，任何投影不创建采集源',()=>{
  const p=page();for(let phase=0;phase<=9;++phase){p.receive(fact(phase));assert.equal(p.sent.length,0);}
  assert.equal(p.elements.get('endpointAccept').disabled,true);
  assert.equal(p.elements.get('endpointReject').disabled,true);
});
