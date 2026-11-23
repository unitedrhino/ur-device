// 文件说明：原生单调时间仅作只读测量；页面不生成许可、不用浏览器时间替代缺失事实。
'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/app.js'),'utf8');
// page 隔离原页面与实际网络，时间事实只通过正式incoming入口。
function page(){const context=vm.createContext({TextEncoder,document:{getElementById(){return{addEventListener(){}};}},window:{addEventListener(){}}});
 vm.runInContext(source,context);vm.runInContext('responseId="reply"',context);
 return{receive:value=>context.incoming(value),state:()=>JSON.parse(vm.runInContext('JSON.stringify(metrics.lastReplyTiming ?? null)',context)),set:code=>vm.runInContext(code,context)};}
// timing 固定实际native时间域，responseId仅关联原回复，不含云端凭据。
function timing(){return{kind:'replyTiming',responseId:'reply',lastVoiceSentMs:1000,firstTextMs:1200,firstNetworkMs:1300,firstPlayableMs:1320};}
test('原回复接纳实际时间且缺失事实保持null',()=>{const p=page();p.receive(timing());assert.deepEqual(p.state(),timing());
 p.receive({...timing(),firstTextMs:null});assert.equal(p.state().firstTextMs,null);});
test('Realtime重叠保留原时间不改起点，迟到旧回复不覆盖当前',()=>{const p=page(),value={...timing(),lastVoiceSentMs:1400};p.receive(value);assert.deepEqual(p.state(),value);
 p.set('responseId="next";completedResponseId="reply"');p.receive({...timing(),firstTextMs:888});assert.deepEqual(p.state(),value);
 p.set('responseId="next";completedResponseId="next";metrics.lastMediaFinalResponseId="next"');p.receive({...timing(),firstTextMs:999});assert.deepEqual(p.state(),value);});
test('身份注入、未知数字与不可能的解码顺序拒绝且不替换事实',()=>{const p=page();p.receive(timing());
 for(const bad of [{secret:'private'},{firstTextMs:-1},{firstTextMs:1.5},{firstTextMs:'1200'},{firstTextMs:Number.MAX_SAFE_INTEGER+1},{firstPlayableMs:1299},{responseId:''}]){
  assert.throws(()=>p.receive({...timing(),...bad}));assert.deepEqual(p.state(),timing());}});
