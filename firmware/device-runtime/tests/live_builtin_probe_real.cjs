// 文件说明：显式真实Chromium固定接收格式诊断，读builtin像素而非节目；不发送协议能力或媒体授权。
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
const {randomUUID,createHash}=require('node:crypto');
const {execFile}=require('node:child_process');
const {promisify}=require('node:util');
const invoke=promisify(execFile);

// inspectBuiltin 独立解码同一固定IDR，输出固定codec阶段和RGBA范围；不改变正式自检判据。
async function inspectBuiltin(){
  const facts={stage:'configure',decoded:0,width:0,height:0,ranges:null,colorSpace:null,decoderError:false,closed:false};
  let decoder,timer,observation=Promise.resolve();
  try{
    decoder=new VideoDecoder({error:()=>{facts.decoderError=true;},output:frame=>{
      facts.decoded++;observation=(async()=>{
        try{
          facts.stage='pixels';facts.width=frame.displayWidth;facts.height=frame.displayHeight;
          facts.colorSpace={matrix:frame.colorSpace.matrix,primaries:frame.colorSpace.primaries,transfer:frame.colorSpace.transfer,fullRange:frame.colorSpace.fullRange};
          const pixels=new Uint8Array(frame.allocationSize({format:'RGBA'}));await frame.copyTo(pixels,{format:'RGBA'});
          const ranges=Array.from({length:4},()=>({min:255,max:0}));
          for(let i=0;i<pixels.length;i++) {const value=ranges[i%4];value.min=Math.min(value.min,pixels[i]);value.max=Math.max(value.max,pixels[i]);}
          facts.ranges=ranges;facts.stage='decoded';
        }finally{frame.close();}
      })();
    }});
    decoder.configure({codec:'avc1.42c00a',codedWidth:32,codedHeight:32,optimizeForLatency:true});facts.stage='decode';
    decoder.decode(new EncodedVideoChunk({type:'key',timestamp:0,data:Uint8Array.from(atob(UR_LIVE_PROBE_H264),value=>value.charCodeAt(0))}));
    await Promise.race([(async()=>{await decoder.flush();await observation;})(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('bounded')),2000);})]);
  }catch{facts.decoderError=true;}
  finally{clearTimeout(timer);if(decoder&&decoder.state!=='closed')decoder.close();facts.closed=!!decoder&&decoder.state==='closed';}
  return facts;
}

// inspectTransform 绑定原接收器及生产worker，仅旁证实际接口和原关闭事件；无SDP、媒体样本或许可。
async function inspectTransform(kind){
  const facts={kind,binding:null,ready:false,failed:false,closed:false,peerClosed:false,trackStopped:false};
  let peer,worker,port,timer;
  try{
    peer=new RTCPeerConnection({iceServers:[]});const receiver=peer.addTransceiver(kind,{direction:'recvonly'}).receiver;
    worker=new Worker('/transform-diagnostic.js');const channel=new MessageChannel();port=channel.port1;
    await new Promise(resolve=>{
      timer=setTimeout(resolve,1800);port.onmessage=event=>{
        const value=event.data;
        if(value?.kind==='diagnosticBinding')facts.binding={readable:value.readable,writable:value.writable,keyRequest:value.keyRequest};
        if(value?.kind==='liveTransformReady'){facts.ready=true;port.postMessage({kind:'liveTransformClose'});}
        if(value?.kind==='liveTransformFailed'){facts.failed=true;resolve();}
        if(value?.kind==='liveTransformClosed'){facts.closed=true;resolve();}
      };port.start();receiver.transform=new RTCRtpScriptTransform(worker,{kind,port:channel.port2},[channel.port2]);
    });
    receiver.track.stop();facts.trackStopped=receiver.track.readyState==='ended';
  }catch{facts.failed=true;}
  finally{clearTimeout(timer);worker?.terminate();port?.close();peer?.close();facts.peerClosed=!!peer&&peer.connectionState==='closed';}
  return facts;
}

// run 只创建独立HTTP和browser session；失败也关闭本次资源，不操作共享浏览器。
async function run(){
  assert.equal(process.env.UR_LIVE_BUILTIN_PROBE,'1');
  const source=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/live-probe.js'));
  const workerSource=fs.readFileSync(path.join(__dirname,'../adapters/desktop/web/live-transform-worker.js'),'utf8');
  const diagnosticWorker=`self.addEventListener('rtctransform',event=>{const value=event.transformer;value.options.port.postMessage({kind:'diagnosticBinding',readable:!!value.readable,writable:!!value.writable,keyRequest:typeof value.sendKeyFrameRequest==='function'});});\n${workerSource}`;
  const session=`task143-live-builtin-${randomUUID()}`;let server,report,failed=false;
  async function browser(...args){
    const output=await invoke('agent-browser',['--session',session,'--json',...args],{timeout:25000,maxBuffer:131072,env:{...process.env,AGENT_BROWSER_ARGS:'--no-sandbox,--mute-audio'}});
    const value=JSON.parse(output.stdout);assert.equal(value.success,true);return value.data;
  }
  try{
    server=http.createServer((request,response)=>{
      const script=request.url==='/live-probe.js'||request.url==='/transform-diagnostic.js';response.writeHead(200,{'content-type':script?'text/javascript':'text/html','cache-control':'no-store'});
      response.end(request.url==='/live-probe.js'?source:request.url==='/transform-diagnostic.js'?diagnosticWorker:'<!doctype html><title>Builtin codec diagnostic</title><script src="/live-probe.js"></script>');
    });
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    await browser('open',`http://127.0.0.1:${server.address().port}`);await browser('snapshot','-i');
    const expression=`(async()=>({formal:await probeLiveReceive(),builtin:await (${inspectBuiltin.toString()})(),rtcH264:RTCRtpReceiver.getCapabilities('video').codecs.some(value=>value.mimeType.toLowerCase()==='video/h264'),transforms:await Promise.all(['audio','video'].map(${inspectTransform.toString()}))}))()`;
    report=(await browser('eval','-b',Buffer.from(expression).toString('base64'))).result;
  }catch{failed=true;}
  finally{if(server)await new Promise(resolve=>server.close(resolve));try{await browser('close');}catch{failed=true;}}
  const result={scope:'fixed-builtin-real-codec-diagnostic-not-device-e2e',sourceSha256:createHash('sha256').update(source).digest('hex'),workerSha256:createHash('sha256').update(workerSource).digest('hex'),failed,facts:report||null,resourcesClosed:!server?.listening};
  console.log(JSON.stringify(result));assert.ok(!failed&&result.resourcesClosed&&report?.builtin?.closed);
  return result;
}
module.exports={inspectBuiltin,run};
if(require.main===module)run().catch(()=>{process.exitCode=1;});
