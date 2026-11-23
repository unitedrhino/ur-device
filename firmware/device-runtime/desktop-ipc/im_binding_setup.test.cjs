// 文件说明：门铃正式IM前置的离线请求/回读契约，不启动网络或把夹具授权称为真实通话。
'use strict';
const assert = require('node:assert/strict');
const {test} = require('node:test');
const {setupDoorbellIM} = require('./im_binding_setup.cjs');

// fixture 只模拟明确HTTP响应，记录请求证明原身份来源和无隐式解除免打扰。
function fixture(change = {}) {
  const requests = [];
  const channel = {channelType: 'device', channelId: 'product:device', userId: 77, isMuted: 0};
  const values = {
    '/api/v1/system/user/self/get-one': {userID: 77},
    '/api/v1/im/channel/get-list': {list: [channel]},
    '/api/v1/im/conversation/create': {id: 88},
    '/api/v1/im/conversation/get': {info: {id: 88, type: 'single', status: 1}},
    '/api/v1/im/conversation/get-members': {list: [
      {participantType: 'user', participantId: '77', isMuted: 0},
      {participantType: 'device', participantId: 'product:device', isMuted: 0}]},
    ...change,
  };
  return {requests, values, transport: async (url, options) => {
    const path = new URL(url).pathname;
    requests.push({path, method: options.method, body: JSON.parse(options.body)});
    assert.ok(Object.hasOwn(values, path), 'unexpected_http_path');
    return {ok: true, text: async () => JSON.stringify({code: 200, data: values[path]})};
  }};
}
// options 没有模型/调用者自报user字段，主人来自正式self/get-one。
function options() {
  return {live: '1', base: 'http://127.0.0.1:10000', token: 'synthetic-test-token', tenant: 'test',
    appId: 'test-app', productId: 'product', deviceName: 'device'};
}

test('读取正式用户并建立精确两成员单聊，回读原渠道与会话', async () => {
  const value = fixture();
  assert.deepEqual(await setupDoorbellIM(options(), value.transport), {userId: '77', conversationId: 88});
  assert.ok(value.requests.every(row => row.method === 'POST'));
  const create = value.requests.find(row => row.path.endsWith('/create'));
  assert.deepEqual(create.body, {type: 'single', participants: [{type: 'user', id: '77'}, {type: 'device', id: 'product:device'}]});
  assert.equal(value.requests.length, 5);
});

test('未绑定只经正式bind并再次读取，不把写入ACK当已绑定', async () => {
  const value = fixture({'/api/v1/im/channel/get-list': {list: []}, '/api/v1/im/channel/bind': {}});
  const transport = async (url, request) => {
    if (new URL(url).pathname.endsWith('/bind')) {
      value.values['/api/v1/im/channel/get-list'] = {list: [{channelType: 'device', channelId: 'product:device', userId: 77, isMuted: 0}]};
    }
    return value.transport(url, request);
  };
  await setupDoorbellIM(options(), transport);
  assert.equal(value.requests.filter(row => row.path.endsWith('/get-list')).length, 2);
  assert.equal(value.requests.filter(row => row.path.endsWith('/bind')).length, 1);
});

test('bind成功但回读仍缺绑定时拒绝创建通话', async () => {
  const value = fixture({'/api/v1/im/channel/get-list': {list: []}, '/api/v1/im/channel/bind': {}});
  await assert.rejects(setupDoorbellIM(options(), value.transport), /unique_channel_required/);
  assert.ok(value.requests.every(row => !row.path.endsWith('/create')));
});

test('关闭开关或远程地址禁止任何HTTP调用', async () => {
  for (const changes of [{live: '0'}, {base: 'http://example.invalid'}, {base: 'http://private:secret@localhost'}, {base: 'https://localhost'}, {deviceName: '+'}]) {
    const value = fixture();
    await assert.rejects(setupDoorbellIM({...options(), ...changes}, value.transport));
    assert.equal(value.requests.length, 0);
  }
});

test('免打扰/错主人/重复渠道在创建会话前拒绝', async () => {
  for (const list of [
    [{channelType: 'device', channelId: 'product:device', userId: 77, isMuted: 1}],
    [{channelType: 'device', channelId: 'product:device', userId: 78, isMuted: 0}],
    [1, 2].map(() => ({channelType: 'device', channelId: 'product:device', userId: 77, isMuted: 0})),
  ]) {
    const value = fixture({'/api/v1/im/channel/get-list': {list}});
    await assert.rejects(setupDoorbellIM(options(), value.transport));
    assert.ok(value.requests.every(row => !row.path.endsWith('/bind') && !row.path.endsWith('/create')));
  }
});

test('不安全用户ID不会被取整写入另一身份', async () => {
  for (const userID of [9007199254740992, '9007199254740992', '0077', null]) {
    const value = fixture({'/api/v1/system/user/self/get-one': {userID}});
    await assert.rejects(setupDoorbellIM(options(), value.transport));
    assert.equal(value.requests.length, 1);
  }
});

test('回读必须是原活跃两成员，而非创建ACK即完成', async () => {
  const cases = [
    {'/api/v1/im/conversation/get': {info: {id: 89, type: 'single', status: 1}}},
    {'/api/v1/im/conversation/get': {info: {id: 88, type: 'group', status: 1}}},
    {'/api/v1/im/conversation/get': {info: {id: 88, type: 'single', status: 0}}},
    {'/api/v1/im/conversation/get-members': {list: []}},
    {'/api/v1/im/conversation/get-members': {list: [{participantType: 'user', participantId: '77', isMuted: 0}, {participantType: 'user', participantId: '78', isMuted: 0}]}},
  ];
  for (const changes of cases) {
    const value = fixture(changes);
    await assert.rejects(setupDoorbellIM(options(), value.transport));
  }
});

test('私有传输和响应错误不进入报告', async () => {
  await assert.rejects(setupDoorbellIM(options(), async () => {throw Error('synthetic-private-address');}), /^Error: doorbell_im_setup_request_failed$/);
  await assert.rejects(setupDoorbellIM(options(), async () => ({ok: true, text: async () => JSON.stringify({code: 403, msg: 'synthetic-private-address'})})), /^Error: doorbell_im_setup_request_failed$/);
});
