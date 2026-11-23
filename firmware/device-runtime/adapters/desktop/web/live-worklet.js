// 文件说明：节目独立末端音频线程；原focus mask与单轨意图均默认关闭，不含采集或平台身份。
class UrLiveAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.mask = false; // 末端focus许可，不是节目授权或播放意图。
    this.track = false; // 单轨显式意图，focus恢复不自动重开。
    this.ticket = 0n; // 原末端uint64票据，与原节目generation独立。
    this.generation = 0; // 原单轨本地操作水位，不能变成平台授权。
    this.gatePending = null; // 一个原末端待实际音频块确认。
    this.trackPending = null; // 一个原单轨待实际输出/关闭确认。
    this.failed = sampleRate !== 48000; // 只能声明真实48k末端驱动，不偷偷替换格式。
    this.closed = false; // 关闭后的迟到消息不能复活输出。
    this.port.onmessage = event => this.command(event.data);
  }

  // fail 仅一次固定故障事实；停止本节目输出，不反射自由异常或影响AI/采集节点。
  fail() {
    if (!this.failed) this.port.postMessage({kind: 'liveAudioFailed'});
    this.failed = true;
    this.mask = this.track = false;
  }

  // command 只接宿主固定局部命令；发布受理不等同于实际音频块关闭/非静音输出。
  command(value) {
    if (this.closed || this.failed) return;
    if (value?.kind === 'liveAudioMask' && Object.keys(value).length === 3 &&
        typeof value.enabled === 'boolean' && typeof value.ticket === 'string' &&
        /^[1-9][0-9]{0,19}$/.test(value.ticket) && BigInt(value.ticket) <= 18446744073709551615n &&
        BigInt(value.ticket) > this.ticket && !this.gatePending) {
      this.ticket = BigInt(value.ticket);
      this.gatePending = {ticket: value.ticket, enabled: value.enabled};
    } else if (value?.kind === 'liveAudioTrack' && Object.keys(value).length === 3 &&
        Number.isSafeInteger(value.generation) && value.generation > this.generation &&
        ['resume', 'pause', 'stop'].includes(value.operation) && !this.trackPending) {
      this.generation = value.generation;
      this.trackPending = {generation: value.generation, operation: value.operation};
    } else if (value?.kind === 'liveAudioClose' && Object.keys(value).length === 1) {
      this.closed = true;
      this.mask = this.track = false;
    } else this.fail();
  }

  // process 当前原音频块完成后才确认末端/单轨；没有输入或静音不能成为active。
  process(inputs, outputs) {
    const output = outputs[0] || [];
    for (const channel of output) channel.fill(0);
    if (this.closed) {
      this.port.postMessage({kind: 'liveAudioClosed'});
      return false;
    }
    if (this.failed) return true;
    let canceled = null;
    if (this.gatePending) {
      this.mask = this.gatePending.enabled;
      if (!this.mask) {
        this.track = false; // 关闭末端时移除旧播放意图；恢复mask不自动播放。
        if (this.trackPending?.operation === 'resume') {
          canceled = this.trackPending;
          this.trackPending = null;
        }
      }
    }
    const operation = this.trackPending;
    if (operation) {
      this.track = operation.operation === 'resume' && this.mask;
      if (operation.operation === 'resume' && !this.mask) {
        canceled = operation;
        this.trackPending = null;
      }
    }
    const input = inputs[0]?.[0]; // 宿主MediaStreamAudioSource明确mono，不回退其他源。
    let energy = 0;
    if (this.mask && this.track && input && output.length) {
      for (let index = 0; index < output[0].length; ++index) {
        const value = input[index] ?? 0;
        if (!Number.isFinite(value) || Math.abs(value) > 1) { this.fail(); break; }
        output[0][index] = value;
        energy += value * value;
      }
      if (this.failed) for (const channel of output) channel.fill(0);
    }
    if (this.gatePending) {
      this.port.postMessage({kind: 'liveAudioMasked', ticket: this.gatePending.ticket,
        enabled: this.gatePending.enabled});
      this.gatePending = null;
    }
    if (canceled) this.port.postMessage({kind: 'liveAudioTrackFailed',
      generation: canceled.generation, failure: 'outputBusy'});
    if (!this.failed && operation && this.trackPending &&
        (operation.operation !== 'resume' || energy > 1e-8)) {
      const fact = {kind: 'liveAudioTrackApplied', generation: operation.generation,
        state: operation.operation === 'resume' ? 'active' : operation.operation === 'pause' ? 'paused' : 'stopped'};
      if (fact.state === 'active') fact.gateTicket = this.ticket.toString(); // actual形成时原true票据，不读未来mask。
      this.port.postMessage(fact);
      this.trackPending = null;
    }
    return true;
  }
}
registerProcessor('ur-live-audio', UrLiveAudio);
