// 文件说明：Human纯语音借用唯一采集源/context；候选sender始终空，输入所有权只归InputOwnershipGate。
(() => {
  'use strict';
  // plain/exact/scope仅接受本地原宿主固定字段，不从当前播放腿补身份。
  const plain = v => v && typeof v === 'object' && !Array.isArray(v);
  const exact = (v, keys) => plain(v) && Object.keys(v).length === keys.length && keys.every(k=>Object.hasOwn(v,k));
  const decimal = v => typeof v === 'string' && /^[1-9][0-9]*$/.test(v) && BigInt(v)<=9007199254740991n;
  const ticket = v => typeof v === 'string' && /^[1-9][0-9]*$/.test(v) && BigInt(v)<=18446744073709551615n;
  // Worklet没有Window TextEncoder；使用ECMAScript UTF8编码并拒绝非法surrogate，双线程同一字节上限。
  const identifier = v => {try{return typeof v==='string'&&v.length&&!/[\x00-\x1f\x7f]/u.test(v)&&encodeURIComponent(v).replace(/%[0-9A-F]{2}|./g,'x').length<=64;}catch{return false;}};
  const scope = v => plain(v) && identifier(v.sessionId) && decimal(v.routeGeneration) && decimal(v.peerEpoch);
  const same = (a,b) => a && b && a.sessionId===b.sessionId && a.routeGeneration===b.routeGeneration && a.peerEpoch===b.peerEpoch;
  const scopeKeys = ['sessionId','routeGeneration','peerEpoch'];
  // stopCauses仅分类原停止分支；不记录异常正文、SDP、会话、代次或媒体内容。
  const stopCauses = new Set(['processorError','startOutputUnavailable','startException',
    'answerException','remoteTrackInvalid','remoteTrackSetupFailed','remoteSinkRejected',
    'peerFailed','peerDisconnected','explicitClose','unknown']);
  // SDP限制单audio，不把opus RTP/2误当成第二物理声道。
  const voiceSdp = v => typeof v==='string' && v.length<=60000 && v.startsWith('v=0') && !v.includes('\0') && (v.match(/(?:^|\n)m=/g)||[]).length===1 && /\nm=audio /.test(v) && /opus\/48000\/2/.test(v);
  if (typeof AudioWorkletProcessor === 'function' && typeof registerProcessor === 'function') {
    // HumanVoiceOutputProcessor的process是唯一实际输出完成事实源；默认每样本零输出。
    class HumanVoiceOutputProcessor extends AudioWorkletProcessor {
      constructor(options) {
        super(); this.scope=scope(options?.processorOptions)?{...options.processorOptions}:null;
        this.boot=decimal(options?.processorOptions?.bootGeneration)?options.processorOptions.bootGeneration:null;
        this.enabled=false; this.pending=null; this.last=0n; this.frames=0; this.non=0; this.stopped=false;
        this.port.onmessage=e=>this.command(e.data);
      }
      // command只排原票据，不能在消息线程生成完成ACK；bind仅在物理静音时允许。
      command(v) {
        if (exact(v,['kind',...scopeKeys]) && v.kind==='humanVoiceOutputBind' && scope(v) && !this.enabled && !this.pending && !this.scope) { this.scope={sessionId:v.sessionId,routeGeneration:v.routeGeneration,peerEpoch:v.peerEpoch}; this.last=0n; return; }
        if (exact(v,['kind']) && v.kind==='humanVoiceOutputStop') { this.stopped=true; this.enabled=false; this.pending=null; return; }
        const idle=exact(v,['kind','bootGeneration','ticket','enabled']) && v.kind==='humanVoiceOutputInit' && !this.scope && v.bootGeneration===this.boot && v.enabled===false;
        const bound=exact(v,['kind',...scopeKeys,'ticket','enabled']) && v.kind==='humanVoiceOutputGate' && same(v,this.scope) && typeof v.enabled==='boolean';
        if ((!idle&&!bound)||!ticket(v.ticket)||BigInt(v.ticket)<=this.last||this.pending) return;
        this.last=BigInt(v.ticket); this.enabled=v.enabled; this.pending={...v,idle};
      }
      // process复制原接收轨到单声道输出；统计只计实际渲染block，不把入队当播放。
      process(inputs,outputs) {
        for(const bus of outputs) for(const channel of bus) channel.fill(0);
        const output=outputs[0]?.[0]; if(this.stopped) return false;
        if(!output?.length) return true;
        const input=inputs[0]?.[0]; let audible=false;
        if(this.enabled&&input) for(let i=0;i<output.length;i++){const value=Number.isFinite(input[i])?input[i]:0; output[i]=value; if(Math.abs(value)>1/32768) audible=true;}
        this.frames=Math.min(4294967295,this.frames+1); if(audible)this.non=Math.min(4294967295,this.non+1);
        if(this.pending){const v=this.pending; this.pending=null;
          this.port.postMessage(v.idle?{kind:'humanVoiceOutputIdle',bootGeneration:this.boot,ticket:v.ticket,enabled:false,frames:this.frames}:{kind:'humanVoiceOutputGated',...this.scope,ticket:v.ticket,enabled:this.enabled,frames:this.frames,nonSilentFrames:this.non});
        } else if(this.scope&&this.frames%128===0) this.port.postMessage({kind:'humanVoiceOutputProgress',...this.scope,frames:this.frames,nonSilentFrames:this.non});
        return true;
      }
    }
    registerProcessor('ur-human-voice-output',HumanVoiceOutputProcessor); return;
  }
  // UrHumanVoiceMedia只拥有Peer、remote source和输出节点；共享stream/source/context均由宿主持有。
  class UrHumanVoiceMedia {
    constructor({stream,source,context,emit,moduleUrl='/human-voice-media.js',observeStop=false,emitDiagnostic=null}) {
      this.stream=stream;this.source=source;this.context=context;this.emit=typeof emit==='function'?emit:()=>{};this.moduleUrl=moduleUrl;
      this.scope=null;this.boot=null;this.node=null;this.peer=null;this.transceiver=null;this.remote=null;this.remoteSource=null;this.receiverSink=null;
      this.closed=false;this.starting=false;this.answered=false;this.connectedSent=false;this.gate=null;this.frames=0;this.nonSilentFrames=0;this.initPromise=null;
      this.observeStop=observeStop===true;this.stopCause=null; // 默认不观察；首个原close分支锁存后不可被清理覆盖。
      this.emitDiagnostic=typeof emitDiagnostic==='function'?emitDiagnostic:null; // 独立非release发送端口；缺省不发旁证，不复用业务emit。
    }
    // enableStopDiagnostics只开启本地旁证；不建立资源、追溯旧故障或改变原驱动调用。
    enableStopDiagnostics(){this.observeStop=true;}
    // audioSnapshot是纯读闭集副本；unknown不证明无故障，关闭后仍保留原首因。
    audioSnapshot(){return this.observeStop?{known:this.stopCause!==null&&this.stopCause!=='unknown',cause:this.stopCause===null?'unknown':this.stopCause}:null;}
    // live回读原native源，只是可用性不是采集所有权；本类从不replaceTrack。
    live() { try { const tracks=this.stream.getAudioTracks();return !this.closed&&this.stream instanceof MediaStream&&this.source instanceof MediaStreamAudioSourceNode&&this.source.mediaStream===this.stream&&this.source.context===this.context&&this.context instanceof AudioContext&&this.context.state==='running'&&this.context.sampleRate===48000&&tracks.length===1&&this.stream.getVideoTracks().length===0&&tracks[0].readyState==='live'&&tracks[0].enabled;}catch{return false;} }
    // initialize仅建同context默认静音节点；返回不证明process已经确认false。
    async initialize(bootGeneration) {
      if(!decimal(bootGeneration)||!this.live()||this.boot||this.scope)return false;
      this.boot=bootGeneration;return this.outputNode({bootGeneration});
    }
    async outputNode(options) {
      if(this.node)return true;
      if(this.initPromise)return this.initPromise;
      this.initPromise=(async()=>{await this.context.audioWorklet.addModule(this.moduleUrl);if(!this.live())return false;
        const node=new AudioWorkletNode(this.context,'ur-human-voice-output',{numberOfInputs:1,numberOfOutputs:1,outputChannelCount:[1],channelCount:1,channelCountMode:'explicit',processorOptions:options});
        if(!this.live()){node.disconnect();node.port.close();return false;}
        this.node=node;node.port.onmessage=e=>this.outputFact(node,e.data);node.onprocessorerror=()=>this.close('processorError');node.connect(this.context.destination);return true;
      })();
      try{return await this.initPromise;}catch{return false;}
    }
    // outputFact只读取原节点port的实际线程事实；外部handle不能注入这个回执。
    outputFact(node,v) {
      if(this.closed||node!==this.node||!this.live())return;
      const counts=plain(v)&&Number.isSafeInteger(v.frames)&&v.frames>0&&v.frames<=4294967295&&Number.isSafeInteger(v.nonSilentFrames)&&v.nonSilentFrames>=0&&v.nonSilentFrames<=v.frames;
      if(exact(v,['kind',...scopeKeys,'frames','nonSilentFrames'])&&v.kind==='humanVoiceOutputProgress'&&same(v,this.scope)&&counts){this.frames=v.frames;this.nonSilentFrames=v.nonSilentFrames;return;}
      const idle=exact(v,['kind','bootGeneration','ticket','enabled','frames'])&&v.kind==='humanVoiceOutputIdle'&&!this.scope&&v.bootGeneration===this.boot&&v.enabled===false&&Number.isSafeInteger(v.frames)&&v.frames>0;
      const bound=exact(v,['kind',...scopeKeys,'ticket','enabled','frames','nonSilentFrames'])&&v.kind==='humanVoiceOutputGated'&&same(v,this.scope)&&counts;
      if((!idle&&!bound)||!this.gate||v.ticket!==this.gate.ticket||v.enabled!==this.gate.enabled)return;
      if(bound&&v.enabled&&!this.actualConnected())return;
      this.gate=null;if(bound){this.frames=v.frames;this.nonSilentFrames=v.nonSilentFrames;}this.publish(v);
    }
    publish(v){try{this.emit({...v});}catch{/* 回调失败不改变媒体事实或再发命令。 */}}
    // handle仅接受认证宿主原本地控制；不接MQTT、不造第二物理源。
    async handle(v) {
      if(!plain(v)||typeof v.kind!=='string')return false;
      if(this.closed)return v.kind==='humanVoiceStop'&&exact(v,['kind',...scopeKeys])&&same(v,this.scope);
      if(v.kind==='humanVoiceOutputInit'){
        if(!exact(v,['kind','bootGeneration','ticket','enabled'])||v.bootGeneration!==this.boot||this.scope||v.enabled!==false||!ticket(v.ticket)||this.gate||!this.node)return false;
        this.gate={...v};this.node.port.postMessage(v);return true;
      }
      if(v.kind==='humanVoiceOutputBind'){
        if(!exact(v,['kind',...scopeKeys])||!scope(v)||!this.boot||!this.node||this.scope||this.gate||!this.live())return false;
        this.scope={sessionId:v.sessionId,routeGeneration:v.routeGeneration,peerEpoch:v.peerEpoch};this.node.port.postMessage(v);return true;
      }
      if(v.kind==='humanVoiceStart'){
        if(!exact(v,['kind',...scopeKeys,'sampleRate','channels','frameMs'])||!scope(v)||v.sampleRate!==48000||v.channels!==1||v.frameMs!==20||this.starting||this.peer||(this.scope&&!same(v,this.scope))||!this.live())return false;
        this.starting=true; // await原module/SDP期间也只接纳一次，不并发制造第二Peer。
        this.scope={sessionId:v.sessionId,routeGeneration:v.routeGeneration,peerEpoch:v.peerEpoch};
        if(!await this.outputNode(this.scope)){this.close('startOutputUnavailable');return false;}
        if(!this.live()){this.close('startOutputUnavailable');return false;}
        let peer;
        try{this.peer=new RTCPeerConnection({iceServers:[]});peer=this.peer;
          this.transceiver=peer.addTransceiver('audio',{direction:'sendrecv'});
          const codecs=RTCRtpSender.getCapabilities('audio')?.codecs?.filter(c=>c.mimeType.toLowerCase()==='audio/opus'&&c.clockRate===48000);
          if(!codecs?.length||this.transceiver.sender.track!==null){this.close('startException');return false;}
          this.transceiver.setCodecPreferences(codecs);peer.ontrack=e=>this.receiveTrack(peer,e);
          peer.onconnectionstatechange=()=>this.networkFact(peer);peer.oniceconnectionstatechange=()=>this.networkFact(peer);
          const offer=await peer.createOffer();if(!this.current(peer))return false;
          const sdp=offer.sdp.replace(/a=fmtp:(\d+) ([^\r\n]*)/g,(line,p,settings)=>`${line};stereo=0;sprop-stereo=0`).replace(/(m=audio[^\r\n]*\r\n)/,'$1a=ptime:20\r\n');
          await peer.setLocalDescription({type:'offer',sdp});await this.gather(peer);
          if(!this.current(peer)||!voiceSdp(peer.localDescription?.sdp))return false;
          this.publish({kind:'humanVoiceOffer',...this.scope,sdp:peer.localDescription.sdp});return true;
        }catch{if(!this.closed)this.close('startException');return false;}
      }
      if(!scope(v)||!same(v,this.scope))return false;
      if(v.kind==='humanVoiceAnswer'){
        if(!exact(v,['kind',...scopeKeys,'sdp'])||!voiceSdp(v.sdp)||this.answered||!this.peer)return false;const peer=this.peer;
        try{await peer.setRemoteDescription({type:'answer',sdp:v.sdp});if(!this.current(peer))return false;this.answered=true;this.publish({kind:'humanVoiceAnswered',...this.scope});this.networkFact(peer);return true;}catch{if(this.current(peer))this.close('answerException');return false;}
      }
      if(v.kind==='humanVoiceOutputGate'){
        if(!exact(v,['kind',...scopeKeys,'ticket','enabled'])||!ticket(v.ticket)||typeof v.enabled!=='boolean'||this.gate||!this.node||!this.live()||(v.enabled&&!this.actualConnected()))return false;
        this.gate={...v};this.node.port.postMessage(v);return true;
      }
      if(v.kind==='humanVoiceStop'&&exact(v,['kind',...scopeKeys])){this.close();return true;}
      return false;
    }
    current(peer){return this.live()&&peer===this.peer&&peer.connectionState!=='closed';}
    // gather只等待原ICE收集，close解除等待；不重发offer或建立其他Peer。
    gather(peer){if(peer.iceGatheringState==='complete')return Promise.resolve();return new Promise((resolve,reject)=>{const finish=()=>{clearTimeout(timer);peer.removeEventListener('icegatheringstatechange',check);peer.removeEventListener('connectionstatechange',check);};const check=()=>{if(!this.current(peer)){finish();reject(new Error('closed'));}else if(peer.iceGatheringState==='complete'){finish();resolve();}};const timer=setTimeout(()=>{finish();reject(new Error('timeout'));},5000);peer.addEventListener('icegatheringstatechange',check);peer.addEventListener('connectionstatechange',check);check();});}
    // receiveTrack仅接原transceiver的唯一远端轨，专属remote source不等同共享采集source。
    receiveTrack(peer,e){
      if(!this.current(peer)||e.receiver!==this.transceiver.receiver||e.track.kind!=='audio'||this.remote){if(this.current(peer))this.close('remoteTrackInvalid');return;}
      try{this.remote=e.track;const received=new MediaStream([e.track]);this.remoteSource=this.context.createMediaStreamSource(received);this.remoteSource.connect(this.node);
        // Chrome的原RTC解码器需实际启动接收sink；此sink永远静音，唯一可听输出仍由Worklet门控。
        const sink=new Audio();sink.muted=true;sink.playsInline=true;sink.srcObject=received;this.receiverSink=sink;
        void sink.play().then(()=>{if(!this.current(peer)||sink!==this.receiverSink){sink.pause();sink.srcObject=null;}},()=>{if(this.current(peer)&&sink===this.receiverSink)this.close('remoteSinkRejected');});
      }catch{if(this.current(peer))this.close('remoteTrackSetupFailed');}
    }
    actualConnected(){try{return this.current(this.peer)&&this.answered&&this.peer.connectionState==='connected'&&['connected','completed'].includes(this.peer.iceConnectionState)&&this.transceiver.sender.transport?.state==='connected'&&this.transceiver.receiver.transport?.state==='connected';}catch{return false;}}
    networkFact(peer){if(this.current(peer)&&this.actualConnected()&&!this.connectedSent){this.connectedSent=true;this.publish({kind:'humanVoiceConnected',...this.scope});}else if(this.current(peer)&&['failed','disconnected'].includes(peer.connectionState))this.close(peer.connectionState==='failed'?'peerFailed':'peerDisconnected');}
    // inputBinding只返回原实际sender；调用方仍须取得独立完整组焦点授权。
    inputBinding(session,generation){if(!this.live()||!this.scope||session!==this.scope.sessionId||String(generation)!==this.scope.routeGeneration||!this.peer||this.peer.getTransceivers().length!==1)return null;return {peer:this.peer,sender:this.transceiver.sender};}
    actualSenders(){return this.peer?this.peer.getSenders():[];}
    statistics(){return {frames:this.frames,nonSilentFrames:this.nonSilentFrames,closed:this.closed,connected:this.actualConnected()};}
    // close仅释放本Peer和本remote输出节点；cause只锁存本地闭集首因，不改变原四字段停止报文。
    close(cause='explicitClose'){if(this.closed)return;if(this.observeStop&&this.stopCause===null)this.stopCause=stopCauses.has(cause)?cause:'unknown';this.closed=true;this.gate=null;let confirmed=true;
      try{this.peer?.close();if(this.peer&&this.peer.connectionState!=='closed')confirmed=false;}catch{confirmed=false;}
      try{this.remoteSource?.disconnect();}catch{confirmed=false;}
      try{if(this.receiverSink){this.receiverSink.pause();this.receiverSink.srcObject=null;if(!this.receiverSink.paused||this.receiverSink.srcObject!==null)confirmed=false;}}catch{confirmed=false;}
      try{this.node?.port.postMessage({kind:'humanVoiceOutputStop'});this.node?.disconnect();this.node?.port.close();}catch{confirmed=false;}
      // 原close首因通过同一本地桥至多旁观一次；异常不能阻止原四字段Stopped或改变其确认条件。
      if(this.observeStop&&this.scope&&this.emitDiagnostic){try{this.emitDiagnostic({kind:'humanVoiceStopDiagnostic',...this.scope,known:this.stopCause!=='unknown',cause:this.stopCause});}catch{/* 旁证失败不重发、不触发release，缺席仍未知。 */}}
      if(this.scope&&confirmed)this.publish({kind:'humanVoiceStopped',...this.scope});
    }
  }
  globalThis.UrHumanVoiceMedia=UrHumanVoiceMedia;
})();
