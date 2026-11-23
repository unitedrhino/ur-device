// 文件说明：隐私正式验收编排的离线契约；合成页面／属性不作为真实采集停流证据。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {privacyClickSnapshot, privacyValue, preparePrivacy, privacyConfiguration, privacyReadAPI} = require('./ipc_privacy_live.cjs');
const vm = require('node:vm');

test('点击证据按原按钮独立且只观察事件，不调用控件或设备出口', () => {
  const handlers = new Map(), buttons = new Map(['privacy-enable', 'privacy-disable'].map(id => [id,
    {addEventListener(type, handler, options) {assert.equal(type, 'click'); assert.deepEqual({...options}, {capture: true, passive: true});
      assert.equal(handlers.has(id), false); handlers.set(id, handler);}}]));
  const context = vm.createContext({window: {}, document: {getElementById: id => buttons.get(id)}});
  const inspect = id => JSON.parse(vm.runInContext(`JSON.stringify((${privacyClickSnapshot.toString()})(${JSON.stringify(id)}))`, context));
  assert.deepEqual(inspect('privacy-enable'), {count: 0, trusted: false});
  assert.deepEqual(inspect('privacy-enable'), {count: 0, trusted: false});
  handlers.get('privacy-enable')({isTrusted: true});
  assert.deepEqual(inspect('privacy-enable'), {count: 1, trusted: true});
  assert.deepEqual(inspect('privacy-disable'), {count: 0, trusted: false});
  handlers.get('privacy-enable')({isTrusted: false});
  assert.deepEqual(inspect('privacy-enable'), {count: 2, trusted: false});
  assert.throws(() => inspect('other'));
});
// row 构造原设备良好物模型记录，坏质量／旧时间单独覆盖。
const row = (enabled, stamp = Date.now()) => ({deviceName: 'device', dataID: 'privacyMode',
  quality: 'GOOD', value: enabled ? '1' : '0', receivedTime: stamp, timestamp: String(stamp)});

// liveEnv 只构造合成启动参数，不包含真实凭据或可连接的测试后端。
const liveEnv = () => ({UR_IPC_LIVE: '1', UR_IPC_PRIVACY_LIVE: '1', UR_IPC_PRIVACY_REPORT: '1',
  UR_IPC_BASE_URL: 'http://127.0.0.1:22222', UR_DEVICE_PRODUCT: 'product', UR_DEVICE_NAME: 'device',
  UR_IPC_APP_TOKEN: 'test-only', UR_IPC_TENANT: 'test', UR_IPC_APP_ID: '100'});

test('独立真实入口必须显式启用并固定回环地址和注入身份', () => {
  assert.deepEqual(privacyConfiguration(liveEnv()), {base: 'http://127.0.0.1:22222', device: {productID: 'product', deviceName: 'device'}, scenario: 'preview'});
  assert.equal(privacyConfiguration({...liveEnv(), UR_IPC_PRIVACY_SCENARIO: 'talk'}).scenario, 'talk');
  assert.equal(privacyConfiguration({...liveEnv(), UR_IPC_PRIVACY_SCENARIO: 'combined', UR_IPC_TALK_PREVIEW_COUNT: '3'}).scenario, 'combined');
  for (const change of [{UR_IPC_PRIVACY_SCENARIO: 'combined'}, {UR_IPC_PRIVACY_SCENARIO: 'talk', UR_IPC_TALK_PREVIEW_COUNT: '3'},
    {UR_IPC_TALK_PREVIEW_COUNT: '3'}]) assert.throws(() => privacyConfiguration({...liveEnv(), ...change}));
  for (const [key, value] of [['UR_IPC_LIVE', '0'], ['UR_IPC_PRIVACY_LIVE', 'true'], ['UR_IPC_PRIVACY_REPORT', undefined],
    ['UR_DEVICE_PRODUCT', '../other'], ['UR_DEVICE_NAME', ''], ['UR_IPC_APP_TOKEN', ''], ['UR_IPC_APP_TOKEN', 'x'.repeat(8193)],
    ['UR_IPC_TENANT', ''], ['UR_IPC_APP_ID', 'a/b'], ['UR_IPC_PRIVACY_SCENARIO', 'other'],
    ...['http://example.invalid:22222', 'https://127.0.0.1:22222', 'http://127.0.0.1:80',
      'http://user@127.0.0.1:22222', 'http://127.0.0.1:22222/path', 'http://127.0.0.1:22222/?token=test',
      'http://127.0.0.1:22222/#other'].map(value => ['UR_IPC_BASE_URL', value])])
    assert.throws(() => privacyConfiguration({...liveEnv(), [key]: value}));
});

