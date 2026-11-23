// 文件说明：节目独立接收transform；默认丢帧，恢复须原新意图和H264新IDR，不发布或采集媒体。
// h264IDR 仅确认Annex-B或4字节长度格式真实NAL5；未知格式不把key标志冒充IDR。
function h264IDR(data) {
  if (!(data instanceof ArrayBuffer) || data.byteLength < 5 || data.byteLength > 4 * 1024 * 1024) return false;
  const bytes = new Uint8Array(data);
  let annex = false;
  for (let index = 0; index + 3 < bytes.length; ++index) {
    if (bytes[index] || bytes[index + 1]) continue;
    let header = -1;
    if (bytes[index + 2] === 1) header = index + 3;
    else if (bytes[index + 2] === 0 && bytes[index + 3] === 1) header = index + 4;
    if (header < 0 || header >= bytes.length) continue;
    annex = true;
    if ((bytes[header] & 31) === 5) return true;
  }
  if (annex) return false;
  let position = 0;
  while (position + 4 < bytes.length) {
    const length = bytes[position] * 16777216 + bytes[position + 1] * 65536 +
      bytes[position + 2] * 256 + bytes[position + 3];
    position += 4;
    if (!length || position + length > bytes.length) return false;
    if ((bytes[position] & 31) === 5) return true;
    position += length;
  }
  return false;
}

// UrLiveTransform 每个真实receiver独立原generation；不按另一轨IDR或恢复mask自动放行。
class UrLiveTransform {
  constructor(transformer) {
    this.transformer = transformer; // 原接收器实际stream，不创建发送器。
    this.port = transformer.options?.port; // 原本地MessagePort，非MQTT或平台token。
    this.video = transformer.options?.kind === 'video'; // 固定实际接收轨kind。
    this.generation = 0; // 原单轨显式意图水位。
    this.ticket = 0n; // 当前原末端mask观察票据，不能成为来源授权。
    this.mask = false; // 末端关闭，初始不允许解码队列积压节目。
    this.wanted = false; // 原单轨意图，focus恢复不自动复活。
    this.needKey = this.video; // video每次显式resume都须当前新IDR。
    this.fresh = false; // 当前原意图是否已放行新编码帧，不等于实际active。
    this.stopped = false; // 单轨stop不可再次resume。
    this.closed = false; // 全组关闭后任何迟到消息都不能入解码器。
    this.failed = false; // 固定故障，不泄漏原自由错误或源标识。
    this.controller = new AbortController(); // 原pipeline取消/关闭，不等待新网络帧到来。
    if (!this.port || !['audio', 'video'].includes(transformer.options?.kind) ||
        !transformer.readable || !transformer.writable ||
        (this.video && typeof transformer.sendKeyFrameRequest !== 'function')) {
      this.fail(); return;
    }
    this.port.onmessage = event => this.command(event.data);
    this.port.start?.();
    this.completion = transformer.readable.pipeThrough(new TransformStream({
      transform: (frame, output) => this.frame(frame, output),
    }), {signal: this.controller.signal}).pipeTo(transformer.writable, {signal: this.controller.signal})
      .then(() => { if (!this.closed) this.fail(); })
      .catch(() => { if (!this.closed) this.fail(); });
    this.port.postMessage({kind: 'liveTransformReady'}); // stream实际绑定，不冒充PCM/IDR/active。
  }

  // fail 固定本接收器失败，立即丢后续帧，不影响另一轨、AI输入或其他媒体handler。
  fail() {
    if (this.failed) return;
    this.failed = true; this.mask = this.wanted = false;
    this.port?.postMessage({kind: 'liveTransformFailed'});
  }

  // command 严格固定本地控制；单轨stop不可恢复，恢复mask没有新的播放意图。
  command(value) {
    if (this.closed) return;
    // 故障也必须允许原精确关闭；否则pipeline持续挂起，无法提供实际transform退出证明。
    if (value?.kind === 'liveTransformClose' && Object.keys(value).length === 1) {
      this.closed = true; this.wanted = this.mask = false;
      this.controller.abort();
      void Promise.resolve(this.completion).then(() => this.port?.postMessage({kind: 'liveTransformClosed'}));
      return;
    }
    if (this.failed) return;
    if (value?.kind === 'liveTransformMask' && Object.keys(value).length === 3 &&
        typeof value.enabled === 'boolean' && typeof value.ticket === 'string' &&
        /^[1-9][0-9]{0,19}$/.test(value.ticket) && BigInt(value.ticket) <= 18446744073709551615n &&
        BigInt(value.ticket) > this.ticket) {
      this.ticket = BigInt(value.ticket); this.mask = value.enabled;
      if (!this.mask) { this.wanted = false; this.fresh = false; this.needKey = this.video; }
      this.port.postMessage({kind: 'liveTransformMasked', ticket: value.ticket, enabled: value.enabled});
    } else if (value?.kind === 'liveTransformTrack' && Object.keys(value).length === 3 &&
        Number.isSafeInteger(value.generation) && value.generation > this.generation &&
        ['resume', 'pause', 'stop'].includes(value.operation) &&
        (!this.stopped || value.operation === 'stop')) {
      this.generation = value.generation;
      this.wanted = value.operation === 'resume'; this.fresh = false; this.needKey = this.video;
      if (value.operation === 'stop') this.stopped = true;
      if (this.video && this.wanted) {
        void this.transformer.sendKeyFrameRequest().catch(() => this.fail()); // 请求成功也不是新IDR事实。
      }
      this.port.postMessage({kind: 'liveTransformTrackAccepted', generation: value.generation});
    } else this.fail();
  }

  // frame 只放当前新意图/原mask；video需真实NAL5及当前RTP时间，旧解码队列由末端另行隔离。
  frame(frame, output) {
    if (this.closed || this.failed || this.stopped || !this.wanted || !this.mask) return;
    let stamp = null;
    if (this.video) {
      stamp = frame.getMetadata?.().rtpTimestamp;
      if (!Number.isInteger(stamp) || stamp < 0 || stamp > 4294967295) { this.fail(); return; }
      if (this.needKey) {
        if (frame.type !== 'key' || !h264IDR(frame.data)) return;
        this.needKey = false;
      }
    }
    if (!this.fresh) {
      this.fresh = true;
      const fact = {kind: 'liveEncodedFresh', generation: this.generation, gateTicket: this.ticket.toString()};
      if (this.video) fact.rtpTimestamp = stamp;
      this.port.postMessage(fact); // 仅编码新帧，真实PCM/可见呈现仍不得省略。
    }
    output.enqueue(frame);
  }
}
// onrtctransform 仅为原receiver绑定实际pipeline，不把Worker启动或库存在作为能力广告。
self.onrtctransform = event => { new UrLiveTransform(event.transformer); };
