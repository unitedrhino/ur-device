// 文件说明：真实C++文件设备日志的逐轮验收器；不启动模型、不接收凭据或音频正文。
'use strict';

// VoiceReport 按轮检查实际解码、动作和工具通知，避免首轮成功掩盖后续失败。
class VoiceReport {
  // latencyBasis 显式区分文字提交与语音末帧，默认保持既有语音报告口径。
  constructor(rounds, expectedVolume, latencyBasis = 'lastNonSilentSentFrame') {
    if (!Number.isInteger(rounds) || rounds < 1 || rounds > 30 ||
        (expectedVolume !== undefined && (!Number.isInteger(expectedVolume) || expectedVolume < 0 || expectedVolume > 100)) ||
        !['lastNonSilentSentFrame', 'textSubmitted'].includes(latencyBasis)) {
      throw new Error('invalid_expectation');
    }
    this.expected = rounds;
    this.latencyBasis = latencyBasis;
    this.volume = expectedVolume;
    this.rounds = [];
    this.current = { actions: 0, starts: 0, results: 0, samples: 0, wrongVolume: false };
    this.failed = false;
  }

  // accept 只读取固定诊断字段；识别/回复正文及凭据不进入报告。
  accept(event) {
    if (!event || typeof event !== 'object') return;
    if (event.event === 'softwareActionCompleted') {
      if (event.code === 200) this.current.actions++;
      if (this.volume !== undefined && (event.code !== 200 || event.volume !== this.volume)) this.current.wrongVolume = true;
    } else if (event.event === 'content') {
      if (event.method === 'toolCallStart') this.current.starts++;
      if (event.method === 'toolCallResult') this.current.results++;
    } else if (event.event === 'virtualPlaybackSaved') {
      if (event.round !== this.rounds.length + 1 || !Number.isSafeInteger(event.samples) || event.samples <= 0) this.failed = true;
      else this.current.samples += event.samples;
    } else if (event.event === 'roundCompleted') {
      const timing = ['firstTextLatencyMs', 'firstNetworkLatencyMs', 'firstPlayableLatencyMs'];
      if (this.rounds.length >= this.expected || event.round !== this.rounds.length + 1 ||
          event.latencyBasis !== this.latencyBasis ||
          timing.some(key => !Number.isFinite(event[key]) || event[key] < 0)) this.failed = true;
      if (this.rounds.length < this.expected) {
        this.rounds.push({ ...this.current, firstTextMs: event.firstTextLatencyMs,
          firstNetworkMs: event.firstNetworkLatencyMs, firstPlayableMs: event.firstPlayableLatencyMs });
      }
      this.current = { actions: 0, starts: 0, results: 0, samples: 0, wrongVolume: false };
    }
  }

  // finish 子进程失败、少轮、无解码音频或任一轮缺少动作/配对通知均不通过。
  finish(exitCode, requireToolEvents = false) {
    const passed = exitCode === 0 && !this.failed && this.rounds.length === this.expected &&
      this.rounds.every(round => round.samples > 0 && !round.wrongVolume &&
        (this.volume === undefined || round.actions > 0) &&
        (!requireToolEvents || (round.starts > 0 && round.starts === round.results)));
    return { passed, latencyBasis: this.latencyBasis, rounds: this.rounds };
  }
}
module.exports = { VoiceReport };

// 命令行只复核既有脱敏JSONL；退出码必须来自实际设备进程，不自动执行真实API。
if (require.main === module) {
  const fs = require('node:fs');
  const readline = require('node:readline');
  const [file, rounds, volume, exitCode, tools] = process.argv.slice(2);
  if (!file || !/^\d+$/.test(rounds || '') || !/^\d+$/.test(exitCode || '') ||
      (volume !== '-' && !/^\d+$/.test(volume || '')) || (tools !== undefined && tools !== '--tools')) {
    console.error('用法：node voice_live_report.cjs LOG.jsonl ROUNDS VOLUME或- 实际退出码 [--tools]');
    process.exitCode = 2;
  } else {
    try {
      const report = new VoiceReport(Number(rounds), volume === '-' ? undefined : Number(volume));
      const input = fs.createReadStream(file);
      const lines = readline.createInterface({ input, crlfDelay: Infinity });
      input.on('error', () => { process.exitCode = 2; lines.close(); });
      lines.on('line', line => {
        if (line.length > 65536) { report.failed = true; return; }
        try { report.accept(JSON.parse(line)); } catch { /* 构建门禁提示不是设备事件。 */ }
      });
      lines.on('close', () => {
        if (process.exitCode) return;
        const result = report.finish(Number(exitCode), tools === '--tools');
        console.log(JSON.stringify(result));
        process.exitCode = result.passed ? 0 : 1;
      });
    } catch {
      console.error('验收配置无效');
      process.exitCode = 2;
    }
  }
}
