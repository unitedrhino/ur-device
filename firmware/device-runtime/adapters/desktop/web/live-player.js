// 文件说明：合法节目接收器的浏览器驱动；不采集媒体，不持设备身份，网络/解码/可见输出分别确认。
// liveExact 仅校验固定本地对象；平台身份与授权始终留在C++，不能从网页自报扩权。
function liveExact(value, names) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === names.length && names.every(name => Object.hasOwn(value, name));
}
// liveTicket 限定原本地uint64票据，不把末端许可当来源授权或播放意图。
function liveTicket(value) {
  return typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value) &&
    BigInt(value) <= 18446744073709551615n;
}
// liveWait 有界等待原实际事实；取消/超时不伪造成功，不泄露自由异常或SDP。
function liveWait(operation, milliseconds = 5000, signal) {
  return new Promise((resolve, reject) => {
    let finished = false;
    const done = (error, result) => {
      if (finished) return;
      finished = true; clearTimeout(timer); signal?.removeEventListener('abort', aborted);
      error ? reject(new Error(error)) : resolve(result);
    };
    const aborted = () => done('live_cancelled');
    const timer = setTimeout(() => done('live_actual_timeout'), milliseconds);
    if (signal?.aborted) { aborted(); return; }
    signal?.addEventListener('abort', aborted, {once: true});
    Promise.resolve(operation).then(value => done(null, value), () => done('live_driver_failed'));
  });
}
// UrLiveMessages 一个原MessagePort的有界订阅器，避免多个媒体处理器覆盖onmessage或无界积压。
class UrLiveMessages {
  constructor(port) {
    this.port = port; // 只使用原本地端口，不含MQTT凭据。
    this.listeners = new Set(); // 最多8个原等待/观察订阅。
    this.closed = false; // 实际端口关闭后拒绝任何重建意图。
    port.onmessage = event => { for (const listener of [...this.listeners]) listener(event.data); };
    port.start?.();
  }
  // subscribe 返回精确取消函数；满额拒绝，不淘汰在途原事实。
  subscribe(listener) {
    if (this.closed || this.listeners.size >= 8) throw new Error('live_listener_full');
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  // post 仅本地原字节控制，受理不等于对应实际事实。
  post(value) { if (this.closed) throw new Error('live_port_closed'); this.port.postMessage(value); }
  // expect 在发控制之前登记原匹配事实，防止同步回调丢失；取消后精确移除订阅。
  async expect(command, predicate, signal, milliseconds = 5000) {
    let remove;
    try {
      if (signal?.aborted) throw new Error('live_cancelled');
      const original = new Promise(resolve => { remove = this.subscribe(value => {
        if (predicate(value)) resolve(value);
      }); });
      if (command) this.post(command);
      return await liveWait(original, milliseconds, signal);
    } finally { remove?.(); }
  }
  // close 只关闭该原本地端口，不能停AI或其它节目路径。
  close() { this.closed = true; this.listeners.clear(); this.port.close?.(); }
}

// UrLivePlayer 一组合法recvonly接收轨；lane末端资源共享但原Peer、generation和实际释放独立。
class UrLivePlayer {
  constructor(command, lane, send, platform = globalThis) {
    if (!liveExact(command, ['kind', 'instance', 'mediaRevision', 'lane', 'audio', 'video']) ||
        command.kind !== 'liveBegin' || !liveTicket(command.instance) ||
        BigInt(command.instance) > 9007199254740991n || command.mediaRevision !== 1 ||
        typeof command.audio !== 'boolean' || typeof command.video !== 'boolean' ||
        (!command.audio && !command.video) || command.lane !== lane.name ||
        command.lane !== (command.audio ? (command.video ? 'audioVideo' : 'audio') : 'video'))
      throw new Error('live_binding_invalid');
    this.binding = Object.freeze({...command}); // 原本地绑定，不允许自由URL/远端地址覆盖。
    this.lane = lane; this.send = send; this.platform = platform; // 可信宿主驱动与固定事实出口。
    this.peer = null; this.transforms = new Map(); // 独立原Peer及每轨编码流驱动。
    this.operations = new Map(); this.generations = new Map(); this.wanted = new Map();
    this.stopped = new Set(); this.closing = false; this.closed = false; // 单轨与整组终态分别保留。
    this.connected = false; this.answerApplied = false; // 网络屏障不能变成输出事实。
    this.video = null; this.source = null; this.frameId = null; this.presenting = false;
    this.callbacks = new Set(); // 原可取消RVFC/rAF回调，不留旧实例作用新节目。
    this.life = new AbortController(); // 原整组异步驱动有限生命周期。
    this.metrics = {visibleFrames: 0, audioActive: false, videoActive: false}; // 只读实际观察，无身份。
  }
  // fact 输出固定本地实际事实；未知异常不反射自由文本或原媒体身份。
  fact(kind, extra = {}) {
    this.send({kind, instance: this.binding.instance, mediaRevision: 1, ...extra});
  }
  // failed 只封原接收器；全组实际释放仍须close独立证明。
  failed() { if (!this.closing) { this.fact('liveFailed'); void this.close(); } }
  // setGain 有界统一speaker本地接口；增益在Worklet之前生效，零增益不能产生非静音active。
  setGain(value) {
    if (this.closing || this.closed || !Number.isFinite(value) || value < 0 || value > 1 ||
        !this.lane.gain?.gain) return false;
    this.lane.gain.gain.value = value;
    return true;
  }
  // initialize 固定recvonly/Opus或H264；完成ICE后报真实offer，不回退VP8/AAC或隐开采集。
  async initialize() {
    const p = this.platform;
    try {
      this.peer = new p.RTCPeerConnection({iceServers: []});
      this.peer.onconnectionstatechange = () => {
        if (this.closing) return;
        if (this.peer.connectionState === 'connected' && this.answerApplied && !this.connected) {
          this.connected = true; this.fact('liveConnected');
        } else if (['failed', 'closed', 'disconnected'].includes(this.peer.connectionState)) this.failed();
      };
      for (const kind of ['audio', 'video']) {
        if (this.closing || this.life.signal.aborted) throw new Error('live_cancelled');
        if (!this.binding[kind]) continue;
        const transceiver = this.peer.addTransceiver(kind, {direction: 'recvonly'});
        const codecs = p.RTCRtpReceiver.getCapabilities(kind)?.codecs?.filter(codec =>
          codec.mimeType.toLowerCase() === (kind === 'audio' ? 'audio/opus' : 'video/h264'));
        if (!codecs?.length || typeof transceiver.setCodecPreferences !== 'function')
          throw new Error('live_codec_unavailable');
        transceiver.setCodecPreferences(codecs);
        const worker = new p.Worker('/live-transform-worker.js');
        const pair = new p.MessageChannel();
        const channel = new UrLiveMessages(pair.port1);
        const ready = channel.expect(null, value => liveExact(value, ['kind']) &&
          value.kind === 'liveTransformReady', this.life.signal);
        channel.subscribe(value => { if (value?.kind === 'liveTransformFailed') this.failed(); });
        transceiver.receiver.transform = new p.RTCRtpScriptTransform(worker,
          {kind, port: pair.port2}, [pair.port2]);
        this.transforms.set(kind, {worker, channel, receiver: transceiver.receiver});
        await ready;
        if (this.closing || this.life.signal.aborted) throw new Error('live_cancelled');
        // 原末端已确认的当前票据必须显式同步新接收器；默认false不允许预先积压解码节目。
        if (this.lane.ticket) await channel.expect({kind: 'liveTransformMask',
          ticket: this.lane.ticket, enabled: this.lane.enabled}, value =>
          value.kind === 'liveTransformMasked' && value.ticket === this.lane.ticket &&
          value.enabled === this.lane.enabled, this.life.signal);
      }
      if (this.closing || this.life.signal.aborted) throw new Error('live_cancelled');
      if (this.binding.video) {
        this.video = p.document.createElement('video');
        this.video.muted = true; this.video.playsInline = true;
        this.video.style.display = 'none'; // 隐藏decoder不是实际可见呈现。
        this.video.srcObject = new p.MediaStream([this.transforms.get('video').receiver.track]);
        p.document.body.appendChild(this.video);
      }
      const gathering = new Promise(resolve => {
        const observe = () => { if (this.peer.iceGatheringState === 'complete') resolve(); };
        this.peer.addEventListener('icegatheringstatechange', observe);
        this.callbacks.add(() => this.peer.removeEventListener('icegatheringstatechange', observe));
        observe();
      });
      await this.peer.setLocalDescription(await this.peer.createOffer());
      await liveWait(gathering, 5000, this.life.signal);
      const sdp = this.peer.localDescription?.sdp;
      if (typeof sdp !== 'string' || sdp.length > 49152 || !sdp.startsWith('v=0\r\n'))
        throw new Error('live_offer_invalid');
      this.fact('liveOffer', {sdp});
      return true;
    } catch { this.failed(); return false; }
  }
  // answer 只接受该原实例m1的一次完整answer，不据此active或打开输出。
  async answer(value) {
    if (this.closing || !this.peer || this.answerApplied ||
        !liveExact(value, ['kind', 'instance', 'mediaRevision', 'sdp']) ||
        value.kind !== 'liveAnswer' || value.instance !== this.binding.instance ||
        value.mediaRevision !== 1 || typeof value.sdp !== 'string' ||
        !value.sdp.startsWith('v=0\r\n') || value.sdp.length > 49152 || value.sdp.includes('\0')) return false;
    try {
      await this.peer.setRemoteDescription({type: 'answer', sdp: value.sdp});
      if (this.closing) return false;
      this.answerApplied = true;
      this.peer.onconnectionstatechange();
      return true;
    } catch { this.failed(); return false; }
  }
  // track 完整原操作只受理一次；失败/实际完成由异步驱动回报，不拿postMessage作为ACK。
  track(value) {
    const kind = value?.trackId === 'broadcast-out' ? 'audio' :
      value?.trackId === 'broadcast-video-out' ? 'video' : null;
    const names = ['kind', 'instance', 'mediaRevision', 'trackId', 'operation', 'generation'];
    if (value?.operation === 'resume') names.push('gateTicket');
    if (this.closing || !this.connected || !kind || !this.binding[kind] || !liveExact(value, names) ||
        value.kind !== 'liveTrackControl' || value.instance !== this.binding.instance ||
        value.mediaRevision !== 1 || !['pause', 'resume', 'stop'].includes(value.operation) ||
        !Number.isSafeInteger(value.generation) || value.generation <= (this.generations.get(kind) || 0) ||
        this.operations.has(kind) || this.stopped.has(kind) ||
        (value.operation === 'resume' && (!liveTicket(value.gateTicket) ||
          value.gateTicket !== this.lane.ticket || !this.lane.enabled))) return false;
    const operation = {command: Object.freeze({...value}), abort: new AbortController(), done: false};
    this.generations.set(kind, value.generation); this.operations.set(kind, operation);
    this.wanted.set(kind, value.operation === 'resume');
    void this.perform(kind, operation);
    return true;
  }
  // finish 保存已合法形成的原票据事实；之后临时focus关闭不得抹除过去事实或重开当前意图。
  finish(kind, operation, state, failure) {
    if (this.closing || operation.done || this.operations.get(kind) !== operation) return;
    operation.done = true; this.operations.delete(kind);
    const original = operation.command;
    const body = {trackId: original.trackId, generation: original.generation,
      code: failure ? (failure === 'outputBusy' ? 409 : 503) : 200};
    if (failure) body.failure = failure;
    else { body.state = state; if (state === 'active') body.gateTicket = original.gateTicket; }
    this.fact('liveTrackFact', body);
  }
  // nextFrame 在原新IDR放行前登记RVFC；只用实际RTP时间匹配，旧隐藏解码帧不是可见节目事实。
  nextFrame(operation, freshPromise) {
    const p = this.platform, video = this.video;
    return new Promise((resolve, reject) => {
      let fresh = null, canceled = false;
      const cancel = () => { canceled = true; if (this.frameId !== null)
        video.cancelVideoFrameCallback(this.frameId); this.frameId = null;
        operation.abort.signal.removeEventListener('abort', cancel); reject(new Error('live_cancelled')); };
      operation.abort.signal.addEventListener('abort', cancel, {once: true});
      const callback = async (_now, metadata) => {
        this.frameId = null;
        if (canceled || this.closing) return;
        const stamp = metadata?.rtpTimestamp;
        const matches = fresh && Number.isInteger(stamp) && stamp >= 0 && stamp <= 4294967295 &&
          ((stamp - fresh.rtpTimestamp) >>> 0) < 2147483648;
        if (!matches || !this.lane.enabled || this.lane.ticket !== operation.command.gateTicket ||
            !this.wanted.get('video')) { this.frameId = video.requestVideoFrameCallback(callback); return; }
        try {
          await this.present(video, operation.abort.signal);
          if (canceled || this.closing || !this.lane.enabled ||
              this.lane.ticket !== operation.command.gateTicket) return;
          operation.abort.signal.removeEventListener('abort', cancel);
          this.metrics.videoActive = true; resolve();
        } catch { cancel(); }
      };
      this.frameId = video.requestVideoFrameCallback(callback);
      Promise.resolve(freshPromise).then(value => { fresh = value; }, () => cancel());
      // muted decoder只在合法resume后运行；play受理/隐藏decoder均不报告active。
      void video.play().catch(() => cancel());
    });
  }
  // present 真实绘制到可见末端并经过呈现帧；隐藏/零尺寸canvas明确失败，不补造视频帧。
  async present(video, signal) {
    const canvas = this.lane.canvas, p = this.platform;
    if (!video.videoWidth || !video.videoHeight || !canvas) throw new Error('live_no_frame');
    canvas.width = video.videoWidth; canvas.height = video.videoHeight;
    canvas.style.display = 'block';
    canvas.getContext('2d').drawImage(video, 0, 0);
    await this.lane.presented(signal);
    const style = p.getComputedStyle(canvas), rect = canvas.getBoundingClientRect();
    if (style.display === 'none' || style.visibility !== 'visible' || Number(style.opacity) <= 0 ||
        rect.width <= 0 || rect.height <= 0) throw new Error('live_not_visible');
    for (let parent = canvas.parentElement; parent; parent = parent.parentElement) {
      const ancestor = p.getComputedStyle(parent);
      if (ancestor.display === 'none' || ancestor.visibility !== 'visible' || Number(ancestor.opacity) <= 0)
        throw new Error('live_not_visible'); // 子canvas自身可见不足以证明被隐藏父容器中的实际呈现。
    }
    ++this.metrics.visibleFrames;
  }
  // keepPresenting 连续可见帧依然核当前末端/意图；临时关闭清队列，不自动恢复呈现。
  keepPresenting() {
    if (this.closing || !this.wanted.get('video') || !this.lane.enabled || this.stopped.has('video')) return;
    this.presenting = true;
    this.frameId = this.video.requestVideoFrameCallback(async () => {
      this.frameId = null;
      if (this.closing || !this.wanted.get('video') || !this.lane.enabled) return;
      try { await this.present(this.video, this.life.signal); this.keepPresenting(); }
      catch { this.failed(); }
    });
  }
  // perform 两轨独立等待实际事实；audio不等待video IDR，stop不暗停另一轨。
  async perform(kind, operation) {
    const original = operation.command, signal = operation.abort.signal;
    const transform = this.transforms.get(kind), channel = transform.channel;
    try {
      const fresh = original.operation === 'resume' ? channel.expect(null, value =>
        value.kind === 'liveEncodedFresh' && value.generation === original.generation &&
        value.gateTicket === original.gateTicket, signal) : null;
      fresh?.catch(() => {}); // 后续实际等待仍保留失败；避免并行另一屏障期间出现未收录拒绝。
      // 视频在控制放行前登记首帧等待，避免已实际呈现帧先于异步新编码通知被丢失。
      const visible = kind === 'video' && fresh ? this.nextFrame(operation, fresh) : null;
      visible?.catch(() => {});
      await channel.expect({kind: 'liveTransformTrack', generation: original.generation,
        operation: original.operation}, value => value.kind === 'liveTransformTrackAccepted' &&
        value.generation === original.generation, signal);
      if (kind === 'audio') {
        if (fresh) {
          await fresh;
          if (signal.aborted) return;
          if (this.source) this.source.disconnect();
          this.source = this.platform.liveContext.createMediaStreamSource(new this.platform.MediaStream([
            transform.receiver.track]));
          this.source.channelCount = 1; this.source.channelCountMode = 'explicit';
          this.source.connect(this.lane.gain); // 增益在实际非静音判据之前，gain0不能冒充active。
        }
        const fact = await this.lane.audio.expect({kind: 'liveAudioTrack',
          generation: original.generation, operation: original.operation}, value =>
          ['liveAudioTrackApplied', 'liveAudioTrackFailed'].includes(value.kind) &&
          value.generation === original.generation, signal);
        if (fact.kind === 'liveAudioTrackFailed') {
          await operation.maskBarrier; // AV焦点取消须完整原组实际清屏/静音，不能先回单audio失败。
          this.finish(kind, operation, '', 'outputBusy'); return;
        }
        const expected = original.operation === 'resume' ? 'active' :
          original.operation === 'pause' ? 'paused' : 'stopped';
        if (fact.state !== expected || (fact.state === 'active' &&
            fact.gateTicket !== original.gateTicket)) throw new Error('live_audio_fact_invalid');
        this.metrics.audioActive = fact.state === 'active';
      } else if (visible) await liveWait(visible, 5000, signal);
      else await this.clearVideo(signal);
      if (original.operation === 'stop') {
        await this.stopTransform(kind, signal);
        this.stopped.add(kind);
      }
      const state = original.operation === 'resume' ? 'active' :
        original.operation === 'pause' ? 'paused' : 'stopped';
      this.finish(kind, operation, state);
      if (kind === 'video' && state === 'active') this.keepPresenting();
    } catch {
      if (!operation.done && !this.closing) {
        this.wanted.set(kind, false);
        this.finish(kind, operation, '', 'mediaFailed');
      }
      operation.abort.abort(); // 超时也必须精确取消仍等待的原RVFC/端口订阅。
    }
  }
  // clearVideo 实际移除末端画面/原回调后等待呈现帧，不报告解码器存在为已停流。
  async clearVideo(signal) {
    if (this.frameId !== null && this.video) this.video.cancelVideoFrameCallback(this.frameId);
    this.frameId = null; this.presenting = false;
    this.video?.pause();
    const canvas = this.lane.canvas;
    if (canvas) { canvas.getContext('2d').clearRect(0, 0, canvas.width, canvas.height);
      canvas.style.display = 'none'; }
    this.metrics.videoActive = false;
    await this.lane.presented(signal);
  }
  // maskChanged 临时focus只关闭末端/旧意图；原未完成resume在实际关闭后失败，恢复不自动播放。
  async maskChanged(enabled, ticket, settled) {
    if (this.closing) return;
    const canceled = [];
    if (!enabled) {
      for (const kind of this.transforms.keys()) {
        this.wanted.set(kind, false);
        const operation = this.operations.get(kind);
        if (operation?.command.operation === 'resume' && !operation.done) {
          operation.maskBarrier = settled;
          canceled.push([kind, operation]); // 原audio actual消息FIFO先于gate实际关闭，不能提前抹历史。
        }
      }
    }
    try {
      await Promise.all([...this.transforms.values()].map(({channel}) => channel.expect({
        kind: 'liveTransformMask', ticket, enabled}, value => value.kind === 'liveTransformMasked' &&
        value.ticket === ticket && value.enabled === enabled, this.life.signal)));
      if (!enabled) await this.clearVideo(this.life.signal);
      await settled;
      if (this.closing) return;
      if (!enabled) this.metrics.audioActive = false;
      for (const [kind, operation] of canceled) {
        if (this.operations.get(kind) !== operation || operation.done) continue;
        this.finish(kind, operation, '', 'outputBusy'); operation.abort.abort();
      }
    } catch { this.failed(); }
  }
  // stopTransform 先实际终止原编码pipeline，再停止原receiver轨；不误把自然stream结束当新授权。
  async stopTransform(kind, signal) {
    const transform = this.transforms.get(kind);
    if (!transform || transform.closed) return;
    await transform.channel.expect({kind: 'liveTransformClose'}, value =>
      value.kind === 'liveTransformClosed', signal);
    transform.receiver.track.stop();
    if (transform.receiver.track.readyState !== 'ended') throw new Error('live_track_not_stopped');
    transform.worker.terminate(); transform.channel.close(); transform.closed = true;
    if (kind === 'audio' && this.source) { this.source.disconnect(); this.source = null; }
  }
  // close 全组真实退出后才交原五项释放事实；基础lane属于宿主，不把它的常驻误当节目未释放。
  async close() {
    if (this.closing) return this.completion;
    this.closing = true;
    this.life.abort();
    for (const operation of this.operations.values()) { operation.done = true; operation.abort.abort(); }
    this.operations.clear(); this.wanted.clear();
    this.completion = (async () => {
      try {
        const signal = new AbortController().signal;
        // 关闭可取消尚在等待非静音的resume；不以新的stop抢占或伪造原请求完成事实。
        if (this.binding.audio) await this.lane.audio.expect({kind: 'liveAudioClose'}, value =>
          value.kind === 'liveAudioClosed', signal);
        if (this.binding.audio) { this.lane.node?.disconnect(); this.lane.nodeClosed = true; }
        this.metrics.audioActive = false;
        await this.clearVideo(signal);
        await Promise.all([...this.transforms.keys()].map(kind => this.stopTransform(kind, signal)));
        for (const callback of this.callbacks) callback(); this.callbacks.clear();
        if (this.video) { this.video.srcObject = null; this.video.remove(); }
        this.peer?.close();
        if (this.peer && this.peer.connectionState !== 'closed') throw new Error('live_peer_not_closed');
        this.closed = true;
        this.fact('liveStopped', {peerClosed: true, tracksStopped: true, outputsDetached: true,
          callbacksStopped: true, transformsStopped: true});
        return true;
      } catch { return false; } // 原真实释放未知，不能发送成功事实或复用预约。
    })();
    return this.completion;
  }
}
// 浏览器与独立Node契约测试共用同一实现，不引入测试专用协议分支。
globalThis.UrLivePlayer = UrLivePlayer;
globalThis.UrLiveMessages = UrLiveMessages;
globalThis.liveExact = liveExact;
globalThis.liveTicket = liveTicket;
