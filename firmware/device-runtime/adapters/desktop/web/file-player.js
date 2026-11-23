// 授权文件媒体组件：借用音频上下文，仅管理自身节目；没有设备身份、麦克风或摄像头采集。
// 模块依次为固定绑定初始化、门控/事实交接、有界下载、媒体控制和独占资源释放。
class UrFilePlayer {
  constructor(context, mount, send, token, destination = context.destination) {
    this.context = context; // 宿主拥有的AI共享音频上下文，组件禁止close/suspend它。
    this.destination = destination; // 可信宿主可提供固定路径节点；旧四参数调用仍接原输出。
    this.mount = mount; // 明确授予本节目的视频容器，不复用摄像头预览元素。
    this.send = send; // 已鉴权本地桥的事实出口，不输出自由异常/授权URL。
    this.token = token; // 仅同源测试资产fetch使用，不能发送到远端内容源。
    this.binding = null; // 独立实例固定授权信息，不允许复用对象切换节目。
    this.media = null; // 本组件拥有的HTMLMediaElement。
    this.source = null; // 本组件独占MediaElementSource。
    this.worklet = null; // 独立输出门控，不修改AI音频节点。
    this.generation = '0'; // 本地桥uint64字符串，避免JS浮点丢失关联。
    this.ticket = 0n; // 本组件门控票据，与MQTT消息序号无关。
    this.gates = new Map(); // 有界在途门控确认。
    this.abort = null; // 唯一下载的取消控制器。
    this.blob = null; // 本组件下载产生的ObjectURL。
    this.state = ''; // 最近实际媒体状态，不以accepted代替playing。
    this.busy = false; // 只允许一条控制在执行；close可随时中断。
    this.closing = false; // 同步隔离所有迟到异步回调。
    this.audioFailed = false; // 音频线程已确认切断自身输出。
    this.waiters = new Set(); // 有界异步条件，关闭必须取消并清理监听。
    this.frames = 0; // 当前操作实际视频呈现计数。
    this.samples = 0; // 当前操作音频线程实际采样计数。
    this.energy = 0; // 当前操作实测平方和，不能用预估时长造证据。
    this.frameHandle = null; // 可取消的真实视频帧回调。
    this.outputEnabled = true; // 独立路径门控只增加限制，不是资源/内容授权。
    this.outputBusy = false; // 与节目控制互斥，close仍可中断。
    this.outputTicket = 0n; // 本地整组票据，不与播放generation或音频线程票据混用。
    this.outputGateFailure = ''; // 固定本地诊断枚举，不发送协议、原异常或授权信息。
  }

  // initialize 只初始化节点，宿主须先用用户手势使音频上下文运行；不偷偷申请媒体权限。
  async initialize(value) {
    const id = v => typeof v === 'string' && v.length > 0 && v.length <= 128 && !/[\x00-\x1f\x7f]/.test(v);
    if (this.binding || !value || Object.keys(value).length !== 7 || value.kind !== 'fileBind' ||
        !id(value.sessionId) || !id(value.playbackId) || typeof value.audio !== 'boolean' ||
        !['audio/wav', 'audio/mpeg', 'video/mp4', 'video/webm'].includes(value.mime) ||
        (!value.audio && value.mime.startsWith('audio/')) ||
        !Number.isInteger(value.maxBytes) || value.maxBytes < 1 || value.maxBytes > 67108864 ||
        typeof value.origin !== 'string' || !/^[0-9a-f]{64}$/.test(this.token) ||
        this.context?.state !== 'running') return false;
    if (value.origin) {
      try {
        const url = new URL(value.origin);
        if (url.protocol !== 'https:' || url.origin !== value.origin || url.username || url.password)
          return false;
      } catch { return false; }
    }
    this.binding = Object.freeze({...value});
    try {
      await this.context.audioWorklet.addModule('/file-worklet.js');
      if (this.closing) return false;
      const video = value.mime.startsWith('video/');
      this.media = document.createElement(video ? 'video' : 'audio');
      this.media.preload = 'auto';
      this.media.playsInline = true;
      this.media.controls = false;
      this.media.hidden = true;
      this.media.muted = !value.audio;
      if (video && typeof this.media.requestVideoFrameCallback !== 'function') throw new Error('unsupported');
      this.mount.appendChild(this.media);
      this.source = this.context.createMediaElementSource(this.media);
      this.worklet = new AudioWorkletNode(this.context, 'ur-file-audio', {numberOfInputs: 1,
        numberOfOutputs: 1, outputChannelCount: [2]});
      this.worklet.port.onmessage = event => this.audio(event.data);
      this.source.connect(this.worklet);
      this.worklet.connect(this.destination);
      this.media.addEventListener('ended', () => this.naturalEnd());
      this.media.addEventListener('error', () => this.mediaFailure());
      this.send({kind: 'fileReady', sessionId: value.sessionId, playbackId: value.playbackId, success: true});
      return true;
    } catch {
      this.release();
      if (!this.closing) this.send({kind: 'fileReady', sessionId: value.sessionId,
        playbackId: value.playbackId, success: false});
      return false;
    }
  }

