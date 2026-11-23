// 文件说明：真实浏览器点击前只调整精确目标视口并核命中；不调用DOM click、不发送业务命令或修改设备状态。
'use strict';
const assert=require('node:assert/strict');
// prepareBrowserClick 在页面执行，返回闭集可点击事实；不把CLI成功或元素存在当实际事件已触发。
function prepareBrowserClick(selector){
 const allowed=['#activate','#start','#stop','#end','#temporaryResumeAI','#handoffEscalate','#handoffAccept','#handoffReject','#handoffCancel','#handoffHangup',
  '#endpointSourcePrepare','#endpointSourceCommit','#endpointSourceAbort']; // 本人接续同样走真实坐标点击，不开放任意selector。
 const element=allowed.includes(selector)?document.querySelector(selector):null;
 const facts={present:!!element,enabled:false,inViewport:false,hit:false,scrolled:false};
 if(!element)return facts;
 const before=element.getBoundingClientRect();
 const outside=before.top<0||before.left<0||before.bottom>innerHeight||before.right>innerWidth;
 if(outside){element.scrollIntoView({behavior:'instant',block:'center'});facts.scrolled=true;}
 const rect=element.getBoundingClientRect(),x=rect.left+rect.width/2,y=rect.top+rect.height/2;
 facts.enabled=element.disabled===false;
 facts.inViewport=rect.width>0&&rect.height>0&&x>=0&&y>=0&&x<innerWidth&&y<innerHeight;
 const hit=facts.inViewport?document.elementFromPoint(x,y):null;
 facts.hit=hit===element||(!!hit&&element.contains(hit));
 return facts;
}
// verifyBrowserClick 严格验证原页面五字段，未知/遮挡/禁用拒绝后续坐标点击，不自动重发业务操作。
function verifyBrowserClick(value){
 const keys=['present','enabled','inViewport','hit','scrolled'];
 assert.ok(value&&Object.keys(value).length===keys.length&&Object.keys(value).every(key=>keys.includes(key))&&keys.every(key=>typeof value[key]==='boolean'),'browser_click_observation_invalid');
 assert.ok(value.present&&value.enabled&&value.inViewport&&value.hit,'browser_click_not_actionable');return true;
}
module.exports={prepareBrowserClick,verifyBrowserClick};
