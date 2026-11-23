// 文件说明：显式AI工具trace的固定安全日志投影；模块分为严格JSON/schema校验与独立双流收录，不截断/改写原业务日志。
'use strict';
const {StringDecoder} = require('node:string_decoder');

// limits 约束每行、解码分片、两流合计诊断量；2048对应后端至多256次trace、每次至多8条记录。
const limits = Object.freeze({lineBytes: 16384, chunkBytes: 4096, records: 2048, jsonDepth: 32});
// keys 固定15字段合同；任意额外/缺失/重复字段均不提升为工具事实。
const keys = Object.freeze(['marker', 'traceSeq', 'stage', 'known', 'truncated',
  'inviteMediaPlaybackPresent', 'controlOutputFocusPresent', 'toolsCount', 'accumulatedCalls',
  'parsedCount', 'toolFragments', 'mergeErrors', 'toolChoice', 'finishReason', 'end']);
// enums 仅匹配生产投影声明值，不反射自由阶段、函数名、finish正文或异常信息。
const enums = Object.freeze(Object.fromEntries(Object.entries({stage: ['boundTools', 'httpRequest', 'streamResult', 'parsedTools'],
  toolChoice: ['unavailable', 'absent', 'null', 'auto', 'none', 'required', 'named', 'allowed_tools', 'invalid'],
  finishReason: ['absent', 'stop', 'length', 'tool_calls', 'function_call', 'content_filter', 'other'],
  end: ['absent', 'eof', 'error', 'canceled', 'panic']}).map(([key, values]) => [key, Object.freeze(values)])));
// booleanKeys/countLimits 固定布尔和整数边界，不接受字符串转换、负数、溢出或非有限值。
const booleanKeys = ['known', 'truncated', 'inviteMediaPlaybackPresent', 'controlOutputFocusPresent'];
const countLimits = {traceSeq: [1, 256], toolsCount: [0, 128], accumulatedCalls: [0, 128],
  parsedCount: [0, 128], toolFragments: [0, 65536], mergeErrors: [0, 65536]};

// uniqueJSON 先由标准JSON解析检查语法，再逐层查解码后的重复key；不允许last-key-wins掩盖私有/畸形字段。
function uniqueJSON(source) {
  const value = JSON.parse(source);
  const stack = []; // 只存有界嵌套层的key集合；字符串值整段跳过，不把content中的JSON当外层字段。
  const quoted = /"(?:[^"\\\u0000-\u001f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"/y;
  for (let index = 0; index < source.length; index++) {
    const character = source[index];
    if (character === '{' || character === '[') {
      stack.push(character === '{' ? new Set() : null);
      if (stack.length > limits.jsonDepth) throw Error('trace_json_depth');
    } else if (character === '}' || character === ']') stack.pop();
    else if (character === '"') {
      quoted.lastIndex = index;
      const token = quoted.exec(source);
      if (!token) throw Error('trace_json_string');
      const end = quoted.lastIndex;
      let next = end;
      while (/[\t\n\r ]/.test(source[next] || '') && next < source.length) next++;
      if (source[next] === ':') {
        const names = stack.at(-1), name = JSON.parse(token[0]);
        if (!(names instanceof Set) || names.has(name)) throw Error('trace_duplicate_key');
        names.add(name);
      }
      index = end - 1;
    }
  }
  return value;
}

// record 只返回逐字段重建的白名单；未知额外key或数值/枚举畸形整条拒绝，不保留raw对象。
function record(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).length !== keys.length || keys.some(key => !Object.hasOwn(value, key)) ||
      value.marker !== 'ai_tool_trace') return null;
  if (booleanKeys.some(key => typeof value[key] !== 'boolean')) return null;
  for (const [key, [minimum, maximum]] of Object.entries(countLimits))
    if (!Number.isSafeInteger(value[key]) || value[key] < minimum || value[key] > maximum) return null;
  for (const [key, allowed] of Object.entries(enums)) if (!allowed.includes(value[key])) return null;
  return Object.fromEntries(keys.map(key => [key, value[key]]));
}

// project 接受纯payload、go-zero JSON content或已知plain TAB正文；外层元数据可忽略，但内层严格15key且无自由prefix。
function project(line) {
  if (typeof line !== 'string' || Buffer.byteLength(line, 'utf8') > limits.lineBytes) return null;
  try {
    let value;
    try {value = uniqueJSON(line);} catch {
      // 级别槽允许go-zero既有ANSI装饰；正文/前缀的任意ANSI或自由文本不作为trace。
      const plain = /^(?:\d{4}\/\d{2}\/\d{2} \d{2}:\d{2}:\d{2} )?\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})\t([^\t]{1,64})\t([^\t]*)(?:\t[^\r\n]*)?\r?$/.exec(line);
      if (!plain || !['info', 'error', 'severe'].includes(plain[1].replace(/\x1b\[[0-9;]*m/g, '').trim())) return null;
      value = uniqueJSON(plain[2]);
    }
    if (value && typeof value.content === 'string') value = uniqueJSON(value.content);
    return record(value);
  } catch {return null;}
}

