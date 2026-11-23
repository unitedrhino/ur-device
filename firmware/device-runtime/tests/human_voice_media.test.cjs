// 文件说明：纯语音浏览器组件使用真实产品源码；VM回归不冒充实际Chrome/音轨解码。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/human-voice-media.js'), 'utf8');

test('新增停止getter可嵌入严格C++资产字面量，不产生trigraph三字符序列',()=>{
  assert.equal(source.includes("??'"),false);
});

test('Worklet默认真实静音，入队不能伪造输出完成', () => {
  let Processor;
  class Base { constructor() { this.facts = []; this.port = {postMessage: v => this.facts.push(v)}; } }
  vm.runInNewContext(source, {AudioWorkletProcessor:Base, sampleRate:48000,
    registerProcessor: (_, value) => {Processor=value;}, TextEncoder});
  const scope = {sessionId:'human',routeGeneration:'11',peerEpoch:'7'};
  const processor = new Processor({processorOptions:scope});
  const output = [new Float32Array(128)];
  processor.process([[new Float32Array(128).fill(0.5)]], [output]);
  assert.equal(output[0].some(v=>v!==0), false);
  const request={kind:'humanVoiceOutputGate',...scope,ticket:'1',enabled:true};
  processor.port.onmessage({data:request});
  assert.equal(processor.facts.length,0);
  processor.process([[new Float32Array(128).fill(0.5)]], [output]);
  assert.equal(output[0].every(v=>v===0.5),true);
  assert.equal(processor.facts.length,1);
  assert.equal(processor.facts[0].kind,'humanVoiceOutputGated');
  assert.equal(processor.facts[0].frames,2); // 包含门控前实际渲染的静音block，不伪造从零重置。
  assert.equal(processor.facts[0].nonSilentFrames,1);
});
// worklet加载真实产品processor，不拷贝协议实现到夹具。
function worklet(options) {
  let Processor;
  class Base { constructor(){this.facts=[];this.port={postMessage:v=>this.facts.push(v)};} }
  vm.runInNewContext(source,{AudioWorkletProcessor:Base,sampleRate:48000,registerProcessor:(_,p)=>{Processor=p;},TextEncoder});
  return new Processor({processorOptions:options});
}
const scope={sessionId:'human',routeGeneration:'11',peerEpoch:'7'};
const command=(kind,extra={})=>({kind,...scope,...extra});
test('Worklet错scope/伪票据/重复/无输出block不确认',()=>{
  const p=worklet(scope);p.command(command('humanVoiceOutputGate',{ticket:'01',enabled:true}));
  p.process([],[[new Float32Array(128)]]);assert.equal(p.facts.length,0);
  p.command({...command('humanVoiceOutputGate',{ticket:'1',enabled:true}),sessionId:'wrong'});
  p.process([],[[new Float32Array(128)]]);assert.equal(p.facts.length,0);
  p.command(command('humanVoiceOutputGate',{ticket:'1',enabled:false}));p.process([],[]);assert.equal(p.facts.length,0);
  p.process([],[[new Float32Array(128)]]);assert.equal(p.facts.length,1);
  p.command(command('humanVoiceOutputGate',{ticket:'1',enabled:true}));const out=new Float32Array(128);p.process([[out.fill(.5)]],[[new Float32Array(128)]]);assert.equal(p.facts.length,1);
});
test('Worklet独立boot探针不能授权正式scope或重用旧ACK',()=>{
  const p=worklet({bootGeneration:'9'});p.command({kind:'humanVoiceOutputInit',bootGeneration:'9',ticket:'1',enabled:false});
  p.process([],[[new Float32Array(128)]]);assert.equal(p.facts[0].kind,'humanVoiceOutputIdle');assert.equal('sessionId' in p.facts[0],false);
  p.command(command('humanVoiceOutputBind'));p.command({kind:'humanVoiceOutputInit',bootGeneration:'9',ticket:'2',enabled:false});p.process([],[[new Float32Array(128)]]);assert.equal(p.facts.length,1);
  p.command(command('humanVoiceOutputGate',{ticket:'2',enabled:false}));p.process([],[[new Float32Array(128)]]);assert.equal(p.facts[1].kind,'humanVoiceOutputGated');
});
test('真实Worklet没有Window TextEncoder时原scope绑定仍须可解析',()=>{
  let Processor;class Base{constructor(){this.port={postMessage:()=>{}};}}
  vm.runInNewContext(source,{AudioWorkletProcessor:Base,sampleRate:48000,registerProcessor:(_,p)=>{Processor=p;}});
  assert.doesNotThrow(()=>new Processor({processorOptions:scope}));
});
test('Worklet非有限样本不输出NaN，stop不再处理',()=>{
  const p=worklet(scope);p.command(command('humanVoiceOutputGate',{ticket:'1',enabled:true}));
  const out=new Float32Array(128);p.process([[new Float32Array(128).fill(NaN)]],[[out]]);assert.equal(out.some(v=>!Number.isFinite(v)),false);
  p.command({kind:'humanVoiceOutputStop'});assert.equal(p.process([],[[out]]),false);
});
// rig的native类型替身只验证装配/顺序，实际Chrome另有独立组件证明。
function rig(observeStop = false, emitDiagnostic = null) {
  const facts=[],nodes=[],peers=[],audios=[];
  const track={kind:'audio',readyState:'live',enabled:true,stops:0,stop(){this.stops++;}};
  class Stream {constructor(tracks=[track]){this.tracks=tracks;}getAudioTracks(){return this.tracks.filter(t=>t.kind==='audio');}getVideoTracks(){return this.tracks.filter(t=>t.kind==='video');}}
  class Context {constructor(){this.state='running';this.sampleRate=48000;this.destination={};this.audioWorklet={addModule:async()=>{}};}createMediaStreamSource(stream){return new Source(this,stream);}}
  class Source {constructor(context,stream){this.context=context;this.mediaStream=stream;this.disconnected=false;}connect(node){this.node=node;}disconnect(){this.disconnected=true;}}
  class Node {constructor(context,name,options){this.context=context;this.name=name;this.options=options;this.commands=[];this.port={postMessage:v=>this.commands.push(v),close:()=>{this.portClosed=true;}};nodes.push(this);}connect(){this.connected=true;}disconnect(){this.connected=false;}}
  class Sender {static getCapabilities(){return{codecs:[{mimeType:'audio/opus',clockRate:48000}]};}constructor(){this.track=null;this.transport={state:'connected'};}}
  // Audio仅模拟原生receiver解码启动资源，静音sink不是第二输出许可。
  class Audio {constructor(){this.muted=false;this.paused=true;this.srcObject=null;audios.push(this);}async play(){this.paused=false;}pause(){this.paused=true;}}
  const sdp='v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10\r\n';
  class Peer {constructor(){this.connectionState='new';this.iceConnectionState='new';this.iceGatheringState='complete';this.listeners={};this.t=null;peers.push(this);}addTransceiver(kind,options){assert.equal(kind,'audio');assert.equal(options.direction,'sendrecv');return this.t={sender:new Sender(),receiver:{transport:{state:'connected'}},setCodecPreferences:c=>{this.codecs=c;}};}getTransceivers(){return[this.t];}getSenders(){return[this.t.sender];}async createOffer(){return{type:'offer',sdp};}async setLocalDescription(d){this.localDescription=d;}async setRemoteDescription(d){this.remoteDescription=d;}addEventListener(k,f){this.listeners[k]=f;}removeEventListener(k){delete this.listeners[k];}close(){this.connectionState='closed';this.onconnectionstatechange?.();}}
  const sandbox={TextEncoder,Audio,MediaStream:Stream,MediaStreamAudioSourceNode:Source,AudioContext:Context,AudioWorkletNode:Node,RTCPeerConnection:Peer,RTCRtpSender:Sender,setTimeout,clearTimeout};vm.runInNewContext(source,sandbox);
  const stream=new Stream(),context=new Context(),input=new Source(context,stream);
  const media=new sandbox.UrHumanVoiceMedia({stream,source:input,context,emit:v=>facts.push(v),observeStop,emitDiagnostic});
  return{media,facts,nodes,peers,audios,Audio,track,context,input,sdp,connected(){const p=peers[0];p.connectionState='connected';p.iceConnectionState='connected';p.onconnectionstatechange();}};
}
const start=()=>command('humanVoiceStart',{sampleRate:48000,channels:1,frameMs:20});
// 双线程连用实际产品processor与Window适配，不手工注入Gated；VM仅证契约，不冒称Chrome现场复现。
test('真实产品双线程boot绑定与原scope输出票据逐帧确认，Peer接通不替代实际输出',async()=>{
  const r=rig();assert.equal(await r.media.initialize('9'),true);
  const node=r.nodes[0],processor=worklet({bootGeneration:'9'});
  node.port.postMessage=value=>{node.commands.push(value);processor.command(value);};
  processor.port.postMessage=value=>node.port.onmessage({data:value});
  const render=(input=[])=>{const output=new Float32Array(128);processor.process(input,[[output]]);return output;};
  assert.equal(await r.media.handle({kind:'humanVoiceOutputInit',bootGeneration:'9',ticket:'1',enabled:false}),true);
  assert.equal(r.facts.length,0);render();assert.equal(r.facts.at(-1).kind,'humanVoiceOutputIdle');
  assert.equal(await r.media.handle(command('humanVoiceOutputBind')),true);
  assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'2',enabled:false})),true);
  assert.equal(r.facts.length,1);render();assert.equal(r.facts.at(-1).kind,'humanVoiceOutputGated');
  assert.equal(r.facts.at(-1).ticket,'2');assert.equal(r.facts.at(-1).enabled,false);
  assert.equal(await r.media.handle(start()),true);r.connected();
  assert.equal(await r.media.handle(command('humanVoiceAnswer',{sdp:r.sdp})),true);
  const before=r.facts.filter(v=>v.kind==='humanVoiceOutputGated').length;
  assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'3',enabled:true})),true);
  assert.equal(r.facts.filter(v=>v.kind==='humanVoiceOutputGated').length,before);
  assert.equal(render([[new Float32Array(128).fill(.25)]]).every(v=>v===.25),true);
  assert.equal(r.facts.at(-1).kind,'humanVoiceOutputGated');assert.equal(r.facts.at(-1).ticket,'3');
  assert.equal(r.facts.at(-1).enabled,true);assert.equal(r.facts.at(-1).nonSilentFrames,1);
  assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'4',enabled:false})),true);
  assert.equal(render([[new Float32Array(128).fill(.25)]]).every(v=>v===0),true);
  assert.equal(r.facts.at(-1).ticket,'4');assert.equal(r.facts.at(-1).enabled,false);
  assert.equal(r.track.stops,0);assert.equal(r.peers[0].t.sender.track,null);
});
// 实际Peer回调必须先留下独立旁证；普通Stopped报文不加字段，也不被重复close重新发送。
test('同原close在Stopped前至多发送一次匹配scope首因旁证',async()=>{
  const order=[];const r=rig(true,value=>{order.push('diagnostic');assert.equal(r.media.closed,true);assert.equal(r.facts.some(v=>v.kind==='humanVoiceStopped'),false);assert.deepEqual(JSON.parse(JSON.stringify(value)),command('humanVoiceStopDiagnostic',{known:true,cause:'peerFailed'}));});
  await r.media.handle(start());r.peers[0].connectionState='failed';r.peers[0].onconnectionstatechange();r.media.close();
  assert.deepEqual(order,['diagnostic']);assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);assert.deepEqual(Object.keys(r.facts.at(-1)).sort(),['kind','peerEpoch','routeGeneration','sessionId']);assert.equal(r.track.stops,0);assert.equal(r.context.state,'running');
});
test('独立旁证默认off和发送异常不改原Stop调用或资源释放',async()=>{
  for(const flag of [false,undefined,'1',1]){let emitted=0;const r=rig(flag,()=>{emitted++;});await r.media.handle(start());r.nodes[0].onprocessorerror();assert.equal(emitted,0);assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);}
  let calls=0;const r=rig(true,()=>{calls++;throw Error('PRIVATE');});await r.media.handle(start());r.nodes[0].onprocessorerror();r.media.close();assert.equal(calls,1);assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);assert.equal(r.track.stops,0);assert.equal(r.input.disconnected,false);
});
// 分类来自实际原回调；new/connecting仍合法，未知或资源释放不完整不得制造Stopped。
test('原Peer/sink/processor/Start/Answer回调旁证均先于原Stopped且不泄漏错误',async()=>{
  for(const [cause,trigger]of [
    ['peerDisconnected',async r=>{r.peers[0].connectionState='disconnected';r.peers[0].onconnectionstatechange();}],
    ['processorError',async r=>r.nodes[0].onprocessorerror()],
    ['remoteTrackInvalid',async r=>r.peers[0].ontrack({receiver:{},track:{kind:'audio'}})],
    ['remoteSinkRejected',async r=>{r.Audio.prototype.play=()=>Promise.reject(Error('PRIVATE'));r.peers[0].ontrack({receiver:r.peers[0].t.receiver,track:{kind:'audio'}});await Promise.resolve();await Promise.resolve();}],
    ['answerException',async r=>{r.peers[0].setRemoteDescription=async()=>{throw Error('PRIVATE');};await r.media.handle(command('humanVoiceAnswer',{sdp:r.sdp}));}],
  ]){const diagnostics=[];const r=rig(true,v=>{assert.equal(r.facts.some(f=>f.kind==='humanVoiceStopped'),false);diagnostics.push(v);});await r.media.handle(start());await trigger(r);assert.equal(diagnostics.length,1);assert.equal(diagnostics[0].cause,cause);assert.equal(diagnostics[0].known,true);assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);assert.equal(JSON.stringify(diagnostics).includes('PRIVATE'),false);}
  const diagnostics=[];const r=rig(true,v=>diagnostics.push(v));r.peers.push=()=>{throw Error('PRIVATE');};assert.equal(await r.media.handle(start()),false);assert.equal(diagnostics[0].cause,'startException');
  for(const state of ['new','connecting']){let calls=0;const r=rig(true,()=>calls++);await r.media.handle(start());r.peers[0].connectionState=state;r.peers[0].onconnectionstatechange();assert.equal(r.media.closed,false);assert.equal(calls,0);}
  const unknown=[];const invalid=rig(true,v=>unknown.push(v));await invalid.media.handle(start());invalid.media.close('PRIVATE');assert.equal(unknown[0].known,false);assert.equal(unknown[0].cause,'unknown');
});
test('取消及资源关闭失败只留原close旁证，不补Stopped或关闭共享源',async()=>{
  const diagnostics=[],r=rig(true,v=>diagnostics.push(v));await r.media.handle(start());r.peers[0].close=()=>{throw Error('PRIVATE');};r.media.close();r.media.close();assert.equal(diagnostics.length,1);assert.equal(diagnostics[0].cause,'explicitClose');assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,0);assert.equal(r.track.stops,0);assert.equal(r.context.state,'running');
  const empty=[];const unbound=rig(true,v=>empty.push(v));unbound.media.close();assert.equal(empty.length,0);assert.equal(unbound.facts.length,0);
});
test('候选只建单audio空sender，共享源/context不重建或停止',async()=>{
  const r=rig();assert.equal(await r.media.handle(start()),true);assert.equal(r.peers.length,1);assert.equal(r.peers[0].t.sender.track,null);
  assert.equal(r.media.inputBinding('wrong','11'),null);assert.equal(r.media.inputBinding('human','11').sender,r.peers[0].t.sender);
  assert.equal(r.facts[0].kind,'humanVoiceOffer');assert.match(r.facts[0].sdp,/ptime:20/);
  assert.equal(await r.media.handle(start()),false);r.media.close();r.media.close();assert.equal(r.track.stops,0);assert.equal(r.context.state,'running');assert.equal(r.input.disconnected,false);
  assert.equal(r.nodes[0].connected,false);assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);
});
test('默认静音/实际Answer后Peer与ICE/DTLS同时成立才连通',async()=>{
  const r=rig();await r.media.handle(start());r.connected();assert.equal(r.facts.some(v=>v.kind==='humanVoiceConnected'),false);
  assert.equal(await r.media.handle(command('humanVoiceAnswer',{sdp:r.sdp})),true);assert.deepEqual(r.facts.map(v=>v.kind),['humanVoiceOffer','humanVoiceAnswered','humanVoiceConnected']);
  assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'1',enabled:true})),true);assert.equal(r.facts.some(v=>v.kind==='humanVoiceOutputGated'),false);
  r.nodes[0].port.onmessage({data:command('humanVoiceOutputGated',{ticket:'1',enabled:true,frames:4,nonSilentFrames:2})});assert.equal(r.facts.at(-1).kind,'humanVoiceOutputGated');
});
test('输出伪scope/零帧/重复票据不得推进，额外字段和错answer拒绝',async()=>{
  const r=rig();await r.media.handle(start());assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'1',enabled:true})),false);
  assert.equal(await r.media.handle(command('humanVoiceAnswer',{sdp:r.sdp,private:true})),false);
  assert.equal(await r.media.handle(command('humanVoiceAnswer',{sdp:r.sdp.replace('m=audio','m=video')})),false);
  assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'1',enabled:false})),true);
  r.nodes[0].port.onmessage({data:command('humanVoiceOutputGated',{ticket:'1',enabled:false,frames:0,nonSilentFrames:0})});assert.equal(r.facts.length,1);
  r.nodes[0].port.onmessage({data:{...command('humanVoiceOutputGated',{ticket:'1',enabled:false,frames:1,nonSilentFrames:0}),peerEpoch:'8'}});assert.equal(r.facts.length,1);
  r.nodes[0].port.onmessage({data:command('humanVoiceOutputGated',{ticket:'1',enabled:false,frames:1,nonSilentFrames:0})});assert.equal(r.facts.length,2);
});
test('boot只建静音节点不建Peer，正式绑定后必须新原scope票据',async()=>{
  const r=rig();assert.equal(await r.media.initialize('9'),true);assert.equal(r.peers.length,0);
  assert.equal(await r.media.handle({kind:'humanVoiceOutputInit',bootGeneration:'9',ticket:'1',enabled:false}),true);
  r.nodes[0].port.onmessage({data:{kind:'humanVoiceOutputIdle',bootGeneration:'9',ticket:'1',enabled:false,frames:1}});assert.equal(r.facts.length,1);
  assert.equal(await r.media.handle(command('humanVoiceOutputBind')),true);assert.equal(await r.media.handle(command('humanVoiceOutputGate',{ticket:'2',enabled:false})),true);
  r.nodes[0].port.onmessage({data:{kind:'humanVoiceOutputIdle',bootGeneration:'9',ticket:'2',enabled:false,frames:2}});assert.equal(r.facts.length,1);
  r.nodes[0].port.onmessage({data:command('humanVoiceOutputGated',{ticket:'2',enabled:false,frames:2,nonSilentFrames:0})});assert.equal(r.facts.length,2);
  assert.equal(await r.media.handle(start()),true);assert.equal(r.nodes.length,1);assert.equal(r.peers.length,1);
});
test('关闭后的原offer/answer承诺不得发迟到成功或重新建立节点',async()=>{
  const r=rig();let resolve;r.context.audioWorklet.addModule=()=>new Promise(r=>{resolve=r;});const pending=r.media.handle(start());r.media.close();resolve();assert.equal(await pending,false);assert.equal(r.nodes.length,0);assert.equal(r.peers.length,0);assert.equal(r.track.stops,0);
  const a=rig();await a.media.handle(start());let answer;a.peers[0].setRemoteDescription=()=>new Promise(r=>{answer=r;});const second=a.media.handle(command('humanVoiceAnswer',{sdp:a.sdp}));a.media.close();answer();assert.equal(await second,false);assert.equal(a.facts.some(v=>v.kind==='humanVoiceAnswered'),false);
});
test('原远端audio轨才准接收，额外track不形成第二输出源',async()=>{
  const r=rig();await r.media.handle(start());const p=r.peers[0];const remote={kind:'audio',readyState:'live'};p.ontrack({receiver:p.t.receiver,track:remote});assert.equal(r.media.remote,remote);
  p.ontrack({receiver:p.t.receiver,track:remote});assert.equal(r.media.closed,true);assert.equal(r.track.stops,0);
});
test('源无效、48k不符、未知字段/种类以及未绑定停止均明确拒绝',async()=>{
  const r=rig();r.context.sampleRate=16000;assert.equal(await r.media.handle(start()),false);assert.equal(await r.media.initialize('9'),false);assert.equal(r.peers.length,0);
  const a=rig();assert.equal(await a.media.handle({...start(),channels:2}),false);assert.equal(await a.media.handle({...start(),private:true}),false);assert.equal(await a.media.handle(command('humanVoiceStop')),false);assert.equal(await a.media.handle({kind:'unknown'}),false);
});
test('同scope并发Start只允许一次原Peer创建',async()=>{
  const r=rig();let resolve;r.context.audioWorklet.addModule=()=>new Promise(r=>{resolve=r;});
  const first=r.media.handle(start()),second=r.media.handle(start());resolve();
  const results=await Promise.all([first,second]);assert.equal(results[1],false);assert.equal(results[0],true);assert.equal(r.peers.length,1);
});
test('原native close拒绝不能伪造Stopped完成',async()=>{
  const r=rig();await r.media.handle(start());r.peers[0].close=()=>{throw new Error('fixed fixture');};r.media.close();assert.equal(r.facts.some(v=>v.kind==='humanVoiceStopped'),false);
});
test('候选原native codec配置失败只停止本候选，不抛至共享宿主',async()=>{
  const r=rig();r.peers.push=()=>{throw new Error('fixed native configuration failure');};
  await assert.doesNotReject(async()=>assert.equal(await r.media.handle(start()),false));
  assert.equal(r.media.closed,true);assert.equal(r.track.stops,0);
});
test('同原scope已停止的重复Stop幂等，不能停止别的腿',async()=>{
  const r=rig();await r.media.handle(start());r.media.close();assert.equal(await r.media.handle(command('humanVoiceStop')),true);assert.equal(await r.media.handle({...command('humanVoiceStop'),sessionId:'other'}),false);
});
test('接收原轨必须实际启动静音native解码sink，结束只释放本sink',async()=>{
  const r=rig();await r.media.handle(start());const peer=r.peers[0],remote={kind:'audio',readyState:'live'};
  peer.ontrack({receiver:peer.t.receiver,track:remote});await Promise.resolve();assert.equal(r.audios.length,1);assert.equal(r.audios[0].muted,true);assert.equal(r.audios[0].paused,false);assert.equal(r.audios[0].srcObject.getAudioTracks()[0],remote);
  r.media.close();assert.equal(r.audios[0].paused,true);assert.equal(r.audios[0].srcObject,null);assert.equal(r.track.stops,0);
});
test('原接收sink play拒绝只关闭本候选，迟到play成功也不能重新保留接收源',async()=>{
  const r=rig();await r.media.handle(start());r.Audio.prototype.play=()=>Promise.reject(new Error('fixed rejected'));
  r.peers[0].ontrack({receiver:r.peers[0].t.receiver,track:{kind:'audio',readyState:'live'}});await Promise.resolve();await Promise.resolve();assert.equal(r.media.closed,true);assert.equal(r.audios[0].srcObject,null);assert.equal(r.track.stops,0);
  const late=rig();await late.media.handle(start());let resolve;late.Audio.prototype.play=()=>new Promise(r=>{resolve=r;});
  late.peers[0].ontrack({receiver:late.peers[0].t.receiver,track:{kind:'audio',readyState:'live'}});late.media.close();resolve();await Promise.resolve();
  assert.equal(late.audios[0].paused,true);assert.equal(late.audios[0].srcObject,null);assert.equal(late.facts.filter(v=>v.kind==='humanVoiceStopped').length,1);assert.equal(late.track.stops,0);
});

