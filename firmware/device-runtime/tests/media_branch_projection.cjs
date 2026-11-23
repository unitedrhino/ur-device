// 文件说明：原MCP媒体分支的独立13字段安全投影；只旁路观察已授权测试进程，不改工具或媒体状态。
'use strict';
const {StringDecoder} = require('node:string_decoder');
const {uniqueJSON} = require('./tool_trace_projection.cjs');
// keys/enums精确对应MediaOperationTraceRecord，不能把自由名称、标识、引用或错误正文投影出来。
const keys = Object.freeze(['marker', 'callSeq', 'tool', 'branch', 'target', 'referencePresent',
  'referenceValid', 'referenceSameOrigin', 'dispatched', 'upstreamObserved', 'upstreamRPCError',
  'upstreamIsError', 'outcome']);
const enums = Object.freeze(Object.fromEntries(Object.entries({
  tool: ['invite_media_playback', 'control_media_playback', 'control_output_focus',
    'invite_live_broadcast', 'control_live_track', 'recover_media_playback_operation',
    'get_media_playback_status', 'get_output_focus_status', 'get_live_broadcast_status'],
  branch: ['unavailable', 'resolvedOriginal', 'localDirectory', 'validationRejected', 'freshEffect', 'directReadOnly'],
  target: ['unavailable', 'parent', 'knownChild', 'other'],
  outcome: ['unavailable', 'returned', 'failed', 'cancelled'],
}).map(([key, values]) => [key, Object.freeze(values)])));
// limits只约束旁路内存；原业务流不截断，预算耗尽不能解释为后端没有调用。
const limits = Object.freeze({lineBytes: 16384, chunkBytes: 4096, records: 1024});
// project支持原go-zero三种日志包装，内层必须完整且唯一，不从任意自由prefix提取JSON。
function project(line) {
  if (typeof line !== 'string' || Buffer.byteLength(line) > limits.lineBytes) return null;
  try {
    let value;
    try { value = uniqueJSON(line); } catch {
      const match = /^(?:\d{4}\/\d{2}\/\d{2} \d{2}:\d{2}:\d{2} )?\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})\t([^\t]{1,64})\t([^\t]*)(?:\t[^\r\n]*)?\r?$/.exec(line);
      if (!match || !['info', 'error', 'severe'].includes(match[1].replace(/\x1b\[[0-9;]*m/g, '').trim())) return null;
      value = uniqueJSON(match[2]);
    }
    if (typeof value?.content === 'string') value = uniqueJSON(value.content);
    if (!value || typeof value !== 'object' || Array.isArray(value) ||
        Object.keys(value).length !== keys.length || keys.some(key => !Object.hasOwn(value, key)) ||
        value.marker !== 'ai_media_branch' || !Number.isSafeInteger(value.callSeq) ||
        value.callSeq < 1 || value.callSeq > limits.records) return null;
    if (keys.slice(5, 12).some(key => typeof value[key] !== 'boolean')) return null;
    if (Object.entries(enums).some(([key, values]) => !values.includes(value[key]))) return null;
    return Object.fromEntries(keys.map(key => [key, value[key]]));
  } catch { return null; }
}
// attach只在实际child环境精确开启开关时订阅双pipe，保留原独立UTF8分片和end事实。
function attach(child, emit, env = {}) {
  const enabled = env?.UR_AI_TOOL_TRACE === '1';
  const states = Object.fromEntries(['stdout', 'stderr'].map(name => [name,
    {decoder: new StringDecoder('utf8'), line: '', bytes: 0, dropping: false, ended: false}]));
  const counts = {accepted: 0, emitted: 0, discarded: 0, oversizedLines: 0,
    invalidChunks: 0, sinkFailures: 0, pendingSinks: 0, truncated: false};
  // bump统计饱和，不允许长期诊断计数溢出成为非法JSON数字。
  const bump = key => { counts[key] = Math.min(Number.MAX_SAFE_INTEGER, counts[key] + 1); };
  // flush先消费原行，再输出白名单；sink重入close不能重复记录或改变后续业务。
  function flush(name) {
    const state = states[name], record = state.dropping ? null : project(state.line);
    state.line = ''; state.bytes = 0; state.dropping = false;
    if (!record) return;
    if (counts.accepted >= limits.records) { bump('discarded'); counts.truncated = true; return; }
    bump('accepted');
    try {
      const result = emit({diagnostic: 'aiMediaBranch', stream: name, facts: record});
      if (result && typeof result.then === 'function') {
        bump('pendingSinks');
        Promise.resolve(result).then(() => { counts.pendingSinks--; bump('emitted'); },
          () => { counts.pendingSinks--; bump('sinkFailures'); counts.truncated = true; });
      } else bump('emitted');
    } catch { bump('sinkFailures'); counts.truncated = true; }
  }
  // append超长整行等待实际换行，不能把截断尾部或另一个流拼成原合法记录。
  function append(name, text) {
    const state = states[name];
    for (const part of text.split(/(\n)/)) {
      if (state.ended) break;
      if (part === '\n') { flush(name); continue; }
      if (state.dropping) continue;
      const bytes = Buffer.byteLength(part);
      if (state.bytes + bytes > limits.lineBytes) {
        state.line = ''; state.bytes = 0; state.dropping = true;
        bump('oversizedLines'); counts.truncated = true;
      } else { state.line += part; state.bytes += bytes; }
    }
  }
  // push只处理Node真实pipe的Buffer，不执行未知对象的toString，不复制或改写原大buffer。
  function push(name, bytes) {
    const state = states[name];
    if (!enabled || state.ended) return;
    if (!Buffer.isBuffer(bytes)) { bump('invalidChunks'); counts.truncated = true; return; }
    for (let offset = 0; offset < bytes.length && !state.ended; offset += limits.chunkBytes)
      append(name, state.decoder.write(bytes.subarray(offset, offset + limits.chunkBytes)));
  }
  // end只从原pipe end取得终态；手工close终止观察，但不关闭child/stdout/stderr或发送信号。
  function end(name) {
    const state = states[name];
    if (state.ended) return;
    if (enabled) { append(name, state.decoder.end()); if (state.line || state.dropping) flush(name); }
    state.ended = true; state.line = ''; state.bytes = 0; state.dropping = false;
  }
  const observer = {close() { end('stdout'); end('stderr'); }, statistics() {
    return {enabled, ...counts, ended: Object.fromEntries(Object.entries(states).map(([name, state]) => [name, state.ended]))};
  }};
  if (enabled) for (const name of ['stdout', 'stderr']) {
    child[name].on('data', bytes => push(name, bytes));
    child[name].once('end', () => end(name));
  }
  return observer;
}
module.exports = {keys, enums, limits, project, attach};
