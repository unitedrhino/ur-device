// 文件说明：正式privacyMode上报→DMS→IM→C++预览／对讲停流验收；不直接stop轨、不伪造MQTT或RPC。
'use strict';
const assert = require('node:assert/strict');
const {performance} = require('node:perf_hooks');
const ipc = require('./ipc_live.cjs');

// privacyClickSnapshot 在原页面只读观察真实按钮事件；不调用onclick、send或生成协议身份。
// 函数可序列化给原浏览器，计数按按钮独立；isTrusted拒绝脚本伪造的点击证明。
function privacyClickSnapshot(buttonID) {
  if (!['privacy-enable', 'privacy-disable'].includes(buttonID)) throw Error('privacy_button_invalid');
  const button = document.getElementById(buttonID); // 必须是原页面真实控件，不补建替身元素。
  if (!button) throw Error('privacy_button_missing');
  if (!window.__privacyReportClicks) window.__privacyReportClicks = Object.create(null);
  if (!Object.hasOwn(window.__privacyReportClicks, buttonID)) {
    const fact = {count: 0, trusted: false}; // 不含身份、token或原响应文字。
    window.__privacyReportClicks[buttonID] = fact;
    button.addEventListener('click', event => {fact.count++; fact.trusted = event.isTrusted === true;}, {capture: true, passive: true});
  }
  return {...window.__privacyReportClicks[buttonID]};
}

// privacyValue 只接受当前原设备唯一良好布尔记录；未知与坏质量不能强转成关闭。
function privacyValue(result, deviceName) {
  assert.ok(result && Array.isArray(result.list) && result.list.length <= 1, 'privacy_latest_invalid');
  if (!result.list.length) return null;
  const row = result.list[0];
  // 单设备latest接口省略deviceName；归属来自闭包内已鉴权的精确原设备请求，显式异名仍拒绝。
  assert.ok((!Object.hasOwn(row, 'deviceName') || row.deviceName === deviceName) && row.dataID === 'privacyMode' && row.quality === 'GOOD' &&
    ['0', '1', 'false', 'true'].includes(row.value) && Number.isSafeInteger(row.receivedTime) && row.receivedTime > 0 &&
    typeof row.timestamp === 'string' && /^[1-9][0-9]{0,15}$/.test(row.timestamp) && Number.isSafeInteger(Number(row.timestamp)),
  'privacy_latest_untrusted');
  return {enabled: row.value === '1' || row.value === 'true', received: row.receivedTime, timestamp: Number(row.timestamp)};
}

