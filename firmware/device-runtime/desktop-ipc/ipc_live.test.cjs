// 文件说明：真实IPC测试入口的本地确认契约；离线断言不能替代浏览器和实际解码E2E。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
// 正式中央编排复用原run入口，不通过旧冻结包装器或另造成功应答执行验收。
test('正式IPC验收入口可显式复用且导入不启动设备', () => {
  assert.equal(typeof require('./ipc_live.cjs').run, 'function');
});
const {admissionMode, confirmAdmission, listeningMode, attachListeningAudio, sourceControlResult, sourceControlFailure} = require('./ipc_live.cjs');

// 必须是原观看腿的正式主动终态；成功RPC、别腿事件和普通挂断均不能证明DMS安全停流。
test('相机安全终态只接受原会话主动事件及对应可信原因', () => {
  const {safetyTerminal} = require('./ipc_live.cjs');
  const original = {method:'interaction.state', sessionId:'original-view', msgToken:'event-token', eventSeq:5,
    data:{state:'ended',reason:'permissionRevoked'}};
  assert.equal(safetyTerminal(original,'original-view','permissionRevoked'),true);
  assert.equal(safetyTerminal({...original,data:{state:'ended',reason:'privacyEnabled'}},'original-view','privacyEnabled'),true);
  for(const change of [{sessionId:'other-view'},{method:'interaction.result'},{code:200},{eventSeq:0},
    {eventSeq:'5'},{eventSeq:Number.MAX_SAFE_INTEGER+1},{msgToken:''},{data:{state:'established',reason:'permissionRevoked'}},
    {data:{state:'ended',reason:'userStopped'}},{data:null}])
    assert.equal(safetyTerminal({...original,...change},'original-view','permissionRevoked'),false);
  assert.equal(safetyTerminal(original,'original-view','unknown'),false);
  assert.equal(safetyTerminal(null,'original-view','permissionRevoked'),false);
});

// 助手日志不是实际执行事实；只允许明确受理标记，媒体仍需真实浏览器解码验证。
test('源RPC助手受理输出严格校验，失败和混杂尾部不能冒充成功', () => {
  sourceControlResult('{"accepted":true}\n');
  sourceControlResult('初始化日志\n{"accepted":true}\n');
  for (const output of ['', '{}', '{"accepted":false}', '{"accepted":"true"}',
    '{"accepted":true,"state":"active"}', '{"accepted":true}\n未知结果'])
    assert.throws(() => sourceControlResult(output));
});

// 子进程失败只保留固定分类，不能将SDK、SQL或密钥自由正文写进E2E结果。
test('源RPC失败诊断只允许稳定数字枚举', () => {
  for (let code=0;code<=16;code++) assert.equal(sourceControlFailure(`ipc_source_control_failed_rpc_${code}\n`),`source_control_rpc_rejected_${code}`);
  for (const output of ['', 'synthetic-secret', 'ipc_source_control_failed_rpc_17',
    'ipc_source_control_failed_rpc_3\nsynthetic-secret', 'prefix ipc_source_control_failed_rpc_3'])
    assert.equal(sourceControlFailure(output),'source_control_rpc_rejected');
});

// Chromium远端轨必须先由静音媒体元素激活解码，分析图不能形成第二条发声路径。
test('监听分析激活静音解码并精确释放音频资源', async () => {
  const calls = [], track = {};
  const sink = {play() { calls.push('play'); return Promise.resolve(); },pause(){calls.push('pause');},remove(){calls.push('remove');}};
  const graph = {connect() {},disconnect() {calls.push('disconnect');}};
  const context = vm.createContext({track, window:{}, document:{createElement(){return sink;}},
    MediaStream: class {constructor(tracks){this.tracks=tracks;}},
    AudioContext: class {constructor(){this.destination={};}
      createAnalyser(){return {...graph,getFloatTimeDomainData(){}};}
      createMediaStreamSource(){assert.equal(sink.muted,true);assert.deepEqual(calls,['play']);return graph;}
      createGain(){return {...graph,gain:{value:1}};}
      resume(){return Promise.resolve();}close(){calls.push('close');return Promise.resolve();}},
    Float32Array, Math, setInterval(){calls.push('timer');return 1;},clearInterval(){calls.push('clear');}});
  vm.runInContext(`(${attachListeningAudio.toString()})(track)`,context);
  assert.equal(sink.muted,true);assert.equal(sink.autoplay,true);
  assert.equal(context.window.__ipcAudio.gain.gain.value,0);
  track.onended();track.onended();
  assert.equal(calls.filter(x=>x==='close').length,1);
  assert.equal(calls.filter(x=>x==='clear').length,1);
  assert.equal(calls.filter(x=>x==='pause').length,1);
  assert.equal(sink.srcObject,null);
  // 自动播放被拒绝同样释放，不保留隐藏第二音路或后台采样计时器。
  calls.length=0;
  sink.play=()=>{calls.push('play');return Promise.reject(Error('synthetic_playback_denied'));};
  vm.runInContext(`(${attachListeningAudio.toString()})(track)`,context);
  await Promise.resolve();await Promise.resolve();
  assert.equal(context.window.__ipcAudio.failed,true);
  assert.equal(context.window.__ipcAudio.closed,true);
  assert.equal(calls.filter(x=>x==='close').length,1);
  assert.equal(sink.srcObject,null);
});

