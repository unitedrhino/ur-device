// 文件说明：正式MCP只读准备器的离线API契约测试；模拟响应不代表真实初始化或模型验收完成。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {mediaTools, safeID, prepareMcpMedia, fromEnvironment} = require('./mcp_media_prepare.cjs');

// options 提供纯离线回环参数，秘密占位值用于验证不得进入输出和失败报告。
function options() {
  return {live: '1', base: 'http://127.0.0.1:18000', endpoint: 'http://127.0.0.1:18001/api/v1/things/ai/mcp/run',
    tenant: 'fixture', appId: '101', adminAppId: '100', ordinaryToken: 'private-user-token',
    adminToken: 'private-admin-token', configs: {realtime: 'native', llm: 'llm', asr: 'asr', tts: 'tts'}};
}
// fixtureTools 仿正式tools/list缓存结构，每个媒体工具均有有限且不泄露授权字段的schema。
function fixtureTools() {
  return JSON.stringify({tools: mediaTools.map(name => ({name,
    inputSchema: {type: 'object', properties: {sessionId: {type: 'string'}}}}))});
}
// backend 记录正式API请求，不联网；可定向改响应以验证未知、越权及不可信字段的安全拒绝。
function backend(rewrite = (value) => value) {
  const calls = [];
  async function transport(url, request) {
    const route = new URL(url).pathname;
    const body = JSON.parse(request.body);
    calls.push({route, body, request});
    let data;
    if (route === '/api/v1/system/user/self/get-one') {
      assert.equal(body.withTenant, true); // 身份预检显式请求企业成员和角色，不依赖当前assemble的兼容兜底。
      const admin = request.headers.token === 'private-admin-token';
      data = {userID: admin ? '100' : '101', tenants: [{tenantCode: 'fixture', status: 1,
        isTenantOwner: 2, roles: [{code: admin ? 'supper' : 'client', status: 1, isFullAuth: 2}]}]};
    } else if (route === '/api/v1/ai/config/info/get-list') {
      assert.deepEqual(body.page, {page: 1, pageSize: 200}); // AISVR分页契约与Core/Things的size不同，非法键不能被fixture忽略。
      assert.deepEqual(Object.keys(body).sort(), ['cfgType', 'onlyEnabled', 'page']);
      assert.ok(['realtime', 'llm', 'asr', 'tts'].includes(body.cfgType));
      assert.equal(body.onlyEnabled, true);
      data = {total: 1, list: [{configID: body.cfgType === 'realtime' ? 'native' : body.cfgType,
        cfgType: body.cfgType, enabled: true, jsonData: 'private-model-credential'}]};
    } else if (route === '/api/v1/ai/mcp/service/get-list') {
      assert.deepEqual(body.page, {page: 1, pageSize: 200});
      assert.deepEqual(body, {tenantCode: 'common', nameLike: 'things_iot_mcp',
        enabled: true, page: {page: 1, pageSize: 200}});
      data = {total: 1, list: [{id: 201, tenantCode: 'common', name: 'things_iot_mcp', enabled: true}]};
    } else if (route === '/api/v1/ai/mcp/service/get-one') {
      assert.deepEqual(body, {tenantCode: 'common', id: 201}); // AISVR数据库ID按数字传，不与模型业务configID字符串混用。
      data = {id: 201, tenantCode: 'common', name: 'things_iot_mcp', enabled: true,
        endpoint: options().endpoint, tools: fixtureTools(), authConfig: 'private-service-credential'};
    } else if (route === '/api/v1/things/device/info/get-one') {
      assert.deepEqual(Object.keys(body).sort(), ['deviceName', 'productID', 'withOwner']);
      assert.equal(body.withOwner, true);
      data = {...body, tenantCode: 'fixture', userID: '101', projectID: '701', cloneID: '501', secret: 'private-device-secret'};
    } else if (route === '/api/v1/ai/clone/get-one') {
      assert.deepEqual(body, {id: 501});
      data = {id: 501, tenantCode: 'fixture', thirdID: '701', agentID: 601};
    } else throw Error('unexpected_api');
    const envelope = rewrite({code: 200, data}, route, body, request);
    return {ok: true, status: 200, text: async () => JSON.stringify(envelope)};
  }
  return {calls, transport};
}

