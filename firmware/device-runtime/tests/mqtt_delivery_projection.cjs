// 文件说明：显式MQTT投递诊断的只读安全投影；分为logger来源/固定字段校验和有界双流观察，不改变原业务pipe。
'use strict';
const {TextDecoder} = require('node:util');
const {uniqueJSON} = require('./tool_trace_projection.cjs');

// limits 与生产Hook有限预算对应，字节上限限制诊断内存而不截断原业务数据。
const limits = Object.freeze({lineBytes: 16384, chunkBytes: 4096, records: 256});
// keys 固定生产12字段；任何额外/缺失/重复key拒绝整条，不能选择性脱敏后冒充事实。
const keys = Object.freeze(['marker', 'stage', 'method', 'messageSeq', 'messageSeqKnown', 'payloadBytes',
  'wireBytes', 'targetMatched', 'socketWrites', 'pubackReads', 'pubackCompleted', 'truncated']);
// enums 只接生产允许的阶段/方法，不反射自由方法或日志正文。
const enums = Object.freeze({stage: Object.freeze(['brokerAccepted', 'socketWritten', 'pubackRead', 'pubackCompleted']),
  method: Object.freeze(['unknown', 'requestAck', 'sessionCreated', 'sessionEstablished', 'sessionEnded', 'sessionError',
    'respCreated', 'respSttDelta', 'respSttDone', 'respTextDelta', 'respTextDone', 'respEmotion', 'respAudioStart',
    'respAudioDone', 'audioSpeechStarted', 'audioSpeechFinished', 'audioSpeechInterrupted', 'respDone',
    'toolCallStart', 'toolCallResult', 'toolCallExecute'])});
// timestamp/caller 只识别实际默认go-zero来源槽；正文嵌套或自由前缀不能提供来源证据。
const timestamp = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})$/;
const caller = /^hook\/delivery_trace\.go:[1-9][0-9]*$/;

