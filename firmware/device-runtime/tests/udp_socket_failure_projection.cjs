// 文件说明：原生UDP接收失败stderr固定行的有界只读投影；未观察不能证明无失败，不解释播放或修改收发。
'use strict';
const {TextDecoder, types} = require('node:util');
const {uniqueJSON} = require('./tool_trace_projection.cjs');

// reasons 精确对应原实物socket闭集合，unknown是显式事实而非未观察的默认结果。
const reasons = Object.freeze(['notConnected', 'peerRefused', 'peerReset', 'unreachable',
    'permissionDenied', 'invalidSocket', 'otherSocketError', 'unknown']);
// limits 只约束旁路观察，不截断原pipe；超界标记不完整，不能用它论证业务事实不存在。
const limits = Object.freeze({chunkBytes: 256, lineBytes: 1024, lines: 1024, records: 16,
    totalBytes: 1048576});

// project 只接受Main原两字段JSON，重复键/额外字段/任意日志包装均整行拒绝，不反射错误正文。
function project(line) {
    if (typeof line !== 'string' || Buffer.byteLength(line) > limits.lineBytes) return null;
    try {
        const value = uniqueJSON(line);
        if (!value || typeof value !== 'object' || Array.isArray(value) ||
            Object.keys(value).length !== 2 || !Object.hasOwn(value, 'kind') ||
            !Object.hasOwn(value, 'reason') || value.kind !== 'udpSocketReceiveFailureDiagnostic' ||
            !reasons.includes(value.reason)) return null;
        return {kind: 'udpSocketReceiveFailureDiagnostic', reason: value.reason};
    } catch { return null; }
}

// create 仅实际子进程精确开启原诊断开关才观察；调用方自行把stderr data/end接到push/end，不自动监听。
function create(actualChildEnv) {
    const enabled = actualChildEnv?.UR_DEVICE_CONTEXT_DIAGNOSTIC === '1';
    const counts = {bytes: 0, lines: 0, records: 0, rejected: 0, duplicates: 0,
        contradictions: 0, oversizedLines: 0, invalidChunks: 0, afterEndPushes: 0};
    let decoder = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});
    let line = '', lineBytes = 0, dropping = false, first = null;
    let ended = false, closed = false, truncated = false, exhausted = false;
    // bump 诊断计数安全饱和，不保存自由输入或异常对象。
    const bump = key => {counts[key] = Math.min(Number.MAX_SAFE_INTEGER, counts[key] + 1);};
    // reset 只释放当前短行及解码残余，不改变已经观察的首次事实。
    function reset() {
        line = ''; lineBytes = 0; dropping = false;
        decoder = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true});
    }
    // flush 消费真实换行或实际end的尾行；首因只收一次，重复/矛盾后不能再给可靠reason。
    function flush() {
        if (counts.lines >= limits.lines) {truncated = exhausted = true; reset(); return;}
        bump('lines');
        let value = null;
        if (!dropping) {
            try {line += decoder.decode(); value = project(line);}
            catch {bump('invalidChunks'); truncated = true;}
        }
        reset();
        if (!value) {bump('rejected'); return;}
        if (counts.records >= limits.records) {truncated = exhausted = true; return;}
        bump('records');
        if (first === null) first = value.reason;
        else bump(first === value.reason ? 'duplicates' : 'contradictions');
    }
    // append 以原UTF8字节计界，畸形/超长整行丢弃到实际换行，不能把残尾拼成合法记录。
    function append(piece) {
        if (dropping) return;
        lineBytes += piece.length;
        if (lineBytes > limits.lineBytes) {
            line = ''; dropping = true; bump('oversizedLines'); truncated = true; return;
        }
        try {line += decoder.decode(piece, {stream: true});}
        catch {line = ''; dropping = true; bump('invalidChunks'); truncated = true;}
    }
    // push 只接Node真实Buffer，不执行未知对象转换；每片/整段预算都有限且不修改原Buffer。
    function push(bytes) {
        if (ended || closed) {bump('afterEndPushes'); return;}
        if (!enabled || exhausted) return;
        if (types.isProxy(bytes) || !Buffer.isBuffer(bytes)) {
            line = ''; dropping = true; bump('invalidChunks'); truncated = true; return;
        }
        const available = Math.min(bytes.length, limits.totalBytes - counts.bytes);
        counts.bytes += available;
        for (let offset = 0; offset < available && !exhausted; offset += limits.chunkBytes) {
            const part = bytes.subarray(offset, Math.min(offset + limits.chunkBytes, available));
            let start = 0;
            for (let index = 0; index < part.length && !exhausted; index++) {
                if (part[index] !== 10) continue;
                append(part.subarray(start, index)); flush(); start = index + 1;
            }
            if (!exhausted && start < part.length) append(part.subarray(start));
        }
        if (available < bytes.length) {truncated = exhausted = true; reset();}
    }
    // end 仅实际stderr end可消费无换行尾行；close主动停止不能伪造此事实。
    function end() {
        if (ended || closed) return;
        if (enabled && !exhausted && (lineBytes || dropping)) flush();
        reset(); ended = true;
    }
    // close 丢弃未终结行，仅停止观察，不关闭child/pipe或变更退出码。
    function close() {if (!closed) {reset(); closed = true;}}
    // statistics 只返回固定布尔/计数/枚举；重复或矛盾令reason不可用，observed/unknown/ended互不替代。
    function statistics() {
        const consistent = counts.duplicates === 0 && counts.contradictions === 0;
        return {enabled, ended, closed, observed: first !== null, unknown: first === 'unknown',
            reason: consistent ? first : null, consistent, truncated, ...counts};
    }
    return {push, end, close, statistics};
}
module.exports = {reasons, limits, project, create};