// 音频真实测试不能因拼写错误静默变成纯视频通过。
test('监听测试开关必须显式且不能猜测', () => {
  assert.equal(listeningMode(undefined), false);
  assert.equal(listeningMode('0'), false);
  assert.equal(listeningMode('1'), true);
  for (const value of ['', 'true', true, 1, null, '01']) assert.throws(() => listeningMode(value));
});

// 只允许已知平台策略，防止拼写错误绕过设备确认验证。
test('默认direct，明确accept，未知策略拒绝', () => {
  assert.equal(admissionMode(undefined), 'direct');
  assert.equal(admissionMode('direct'), 'direct');
  assert.equal(admissionMode('accept'), 'accept');
  for (const value of ['', 'auto', 'Accept', null, true]) assert.throws(() => admissionMode(value));
});

// 授权直连不冒充本地确认，不能自动点击接听控件。
test('direct不生成本地确认动作', async () => {
  await confirmAdmission('direct', 'session', () => { throw Error('unexpected_browser_action'); });
});

// 对讲与观看同意范围不同，必须点击对讲控件，不能把观看授权复用为麦克风/扬声器授权。
test('对讲使用独立本地同意控件，未知用途无副作用', async () => {
  const calls = [];
  await confirmAdmission('accept', 'talk-session', async (...args) => {
    calls.push(args);
    return args[0] === 'eval' ? JSON.stringify({pending:true,captured:false}) : '';
  }, 'ipcTalk');
  assert.deepEqual(calls[4], ['find','role','button','click','--name','接受本次对讲']);
  await assert.rejects(confirmAdmission('direct','session',() => {
    throw Error('unexpected_browser_action');
  },'aiConversation'));
});

// 使用含引号的会话验证脚本转义及严格的确认前检查顺序。
test('accept确认前无采集，快照后点击真实控件', async () => {
  const calls = [];
  await confirmAdmission('accept', 'session"quoted', async (...args) => {
    calls.push(args);
    return args[0] === 'eval' ? JSON.stringify({pending: true, captured: false}) : '';
  });
  assert.deepEqual(calls.map(value => value[0]), ['wait', 'eval', 'eval', 'snapshot', 'find', 'wait']);
  assert.ok(calls[0][2].includes(JSON.stringify('session"quoted')));
  assert.ok(calls[2][1].includes('scrollIntoView'));
  assert.ok(calls[2][1].includes(JSON.stringify('session"quoted')));
  assert.ok(!calls[2][1].includes('.click('));
  assert.deepEqual(calls[4], ['find', 'role', 'button', 'click', '--name', '接受本次观看']);
  assert.ok(calls[5][2].startsWith('!invitations.has('));
});

// 缺少邀请或已提前采集都必须失败，不能继续自动同意掩盖权限漏洞。
test('邀请缺失或提前采集时禁止点击', async () => {
  for (const state of [{pending:false,captured:false}, {pending:true,captured:true}]) {
    const calls = [];
    await assert.rejects(confirmAdmission('accept','session',async (...args) => {
      calls.push(args[0]);
      return args[0] === 'eval' ? JSON.stringify(state) : '';
    }));
    assert.deepEqual(calls, ['wait', 'eval']);
  }
});

// 非法会话和策略在执行任何网页命令前拒绝。
test('非法输入不触发浏览器副作用', async () => {
  for (const [mode, session] of [['accept',''], ['accept','x'.repeat(65)], ['unknown','session']]) {
    await assert.rejects(confirmAdmission(mode,session,() => { throw Error('unexpected_browser_action'); }));
  }
});

// 模拟视口外按钮：GUI点击可以返回成功但未执行，必须滚动并观察邀请移除才通过。
test('视口外确认控件先滚动，点击返回不能冒充授权事件', async () => {
  let visible = false, pending = true, clicked = false;
  await confirmAdmission('accept','third',async (...args) => {
    if (args[0] === 'eval' && args[1].includes('scrollIntoView')) visible = true;
    else if (args[0] === 'eval') return JSON.stringify({pending, captured:false,
      buttonTop:762, buttonBottom:787, viewportHeight:577});
    else if (args[0] === 'find' && visible) { pending = false; clicked = true; }
    else if (args[0] === 'wait' && args[2].startsWith('!') && pending) throw Error('click_not_delivered');
    return '';
  });
  assert.equal(clicked,true);
  assert.equal(pending,false);
});

