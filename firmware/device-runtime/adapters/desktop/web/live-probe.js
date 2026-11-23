// 文件说明：实时接收格式builtin自检；只解码固定PCM/彩块，不获取媒体权限或合法节目内容。
// 固定32×32 Annex-B Constrained Baseline单IDR，SHA256=5d1eb2e05252a396ed7050390f180d571740c43ac2018528152f8de236aba4a1。
const UR_LIVE_PROBE_H264 = 'AAAAAWdCwArclsBEAAADAAQAAAMACjxIngAAAAFozg/IAAABBgX//03cRem95tlIt5Ys2CDZI+7veDI2NCAtIGNvcmUgMTY0IHIzMTA4IDMxZTE5ZjkgLSBILjI2NC9NUEVHLTQgQVZDIGNvZGVjIC0gQ29weWxlZnQgMjAwMy0yMDIzIC0gaHR0cDovL3d3dy52aWRlb2xhbi5vcmcveDI2NC5odG1sIC0gb3B0aW9uczogY2FiYWM9MCByZWY9MSBkZWJsb2NrPTA6MDowIGFuYWx5c2U9MDowIG1lPWRpYSBzdWJtZT0wIHBzeT0xIHBzeV9yZD0xLjAwOjAuMDAgbWl4ZWRfcmVmPTAgbWVfcmFuZ2U9MTYgY2hyb21hX21lPTEgdHJlbGxpcz0wIDh4OGRjdD0wIGNxbT0wIGRlYWR6b25lPTIxLDExIGZhc3RfcHNraXA9MSBjaHJvbWFfcXBfb2Zmc2V0PTAgdGhyZWFkcz0xIGxvb2thaGVhZF90aHJlYWRzPTEgc2xpY2VkX3RocmVhZHM9MCBucj0wIGRlY2ltYXRlPTEgaW50ZXJsYWNlZD0wIGJsdXJheV9jb21wYXQ9MCBjb25zdHJhaW5lZF9pbnRyYT0wIGJmcmFtZXM9MCB3ZWlnaHRwPTAga2V5aW50PTEga2V5aW50X21pbj0xIHNjZW5lY3V0PTAgaW50cmFfcmVmcmVzaD0wIHJjPWNyZiBtYnRyZWU9MCBjcmY9MjMuMCBxY29tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgaXBfcmF0aW89MS40MCBhcT0wAIAAAAFliIQ6EYoAAgbxwABBojgACCvJ114=';

// liveProbeBound 固定预算等待实际解码；超时返回不支持，不延时假装驱动准备完成。
async function liveProbeBound(operation) {
  let timer;
  try { return await Promise.race([operation(), new Promise(resolve => { timer = setTimeout(() => resolve(false), 1500); })]); }
  catch { return false; }
  finally { clearTimeout(timer); }
}

// liveProbeAudio 实际解码标准20ms静音Opus包，核真实48k/mono/960samples，不声称实际非静音输出。
async function liveProbeAudio() {
  if (typeof AudioDecoder !== 'function' || typeof EncodedAudioChunk !== 'function') return false;
  let decoder, count = 0, actual = false, failed = false;
  try {
    return await liveProbeBound(async () => {
      decoder = new AudioDecoder({error: () => { failed = true; }, output: value => {
        count += 1;
        try {
          actual = value.sampleRate === 48000 && value.numberOfChannels === 1 && value.numberOfFrames === 960;
          const pcm = new Float32Array(value.numberOfFrames);
          value.copyTo(pcm, {planeIndex: 0, format: 'f32-planar'});
          actual = actual && pcm.every(Number.isFinite);
        } catch { actual = false; }
        finally { value.close(); }
      }});
      decoder.configure({codec: 'opus', sampleRate: 48000, numberOfChannels: 1});
      decoder.decode(new EncodedAudioChunk({type: 'key', timestamp: 0, duration: 20000,
        data: new Uint8Array([0xf8, 0xff, 0xfe])}));
      await decoder.flush();
      return !failed && count === 1 && actual;
    });
  } finally { if (decoder && decoder.state !== 'closed') decoder.close(); }
}

// liveProbeVideo 实际解码固定可信IDR并核RGBA彩块；API存在/声称H264支持不等同实际成功。
async function liveProbeVideo() {
  if (typeof VideoDecoder !== 'function' || typeof EncodedVideoChunk !== 'function' ||
      typeof RTCRtpScriptTransform !== 'function' || typeof Worker !== 'function') return false;
  let decoder, count = 0, failed = false, observation = Promise.resolve(false);
  try {
    return await liveProbeBound(async () => {
      decoder = new VideoDecoder({error: () => { failed = true; }, output: frame => {
        count += 1;
        observation = (async () => {
          try {
            if (frame.displayWidth !== 32 || frame.displayHeight !== 32) return false;
            // 原IDR未指定色彩矩阵，合法解码器可选择BT.709或BT.601；只接两种固定转换色块，未知矩阵拒绝。
            const matrix = frame.colorSpace?.matrix;
            const expected = matrix === 'bt709' ? [21, 123, 221] :
              ['bt470bg', 'smpte170m'].includes(matrix) ? [30, 126, 221] : null;
            if (!expected) return false;
            const rgba = new Uint8Array(32 * 32 * 4);
            await frame.copyTo(rgba, {format: 'RGBA'});
            for (let index = 0; index < rgba.length; index += 4) {
              if (expected.some((value, channel) => Math.abs(rgba[index + channel] - value) > 3) ||
                  rgba[index + 3] !== 255) return false;
            }
            return true;
          } catch { return false; }
          finally { frame.close(); }
        })();
      }});
      decoder.configure({codec: 'avc1.42c00a', codedWidth: 32, codedHeight: 32, optimizeForLatency: true});
      const bytes = Uint8Array.from(atob(UR_LIVE_PROBE_H264), value => value.charCodeAt(0));
      decoder.decode(new EncodedVideoChunk({type: 'key', timestamp: 0, data: bytes}));
      await decoder.flush();
      const actual = await observation;
      return !failed && count === 1 && actual;
    });
  } finally { if (decoder && decoder.state !== 'closed') decoder.close(); }
}

// probeLiveReceive 实际codec自检与末端/transform实际准备另行求交集，不连接云端或读取自由URL。
async function probeLiveReceive() {
  const [audio, video] = await Promise.all([liveProbeAudio(), liveProbeVideo()]);
  return {audio, video}; // 仅格式驱动事实，不是WebRTC节目成功、扬声器听感或硬件AEC验收。
}
globalThis.probeLiveReceive = probeLiveReceive;
