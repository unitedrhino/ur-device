// 文件说明：隔离Chromium执行原生产资产，核真实浏览器轨释放；本地桥模拟不是平台错误ACK或硬件AEC验收。
'use strict';
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),crypto=require('node:crypto');
const {execFile}=require('node:child_process'),{promisify}=require('node:util'),assert=require('node:assert/strict');
const execute=promisify(execFile),root=path.resolve(__dirname,'../../..');
// browserArgs 按已安装CLI的逗号分隔合同传参，不让fake-media标志拼成一个无效Chromium参数。
const browserArgs=['--no-sandbox','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required'].join(',');
// verifyTracks 只认可实际MediaStreamTrack及对应种类的全部状态，不拿UI按钮或HAL计数代替停止。
function verifyTracks(facts,kinds,state){
  assert.ok(Array.isArray(facts.tracks)&&facts.tracks.length===kinds.length,'actual_track_count_required');
  assert.deepEqual(facts.tracks.map(track=>track.kind).sort(),[...kinds].sort(),'actual_track_kinds_required');
  assert.ok(facts.tracks.every(track=>track.native===true&&track.state===state),'actual_track_state_required');
}
// frame 仅编码测试服务器发出的短本地文本／关闭帧，不承载设备身份或平台信令。
function frame(payload,opcode=1){
  const bytes=Buffer.from(payload);assert.ok(bytes.length<65536,'local_frame_budget');
  const header=bytes.length<126?Buffer.from([0x80|opcode,bytes.length]):Buffer.from([0x80|opcode,126,bytes.length>>8,bytes.length&255]);
  return Buffer.concat([header,bytes]);
}
// localAssets 原样提供指定生产文件，回环服务只绑定动态端口；所有socket均由本测试登记并关闭。
async function localAssets(kind){
  const folder=path.join(root,'firmware/device-runtime',kind==='ai'?'adapters/desktop/web':'desktop-ipc/web');
  const files=new Map(['index.html','app.js','app.css','audio-worklet.js','client-theme.js'].filter(name=>fs.existsSync(path.join(folder,name))).map(name=>['/'+name,fs.readFileSync(path.join(folder,name))]));
  const sockets=new Set();let peer;
  const server=http.createServer((request,response)=>{
    if(request.url==='/bootstrap'){response.writeHead(200,{'content-type':'application/json'});response.end(JSON.stringify({token:'a'.repeat(64)}));return;}
    const name=request.url==='/'?'/index.html':request.url,bytes=files.get(name);
    if(!bytes){response.writeHead(404);response.end();return;}
    response.writeHead(200,{'content-type':name.endsWith('.js')?'text/javascript':name.endsWith('.css')?'text/css':'text/html','cache-control':'no-store'});response.end(bytes);
  });
  server.on('connection',socket=>{sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});});
  server.on('upgrade',(request,socket)=>{
    if(request.url!=='/bridge?token='+'a'.repeat(64)||peer){socket.destroy();return;}
    const accept=crypto.createHash('sha1').update(request.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n');peer=socket;
    // 浏览器发来的数据不作平台请求；关闭时立即完成本地握手，避免关会话排队。
    socket.on('data',bytes=>{if(bytes.length&&((bytes[0]&15)===8)){socket.end(frame('',8));}});
  });
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
  return {url:'http://127.0.0.1:'+server.address().port+'/',send:value=>{assert.ok(peer&&!peer.destroyed,'local_bridge_required');peer.write(frame(JSON.stringify(value)));},disconnect:()=>{assert.ok(peer,'local_bridge_required');peer.end(frame('',8));},
    async close(){for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));}};
}
// browser 使用独立具名会话与真实Chromium媒体轨；fake-device仅替换输入源，不替换MediaStreamTrack类。
async function browser(session,args){
  const result=await execute('agent-browser',['--session',session,...args],{cwd:root,timeout:25000,maxBuffer:65536,
    env:{...process.env,AGENT_BROWSER_ARGS:browserArgs}});
  return result.stdout.trim();
}
// readFacts 保留原轨引用与Peer，释放从views移除后仍检查同一真实对象；不报告trackID／会话身份。
const readFacts='({tracks:globalThis.__releaseTracks.map(track=>({kind:track.kind,state:track.readyState,native:track instanceof MediaStreamTrack})),gumCalls:globalThis.__gumCalls,peerClosed:globalThis.__releasePeer?globalThis.__releasePeer.connectionState==="closed":null})';
// run 两次完整执行音频error／断连和IPC音视频stop／断连；正式生产资源不改，不接模型或中央服务。
async function run(){
  assert.equal(process.env.UR_BROWSER_RESOURCE_RELEASE_LIVE,'1','explicit_browser_release_required');
  const results=[];
  for(let round=1;round<=2;++round)for(const scenario of ['aiError','aiDisconnect','ipcStop','ipcDisconnect']){
    const ai=scenario.startsWith('ai'),kind=ai?'ai':'ipc',session='task143-track-release-'+process.pid+'-'+round+'-'+scenario;
    let service,opened=false,phase='open',primaryFailure;
    try{
      service=await localAssets(kind);opened=true;await browser(session,['open',service.url]);await browser(session,['snapshot','-i']);
      await browser(session,['eval','globalThis.__gumCalls=0;globalThis.__gumOriginal=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);navigator.mediaDevices.getUserMedia=(options)=>{++globalThis.__gumCalls;return globalThis.__gumOriginal(options)};({observerInstalled:true})']);
      phase='activate';await browser(session,['click','#activate']);
      await browser(session,['wait','--fn',ai?'typeof context!=="undefined"&&context?.state==="running"&&stream?.getAudioTracks().length===1':'socket?.readyState===WebSocket.OPEN']);
      phase='captureReady';if(!ai){service.send({kind:'ipcConfigure',sessionId:'isolated-view',releaseId:'1',audio:true,audioTrack:'isolated-mic',videoTrack:'isolated-camera'});await browser(session,['wait','--fn','views.get("isolated-view")?.peer?.localDescription?.type==="offer"']);}
      const remember=ai?'globalThis.__releaseTracks=stream.getTracks();globalThis.__releasePeer=null;':'globalThis.__releaseTracks=views.get("isolated-view").stream.getTracks();globalThis.__releasePeer=views.get("isolated-view").peer;';
      await browser(session,['eval',remember+'({remembered:true})']);
      phase='before';const before=JSON.parse(await browser(session,['eval',readFacts]));verifyTracks(before,ai?['audio']:['audio','video'],'live');assert.equal(before.gumCalls,1);
      phase='stop';
      if(scenario==='aiError')service.send({kind:'error'});
      else if(scenario==='ipcStop')service.send({kind:'ipcStop',sessionId:'isolated-view',releaseId:'1'});
      else service.disconnect();
      phase='ended';await browser(session,['wait','--fn','globalThis.__releaseTracks.every(track=>track.readyState==="ended")'+(ai?'&&context.state==="closed"':'&&globalThis.__releasePeer.connectionState==="closed"')]);
      const late=ai?'try{incoming({kind:"captureStart",id:"late-retired-input"})}catch{};':'await incoming({kind:"ipcAnswer",sessionId:"isolated-view",releaseId:"1",sdp:"late-retired-answer"});await incoming({kind:"ipcActive",sessionId:"isolated-view"});';
      phase='late';await browser(session,['eval','(async()=>{'+late+'return '+readFacts+'})()']);
      const after=JSON.parse(await browser(session,['eval',readFacts]));verifyTracks(after,ai?['audio']:['audio','video'],'ended');assert.equal(after.gumCalls,before.gumCalls,'late_message_reopened_capture');
      if(!ai)assert.equal(after.peerClosed,true);
      results.push({round,scenario,actualNativeTracks:true,trackKinds:ai?['audio']:['audio','video'],allTracksEnded:true,lateDidNotReopen:true});
    }catch(error){primaryFailure=error;error.releaseScenario=scenario;error.releasePhase=phase;throw error;
    }finally{
      let closeError;
      if(opened)try{await browser(session,['close']);}catch(error){closeError=error;}
      if(service)await service.close();
      if(closeError){if(primaryFailure)primaryFailure.releaseCleanupFailed=true;else throw Error('named_browser_cleanup_failed');}
    }
  }
  return {passed:true,evidence:'production-browser-local-bridge-only',cppACKChainVerified:false,hardwareAECVerified:false,results,ownedResourcesClosed:true};
}
module.exports={verifyTracks,frame,localAssets,browserArgs,run};
if(require.main===module)run().then(result=>console.log(JSON.stringify(result))).catch(error=>{console.error(JSON.stringify({passed:false,evidence:'production-browser-local-bridge-only',reason:'browser_release_verification_failed',scenario:error.releaseScenario??'preflight',phase:error.releasePhase??'preflight',cleanupFailed:error.releaseCleanupFailed===true}));process.exitCode=1;});
