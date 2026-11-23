// 文件说明：受控整份MP4历史源；只解码本地固定档案并向原Peer发布，不采集相机/麦克风或播放到扬声器。
// 模块划分：白名单协议、有限档案加载、真实播放事实、独立Peer及全组实际关闭。
'use strict';
class UrRecordSource {
  // options由同一鉴权本地桥提供send/token/mount；不包含MQTT身份或用户授权。
  constructor(options) {
    this.send = options.send; // 固定本地事实出口，不发送自由MQTT命令。
    this.token = options.token; // 本地桥鉴权，仅用于同源有限资产fetch。
    this.mount = options.mount; // 本源独立video容器，不借其它预览元素。
    this.binding = null; // C++已绑定的有限本地执行计划，不能由UI换档案。
    this.media = null; this.blob = null; this.abort = null; // 独立档案资源。
    this.audioContext = null; this.audioSource = null; // 只连接MediaStreamDestination，不接扬声器。
    this.peer = null; this.retiring = new Map(); // 当前及退出Peer，不复用旧SRTP。
    this.generation = '0'; this.mediaRevision = 1; // 两个独立原关联。
    this.state = ''; this.closing = false; this.closed = false; // Actual与全组退出分开。
    this.chain = Promise.resolve(); this.queued = 0; // 最多32条，close立即打断旧异步操作。
    this.waiters = new Set(); this.frameHandle = null; // 本实例有界监听与可取消帧回调。
    this.frames = 0; this.decodedPosition = 0; // 来自requestVideoFrameCallback真实呈现事实。
  }
  // identity只用于本地会话关联，绝不从页面选择设备或平台用户。
  identity(value) {
    const id = (v, max) => typeof v === 'string' && v.length > 0 && v.length <= max && !/[\x00-\x1f\x7f]/.test(v);
    return id(value.sessionId, 64) && id(value.playbackId, 128) && this.binding &&
      value.sessionId === this.binding.sessionId && value.playbackId === this.binding.playbackId;
  }
  // uint/generation分别限制JSON安全整数和本地uint64字符串，禁止数字隐式转换。
  uint(value, positive = false) { return Number.isSafeInteger(value) && value >= (positive ? 1 : 0); }
  validGeneration(value) { return typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value) && BigInt(value) <= 18446744073709551615n; }
  // emit固定增加原本地身份，不传目录路径、错误堆栈或设备凭据。
  emit(kind, facts = {}) { this.send({kind, sessionId: this.binding.sessionId, playbackId: this.binding.playbackId, ...facts}); }
  // probe只读实际容器和RTP能力，不加载或偷偷播放档案。
  probe(value) {
    const mime = value.audio ? 'video/mp4; codecs="avc1.42E01E,opus"' : 'video/mp4; codecs="avc1.42E01E"';
    if (Object.keys(value).length !== 3 || typeof value.audio !== 'boolean' || value.mime !== mime) return false;
    const video = document.createElement('video');
    const codecs = kind => typeof RTCRtpSender === 'undefined' ? [] : RTCRtpSender.getCapabilities(kind)?.codecs || [];
    const supported = !!video.canPlayType(mime) && typeof video.captureStream === 'function' &&
      typeof video.requestVideoFrameCallback === 'function' && codecs('video').some(c => c.mimeType.toLowerCase() === 'video/h264') &&
      (!value.audio || codecs('audio').some(c => c.mimeType.toLowerCase() === 'audio/opus'));
    this.send({kind: 'recordBridgeReady', mime, supported});
    return true;
  }
  // bind只保留C++给的固定同源资产路由；尚未读取、解码或创建Peer。
  bind(value) {
    if (this.binding || this.closing || Object.keys(value).length !== 8 ||
        typeof value.sessionId !== 'string' || !value.sessionId || value.sessionId.length > 64 ||
        typeof value.playbackId !== 'string' || !value.playbackId || value.playbackId.length > 128 ||
        /[\x00-\x1f\x7f]/.test(value.sessionId + value.playbackId) ||
        typeof value.assetRoute !== 'string' || !/^\/record-assets\/[A-Za-z0-9_.-]+$/.test(value.assetRoute) || value.assetRoute.includes('..') ||
        typeof value.audio !== 'boolean' || value.mime !== (value.audio ? 'video/mp4; codecs="avc1.42E01E,opus"' : 'video/mp4; codecs="avc1.42E01E"') ||
        !this.uint(value.durationMs, true) || !this.uint(value.maxBytes, true) || value.maxBytes > 67108864 ||
        typeof this.token !== 'string' || !/^[0-9a-f]{64}$/.test(this.token)) return false;
    this.binding = Object.freeze({...value});
    return true;
  }
  // wait带有限取消监听；关闭／旧Peer退出必须结束原异步协商，不能靠等待超时释放。
  wait(predicate, timeout = 5000, peer = null) {
    return new Promise((resolve, reject) => {
      const deadline = performance.now() + timeout;
      let timer = null, done = false;
      const finish = error => { if (done) return; done = true; clearTimeout(timer); this.waiters.delete(cancel); error ? reject(Error(error)) : resolve(); };
      const cancel = () => finish('closed');
      const check = () => {
        if (this.closing || peer?.closed) return cancel();
        if (this.media?.error) return finish('decodeFailed');
        if (predicate()) return finish();
        if (performance.now() >= deadline) return finish('timeout');
        timer = setTimeout(check, 10);
      };
      this.waiters.add(cancel); check();
    });
  }
  // framesLoop测量真实解码位置；暂停/定位/旧Peer结束会取消原呈现回调。
  framesLoop() {
    if (!this.media || this.closing || this.media.paused || this.frameHandle !== null) return;
    this.frameHandle = this.media.requestVideoFrameCallback((time, metadata) => {
      this.frameHandle = null;
      if (this.closing || this.media.paused) return;
      this.frames++; this.decodedPosition = Math.round(metadata.mediaTime * 1000);
      this.framesLoop();
    });
  }
  // pause切断读取进展并取消原回调，不只把接收页面静音。
  pause() {
    this.media?.pause();
    if (this.frameHandle !== null) this.media?.cancelVideoFrameCallback(this.frameHandle);
    this.frameHandle = null;
  }
  // fact仅来自已确认状态及实际位置；失败ACK不夹带伪造位置。
  fact(state, spontaneous = false, code = 200) {
    const value = {generation: this.generation, mediaRevision: this.mediaRevision, code};
    if (code === 200) Object.assign(value, {state, spontaneous, positionMs: this.decodedPosition,
      durationMs: Math.round(this.media.duration * 1000)});
    this.emit('recordFact', value);
  }
  // archiveFormat只核对有限MP4结构与真实样本格式；HTML解码仍负责实际帧和时长，不信Blob MIME。
  archiveFormat(buffer) {
    const view = new DataView(buffer), size = buffer.byteLength;
    const tag = offset => String.fromCharCode(...new Uint8Array(buffer, offset, 4));
    let budget = 512; // 固定目录首驱动最多两轨；异常盒数量不得制造无界解析。
    const boxes = (start, end) => {
      const result = [];
      while (start < end) {
        if (--budget < 0 || end - start < 8) throw Error('invalidArchive');
        let length = view.getUint32(start), header = 8;
        if (length === 1) {
          if (end - start < 16) throw Error('invalidArchive');
          const wide = view.getBigUint64(start + 8);
          if (wide > BigInt(Number.MAX_SAFE_INTEGER)) throw Error('invalidArchive');
          length = Number(wide); header = 16;
        } else if (!length) length = end - start;
        if (length < header || length > end - start) throw Error('invalidArchive');
        result.push({type: tag(start + 4), payload: start + header, end: start + length});
        start += length;
      }
      return result;
    };
    const only = (values, type) => {
      const matched = values.filter(value => value.type === type);
      if (matched.length !== 1) throw Error('invalidArchive');
      return matched[0];
    };
    const child = (value, type) => only(boxes(value.payload, value.end), type);
    const top = boxes(0, size), brand = only(top, 'ftyp'), movie = only(top, 'moov');
    if (brand.end - brand.payload < 8) throw Error('invalidArchive');
    const tracks = boxes(movie.payload, movie.end).filter(value => value.type === 'trak');
    if (tracks.length !== (this.binding.audio ? 2 : 1)) throw Error('unsupportedArchive');
    let video = 0, audio = 0;
    for (const track of tracks) {
      const mdia = child(track, 'mdia'), handler = child(mdia, 'hdlr');
      if (handler.end - handler.payload < 12) throw Error('invalidArchive');
      const kind = tag(handler.payload + 8), samples = child(child(child(mdia, 'minf'), 'stbl'), 'stsd');
      if (samples.end - samples.payload < 8 || view.getUint32(samples.payload + 4) !== 1) throw Error('unsupportedArchive');
      const entry = only(boxes(samples.payload + 8, samples.end), kind === 'vide' ? 'avc1' : kind === 'soun' ? 'Opus' : 'unsupported');
      if (kind === 'vide') {
        if (++video !== 1 || entry.end - entry.payload < 78) throw Error('unsupportedArchive');
        const config = only(boxes(entry.payload + 78, entry.end), 'avcC');
        if (config.end - config.payload < 7 || view.getUint8(config.payload) !== 1) throw Error('unsupportedArchive');
      } else {
        if (!this.binding.audio || ++audio !== 1 || entry.end - entry.payload < 28) throw Error('unsupportedArchive');
        const config = only(boxes(entry.payload + 28, entry.end), 'dOps');
        if (config.end - config.payload < 11 || view.getUint8(config.payload) !== 0 ||
            ![1, 2].includes(view.getUint8(config.payload + 1))) throw Error('unsupportedArchive');
      }
    }
    if (video !== 1 || audio !== (this.binding.audio ? 1 : 0)) throw Error('unsupportedArchive');
  }
  // load流式限制读取、禁止重定向和跨源；真实解码及duration匹配后才报告loaded。
  async load() {
    this.abort = new AbortController();
    const response = await fetch(this.binding.assetRoute, {headers: {Authorization: `Bearer ${this.token}`}, redirect: 'error', signal: this.abort.signal});
    if (!response.ok || response.redirected || !response.body) throw Error('sourceUnavailable');
    const declared = response.headers.get('content-length');
    if (declared && (!/^[0-9]+$/.test(declared) || Number(declared) > this.binding.maxBytes)) throw Error('tooLarge');
    const reader = response.body.getReader(), chunks = [];
    let size = 0;
    try {
      while (true) {
        const value = await reader.read();
        if (this.closing) throw Error('closed');
        if (value.done) break;
        size += value.value.byteLength;
        if (size > this.binding.maxBytes) throw Error('tooLarge');
        chunks.push(value.value);
      }
    } catch (error) { await reader.cancel(); throw error; }
    finally { reader.releaseLock(); }
    if (!size || this.closing) throw Error('closed');
    const archive = new Blob(chunks, {type: this.binding.mime});
    this.archiveFormat(await archive.arrayBuffer());
    if (this.closing) throw Error('closed');
    this.media = document.createElement('video'); this.media.preload = 'auto'; this.media.playsInline = true;
    this.media.controls = false; this.media.autoplay = false;
    if (!this.media.canPlayType(this.binding.mime) || typeof this.media.captureStream !== 'function') throw Error('unsupported');
    if (this.binding.audio) {
      const Context = globalThis.AudioContext || globalThis.webkitAudioContext;
      if (!Context) throw Error('unsupported');
      this.audioContext = new Context();
      await this.audioContext.resume();
      if (this.audioContext.state !== 'running' || this.closing) throw Error('audioUnavailable');
      this.audioSource = this.audioContext.createMediaElementSource(this.media);
      // 不连接context.destination：HTML档案声音只进入每代次MediaStreamDestination，不进入扬声器。
    } else this.media.muted = true;
    this.mount.appendChild(this.media);
    this.media.addEventListener('ended', () => {
      if (!this.closing && this.state === 'playing') {
        this.pause(); this.decodedPosition = Math.round(this.media.currentTime * 1000); this.state = 'ended'; this.fact('ended', true);
      }
    });
    this.media.addEventListener('error', () => { if (!this.closing) this.fail(); });
    this.blob = URL.createObjectURL(archive);
    this.media.src = this.blob; this.media.load();
    await this.wait(() => this.media.readyState >= 2 && Number.isFinite(this.media.duration));
    if (Math.round(this.media.duration * 1000) !== this.binding.durationMs || !this.media.paused) throw Error('durationMismatch');
    this.decodedPosition = 0; this.state = 'loaded';
  }
  // detach暂停实际发布，等待replaceTrack(null)完成后才能确认paused；不只让接收者静音。
  async detach() {
    const peer = this.peer;
    if (!peer || peer.closed) return;
    await Promise.all(peer.transceivers.map(t => t.sender.replaceTrack(null)));
    for (const track of peer.tracks) track.stop();
    for (const track of peer.stream?.getTracks() || []) track.stop();
    if (peer.audio) { this.audioSource?.disconnect(peer.audio); for (const track of peer.audio.stream.getTracks()) track.stop(); }
    peer.tracks = []; peer.stream = peer.audio = null;
  }
  // attach只在Core当前Established后的合法play中调用；实际档案轨未就绪明确失败，不取硬件源。
  async attach(peer) {
    peer.stream = this.media.captureStream();
    await this.wait(() => peer.stream.getVideoTracks().length === 1, 5000, peer);
    const video = peer.stream.getVideoTracks()[0];
    if (video.readyState !== 'live') throw Error('trackNotReady');
    peer.tracks.push(video);
    for (const track of peer.stream.getAudioTracks()) track.stop();
    if (this.binding.audio) {
      peer.audio = this.audioContext.createMediaStreamDestination(); this.audioSource.connect(peer.audio);
      const tracks = peer.audio.stream.getAudioTracks();
      if (tracks.length !== 1 || tracks[0].readyState !== 'live') throw Error('trackNotReady');
      peer.tracks.push(tracks[0]);
    }
    for (const track of peer.tracks) {
      const transceiver = peer.transceivers.find(t => t.recordKind === track.kind);
      if (!transceiver || peer.closed || this.closing) throw Error('closed');
      await transceiver.sender.replaceTrack(track);
    }
  }
  // play须当前实际Connected；附真实轨后连续帧和读取进展发生才报告playing。
  async play() {
    if (!this.peer || this.peer.closed || this.peer.connection.connectionState !== 'connected') throw Error('mediaNotReady');
    const before = this.media.currentTime;
    await this.media.play(); this.framesLoop();
    if (!this.peer.tracks.length) await this.attach(this.peer);
    const frames = this.frames;
    await this.wait(() => !this.media.paused && this.media.currentTime > before && this.frames >= frames + 2);
    this.state = 'playing';
  }
  // seekFrame 在修改档案位置前注册原呈现事实，暂停时一次性目标帧不能被ready轮询错过。
  // 原代次/关闭/超时都取消本回调；seeking中的旧帧不作为当前目标成功，实际位置只取metadata。
  async seekFrame(positionMs) {
    const media=this.media,generation=this.generation,revision=this.mediaRevision;
    let issued=false,seekComplete=false,handle=null,timer=null,poll=null,finished=false,cancel,seeked;
    const frame=new Promise((resolve,reject)=>{
      const finish=(error,position)=>{
        if(finished)return;finished=true;clearTimeout(timer);clearTimeout(poll);
        media.removeEventListener('seeked',seeked);
        if(handle!==null){media.cancelVideoFrameCallback(handle);if(this.frameHandle===handle)this.frameHandle=null;handle=null;}
        this.waiters.delete(cancel);error?reject(Error(error)):resolve(position);
      };
      cancel=()=>finish('closed');this.waiters.add(cancel);
      // 先监听解码器的原seek完成事件；同一代次在seeked之前的旧呈现回调不能充当目标帧。
      seeked=()=>{if(issued&&!media.seeking&&media.readyState>=2)seekComplete=true;};
      media.addEventListener('seeked',seeked);
      const check=()=>{
        if(finished)return;
        if(this.closing||this.media!==media||this.generation!==generation||this.mediaRevision!==revision)return finish('closed');
        if(media.error)return finish('decodeFailed');
        poll=setTimeout(check,10);
      };
      const observe=()=>{
        handle=media.requestVideoFrameCallback((time,metadata)=>{
          if(this.frameHandle===handle)this.frameHandle=null;handle=null;
          if(finished)return;
          if(this.closing||this.media!==media||this.generation!==generation||this.mediaRevision!==revision)return finish('closed');
          const actual=Math.round(metadata.mediaTime*1000);
          if(!issued||!seekComplete||media.seeking||media.readyState<2||!Number.isSafeInteger(actual)||actual<0||actual>this.binding.durationMs){observe();return;}
          // 任意毫秒定位可落在一个实际视频帧内；不以currentTime或请求目标伪造帧时间，也不猜FPS误差。
          finish(null,actual);
        });
        this.frameHandle=handle;
      };
      timer=setTimeout(()=>finish('timeout'),5000);observe();check();
    });
    // 原seeked、ready和首帧共享同一关闭屏障；失败取消全部本轮监听，避免悬空回调写入下一m。
    try{
      issued=true;media.currentTime=positionMs/1000;
      const actual=await frame;
      if(this.closing||this.media!==media||this.generation!==generation||this.mediaRevision!==revision)throw Error('closed');
      this.decodedPosition=actual;
    }catch(error){cancel();await frame.catch(()=>{});throw error;}
  }
  // control每代次只有一条实际操作；seek不沿旧SRTP改变位置。
  async control(value, restore = false) {
    if (!this.identity(value) || !this.validGeneration(value.generation) || !this.uint(value.mediaRevision, true) ||
        (!restore && BigInt(value.generation) <= BigInt(this.generation)) ||
        (restore && (value.generation !== this.generation || value.mediaRevision !== this.mediaRevision)) ||
        (restore ? Object.keys(value).length !== 6 : Object.keys(value).length !== (value.operation === 'seek' ? 7 : 6)) ||
        (restore ? !['loaded', 'paused', 'playing'].includes(value.operation) : !['load', 'play', 'pause', 'seek', 'stop'].includes(value.operation))) return false;
    if (!restore && value.operation !== 'seek' && value.mediaRevision !== this.mediaRevision) return false;
    if (!restore && value.operation === 'seek' && (value.mediaRevision !== this.mediaRevision + 1 ||
        !this.uint(value.positionMs) || value.positionMs > this.binding.durationMs || !this.media)) return false;
    this.generation = value.generation; const original = this.generation;
    if (value.operation === 'seek') this.mediaRevision = value.mediaRevision;
    try {
      if (value.operation === 'load') { if (this.media) return false; await this.load(); }
      else if (!this.media || ['stopped', 'ended', 'failed'].includes(this.state)) return false;
      else if (value.operation === 'play' || (restore && value.operation === 'playing')) await this.play();
      else if (value.operation === 'seek') {
        this.pause();
        await Promise.all([...this.retiring.values()]);
        if (this.peer && !this.peer.closed) throw Error('oldPeerAlive');
        await this.seekFrame(value.positionMs);
        this.state = 'seeking';
      } else { this.pause(); await this.detach(); await this.wait(() => this.media.paused); this.state = restore ? value.operation : value.operation === 'stop' ? 'stopped' : 'paused'; }
      if (this.closing || original !== this.generation) return false;
      this.fact(this.state, restore);
      return true;
    } catch {
      if (!this.closing && original === this.generation) { this.pause(); this.fact('', false, 503); }
      return false;
    }
  }
  // beginPeer只协商sendonly空sender；Connected不是档案轨就绪，合法play才附实际轨。
  async beginPeer(value) {
    if (!this.identity(value) || Object.keys(value).length !== 4 || value.mediaRevision !== this.mediaRevision ||
        !this.media || this.peer || this.retiring.size || this.closing) return false;
    const peer = {revision: value.mediaRevision, connection: null, stream: null, audio: null, tracks: [], transceivers: [], closed: false, pending: null};
    this.peer = peer;
    peer.pending = (async () => {
      try {
        peer.connection = new RTCPeerConnection();
        for (const kind of this.binding.audio ? ['video', 'audio'] : ['video']) {
          const type = kind === 'video' ? 'video/h264' : 'audio/opus';
          const codecs = (RTCRtpSender.getCapabilities(kind)?.codecs || []).filter(c => c.mimeType.toLowerCase() === type);
          if (!codecs.length) throw Error('unsupported');
          const transceiver = peer.connection.addTransceiver(kind, {direction: 'sendonly'});
          if (typeof transceiver.setCodecPreferences !== 'function') throw Error('unsupported');
          transceiver.setCodecPreferences(codecs);
          transceiver.recordKind = kind; peer.transceivers.push(transceiver);
        }
        peer.connection.ontrack = event => { event.track.stop(); if (!peer.closed) this.emit('recordFailure', {mediaRevision: peer.revision}); };
        peer.connection.onconnectionstatechange = () => {
          if (peer.closed || this.closing) return;
          if (peer.connection.connectionState === 'connected') this.emit('recordConnected', {mediaRevision: peer.revision});
          if (['failed', 'disconnected', 'closed'].includes(peer.connection.connectionState)) this.emit('recordFailure', {mediaRevision: peer.revision});
        };
        const offer = await peer.connection.createOffer();
        if (peer.closed || this.closing) return;
        await peer.connection.setLocalDescription(offer);
        await this.wait(() => peer.connection.iceGatheringState === 'complete', 10000, peer);
        if (!peer.closed && !this.closing) this.emit('recordOffer', {mediaRevision: peer.revision, sdp: peer.connection.localDescription.sdp});
      } catch { if (!peer.closed && !this.closing) this.emit('recordFailure', {mediaRevision: peer.revision}); }
    })();
    await peer.pending;
    return !peer.closed && !this.closing;
  }
  // closePeer同步切断发布，再等待旧协商/回调退出，实际Stopped后才允许下一m。
  async closePeer(revision) {
    if (!this.peer || this.peer.revision !== revision) return false;
    const peer = this.peer;
    if (peer.closed) return this.retiring.get(revision) || true;
    peer.closed = true; this.pause();
    peer.connection?.close();
    for (const track of peer.tracks) track.stop();
    for (const track of peer.stream?.getTracks() || []) track.stop();
    if (peer.audio) { this.audioSource?.disconnect(peer.audio); for (const track of peer.audio.stream.getTracks()) track.stop(); }
    const pending = (async () => {
      await peer.pending;
      if (this.peer === peer) this.peer = null;
      this.retiring.delete(revision);
      if (!this.closing) this.emit('recordPeerStopped', {mediaRevision: revision});
    })();
    this.retiring.set(revision, pending);
    await pending; return true;
  }
  // fail不透传浏览器异常，不伪造资源已退出。
  fail() { if (this.peer) this.emit('recordFailure', {mediaRevision: this.peer.revision}); }
  // close等待原所有协程/Peer/AudioContext真实退出，再发唯一全组事实。
  async close(value) {
    if (!this.identity(value) || Object.keys(value).length !== 4 || value.releaseId !== '1') return false;
    if (this.closed) { this.emit('recordClosed', {releaseId: '1', success: true}); return true; }
    this.closing = true; this.pause(); this.abort?.abort();
    for (const cancel of [...this.waiters]) cancel();
    if (this.peer) await this.closePeer(this.peer.revision);
    await Promise.all([...this.retiring.values()]);
    await this.chain;
    this.audioSource?.disconnect(); await this.audioContext?.close();
    if (this.media) { this.media.removeAttribute('src'); this.media.load(); this.media.remove(); }
    if (this.blob) URL.revokeObjectURL(this.blob);
    this.media = this.blob = this.audioSource = this.audioContext = null;
    this.closed = true; this.emit('recordClosed', {releaseId: '1', success: true});
    return true;
  }
  // incoming仅接C++固定白名单；close/旧Peer关闭立即打断，不等待长seek队列。
  async incoming(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
    if (value.kind === 'recordProbe') return this.probe(value);
    if (value.kind === 'recordBind') return this.bind(value);
    if (value.kind === 'recordClose') return this.close(value);
    if (!this.identity(value) || this.closing || !this.uint(value.mediaRevision, true)) return false;
    if (value.kind === 'recordPeerClose') return Object.keys(value).length === 4 && this.closePeer(value.mediaRevision);
    if (value.kind === 'recordAnswer') {
      if (Object.keys(value).length !== 5 || !this.peer || this.peer.closed || this.peer.revision !== value.mediaRevision ||
          typeof value.sdp !== 'string' || value.sdp.length < 8 || value.sdp.length > 49152 || !value.sdp.startsWith('v=0\r\n') || value.sdp.includes('\0')) return false;
      try { await this.peer.connection.setRemoteDescription({type: 'answer', sdp: value.sdp}); return true; }
      catch { this.fail(); return false; }
    }
    if (!['recordControl', 'recordRestore', 'recordPeerBegin'].includes(value.kind) || this.queued >= 32) return false;
    this.queued++;
    const task = this.chain.then(() => this.closing ? false : value.kind === 'recordPeerBegin' ? this.beginPeer(value) : this.control(value, value.kind === 'recordRestore'));
    this.chain = task.catch(() => false).finally(() => { this.queued--; });
    return task;
  }
}
if (typeof module !== 'undefined') module.exports = {UrRecordSource};
else globalThis.UrRecordSource = UrRecordSource;