const {prepareDmsRevocation} = require('./ipc_dms_revocation.cjs');
// dmsFixture只替代正式HTTP边界；同一设备模型保留转让及恢复事实，不伪装成真实DMS验收。
function dmsFixture() {
  const original = {productID:'p',deviceName:'camera',tenantCode:'platform',userID:'11',projectID:'21',areaID:'31',cloneID:'41'};
  const state = {device:{...original},shares:0,writes:[],lostReply:false,foreignTarget:false,large:false,defaultAgentId:0};
  const config = {baseUrl:'http://127.0.0.1:27777',token:'test-only-token',tenantCode:'platform',
    ownerUserID:'11',targetProjectID:'22',productID:'p',deviceName:'camera'};
  // request严格检查headers/路由/禁止清数据；lostReply模拟写已提交但HTTP应答丢失。
  const request = async (url, options) => {
    assert.equal(options.redirect,'error');assert.equal(options.headers.token,config.token);
    const route = new URL(url).pathname, body = JSON.parse(options.body); let data;
    if (route.endsWith('/device/info/get-one')) data = state.device;
    else if (route.endsWith('/product/info/get-one')) data = {productID:'p',defaultAgentId:state.defaultAgentId};
    else if (route.endsWith('/project/info/get-one')) data = {projectID:body.projectID,
      adminUserID:body.projectID === '21' ? '11' : '12',tenantCode:state.foreignTarget && body.projectID === '22' ? 'foreign' : 'platform'};
    else if (route.endsWith('/user/device/share/get-list')) data = {total:state.shares,list:state.shares ? [{}] : []};
    else if (route.endsWith('/device/info/transfer')) {
      assert.equal(body.isCleanData,2);assert.equal(body.transferTo,2);assert.equal(body.srcProjectID,state.device.projectID);
      state.writes.push(body);state.device = {...state.device,projectID:body.projectID,
        userID:body.projectID === '21' ? '11' : '12',areaID:body.areaID || '1'};
      if (state.lostReply) { state.lostReply = false; throw Error('private-response-must-not-leak'); }
    } else throw Error('unexpected_route');
    return new Response(JSON.stringify({code:200,data:state.large ? 'x'.repeat(1048576) : data}));
  };
  return {config,state,request,original};
}
test('DMS正式转让适配保留数据并完整恢复原归属，重复恢复无副作用', async () => {
  const f = dmsFixture(), probe = await prepareDmsRevocation(f.config,f.request);
  assert.equal(probe.cause,'permissionRevoked');assert.equal(f.state.writes.length,0);
  await probe.trigger();assert.equal(probe.facts.transferred,true);
  await assert.rejects(probe.trigger(),/trigger_once/);
  await probe.restore();await probe.restore();
  assert.deepEqual(f.state.device,f.original);assert.equal(f.state.writes.length,2);
  assert.equal(f.state.writes[1].areaID,'31');assert.equal(probe.facts.restored,true);
});
test('DMS已写入但应答丢失仍正式读回并恢复，不泄漏错误正文', async () => {
  const f = dmsFixture(), probe = await prepareDmsRevocation(f.config,f.request);
  f.state.lostReply = true;
  await assert.rejects(probe.trigger(),/^Error: dms_safety_api_failed$/);
  await probe.restore();assert.deepEqual(f.state.device,f.original);assert.equal(f.state.writes.length,2);
});
test('DMS恢复拒绝覆盖第三方新归属', async () => {
  const f = dmsFixture(), probe = await prepareDmsRevocation(f.config,f.request);
  await probe.trigger();f.state.device.userID = '99';
  await assert.rejects(probe.restore(),/restore_foreign_binding/);
  assert.equal(f.state.writes.length,1);assert.equal(probe.facts.restored,false);
});
test('DMS原主人／目标企业／分享非空／隐含新Clone在写入前拒绝', async () => {
  for (const mutate of [f => f.state.device.userID = '99',f => f.state.foreignTarget = true,
    f => f.state.shares = 1,f => { f.state.device.cloneID = '0'; f.state.defaultAgentId = 99; }]) {
    const f = dmsFixture();mutate(f);await assert.rejects(prepareDmsRevocation(f.config,f.request));
    assert.equal(f.state.writes.length,0);
  }
});
test('无默认Agent的纯IPC设备允许零Clone，仍精确恢复', async () => {
  const f = dmsFixture();f.original.cloneID = f.state.device.cloneID = '0';
  const probe = await prepareDmsRevocation(f.config,f.request);
  await probe.trigger();await probe.restore();assert.deepEqual(f.state.device,f.original);
});
test('DMS触发前重新核验原归属，不把预检当长期授权', async () => {
  const f = dmsFixture(), probe = await prepareDmsRevocation(f.config,f.request);
  f.state.device.areaID = '999';await assert.rejects(probe.trigger(),/original_changed/);
  assert.equal(f.state.writes.length,0);
});
test('DMS来源／ID／响应大小严格有界', async () => {
  for (const change of [{baseUrl:'http://example.com'},{baseUrl:'http://127.0.0.1/?token=private'},
    {ownerUserID:11},{targetProjectID:'9223372036854775808'},{deviceName:'../camera'}]) {
    const f = dmsFixture();await assert.rejects(prepareDmsRevocation({...f.config,...change},f.request));
    assert.equal(f.state.writes.length,0);
  }
  const f = dmsFixture();f.state.large = true;
  await assert.rejects(prepareDmsRevocation(f.config,f.request),/^Error: dms_safety_api_failed$/);
  assert.equal(f.state.writes.length,0);
});
