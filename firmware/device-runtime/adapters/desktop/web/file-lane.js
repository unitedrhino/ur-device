// 文件说明：固定文件输出路径在实际音频块与DOM输出隔离确认后回原票据；关闭用零布局证明，启用仍等真实绘制周期。
class UrFileLane {
  constructor(context, mount, send, lane) {
    this.context = context; // 仅借用宿主上下文，不close/suspend麦克风或AI节点。
    this.mount = mount; // 此路径独占父容器，动态播放器不能绕过父容器的显示隔离。
    this.send = send; // 同源已配对本地事实出口，不发送MQTT或业务授权。
    this.lane = lane; // 固定audio/video/audioVideo，不能随控制请求改换资源组。
    this.audio = lane === 'audio' || lane === 'audioVideo'; // 真实speaker完整组选择。
    this.video = lane === 'video' || lane === 'audioVideo'; // 真实display完整组选择。
    this.node = null; // 所有节目必须接此唯一实际音频输出节点。
    this.pending = null; // 一条原音频线程票据，不因播放器创建或关闭刷新。
    this.ticket = 0n; // 本地uint64票据，和云端会话/播放器水位无关。
    this.busy = false; // 初始化/整组执行互斥，不覆盖正在执行的关闭。
    this.failed = !['audio', 'video', 'audioVideo'].includes(lane); // 配置失效永久拒绝。
    this.closing = false; // 迟到异步初始化不能创建新输出。
    this.cancelPaint = null; // 原绘制等待只由本路径清理。
  }

  // initialize 建立固定默认关闭节点；构造和连接本身不发布首次idle成功。
  async initialize() {
    if (this.failed || this.closing || this.context?.state !== 'running' ||
        !this.mount?.style || typeof this.send !== 'function') return false;
    try {
      await this.context.audioWorklet.addModule('/file-worklet.js');
      if (this.closing) return false;
      this.mount.style.setProperty('display', 'none', 'important');
      this.node = new AudioWorkletNode(this.context, 'ur-file-audio', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2]});
      this.node.port.onmessage = event => this.fact(event.data);
      this.node.connect(this.context.destination);
      return true;
    } catch { this.release(); this.failed = true; return false; }
  }

  // fact 只消费原实际音频块回执；测量及时确认避免背景节目阻塞队列，不伪造播放器事实。
  fact(value) {
    if (this.closing) return;
    if (value?.kind === 'fileAudioMeasured') {
      this.node?.port.postMessage({kind: 'fileAudioAck', ticket: value.ticket});
    } else if (value?.kind === 'fileAudioGated' && this.pending &&
        value.generation === '1' && value.ticket === this.pending.ticket &&
        value.enabled === this.pending.enabled) {
      const pending = this.pending;
      this.pending = null;
      pending.resolve();
    } else { this.failed = true; this.release(); }
  }

  // audioGate 等本路径的实际音频块，不把postMessage受理或空节目等价为已关闭。
  audioGate(ticket, enabled) {
    if (!this.node || this.pending || this.context.state !== 'running')
      return Promise.reject(new Error('unknown'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending = null;
        reject(new Error('timeout'));
      }, 1800);
      this.pending = {ticket, enabled, resolve: () => { clearTimeout(timer); resolve(); },
        reject: () => { clearTimeout(timer); reject(new Error('closed')); }};
      this.node.port.postMessage({kind: 'fileAudioGate', generation: '1', ticket, enabled});
    });
  }

  // paint 按enabled核本路径输出，返回实际完成Promise；关闭取消旧绘制并核零布局，启用仍须1500ms内两个真实绘制周期。
  paint(enabled) {
    return new Promise((resolve, reject) => {
      // 隐藏节点不保证产生rAF；实际零布局已证明没有显示输出，不能据其少帧将可用管线误判为失效。
      if (enabled === false) {
        this.cancelPaint?.();
        if (!this.closing && this.context?.state === 'running' && this.mount?.isConnected === true &&
            getComputedStyle(this.mount).display === 'none' &&
            typeof this.mount.getClientRects === 'function' && this.mount.getClientRects().length === 0)
          resolve();
        else reject(new Error('unknown'));
        return;
      }
      const started = performance.now(), deadline = started + 1500; // 单调原预算，不因timer调度延迟续期。
      let frame = null, timer = null, settled = false, observed = started; // 取消后旧回调不能登记下一帧。
      // inBudget 要求每次观察仍单调且未到截止，非有限/倒退时钟不能造成功。
      const inBudget = () => {
        const now = performance.now();
        if (!Number.isFinite(now) || !Number.isFinite(observed) ||
            !Number.isFinite(deadline) || now < observed || now >= deadline) return false;
        observed = now;
        return true;
      };
      // finish 只结算原等待一次；回执仍受当前生命周期与原绝对截止限制。
      const finish = success => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (frame !== null) cancelAnimationFrame(frame);
        if (this.cancelPaint === cancel) this.cancelPaint = null;
        success && !this.closing && inBudget() ? resolve() : reject(new Error('unknown'));
      };
      const cancel = () => finish(false);
      this.cancelPaint = cancel;
      timer = setTimeout(() => finish(false), 1500);
      frame = requestAnimationFrame(() => {
        if (settled) return;
        frame = null;
        if (this.closing || !inBudget()) return finish(false);
        frame = requestAnimationFrame(() => {
          if (settled) return;
          frame = null;
          finish(getComputedStyle(this.mount).display === (enabled ? 'block' : 'none'));
        });
      });
    });
  }

  // gate 完整路径实际执行后才回原本地票据；首次只允许false，不授权内容或动态租约。
  async gate(value) {
    if (this.failed || this.closing || this.busy || !value ||
        Object.keys(value).length !== 4 || value.kind !== 'fileLaneGate' ||
        value.lane !== this.lane || typeof value.enabled !== 'boolean' ||
        typeof value.gateTicket !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value.gateTicket) ||
        BigInt(value.gateTicket) > 18446744073709551615n ||
        BigInt(value.gateTicket) <= this.ticket || (!this.node && value.enabled)) return false;
    this.ticket = BigInt(value.gateTicket);
    this.busy = true;
    try {
      if (!this.node && !await this.initialize()) return false;
      this.mount.style.setProperty('display', value.enabled && this.video ? 'block' : 'none', 'important');
      await this.audioGate(value.gateTicket, value.enabled && this.audio);
      if (this.video) await this.paint(value.enabled);
      if (this.closing || this.failed || this.context.state !== 'running') return false;
      this.send({kind: 'fileLaneGated', lane: this.lane,
        gateTicket: value.gateTicket, enabled: value.enabled});
      return true;
    } catch { this.failed = true; this.release(); return false; }
    finally { this.busy = false; }
  }

  // destination 仅供可信宿主连接原节目，不默认直通；初始化未知时不提供输出节点。
  destination() { return !this.failed && !this.closing ? this.node : null; }

  // release 只尽力断开本路径；不发送实际成功事实，不销毁宿主上下文或节目资源。
  release() {
    this.closing = true;
    this.pending?.reject();
    this.pending = null;
    this.cancelPaint?.();
    this.node?.disconnect();
    if (this.mount?.style) this.mount.style.setProperty('display', 'none', 'important');
  }
}
globalThis.UrFileLane = UrFileLane;
