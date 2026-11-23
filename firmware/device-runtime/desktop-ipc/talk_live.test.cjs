// 文件说明：真实对讲测试入口的配置安全与异步事实契约；不替代正式MQTT/IM媒体E2E。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {validateConfiguration,waitFor,validateTalkOptions} = require('./talk_live.cjs');

test('正式安全验收只允许明确原因和原触发器，不以任意本地操作充当终态', () => {
  validateTalkOptions({}); validateTalkOptions({onReady: async () => {}});
  for (const cause of ['privacyEnabled', 'permissionRevoked'])
    validateTalkOptions({safety: {cause, trigger: async () => {}, restore: async () => {}}});
  for (const options of [null, [], {unknown: true}, {onReady: true}, {safety: null}, {safety: []},
    {safety: {cause: 'userStopped', trigger: async () => {}}}, {safety: {cause: 'privacyEnabled'}},
    {safety: {cause: 'privacyEnabled', trigger: true}}, {safety: {cause: 'privacyEnabled', trigger: async () => {}, restore: true}},
    {safety: {cause: 'privacyEnabled', trigger: async () => {}, inject: true}}])
    assert.throws(() => validateTalkOptions(options));
});

// 正式测试必须显式启用并注入全部身份，不能因错配置绕过对讲或音频验收。
test('真实对讲只允许明确开关、回环入口、存在的夹具与设备身份', () => {
  const env = {UR_IPC_TALK_LIVE:'1',UR_IPC_BASE_URL:'http://127.0.0.1:22222',UR_IPC_DEVICE_PORT:'22223',
    UR_IPC_DEVICE_BIN:process.execPath,UR_IPC_AUDIO_FIXTURE:__filename,UR_IPC_APP_TOKEN:'test-only',
    UR_DEVICE_PRODUCT:'test',UR_DEVICE_NAME:'test',UR_DEVICE_SECRET:'test-only',UR_IPC_CONFERENCE_TALK:'1',UR_IPC_MICROPHONE:'1'};
  assert.equal(validateConfiguration(env),22223);
  assert.equal(validateConfiguration({...env,UR_IPC_TALK_PREVIEW_COUNT:'3'}),22223);
  for (const value of ['', '1', '2', '4', 'true']) assert.throws(() => validateConfiguration({...env,UR_IPC_TALK_PREVIEW_COUNT:value}));
  const speaker = {...env,UR_IPC_SPEAKER_CONTROL:'1',UR_IPC_SOURCE_CONTROL_BIN:process.execPath,
    UR_IPC_SOURCE_CONTROL_RPC:'127.0.0.1:22224',UR_IPC_TENANT:'test'};
  assert.equal(validateConfiguration(speaker),22223);
  assert.throws(() => validateConfiguration({...speaker,UR_IPC_TALK_PREVIEW_COUNT:'3'}));
  for (const [key,value] of [['UR_IPC_SPEAKER_CONTROL','true'],['UR_IPC_SOURCE_CONTROL_BIN','/does-not-exist'],
    ['UR_IPC_SOURCE_CONTROL_RPC','example.invalid:22224'],['UR_IPC_SOURCE_CONTROL_RPC','127.0.0.1:1'],
    ['UR_IPC_SOURCE_CONTROL_RPC','127.0.0.1:65536'],['UR_IPC_TENANT','']])
    assert.throws(() => validateConfiguration({...speaker,[key]:value}));
  for (const [key,value] of [['UR_IPC_TALK_LIVE','true'],['UR_IPC_CONFERENCE_TALK','0'],['UR_IPC_MICROPHONE','0'],
    ['UR_IPC_DEVICE_PORT','1'],['UR_IPC_DEVICE_PORT','1.5'],['UR_IPC_DEVICE_PORT','65536'],['UR_DEVICE_SECRET',''],
    ['UR_IPC_AUDIO_FIXTURE','/does-not-exist'],['UR_IPC_APP_TOKEN',''],
    ...['http://example.invalid:22222','https://127.0.0.1:22222','http://user@127.0.0.1:22222',
      'http://127.0.0.1:22222/path','http://127.0.0.1:22222/?token=test'].map(value => ['UR_IPC_BASE_URL',value])])
    assert.throws(() => validateConfiguration({...env,[key]:value}));
});

// false异步返回必须持续等待，不把Promise对象当成成功；异常与截止都向上返回。
test('异步条件等待真正事实，失败和截止不能冒充成功', async () => {
  let count = 0;
  await waitFor(async () => ++count === 2,1000);
  assert.equal(count,2);
  await assert.rejects(waitFor(async () => false,0),/actual_state_timeout/);
  await assert.rejects(waitFor(async () => {throw Error('synthetic_condition_error');}),/synthetic_condition_error/);
});
