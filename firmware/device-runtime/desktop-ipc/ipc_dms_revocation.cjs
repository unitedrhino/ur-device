// 文件说明：IPC撤权验收的正式DMS转让适配，只允许本机隔离后端；原归属必须在finally恢复。
'use strict';
const assert = require('node:assert/strict');

// binding只比较转让会影响的归属及原Clone，不用采集活跃时间覆盖正常设备事件。
function binding(value) {
  return {productID:value.productID,deviceName:value.deviceName,tenantCode:value.tenantCode,
    userID:String(value.userID),projectID:String(value.projectID),areaID:String(value.areaID || 0),
    cloneID:String(value.cloneID || 0)};
}
// same用于不可变原归属比较，字段集由binding固定，不接受请求正文扩充。
function same(left, right) { return JSON.stringify(left) === JSON.stringify(right); }
// decimal接受无精度损失的正式ID字符串，不把JSON数字舍入后当作可信身份。
function decimal(value) { return typeof value === 'string' && /^[1-9][0-9]{0,18}$/.test(value) && BigInt(value) <= 9223372036854775807n; }

// prepareDmsRevocation先读回原设备／原项目／目标项目及空分享，返回正式触发和有条件恢复操作。
// 配置只接受原主人和已建目标项目；不创建用户／项目、不删除数据、不发RPC或伪造MQTT终态。
// fetchImpl供单测注入；真实验收默认使用系统fetch，API失败只暴露固定错误，不反射凭据或响应正文。
async function prepareDmsRevocation(config, fetchImpl = fetch) {
  assert.ok(config && typeof config === 'object', 'dms_safety_config_required');
  const origin = new URL(config.baseUrl);
  assert.ok(origin.protocol === 'http:' && origin.hostname === '127.0.0.1' && !origin.username &&
    !origin.password && origin.pathname === '/' && !origin.search && !origin.hash, 'dms_safety_local_origin_required');
  assert.ok(typeof config.token === 'string' && config.token.length > 0 && config.token.length <= 8192, 'dms_safety_token_required');
  assert.ok(decimal(config.ownerUserID) && decimal(config.targetProjectID), 'dms_safety_identity_required');
  assert.ok(typeof config.tenantCode === 'string' && /^[a-zA-Z0-9_-]{1,50}$/.test(config.tenantCode), 'dms_safety_scope_required');
  for (const field of ['productID','deviceName']) assert.ok(typeof config[field] === 'string' &&
    /^[a-zA-Z0-9_.:-]{1,128}$/.test(config[field]), 'dms_safety_device_required');
  const key = {productID:config.productID,deviceName:config.deviceName};
  // api限制固定路由和1MiB流式上限；禁止重定向到非本机服务或记录认证信息。
  async function api(route, body) {
    assert.ok(['things/device/info/get-one','things/device/info/transfer','things/project/info/get-one','things/product/info/get-one',
      'things/user/device/share/get-list'].includes(route), 'dms_safety_route_denied');
    try {
      const response = await fetchImpl(new URL('/api/v1/' + route, origin).href, {method:'POST',redirect:'error',
        headers:{'content-type':'application/json','app-id':config.appID || '100','tenant-code':config.tenantCode,token:config.token},
        body:JSON.stringify(body),signal:AbortSignal.timeout(20000)});
      assert.equal(response.status,200);
      const reader = response.body.getReader(), parts = []; let bytes = 0;
      try {
        for (;;) {
          const item = await reader.read(); if (item.done) break;
          bytes += item.value.length; assert.ok(bytes <= 1048576); parts.push(item.value);
        }
      } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
      const result = JSON.parse(Buffer.concat(parts)); assert.equal(result.code,200);
      return result.data;
    } catch { throw Error('dms_safety_api_failed'); }
  }
  const before = binding(await api('things/device/info/get-one',key));
  assert.ok(before.productID === key.productID && before.deviceName === key.deviceName &&
    before.tenantCode === config.tenantCode && before.userID === config.ownerUserID && decimal(before.projectID) &&
    (before.cloneID === '0' || decimal(before.cloneID)), 'dms_safety_original_mismatch');
  // 无Clone仅允许没有默认Agent的专用相机产品，避免转让顺带创建不可用此接口撤销的Clone绑定。
  async function verifyNoImplicitClone() {
    if (before.cloneID !== '0') return;
    const product = await api('things/product/info/get-one',{productID:key.productID});
    assert.ok(product.productID === key.productID && product.defaultAgentId === 0, 'dms_safety_implicit_clone_denied');
  }
  await verifyNoImplicitClone();
  const originalProject = await api('things/project/info/get-one',{projectID:before.projectID});
  assert.ok(String(originalProject.adminUserID) === before.userID && originalProject.tenantCode === before.tenantCode, 'dms_safety_original_project_mismatch');
  const target = await api('things/project/info/get-one',{projectID:config.targetProjectID});
  assert.ok(String(target.projectID) === config.targetProjectID && target.tenantCode === before.tenantCode &&
    decimal(String(target.adminUserID)) && String(target.adminUserID) !== before.userID &&
    config.targetProjectID !== before.projectID, 'dms_safety_target_mismatch');
  const shares = await api('things/user/device/share/get-list',{device:key,page:{page:1,size:1}});
  // DMS转让会清分享，即使isCleanData=2也一样；只操作没有分享的专用夹具，避免无法完整恢复授权。
  assert.ok(Number(shares.total) === 0 && (shares.list || []).length === 0, 'dms_safety_shared_device_denied');
  const after = {...before,userID:String(target.adminUserID),projectID:config.targetProjectID};
  let attempted = false, transferred;
  const facts = {originalVerified:true,targetVerified:true,originalSharesEmpty:true,transferred:false,restored:false};
  // expectedTarget核对唯一任务转让后的归属；area由DMS分配但只能首次原操作读回后固化。
  function expectedTarget(current) {
    return current.productID === after.productID && current.deviceName === after.deviceName &&
      current.tenantCode === after.tenantCode && current.userID === after.userID &&
      current.projectID === after.projectID && current.cloneID === after.cloneID &&
      (!transferred || current.areaID === transferred.areaID);
  }
  return {cause:'permissionRevoked',facts,
    trigger:async () => {
      assert.ok(!attempted, 'dms_safety_trigger_once');
      assert.ok(same(binding(await api('things/device/info/get-one',key)),before), 'dms_safety_original_changed');
      await verifyNoImplicitClone();
      attempted = true; // HTTP应答丢失也必须在finally尝试正式读回和恢复，不能以抛错推断未写入。
      await api('things/device/info/transfer',{device:key,srcProjectID:before.projectID,transferTo:2,
        projectID:after.projectID,isCleanData:2});
      transferred = binding(await api('things/device/info/get-one',key));
      assert.ok(expectedTarget(transferred), 'dms_safety_transfer_mismatch'); facts.transferred = true;
    },
    restore:async () => {
      let current = binding(await api('things/device/info/get-one',key));
      if (!same(current,before)) {
        assert.ok(attempted && expectedTarget(current), 'dms_safety_restore_foreign_binding');
        await api('things/device/info/transfer',{device:key,srcProjectID:after.projectID,transferTo:2,
          projectID:before.projectID,areaID:before.areaID,isCleanData:2});
        current = binding(await api('things/device/info/get-one',key));
      }
      assert.ok(same(current,before), 'dms_safety_restore_mismatch'); facts.restored = true;
    }};
}
module.exports = {prepareDmsRevocation};