test('仅正式只读API准备两模型链计划，秘密与媒体成功事实不出现在结果中', async () => {
  const server = backend();
  const result = await prepareMcpMedia(options(), server.transport);
  assert.equal(result.initialized, false);
  assert.equal(result.backendExecutionChainVerified, false);
  assert.deepEqual(result.agentRequests.map(value => [value.voicePipeline, value.interactionMode]),
    [['cascade', 'halfDuplex'], ['realtime', 'fullDuplex']]);
  for (const value of result.agentRequests) {
    assert.equal(value.userID, 101);
    assert.equal(value.mcpServiceIds, '[201]');
    assert.equal(value.isAllMcp, 2);
  }
  assert.equal(JSON.stringify(result).includes('private'), false);
  assert.ok(server.calls.every(value => value.route.endsWith('/get-one') || value.route.endsWith('/get-list')));
  assert.ok(server.calls.every(value => value.request.method === 'POST' && value.request.redirect === 'manual'));
  assert.equal(server.calls.filter(value => value.route.includes('/config/')).length, 4);
  assert.ok(server.calls.filter(value => value.route.includes('/self/get-one')).every(value => value.body.withTenant === true));
});

test('无显式入口或非本机地址不联网，既有共享profile不能补齐凭据', async () => {
  for (const change of [value => { delete value.live; }, value => { value.base = 'http://example.org'; },
    value => { value.endpoint = 'http://127.0.0.1/wrong'; },
    value => { value.endpoint += '?secret=private'; }, value => { value.appId = '100'; },
    value => { value.adminToken = value.ordinaryToken; }, value => { delete value.ordinaryToken; },
    value => { value.tenant = 'platform'; }, value => { value.tenant = 'all'; }]) {
    const input = options(); change(input); const server = backend();
    await assert.rejects(prepareMcpMedia(input, server.transport), /^Error: mcp_media_prepare_failed:input$/);
    assert.equal(server.calls.length, 0);
  }
  const injected = fromEnvironment({UR_MCP_PREPARE_LIVE: '1', UR_MCP_CONFIG_IDS: '{}'});
  assert.equal(injected.ordinaryToken, undefined);
});

test('common实际普通client按正式归属通过，同企业管理员仍拒绝而不借字面授权', async () => {
  const input = {...options(), tenant: 'common', devices: [
    {productID: 'fixture-cascade', deviceName: 'one'}, {productID: 'fixture-native', deviceName: 'two'}]};
  // commonBackend 仅改变正式fixture所属企业，不伪造角色或设备owner来通过授权。
  const commonBackend = (denied = false) => backend((envelope, route, body, request) => {
    if (route.endsWith('/self/get-one')) {
      envelope.data.tenants[0].tenantCode = 'common';
      if (denied && request.headers.token === input.ordinaryToken) envelope.data.tenants[0].roles[0].code = 'admin';
    }
    if (route.includes('/device/') || route.includes('/clone/')) envelope.data.tenantCode = 'common';
    return envelope;
  });
  const allowed = commonBackend();
  const result = await prepareMcpMedia(input, allowed.transport);
  assert.equal(result.ownerUserID, '101');
  assert.equal(result.devices.length, 2);
  assert.ok(result.agentRequests.every(value => value.tenantCode === 'common'));
  const denied = commonBackend(true);
  await assert.rejects(prepareMcpMedia(input, denied.transport), /failed:ordinary_identity$/);
  assert.equal(denied.calls.length, 1);
});

test('原普通用户不能被企业管理员、企业主人、跨企业成员或不安全ID冒充', async () => {
  for (const change of [value => { value.data.tenants[0].roles[0].code = 'admin'; },
    value => { value.data.tenants[0].roles[0].isFullAuth = 1; },
    value => { value.data.tenants[0].isTenantOwner = 1; },
    value => { value.data.tenants[0].tenantCode = 'other'; },
    value => { value.data.userID = 9007199254740992; },
    value => { value.data.tenants.push(structuredClone(value.data.tenants[0])); }]) {
    const server = backend((value, route, body, request) => {
      if (route.endsWith('/self/get-one') && request.headers.token === 'private-user-token') change(value);
      return value;
    });
    await assert.rejects(prepareMcpMedia(options(), server.transport), /failed:ordinary_identity$/);
    assert.equal(server.calls.length, 1);
  }
});