  // audio 测量来自音频线程；即使旧代次也确认消费，避免旧事实积压阻塞新节目。
  audio(value) {
    if (value?.kind === 'fileAudioFailed') {
      this.audioFailed = true;
      for (const gate of this.gates.values()) gate.reject(new Error('audioFailed'));
      this.gates.clear();
      this.mediaFailure();
    } else if (value?.kind === 'fileAudioGated' || value?.kind === 'fileAudioClosed') {
      const gate = this.gates.get(value.ticket);
      if (!gate) return;
      this.gates.delete(value.ticket);
      if (value.generation === gate.generation && value.enabled === gate.enabled &&
          (value.kind === 'fileAudioClosed') === gate.release) gate.resolve();
      else gate.reject(new Error('gateMismatch'));
    } else if (value?.kind === 'fileAudioMeasured') {
      this.worklet?.port.postMessage({kind: 'fileAudioAck', ticket: value.ticket});
      if (value.generation !== this.generation || this.closing) return;
      this.samples = value.samples;
      this.energy = value.energy;
    }
  }

  // wait 使用有限监听/计时器，关闭或超时清理；不依赖未结束的Promise撑住实例。
  wait(predicate, timeout = 5000) {
    return new Promise((resolve, reject) => {
      const deadline = performance.now() + timeout;
      let timer;
      const finish = error => {
        clearTimeout(timer);
        this.waiters.delete(cancel);
        error ? reject(error) : resolve();
      };
      const cancel = () => finish(new Error('closed'));
      const check = () => {
        if (this.closing) return cancel();
        if (this.media?.error || this.audioFailed) return finish(new Error('decodeFailed'));
        if (predicate()) return finish();
        if (performance.now() >= deadline) return finish(new Error('timeout'));
        timer = setTimeout(check, 10);
      };
      if (this.waiters.size >= 8) return reject(new Error('capacity'));
      this.waiters.add(cancel);
      check();
    });
  }

