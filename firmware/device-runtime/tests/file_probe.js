// 测试探针页面：实际借用AudioContext/HTMLMediaElement与C++桥，不采集麦克风或持设备身份。
let fileProbe = null; // 仅供本页软件证据读取的独占组件，不是平台协议状态。
let socket = null; // 唯一回环已鉴权连接。
let context = null; // 本页自己拥有的音频上下文；文件组件依然只借用。
let fileLaneProbe = null; // 显式固定路径验收模式，身份仍在C++，不是平台许可。
let fileLaneAnalyser = null; // 采样实际路径末端，不能用门控前节目采样代替最终声音。
// fileLanePower 返回末端真实采样平方和均值，不修改音量或注入合成播放事实。
function fileLanePower() {
  if (!fileLaneAnalyser) return -1;
  const samples = new Float32Array(fileLaneAnalyser.fftSize);
  fileLaneAnalyser.getFloatTimeDomainData(samples);
  return samples.reduce((sum, sample) => sum + sample * sample, 0) / samples.length;
}
document.getElementById('enable').addEventListener('click', async () => {
  const button = document.getElementById('enable');
  button.disabled = true;
  const status = document.getElementById('status');
  try {
    context = new AudioContext();
    await context.resume();
    const bootstrap = await fetch('/bootstrap', {credentials: 'omit', redirect: 'error'});
    if (!bootstrap.ok) throw new Error('bootstrap');
    const {token} = await bootstrap.json();
    socket = new WebSocket(`ws://${location.host}/bridge?token=${token}`);
    const report = value => {
      if (socket.readyState !== WebSocket.OPEN) return;
      socket.send(JSON.stringify(value));
      status.textContent = value.state || value.kind; // 固定状态，不反射下载地址或异常。
    };
    const laneMode = new URLSearchParams(location.hash.slice(1)).get('lane') === '1'; // 仅本页探针启用，不扩大HTTP查询白名单。
    let pendingBinding = null; // 原播放器绑定等待实际路径关闭，不先接默认扬声器绕过栅栏。
    // prepare 固定节目只能接已实际初始化的原路径；旧模式仍使用原组件默认输出。
    const prepare = async value => {
      if (fileProbe) throw new Error('duplicateBinding');
      const destination = laneMode ? fileLaneProbe?.destination() : context.destination;
      if (!destination) { pendingBinding = value; return; }
      fileProbe = new UrFilePlayer(context, document.getElementById('media'), report, token, destination);
      await fileProbe.initialize(value);
    };
    socket.onmessage = async event => {
      const value = JSON.parse(event.data);
      try {
        if (value.kind === 'fileLaneGate' && laneMode) {
          if (!fileLaneProbe) fileLaneProbe = new UrFileLane(context, document.getElementById('media'), report, value.lane);
          if (!await fileLaneProbe.gate(value)) throw new Error('unknownLane');
          if (!fileLaneAnalyser) {
            fileLaneAnalyser = context.createAnalyser();
            fileLaneProbe.destination().connect(fileLaneAnalyser);
          }
          if (pendingBinding) { const binding = pendingBinding; pendingBinding = null; await prepare(binding); }
        } else if (value.kind === 'fileBind') await prepare(value);
        else if (value.kind === 'fileControl') await fileProbe.control(value);
        else if (value.kind === 'fileOutputGate') await fileProbe.outputGate(value);
        else if (value.kind === 'fileClose') await fileProbe.close(value);
        else socket.close();
      } catch { socket.close(); }
    };
    socket.onclose = () => { if (fileProbe) { fileProbe.closing = true; fileProbe.release(); }
      fileLaneProbe?.release(); fileLaneAnalyser?.disconnect(); status.textContent = '已断开'; };
    socket.onerror = () => { status.textContent = '连接失败'; };
  } catch { status.textContent = '初始化失败'; }
});
// 页面离开仅释放本探针独占资源，不调用任何设备或后端控制命令。
window.addEventListener('beforeunload', () => {
  if (fileProbe) { fileProbe.closing = true; fileProbe.release(); }
  fileLaneProbe?.release(); fileLaneAnalyser?.disconnect();
  socket?.close();
});