// streams 默认关闭；显式UR_AI_TOOL_TRACE=1才读双流，每流独立UTF8/行状态，sink异常和诊断输入错误不抛入原转发。
function streams(emit, env = process.env) {
  const enabled = !!env && env.UR_AI_TOOL_TRACE === '1';
  const states = Object.fromEntries(['stdout', 'stderr'].map(name => [name, {decoder: new StringDecoder('utf8'),
    line: '', bytes: 0, dropping: false, ended: false, totalBytes: 0, lineCount: 0,
    oversizedLines: 0, rejectedLines: 0, invalidChunks: 0}]));
  const totals = {acceptedRecords: 0, emittedRecords: 0, discardedRecords: 0, sinkFailures: 0, pendingSinks: 0,
    projectionFailures: 0, invalidStreams: 0, truncated: false};
  // bump 统计饱和到safeint，任何计数均不成为模型没有工具或业务成功的判据。
  const bump = (object, key, amount = 1) => {object[key] = Math.min(Number.MAX_SAFE_INTEGER, object[key] + amount);};
  // publish 只输出有界固定record；达到cap后仍解析/统计丢弃，不把缺失后段当后端没工具。
  function publish(name) {
    const state = states[name];
    bump(state, 'lineCount');
    const facts = state.dropping ? null : project(state.line);
    state.line = ''; state.bytes = 0; state.dropping = false; // 先消耗当前行，sink若同步close不能重复收录同一条事实。
    if (facts) {
      if (totals.acceptedRecords >= limits.records) {bump(totals, 'discardedRecords'); totals.truncated = true;}
      else {
        bump(totals, 'acceptedRecords');
        try {
          if (typeof emit !== 'function') throw Error('trace_sink');
          const result = emit({diagnostic: 'aiToolTrace', stream: name, facts});
          // 异步sink失败也只成为诊断缺失；不await、不改原业务流回调，未完成sink明确保留pending计数。
          if (result && typeof result.then === 'function') {
            bump(totals, 'pendingSinks');
            Promise.resolve(result).then(() => {totals.pendingSinks--; bump(totals, 'emittedRecords');},
              () => {totals.pendingSinks--; bump(totals, 'sinkFailures'); totals.truncated = true;});
          } else bump(totals, 'emittedRecords');
        }
        catch {bump(totals, 'sinkFailures'); totals.truncated = true;}
      }
    } else if (!state.dropping) bump(state, 'rejectedLines');
  }
  // append 超长整行丢到真实换行，绝不把截断尾部或另一流续片伪装成合法trace。
  function append(name, text) {
    const state = states[name];
    for (const part of text.split(/(\n)/)) {
      if (state.ended) break;
      if (part === '\n') {publish(name); continue;}
      if (state.dropping) continue;
      const size = Buffer.byteLength(part, 'utf8');
      if (state.bytes + size > limits.lineBytes) {
        state.line = ''; state.bytes = 0; state.dropping = true; bump(state, 'oversizedLines'); totals.truncated = true;
      } else {state.line += part; state.bytes += size;}
    }
  }
  // push 按4096字节解码Buffer；字符串按小片转码以避免额外大Buffer，不调用未知对象的toString/valueOf。
  function push(name, bytes) {
    const state = states[name];
    if (!enabled || state.ended) return;
    try {
      if (Buffer.isBuffer(bytes)) {
        bump(state, 'totalBytes', bytes.length);
        for (let offset = 0; offset < bytes.length && !state.ended; offset += limits.chunkBytes)
          append(name, state.decoder.write(bytes.subarray(offset, offset + limits.chunkBytes)));
      } else if (typeof bytes === 'string') {
        bump(state, 'totalBytes', Buffer.byteLength(bytes, 'utf8'));
        for (let offset = 0; offset < bytes.length && !state.ended;) {
          let end = Math.min(offset + 1024, bytes.length);
          if (end < bytes.length && /[\ud800-\udbff]/.test(bytes[end - 1]) && /[\udc00-\udfff]/.test(bytes[end])) end++;
          append(name, state.decoder.write(Buffer.from(bytes.slice(offset, end)))); offset = end;
        }
      } else {bump(state, 'invalidChunks'); totals.truncated = true;}
    } catch {bump(totals, 'projectionFailures'); state.line = ''; state.bytes = 0; state.dropping = true; totals.truncated = true;}
  }
  // end 原流结束才收录无换行末尾，幂等；close仅结束诊断观察器，不关闭child或改原业务流。
  function end(name) {
    if (!Object.hasOwn(states, name)) {bump(totals, 'invalidStreams'); return;}
    const state = states[name];
    if (state.ended) return;
    try {if (enabled) {append(name, state.decoder.end()); if (state.line || state.dropping) publish(name);}}
    catch {bump(totals, 'projectionFailures'); totals.truncated = true;}
    state.line = ''; state.bytes = 0; state.dropping = false; state.ended = true;
  }
  // statistics 只透出固定bool/count，显式记录诊断缺失；不保存raw/body、身份、header或错误消息。
  function statistics() {return {enabled, ...totals, streams: Object.fromEntries(Object.entries(states).map(([name, state]) => [name,
    {totalBytes: state.totalBytes, lineCount: state.lineCount, oversizedLines: state.oversizedLines,
      rejectedLines: state.rejectedLines, invalidChunks: state.invalidChunks, ended: state.ended}]))};}
  return {stdout: bytes => push('stdout', bytes), stderr: bytes => push('stderr', bytes), end,
    close: () => {end('stdout'); end('stderr');}, statistics};
}
// attach 将固定投影旁路接入已有child双pipe，emit只收安全值；actualChildEnv缺省关闭，不借父进程开关。
// 返回原观察器供读取统计，不修改child参数/数据/退出码；仅真实stdio end收尾，child close不代表流已排空。
function attach(child, emit, actualChildEnv) {
  const observer = streams(emit, actualChildEnv === undefined ? {} : actualChildEnv);
  if (!observer.statistics().enabled) return observer;
  for (const name of ['stdout', 'stderr']) {
    child[name].on('data', observer[name]);
    child[name].once('end', () => observer.end(name));
  }
  return observer;
}
module.exports = {limits, keys, enums, project, streams, attach, uniqueJSON};
