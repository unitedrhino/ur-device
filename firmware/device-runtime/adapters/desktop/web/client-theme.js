// 文件说明：平台审核的本地显示主题末端；仅固定颜色与本地票据，不接收平台身份、脚本或自由参数。
(() => {
  // palettes 是代码内固定实际主题，不能由工具参数提供任意CSS或远程资源。
  const palettes = Object.freeze({
    light: Object.freeze({background: 'rgb(248, 250, 252)', foreground: 'rgb(15, 23, 42)'}),
    dark: Object.freeze({background: 'rgb(15, 23, 42)', foreground: 'rgb(248, 250, 252)'}),
  });
  // ClientThemeDisplay 在原页面生命周期运行；实际绘制后读回颜色才回报succeeded。
  class ClientThemeDisplay {
    // platform显式提供原页面DOM/渲染/定时/发送出口，离线测试不伪装实际浏览器验收。
    constructor(platform) {
      this.platform = platform; // 原页面绑定端口，不读取其他窗口或创建网络连接。
      this.pending = null; // 唯一原本地票据；不覆盖在途主题。
      this.records = new Map(); // 最多512墓碑，不能淘汰旧票据后重新应用。
      this.closed = false; // 页面退休后禁止新执行，迟到帧不能确认。
      this.highWater = 0n; // 原本地票据单调，不能由老页面恢复新授权。
    }
    // ready 只在真实主题端口构造后告知宿主；该事实不能替代平台原会话/回复授权。
    ready() {
      if (this.closed || !this.platform.element?.style || !this.platform.element?.dataset)
        return false;
      this.platform.emit({kind: 'clientThemeReady', schemaRevision: 1});
      return true;
    }
    // handle 严格白名单校验后应用固定主题；返回true只表示接纳，实际结果另行关联原票据。
    handle(value) {
      if (this.closed || !value || Object.keys(value).sort().join(',') !== 'kind,theme,themeTicket' ||
          value.kind !== 'clientThemeSet' || !Object.hasOwn(palettes, value.theme) ||
          typeof value.themeTicket !== 'string' || !/^[1-9][0-9]{0,19}$/.test(value.themeTicket) ||
          BigInt(value.themeTicket) > 18446744073709551615n)
        return false;
      const previous = this.records.get(value.themeTicket);
      if (previous) {
        if (previous.theme !== value.theme) return false;
        if (previous.fact) this.platform.emit({...previous.fact}); // 仅重放原读回结果，不重新改样式。
        return true;
      }
      if (this.pending || this.records.size >= 512 || BigInt(value.themeTicket) <= this.highWater)
        return false;
      const record = {theme: value.theme, ticket: value.themeTicket, frame: null, timer: null, fact: null};
      this.records.set(value.themeTicket, record);
      this.highWater = BigInt(value.themeTicket);
      this.pending = record;
      // 1500ms仅是网页实际读回预算；C++另服从原平台截止与最多2s，不续期或自动重放副作用。
      record.timer = this.platform.setTimer(() => this.finish(record, 'unknown'), 1500);
      try {
        const palette = palettes[value.theme];
        this.platform.element.style.backgroundColor = palette.background;
        this.platform.element.style.color = palette.foreground;
        this.platform.element.dataset.clientTheme = value.theme;
        record.frame = this.platform.scheduleFrame(() => {
          if (this.closed || this.pending !== record || record.fact) return;
          const actual = this.platform.readStyle(this.platform.element);
          const confirmed = this.platform.isVisible() &&
            this.platform.element.dataset.clientTheme === value.theme &&
            actual.backgroundColor === palette.background && actual.color === palette.foreground;
          this.finish(record, confirmed ? 'succeeded' : 'unknown');
        });
      } catch {
        this.finish(record, 'unknown'); // 样式可能部分生效，不能把异常冒称确定未执行。
      }
      return true;
    }
    // finish 只确认仍在等待的原票据，终态固定；不回滚已经发生的UI副作用。
    finish(record, status) {
      if (this.pending !== record || record.fact) return;
      if (record.frame !== null) this.platform.cancelFrame(record.frame);
      if (record.timer !== null) this.platform.clearTimer(record.timer);
      record.fact = {kind: 'clientThemeActual', themeTicket: record.ticket, status,
        theme: status === 'succeeded' ? record.theme : ''};
      this.pending = null;
      if (!this.closed) this.platform.emit({...record.fact});
    }
    // close 清理原渲染/定时等待，不发送伪成功或把旧页面票据转给新页面。
    close() {
      if (this.closed) return;
      this.closed = true;
      if (this.pending) this.finish(this.pending, 'unknown');
    }
  }
  globalThis.UrClientThemeDisplay = ClientThemeDisplay; // 仅供同源正式app显式装配，不自动广告能力或执行。
})();
