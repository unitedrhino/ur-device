// 文件说明：原人际呼叫下行音频的末端实态门控；不持平台身份、不控制麦克风或自动恢复。
'use strict';
class UrCallAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.enabled = false; // 构造不授权输出，首块默认归零。
    this.ticket = 0n; // 本节点原末端票据，不能借call revision确认。
    this.epoch = 0n; // 原Peer代次，旧Peer不能重新打开新原输出。
    this.pending = null; // 至多一个尚未实际处理的原目标。
    this.failed = sampleRate !== 48000; // 不把浏览器重采样猜成真实声明格式。
    this.closed = false; // 页面释放后迟到指令不复活节点。
    this.zeroBlocks = 0; this.nonSilentBlocks = 0; // 饱和固定计数，不保存PCM正文。
    this.port.onmessage = event => this.command(event.data);
  }
  // fail 未知输出只收敛静音；不把失败广播当成实际零块已完成。
  fail() {
    this.failed = true; this.enabled = false; this.pending = null;
    this.port.postMessage({kind: 'callAudioFailed'});
  }
  // command 严格局部票据；abort能撤销原在途打开，但不能形成伪造成功回执。
  command(value) {
    if (this.closed) return;
    if (value?.kind === 'callAudioClose' && Object.keys(value).length === 1) {
      this.closed = true; this.enabled = false; this.pending = null; return;
    }
    if (value?.kind === 'callAudioAbort' && Object.keys(value).length === 2 &&
        typeof value.ticket === 'string' && value.ticket === this.ticket.toString()) {
      this.fail(); return;
    }
    if (this.failed) return;
    if (value?.kind !== 'callAudioMask' || Object.keys(value).length !== 4 ||
        typeof value.enabled !== 'boolean' || typeof value.ticket !== 'string' ||
        !/^[1-9][0-9]{0,19}$/.test(value.ticket) || BigInt(value.ticket) > 18446744073709551615n ||
        typeof value.peerEpoch !== 'string' || !/^(0|[1-9][0-9]{0,19})$/.test(value.peerEpoch) ||
        BigInt(value.peerEpoch) > 18446744073709551615n || BigInt(value.peerEpoch) < this.epoch ||
        BigInt(value.ticket) <= this.ticket || this.pending) {
      this.fail(); return;
    }
    this.ticket = BigInt(value.ticket); this.epoch = BigInt(value.peerEpoch);
    this.pending = {ticket: value.ticket, peerEpoch: value.peerEpoch, enabled: value.enabled};
  }
  // process 当前原音频块实际归零/通过后才确认，命令受理不提供扬声器事实。
  process(inputs, outputs) {
    const channels = outputs[0] || [];
    for (const channel of channels) channel.fill(0);
    if (this.closed) { this.port.postMessage({kind: 'callAudioClosed'}); return false; }
    // 调度空块不含任何实际输出，不能确认末端停止或打开，也不累加实际块计数。
    if (!channels.length || !channels.every(channel => channel.length > 0)) return true;
    if (this.pending) this.enabled = this.pending.enabled;
    let energy = 0;
    if (!this.failed && this.enabled) {
      for (let channel = 0; channel < channels.length; ++channel) {
        const input = inputs[0]?.[channel] || inputs[0]?.[0];
        for (let index = 0; index < channels[channel].length; ++index) {
          const value = input?.[index] ?? 0;
          if (!Number.isFinite(value) || Math.abs(value) > 1) { this.fail(); break; }
          channels[channel][index] = value; energy += value * value;
        }
        if (this.failed) break;
      }
      if (this.failed) for (const channel of channels) channel.fill(0);
    }
    if (this.pending && !this.failed) {
      this.port.postMessage({kind: 'callAudioGated', ...this.pending}); this.pending = null;
    }
    if (!energy || this.failed) this.zeroBlocks = Math.min(1e9, this.zeroBlocks + 1);
    else this.nonSilentBlocks = Math.min(1e9, this.nonSilentBlocks + 1);
    if ((this.zeroBlocks + this.nonSilentBlocks) % 64 === 0)
      this.port.postMessage({kind: 'callAudioStats', zeroBlocks: this.zeroBlocks,
        nonSilentBlocks: this.nonSilentBlocks});
    return true;
  }
}
registerProcessor('ur-call-audio', UrCallAudio);