// stopSnapshot只读取真实回调已锁存的首因，不给夹具手工赋诊断结果。
const stopSnapshot = r => JSON.parse(JSON.stringify(r.media.audioSnapshot()));
test('停止首因默认关闭，精确开启后纯读取未知；显式关闭不覆盖原回调首因', async()=>{
  for(const flag of [undefined,false,'1',1]) {
    const r=rig(flag);await r.media.handle(start());r.nodes[0].onprocessorerror();
    assert.equal(r.media.audioSnapshot(),null);assert.equal(r.track.stops,0);
  }
  const r=rig();assert.equal(r.media.audioSnapshot(),null);r.media.enableStopDiagnostics();
  assert.deepEqual(stopSnapshot(r),{known:false,cause:'unknown'});
  await r.media.handle(start());const count=r.facts.length;r.nodes[0].onprocessorerror();
  assert.deepEqual(stopSnapshot(r),{known:true,cause:'processorError'});
  r.media.close();assert.deepEqual(stopSnapshot(r),{known:true,cause:'processorError'});
  assert.equal(r.facts.length,count+1);assert.deepEqual(Object.keys(r.facts.at(-1)).sort(),['kind','peerEpoch','routeGeneration','sessionId']);
  const reads=r.facts.length;stopSnapshot(r);stopSnapshot(r);assert.equal(r.facts.length,reads);
});
test('原Peer new/connecting回调不误停，failed/disconnected锁存不同首因',async()=>{
  for(const state of ['new','connecting','failed','disconnected']) {
    const r=rig(true);await r.media.handle(start());const peer=r.peers[0];
    peer.connectionState=state;peer.onconnectionstatechange();peer.oniceconnectionstatechange();
    const failed=['failed','disconnected'].includes(state);
    assert.equal(r.media.closed,failed);
    assert.deepEqual(stopSnapshot(r),failed?{known:true,cause:state==='failed'?'peerFailed':'peerDisconnected'}:{known:false,cause:'unknown'});
    assert.equal(r.facts.filter(v=>v.kind==='humanVoiceStopped').length,failed?1:0);assert.equal(r.track.stops,0);
  }
});
test('原sink拒绝和重复远端轨分别锁存首因，停止后迟到承诺不改首因',async()=>{
  const r=rig(true);await r.media.handle(start());r.Audio.prototype.play=()=>Promise.reject(Error('SECRET'));
  r.peers[0].ontrack({receiver:r.peers[0].t.receiver,track:{kind:'audio'}});
  await Promise.resolve();await Promise.resolve();assert.deepEqual(stopSnapshot(r),{known:true,cause:'remoteSinkRejected'});
  r.media.close();assert.deepEqual(stopSnapshot(r),{known:true,cause:'remoteSinkRejected'});assert.equal(JSON.stringify(stopSnapshot(r)).includes('SECRET'),false);
  const duplicate=rig(true);await duplicate.media.handle(start());const event={receiver:duplicate.peers[0].t.receiver,track:{kind:'audio'}};
  duplicate.peers[0].ontrack(event);duplicate.peers[0].ontrack(event);
  assert.deepEqual(stopSnapshot(duplicate),{known:true,cause:'remoteTrackInvalid'});
  const late=rig(true);await late.media.handle(start());let reject;
  late.Audio.prototype.play=()=>new Promise((_r,j)=>{reject=j;});late.peers[0].ontrack({receiver:late.peers[0].t.receiver,track:{kind:'audio'}});
  late.media.close();reject(Error('SECRET'));await Promise.resolve();await Promise.resolve();
  assert.deepEqual(stopSnapshot(late),{known:true,cause:'explicitClose'});assert.equal(late.track.stops,0);
});
test('原Start/Answer及接收图异常只保闭集首因，成功与取消不造异常',async()=>{
  const startFailure=rig(true);startFailure.peers.push=()=>{throw Error('SECRET');};
  assert.equal(await startFailure.media.handle(start()),false);assert.deepEqual(stopSnapshot(startFailure),{known:true,cause:'startException'});
  const moduleFailure=rig(true);moduleFailure.context.audioWorklet.addModule=async()=>{throw Error('SECRET');};
  assert.equal(await moduleFailure.media.handle(start()),false);assert.deepEqual(stopSnapshot(moduleFailure),{known:true,cause:'startOutputUnavailable'});
  const answer=rig(true);await answer.media.handle(start());answer.peers[0].setRemoteDescription=async()=>{throw Error('SECRET');};
  assert.equal(await answer.media.handle(command('humanVoiceAnswer',{sdp:answer.sdp})),false);assert.deepEqual(stopSnapshot(answer),{known:true,cause:'answerException'});
  const receive=rig(true);await receive.media.handle(start());receive.context.createMediaStreamSource=()=>{throw Error('SECRET');};
  receive.peers[0].ontrack({receiver:receive.peers[0].t.receiver,track:{kind:'audio'}});assert.deepEqual(stopSnapshot(receive),{known:true,cause:'remoteTrackSetupFailed'});
  const normal=rig(true);await normal.media.handle(start());await normal.media.handle(command('humanVoiceAnswer',{sdp:normal.sdp}));normal.connected();
  assert.deepEqual(stopSnapshot(normal),{known:false,cause:'unknown'});assert.equal(await normal.media.handle(command('humanVoiceStop')),true);
  assert.deepEqual(stopSnapshot(normal),{known:true,cause:'explicitClose'});assert.equal(normal.track.stops,0);assert.equal(normal.context.state,'running');
});
test('开启前已停止不能追溯首因，取消中的迟到Start异常不覆盖原关闭',async()=>{
  const early=rig();await early.media.handle(start());early.nodes[0].onprocessorerror();
  assert.equal(early.media.audioSnapshot(),null);early.media.enableStopDiagnostics();
  assert.deepEqual(stopSnapshot(early),{known:false,cause:'unknown'});
  const cancelled=rig(true);let reject;cancelled.context.audioWorklet.addModule=()=>new Promise((_r,j)=>{reject=j;});
  const pending=cancelled.media.handle(start());cancelled.media.close();reject(Error('SECRET'));assert.equal(await pending,false);
  assert.deepEqual(stopSnapshot(cancelled),{known:true,cause:'explicitClose'});
  assert.equal(cancelled.peers.length,0);assert.equal(cancelled.nodes.length,0);assert.equal(cancelled.track.stops,0);
});
