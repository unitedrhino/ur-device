// 文件说明：借用唯一原采集源和Worklet的输入门控；不申请麦克风、Peer或替宿主猜会话。
(() => {
  // decimal验证跨语言原安全代次或完整uint64票据，禁止隐式数字转换。
  const decimal = (value, max = 9007199254740991n) => typeof value === 'string' &&
    /^[1-9][0-9]{0,19}$/.test(value) && BigInt(value) <= max;
  // scope只接受原会话/路由代次，拒控制字符、孤立surrogate及超长UTF8。
  const scope = value => value && typeof value.sessionId === 'string' && value.sessionId.length > 0 &&
    !/[\x00-\x1f\x7f]/.test(value.sessionId) &&
    !Array.from(value.sessionId).some(c => { const n = c.codePointAt(0); return n >= 0xd800 && n <= 0xdfff; }) &&
    new TextEncoder().encode(value.sessionId).length <= 64 && decimal(value.routeGeneration);
  const key = value => JSON.stringify([value.sessionId, value.routeGeneration]); // 原scope的内部索引，不输出诊断。
  // senderRoute复用同一实际替轨屏障；AI仅新增用户输入意图门，不改变人工会话的开麦策略。
  const senderRoute = kind => kind === 'humanSender' || kind === 'aiSender';
  // UrInputOwnershipGate只由原authenticated本地桥调用；senders必须回读实际原RTCRtpSender registry。
  class UrInputOwnershipGate {
    constructor({stream, source, worklet, bootGeneration, senders, emit}) {
      this.stream = stream; this.source = source; this.worklet = worklet;
      this.boot = bootGeneration; this.senders = senders; this.emit = emit;
      this.routes = new Map(); this.records = new Map(); this.pending = null;
      this.owner = null; this.high = 0n; this.idle = false; this.closed = false;
      this.inflight = new Set(); this.cleanupFact = 'confirmed'; // 只报告Sender清理，不伪造共享源已释放。
      this.lastTime = null;
      if (!decimal(this.boot) || typeof senders !== 'function' || typeof emit !== 'function') this.closed = true;
    }
    // live以实际原节点/stream/track及Sender回读判定；空回调、未知或异常均不能成为闲置事实。
    live() {
      if (this.closed) return false;
      try {
        const tracks = this.stream.getAudioTracks(), senders = this.senders();
        return this.stream instanceof MediaStream && this.source instanceof MediaStreamAudioSourceNode &&
          this.worklet instanceof AudioWorkletNode && this.source.mediaStream === this.stream && this.source.context === this.worklet.context &&
          this.worklet.context.state === 'running' && tracks.length === 1 &&
          tracks[0].readyState === 'live' && (tracks[0].enabled === true ||
            [...this.routes.values()].some(route => route.kind === 'aiSender' && this.peerMatches(route, false))) &&
          Array.isArray(senders) && senders.length <= 2 && senders.every(sender => {
            if (!(sender instanceof RTCRtpSender)) return false;
            if (sender.track === null) return true;
            const route = this.routes.get(this.owner) ||
              (this.pending?.phase === 'replacing' ? this.routes.get(key(this.pending)) : null);
            return senderRoute(route?.kind) && this.peerMatches(route, route.kind !== 'aiSender') &&
              sender === route.sender && sender.track === tracks[0];
          });
      } catch (_) { return false; }
    }
    // peerMatches只回读实际Peer归属；候选登记可未连通，执行和拥有事实必须connected。
    peerMatches(route, requireConnected = true) {
      try { return route.peer instanceof RTCPeerConnection && route.sender instanceof RTCRtpSender &&
        (requireConnected ? route.peer.connectionState === 'connected' :
          (route.kind === 'aiSender' ? ['new','connecting','connected','disconnected','failed'] :
            ['new','connecting','connected']).includes(route.peer.connectionState)) && route.peer.getSenders().includes(route.sender) &&
        this.senders().includes(route.sender); } catch (_) { return false; }
    }
    // senderCleanupState有任何原承诺在途即未知未完成，不借较早null回读宣称released。
    get senderCleanupState() {
      if (this.inflight.size) return 'pending';
      try {
        if ([...this.routes.values()].some(route => senderRoute(route.kind) && route.sender.track !== null)) return 'unknown';
        return this.cleanupFact;
      } catch (_) { return 'unknown'; }
    }
    // withinDeadline保持原单调两秒窗口，倒退或过期不能安装实际完成事实。
    withinDeadline(record) {
      const now = performance.now();
      if (!Number.isFinite(now) || now < this.lastTime || now >= record.deadline) return false;
      this.lastTime = now; return true;
    }
    // bindRoute仅保存可信宿主的原关联，不根据页面当前会话补字段、不授予资源。
    bindRoute(value, kind, media) {
      if (!this.live() || !scope(value) || !['udp', 'humanSender', 'aiSender'].includes(kind) || this.routes.size >= 2 ||
          [...this.routes.values()].some(route => route.sessionId === value.sessionId || route.routeGeneration === value.routeGeneration)) return false;
      if (senderRoute(kind) && (!media || Object.keys(media).sort().join(',') !== 'peer,sender')) return false;
      if (kind === 'udp' && media !== undefined) return false;
      const route = Object.freeze({sessionId:value.sessionId,routeGeneration:value.routeGeneration,kind,
        ...(senderRoute(kind) ? {peer:media.peer,sender:media.sender} : {})});
      if (senderRoute(kind) && (!this.peerMatches(route, false) || route.sender.track !== null)) return false;
      this.routes.set(key(value), route); return true;
    }
    // request只转发原CPP已完成fence后的票据；Human先固定false线程屏障再执行原Sender。
    request(value) {
      if (!this.live() || !value || Object.keys(value).sort().join(',') !== 'enabled,kind,routeGeneration,sessionId,ticket' ||
          value.kind !== 'inputOwnershipGate' || !scope(value) || typeof value.enabled !== 'boolean' ||
          !decimal(value.ticket, 18446744073709551615n)) return false;
      const route = this.routes.get(key(value)), ticket = BigInt(value.ticket);
      if (!route || (senderRoute(route.kind) && !this.peerMatches(route, value.enabled || route.kind !== 'aiSender'))) return false;
      const old = this.records.get(value.ticket);
      if (ticket <= this.high) return !!old && old.sessionId === value.sessionId &&
        old.routeGeneration === value.routeGeneration && old.enabled === value.enabled;
      if (this.pending || this.records.size >= 128 || (value.enabled && this.owner && this.owner !== key(value))) return false;
      const now = performance.now();
      if (!Number.isFinite(now) || now < 0 || (this.lastTime !== null && now < this.lastTime)) { this.close(); return false; }
      this.lastTime = now;
      const record = {...value, actual: false, phase: 'worklet', deadline: now + 2000};
      this.records.set(value.ticket, record); this.high = ticket; this.pending = record; this.idle = false;
      if (!value.enabled && route.kind === 'udp' && this.owner === key(value)) this.owner = null; // 先阻断主线程旧帧。
      record.timer = setTimeout(() => { if (this.pending === record) this.close(); }, 2000);
      try { this.worklet.port.postMessage({...value, enabled: senderRoute(route.kind) ? false : value.enabled}); return true; }
      catch (_) { this.close(); return false; }
    }
    // handleActual只消费原Worklet onmessage事实；入队与UI状态不能调用此函数制造完成。
    handleActual(value) {
      if (!this.live() || !value || typeof value !== 'object') return false;
      if (value.kind === 'inputOwnershipIdle') {
        if (Object.keys(value).sort().join(',') !== 'bootGeneration,kind' || value.bootGeneration !== this.boot ||
            this.records.size || this.pending) return false;
        this.idle = true; this.emit({...value}); return true;
      }
      if (value.kind !== 'inputOwnershipGated' || Object.keys(value).sort().join(',') !== 'enabled,kind,routeGeneration,sessionId,ticket' ||
          !scope(value) || !decimal(value.ticket, 18446744073709551615n) || typeof value.enabled !== 'boolean') return false;
      const original = this.records.get(value.ticket);
      const route = original && this.routes.get(key(original));
      if (!original || original.sessionId !== value.sessionId || original.routeGeneration !== value.routeGeneration ||
          (senderRoute(route.kind) ? value.enabled !== false : original.enabled !== value.enabled)) return false;
      if (original.actual) return true; // 重复事实不重复发回执或重开旧路由。
      if (this.pending !== original || !this.withinDeadline(original)) { this.close(); return false; }
      if (senderRoute(route.kind)) {
        if (original.phase === 'worklet') this.replace(original, route);
        return true;
      }
      clearTimeout(original.timer);
      original.actual = true; this.pending = null;
      if (value.enabled) this.owner = key(value);
      this.emit({...value}); return true;
    }
    // forwardControl必须已有原scope，禁止给旧captureStart/Stop/Ack贴当前会话身份。
    forwardControl(value) {
      if (!this.live() || !scope(value) || !['captureStart', 'captureStop', 'captureAck'].includes(value.kind) ||
          this.pending || this.owner !== key(value) || this.routes.get(this.owner)?.kind !== 'udp') return false;
      this.worklet.port.postMessage({...value}); return true;
    }
    // forwardSenderControl要求原AI整组取得和新采集意图同时存在；取得焦点本身不能传音。
    forwardSenderControl(value) {
      if (!this.live() || !scope(value) || !['captureStart', 'captureStop'].includes(value.kind) ||
          typeof value.id !== 'string' || !value.id || value.id.length > 128 ||
          this.pending || this.owner !== key(value)) return false;
      const route = this.routes.get(this.owner), track = this.stream.getAudioTracks()[0];
      if (route?.kind !== 'aiSender' || !this.peerMatches(route) || route.sender.track !== track) return false;
      track.enabled = value.kind === 'captureStart';
      return track.enabled === (value.kind === 'captureStart');
    }
    // forwardCapture保留原已绑定scope给CPP独立验证，不借旧帧的当前owner补关联。
    forwardCapture(value) {
      return !!(this.live() && value && value.kind === 'captured' && scope(value) && !this.pending && this.owner === key(value) &&
        this.routes.get(this.owner)?.kind === 'udp');
    }
    // replace只在原线程false屏障之后调用原Sender；完成回读或迟到清理不操作另一腿。
    replace(record, route) {
      record.phase = 'replacing';
      let promise;
      try {
        const track = this.stream.getAudioTracks()[0];
        if (route.kind === 'aiSender') track.enabled = false; // 先停实际传输，新audioStart才能解除静音。
        promise = route.sender.replaceTrack(record.enabled ? track : null);
      }
      catch (_) { this.close(); return; }
      const task = Promise.resolve(promise).then(async () => {
        if (this.closed || this.pending !== record || !this.withinDeadline(record) ||
            !this.peerMatches(route, record.enabled || route.kind !== 'aiSender') || !this.live() || this.routes.get(key(record)) !== route ||
            route.sender.track !== (record.enabled ? this.stream.getAudioTracks()[0] : null)) {
          this.close(); await this.detach(route); return;
        }
        clearTimeout(record.timer); record.actual = true; this.pending = null;
        if (record.enabled) this.owner = key(record);
        else if (this.owner === key(record)) this.owner = null;
        try { this.emit({kind:'inputOwnershipSenderGated',sessionId:record.sessionId,
          routeGeneration:record.routeGeneration,ticket:record.ticket,enabled:record.enabled}); }
        catch (_) { this.close(); }
      }, async () => { this.close(); await this.detach(route); }).catch(async () => {
        this.close(); await this.detach(route);
      });
      this.inflight.add(task);
      task.finally(() => this.inflight.delete(task));
    }
    // detach精确原Sender的null补偿，承诺和回读缺任一均未知；不停止共享音轨。
    detach(route) {
      this.cleanupFact = 'pending';
      let promise;
      try { promise = route.sender.replaceTrack(null); }
      catch (_) { this.cleanupFact = 'unknown'; return Promise.resolve(false); }
      const task = Promise.resolve(promise).then(() => {
        const confirmed = route.sender.track === null;
        this.cleanupFact = confirmed ? 'confirmed' : 'unknown'; return confirmed;
      }, () => { this.cleanupFact = 'unknown'; return false; }).catch(() => { this.cleanupFact = 'unknown'; return false; });
      this.inflight.add(task); task.finally(() => this.inflight.delete(task)); return task;
    }
    // close退役原适配并精确清原Sender；旧true承诺晚到后replace回调必须再次null补偿。
    close() {
      if (!this.closed) {
        const pending = this.pending;
        this.closed = true; this.idle = false; this.owner = null; this.pending = null;
        if (pending) clearTimeout(pending.timer);
        // AI源实际静音先于异步null；迟到替轨承诺也不能再次传送已关闭会话的音频。
        if ([...this.routes.values()].some(route => route.kind === 'aiSender'))
          for (const track of this.stream.getAudioTracks()) track.enabled = false;
        for (const route of this.routes.values()) if (senderRoute(route.kind) &&
            (route.sender.track !== null || (pending?.phase === 'replacing' && key(pending) === key(route)))) this.detach(route);
      }
      // 只等待原在途承诺；两秒后明确未知，不阻塞宿主收尾，也不注销迟到补偿回调。
      return new Promise(resolve => {
        const timer = setTimeout(() => resolve(false), 2000);
        Promise.allSettled([...this.inflight]).then(() => { clearTimeout(timer); resolve(this.senderCleanupState === 'confirmed'); });
      });
    }
  }
  globalThis.UrInputOwnershipGate = UrInputOwnershipGate;
})();
