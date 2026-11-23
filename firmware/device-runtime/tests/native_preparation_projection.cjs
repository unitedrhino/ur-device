// 文件说明：平台临时资源准备失败的两字段只读投影；无自由正文、设备身份或自动重试语义。
'use strict';
const {uniqueJSON} = require('./tool_trace_projection.cjs');

// reasons 与后台准备失败闭集一致；标签只说明失败谓词，不证明资源许可或可安全重试。
const reasons = Object.freeze(['aiPendingOtherCall', 'aiPreviousNotResumed', 'aiConnectionUnavailable',
    'aiOriginalInvalid', 'aiRetainOrigin', 'aiRetainPending', 'aiRetainCapacity', 'aiRetainExistingCall',
    'aiRetainConnection', 'aiRetainPreviousCall', 'aiRetainLeaseShape', 'aiRetainWatermark',
    'aiRetainLeaseInvalid', 'aiRetainAuthority', 'humanLeaseInvalid', 'humanLeaseNotReleased',
    'nativeCallCapacity']);
// marker 是独立失败类别，不修改既有13字段建立过程观察契约。
const marker = 'UR_NATIVE_TEMPORARY_PREPARATION_DIAGNOSTIC';

// project 严格解析一条有界JSON日志，返回闭集两字段或null；不转换对象、不保留logger原文。
function project(line) {
    if (typeof line !== 'string' || Buffer.byteLength(line) > 16384) return null;
    try {
        let value = uniqueJSON(line);
        if (value && typeof value === 'object' && !Array.isArray(value) && Object.hasOwn(value, 'content')) {
            if (value.level !== 'error' || typeof value.content !== 'string') return null;
            value = uniqueJSON(value.content);
        }
        if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== 2 ||
            !Object.hasOwn(value, 'marker') || !Object.hasOwn(value, 'reason') || value.marker !== marker ||
            !reasons.includes(value.reason)) return null;
        return {marker, reason: value.reason};
    } catch {return null;}
}
module.exports = {project, reasons};
