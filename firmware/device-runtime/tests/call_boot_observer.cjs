// 文件说明：正式人际页启动的可选透明观察及原daemon console缓存安全投影；不生成帧、音频、业务回执或定时器。
'use strict';
const assert=require('node:assert/strict');
// enabled 只有显式1才增加安装命令；缺省/0保留原open→click负载，未知开关在资源前拒绝。
function enabled(env=process.env){assert.ok(env.UR_CALL_BOOT_DIAGNOSTICS===undefined||['0','1'].includes(env.UR_CALL_BOOT_DIAGNOSTICS),'call_boot_diagnostic_invalid');return env.UR_CALL_BOOT_DIAGNOSTICS==='1';}
// install 完整序列化至原页；原标识仅闭包比较，console仅固定事件且最多96条（末条为截断）。
function install(scope,readAudio=()=>scope.callAudioNode,readContext=()=>null){
  if(scope.callBootObserver)return false;
  for(const key of ['incoming','send','callAudioReady','paintBoundary','requestAnimationFrame','cancelAnimationFrame'])if(typeof scope[key]!=='function')throw Error('call_boot_original_missing');
  const events=[],originals=[],ports=[],frames=new Map(),origin=scope.performance.now(),console=scope.console; // 有界本页观察，不共享其它实例票据。
  let current=null,active=null,stopped=false,truncated=false,unknown=false,last=0; // 未知不能升格为成功。
  // append 只记录单调时差与实际环境，不保存报文/身份；sink异常仅使诊断未知。
  function append(phase,record,applied=null,timestamp=null,matchedOriginal=null){
    if(stopped||truncated)return;
    try{
      const elapsed=scope.performance.now()-origin,valid=typeof elapsed==='number'&&Number.isFinite(elapsed)&&elapsed>=last&&elapsed<=60000;
      if(valid)last=elapsed;else unknown=true;
      let visibility=null,focused=null,topLevel=null,contextState=null; // 失读保持null，不猜背景/静音。
      try{const doc=scope.document;visibility=['visible','hidden'].includes(doc?.visibilityState)?doc.visibilityState:null;const focus=doc?.hasFocus();focused=typeof focus==='boolean'?focus:null;topLevel=scope.self&&scope.top?scope.self===scope.top:null;const state=readContext();contextState=['running','suspended','closed','interrupted'].includes(state)?state:null;}catch{}
      const relative=typeof timestamp==='number'?timestamp-origin:null; // 原RAF timestamp不变，只投影同一origin时差。
      if(events.length===95){phase='truncated';truncated=unknown=true;}
      const value={marker:'call_boot_observer',phase,elapsedMs:valid?elapsed:null,actualFrames:record?record.frames:null,
        initialIdle:record?record.initial:null,matchedOriginal,applied,visibility,focused,topLevel,contextState,
        rafTimestampMs:relative!==null&&Number.isFinite(relative)&&relative>=0&&relative<=60000?relative:null};
      events.push(value);console.info.call(console,JSON.stringify(value));
    }catch{unknown=true;}
  }
  // wrap 保持原receiver、所有参数与原返回值/异常，不用观察Promise替代业务Promise。
  function wrap(key,invoke){const original=scope[key],wrapper=function(...args){return invoke(original,this,args);};originals.push({key,original,wrapper});scope[key]=wrapper;}
  // observe 只旁观原Promise终态，所有旁证异常隔离，不追加业务等待。
  function observe(result,success,failure){try{if(result&&typeof result.then==='function')result.then(()=>{try{success();}catch{unknown=true;}},()=>{try{failure();}catch{unknown=true;}});}catch{unknown=true;}return result;}
  wrap('incoming',(original,self,args)=>{try{const value=args[0];if(value?.kind==='callOutputGate'){current={ticket:value.gateTicket,epoch:value.peerEpoch,session:value.sessionId,enabled:value.enabled,initial:value.sessionId===''&&value.peerEpoch==='0'&&value.enabled===false,frames:0};append('gate_received',current);}}catch{unknown=true;}return original.apply(self,args);});
  wrap('send',(original,self,args)=>{const record=current,result=original.apply(self,args);try{const value=args[0];if(value?.kind==='ready')append('ready_sent',null,result===true);if(value?.kind==='callOutputApplied')append('bridge_applied',record,typeof value.applied==='boolean'?value.applied:null,null,!!record&&value.sessionId===record.session&&value.peerEpoch===record.epoch&&value.gateTicket===record.ticket&&value.enabled===record.enabled);}catch{unknown=true;}return result;});
  wrap('callAudioReady',(original,self,args)=>observe(original.apply(self,args),()=>{append('audio_ready',null,true);const port=readAudio()?.port;if(!port||ports.some(value=>value.port===port))return;const listener=event=>{try{if(event?.data?.kind==='callAudioGated'){const value=event.data,record=current;append('audio_gated',record,null,null,!!record&&value.ticket===record.ticket&&value.peerEpoch===record.epoch&&value.enabled===record.enabled);}if(event?.data?.kind==='callAudioFailed')append('audio_failed',current,false);}catch{unknown=true;}};port.addEventListener('message',listener);ports.push({port,listener});},()=>append('audio_ready_failed',null,false)));
  wrap('paintBoundary',(original,self,args)=>{const record=current,previous=active;active=record;append('paint_begin',record);try{return observe(original.apply(self,args),()=>append('paint_settled',record,true),()=>append('paint_settled',record,false));}finally{active=previous;}});
  wrap('requestAnimationFrame',(original,self,args)=>{const record=active,callback=args[0];if(!record||frames.size>=16){if(record){unknown=true;append('unknown',record);}return original.apply(self,args);}let handle;const observed=function(...actual){frames.delete(handle);record.frames=Math.min(2,record.frames+1);append('raf_actual',record,true,actual[0]);const previous=active;active=record;try{return callback.apply(this,actual);}finally{active=previous;}};handle=original.apply(self,[observed,...args.slice(1)]);frames.set(handle,record);append('raf_scheduled',record);return handle;});
  wrap('cancelAnimationFrame',(original,self,args)=>{const record=frames.get(args[0]);if(record){frames.delete(args[0]);append('raf_cancelled',record);}return original.apply(self,args);});
  // snapshot/stop只处理观察器副本和监听器，不关闭AudioContext/Worklet或取消原帧。
  const snapshot=()=>({sampled:events.length>0&&!unknown,truncated,events:events.map(value=>({...value}))});
  const stop=()=>{if(!stopped){stopped=true;for(const {key,original,wrapper}of originals)if(scope[key]===wrapper)scope[key]=original;for(const {port,listener}of ports)try{port.removeEventListener('message',listener);}catch{unknown=true;}}return snapshot();};
  scope.callBootObserver={snapshot,stop};return true;
}
// installSource 从真实模块函数生成原页安装表达式；额外eval明确改变click前准备负载，非原无observer等价对照。
const installSource='('+install.toString()+')(globalThis,()=>callAudioNode,()=>typeof callAudio==="undefined"?null:callAudio?.state)';
// unknown 返回固定未采到状态，不带CLI自由输出或异常。
function unknown(){return {sampled:false,truncated:false,events:[]};}
// own 仅读取自有data属性；getter/proxy异常由投影统一降为未知。
function own(value,key){if(!value||typeof value!=='object')return undefined;const field=Object.getOwnPropertyDescriptor(value,key);if(field&&!Object.hasOwn(field,'value'))throw Error('call_boot_getter');return field?.value;}
// projectCache 识别v0.27原console JSON包装，忽略其它自由日志，marker payload严格固定字段/枚举/数量。
function projectCache(raw){try{
  const success=own(raw,'success');if(success!==undefined&&success!==true)return unknown(); // 原失败响应不能借data伪装可读缓存。
  const wrapped=own(raw,'data'),messages=own(wrapped===undefined?raw:wrapped,'messages');if(!Array.isArray(messages)||messages.length>1000)return unknown();
  const events=[],keys=['marker','phase','elapsedMs','actualFrames','initialIdle','matchedOriginal','applied','visibility','focused','topLevel','contextState','rafTimestampMs']; // 固定12字段，不透传args/url或metadata。
  const phases=['ready_sent','gate_received','audio_ready','audio_ready_failed','audio_gated','audio_failed','paint_begin','paint_settled','raf_scheduled','raf_actual','raf_cancelled','bridge_applied','unknown','truncated'];
  let last=0,truncated=false,invalid=false; // 原单调水位，任何非法marker不能借邻近合法事件变成功。
  for(const message of messages){const text=own(message,'text');if(typeof text!=='string')continue;let value;try{value=JSON.parse(text);}catch{if(text.startsWith('{"marker":"call_boot_observer"'))return unknown();continue;}if(own(value,'marker')!=='call_boot_observer')continue;
    if(text.length>4096||events.length>=96)return {...unknown(),truncated:true};
    if(JSON.stringify(value)!==text||Object.keys(value).sort().join(',')!==[...keys].sort().join(',')||!phases.includes(own(value,'phase')))return unknown(); // 原固定序列化；重复键/非原marker形状均拒绝。
    const event={};for(const key of keys)event[key]=own(value,key);
    for(const key of ['initialIdle','matchedOriginal','applied','focused','topLevel'])if(event[key]!==null&&typeof event[key]!=='boolean')return unknown();
    for(const key of ['elapsedMs','rafTimestampMs'])if(event[key]!==null&&(typeof event[key]!=='number'||!Number.isFinite(event[key])||event[key]<0||event[key]>60000))return unknown();
    if(event.elapsedMs===null||event.elapsedMs<last)invalid=true;else last=event.elapsedMs;
    if(event.actualFrames!==null&&(!Number.isInteger(event.actualFrames)||event.actualFrames<0||event.actualFrames>2))return unknown();
    if(event.visibility!==null&&!['visible','hidden'].includes(event.visibility)||event.contextState!==null&&!['running','suspended','closed','interrupted'].includes(event.contextState))return unknown();
    if(event.phase==='truncated')truncated=true;if(event.phase==='unknown')invalid=true;
    events.push(event);
  }
  return {sampled:events.length>0&&!invalid&&!truncated,truncated,events};
}catch{return unknown();}}
// originalSocket 只解析原具名session和任务显示自有短alias；真实目录/目标socket均拒软链及越界，不创建任何资源。
function originalSocket(session,env,io=require('node:fs')){
  const path=require('node:path'),base=path.resolve(__dirname,'../../../.temp/task143-ipc-env');
  assert.match(session,/^transfer-[012]-[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/);
  const alias=env.AGENT_BROWSER_SOCKET_DIR;assert.ok(typeof alias==='string'&&/^\/tmp\/task143-tr-[1-9][0-9]*-[a-f0-9]{8}$/.test(alias));
  const link=io.lstatSync(alias);assert.ok(link.isSymbolicLink()&&link.uid===process.getuid());
  const root=io.readlinkSync(alias),relative=path.relative(base,root);assert.match(relative,/^s13-transfer-work-(?:(?:success|reject|cancel|timeout)-1|success-[23])\/headed\/socket$/); // success3仅Root已确定的新取证场地，不创建/续用意图。
  assert.equal(root,path.join(base,relative));assert.equal(io.realpathSync(alias),root);
  for(const directory of [path.dirname(path.dirname(root)),path.dirname(root),root]){const stat=io.lstatSync(directory);assert.ok(stat.isDirectory()&&!stat.isSymbolicLink()&&stat.uid===process.getuid()&&(stat.mode&0o777)===0o700);}
  const target=path.join(root,session+'.sock'),stat=io.lstatSync(target);assert.ok(stat.isSocket()&&!stat.isSymbolicLink()&&stat.uid===process.getuid());
  const short=path.join(alias,session+'.sock');assert.ok(Buffer.byteLength(short)<=107);return short;
}
// readOriginalCache 沿v0.27原UnixSocket单条换行JSON action=console，绕过会自动ensure_daemon的CLI。
// 两秒绝对IO期限仅用于首失败之后取证，不改变任何业务deadline；一次连接/零重试，结束即销毁本连接。
async function readOriginalCache(session,env,options={}){
  let socket;try{socket=originalSocket(session,env,options.io);}catch{return unknown();}
  return new Promise(resolve=>{let connection,timer,settled=false,total=0;const chunks=[]; // 最多262144字节/4096分片，仅临时内存不写原console正文。
    const finish=value=>{if(settled)return;settled=true;clearTimeout(timer);connection?.destroy();resolve(value);}; // sink错误不抛给首业务失败。
    timer=setTimeout(()=>finish(unknown()),2000);
    try{connection=(options.connect||require('node:net').createConnection)({path:socket});
      connection.once('connect',()=>{try{assert.equal(originalSocket(session,env,options.io),socket);connection.write('{"id":"call-boot-cache","action":"console"}\n');}catch{finish(unknown());}}); // 原alias/目标在连接后重核，变化不发送查询。
      connection.on('data',chunk=>{try{total+=chunk.length;if(total>262144||chunks.length>=4096){finish({...unknown(),truncated:true});return;}chunks.push(chunk);if(chunk.indexOf(10)<0)return;const bytes=Buffer.concat(chunks,total),end=bytes.indexOf(10);
        const text=new TextDecoder('utf-8',{fatal:true}).decode(bytes.subarray(0,end)),response=JSON.parse(text);if(own(response,'success')!==true){finish(unknown());return;}finish(projectCache(response));
      }catch{finish(unknown());}});
      connection.once('error',()=>finish(unknown()));connection.once('end',()=>finish(unknown()));connection.once('close',()=>finish(unknown()));
    }catch{finish(unknown());}
  });
}
module.exports={enabled,install,installSource,projectCache,readOriginalCache,originalSocket,unknown};
