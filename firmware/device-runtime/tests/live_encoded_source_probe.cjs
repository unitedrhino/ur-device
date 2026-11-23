// 文件说明：显式同机连续源网络/真实WebCodecs解码检查；不等同MQTT/MCP/设备播出E2E。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const net = require('node:net');
const {randomUUID,createHash} = require('node:crypto');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const {startSource} = require('./live_encoded_source.cjs');
const invoke = promisify(execFile);

// accessUnit 解包此可信源的固定12字节RTP头；完整FU-A才可还原原NAL，不承诺任意网络包解码。
function accessUnit(sample) {
  const nals = []; let fragments = null;
  for (const value of sample.video) {
    const packet = Buffer.from(value,'base64');
    assert.equal(packet[0],128); assert.equal(packet.readUInt32BE(4),sample.timestamp);
    const payload = packet.subarray(12), type = payload[0]&31;
    if (type !== 28) { assert.equal(fragments,null); nals.push(payload); }
    else {
      if(payload[1]&128) { assert.equal(fragments,null); fragments=[Buffer.from([(payload[0]&0xe0)|(payload[1]&31)])]; }
      assert.ok(fragments); fragments.push(payload.subarray(2));
      if(payload[1]&64) { nals.push(Buffer.concat(fragments)); fragments=null; }
    }
  }
  assert.equal(fragments,null);
  return nals;
}

// decode 在真实Chromium执行codec解码并读PCM/RGBA，API存在或容器数量均不构成通过。
async function decode(audio, video, codec) {
  const metrics={audioFrames:0,samples:0,peak:0,frequencyHz:0,videoFrames:0,colored:0,markerSecond:null,markerReadback:[],failed:false};
  let crossings=0,previous=0; // 连续实际PCM过零数只用于本固定测试音调，不作为通用音乐或语音效果判定。
  let ad,vd; const reads=[];
  try {
    ad = new AudioDecoder({error:()=>{metrics.failed=true;},output:value=>{
      try {
        if(value.sampleRate!==48000||value.numberOfChannels!==1||value.numberOfFrames!==960) {metrics.failed=true;return;}
        const pcm=new Float32Array(value.numberOfFrames); value.copyTo(pcm,{planeIndex:0,format:'f32-planar'});
        let sum=0; for(const sample of pcm) {if(!Number.isFinite(sample))metrics.failed=true; sum+=sample*sample;if(previous*sample<0)crossings++;previous=sample;}
        metrics.peak=Math.max(metrics.peak,Math.sqrt(sum/pcm.length)); metrics.samples+=pcm.length; metrics.audioFrames++;
      } finally {value.close();}
    }});
    vd = new VideoDecoder({error:()=>{metrics.failed=true;},output:frame=>{
      reads.push((async()=>{
        try {
          if(frame.displayWidth!==320||frame.displayHeight!==240) {metrics.failed=true;return;}
          const rgba=new Uint8Array(frame.allocationSize({format:'RGBA'})); await frame.copyTo(rgba,{format:'RGBA'});
          let colored=0; for(let i=0;i<rgba.length;i+=4) if(Math.max(rgba[i],rgba[i+1],rgba[i+2])-Math.min(rgba[i],rgba[i+1],rgba[i+2])>32)colored++;
          let marker=0; for(let bit=0;bit<5;bit++) { const at=(28*320+28+bit*40)*4; if(rgba[at]>200&&rgba[at+1]>200&&rgba[at+2]>200)marker+=2**bit; }
          metrics.markerSecond=marker;
          metrics.markerReadback.push({frameIndex:Math.round(frame.timestamp/66667),second:marker});
          metrics.colored=Math.max(metrics.colored,colored); metrics.videoFrames++;
        } finally {frame.close();}
      })());
    }});
    ad.configure({codec:'opus',sampleRate:48000,numberOfChannels:1});
    vd.configure({codec,codedWidth:320,codedHeight:240,optimizeForLatency:true});
    const bytes=value=>Uint8Array.from(atob(value),letter=>letter.charCodeAt(0));
    for(let i=0;i<audio.length;i++)ad.decode(new EncodedAudioChunk({type:'key',timestamp:i*20000,duration:20000,data:bytes(audio[i])}));
    for(let i=0;i<video.length;i++)vd.decode(new EncodedVideoChunk({type:i===0?'key':'delta',timestamp:i*66667,data:bytes(video[i])}));
    await Promise.all([ad.flush(),vd.flush()]); await Promise.all(reads);metrics.frequencyHz=crossings*48000/(2*metrics.samples);return metrics;
  } finally {if(ad&&ad.state!=='closed')ad.close();if(vd&&vd.state!=='closed')vd.close();}
}

