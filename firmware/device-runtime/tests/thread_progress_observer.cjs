// 文件说明：通过原桌面回环HTTP只读取固定线程计数；分为严格投影、受限GET和原实例生命周期观察。
'use strict';
const http = require('node:http'), assert = require('node:assert/strict');
const {uniqueJSON} = require('./tool_trace_projection.cjs');
// counters/stages 与C++本地诊断逐字段对应，非设备协议，也不把Taken计数当作工具执行事实。
const counters = Object.freeze(['callbacksEntered','callbacksLocked','callbacksExited','rejected','enqueueOverflow',
  'mqttPollEntered','mqttPollExited','takeEntered','takeExited','loopsEntered','loopsExited',
  'runtimeEntered','runtimeExited','diagnosticAttempts','diagnosticSent','diagnosticFailed',
  'messageValid','messageEnqueued','messageTaken','signalingValid','signalingEnqueued','signalingTaken',
  'otherValid','otherEnqueued','otherTaken']);
const callbackStages = Object.freeze(['unknown','idle','entered','locked','rejected','enqueued','exited']);
const hostStages = Object.freeze(['unknown','idle','bridge_input','output_gate','runtime_start','runtime_poll','receipts',
  'tool_command','text_result','content','message_diagnostic','media_commands','playback_frames','sleep','shutdown','stopped']);