// wellFormed 检查原字符串的成对UTF16，避免Buffer替换非法surrogate后反而形成可接纳JSON。
function wellFormed(value) {
  for (let index = 0; index < value.length; index++) {
    const unit = value.charCodeAt(index);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const next = value.charCodeAt(++index);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
  }
  return true;
}
// parse 复用严格重复key/depth校验并拒绝转义后的非法字符串；不递归提升嵌套content为日志来源。
function parse(source) {
  const value = uniqueJSON(source), pending = [value];
  while (pending.length) {
    const item = pending.pop();
    if (typeof item === 'string' && !wellFormed(item)) throw Error('mqtt_trace_unicode');
    if (item && typeof item === 'object') {
      for (const [key, entry] of Object.entries(item)) {
        if (!wellFormed(key)) throw Error('mqtt_trace_unicode');
        pending.push(entry);
      }
    }
  }
  return value;
}
// record 只重建固定字段，未知水位必须0；四阶段不得用ACK推断socket或业务已消费。
function record(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== keys.length ||
      keys.some(key => !Object.hasOwn(value, key)) || value.marker !== 'UR_MQTT_DELIVERY_TRACE') return null;
  for (const key of ['messageSeqKnown', 'targetMatched', 'truncated']) if (typeof value[key] !== 'boolean') return null;
  if (!enums.stage.includes(value.stage) || !enums.method.includes(value.method) || !Number.isSafeInteger(value.messageSeq) ||
      value.messageSeq < 0 || (value.messageSeqKnown ? value.messageSeq === 0 : value.messageSeq !== 0)) return null;
  for (const [key, maximum] of Object.entries({payloadBytes: 65536, wireBytes: 131072,
    socketWrites: 256, pubackReads: 256, pubackCompleted: 256}))
    if (!Number.isSafeInteger(value[key]) || value[key] < 0 || value[key] > maximum) return null;
  if (value.stage !== 'socketWritten' && value.wireBytes !== 0) return null;
  return Object.fromEntries(keys.map(key => [key, value[key]]));
}
// project 仅接有原logger来源的JSON或TAB行；仅一次解析content，裸记录/任意递归包装均拒绝。
function project(line) {
  if (typeof line !== 'string' || !wellFormed(line) || Buffer.byteLength(line) > limits.lineBytes) return null;
  try {
    let outer;
    try {outer = parse(line);} catch {
      const slots = line.replace(/\r$/, '').split('\t');
      const time = slots[0].replace(/^\d{4}\/\d{2}\/\d{2} \d{2}:\d{2}:\d{2} /, '');
      if (!timestamp.test(time) || slots[1]?.replace(/\x1b\[[0-9;]*m/g, '').trim() !== 'info' || slots.length < 4) return null;
      const callers = slots.slice(3).filter(slot => slot.startsWith('caller='));
      if (callers.length !== 1 || !caller.test(callers[0].slice(7))) return null;
      return record(parse(slots[2]));
    }
    if (!outer || typeof outer !== 'object' || Array.isArray(outer) || typeof outer['@timestamp'] !== 'string' || !timestamp.test(outer['@timestamp']) ||
        outer.level !== 'info' || typeof outer.caller !== 'string' || !caller.test(outer.caller) ||
        typeof outer.content !== 'string') return null;
    return record(parse(outer.content));
  } catch {return null;}
}

// streams 仅实际child显式开关启用；两流独立严格UTF8，诊断错误/预算不影响原业务输入。
function streams(emit, actualChildEnv) {
  const enabled = !!actualChildEnv && actualChildEnv.UR_MQTT_DELIVERY_TRACE === '1';
  const states = Object.fromEntries(['stdout', 'stderr'].map(name => [name, {
    decoder: new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}), line: '', bytes: 0, dropping: false, ended: false,
    totalBytes: 0, lineCount: 0, oversizedLines: 0, rejectedLines: 0, invalidChunks: 0}]));
  const totals = {acceptedRecords: 0, emittedRecords: 0, discardedRecords: 0, sinkFailures: 0, pendingSinks: 0,
    projectionFailures: 0, invalidStreams: 0, truncated: false};
  let closed = false; // 主动停止不等于真实流end，不能收录未终结尾行。
  // bump 统计安全整数饱和，不保留自由数据或错误消息。
  const bump = (object, key, amount = 1) => {object[key] = Math.min(Number.MAX_SAFE_INTEGER, object[key] + amount);};
  // reset 只清理当前诊断行/解码状态，不改变原pipe或上一条业务事实。
  function reset(state) {state.line = ''; state.bytes = 0; state.dropping = false; state.decoder = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});}
  // publish 在真实换行/end后收录；先清状态再调用sink，重入close不会重复收录。
  function publish(name) {
    const state = states[name], facts = state.dropping ? null : project(state.line);
    bump(state, 'lineCount');
    if (!facts) bump(state, 'rejectedLines');
    reset(state);
    if (!facts) return;
    if (totals.acceptedRecords >= limits.records) {bump(totals, 'discardedRecords'); totals.truncated = true; return;}
    bump(totals, 'acceptedRecords');
    try {
      if (typeof emit !== 'function') throw Error('mqtt_trace_sink');
      const result = emit({diagnostic: 'mqttDeliveryTrace', stream: name, facts});
      if (result && typeof result.then === 'function') {
        bump(totals, 'pendingSinks');
        Promise.resolve(result).then(() => {totals.pendingSinks--; bump(totals, 'emittedRecords');},
          () => {totals.pendingSinks--; bump(totals, 'sinkFailures'); totals.truncated = true;});
      } else bump(totals, 'emittedRecords');
    } catch {bump(totals, 'sinkFailures'); totals.truncated = true;}
  }
  // append 按原字节计界，UTF8失败整行丢弃到真实换行；下一行独立恢复，不接纳替换字符补造内容。
  function append(name, piece, ending) {
    const state = states[name];
    if (!state.dropping) {
      state.bytes += piece.length;
      if (state.bytes > limits.lineBytes) {state.line = ''; state.dropping = true; bump(state, 'oversizedLines'); totals.truncated = true;}
      else try {state.line += state.decoder.decode(piece, {stream: !ending});}
      catch {state.line = ''; state.dropping = true; bump(state, 'invalidChunks'); totals.truncated = true;}
    }
    if (ending) publish(name);
  }
  // push 以至多4096字节分片观察，原Buffer引用/内容均不变；不调用未知对象转换方法。
  function push(name, bytes) {
    const state = states[name];
    if (!enabled || closed || state.ended) return;
    try {
      if (typeof bytes === 'string') {
        if (!wellFormed(bytes)) {state.dropping = true; state.line = ''; bump(state, 'invalidChunks'); totals.truncated = true; return;}
        for (let offset = 0; offset < bytes.length && !closed && !state.ended;) {
          let end = Math.min(offset + 1024, bytes.length);
          if (end < bytes.length && /[\ud800-\udbff]/.test(bytes[end - 1])) end++;
          push(name, Buffer.from(bytes.slice(offset, end))); offset = end;
        }
        return;
      }
      if (!Buffer.isBuffer(bytes)) {bump(state, 'invalidChunks'); state.dropping = true; state.line = ''; totals.truncated = true; return;}
      bump(state, 'totalBytes', bytes.length);
      for (let offset = 0; offset < bytes.length && !closed && !state.ended; offset += limits.chunkBytes) {
        const chunk = bytes.subarray(offset, offset + limits.chunkBytes);
        let start = 0;
        for (let index = 0; index < chunk.length && !closed && !state.ended; index++) if (chunk[index] === 10) {
          append(name, chunk.subarray(start, index), true); start = index + 1;
        }
        if (!closed && !state.ended && start < chunk.length) append(name, chunk.subarray(start), false);
      }
    } catch {bump(totals, 'projectionFailures'); state.line = ''; state.dropping = true; totals.truncated = true;}
  }
  // end 仅真实流end可排空无换行尾行；child.close或observer.close不能伪造end。
  function end(name) {
    if (!Object.hasOwn(states, name)) {bump(totals, 'invalidStreams'); return;}
    const state = states[name];
    if (closed || state.ended) return;
    if (enabled && (state.bytes || state.dropping)) append(name, Buffer.alloc(0), true);
    reset(state); state.ended = true;
  }
  // statistics 仅返回固定布尔/计数，未收到记录或截断不证明消息不存在。
  function statistics() {return {enabled, closed, ...totals, streams: Object.fromEntries(Object.entries(states).map(([name, state]) => [name,
    {totalBytes: state.totalBytes, lineCount: state.lineCount, oversizedLines: state.oversizedLines,
      rejectedLines: state.rejectedLines, invalidChunks: state.invalidChunks, ended: state.ended}]))};}
  // close 只停止观察并丢弃未终结行，不触碰子进程或假造stdio已排空。
  function close() {if (closed) return; closed = true; for (const state of Object.values(states)) reset(state);}
  return {stdout: bytes => push('stdout', bytes), stderr: bytes => push('stderr', bytes), end, close, statistics};
}
// attach 旁路观察原child真实pipe，缺省不借父环境；仅原end事件收尾，不修改child参数或退出码。
function attach(child, emit, actualChildEnv) {
  const observer = streams(emit, actualChildEnv);
  if (!observer.statistics().enabled) return observer;
  for (const name of ['stdout', 'stderr']) {
    child[name].on('data', observer[name]);
    child[name].once('end', () => observer.end(name));
  }
  return observer;
}
module.exports = {limits, keys, enums, project, streams, attach};