// preparePrivacy 只核已有布尔定义与原关闭值；只有专用新夹具明确允许时才接受尚未上报的空值。
// api/execute注入只用于离线测试，真实入口使用原主人HTTP及原C++页面，绝不注册或修改物模型定义。
async function preparePrivacy({api, device, allowInitialize = false, mediaScope = 'preview', now = () => performance.now(), sleep = ms => new Promise(r => setTimeout(r, ms))}) {
  assert.equal(typeof api, 'function'); assert.equal(typeof allowInitialize, 'boolean');
  assert.ok(['preview', 'talk', 'combined'].includes(mediaScope), 'privacy_media_scope_invalid');
  const expectedPeers = {preview: 3, talk: 1, combined: 4}[mediaScope]; // 组合必须保留四个原采集和Peer对象。
  for (const key of ['productID', 'deviceName']) assert.match(device[key], /^[A-Za-z0-9_-]{1,64}$/);
  const schema = await api('things/device/schema/tsl-read', device);
  assert.ok(typeof schema?.tsl === 'string' && schema.tsl.length <= 1048576, 'privacy_schema_invalid');
  const properties = JSON.parse(schema.tsl).properties;
  assert.ok(Array.isArray(properties) && properties.length <= 1024, 'privacy_schema_invalid');
  const matches = properties.filter(p => p.identifier === 'privacyMode');
  assert.ok(matches.length === 1 && matches[0].define?.type === 'bool', 'privacy_registered_bool_required');
  const latest = () => api('things/device/msg/property-latest/get-list', {...device, dataIDs: ['privacyMode'], ignoreEmpty: true})
    .then(value => privacyValue(value, device.deviceName));
  const original = await latest();
  assert.ok(original ? !original.enabled : allowInitialize, 'privacy_original_off_required');
  const facts = {registeredBoolVerified: true, originalOffVerified: !!original, initializedByCpp: false,
    enableReportVerified: false, originalOffRestored: false, oldMediaNotResumed: false};
  let execute, initialized = false, attempted = false, restorePromise;
  // waitFor 用单调截止短查询，不重发任何按钮或平台写入，也不延长原物模型请求预算。
  async function waitFor(predicate, message) {
    const deadline = now() + 35000;
    while (!await predicate()) {
      assert.ok(now() < deadline, message); await sleep(100);
    }
  }
  // report 只点击原页面按钮一次；ACK与正式最新值分别核验，不能用本地点击作为停流证明。
  async function report(enabled) {
    assert.ok(execute, 'privacy_original_page_required');
    await waitFor(async () => ipc.evaluated(await execute('eval', 'JSON.stringify(!closed && privacyAvailable && !privacyPending)')) === true,
      'privacy_report_not_ready');
    const sentAt = Date.now(); // 仅核本次报告实际时间，不参与重试或等待截止。
    await execute('snapshot', '-i');
    const buttonID = enabled ? 'privacy-enable' : 'privacy-disable'; // 多路视频滚动后必须把原控件带回可点击区域。
    await execute('scrollintoview', `#${buttonID}`);
    const click = () => execute('eval', `JSON.stringify((${privacyClickSnapshot.toString()})(${JSON.stringify(buttonID)}))`).then(ipc.evaluated);
    const before = await click();
    await execute('find', 'role', 'button', 'click', '--name', enabled ? '上报隐私开启' : '上报隐私关闭');
    const after = await click();
    assert.ok(Number.isSafeInteger(before.count) && Number.isSafeInteger(after.count) && after.count === before.count + 1 &&
      after.trusted === true, 'privacy_original_click_required');
    await waitFor(async () => ipc.evaluated(await execute('eval',
      `JSON.stringify(!closed && !privacyPending && document.getElementById('privacy-status').textContent.includes('已获平台确认'))`)) === true,
    'privacy_original_report_ack_required');
    await waitFor(async () => {
      const value = await latest();
      return value && value.enabled === enabled && value.received >= sentAt && value.timestamp >= sentAt;
    }, 'privacy_formal_latest_required');
  }
  return {cause: 'privacyEnabled', facts,
    ready: async context => {
      assert.ok(!execute && typeof context.execute === 'function', 'privacy_ready_once');
      execute = context.execute;
      const current = await latest(); assert.ok(current ? !current.enabled : allowInitialize, 'privacy_original_changed');
      await report(false); initialized = true; facts.initializedByCpp = true;
    },
    trigger: async () => {
      assert.ok(initialized && !attempted, 'privacy_trigger_once');
      const before = await latest(); assert.ok(before && !before.enabled, 'privacy_original_changed');
      attempted = true; // ACK丢失也必须恢复；不能以异常推断原报告未到达平台。
      await report(true); facts.enableReportVerified = true;
    },
    restore: async () => {
      if (!execute || !initialized) return;
      // 恢复意图只执行一次；并发finally或停止证据失败仍共享原Promise，不换token重发false。
      if (!restorePromise) restorePromise = (async () => {
        if (attempted) {
          await report(false); facts.originalOffRestored = true;
          // 停流完成路径必须保留原轨与Peer对象检查；不能仅以views为空证明真实驱动停止。
          const stopped = () => execute('eval', 'JSON.stringify(views.size===0 && invitations.size===0 && ' +
            `window.__ipcAllTracks?.length>=${expectedPeers} && window.__ipcAllTracks.every(t=>t.readyState==="ended") && ` +
            `window.__ipcAllPeers?.length===${expectedPeers} && window.__ipcAllPeers.every(p=>p.connectionState==="closed"))`)
            .then(ipc.evaluated);
          assert.equal(await stopped(), true, 'privacy_actual_stop_required');
          await sleep(1000); assert.equal(await stopped(), true, 'privacy_false_must_not_resume');
          facts.oldMediaNotResumed = true;
        } else facts.originalOffRestored = true;
      })();
      return restorePromise;
    }};
}
// privacyConfiguration 在真实入口启动前核验显式许可及回环身份；不读取本机凭据文件或默认连环境。
function privacyConfiguration(env) {
  for (const key of ['UR_IPC_LIVE', 'UR_IPC_PRIVACY_LIVE', 'UR_IPC_PRIVACY_REPORT'])
    assert.equal(env[key], '1', 'privacy_explicit_switch_required');
  const base = new URL(env.UR_IPC_BASE_URL);
  assert.ok(base.protocol === 'http:' && base.hostname === '127.0.0.1' && base.port &&
    Number(base.port) >= 1024 && !base.username && !base.password && base.pathname === '/' &&
    !base.search && !base.hash, 'privacy_loopback_required');
  const device = {productID: env.UR_DEVICE_PRODUCT, deviceName: env.UR_DEVICE_NAME};
  for (const value of Object.values(device)) assert.match(value || '', /^[A-Za-z0-9_-]{1,64}$/);
  assert.ok(typeof env.UR_IPC_APP_TOKEN === 'string' && env.UR_IPC_APP_TOKEN.length > 0 && env.UR_IPC_APP_TOKEN.length <= 8192,
    'privacy_owner_token_required');
  assert.match(env.UR_IPC_TENANT || '', /^[A-Za-z0-9_-]{1,50}$/);
  assert.match(env.UR_IPC_APP_ID || '100', /^[A-Za-z0-9_-]{1,50}$/);
  const scenario = env.UR_IPC_PRIVACY_SCENARIO || 'preview';
  assert.ok(['preview', 'talk', 'combined'].includes(scenario), 'privacy_media_scope_invalid');
  if (scenario === 'combined') assert.equal(env.UR_IPC_TALK_PREVIEW_COUNT, '3', 'combined_three_previews_required');
  else assert.ok(env.UR_IPC_TALK_PREVIEW_COUNT === undefined || env.UR_IPC_TALK_PREVIEW_COUNT === '0',
    'privacy_scope_must_match_media');
  return {base: base.origin, device, scenario};
}