// probe 仅创建本任务socket/编码器/浏览器；失败也等待精确退出，不删历史证据或共享资源。
async function probe() {
  assert.equal(process.env.UR_LIVE_ENCODED_PROBE,'1','explicit_probe_required');
  const id=randomUUID(), directory=path.resolve('.temp/task143-ipc-env'); fs.mkdirSync(directory,{recursive:true});
  const reportPath=path.join(directory,`continuous-source-${id}.json`), socketPath=path.join(directory,`live-${process.pid}.sock`);
  // 固定测试源码散列区分主线SHA与尚未提交的验收脚本；不读取环境或身份配置。
  const provenance={head:(await invoke('git',['rev-parse','HEAD'])).stdout.trim(),sources:{}};
  for(const name of ['live_encoded_source.cjs','live_encoded_source_probe.cjs'])provenance.sources[name]=createHash('sha256').update(fs.readFileSync(path.join(__dirname,name))).digest('hex');
  const session=`task143-live-codec-${id}`, audio=[], video=[];
  let source,socket,server,stage='start',failure=false,result=null;
  const environment={...process.env,AGENT_BROWSER_ARGS:'--no-sandbox,--mute-audio'};
  // browser 使用本次独立会话；任何自由CLI错误不进入诊断报告。
  async function browser(...args) {
    const output=await invoke('agent-browser',['--session',session,'--json',...args],{env:environment,timeout:25000,maxBuffer:131072});
    const parsed=JSON.parse(output.stdout); assert.equal(parsed.success,true); return parsed.data;
  }
  try {
    server=http.createServer((_,res)=>{res.writeHead(200,{'content-type':'text/html','cache-control':'no-store'});res.end('<!doctype html><title>Live codec probe</title>');});
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    stage='browser'; await browser('open',`http://127.0.0.1:${server.address().port}`);
    stage='encoding'; source=await startSource({socketPath,sourceId:'continuous-probe',epoch:1,seconds:20});
    stage='network'; let input='',descriptor=null,networkFailed=false;
    socket=net.createConnection(socketPath);
    socket.on('error',()=>{networkFailed=true;});
    socket.on('data',chunk=>{
      try {
        input+=chunk.toString(); assert.ok(input.length<=1048576);
        let newline;
        while((newline=input.indexOf('\n'))>=0) {
          const value=JSON.parse(input.slice(0,newline));input=input.slice(newline+1);
          if(!descriptor) {assert.equal(value.sourceId,'continuous-probe');assert.equal(value.sourceEpoch,1);assert.deepEqual(value.tracks,source.tracks);descriptor=value;continue;}
          assert.equal(value.epoch,1);
          if(value.kind==='audio'&&audio.length<8)audio.push(value);
          if(value.kind==='video'&&video.length<3&&(video.length||value.keyFrame))video.push(value);
        }
      } catch {networkFailed=true;socket.destroy();}
    });
    const deadline=Date.now()+8000;
    while(audio.length<8||video.length<3) {assert.ok(!networkFailed&&!source.metrics.failed&&Date.now()<deadline,'actual_encoded_samples_required');await new Promise(resolve=>setTimeout(resolve,20));}
    assert.ok(audio.every((sample,i)=>i===0||sample.sequence>audio[i-1].sequence&&sample.timestamp>audio[i-1].timestamp));
    assert.ok(video.every((sample,i)=>i===0||sample.sequence>video[i-1].sequence&&sample.timestamp>video[i-1].timestamp));
    const units=video.map(accessUnit),sps=units[0].find(nal=>(nal[0]&31)===7);assert.ok(sps?.length>=4);
    const codec=`avc1.${sps.subarray(1,4).toString('hex')}`,payloads=units.map(nals=>Buffer.concat(nals.flatMap(nal=>[Buffer.from([0,0,0,1]),nal])).toString('base64'));
    stage='decode'; const expression=`(${decode.toString()})(${JSON.stringify(audio.map(sample=>sample.audio))},${JSON.stringify(payloads)},${JSON.stringify(codec)})`;
    result=(await browser('eval','-b',Buffer.from(expression).toString('base64'))).result;
    assert.ok(result&&!result.failed&&result.audioFrames===8&&result.samples===7680&&result.peak>0.001&&result.videoFrames===3&&result.colored>1000,'actual_pcm_pixels_required');
    assert.ok(result.markerReadback.length===3&&result.markerReadback.every(value=>Number.isInteger(value.frameIndex)&&value.frameIndex>=0&&value.frameIndex<video.length&&value.second===Math.floor(video[value.frameIndex].timestamp/90000)%32),'actual_pixel_second_marker_required');
    const audioSeconds=audio.map(value=>Math.floor(value.timestamp/48000)%32);
    assert.ok(audioSeconds.every(value=>value===audioSeconds[0])&&Math.abs(result.frequencyHz-(440+40*audioSeconds[0]))<=10,'actual_pcm_time_tone_required');
    stage='done';
  } catch {failure=true;}
  finally {
    socket?.destroy(); if(source)await source.close();
    if(server)await new Promise(resolve=>server.close(resolve));
    try {await browser('close');}catch {failure=true;}
  }
  const report={scope:'continuous-local-unix-source-real-codec-not-device-e2e',provenance,passed:!failure,stage,decoded:result,
    actualEncoded:source?{...source.metrics}:null,resourcesClosed:!fs.existsSync(socketPath)};
  fs.writeFileSync(reportPath,JSON.stringify(report,null,2)+'\n',{mode:0o600}); console.log(JSON.stringify(report));
  if(failure)throw Error('continuous_source_probe_failed'); return report;
}
module.exports={accessUnit,decode,probe};
if(require.main===module)probe().catch(()=>{process.exitCode=1;});