  // gate 等音频线程实际处理块；运行上下文不能凭postMessage就认定静音。
  gate(enabled, generation = this.generation, release = false) {
    if (!release) enabled = enabled && this.outputEnabled;
    // 上下文暂停/节点丢失仅代表无法观测；恢复后旧节点仍可能发声，不能补造静音或释放事实。
    if (this.audioFailed || !this.worklet || this.context.state !== 'running')
      return Promise.reject(new Error('gateUnknown'));
    if (this.gates.size >= 1 || this.ticket === 18446744073709551615n)
      return Promise.reject(new Error('gateBusy'));
    const ticket = String(++this.ticket);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.gates.delete(ticket); reject(new Error('gateTimeout')); }, 2000);
      this.gates.set(ticket, {generation, enabled, release,
        resolve: () => { clearTimeout(timer); resolve(); },
        reject: error => { clearTimeout(timer); reject(error); }});
      this.worklet.port.postMessage({kind: release ? 'fileAudioClose' : 'fileAudioGate', generation, enabled, ticket});
    });
  }

  // paint 在1500ms内等两个渲染调度周期；实际视频解码证据仍独立由requestVideoFrameCallback提供。
  paint() {
    return new Promise((resolve, reject) => {
      const started = performance.now(), deadline = started + 1500; // 单调原截止，不依赖timer及时调度。
      let frame = null, timer = null, settled = false, observed = started; // 原等待只结算一次，不吞新操作。
      // inBudget 拒绝无效或倒退时钟，迟到渲染回调不能清掉滞后timer后补报成功。
      const inBudget = () => {
        const now = performance.now();
        if (!Number.isFinite(now) || !Number.isFinite(observed) ||
            !Number.isFinite(deadline) || now < observed || now >= deadline) return false;
        observed = now;
        return true;
      };
      // finish 只清理本等待的句柄/取消器，保留其他代次的资源与等待。
      const finish = error => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (frame !== null) cancelAnimationFrame(frame);
        this.waiters.delete(cancel);
        if (!error && (this.closing || !inBudget()))
          error = new Error(this.closing ? 'closed' : 'paintTimeout');
        error ? reject(error) : resolve();
      };
      const cancel = () => finish(new Error('closed'));
      if (this.waiters.size >= 8) return reject(new Error('capacity'));
      this.waiters.add(cancel);
      timer = setTimeout(() => finish(new Error('paintTimeout')), 1500);
      frame = requestAnimationFrame(() => {
        if (settled) return;
        frame = null;
        if (this.closing) return cancel();
        if (!inBudget()) return finish(new Error('paintTimeout'));
        frame = requestAnimationFrame(() => {
          if (settled) return;
          frame = null;
          finish();
        });
      });
    });
  }

  // outputGate 仅作用于本文件完整路径，音频线程及DOM显示栅栏确认后回原票据。
  async outputGate(value) {
    if (!this.binding || this.closing || this.busy || this.outputBusy || !value ||
        Object.keys(value).length !== 5 || value.kind !== 'fileOutputGate' ||
        value.sessionId !== this.binding.sessionId || value.playbackId !== this.binding.playbackId ||
        typeof value.enabled !== 'boolean' || typeof value.gateTicket !== 'string' ||
        !/^[1-9][0-9]{0,19}$/.test(value.gateTicket) ||
        String(BigInt(value.gateTicket)) !== value.gateTicket ||
        BigInt(value.gateTicket) > 18446744073709551615n ||
        BigInt(value.gateTicket) <= this.outputTicket) return false;
    this.outputTicket = BigInt(value.gateTicket);
    this.outputBusy = true;
    this.outputEnabled = value.enabled;
    this.outputGateFailure = '';
    let phase = 'audio'; // 仅记录实际确认失败所在路径，不从异常文本推断原因。
    try {
      if (this.frameHandle !== null) this.media.cancelVideoFrameCallback(this.frameHandle);
      this.frameHandle = null;
      const playing = this.state === 'playing' && !this.media.paused && !this.media.ended;
      this.media.hidden = !value.enabled || !playing || !this.binding.mime.startsWith('video/');
      await this.gate(value.enabled && playing && this.binding.audio);
      // 纯音频没有显示输出，不能因后台绘制节流失败；视频仍须实际绘制隔离确认。
      if (this.binding.mime.startsWith('video/')) {
        phase = 'paint';
        await this.paint();
      }
      if (this.closing) { this.outputGateFailure = 'closed'; return false; }
      if (value.enabled && playing) this.framesLoop(this.generation);
      this.send({kind: 'fileOutputGated', sessionId: this.binding.sessionId,
        playbackId: this.binding.playbackId, gateTicket: value.gateTicket, enabled: value.enabled});
      return true;
    } catch {
      this.outputGateFailure = this.closing ? 'closed' : this.audioFailed ? 'audioThread' : phase;
      this.mediaFailure(); // 未确认仍未知，核心固定截止后关闭，不能假报静音/释放。
      return false;
    } finally {
      this.outputBusy = false;
      if (!this.closing && this.media?.ended && this.state === 'playing') this.naturalEnd();
    }
  }

  // download 仅固定授权源，不跟随重定向；逐块检查真实大小，Bearer仅送回环同源资产。
  async download(value) {
    const local = !this.binding.origin;
    const fixed = {'audio/wav': '/media/file.wav', 'audio/mpeg': '/media/file.mp3',
      'video/mp4': '/media/file.mp4', 'video/webm': '/media/file.webm'};
    if (typeof value.url !== 'string' || value.url.length > 4096 || /[\x00-\x20\x7f\\#]/.test(value.url) ||
        !Number.isInteger(value.validForMs) || value.validForMs < 1 || value.validForMs > 10000 ||
        (local ? value.url !== fixed[this.binding.mime] :
          new URL(value.url).origin !== this.binding.origin || !value.url.startsWith(this.binding.origin + '/')))
      throw new Error('source');
    this.abort = new AbortController();
    const deadline = performance.now() + value.validForMs; // 实际读循环也检查期限，不能只依赖timer调度。
    const timeout = setTimeout(() => this.abort?.abort(), value.validForMs);
    let reader;
    try {
      const response = await fetch(value.url, {redirect: 'error', credentials: 'omit',
        signal: this.abort.signal, headers: local ? {Authorization: 'Bearer ' + this.token} : {}});
      const length = response.headers.get('Content-Length');
      if (!response.ok || response.redirected || response.headers.get('Content-Type')?.split(';')[0].trim() !== this.binding.mime ||
          (length !== null && (!/^[0-9]+$/.test(length) || !Number.isSafeInteger(Number(length)) ||
            Number(length) > this.binding.maxBytes)) || !response.body) throw new Error('content');
      reader = response.body.getReader();
      const chunks = [];
      let bytes = 0;
      let blocks = 0; // 同时限制对象数量，避免大量微小/空流块耗尽内存而字节数仍很小。
      while (true) {
        const {done, value: chunk} = await reader.read();
        if (this.closing) throw new Error('closed');
        if (done) break;
        if (++blocks > 8192 || performance.now() >= deadline || !(chunk instanceof Uint8Array) ||
            bytes + chunk.byteLength > this.binding.maxBytes)
          throw new Error('size');
        bytes += chunk.byteLength;
        chunks.push(chunk);
      }
      if (bytes === 0) throw new Error('empty');
      return new Blob(chunks, {type: this.binding.mime});
    } catch (error) {
      this.abort.abort();
      if (reader) await reader.cancel().catch(() => {});
      throw error; // 仅组件内部，输出固定状态而不是异常/地址正文。
    } finally {
      clearTimeout(timeout);
      reader?.releaseLock();
      this.abort = null;
    }
  }

  // framesLoop 使用真实呈现回调；隐藏/暂停或旧代次不能拼成新操作的呈现证据。
  framesLoop(generation) {
    if (!this.outputEnabled || !this.binding.mime.startsWith('video/')) return;
    this.frameHandle = this.media.requestVideoFrameCallback(() => {
      this.frameHandle = null;
      if (this.closing || !this.outputEnabled || generation !== this.generation) return;
      if (!this.media.hidden && !this.media.paused && !this.media.ended) ++this.frames;
      this.framesLoop(generation);
    });
  }

  // fact 使用实际媒体数据，不回显目标位置；成功应答和自然事件由共享C++继续约束。
  fact(state, spontaneous = false, code = 200) {
    const event = {kind: 'fileFact', sessionId: this.binding.sessionId, playbackId: this.binding.playbackId,
      generation: this.generation, spontaneous, code};
    if (code === 200) {
      event.state = state;
      if (state !== 'failed') Object.assign(event, {positionMs: Math.round(this.media.currentTime * 1000),
        durationMs: Math.round(this.media.duration * 1000), rate: this.media.playbackRate,
        audioSamples: this.samples, videoFrames: this.frames});
    }
    this.send(event);
  }

  // control 输入来自C++固定命令，接受一条异步操作；播放必须等待采样/时间推进及视频呈现。
  async control(value) {
    if (!this.binding || this.closing || value?.kind !== 'fileControl' ||
        value.sessionId !== this.binding.sessionId || value.playbackId !== this.binding.playbackId ||
        typeof value.generation !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value.generation) ||
        BigInt(value.generation) > 18446744073709551615n || BigInt(value.generation) <= BigInt(this.generation) ||
        this.busy || this.outputBusy || (!this.outputEnabled && !['load', 'pause', 'stop'].includes(value.operation))) return false;
    const operation = value.operation;
    const expected = operation === 'load' ? 7 : ['seek', 'setRate'].includes(operation) ? 6 : 5;
    if (Object.keys(value).length !== expected || !['load', 'play', 'pause', 'stop', 'seek', 'setRate'].includes(operation))
      return false;
    this.generation = value.generation;
    const operationGeneration = this.generation; // 长seek可被stop打断，旧协程不能改变新操作状态。
    this.samples = this.energy = this.frames = 0;
    this.busy = true;
    if (this.frameHandle !== null) this.media.cancelVideoFrameCallback(this.frameHandle);
    this.frameHandle = null;
    try {
      if (operation === 'load') {
        if (this.state) throw new Error('alreadyLoaded');
        const blob = await this.download(value);
        if (this.closing) return false;
        this.blob = URL.createObjectURL(blob);
        this.media.src = this.blob;
        this.media.load();
        await this.wait(() => this.media.readyState >= 2 && Number.isFinite(this.media.duration) && this.media.duration > 0);
        this.state = 'loaded';
      } else if (!this.state || ['stopped', 'ended', 'failed'].includes(this.state)) {
        throw new Error('terminal');
      } else if (operation === 'play') {
        const before = this.media.currentTime;
        this.media.hidden = !this.binding.mime.startsWith('video/');
        this.framesLoop(this.generation);
        await this.media.play();
        if (this.closing) return false;
        await this.gate(this.binding.audio);
        await this.wait(() => !this.media.paused && !this.media.ended && this.media.currentTime > before + 0.001 &&
          (!this.binding.audio || this.samples >= 256) && (!this.binding.mime.startsWith('video/') || this.frames > 0));
        this.state = 'playing';
      } else if (operation === 'pause' || operation === 'stop') {
        this.media.pause();
        this.media.hidden = true;
        await this.gate(false);
        this.state = operation === 'pause' ? 'paused' : 'stopped';
      } else if (operation === 'setRate') {
        if (![0.5, 1, 1.5, 2].includes(value.rate)) throw new Error('rate');
        this.media.playbackRate = value.rate;
        await this.gate(this.state === 'playing' && this.binding.audio);
        this.framesLoop(this.generation);
        await this.wait(() => this.media.playbackRate === value.rate &&
          (this.state !== 'playing' || ((!this.binding.audio || this.samples >= 256) &&
            (!this.binding.mime.startsWith('video/') || this.frames > 0))));
      } else {
        if (!Number.isSafeInteger(value.positionMs) || value.positionMs < 0) throw new Error('position');
        const resume = this.state;
        this.media.currentTime = Math.min(value.positionMs / 1000, this.media.duration);
        await this.gate(resume === 'playing' && this.binding.audio);
        this.framesLoop(this.generation);
        if (this.media.seeking) {
          this.state = 'seeking';
          this.fact('seeking');
          const generation = this.generation;
          this.busy = false;
          await this.wait(() => generation !== this.generation || (!this.media.seeking &&
            (resume !== 'playing' || ((!this.binding.audio || this.samples >= 256) &&
              (!this.binding.mime.startsWith('video/') || this.frames > 0)))));
          if (generation !== this.generation || this.closing) return false;
          this.state = resume;
          this.fact(resume, true);
          return true;
        }
        if (resume === 'playing') await this.wait(() => (!this.binding.audio || this.samples >= 256) &&
          (!this.binding.mime.startsWith('video/') || this.frames > 0));
      }
      if (!this.closing) this.fact(this.state);
      return true;
    } catch {
      if (!this.closing && operationGeneration === this.generation) {
        this.media?.pause();
        if (this.media) this.media.hidden = true;
        await this.gate(false).catch(() => {});
        this.fact('', false, 502);
      }
      return false;
    } finally {
      if (operationGeneration === this.generation) this.busy = false;
    }
  }

  // naturalEnd 必须先实际切断节目音频；独立终态不能冒用控制应答。
  async naturalEnd() {
    if (this.closing || this.busy || this.outputBusy || this.state !== 'playing') return;
    const generation = this.generation;
    this.media.hidden = true;
    try {
      await this.gate(false);
      if (!this.closing && generation === this.generation) {
        this.state = 'ended';
        this.fact('ended', true);
      }
    } catch { this.mediaFailure(); }
  }

  // mediaFailure 发布脱敏自然失败，之后只释放本组件，绝不重启主AI或节目。
  mediaFailure() {
    if (!this.binding || this.closing || this.state === 'failed') return;
    this.state = 'failed';
    this.media?.pause();
    if (this.media) this.media.hidden = true;
    if (!this.busy && this.generation !== '0') this.fact('failed', true);
  }

  // release 清理本组件对象，借用的AudioContext和宿主媒体始终保持原样。
  release() {
    this.abort?.abort();
    for (const cancel of [...this.waiters]) cancel();
    if (this.frameHandle !== null) this.media?.cancelVideoFrameCallback(this.frameHandle);
    this.frameHandle = null;
    if (this.media) {
      this.media.pause();
      this.media.hidden = true;
      this.media.removeAttribute('src');
      this.media.load();
      this.media.remove();
    }
    this.source?.disconnect();
    for (const gate of this.gates.values()) gate.reject(new Error('released'));
    this.gates.clear();
    // 断连/初始化失败也结束本节点；无需也不能close宿主共享上下文。
    this.worklet?.port.postMessage({kind: 'fileAudioClose', generation: this.generation, enabled: false, ticket: '1'});
    this.worklet?.disconnect();
    if (this.worklet) this.worklet.port.onmessage = null;
    this.source = this.worklet = this.media = null; // 幂等记录可保留状态，但不能保留已释放媒体对象。
    if (this.blob) URL.revokeObjectURL(this.blob);
    this.blob = null;
  }

  // close 同步隔离迟到回调，再确认零输出和独占资源释放；未知门控不能伪报fileClosed。
  async close(value) {
    if (!this.binding || value?.kind !== 'fileClose' || Object.keys(value).length !== 4 ||
        value.sessionId !== this.binding.sessionId || value.playbackId !== this.binding.playbackId ||
        value.generation !== this.generation) return false;
    if (this.closing) return false;
    this.closing = true;
    this.abort?.abort();
    this.media?.pause();
    if (this.media) this.media.hidden = true;
    for (const cancel of [...this.waiters]) cancel();
    try {
      // 先等在途门控实际退出；close不抢占MessagePort造成两条未完成门控。
      if (this.gates.size) await new Promise((resolve, reject) => {
        const deadline = performance.now() + 2100;
        const check = () => this.gates.size === 0 ? resolve() : performance.now() >= deadline ?
          reject(new Error('gateUnknown')) : setTimeout(check, 10);
        check();
      });
      await this.gate(false, this.generation, true);
      this.release();
      this.send({kind: 'fileClosed', sessionId: this.binding.sessionId, playbackId: this.binding.playbackId,
        generation: this.generation});
      return true;
    } catch {
      this.release();
      return false;
    }
  }
}
globalThis.UrFilePlayer = UrFilePlayer;
