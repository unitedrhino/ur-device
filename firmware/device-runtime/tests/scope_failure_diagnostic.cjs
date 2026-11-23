// 文件说明：只接纳宿主原候选焦点首因的五字段闭集；不导出身份、正文或自由错误。
'use strict';

// scopePairs 与C++固定枚举一一互解；未知组合不可通过单独字段白名单洗白。
const scopePairs = new Set([
  'candidate/bindingMissing', 'candidate/endpointMissing',
  'focusEndpoint/stateUnavailable', 'focusEndpoint/clockInvalid', 'focusEndpoint/sessionMissing',
  'focusEngine/engineClosed', 'focusEngine/operationPending', 'focusEngine/leaseMissing',
  'focusEngine/receiptUndelivered', 'focusEngine/stateUnavailable', 'focusEngine/resourcesMismatch',
  'focusEngine/clockInvalid', 'focusEngine/leaseExpired',
  'authority/permissionDenied', 'candidateHAL/stateUnavailable', 'candidateHAL/clockInvalid',
  'authority/currentUnavailable', 'authority/permitInvalid', 'authority/operationDenied',
  'duplexDriver/stateUnavailable', 'duplexDriver/outputUnconfirmed',
  'duplexDriver/inputUnconfirmed', 'duplexDriver/otherPathUnconfirmed',
]);

// scopeFailureCategory 精确解码原宿主固定顺序与字节；无法验证的输出保持未知，不阻塞资源清理。
function scopeFailureCategory(stdout) {
  if (!Buffer.isBuffer(stdout) || stdout.length > 8 * 1024 * 1024) return null;
  let text;
  try { text = new TextDecoder('utf-8', { fatal: true }).decode(stdout); } catch { return null; }
  const lines = text.split('\n').filter(line => line.startsWith('host_scope_failure='));
  if (lines.length !== 1 || lines[0].length > 512) return null;
  const match = /^host_scope_failure=\{"kind":"scopeFailureDiagnostic","origin":"candidateGate","layer":"([A-Za-z]+)","predicate":"([A-Za-z]+)","observation":1\}\r?$/.exec(lines[0]);
  if (!match || !scopePairs.has(`${match[1]}/${match[2]}`)) return null;
  return Object.freeze({ kind: 'scopeFailureDiagnostic', origin: 'candidateGate',
    layer: match[1], predicate: match[2], observation: 1 });
}
module.exports = { scopeFailureCategory };