const keys = Object.freeze(['kind','version','nonTransactional','callbackStage','hostStage','counters','overflow']);
// HTTP旁证仅允许固定失败类别和有界事实，不能携带响应头、正文、身份或原异常。
const httpReasons=Object.freeze(['none','http_status','content_type','transport','oversized','invalid_utf8']);
const httpKeys=Object.freeze(['reason','httpStatus','bodyBytes','headersObserved','endObserved','bodyComplete']);
// httpFacts重建可信metadata；未实际观察到的字段保持null，不借错误字符串补事实。
function httpFacts(value) {
  if(!exact(value,httpKeys)||!httpReasons.includes(value.reason)||
    !(value.httpStatus===null||(Number.isInteger(value.httpStatus)&&value.httpStatus>=100&&value.httpStatus<=599))||
    !(value.bodyBytes===null||(Number.isInteger(value.bodyBytes)&&value.bodyBytes>=0&&value.bodyBytes<=8193))||
    ['headersObserved','endObserved','bodyComplete'].some(name=>value[name]!==null&&typeof value[name]!=='boolean'))return null;
  return Object.fromEntries(httpKeys.map(name=>[name,value[name]]));
}
// exact 只接受自身字段恰好匹配的普通JSON对象，不对不可信值做字符串或数字转换。
function exact(value,names) {
  return !!value && typeof value==='object' && !Array.isArray(value) &&
    Object.keys(value).length===names.length && names.every(name=>Object.hasOwn(value,name));
}
// project 重建固定安全快照；独立原子读非事务一致，不附加跨字段顺序断言。
function project(source) {
  if(typeof source!=='string'||Buffer.byteLength(source)>8192) return null;
  try {
    const value=uniqueJSON(source);
    if(!exact(value,keys)||value.kind!=='threadProgress'||value.version!==1||value.nonTransactional!==true||
      !callbackStages.includes(value.callbackStage)||!hostStages.includes(value.hostStage)||typeof value.overflow!=='boolean'||
      !exact(value.counters,counters)||counters.some(name=>!Number.isInteger(value.counters[name])||
        value.counters[name]<0||value.counters[name]>4294967295)) return null;
    return {kind:value.kind,version:1,nonTransactional:true,callbackStage:value.callbackStage,hostStage:value.hostStage,
      counters:Object.fromEntries(counters.map(name=>[name,value.counters[name]])),overflow:value.overflow};
  } catch {return null;}
}
// readHTTP 仅访问调用者已持有实例的回环端口和两个固定GET；不跟随跳转、不输出body、地址或令牌。
function readHTTP(port,pathname,token,signal,metadata) {
  assert.ok(Number.isInteger(port)&&port>0&&port<=65535,'thread_progress_port_invalid');
  assert.ok(['/bootstrap','/diagnostics/progress'].includes(pathname),'thread_progress_path_invalid');
  return new Promise((resolve,reject)=>{
    let settled=false,length=0;
    const facts={reason:'none',httpStatus:null,bodyBytes:null,headersObserved:false,endObserved:false,bodyComplete:false};
    // notify失败只丢旁证，既不改原字符串返回值，也不追加请求或等待外部sink。
    function notify() {
      try {const pending=metadata?.({...facts});if(pending&&typeof pending.then==='function')Promise.resolve(pending).catch(()=>{});}
      catch {}
    }
    // fail保留第一次确切拒绝事实，destroy引发的后继error不得改成另一原因。
    function fail(reason,message) {if(settled)return;settled=true;facts.reason=reason;notify();reject(Error(message));}
    notify();
    const headers={Host:'127.0.0.1:'+port,'Sec-Fetch-Site':'same-origin',Origin:'http://127.0.0.1:'+port};
    if(token!==null) headers.Authorization='Bearer '+token;
    const request=http.request({hostname:'127.0.0.1',port,path:pathname,method:'GET',headers,signal,agent:false},response=>{
      const chunks=[];
      facts.headersObserved=true;facts.httpStatus=Number.isInteger(response.statusCode)&&response.statusCode>=100&&response.statusCode<=599?response.statusCode:null;
      facts.bodyBytes=0;notify();
      if(response.statusCode!==200) {
        fail('http_status','thread_progress_http_unavailable');response.destroy();request.destroy();return;
      }
      if(response.headers['content-type']?.split(';')[0]!=='application/json') {
        fail('content_type','thread_progress_http_unavailable');response.destroy();request.destroy();return;
      }
      response.on('data',chunk=>{
        if(settled)return;
        length+=chunk.length;
        facts.bodyBytes=Math.min(length,8193); // 8193表示已超限，不假称知道后续未读正文长度。
        if(length>8192) {fail('oversized','thread_progress_http_oversized');response.destroy();request.destroy();}
        else chunks.push(chunk);
      });
      response.once('error',()=>fail('transport','thread_progress_http_failed'));
      response.once('aborted',()=>fail('transport','thread_progress_http_failed'));
      response.once('end',()=>{
        if(settled)return;facts.endObserved=true;facts.bodyComplete=true;
        try {const body=new TextDecoder('utf-8',{fatal:true}).decode(Buffer.concat(chunks));settled=true;notify();resolve(body);}
        catch {fail('invalid_utf8','thread_progress_http_invalid_utf8');}
      });
    });
    request.once('error',()=>fail('transport','thread_progress_http_failed')); request.end();
  });
}
// create 默认关闭；上层仅为原child提供实时指纹核验，sample不驱动任何业务或自行创建周期任务。
function create({env={},port,originalAlive,emit,request=readHTTP}) {
  assert.ok(Number.isInteger(port)&&port>0&&port<=65535,'thread_progress_port_invalid');
  const enabled=env.UR_DEVICE_THREAD_PROGRESS_DIAGNOSTICS==='1';
  let closed=false, pending=null, controller=null, token=null;
  const counts={attempted:0,sampled:0,missing:0,sinkFailures:0,pendingSinks:0};
  // alive 每次读取前后确认原进程实例仍有效；未知也不允许把端口上的新进程当原设备。
  function alive() {try{return !closed && originalAlive()===true;}catch{return false;}}
  // get 每次有限HTTP读取在独立截止后取消，不延长模型或通话原业务预算。
  async function get(pathname,observation) {
    Object.assign(observation,{reason:'unknown',httpStatus:null,bodyBytes:null,headersObserved:null,endObserved:null,bodyComplete:null});
    controller=new AbortController(); const current=controller;
    const timer=setTimeout(()=>current.abort(),1500);
    try {return await request(port,pathname,token,current.signal,value=>{const safe=httpFacts(value);if(safe)Object.assign(observation,safe);});}
    catch(error) {if(current.signal.aborted)observation.reason='deadline';else if(observation.reason==='none')observation.reason='unknown';throw error;}
    finally {clearTimeout(timer); if(controller===current)controller=null;}
  }
  // collect 缺失明确输出sampled=false，不借用上一轮实态；sink错误仅成为旁证缺失。
  async function collect(sequence) {
    let facts=null;
    const observation={stage:'identity_before',reason:'identity_unconfirmed',httpStatus:null,bodyBytes:null,
      headersObserved:null,endObserved:null,bodyComplete:null};
    try {
      if(alive()) {
        if(token===null) {
          observation.stage='bootstrap';const source=await get('/bootstrap',observation);
          observation.reason='bootstrap_rejected';
          try {if(typeof source==='string'&&Buffer.byteLength(source)<=8192) {
            const bootstrap=uniqueJSON(source);
            if(exact(bootstrap,['token'])&&typeof bootstrap.token==='string'&&/^[a-f0-9]{64}$/.test(bootstrap.token)) token=bootstrap.token;
          }}catch {} // 清洗前JSON异常不能进入固定旁证。
        }
        if(token!==null) {
          observation.stage='identity_after_bootstrap';observation.reason='identity_unconfirmed';
          if(alive()) {
            observation.stage='diagnostic';const source=await get('/diagnostics/progress',observation);
            observation.stage='identity_after';observation.reason='identity_unconfirmed';
            if(alive()) {observation.stage='projection';observation.reason='shape_rejected';facts=project(source);
              if(facts){observation.stage='complete';observation.reason='none';}}
          }
        }
      }
    } catch {facts=null;}
    if(closed)return;
    if(facts)counts.sampled++;else counts.missing++;
    const result={diagnostic:'threadProgress',sample:sequence,sampled:!!facts,observation,...(facts?{facts}:{})};
    try {
      const delivered=emit(result);
      // sink可请求关闭；不能等待它再结束本采样，否则sink等待close会产生循环依赖。
      // 未完成sink显式计数，不把HTTP结束冒充所有外部日志已经写完。
      if(delivered&&typeof delivered.then==='function') {
        counts.pendingSinks++;
        Promise.resolve(delivered).then(()=>{counts.pendingSinks--;},()=>{counts.pendingSinks--;counts.sinkFailures++;});
      }
    }catch{counts.sinkFailures++;}
  }
  // sample 同一观察器至多一个在途读取，96次后停止；重入不补新任务、不消耗原设备工作队列。
  function sample() {
    if(!enabled||closed||pending||counts.attempted>=96)return Promise.resolve();
    const sequence=++counts.attempted;
    // 先占原在途槽再执行回调，实例未知的同步分支也不能由sink重入启动第二次采样。
    const task=Promise.resolve().then(()=>collect(sequence)); pending=task;
    return task.finally(()=>{if(pending===task)pending=null;});
  }
  // close 幂等中止原HTTP并等待其结算；不关闭child、媒体或任何业务socket。
  async function close() {closed=true;token=null;controller?.abort();await pending;}
  // statistics 只报告固定计数，调用者须单独记录真实业务结果，不能从计数推断成功。
  function statistics() {return {enabled,...counts,capacityReached:counts.attempted>=96,active:!!pending};}
  return {sample,close,statistics};
}
module.exports={project,create,readHTTP};
