// 文件说明：原设备页面的独立节目接收宿主；只有固定kind分派，不覆盖AI/file回调或持有设备身份。
class UrLiveHost {
  constructor(send, root, platform = globalThis) {
    this.send = send; this.root = root; this.platform = platform; // 原本地事实出口及固定可见容器。
    this.context = null; this.lanes = new Map(); this.player = null; // 一个节目预约，三组末端基础资源。
    this.active = false; this.closing = false; this.closed = false; this.activation = null;
    this.lastInstance = 0n; this.gain = 1; // 本地实例单调水位和同物理speaker统一音量。
    this.readiness = Object.freeze({audio: false, video: false}); // 真实格式/接收pipeline自检，不凭API名字广告。
    this.life = new AbortController(); // 原宿主关闭取消自身等待，不触碰AI采集或播放节点。
  }
  // presented 真实两个浏览器呈现帧；超时/宿主关闭不伪造可见或已清屏事实。
  presented(signal) {
    const p = this.platform;
    let first = null, second = null, remove;
    const actual = new Promise((resolve, reject) => {
      const cancel = () => { if (first !== null) p.cancelAnimationFrame(first);
        if (second !== null) p.cancelAnimationFrame(second); reject(new Error('live_cancelled')); };
      signal?.addEventListener('abort', cancel, {once: true});
      remove = () => signal?.removeEventListener('abort', cancel);
      if (signal?.aborted) { cancel(); return; }
      first = p.requestAnimationFrame(() => { first = null;
        second = p.requestAnimationFrame(() => { second = null; resolve(); }); });
    });
    return liveWait(actual, 1500, signal).finally(() => { remove?.();
      if (first !== null) p.cancelAnimationFrame(first);
      if (second !== null) p.cancelAnimationFrame(second); });
  }
  // makeAudio 只构造默认关闭的该lane原新基础node，不重放旧true或复活旧播放意图。
  makeAudio(lane) {
    lane.audio?.close(); lane.node?.disconnect(); lane.gain?.disconnect();
    lane.node = new this.platform.AudioWorkletNode(this.context, 'ur-live-audio', {
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
    });
    lane.audio = new UrLiveMessages(lane.node.port);
    lane.nodeClosed = false; // 原新node实际退出与MessagePort逻辑关闭分别记录。
    lane.gain = this.context.createGain(); lane.gain.gain.value = this.gain;
    lane.gain.channelCount = 1; lane.gain.channelCountMode = 'explicit';
    lane.gain.connect(lane.node); lane.node.connect(this.context.destination);
    lane.ready = false; // 新programme仍须新的原false票据实际确认，不能借旧node的关闭事实。
    lane.audio.subscribe(value => { if (value?.kind === 'liveAudioFailed') this.player?.failed(); });
  }
  // probeTransform 为实际原RTCRtpScriptTransform绑定/关闭自检；无网络样本，不冒合法节目active。
  async probeTransform(kind) {
    const p = this.platform;
    if (typeof p.RTCRtpScriptTransform !== 'function' || typeof p.Worker !== 'function' ||
        typeof p.RTCRtpReceiver?.getCapabilities !== 'function') return false;
    let peer, worker, messages, receiver;
    let valid = false;
    try {
      peer = new p.RTCPeerConnection({iceServers: []});
      const transceiver = peer.addTransceiver(kind, {direction: 'recvonly'});
      receiver = transceiver.receiver;
      const codec = p.RTCRtpReceiver.getCapabilities(kind)?.codecs?.some(value =>
        value.mimeType.toLowerCase() === (kind === 'audio' ? 'audio/opus' : 'video/h264'));
      if (!codec) return false;
      worker = new p.Worker('/live-transform-worker.js');
      const ports = new p.MessageChannel(); messages = new UrLiveMessages(ports.port1);
      const ready = messages.expect(null, value => liveExact(value, ['kind']) &&
        value.kind === 'liveTransformReady', this.life.signal, 1500);
      receiver.transform = new p.RTCRtpScriptTransform(worker, {kind, port: ports.port2}, [ports.port2]);
      await ready;
      await messages.expect({kind: 'liveTransformClose'}, value =>
        liveExact(value, ['kind']) && value.kind === 'liveTransformClosed', this.life.signal, 1500);
      receiver.track.stop();
      valid = receiver.track.readyState === 'ended';
    } catch { valid = false; }
    finally {
      worker?.terminate(); messages?.close(); receiver?.track.stop(); peer?.close();
      if (peer && peer.connectionState !== 'closed') valid = false;
    }
    return valid;
  }
  // activate 只能在用户已有媒体激活手势调用；独立48k，不改AI16k、AEC声明或原采集默认值。
  activate() {
    if (this.closing) return Promise.resolve(false);
    if (this.activation) return this.activation;
    this.activation = (async () => {
      try {
        const p = this.platform;
        this.context = new p.AudioContext({sampleRate: 48000});
        if (this.context.sampleRate !== 48000) throw new Error('live_audio_format');
        await this.context.resume();
        if (this.closing) return false;
        if (this.context.state !== 'running') throw new Error('live_audio_not_running');
        await this.context.audioWorklet.addModule('/live-worklet.js');
        if (this.closing) return false;
        for (const name of ['audio', 'video', 'audioVideo']) {
          const canvas = p.document.createElement('canvas');
          canvas.style.display = 'none'; this.root.appendChild(canvas);
          const lane = {name, canvas, ticket: null, enabled: false, pending: false, ready: false,
            presented: signal => this.presented(signal)};
          this.makeAudio(lane); this.lanes.set(name, lane);
        }
        const format = await liveWait(p.probeLiveReceive(), 3500, this.life.signal);
        const audio = Boolean(format.audio) && await this.probeTransform('audio');
        const video = Boolean(format.video) && await this.probeTransform('video') &&
          typeof p.HTMLVideoElement?.prototype?.requestVideoFrameCallback === 'function';
        if (this.closing) return false;
        this.active = true; this.readiness = Object.freeze({audio, video});
        this.send({kind: 'liveReady', audio, video}); // lane实际false票据另走原C++门控，不能省略。
        return audio || video;
      } catch {
        this.readiness = Object.freeze({audio: false, video: false});
        if (!this.closing) this.send({kind: 'liveReady', audio: false, video: false});
        return false;
      }
    })();
    return this.activation;
  }
  // setGain 同物理speaker唯一音量的本地聚合入口；关闭/非法值拒绝，不新增MQTT或动作协议。
  setGain(value) {
    if (this.closing || this.closed || !Number.isFinite(value) || value < 0 || value > 1) return false;
    this.gain = value;
    for (const lane of this.lanes.values()) lane.gain.gain.value = value;
    return true;
  }
  // volume 本地唯一票据等待全部实际gain属性读回；原actionToken由C++保留，不交网页或伪造成ACK。
  volume(value) {
    if (!this.active || this.closing || this.volumePending || !liveExact(value,
        ['kind', 'volumeTicket', 'percent']) || value.kind !== 'liveVolume' ||
        !liveTicket(value.volumeTicket) || !Number.isInteger(value.percent) || value.percent < 0 ||
        value.percent > 100 || (this.volumeTicket && BigInt(value.volumeTicket) <= BigInt(this.volumeTicket)))
      return false;
    this.volumeTicket = value.volumeTicket; this.volumePending = true;
    const gain = value.percent / 100;
    void (async () => {
      try {
        if (!this.setGain(gain)) return;
        await this.presented(this.life.signal);
        if (this.closing || this.volumeTicket !== value.volumeTicket || this.lanes.size !== 3) return;
        // AudioParam使用float32存储；只允许目标值本身或其实际float32表示，不以自由误差预算蒙过。
        if (![...this.lanes.values()].every(lane =>
          lane.gain.gain.value === gain || lane.gain.gain.value === Math.fround(gain))) return;
        this.send({kind: 'liveVolumeApplied', volumeTicket: value.volumeTicket, percent: value.percent});
      } catch { /* 保留未知，不发成功；C++原请求自行超时，不能自动重试换票据。 */ }
      finally { this.volumePending = false; }
    })();
    return true;
  }
  // gate 固定三组实际末端票据；恢复不创建播放意图，未完成/错组/旧字节冲突不发成功事实。
  gate(value) {
    const lane = this.lanes.get(value?.lane);
    if (!this.active || this.closing || !lane || !liveExact(value,
        ['kind', 'lane', 'gateTicket', 'enabled']) || value.kind !== 'liveLaneGate' ||
        !liveTicket(value.gateTicket) || typeof value.enabled !== 'boolean' || lane.pending ||
        (!lane.ticket && value.enabled) || (lane.ticket && BigInt(value.gateTicket) <= BigInt(lane.ticket)))
      return false;
    lane.pending = true; lane.ticket = value.gateTicket; lane.enabled = value.enabled;
    // 关闭先更新当前意图屏障；已实际形成原active由FIFO事实仍保留，不按未来票据重开输出。
    const settle = (async () => {
      await lane.audio.expect({kind: 'liveAudioMask', ticket: value.gateTicket, enabled: value.enabled},
        fact => fact.kind === 'liveAudioMasked' && fact.ticket === value.gateTicket &&
        fact.enabled === value.enabled, this.life.signal, 1500);
      if (!value.enabled) { lane.canvas.getContext('2d').clearRect(0, 0,
        lane.canvas.width, lane.canvas.height); lane.canvas.style.display = 'none'; }
      await this.presented(this.life.signal);
    })();
    const original = this.player?.lane === lane ? this.player : null;
    const update = original ? original.maskChanged(value.enabled, value.gateTicket, settle) : settle;
    void Promise.all([settle, update]).then(() => {
      if (this.closing || lane.ticket !== value.gateTicket) return;
      lane.pending = false; lane.ready = true;
      this.send({kind: 'liveLaneGated', lane: lane.name, gateTicket: value.gateTicket,
        enabled: value.enabled});
    }, () => { lane.pending = false; lane.ready = false; original?.failed(); });
    return true;
  }
  // begin 只接受原C++完整本地绑定/已实际关闭组，不复用旧programme资源或默认继承焦点许可。
  begin(value) {
    const lane = this.lanes.get(value?.lane);
    if (!this.active || this.closing || this.player || !lane?.ready || lane.enabled || lane.pending ||
        !liveTicket(value?.instance) || BigInt(value.instance) <= this.lastInstance ||
        (value.audio && !this.readiness.audio) || (value.video && !this.readiness.video)) return false;
    try {
      const originalPlatform = this.platform;
      const p = Object.create(originalPlatform); p.liveContext = this.context;
      // 节目上下文保留在派生平台，Window的document getter与样式方法仍以原平台作receiver。
      // 原生品牌检查会拒绝派生对象，分别导致视频初始化或已绘制帧被误报失败；构造器不额外绑定。
      Object.defineProperty(p, 'document', {get: () => originalPlatform.document});
      p.getComputedStyle = element => originalPlatform.getComputedStyle(element);
      const player = new UrLivePlayer(value, lane, this.send, p);
      this.lastInstance = BigInt(value.instance); this.player = player;
      void player.initialize();
      return true;
    } catch { return false; }
  }
  // handle 明确kind分派，未知消息留给原宿主AI/file处理；不接受节目伪trackState或网页身份。
  handle(value) {
    if (value?.kind === 'liveVolume') return this.volume(value);
    if (value?.kind === 'liveLaneGate') return this.gate(value);
    if (value?.kind === 'liveBegin') return this.begin(value);
    const player = this.player;
    if (!player || value?.instance !== player.binding.instance || value?.mediaRevision !== 1) return false;
    if (value.kind === 'liveAnswer') { void player.answer(value); return true; }
    if (value.kind === 'liveTrackControl') return player.track(value);
    if (value.kind === 'liveClose' && liveExact(value, ['kind', 'instance', 'mediaRevision'])) {
      void this.closePlayer(player); return true;
    }
    return false;
  }
  // closePlayer 原programme实际退出事实独立于新基础node准备；新false票据未完成前不得新建Peer。
  async closePlayer(player) {
    const actual = await player.close();
    if (!actual || this.player !== player) return false;
    this.player = null;
    if (!this.closing && player.binding.audio) {
      this.makeAudio(player.lane); // 默认mask/track false，不同步残留true或自动resume。
      player.lane.enabled = false; // 新node不是旧票据actual事实；ready=false要求新的C++false票据。
    }
    return true;
  }
  // close 全宿主关闭不重建任何node；严格等待自身资源，不关闭AI/File的上下文或其它页面handler。
  async close() {
    if (this.closing) return this.completion;
    this.closing = true; this.life.abort();
    this.completion = (async () => {
      try {
        await this.activation; // 等实际取消完成，防止关闭之后原resume/probe再创建基础资源。
        if (this.player && !await this.closePlayer(this.player)) return false;
        for (const lane of this.lanes.values()) {
          // programme音频已关闭时原node无需再等第二个process(false)。
          if (!lane.audio.closed && !lane.nodeClosed) {
            await lane.audio.expect({kind: 'liveAudioClose'}, value =>
              value.kind === 'liveAudioClosed', undefined, 1500);
          }
          lane.audio.close(); lane.node.disconnect(); lane.gain.disconnect(); lane.canvas.remove();
        }
        await this.context?.close();
        if (this.context && this.context.state !== 'closed') return false;
        this.closed = true; this.active = false;
        return true;
      } catch { return false; }
    })();
    return this.completion;
  }
}
globalThis.UrLiveHost = UrLiveHost;
