// 浏览器音频线程：按显式帧长采集PCM，独立消费播放队列；不持设备身份或云端协议状态。
// audioFailureCauses只允许固定首因标签；诊断不开启时不新增MessagePort事实或采集原命令内容。
const audioFailureCauses = new Set(['configurationInvalid', 'commandInvalid', 'unknownCommand',
  'captureStartInvalid', 'zeroPendingAck', 'captureSampleInvalid', 'captureBacklogFull',
  'playbackStartInvalid', 'playbackFrameInvalid', 'playbackQueueFull', 'playbackGateInvalid',
  'playbackGateConflict', 'playbackGatePending', 'ownershipGateInvalid', 'ownershipTicketConflict',
  'ownershipGatePending', 'ownershipOwnerConflict', 'ownershipRouteCapacity', 'unknown']);
class DeviceAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const config = options.processorOptions || {};
    // rate必须与AudioContext真实采样率一致，不伪装成已经重采样。
    this.valid = [16000, 24000, 48000].includes(sampleRate) &&
      config.sampleRate === sampleRate && [20, 60].includes(config.frameMs);
    this.frameSamples = this.valid ? sampleRate * config.frameMs / 1000 : 0;
    this.inputId = '';
    this.capture = [];
    this.capturePending = 0; // 主线程未确认接纳的帧，防止MessagePort无限积压。
    this.observeCaptureState = config.observeCaptureState === true; // 仅显式桌面观察启停实态，不改变采集规则。
    this.epoch = '';
    this.queue = [];
    this.position = 0;
    this.ticket = 0;
    this.failed = false;
    this.observeFailure = config.observeFailure === true; // 仅本地明确开启的固定旁证，不改变业务故障策略。
    // 原故障开关下才保存最多16个同线程时刻；默认关闭不记录PCM、scope或id。
    this.captureAckDiagnostic = this.observeFailure ? {generated: 0, applied: 0,
      oldInputRejected: 0, ownerRejected: 0, highWater: 0, overflow: false,
      clock: 'valid', lastTime: null, maximumAgeMilliseconds: null, times: []} : null;
    this.failureCause = null; // 首次fail后锁存literal，不持有PCM、关联标识或异常对象。
    this.outputEnabled = true; // 默认语音保持原行为，门控不改变输入或用户音量。
    this.gateTicket = 0n; // 独立64位输出票据，不能在切换回复时重置。
    this.pendingGate = null; // 只有实际非空输出块完成后才确认，不在command中假报。
    this.frameEnergy = 0; // 实际渲染PCM平方和；被门控消费的样本按零记数。
    this.ownership = config.inputOwnership === true; // 仅显式新宿主启用，不改变旧采集路径。
    this.ownershipBoot = config.ownershipBootGeneration;
    this.ownershipRoutes = new Map(); // 最多两条原路由，不申请另一物理源或从当前会话补身份。
    this.ownershipOwner = null; // 原实际启用路由；租约与用户captureStart是两个独立条件。
    this.ownershipTicket = 0n;
    this.ownershipPending = null;
    this.ownershipCaptureStop = null; // 原false门的停采旁证，仅下一实际处理块可发。
    this.ownershipProbe = this.ownership;
    if (this.ownership && !this.ownershipNumber(this.ownershipBoot)) this.valid = false;
    this.port.onmessage = event => this.command(event.data);
    if (!this.valid) this.fail('configurationInvalid');
    else if (this.observeCaptureState)
      this.port.postMessage({kind: 'captureState', id: '', enabled: false});
  }

  // ownershipNumber只接受安全原代次，票据另允许完整uint64十进制，不接受数值隐式转换。
  ownershipNumber(value, maximum = 9007199254740991n) {
    return typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value) && BigInt(value) <= maximum;
  }

  // ownershipScope拒绝控制字符及孤立surrogate；会话UTF8边界与核心严格解码保持一致。
  ownershipScope(value) {
    if (!value || typeof value.sessionId !== 'string' || !value.sessionId ||
        !this.ownershipNumber(value.routeGeneration)) return false;
    let bytes = 0;
    for (const character of value.sessionId) {
      const point = character.codePointAt(0);
      if (point < 32 || point === 127 || (point >= 0xd800 && point <= 0xdfff)) return false;
      bytes += point < 128 ? 1 : point < 2048 ? 2 : point < 65536 ? 3 : 4;
    }
    return bytes <= 64;
  }

  // ownershipCommand只管理原路由门控；false立即清本路由旧帧，实际确认仍等待下一个处理块。
  ownershipCommand(value) {
    if (!this.ownership || Object.keys(value).sort().join(',') !== 'enabled,kind,routeGeneration,sessionId,ticket' ||
        !this.ownershipScope(value) || typeof value.enabled !== 'boolean' ||
        !this.ownershipNumber(value.ticket, 18446744073709551615n)) return this.fail('ownershipGateInvalid');
    const ticket = BigInt(value.ticket);
    if (ticket < this.ownershipTicket) return;
    const key = JSON.stringify([value.sessionId, value.routeGeneration]);
    if (ticket === this.ownershipTicket) {
      const original = this.ownershipPending || this.ownershipLast;
      if (!original || original.sessionId !== value.sessionId ||
          original.routeGeneration !== value.routeGeneration || original.enabled !== value.enabled) this.fail('ownershipTicketConflict');
      return;
    }
    // 保留原pending→owner→capacity短路次序；首因不能归到尚未求值的后置条件。
    if (this.ownershipPending) return this.fail('ownershipGatePending');
    if (value.enabled && this.ownershipOwner && this.ownershipOwner !== key) return this.fail('ownershipOwnerConflict');
    if (!this.ownershipRoutes.has(key) && this.ownershipRoutes.size >= 2) return this.fail('ownershipRouteCapacity');
    this.ownershipRoutes.set(key, value.enabled);
    if (value.enabled) this.ownershipOwner = key;
    else if (this.ownershipOwner === key) {
      this.ownershipOwner = null;
      if (this.observeCaptureState && this.inputId)
        this.ownershipCaptureStop = {kind: 'captureState', id: this.inputId, enabled: false,
          sessionId: value.sessionId, routeGeneration: value.routeGeneration};
      this.inputId = '';
      this.capture = [];
      this.capturePending = 0;
      if (this.captureAckDiagnostic) this.captureAckDiagnostic.times.length = 0;
    }
    this.ownershipTicket = ticket;
    this.ownershipPending = {kind: 'inputOwnershipGated', sessionId: value.sessionId,
      routeGeneration: value.routeGeneration, ticket: value.ticket, enabled: value.enabled};
  }

  // ownsCapture检查原命令绑定，不给旧captureStart贴新会话身份或用UI指标替代所有权。
  ownsCapture(value) {
    return this.ownershipScope(value) && this.ownershipOwner === JSON.stringify([value.sessionId, value.routeGeneration]) &&
      !this.ownershipPending;
  }

  // fail按原次序清双向媒体并只发一次单字段audioFailed；可选cause只能旁观首因，不改变失败。
  fail(cause = 'unknown') {
    if (this.failed) return;
    this.failed = true;
    this.inputId = this.epoch = '';
    this.capture = [];
    this.queue = [];
    this.pendingGate = null;
    this.ownershipPending = null;
    this.ownershipOwner = null;
    if (this.observeFailure) {
      this.failureCause = audioFailureCauses.has(cause) ? cause : 'unknown';
      try { this.port.postMessage({kind: 'audioFailureDiagnostic', cause: this.failureCause}); }
      catch { /* 旁证失败不能阻断、替代或重试原audioFailed。 */ }
    }
    if (this.observeFailure && cause === 'captureBacklogFull') {
      try { this.port.postMessage(this.captureAckSnapshot()); }
      catch { /* 诊断发送失败不阻断或重试原audioFailed。 */ }
    }
    this.port.postMessage({kind: 'audioFailed'});
  }

  // noteCaptureAck 只旁观原生成或ACK判定；异常不影响业务，计数与FIFO均有界。
  noteCaptureAck(field) {
    const d = this.captureAckDiagnostic;
    if (!d) return;
    try {
      if (!['generated', 'applied', 'oldInputRejected', 'ownerRejected'].includes(field)) return;
      if (d[field] >= 1000000) { d.overflow = true; return; }
      ++d[field];
      d.highWater = Math.max(d.highWater, this.capturePending);
      if (field !== 'generated' && field !== 'applied') return;
      const now = typeof currentTime === 'number' ? currentTime * 1000 : NaN;
      const valid = Number.isFinite(now) && now >= 0;
      if (Number.isFinite(now) && now < 0) d.clock = 'invalid';
      if (!valid && d.clock !== 'invalid') d.clock = 'unavailable';
      if (valid && d.lastTime !== null && now < d.lastTime) d.clock = 'invalid';
      if (valid) d.lastTime = now;
      if (field === 'generated') {
        if (d.times.length < 16) d.times.push(valid ? now : null); else d.overflow = true;
      } else if (d.times.length) {
        const start = d.times.shift();
        if (d.clock === 'valid' && start !== null) {
          const age = now - start;
          d.maximumAgeMilliseconds = Math.max(d.maximumAgeMilliseconds ?? 0, age);
        }
      }
    } catch { try { d.overflow = true; } catch { /* 旁证对象异常不阻断原采集。 */ } }
  }

  // captureAckSnapshot 不输出原FIFO时刻或任何关联；缺钟不能伪报零时延。
  captureAckSnapshot() {
    const d = this.captureAckDiagnostic;
    let oldestPendingAgeMilliseconds = null;
    if (d) {
      const now = typeof currentTime === 'number' ? currentTime * 1000 : NaN;
      if (Number.isFinite(now) && (now < 0 || (d.lastTime !== null && now < d.lastTime))) d.clock = 'invalid';
      else if (!Number.isFinite(now) && d.clock !== 'invalid') d.clock = 'unavailable';
      if (d.clock === 'valid' && this.capturePending > 0 && d.times.length && d.times[0] !== null)
        oldestPendingAgeMilliseconds = now - d.times[0];
    }
    return d ? {kind: 'captureAckDiagnostic', generated: d.generated, applied: d.applied,
      oldInputRejected: d.oldInputRejected, ownerRejected: d.ownerRejected,
      highWater: d.highWater, pending: this.capturePending, overflow: d.overflow,
      clock: d.clock, maximumAgeMilliseconds: d.clock === 'valid' ? d.maximumAgeMilliseconds : null,
      oldestPendingAgeMilliseconds} : null;
  }

  // command只接受主线程已解码的驱动消息，epoch用十进制字符串避免64位精度丢失。
  command(value) {
    if (this.failed) return;
    if (!value || typeof value !== 'object') return this.fail('commandInvalid');
    const idValid = id => typeof id === 'string' && id.length > 0 && id.length <= 128;
    const epochValid = epoch => typeof epoch === 'string' && /^[1-9][0-9]{0,19}$/.test(epoch);
    switch (value.kind) {
      case 'captureStart':
        if (this.ownership && !this.ownsCapture(value)) return; // 原路由暂停后旧启用不得重开输入。
        if (!idValid(value.id) || this.inputId) return this.fail('captureStartInvalid');
        this.inputId = value.id;
        this.capture = [];
        this.capturePending = 0;
        if (this.captureAckDiagnostic) this.captureAckDiagnostic.times.length = 0;
        if (this.observeCaptureState)
          this.port.postMessage({kind: 'captureState', id: value.id, enabled: true});
        break;
      case 'captureAck':
        if (this.ownership && !this.ownsCapture(value)) { this.noteCaptureAck('ownerRejected'); return; }
        if (value.id !== this.inputId || !this.inputId) { this.noteCaptureAck('oldInputRejected'); return; }
        if (this.capturePending === 0) return this.fail('zeroPendingAck');
        --this.capturePending;
        this.noteCaptureAck('applied');
        break;
      case 'captureStop':
        if (this.ownership && !this.ownsCapture(value)) return;
        if (value.id === this.inputId) {
          this.inputId = '';
          this.capture = [];
          if (this.observeCaptureState)
            this.port.postMessage({kind: 'captureState', id: value.id, enabled: false});
        }
        break;
      case 'playbackStart':
        if (!epochValid(value.epoch) || this.epoch) return this.fail('playbackStartInvalid');
        this.epoch = value.epoch;
        this.queue = [];
        this.position = this.ticket = 0;
        this.frameEnergy = 0;
        break;
      case 'playbackStop':
        if (value.epoch === this.epoch) {
          this.epoch = '';
          this.queue = [];
          this.position = 0;
          this.frameEnergy = 0;
        }
        break;
      case 'playbackGate': {
        if (Object.keys(value).length !== 3 || typeof value.enabled !== 'boolean' ||
            !epochValid(value.ticket) || BigInt(value.ticket) > 18446744073709551615n)
          return this.fail('playbackGateInvalid');
        const ticket = BigInt(value.ticket);
        if (ticket < this.gateTicket) return; // 迟到旧票据不重开输出。
        if (ticket === this.gateTicket) {
          if (value.enabled !== this.outputEnabled) this.fail('playbackGateConflict');
          return; // 同正文重复不新增确认或刷新截止。
        }
        if (this.pendingGate) return this.fail('playbackGatePending');
        this.gateTicket = ticket;
        this.outputEnabled = value.enabled;
        this.pendingGate = {kind: 'playbackGated', ticket: value.ticket, enabled: value.enabled};
        break;
      }
      case 'playbackFrame':
        if (value.epoch !== this.epoch || !this.epoch) return;
        // 保留原票据→容量→PCM短路顺序；满队列不能掩盖先发生的非法票据。
        if (!Number.isInteger(value.ticket) || value.ticket !== this.ticket + 1 ||
            value.ticket > 4294967295) return this.fail('playbackFrameInvalid');
        if (this.queue.length >= 32) return this.fail('playbackQueueFull');
        if (!Array.isArray(value.samples) || value.samples.length !== this.frameSamples ||
            !value.samples.every(v => Number.isInteger(v) && v >= -32768 && v <= 32767))
          return this.fail('playbackFrameInvalid');
        this.ticket = value.ticket;
        this.queue.push(value);
        break;
      case 'inputOwnershipGate':
        this.ownershipCommand(value);
        break;
      default:
        this.fail('unknownCommand');
    }
  }

  // process对无音频/停止输入不造语音帧，输出缺帧补静音但不伪造播放确认。
  process(inputs, outputs) {
    for (const output of outputs) for (const channel of output) channel.fill(0);
    if (this.failed) return false;
    const input = inputs[0]?.[0];
    if (this.inputId && input) {
      for (const sample of input) {
        if (!Number.isFinite(sample)) { this.fail('captureSampleInvalid'); return false; }
        const bounded = Math.max(-1, Math.min(1, sample));
        this.capture.push(Math.round(bounded * (bounded < 0 ? 32768 : 32767)));
        if (this.capture.length === this.frameSamples) {
          if (this.capturePending >= 16) { this.fail('captureBacklogFull'); return false; }
          ++this.capturePending;
          this.noteCaptureAck('generated');
          const frame = {kind: 'captured', id: this.inputId, samples: this.capture};
          if (this.ownership) {
            const [sessionId, routeGeneration] = JSON.parse(this.ownershipOwner);
            Object.assign(frame, {sessionId, routeGeneration});
          }
          this.port.postMessage(frame);
          this.capture = [];
        }
      }
    }
    const output = outputs[0]?.[0];
    if (!output) return true;
    for (let index = 0; index < output.length && this.queue.length; ++index) {
      const frame = this.queue[0];
      const sample = this.outputEnabled ? frame.samples[this.position] : 0;
      ++this.position;
      output[index] = sample / 32768;
      this.frameEnergy += sample * sample;
      if (this.position === this.frameSamples) {
        this.queue.shift();
        this.position = 0;
        // 这是音频线程已消费事实，不等同于物理扬声器发声或硬件AEC验收。
        this.port.postMessage({kind: 'played', epoch: this.epoch, ticket: frame.ticket,
          nonSilent: this.frameEnergy / this.frameSamples >= 128 * 128});
        this.frameEnergy = 0;
      }
    }
    if (output.length > 0 && this.pendingGate) {
      this.port.postMessage(this.pendingGate);
      this.pendingGate = null;
    }
    // 只有与原source连接的非空输入/输出处理块才证明音频线程应用，命令入队不回ACK。
    if (this.ownership && input?.length > 0 && output.length > 0) {
      if (this.ownershipProbe && !this.inputId && !this.ownershipOwner && !this.ownershipPending) {
        this.port.postMessage({kind: 'inputOwnershipIdle', bootGeneration: this.ownershipBoot});
        this.ownershipProbe = false;
      }
      if (this.ownershipPending) {
        this.ownershipLast = this.ownershipPending;
        this.port.postMessage(this.ownershipPending);
        this.ownershipPending = null;
        if (this.ownershipCaptureStop) {
          this.port.postMessage(this.ownershipCaptureStop);
          this.ownershipCaptureStop = null;
        }
      }
    }
    return true;
  }
}
registerProcessor('ur-device-audio', DeviceAudioProcessor);