test('正式属性适配只读且有界，错误响应不能伪装为当前事实', async t => {
  const calls = [];
  let status = 200, code = 200;
  t.mock.method(globalThis, 'fetch', async (url, request) => {
    calls.push({url, request}); return new Response(JSON.stringify({code, data: {list: []}}), {status});
  });
  const env = liveEnv(), configuration = privacyConfiguration(env), api = privacyReadAPI(env, configuration);
  for (const route of ['things/device/schema/create', 'im/interaction/commands', '../system/user'])
    await assert.rejects(api(route, configuration.device), /privacy_readonly_route_required/);
  assert.equal(calls.length, 0);
  assert.deepEqual(await api('things/device/msg/property-latest/get-list', configuration.device), {list: []});
  assert.equal(calls[0].url, 'http://127.0.0.1:22222/api/v1/things/device/msg/property-latest/get-list');
  assert.equal(calls[0].request.redirect, 'error');
  assert.equal(calls[0].request.headers.token, env.UR_IPC_APP_TOKEN);
  assert.deepEqual(JSON.parse(calls[0].request.body), configuration.device);
  for (const failure of [{status: 403, code: 200}, {status: 200, code: 100007}]) {
    ({status, code} = failure);
    await assert.rejects(api('things/device/schema/tsl-read', configuration.device), /privacy_readonly_api_rejected/);
  }
});

test('属性只接受唯一原设备良好布尔记录，空值不等于关闭', () => {
  assert.equal(privacyValue({list: []}, 'device'), null);
  assert.equal(privacyValue({list: [row(true)]}, 'device').enabled, true);
  assert.equal(privacyValue({list: [row(false)]}, 'device').enabled, false);
  for (const change of [{deviceName: 'other'}, {dataID: 'other'}, {quality: 'BAD'}, {quality: undefined},
    {value: 0}, {value: null}, {value: '0.1'}, {value: 'False'}, {timestamp: 1}, {timestamp: '1.1'},
    {receivedTime: 0}, {receivedTime: Number.MAX_SAFE_INTEGER + 1}])
    assert.throws(() => privacyValue({list: [{...row(false), ...change}]}, 'device'));
  for (const list of [null, {}, [row(false), row(false)]]) assert.throws(() => privacyValue({list}, 'device'));
});
test('正式单设备latest接口可省略设备名，归属由已鉴权原请求固定而非响应自报',()=>{
  const original=row(false);delete original.deviceName;
  assert.equal(privacyValue({list:[original]},'device').enabled,false);
  for(const deviceName of ['',null,'other'])assert.throws(()=>privacyValue({list:[{...original,deviceName}]},'device'));
});

// fixture 用可控单调时钟观察按钮次数；没有媒体驱动、云端写入或真实等待。
function fixture(options = {}) {
  let current = options.empty ? null : row(!!options.enabled), clock = 0;
  let stop = options.stopped !== false, ack = true, ready = true;
  let clickCount = 0; // 浏览器合成夹具只核编排，真实可信事件另由原网页E2E验证。
  const clicks = [], calls = [], commands = [];
  const api = async (route, body) => {
    calls.push({route, body});
    if (route === 'things/device/schema/tsl-read') return {tsl: JSON.stringify({properties:
      options.properties || [{identifier: 'privacyMode', define: {type: 'bool'}}]})};
    assert.equal(route, 'things/device/msg/property-latest/get-list');
    return {list: current ? [current] : []};
  };
  const execute = async (...args) => {
    commands.push(args);
    if (args[0] === 'find') {
      const enabled = args.at(-1) === '上报隐私开启'; clicks.push(enabled);
      if (options.missedClick) return '';
      clickCount++;
      current = row(enabled, options.stale ? 1 : Date.now());
      ack = !options.lostAck;
      return '';
    }
    if (args[0] === 'snapshot' || args[0] === 'scrollintoview') return '';
    assert.equal(args[0], 'eval');
    if (args[1].includes('privacyClickSnapshot')) return JSON.stringify({count: clickCount, trusted: options.untrustedClick !== true});
    return JSON.stringify(args[1].includes('__ipcAllTracks') ? stop :
      args[1].includes('已获平台确认') ? ack : ready);
  };
  return {config: {api, device: {productID: 'product', deviceName: 'device'}, allowInitialize: !!options.allowInitialize,
    now: () => clock, sleep: async ms => {clock += ms;}}, execute, clicks, calls, commands,
    setReady(value) {ready = value;}, setStopped(value) {stop = value;}};
}

