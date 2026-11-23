// 文件说明：门铃软件E2E的正式IM前置；使用当前鉴权用户创建渠道/单聊并回读，不插入授权数据库。
'use strict';
const assert = require('node:assert/strict');

// safeID 只接纳已解码的精确正整数，不把不安全数字取整成为另一个参与者。
function safeID(value) {
  if (typeof value === 'string') {
    assert.ok(/^[1-9][0-9]{0,15}$/.test(value), 'invalid_im_identity');
    const number = Number(value);
    assert.ok(Number.isSafeInteger(number) && String(number) === value, 'invalid_im_identity');
    return number;
  }
  assert.ok(Number.isSafeInteger(value) && value > 0, 'invalid_im_identity');
  return value;
}

// setupDoorbellIM 仅显式运行于本机隔离后端；可注入HTTP传输用于离线契约回归，不替代真实E2E。
async function setupDoorbellIM(options, transport = fetch) {
  assert.equal(options?.live, '1', 'explicit_doorbell_live_required');
  const origin = new URL(options.base);
  assert.ok(origin.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(origin.hostname) &&
    !origin.username && !origin.password && !origin.search && !origin.hash && origin.pathname === '/', 'local_backend_required');
  assert.ok(typeof options.token === 'string' && options.token.length > 0 && options.token.length <= 8192 &&
    !/[\r\n]/.test(options.token), 'invalid_private_auth');
  assert.ok(typeof options.tenant === 'string' && /^[A-Za-z0-9_-]{1,50}$/.test(options.tenant), 'invalid_im_scope');
  assert.ok(typeof options.appId === 'string' && /^[A-Za-z0-9_-]{1,50}$/.test(options.appId), 'invalid_im_app');
  for (const id of [options.productId, options.deviceName]) {
    assert.ok(typeof id === 'string' && /^[A-Za-z0-9_.-]{1,128}$/.test(id), 'invalid_device_route');
  }
  assert.equal(typeof transport, 'function', 'http_transport_required');
  const headers = {'content-type': 'application/json', 'app-id': options.appId,
    'tenant-code': options.tenant, token: options.token}; // 私有凭据只传当前本机鉴权入口，不写报告。
  // request 固定POST路径和预算；任何原错误/响应正文都不得进入公开失败报告。
  async function request(path, body) {
    try {
      const response = await transport(`${origin.origin}${path}`, {method: 'POST', headers,
        body: JSON.stringify(body), signal: AbortSignal.timeout(5000)});
      assert.equal(response.ok, true);
      const text = await response.text();
      assert.ok(text.length <= 65536);
      const envelope = JSON.parse(text);
      assert.equal(envelope.code, 200);
      assert.ok(envelope.data && typeof envelope.data === 'object' && !Array.isArray(envelope.data));
      return envelope.data;
    } catch {
      throw Error('doorbell_im_setup_request_failed');
    }
  }
  const current = await request('/api/v1/system/user/self/get-one', {});
  const userId = safeID(current.userID); // 只来自正式当前用户，不能由测试参数指定主人。
  const channelId = `${options.productId}:${options.deviceName}`;
  // bindings 当前用户查询且精确回读；免打扰不擅自关闭，不覆盖他人的绑定。
  async function bindings() {
    const result = await request('/api/v1/im/channel/get-list', {});
    assert.ok(Array.isArray(result.list) && result.list.length <= 1024, 'invalid_channel_result');
    return result.list.filter(row => row?.channelType === 'device' && row.channelId === channelId);
  }
  let channels = await bindings();
  if (channels.length === 0) {
    await request('/api/v1/im/channel/bind', {channelType: 'device', channelId, channelName: '门铃协议验证设备'});
    channels = await bindings();
  }
  assert.equal(channels.length, 1, 'unique_channel_required');
  assert.equal(safeID(channels[0].userId), userId, 'original_channel_owner_required');
  assert.equal(channels[0].isMuted, 0, 'channel_muted');
  const participants = [{type: 'user', id: String(userId)}, {type: 'device', id: channelId}];
  const created = await request('/api/v1/im/conversation/create', {type: 'single', participants});
  const conversationId = safeID(created.id);
  const detail = await request('/api/v1/im/conversation/get', {conversationId});
  assert.equal(safeID(detail.info?.id), conversationId, 'original_conversation_required');
  assert.equal(detail.info.type, 'single', 'single_conversation_required');
  assert.equal(detail.info.status, 1, 'active_conversation_required');
  const members = await request('/api/v1/im/conversation/get-members', {conversationId});
  assert.ok(Array.isArray(members.list) && members.list.length === 2, 'exact_two_members_required');
  const actual = members.list.map(member => {
    assert.equal(member.isMuted, 0, 'member_muted');
    assert.ok(typeof member.participantId === 'string', 'exact_member_identity_required');
    return `${member.participantType}:${member.participantId}`;
  }).sort();
  assert.deepEqual(actual, participants.map(member => `${member.type}:${member.id}`).sort(), 'original_im_members_required');
  return {userId: String(userId), conversationId}; // 非媒体成功结果，不返回凭据或隐含接听。
}

module.exports = {setupDoorbellIM};
