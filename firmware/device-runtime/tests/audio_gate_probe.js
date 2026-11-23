// 文件说明：实际WebAudio探针，合成输入而非麦克风；正式Worklet输出交给独立Analyser核验。
const gateEvidence = {played: [0, 0, 0], nonSilent: [0, 0, 0], capture: [0, 0, 0], peak: [0, 0, 0], gates: [], failure: false};
let context, socket, node, source, analyser; // 本页面独占资源，退出不操作其他页面或设备。
let phase = 0, focusMode = false; // 焦点初始化期间不计入三个实际媒体段。
// release 释放本页资源，不伪造C++或云端资源释放证明。
function release() {
  source?.stop(); source = null;
  node?.disconnect(); node = null;
  analyser?.disconnect(); analyser = null;
  context?.close(); context = null;
}
document.getElementById('enable').addEventListener('click', async () => {
  document.getElementById('enable').disabled = true;
  try {
    context = new AudioContext({sampleRate: 48000});
    await context.resume();
    await context.audioWorklet.addModule('/audio-worklet.js');
    node = new AudioWorkletNode(context, 'ur-device-audio', {numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1], processorOptions: {sampleRate: context.sampleRate, frameMs: 20}});
    analyser = context.createAnalyser(); analyser.fftSize = 128;
    node.connect(analyser); analyser.connect(context.destination);
    source = context.createOscillator(); source.frequency.value = 440;
    const gain = context.createGain(); gain.gain.value = 0.25;
    source.connect(gain); gain.connect(node); source.start();
    const response = await fetch('/bootstrap', {credentials: 'omit', redirect: 'error'});
    if (!response.ok) throw new Error('bootstrap');
    const {token} = await response.json();
    socket = new WebSocket(`ws://${location.host}/bridge?token=${token}`);
    socket.onopen = () => socket.send(JSON.stringify({kind: 'ready', sampleRate: context.sampleRate, frameMs: 20, aec: false}));
    socket.onmessage = event => {
      const command = JSON.parse(event.data);
      if (command.kind === 'focusProbe') { focusMode = true; phase = -1; }
      else node.port.postMessage(command);
    };
    node.port.onmessage = event => {
      const value = event.data;
      if (value.kind === 'captured') {
        if (phase >= 0) ++gateEvidence.capture[phase];
        socket.send(JSON.stringify(value));
        node.port.postMessage({kind: 'captureAck', id: value.id});
      } else if (value.kind === 'played') {
        ++gateEvidence.played[phase];
        if (value.nonSilent) ++gateEvidence.nonSilent[phase];
        // 跳过每段首五帧，避免把门控前在途音频块计入静音段；独立读取真实输出图。
        if (gateEvidence.played[phase] > 5) {
          const output = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(output);
          gateEvidence.peak[phase] = Math.max(gateEvidence.peak[phase], ...output.map(Math.abs));
        }
        socket.send(JSON.stringify({kind: 'played', epoch: value.epoch, ticket: value.ticket}));
      } else if (value.kind === 'playbackGated') {
        gateEvidence.gates.push({ticket: value.ticket, enabled: value.enabled});
        if (focusMode) {
          const count = gateEvidence.gates.length;
          if (count >= 2 && count <= 4) phase = count - 2;
          if (count === 5) {
            // 输出线程先确认原false，再独立观察真实图静音；不能把C++入队或声音素材为空当证明。
            setTimeout(() => {
              const output = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(output);
              gateEvidence.releasePeak = Math.max(...output.map(Math.abs));
              socket.send(JSON.stringify({kind: 'outputGated', ticket: value.ticket, enabled: value.enabled}));
            }, 64);
            return;
          }
        } else phase = value.enabled ? 2 : 1;
        socket.send(JSON.stringify({kind: 'outputGated', ticket: value.ticket, enabled: value.enabled}));
      } else { gateEvidence.failure = true; socket.close(); }
    };
    socket.onclose = () => { release(); document.getElementById('status').textContent = '已结束'; };
  } catch { gateEvidence.failure = true; socket?.close(); release(); }
});
window.addEventListener('beforeunload', () => { socket?.close(); release(); });
