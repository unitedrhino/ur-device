// 文件说明：原输入旁观器的三个字面布尔VM回归；只使用合成文本，不运行浏览器、模型或真实设备。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),vm=require('node:vm');
const {installInputDiagnostic}=require('./mcp_file_media_live.cjs');
// fixture提供原incoming唯一端口；显式记录对象/this/返回或异常，不模拟任何业务成功。
function fixture(enabled,original){if(arguments.length===0)enabled=true;const calls=[],result={original:true},realm={incoming:original||function(value){calls.push({value,receiver:this});return result;}},initial=realm.incoming;
 const installed=vm.runInNewContext('('+installInputDiagnostic.toString()+')('+JSON.stringify(enabled)+')',realm);return {realm,initial,calls,result,installed,observer:realm.__urMcpInputDiagnostic};}
// send仅模拟原incoming同源用户文本框架，输出事实不包含传入的合成正文。
function send(f,text){return f.realm.incoming({kind:'text',text:'\n用户：'+text+'\n助手：'});}
// plain消除VM跨realm原型差异，只用于比较固定事实对象。
function plain(value){return JSON.parse(JSON.stringify(value));}
test('默认关闭不包装，不建立观察对象，也不改变原返回',()=>{
 for(const enabled of [false,undefined,'1',1,null]){const f=fixture(enabled);assert.equal(f.installed,false);assert.equal(f.observer,undefined);assert.equal(f.realm.incoming,f.initial);const value={kind:'text',text:'合成'};assert.equal(f.realm.incoming(value),f.result);assert.equal(f.calls.length,1);assert.equal(f.calls[0].value,value);}
});
test('新字面字段在默认和arm清空时均为明确false，arm不读取输入',()=>{
 const f=fixture();for(const facts of [f.observer.facts(),(f.observer.arm('合成期望'),f.observer.facts())])for(const key of ['suspendTokenPresent','releaseTokenPresent','doNotReleasePhrasePresent'])assert.equal(facts[key],false);
 assert.equal(f.calls.length,0);send(f,'暂停，不要释放');assert.equal(f.observer.facts().suspendTokenPresent,true);f.observer.arm('新期望');const reset=f.observer.facts();assert.equal(reset.known,false);assert.equal(reset.observations,0);for(const key of ['suspendTokenPresent','releaseTokenPresent','doNotReleasePhrasePresent'])assert.equal(reset[key],false);
});
test('字面匹配与标点归一化严格分开，不宣称自然语言语义等价',()=>{
 const cases=[['暂停，不要释放',true,true,true],['不要立即释放',false,true,false],['无需释放',false,true,false],['不要，释放',false,true,true],['不、要，释 放',false,false,true],['暂、停',false,false,false],['只是引用“不要释放”',false,true,true],['暂时停止输出',false,false,false]];
 for(const [text,suspend,release,phrase]of cases){const f=fixture();f.observer.arm(text);send(f,text);const x=f.observer.facts();assert.equal(x.known,true);assert.equal(x.normalizedExactMatch,true);assert.equal(x.suspendTokenPresent,suspend);assert.equal(x.releaseTokenPresent,release);assert.equal(x.doNotReleasePhrasePresent,phrase);}
});
test('只观察原同源用户文本，不把助手后段或不同kind计为用户词',()=>{
 const f=fixture();f.observer.arm('合成');for(const value of [null,{kind:'audio',text:'\n用户：暂停释放'},{kind:'text',text:'用户：暂停释放'},{kind:'text',text:'\n助手：暂停释放'},{kind:'text',text:1}])f.realm.incoming(value);assert.equal(f.observer.facts().known,false);assert.equal(f.observer.facts().observations,0);
 f.realm.incoming({kind:'text',text:'\n用户：合成\n助手：暂停，不要释放'});const x=f.observer.facts();assert.equal(x.known,true);for(const k of ['suspendTokenPresent','releaseTokenPresent','doNotReleasePhrasePresent'])assert.equal(x[k],false);
});
test('开启后原this/对象/唯一调用/返回和throw均保持，不吞异常',()=>{
 const f=fixture();f.observer.arm('暂停');const receiver={synthetic:true},value={kind:'text',text:'\n用户：暂停'};assert.equal(f.realm.incoming.call(receiver,value),f.result);assert.equal(f.calls.length,1);assert.equal(f.calls[0].value,value);assert.equal(f.calls[0].receiver,receiver);
 const failure=Error('synthetic-private-failure');let calls=0;const q=fixture(true,()=>{calls++;throw failure;});q.observer.arm('暂停');const before=plain(q.observer.facts());assert.throws(()=>send(q,'暂停'),e=>e===failure);assert.equal(calls,1);assert.deepEqual(plain(q.observer.facts()),before);
});
test('原八次/8192界限、release和私密正文无回显保持',()=>{
 const secret='synthetic-private-body',f=fixture();f.observer.arm('暂停，不要释放'+secret);send(f,'暂停，不要释放'+secret);assert.ok(!JSON.stringify(f.observer.facts()).includes(secret));for(let n=0;n<8;n++)send(f,'合成');assert.equal(f.observer.facts().known,false);f.observer.arm('合成');send(f,'x'.repeat(8193));assert.equal(f.observer.facts().known,false);
 f.observer.arm('合成');send(f,'合成');const before=plain(f.observer.facts());f.observer.release();send(f,'暂停，不要释放');assert.deepEqual(plain(f.observer.facts()),before);assert.throws(()=>f.observer.arm('x'.repeat(4097)),/invalid_fixed_fixture/);
});
