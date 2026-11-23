// 文件说明：Live浏览器驱动确定性模拟；显式手交编码/PCM/帧事实，不代表真实Chrome或媒体端云验收。
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 独立模拟Port/Peer与呈现驱动；每个用例隔离节点及原generation，不伪造生产授权。
function fixture(options = {}) {
  const sent = [], workers = [], nodes = [], peers = [], frames = new Map();
  let frameNumber = 0, audioTime = 0;
  // Port 本地模拟消息仅按显式原控制回调；不访问实际WebSocket/MQTT或设备凭据。
  class Port {
    constructor() { this.onmessage = null; this.closed = false; this.commands = []; }
    postMessage(value) { if (this.closed) throw new Error('closed');
      this.commands.push(value); this.command?.(value); }
    emit(value) { if (!this.closed) this.onmessage?.({data: value}); }
    close() { this.closed = true; }
    start() {}
  }
  class MessageChannel { constructor() { this.port1 = new Port(); this.port2 = {original: this.port1}; } }
  // Worker 本例仅模拟实际pipeline事件；fresh帧必须用emitFresh明确交付，不能凭resume造active。
  class Worker {
    constructor(url) { this.url = url; this.closed = false; workers.push(this); }
    terminate() { this.closed = true; }
  }
  class Transform {
    constructor(worker, value) {
      worker.kind = value.kind; worker.port = value.port.original;
      worker.port.command = command => {
        queueMicrotask(() => {
          if (command.kind === 'liveTransformMask') worker.port.emit({kind: 'liveTransformMasked',
            ticket: command.ticket, enabled: command.enabled});
          else if (command.kind === 'liveTransformTrack') worker.port.emit({
            kind: 'liveTransformTrackAccepted', generation: command.generation});
          else if (command.kind === 'liveTransformClose' && !options.failedTransformClose)
            worker.port.emit({kind: 'liveTransformClosed'});
        });
      };
      queueMicrotask(() => worker.port.emit({kind: 'liveTransformReady'}));
    }
  }
  // Track 停止只作用本原receiver，真实单轨/全组资源断言均检查readyState。
  class Track { constructor(kind) { this.kind = kind; this.readyState = 'live'; }
    stop() { this.readyState = 'ended'; } }
  class Peer {
    constructor() { this.connectionState = 'new'; this.iceGatheringState = 'complete';
      this.transceivers = []; this.localDescription = null; peers.push(this); }
    addTransceiver(kind, config) {
      const transceiver = {kind, direction: config.direction, receiver: {track: new Track(kind)},
        setCodecPreferences: codecs => { transceiver.codecs = codecs; }};
      this.transceivers.push(transceiver); return transceiver;
    }
    addEventListener() {}
    removeEventListener() {}
    async createOffer() { return {type: 'offer', sdp: 'v=0\r\ns=contract-only\r\n'}; }
    async setLocalDescription(value) { this.localDescription = value; }
    async setRemoteDescription() { this.connectionState = 'connected'; this.onconnectionstatechange?.(); }
    close() { this.connectionState = 'closed'; }
  }
  // Video 每帧必须测试显式交真实模拟stamp，play受理不会交帧；便于复现迟到/旧帧竞态。
  class Video {
    constructor() { this.style = {}; this.videoWidth = this.videoHeight = 32; this.removed = false;
      this.srcObject = null; this.playing = false; }
    requestVideoFrameCallback(callback) { const id = ++frameNumber; frames.set(id, callback); return id; }
    cancelVideoFrameCallback(id) { frames.delete(id); }
    async play() { this.playing = true; }
    pause() { this.playing = false; }
    remove() { this.removed = true; }
  }
  class Canvas {
    constructor() { this.style = {display: 'none'}; this.width = this.height = 32; this.draws = 0; }
    getContext() { return {drawImage: () => { ++this.draws; }, clearRect: () => { this.draws = 0; }}; }
    getBoundingClientRect() { return {width: 32, height: 32}; }
    remove() { this.removed = true; }
  }
  // Node 音频真实active只由emitAudio交模拟PCM事实；pause/mask/close为模拟实际音频块完成。
  class AudioNode {
    constructor() { this.port = new Port(); this.connected = false; this.disconnected = false; nodes.push(this);
      this.port.command = command => { queueMicrotask(() => {
        ++audioTime;
        if (command.kind === 'liveAudioMask') this.port.emit({kind: 'liveAudioMasked',
          ticket: command.ticket, enabled: command.enabled});
        else if (command.kind === 'liveAudioTrack' && command.operation !== 'resume')
          this.port.emit({kind: 'liveAudioTrackApplied', generation: command.generation,
            state: command.operation === 'stop' ? 'stopped' : 'paused'});
        else if (command.kind === 'liveAudioClose') this.port.emit({kind: 'liveAudioClosed'});
      }); };
    }
    connect() { this.connected = true; }
    disconnect() { this.disconnected = true; }
  }
  // Context 假驱动支持相同端口合同；不是native采样或WebRTC解码证据。
  class Context {
    constructor() { this.sampleRate = options.sampleRate || 48000; this.state = 'suspended';
      this.destination = {}; this.audioWorklet = {addModule: async () => {}}; this.sources = []; }
    async resume() { this.state = 'running'; }
    async close() { this.state = 'closed'; }
    createGain() { return {gain: {value: 1}, connect() {}, disconnect() { this.disconnected = true; }}; }
    createMediaStreamSource() { const source = {connect() { this.connected = true; },
      disconnect() { this.disconnected = true; }}; this.sources.push(source); return source; }
  }
  const root = {children: [], appendChild(value) { this.children.push(value); }};
  const context = vm.createContext({setTimeout: (callback, milliseconds) => setTimeout(callback,
    options.fastTimeout && milliseconds >= 1500 ? 5 : milliseconds), clearTimeout, queueMicrotask, AbortController,
    console, Map, Set, Promise, Error, Number, Object, BigInt});
  Object.assign(context, {RTCPeerConnection: Peer, RTCRtpReceiver: {getCapabilities: kind => ({
    codecs: options.missingCodec ? [] : [{mimeType: kind === 'audio' ? 'audio/opus' : 'video/H264'}],
  })}, RTCRtpScriptTransform: Transform, Worker, MessageChannel, MediaStream: class {
    constructor(tracks) { this.tracks = tracks; } }, AudioWorkletNode: AudioNode, AudioContext: Context,
    HTMLVideoElement: Video, probeLiveReceive: async () => ({audio: true, video: true}),
    document: {body: root, createElement: kind => kind === 'video' ? new Video() : new Canvas()},
    getComputedStyle: value => ({display: value.style.display || 'block', visibility: 'visible', opacity: '1'}),
    requestAnimationFrame: callback => { const id = setTimeout(() => callback(0), 0); return id; },
    cancelAnimationFrame: clearTimeout});
  for (const name of ['live-player.js', 'live-host.js']) vm.runInContext(fs.readFileSync(path.join(
    __dirname, '../adapters/desktop/web', name), 'utf8'), context, {filename: name});
  const node = new AudioNode(), audio = new context.UrLiveMessages(node.port), gain = {gain: {value: 1}};
  const lane = {name: options.lane || 'audioVideo', ticket: '2', enabled: true, ready: true,
    audio, node, gain, canvas: new Canvas(), presented: async () => {}};
  const platform = Object.create(context); platform.liveContext = new Context(); platform.liveContext.state = 'running';
  const binding = {kind: 'liveBegin', instance: '1', mediaRevision: 1, lane: lane.name,
    audio: lane.name !== 'video', video: lane.name !== 'audio'};
  const player = () => new context.UrLivePlayer(binding, lane, value => sent.push({...value}), platform);
  const control = (kind, operation, generation = 1) => ({kind: 'liveTrackControl', instance: '1',
    mediaRevision: 1, trackId: kind === 'audio' ? 'broadcast-out' : 'broadcast-video-out',
    operation, generation, ...(operation === 'resume' ? {gateTicket: '2'} : {})});
  const emitFresh = (kind, generation = 1, stamp = 100, ticket = '2') => workers.findLast(value =>
    value.kind === kind)?.port.emit({kind: 'liveEncodedFresh', generation, gateTicket: ticket,
      ...(kind === 'video' ? {rtpTimestamp: stamp} : {})});
  const emitAudio = (generation = 1, ticket = '2') => node.port.emit({kind: 'liveAudioTrackApplied',
    generation, state: 'active', gateTicket: ticket});
  const frame = stamp => { const callbacks = [...frames.values()]; frames.clear();
    for (const callback of callbacks) void callback(0, {rtpTimestamp: stamp}); };
  return {context, platform, root, nodes, peers, workers, frames, lane, sent, binding,
    player, control, emitFresh, emitAudio, frame, audioTime: () => audioTime};
}
// flush 只推进本例微任务，不启动真实浏览器/媒体服务或等待网络。
const flush = () => new Promise(resolve => setImmediate(resolve));
module.exports = {fixture, flush};