// privacyReadAPI 只开放物模型与当前属性两条正式只读接口；错误仅给固定分类，不泄漏响应正文。
function privacyReadAPI(env, configuration) {
  return async (route, body) => {
    assert.ok(['things/device/schema/tsl-read', 'things/device/msg/property-latest/get-list'].includes(route),
      'privacy_readonly_route_required');
    const response = await fetch(`${configuration.base}/api/v1/${route}`, {method: 'POST', redirect: 'error',
      headers: {'content-type': 'application/json', 'app-id': env.UR_IPC_APP_ID || '100',
        'tenant-code': env.UR_IPC_TENANT, token: env.UR_IPC_APP_TOKEN}, body: JSON.stringify(body),
      signal: AbortSignal.timeout(15000)});
    const {readBoundedJSON} = require('./conference_member_live.cjs');
    const value = await readBoundedJSON(response);
    assert.ok(response.ok && value.code === 200, 'privacy_readonly_api_rejected');
    return value.data;
  };
}

// runPrivacy 提供可重复真实验收入口；原属性必须已明确关闭，不自动初始化、创建定义或授权。
async function runPrivacy() {
  const configuration = privacyConfiguration(process.env);
  const driver = configuration.scenario !== 'preview' ? require('./talk_live.cjs') : ipc;
  if (configuration.scenario !== 'preview') driver.validateConfiguration(process.env);
  const probe = await preparePrivacy({api: privacyReadAPI(process.env, configuration), device: configuration.device, mediaScope: configuration.scenario});
  const outcome = await driver.run({onReady: ({deviceBrowser, env}) => probe.ready({execute: (...args) => ipc.browser(deviceBrowser, env, ...args)}),
    safety: {cause: probe.cause, trigger: probe.trigger, restore: probe.restore}});
  return {outcome, facts: probe.facts};
}
module.exports = {privacyClickSnapshot, privacyValue, preparePrivacy, privacyConfiguration, privacyReadAPI, runPrivacy};
if (require.main === module) runPrivacy().catch(() => {
  console.error('privacy_live_failed'); process.exitCode = 1;
});
