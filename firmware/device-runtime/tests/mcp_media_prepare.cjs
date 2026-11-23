// 文件说明：本机正式MCP媒体验收的只读准备入口；生成待执行Agent请求，不初始化数据库或发送媒体副作用。
'use strict';
const assert = require('node:assert/strict');

// mediaTools 固定正式媒体工具集合，不让模型或配置文件指定任意接口。
const mediaTools = ['invite_media_playback', 'get_media_playback_status', 'control_media_playback',
  'recover_media_playback_operation', 'get_output_focus_status', 'control_output_focus'];
// safeID 校验正式API整数，拒绝浏览器精度丢失、零及自报不可信身份。
function safeID(value) {
  const text = typeof value === 'number' ? String(value) : value;
  assert.ok(typeof text === 'string' && /^[1-9][0-9]{0,15}$/.test(text));
  const number = Number(text);
  assert.ok(Number.isSafeInteger(number) && String(number) === text);
  return number;
}
// localURL 只允许本任务回环HTTP端点，禁止跳转、凭据和远程环境误用。
function localURL(value, route = '/') {
  const url = new URL(value);
  assert.ok(url.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname) &&
    !url.username && !url.password && !url.search && !url.hash && url.pathname === route);
  return url;
}
// readBounded 真实HTTP按字节限制正文；离线传输也必须满足同一大小上限，不归档自由响应。
async function readBounded(response) {
  const limit = 1024 * 1024;
  if (!response.body?.getReader) {
    const text = await response.text();
    assert.ok(Buffer.byteLength(text) <= limit);
    return text;
  }
  const reader = response.body.getReader();
  const chunks = [];
  let size = 0;
  try {
    for (;;) {
      const item = await reader.read();
      if (item.done) break;
      size += item.value.byteLength;
      assert.ok(size <= limit);
      chunks.push(Buffer.from(item.value));
    }
  } finally {
    await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
  return Buffer.concat(chunks).toString('utf8');
}
// ordinaryOwner 仅接纳正式self接口的启用普通client身份，企业管理员不能充当媒体原用户。
function ordinaryOwner(user, tenant) {
  const owner = safeID(user.userID);
  assert.ok(Array.isArray(user.tenants));
  const memberships = user.tenants.filter(value => value?.tenantCode === tenant);
  assert.equal(memberships.length, 1);
  const membership = memberships[0];
  assert.ok(membership.status === 1 && membership.isTenantOwner !== 1 &&
    Array.isArray(membership.roles) && membership.roles.length > 0 &&
    membership.roles.every(role => role?.code === 'client' && role.status === 1 && role.isFullAuth !== 1));
  return owner;
}
// parseTools 检查服务正式发现的完整schema；缓存文本、任意工具名及占位schema不能证明工具已装配。
function parseTools(raw) {
  const parsed = typeof raw === 'string' ? JSON.parse(raw) : raw;
  assert.ok(parsed && typeof parsed === 'object' && Array.isArray(parsed.tools));
  const seen = new Set();
  for (const tool of parsed.tools) {
    assert.ok(tool && typeof tool.name === 'string' && !seen.has(tool.name));
    seen.add(tool.name);
    if (mediaTools.includes(tool.name)) {
      assert.ok(tool.inputSchema && tool.inputSchema.type === 'object' &&
        tool.inputSchema.properties && typeof tool.inputSchema.properties === 'object' &&
        !Array.isArray(tool.inputSchema.properties) &&
        Buffer.byteLength(JSON.stringify(tool.inputSchema)) <= 8192);
      for (const key of Object.keys(tool.inputSchema.properties))
        assert.ok(!['_sessionID', '_operationID', 'tenantCode', 'userID', 'epoch', 'focusId', 'resources'].includes(key));
    }
  }
  assert.ok(mediaTools.every(name => seen.has(name)));
  return parsed.tools.length;
}
// prepareMcpMedia 用正式普通用户和管理API做只读预检，返回有限初始化计划；不更改共享MCP、Agent、设备或许可。
async function prepareMcpMedia(options, transport = fetch) {
  let phase = 'input';
  try {
    assert.equal(options?.live, '1');
    const origin = localURL(options.base);
    const endpoint = localURL(options.endpoint, '/api/v1/things/ai/mcp/run').href;
    assert.ok(typeof options.tenant === 'string' && /^[A-Za-z0-9_-]{1,50}$/.test(options.tenant) &&
      !['platform', 'all'].includes(options.tenant));
    for (const name of ['ordinaryToken', 'adminToken'])
      assert.ok(typeof options[name] === 'string' && options[name].length > 0 && options[name].length <= 8192 &&
        !/[\r\n]/.test(options[name]));
    assert.notEqual(options.ordinaryToken, options.adminToken);
    for (const name of ['appId', 'adminAppId'])
      assert.ok(typeof options[name] === 'string' && /^[A-Za-z0-9_-]{1,50}$/.test(options[name]));
    assert.notEqual(options.appId, '100'); // 平台应用的内部管理位不能代替普通用户上下文。
    assert.ok(options.configs && Object.keys(options.configs).sort().join(',') === 'asr,llm,realtime,tts');
    for (const value of Object.values(options.configs))
      assert.ok(typeof value === 'string' && /^[A-Za-z0-9_.:-]{1,128}$/.test(value));
    assert.ok(typeof transport === 'function');
    // request 不重试未知、不跟随重定向；原正文及私密HTTP错误不对外传播。
    async function request(route, body, admin = false) {
      const response = await transport(origin.origin + route, {method: 'POST', redirect: 'manual',
        headers: {'content-type': 'application/json', 'tenant-code': options.tenant,
          'app-id': admin ? options.adminAppId : options.appId,
          token: admin ? options.adminToken : options.ordinaryToken},
        body: JSON.stringify(body), signal: AbortSignal.timeout(8000)});
      assert.ok(response.ok && !(response.status >= 300 && response.status < 400));
      const envelope = JSON.parse(await readBounded(response));
      assert.equal(envelope.code, 200);
      assert.ok(envelope.data && typeof envelope.data === 'object' && !Array.isArray(envelope.data));
      return envelope.data;
    }
    phase = 'ordinary_identity';
    const owner = ordinaryOwner(await request('/api/v1/system/user/self/get-one', {withTenant: true}), options.tenant);
    phase = 'admin_identity';
    const admin = await request('/api/v1/system/user/self/get-one', {withTenant: true}, true);
    assert.notEqual(safeID(admin.userID), owner);
    assert.ok(Array.isArray(admin.tenants) && admin.tenants.some(tenant =>
      Array.isArray(tenant.roles) && tenant.roles.some(role => role?.code === 'supper' && role.status === 1)));
    phase = 'model_configs';
    const selected = {};
    for (const kind of ['realtime', 'llm', 'asr', 'tts']) {
      const data = await request('/api/v1/ai/config/info/get-list', {cfgType: kind,
        onlyEnabled: true, page: {page: 1, pageSize: 200}}, true);
      assert.ok(Array.isArray(data.list) && data.list.length <= 200 &&
        Number.isSafeInteger(data.total) && data.total === data.list.length);
      const matches = data.list.filter(row => row.configID === options.configs[kind]);
      assert.equal(matches.length, 1);
      assert.ok(matches[0].cfgType === kind && matches[0].enabled === true);
      selected[kind] = matches[0].configID; // 从不返回模型jsonData或认证配置。
    }
    phase = 'builtin_mcp';
    const services = await request('/api/v1/ai/mcp/service/get-list', {tenantCode: 'common',
      nameLike: 'things_iot_mcp', enabled: true, page: {page: 1, pageSize: 200}}, true);
    assert.ok(Array.isArray(services.list) && services.list.length <= 200);
    const matched = services.list.filter(service => service.name === 'things_iot_mcp' &&
      service.tenantCode === 'common' && service.enabled === true);
    assert.equal(matched.length, 1);
    const mcpId = safeID(matched[0].id);
    const service = await request('/api/v1/ai/mcp/service/get-one', {tenantCode: 'common', id: mcpId}, true);
    assert.ok(safeID(service.id) === mcpId && service.tenantCode === 'common' &&
      service.name === 'things_iot_mcp' && service.enabled === true && service.endpoint === endpoint);
    const toolCount = parseTools(service.tools);
    const devices = [];
    if (options.devices !== undefined) {
      phase = 'device_owner';
      assert.ok(Array.isArray(options.devices) && options.devices.length === 2);
      const routes = new Set();
      for (const [index, device] of options.devices.entries()) {
        assert.ok(device && Object.keys(device).sort().join(',') === 'deviceName,productID');
        for (const key of ['productID', 'deviceName'])
          assert.ok(typeof device[key] === 'string' && /^[A-Za-z0-9_.-]{1,128}$/.test(device[key]));
        const route = `${device.productID}:${device.deviceName}`;
        assert.ok(!routes.has(route)); routes.add(route);
        const actual = await request('/api/v1/things/device/info/get-one', {...device, withOwner: true}, true);
        assert.ok(actual.productID === device.productID && actual.deviceName === device.deviceName &&
          actual.tenantCode === options.tenant && safeID(actual.userID) === owner);
        const cloneId = safeID(actual.cloneID);
        const projectId = safeID(actual.projectID);
        const clone = await request('/api/v1/ai/clone/get-one', {id: cloneId}, true);
        assert.ok(safeID(clone.id) === cloneId && clone.tenantCode === options.tenant &&
          clone.thirdID === String(projectId));
        devices.push({chain: index === 0 ? 'cascaded_agentruntime' : 'realtime_native',
          productID: device.productID, deviceName: device.deviceName,
          projectID: String(projectId), cloneID: cloneId, agentID: safeID(clone.agentID)});
      }
    }
    // Agent请求只是待管理员执行的正式初始化计划；不绑定旧Clone或隐式改变设备行为。
    const prompt = '只执行当前单一显式意图。测试音乐为task143.music.fixture，测试视频为task143.video.fixture。' +
      '不连做下一步。未知结果按原操作只读恢复，不重新邀请、不更换操作标识。' +
      '父会话、身份、连接代次、焦点和原完整组由可信后端派生。不要停止AI音频输入。';
    const common = {tenantCode: options.tenant, userID: owner, capabilities: ['voice', 'text'],
      mcpServiceIds: JSON.stringify([mcpId]), isAllMcp: 2, systemPrompt: prompt, status: '1'};
    return {stage: 'read_only_preflight', initialized: false, ownerUserID: String(owner),
      selectedConfigs: selected, mcpServiceID: mcpId, mediaTools: mediaTools.slice(), discoveredToolCount: toolCount,
      devices, agentCreateEndpoint: '/api/v1/ai/agent/info/create', agentRequests: [
        {...common, name: 'TASK143-MCP串联测试', voicePipeline: 'cascade', interactionMode: 'halfDuplex',
          llmConfigID: selected.llm, asrConfigID: selected.asr, ttsConfigID: selected.tts},
        {...common, name: 'TASK143-MCP实时测试', voicePipeline: 'realtime', interactionMode: 'fullDuplex',
          realtimeConfigID: selected.realtime}],
      remaining: ['正式创建独立Agent', '新产品defaultAgentId与普通主人项目绑定并读回Clone',
        '独立AI全媒体配置与受控HTTPS目录', '真实模型音乐及视频各十轮与后端原操作只读证据'],
      budget: {legacyHttpDefaultSeconds: 8, shortOperationsMayComplete: true,
        worstCaseBudgetVerified: false}, backendExecutionChainVerified: false};
  } catch {
    throw Error('mcp_media_prepare_failed:' + phase); // 阶段是固定白名单，不传播自由API正文或秘密。
  }
}

// fromEnvironment 只接收同一进程完整凭据组，不读取旧profile、临时明文文件或远程环境补齐。
function fromEnvironment(env) {
  return {live: env.UR_MCP_PREPARE_LIVE, base: env.UR_MCP_BASE_URL, endpoint: env.UR_MCP_ENDPOINT,
    appId: env.UR_MCP_USER_APP_ID, adminAppId: env.UR_MCP_ADMIN_APP_ID, tenant: env.UR_MCP_TENANT,
    ordinaryToken: env.UR_MCP_USER_TOKEN, adminToken: env.UR_MCP_ADMIN_TOKEN,
    configs: JSON.parse(env.UR_MCP_CONFIG_IDS || '{}'),
    ...(env.UR_MCP_DEVICES ? {devices: JSON.parse(env.UR_MCP_DEVICES)} : {})};
}

module.exports = {mediaTools, safeID, ordinaryOwner, parseTools, prepareMcpMedia, fromEnvironment};
if (require.main === module) {
  Promise.resolve().then(() => prepareMcpMedia(fromEnvironment(process.env))).then(result => {
    console.log(JSON.stringify(result));
  }).catch(() => { console.error('mcp_media_prepare_failed'); process.exitCode = 1; });
}