test('缺定义、错类型或重复定义不能注册或报告属性', async () => {
  for (const properties of [[], [{identifier: 'privacyMode', define: {type: 'int'}}],
    [{identifier: 'privacyMode', define: {type: 'bool'}}, {identifier: 'privacyMode', define: {type: 'bool'}}]]) {
    const f = fixture({properties}); await assert.rejects(preparePrivacy(f.config));
    assert.equal(f.clicks.length, 0); assert.equal(f.calls.length, 1);
  }
});

test('原值开启不得静默关闭；新夹具空值必须明确允许初始化', async () => {
  for (const options of [{enabled: true}, {empty: true}]) {
    const f = fixture(options); await assert.rejects(preparePrivacy(f.config)); assert.equal(f.clicks.length, 0);
  }
  const f = fixture({empty: true, allowInitialize: true}); const probe = await preparePrivacy(f.config);
  assert.equal(probe.facts.originalOffVerified, false); await probe.ready({execute: f.execute});
  assert.deepEqual(f.clicks, [false]); assert.equal(probe.facts.initializedByCpp, true);
});

test('原页面一次上报、正式新时间读回、实际旧轨检查与幂等恢复均不可省略', async () => {
  const f = fixture(), probe = await preparePrivacy(f.config);
  await assert.rejects(probe.trigger()); await probe.restore(); assert.equal(f.clicks.length, 0);
  await probe.ready({execute: f.execute}); await assert.rejects(probe.ready({execute: f.execute}));
  await probe.trigger(); await assert.rejects(probe.trigger()); await probe.restore(); await probe.restore();
  assert.deepEqual(f.clicks, [false, true, false]);
  assert.deepEqual(probe.facts, {registeredBoolVerified: true, originalOffVerified: true, initializedByCpp: true,
    enableReportVerified: true, originalOffRestored: true, oldMediaNotResumed: true});
  assert.ok(f.commands.filter(c => c[0] === 'eval').every(c => !/\.stop\(|\.close\(|send\(/.test(c[1])));
});

test('旧属性时间或原ACK丢失不能冒充新报告成功，也不重复点击', async () => {
  for (const options of [{stale: true}, {lostAck: true}]) {
    const f = fixture(options), probe = await preparePrivacy(f.config);
    await assert.rejects(probe.ready({execute: f.execute}));
    assert.deepEqual(f.clicks, [false]); assert.equal(probe.facts.initializedByCpp, false);
  }
});

test('角色点击未触发或脚本点击不能借上一条ACK冒充原报告', async () => {
  for (const options of [{missedClick: true}, {untrustedClick: true}]) {
    const f = fixture(options), probe = await preparePrivacy(f.config);
    await assert.rejects(probe.ready({execute: f.execute}), /privacy_original_click_required/);
    assert.deepEqual(f.clicks, [false]); assert.equal(probe.facts.initializedByCpp, false);
  }
});

test('属性确认不是实际停止证明；恢复失败不得声明无自动复流', async () => {
  const f = fixture({stopped: false}), probe = await preparePrivacy(f.config);
  await probe.ready({execute: f.execute}); await probe.trigger(); await assert.rejects(probe.restore());
  // 外层finally再次恢复须保留原失败，而不是再次产生一个相反值的新token。
  await assert.rejects(probe.restore());
  assert.equal(probe.facts.oldMediaNotResumed, false); assert.deepEqual(f.clicks, [false, true, false]);
});

test('出口未就绪有界超时不产生新报告，导入不启动真实平台', async () => {
  const f = fixture(), probe = await preparePrivacy(f.config); f.setReady(false);
  await assert.rejects(probe.ready({execute: f.execute})); assert.deepEqual(f.clicks, []);
  assert.equal(probe.facts.initializedByCpp, false);
});

test('对讲作用域核一个原Peer及实际采集轨，未知作用域在读取平台前拒绝', async () => {
  const f = fixture();
  await assert.rejects(preparePrivacy({...f.config, mediaScope: 'other'}), /privacy_media_scope_invalid/);
  assert.equal(f.calls.length, 0);
  const probe = await preparePrivacy({...f.config, mediaScope: 'talk'});
  await probe.ready({execute: f.execute}); await probe.trigger(); await probe.restore();
  assert.equal(probe.facts.oldMediaNotResumed, true);
  const checks = f.commands.filter(command => command[0] === 'eval' && command[1].includes('__ipcAllTracks'));
  assert.equal(checks.length, 2);
  for (const command of checks) {
    assert.ok(command[1].includes('__ipcAllPeers?.length===1'));
    assert.ok(command[1].includes('__ipcAllTracks?.length>=1'));
    assert.ok(command[1].includes('readyState==="ended"') && command[1].includes('connectionState==="closed"'));
  }
});
