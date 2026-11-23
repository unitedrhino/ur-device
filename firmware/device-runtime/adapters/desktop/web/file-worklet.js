// 文件节目音频线程：独立门控和实际采样事实，不持设备身份、不采集麦克风、不停止共享AI上下文。
class FileAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.generation = '0'; // 内部操作代次以字符串精确表示uint64。
    this.enabled = false; // 未收到显式play门控时始终输出静音。
    this.failed = false; // 终态后不复活本节点。
    this.closing = false; // 收到独占节点释放后下一实际块退出音频线程。
    this.closed = false; // 已实际退出，不能再次发布释放或接纳门控。
    this.samples = 0; // 当前代次真实输入采样数，不从时间估算。
    this.energy = 0; // 当前代次实测平方和，用于复听/自动化证据。
    this.reported = 0; // 已发布的采样水位。
    this.pending = []; // 最多8个未确认事实，防止MessagePort无限积压。
    this.ticket = 0; // 本节点有界确认序号，不是MQTT eventSeq。
    this.gate = null; // 等待实际音频块后才能确认的门控。
    this.port.onmessage = event => this.command(event.data);
  }

  // fail 只切断本节目节点，发布一次脱敏失败；不影响其他输入/输出。
  fail() {
    if (this.failed || this.closed) return;
    this.failed = true;
    this.enabled = false;
    this.port.postMessage({kind: 'fileAudioFailed'});
  }

  // command 接受同线程宿主的严格门控与有序确认，不接受PCM注入或自由JSON功能。
  command(value) {
    if (this.failed || this.closed || this.closing) return;
    if (!value || typeof value !== 'object') return this.fail();
    if (value.kind === 'fileAudioAck') {
      if (Object.keys(value).length !== 2 || value.ticket !== this.pending[0]) return this.fail();
      this.pending.shift();
      return;
    }
    if (!['fileAudioGate', 'fileAudioClose'].includes(value.kind) || Object.keys(value).length !== 4 ||
        typeof value.enabled !== 'boolean' || typeof value.ticket !== 'string' ||
        !/^[1-9][0-9]{0,19}$/.test(value.ticket) || typeof value.generation !== 'string' ||
        !/^(0|[1-9][0-9]{0,19})$/.test(value.generation)) return this.fail();
    const epoch = BigInt(value.generation);
    if ((value.enabled && (epoch === 0n || value.kind === 'fileAudioClose')) || epoch > 18446744073709551615n || BigInt(value.ticket) > 18446744073709551615n)
      return this.fail();
    if (epoch < BigInt(this.generation)) return; // 迟到旧操作不能重开节目输出。
    if (epoch !== BigInt(this.generation)) {
      this.generation = value.generation;
      this.samples = this.energy = this.reported = 0;
    }
    if (this.gate) return this.fail(); // 同一时刻只允许一条待确认门控。
    this.enabled = value.enabled;
    this.closing = value.kind === 'fileAudioClose';
    this.gate = value.ticket;
  }

  // process 首先清零；只有当前显式许可才输出节目，实际块完成后回门控/采样事实。
  process(inputs, outputs) {
    for (const bus of outputs) for (const channel of bus) channel.fill(0);
    if (this.failed || this.closed) return false;
    const output = outputs[0];
    const input = inputs[0];
    if (!output?.length || !output[0]?.length) return true;
    if (this.enabled && input?.length && input[0]?.length === output[0].length) {
      for (let channel = 0; channel < output.length; ++channel) {
        const source = input[Math.min(channel, input.length - 1)];
        if (!source || source.length !== output[channel].length || !source.every(Number.isFinite)) {
          this.fail();
          for (const bus of outputs) for (const target of bus) target.fill(0);
          return false;
        }
        for (let index = 0; index < source.length; ++index)
          output[channel][index] = Math.max(-1, Math.min(1, source[index]));
      }
      for (const sample of output[0]) this.energy += sample * sample;
      this.samples += output[0].length;
      if (!Number.isSafeInteger(this.samples) || !Number.isFinite(this.energy)) {
        this.fail();
        for (const bus of outputs) for (const target of bus) target.fill(0);
        return false;
      }
    }
    if (this.gate) {
      this.port.postMessage({kind: this.closing ? 'fileAudioClosed' : 'fileAudioGated', generation: this.generation,
        ticket: this.gate, enabled: this.enabled});
      this.gate = null;
    }
    if (this.closing) {
      this.closed = true;
      this.pending = [];
      return false; // 独占节目节点结束处理，不因共享AudioContext继续运行而残留活动处理器。
    }
    if (this.samples - this.reported >= (this.reported < 256 ? 256 : 2048)) {
      if (this.pending.length >= 8 || this.ticket === 4294967295) {
        this.fail();
        for (const bus of outputs) for (const target of bus) target.fill(0);
        return false;
      }
      this.reported = this.samples;
      this.pending.push(++this.ticket);
      this.port.postMessage({kind: 'fileAudioMeasured', generation: this.generation,
        ticket: this.ticket, samples: this.samples, energy: this.energy});
    }
    return true;
  }
}
registerProcessor('ur-file-audio', FileAudioProcessor);
