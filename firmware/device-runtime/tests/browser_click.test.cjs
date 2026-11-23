// 文件说明：点击准备使用实际导出页面函数；覆盖离屏滚动、命中、遮挡、禁用和非法观察，不冒充现场浏览器。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm');
const {prepareBrowserClick,verifyBrowserClick}=require('./browser_click.cjs');
// fixture 提供可控几何端口，禁止被测准备调用DOM click或读身份正文。
function fixture({outside=false,disabled=false,covered=false,selector='#handoffEscalate'}={}){
 let scrolled=0;const rect=()=>({left:0,right:100,top:outside?1000:10,bottom:outside?1030:40,width:100,height:30});
 const button={disabled,getBoundingClientRect:rect,scrollIntoView(options){assert.deepEqual(JSON.parse(JSON.stringify(options)),{behavior:'instant',block:'center'});++scrolled;outside=false;},contains(){return false;},click(){throw Error('must_not_fake_click');}};
 const ctx=vm.createContext({innerWidth:800,innerHeight:600,document:{querySelector(value){return value===selector?button:null;},elementFromPoint(){return covered?{}:button;}}});
 return {read:selector=>JSON.parse(JSON.stringify(vm.runInContext('('+prepareBrowserClick.toString()+')('+JSON.stringify(selector)+')',ctx))),scrolls:()=>scrolled};
}
test('离屏精确滚动后核中心命中，不调用DOM点击',()=>{const f=fixture({outside:true}),facts=f.read('#handoffEscalate');assert.deepEqual(facts,{present:true,enabled:true,inViewport:true,hit:true,scrolled:true});assert.equal(f.scrolls(),1);assert.equal(verifyBrowserClick(facts),true);});
test('视口内不滚动，覆盖/禁用/未知目标均拒绝',()=>{const f=fixture();assert.equal(verifyBrowserClick(f.read('#handoffEscalate')),true);assert.equal(f.scrolls(),0);for(const facts of [fixture({covered:true}).read('#handoffEscalate'),fixture({disabled:true}).read('#handoffEscalate'),f.read('#private')])assert.throws(()=>verifyBrowserClick(facts),/browser_click_not_actionable/);});
test('额外身份或非布尔状态不获点击资格',()=>{const facts=fixture().read('#handoffEscalate');for(const change of [{targetId:'private'},{hit:1},{present:null}])assert.throws(()=>verifyBrowserClick({...facts,...change}),/browser_click_observation_invalid/);});
test('endpoint三个固定本人按钮复用原几何核验，不开放自由selector或DOM点击',()=>{
 for(const selector of ['#endpointSourcePrepare','#endpointSourceCommit','#endpointSourceAbort']){const f=fixture({outside:true,selector});assert.equal(verifyBrowserClick(f.read(selector)),true);assert.equal(f.scrolls(),1);}
 assert.throws(()=>verifyBrowserClick(fixture({selector:'#private'}).read('#private')),/browser_click_not_actionable/);
});
