// 默认桌面文件媒体宿主：固定三组输出与单播放器；不持设备身份、云端授权或麦克风权限。
class UrDesktopFiles {
  constructor(context, send, token, fail) {
    this.context = context; // 借用AI上下文，不close/suspend采集。
    this.send = send; // 原本地事实出口，不发送MQTT。
    this.token = token; // 短期本地配对令牌，不是设备密钥。
    this.fail = fail; // 不可信/未知驱动事实令宿主收敛。
    this.lanes = new Map(); // 固定audio/video/audioVideo真实完整资源组。
    this.player = null; // 原实际关闭完成之前不换播放器。
    this.closed = false; // 迟到任务不能恢复已关闭页面。
    this.mount = document.createElement('section'); // 独占节目容器，不修改AI文字/摄像头。
    this.mount.setAttribute('aria-label', '设备文件媒体');
    document.querySelector('main').appendChild(this.mount);
  }

  // handle 只分派C++固定命令；关闭不排在长下载后，实际票据由原组件独立确认。
  handle(value) {
    if (!['fileLaneGate', 'fileBind', 'fileControl', 'fileOutputGate', 'fileClose'].includes(value?.kind))
      return false;
    if (!this.closed) void this.execute(value).catch(() => { this.close(); this.fail(); });
    return true;
  }

  // execute 必须接已初始化原节点；程序解码不能绕过固定末端声音/画面栅栏。
  async execute(value) {
    if (this.closed) return;
    if (value.kind === 'fileLaneGate') {
      if (!['audio', 'video', 'audioVideo'].includes(value.lane)) throw Error('invalidLane');
      let lane = this.lanes.get(value.lane);
      if (!lane) {
        const mount = document.createElement('div');
        this.mount.appendChild(mount);
        lane = new UrFileLane(this.context, mount, this.send, value.lane);
        this.lanes.set(value.lane, lane);
      }
      if (!await lane.gate(value)) throw Error('unknownGate');
    } else if (value.kind === 'fileBind') {
      if (this.player) throw Error('busy');
      const video = value.mime === 'video/mp4' || value.mime === 'video/webm';
      const lane = this.lanes.get(video ? (value.audio ? 'audioVideo' : 'video') : 'audio');
      if (!lane?.destination()) throw Error('unknownLane');
      this.player = new UrFilePlayer(this.context, lane.mount, this.send, this.token, lane.destination());
      await this.player.initialize(value); // 原fileReady报告成败，不替它发接受或播放事实。
    } else {
      const player = this.player;
      if (!player) throw Error('unknownPlayer');
      if (value.kind === 'fileControl') await player.control(value);
      else if (value.kind === 'fileOutputGate') {
        if (!await player.outputGate(value)) throw Error('unknownGate');
      } else {
        if (!await player.close(value)) throw Error('unknownClose');
        if (this.player === player) this.player = null; // 仅实际关闭完成可移除原实例。
      }
    }
  }

  // close 仅尽力释放本文件节点，不伪报fileClosed、不关闭借用的AI上下文。
  close() {
    if (this.closed) return;
    this.closed = true;
    if (this.player) { this.player.closing = true; this.player.release(); }
    for (const lane of this.lanes.values()) lane.release();
    this.mount.remove();
  }
}
globalThis.UrDesktopFiles = UrDesktopFiles;