test('未启用、不唯一、未分页完整的模型配置不能被当成可复用模型', async () => {
  for (const change of [value => { value.data.list[0].enabled = false; },
    value => { value.data.list[0].enabled = 1; },
    value => { value.data.list[0].configID = 123; },
    value => { value.data.list.push({...value.data.list[0]}); value.data.total = 2; },
    value => { value.data.total = 201; }, value => { value.data.list[0].cfgType = 'other'; }]) {
    const server = backend((value, route) => { if (route.includes('/config/')) change(value); return value; });
    await assert.rejects(prepareMcpMedia(options(), server.transport), /failed:model_configs$/);
  }
});

test('同名私有服务、错误端点及隐藏授权schema不能替代正式common MCP', async () => {
  for (const change of [value => { value.data.tenantCode = 'fixture'; },
    value => { value.data.endpoint = 'http://example.org/api/v1/things/ai/mcp/run'; },
    value => { value.data.enabled = false; },
    value => { value.data.tools = '{"tools":[]}'; },
    value => { const raw = JSON.parse(value.data.tools); raw.tools[0].inputSchema.properties._sessionID = {}; value.data.tools = JSON.stringify(raw); },
    value => { const raw = JSON.parse(value.data.tools); raw.tools[0].inputSchema.properties = null; value.data.tools = JSON.stringify(raw); },
    value => { const raw = JSON.parse(value.data.tools); raw.tools[0].inputSchema.properties = []; value.data.tools = JSON.stringify(raw); },
    value => { value.data.id = '9007199254740992'; }]) {
    const server = backend((value, route) => { if (route.includes('/mcp/') && route.endsWith('/get-one')) change(value); return value; });
    await assert.rejects(prepareMcpMedia(options(), server.transport), /failed:builtin_mcp$/);
  }
});

test('已有设备只接受正式实际原主人与项目Clone绑定，不复制设备秘密', async () => {
  const input = {...options(), devices: [{productID: 'fixture-cascade', deviceName: 'one'},
    {productID: 'fixture-native', deviceName: 'two'}]};
  const server = backend();
  const result = await prepareMcpMedia(input, server.transport);
  assert.equal(result.devices.length, 2);
  assert.deepEqual(result.devices.map(value => value.chain), ['cascaded_agentruntime', 'realtime_native']);
  assert.equal(JSON.stringify(result).includes('private'), false);
  for (const [routePart, key, value] of [['/device/', 'userID', '999'], ['/device/', 'tenantCode', 'other'],
    ['/device/', 'cloneID', '0'], ['/clone/', 'thirdID', '999']]) {
    const rejected = backend((envelope, route) => { if (route.includes(routePart)) envelope.data[key] = value; return envelope; });
    await assert.rejects(prepareMcpMedia(input, rejected.transport), /failed:device_owner$/);
  }
});

test('网络未知只读一次即停止，错误正文和重定向不能泄露秘密或变成成功', async () => {
  let calls = 0;
  const privateError = async () => { ++calls; throw Error('private-admin-token'); };
  await assert.rejects(prepareMcpMedia(options(), privateError), /^Error: mcp_media_prepare_failed:ordinary_identity$/);
  assert.equal(calls, 1);
  for (const response of [{ok: false, status: 302, text: async () => 'private'},
    {ok: true, status: 200, text: async () => JSON.stringify({code: 500, msg: 'private'})},
    {ok: true, status: 200, text: async () => 'x'.repeat(1024 * 1024 + 1)}]) {
    await assert.rejects(prepareMcpMedia(options(), async () => response),
      /^Error: mcp_media_prepare_failed:ordinary_identity$/);
  }
});

test('真实流式正文同样有界，纯整数解析不吞不安全值', async () => {
  const response = new Response('x'.repeat(1024 * 1024 + 1), {status: 200});
  await assert.rejects(prepareMcpMedia(options(), async () => response), /failed:ordinary_identity$/);
  assert.equal(response.body.locked, false);
  assert.equal(safeID('101'), 101);
  for (const value of ['0101', '0', '-1', '9007199254740992', 1.5, Infinity]) assert.throws(() => safeID(value));
});
