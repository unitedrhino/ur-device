// 文件说明：冻结C++经本机合成MQTT拒接错误ACK触发原Runtime退出与Chromium真停轨；不是正式平台鉴权验收。
'use strict';
const fs=require('node:fs'),path=require('node:path'),net=require('node:net'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const {spawn,execFile}=require('node:child_process'),{promisify}=require('node:util');
const {verifyTracks,browserArgs}=require('./browser_resource_release_live.cjs');
const execute=promisify(execFile),root=path.resolve(__dirname,'../../..');
const binary=path.join(root,'.temp/task143-remediation-20261008/device-browser-ai-endpoint-stage6');
const expectedSHA='866cf7a34a8663013d5c701c3c94dc3351d90b4187413fd889b323808c23b8a0';
// mqttPacket 仅编码MQTT3测试应答；剩余长度遵守协议上限，不扩展生产网络适配。
function mqttPacket(header,body){
  assert.ok(body.length<=65536,'synthetic_mqtt_packet_budget');
  let length=body.length;const prefix=[header];do{const part=length%128;length=Math.floor(length/128);prefix.push(part|(length?128:0));}while(length);
  return Buffer.concat([Buffer.from(prefix),body]);
}
// mqttText 为本测试公开Topic编码UTF8长度，所有身份均是合成占位，不读取实际设备凭据。
function mqttText(value){const bytes=Buffer.from(value);return Buffer.concat([Buffer.from([bytes.length>>8,bytes.length&255]),bytes]);}
// capabilityReply 原监听入口必须收到应用层能力回执；MQTT PUBACK只证明Broker受理，不能替代协议握手。
function capabilityReply(value){
  assert.equal(value.method,'capabilityReport');assert.ok(typeof value.msgToken==='string'&&value.msgToken.length>0&&value.msgToken.length<=128);
  assert.ok(Number.isSafeInteger(value.params?.revision)&&value.params.revision>0,'original_capability_revision_required');
  return {method:'requestAck',msgToken:value.msgToken,code:200,data:{requestMethod:'capabilityReport',result:{acceptedRevision:value.params.revision}}};
}
// syntheticBroker 只响应本机冻结设备的连接/订阅/PUBACK；仅一个原sessionReject可收到合成403，不模拟模型或权限。
async function syntheticBroker(){
  const sockets=new Set();let peer,rejected=false,rejectToken,invalid=false;
  const publish=value=>{assert.ok(peer&&!peer.destroyed,'synthetic_mqtt_peer_required');peer.write(mqttPacket(0x30,Buffer.concat([mqttText('$thing/down/signaling/offline-release/device'),Buffer.from(JSON.stringify(value))])));};
  const server=net.createServer(socket=>{
    if(peer){socket.destroy();return;}peer=socket;sockets.add(socket);let pending=Buffer.alloc(0);
    socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});
    socket.on('data',bytes=>{
      try{
        pending=Buffer.concat([pending,bytes]);assert.ok(pending.length<=131072,'synthetic_mqtt_buffer_budget');
        while(pending.length>=2){
          let at=1,length=0,factor=1,complete=false;
          for(let index=0;index<4&&at<pending.length;++index){const part=pending[at++];length+=(part&127)*factor;if(!(part&128)){complete=true;break;}factor*=128;}
          if(!complete||pending.length<at+length)return;
          assert.ok(length<=65536,'synthetic_mqtt_packet_budget');
          const header=pending[0],body=pending.subarray(at,at+length);pending=pending.subarray(at+length);
          const type=header>>4;
          if(type===1)socket.write(Buffer.from([0x20,2,0,0]));
          else if(type===8){
            const result=[body[0],body[1]];let offset=2;
            while(offset<body.length){const size=body.readUInt16BE(offset);offset+=2+size;assert.ok(offset<body.length,'synthetic_subscription_invalid');++offset;result.push(1);}
            socket.write(mqttPacket(0x90,Buffer.from(result)));
          }else if(type===3){
            const size=body.readUInt16BE(0),topic=body.subarray(2,2+size).toString();let offset=2+size;
            if(((header>>1)&3)===1){socket.write(mqttPacket(0x40,body.subarray(offset,offset+2)));offset+=2;}
            if(topic!=='$thing/up/signaling/offline-release/device')continue;
            const value=JSON.parse(body.subarray(offset));
            if(value.method==='capabilityReport')publish(capabilityReply(value));
            if(value.method==='sessionReject'){
              assert.equal(rejected,false,'single_original_reject_required');assert.equal(value.sessionId,'isolated-candidate');
              assert.equal(value.params.reason,'userRejected');assert.ok(typeof value.msgToken==='string'&&value.msgToken);
              rejected=true;rejectToken=value.msgToken;
              publish({method:'requestAck',msgToken:value.msgToken,sessionId:value.sessionId,code:403,data:{requestMethod:'sessionReject',reason:'requestDenied'}});
            }
          }else if(type===12)socket.write(Buffer.from([0xd0,0]));
        }
      }catch{invalid=true;socket.destroy();}
    });
  });
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
  return {port:server.address().port,invite(){publish({method:'sessionInvite',msgToken:'isolated-invite',sessionId:'isolated-candidate',eventSeq:1,params:{purpose:'aiConversation',admission:'accept',expiresAt:Date.now()+30000,handoff:{sourceSessionId:'isolated-source',handoffId:'isolated-move'},selected:{transport:'udp',interactionMode:'halfDuplex',audioParams:{format:'opus',sampleRate:16000,channels:1,frameDuration:20}},tracks:[{trackId:'isolated-mic',kind:'audio',sourceId:'mic',direction:'sendonly',codec:'opus',state:'requested'},{trackId:'isolated-speaker',kind:'audio',sourceId:'speaker',direction:'recvonly',codec:'opus',state:'requested'}],media:{udp:{server:'127.0.0.1',port:12345,key:'000102030405060708090a0b0c0d0e0f',nonce:'01000000112233445566778800000000'}}}});},
    facts:()=>({originalRejectObserved:rejected,originalTokenAcknowledged:typeof rejectToken==='string',invalid}),
    async close(){for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));}};
}
// unusedPort 只分配本机测试端口并马上释放给原C++监听；不会选用中央固定端口。
async function unusedPort(){const server=net.createServer();await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;}
// browser 所有命令绑定本次具名会话，单命令不超过CLI原IPC预算。
async function browser(session,args){const result=await execute('agent-browser',['--session',session,...args],{cwd:root,timeout:25000,maxBuffer:65536,env:{...process.env,AGENT_BROWSER_ARGS:browserArgs}});return result.stdout.trim();}
// run 两次沿实际冻结原Runtime及本人拒接入口执行，不注入HAL计数或调用私有核心方法。
async function run(){
  assert.equal(process.env.UR_BROWSER_CPP_ACK_RELEASE_LIVE,'1','explicit_cpp_ack_release_required');
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(binary)).digest('hex'),expectedSHA,'frozen_cpp_required');
  const results=[];
  for(let round=1;round<=2;++round){
    const session='task143-cpp-ack-release-'+process.pid+'-'+round;let broker,child,closed=false,opened=false,exitCode,completion,phase='preflight',primary;
    try{
      broker=await syntheticBroker();const port=await unusedPort();
      child=spawn(binary,[String(port)],{cwd:root,env:{PATH:process.env.PATH,UR_DEVICE_PRODUCT:'offline-release',UR_DEVICE_NAME:'device',UR_DEVICE_SECRET:'0'.repeat(64),UR_MQTT_HOST:'127.0.0.1',UR_MQTT_PORT:String(broker.port),UR_DEVICE_AI_ENDPOINT_LISTEN:'1',UR_MEDIA_TRANSPORT:'udp'},stdio:['ignore','pipe','ignore']});
      completion=new Promise(resolve=>child.once('close',code=>{closed=true;exitCode=code;resolve();}));
      await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('frozen_cpp_start_timeout')),5000);child.once('error',()=>{clearTimeout(timer);reject(Error('frozen_cpp_spawn_failed'));});child.stdout.on('data',bytes=>{if(bytes.toString().includes('http://127.0.0.1:'+port+'/')){clearTimeout(timer);resolve();}});});
      phase='open';opened=true;await browser(session,['open','http://127.0.0.1:'+port+'/']);phase='snapshot';await browser(session,['snapshot','-i']);phase='activate';await browser(session,['click','#activate']);
      phase='listenerReady';
      await browser(session,['wait','--fn','aiEndpointState?.phase===1&&context.state==="running"']);
      await browser(session,['eval','globalThis.__bridgeErrors=0;globalThis.__originalBridgeMessage=socket.onmessage;socket.onmessage=event=>{try{if(JSON.parse(event.data).kind==="error")++globalThis.__bridgeErrors}catch{};globalThis.__originalBridgeMessage.call(socket,event)};({observerInstalled:true})']);
      const expression='({tracks:stream.getTracks().map(track=>({kind:track.kind,state:track.readyState,native:track instanceof MediaStreamTrack})),terminated,contextClosed:context.state==="closed",bridgeErrors:globalThis.__bridgeErrors})';
      const before=JSON.parse(await browser(session,['eval',expression]));verifyTracks(before,['audio'],'live');assert.equal(before.terminated,false);
      phase='invite';broker.invite();await browser(session,['wait','--fn','aiEndpointState?.phase===2&&!element("endpointReject").disabled']);
      phase='reject403';await browser(session,['click','#endpointReject']);await browser(session,['wait','--fn','terminated&&context.state==="closed"&&stream.getTracks().every(track=>track.readyState==="ended")']);
      const after=JSON.parse(await browser(session,['eval',expression]));verifyTracks(after,['audio'],'ended');assert.equal(after.contextClosed,true);
      assert.deepEqual(broker.facts(),{originalRejectObserved:true,originalTokenAcknowledged:true,invalid:false});
      await completion;assert.equal(exitCode,1,'original_runtime_failure_exit_required');
      results.push({round,actualNativeTrackEnded:true,originalCPPRejectAndSynthetic403:true,originalRuntimeFailed:true,originalContextClosed:true,originalBridgeErrorObserved:after.bridgeErrors>0});
    }catch(error){
      primary=error;error.releasePhase=phase;
      // 只投影本机闭集媒体状态；原自由错误文本、Topic、身份、令牌与正文不进入诊断。
      error.releaseFacts={cppClosed:closed,cppExit:closed&&Number.isInteger(exitCode)?exitCode:null};
      if(opened)try{
        const facts=JSON.parse(await browser(session,['eval','({phase:typeof aiEndpointState!=="undefined"?aiEndpointState?.phase??null:null,contextState:typeof context!=="undefined"?context?.state??null:null,terminated:typeof terminated!=="undefined"?terminated:null,trackCount:typeof stream!=="undefined"?stream?.getTracks().length??null:null,socketState:typeof socket!=="undefined"?socket?.readyState??null:null})']));
        error.releaseFacts={...error.releaseFacts,...failureFacts(facts)};
      }catch{error.releaseFacts.browserFactsUnavailable=true;}
      throw error;
    }finally{
      let cleanupFailed=false;
      if(opened)try{await browser(session,['close']);}catch{cleanupFailed=true;}
      if(child&&!closed){child.kill('SIGTERM');await completion;}
      if(broker)await broker.close();
      if(cleanupFailed){if(primary)primary.cleanupFailed=true;else throw Error('named_browser_cleanup_failed');}
    }
  }
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(binary)).digest('hex'),expectedSHA,'frozen_cpp_changed');
  return {passed:true,evidence:'frozen-cpp-synthetic-mqtt-local-chain',formalPlatformVerified:false,hardwareAECVerified:false,cppSHA:expectedSHA,results,ownedResourcesClosed:true};
}
// failureFacts 仅允许闭集枚举与小整数，防止夹具失败报告泄漏自由字段或误称CPPACK已执行。
function failureFacts(value){
  const small=number=>Number.isInteger(number)&&number>=0&&number<=16?number:null;
  return {phase:small(value.phase),contextState:['running','suspended','closed'].includes(value.contextState)?value.contextState:null,
    terminated:typeof value.terminated==='boolean'?value.terminated:null,trackCount:small(value.trackCount),socketState:small(value.socketState)};
}
module.exports={mqttPacket,mqttText,capabilityReply,syntheticBroker,failureFacts,run};
if(require.main===module)run().then(value=>console.log(JSON.stringify(value))).catch(error=>{console.error(JSON.stringify({passed:false,evidence:'frozen-cpp-synthetic-mqtt-local-chain',phase:error.releasePhase??'preflight',reason:'cpp_ack_release_verification_failed',facts:error.releaseFacts??null,cleanupFailed:error.cleanupFailed===true}));process.exitCode=1;});
