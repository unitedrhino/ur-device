// 文件说明：严格互解宿主本机UDP预信令计数，不导出nonce、回复身份、音频或自由错误。
'use strict';

// presignalDiagnostics仅接纳固定顺序、uint32与完整性关系；无法证实的输入返回null供原清理继续。
function presignalDiagnostics(stdout) {
  if (!Buffer.isBuffer(stdout) || stdout.length > 8 * 1024 * 1024) return null;
  let text;
  try { text = new TextDecoder('utf-8', { fatal: true }).decode(stdout); } catch { return null; }
  const lines = text.split('\n').filter(line => line.startsWith('host_udp_presignal='));
  if (lines.length === 0 || lines.length > 1024) return null;
  const records = [];
  let previous = 0;
  for (const line of lines) {
    if (line.length > 512) return null;
    const match = /^host_udp_presignal=\{"kind":"udpPresignalDiagnostic","observation":(0|[1-9]\d{0,9}),"received":(0|[1-9]\d{0,9}),"evicted":(0|[1-9]\d{0,9}),"retained":(0|[1-9]\d{0,9}),"complete":(true|false)\}\r?$/.exec(line);
    if (!match) return null;
    const [observation, received, evicted, retained] = match.slice(1, 5).map(Number);
    if (![observation, received, evicted, retained].every(value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff) ||
      observation <= previous || retained > 16 || evicted > received) return null;
    const complete = match[5] === 'true';
    if (complete && received !== evicted + retained) return null;
    records.push(Object.freeze({ kind: 'udpPresignalDiagnostic', observation, received, evicted, retained, complete }));
    previous = observation;
  }
  return Object.freeze(records);
}
module.exports = { presignalDiagnostics };
