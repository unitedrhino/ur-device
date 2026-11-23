// 文件说明：门铃真实E2E入口离线契约回归；不执行浏览器、API或C++进程，不称媒体已验证。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {validateConfiguration, validateInvite, mediaPass, waitFor, uniqueAnswer} = require('./doorbell_live.cjs');

// configuration 仅提供占位隔离环境；文件判断由用例注入，不访问真实凭据或启动进程。
function configuration() {
  return {UR_DOORBELL_LIVE:'1',UR_IPC_BASE_URL:'http://127.0.0.1:18080',UR_IPC_DEVICE_PORT:'18180',
    UR_IPC_DEVICE_BIN:'placeholder-binary',UR_IPC_AUDIO_FIXTURE:'placeholder-pcm.wav',
    UR_IPC_APP_TOKEN:'placeholder-token',UR_DEVICE_PRODUCT:'example',UR_DEVICE_NAME:'doorbell',
    UR_DEVICE_SECRET:'placeholder-secret',UR_IPC_TENANT:'example',UR_IPC_APP_ID:'100',
    UR_IPC_CAMERA_ID:'entry',UR_IPC_DOORBELL_EVENT:'doorbellPress',UR_IPC_HUMAN_CALL:'1',
    UR_IPC_CONFERENCE_TALK:'1',UR_IPC_MICROPHONE:'1'};
}
// invitation 固定APP三轨原方向；不能借旧单视频通知获得麦克风。
function invitation() {
  const tracks = [
    {trackId:'mic-1',kind:'audio',sourceId:'mic',direction:'sendonly',codec:'opus',state:'requested'},
    {trackId:'speaker-1',kind:'audio',sourceId:'speaker',direction:'recvonly',codec:'opus',state:'requested'},
    {trackId:'video-1',kind:'video',sourceId:'entry',sourceType:'camera',direction:'recvonly',codec:'H264',state:'requested'},
  ];
  return {method:'interaction.invite',msgToken:'original-invite',sessionId:'candidate-1',data:{endpointId:'endpoint-1',
    callGroupId:'group',invitation:{purpose:'humanCall',callType:'video',admission:'accept',transport:'webrtc',
      expiresAt:Date.now()+1000000,sourceEventId:'original-event',callGroupId:'group',tracks}}};
}
// facts 表示双方实际解码门槛，不能由协议ACK或媒体包数量单独满足。
function facts() {
  return [{human:true,active:true,connected:true,playing:true,frames:3,audioSent:5,audioReceived:5,audioSamples:2500,audioEnergy:1},
    {connected:true,samples:2500,sent:5,frames:3,nonBlackPixels:100,peak:0.1,run:3,closed:false,failed:false},
    {peak:0.1,run:3,closed:false,failed:false}];
}

test('真实门铃入口默认不运行，必须显式本机与三项实际驱动', () => {
  assert.equal(validateConfiguration(configuration(), () => true),18180);
  for (const key of ['UR_DOORBELL_LIVE','UR_IPC_HUMAN_CALL','UR_IPC_CONFERENCE_TALK','UR_IPC_MICROPHONE',
    'UR_IPC_CAMERA_ID','UR_IPC_DOORBELL_EVENT','UR_IPC_TENANT','UR_IPC_APP_ID']) {
    const env = configuration(); delete env[key];
    assert.throws(() => validateConfiguration(env, () => true));
  }
});

test('并发接听必须显式开启，不按端点顺序预定胜者', () => {
  assert.equal(validateConfiguration({...configuration(),UR_DOORBELL_CONCURRENT_ACCEPT:'1'},()=>true),18180);
  assert.throws(()=>validateConfiguration({...configuration(),UR_DOORBELL_CONCURRENT_ACCEPT:'0'},()=>true));
  const won = {code:200,data:{result:{state:'negotiating'}}};
  const lost = {code:409,data:{reason:'alreadyAnswered'}};
  for (let winner=0;winner<3;winner++) {
    const results=[lost,lost,lost];results[winner]=won;assert.equal(uniqueAnswer(results),winner);
  }
  for (const results of [[lost,lost,lost],[won,won,lost],[won,lost],
    [won,lost,{code:500}],[won,lost,{code:409,data:{reason:'notAuthorized'}}],
    [{code:200,data:{result:{state:'established'}}},lost,lost]]) assert.throws(()=>uniqueAnswer(results));
});

test('免打扰与真实超时另行显式开启，不能用任意环境值静默触发',()=>{
  assert.equal(validateConfiguration({...configuration(),UR_DOORBELL_NEGATIVE_CASES:'1'},()=>true),18180);
  for(const value of ['0','true','yes',''])assert.throws(()=>validateConfiguration({...configuration(),UR_DOORBELL_NEGATIVE_CASES:value},()=>true));
});

test('真实门铃禁止远端凭据地址与不存在的媒体夹具', () => {
  for (const value of ['https://127.0.0.1:18080','http://example.invalid','http://user:password@127.0.0.1:18080',
    'http://127.0.0.1:18080/?token=example']) {
    assert.throws(() => validateConfiguration({...configuration(),UR_IPC_BASE_URL:value}, () => true));
  }
  assert.throws(() => validateConfiguration(configuration(), () => false));
  assert.throws(() => validateConfiguration({...configuration(),UR_IPC_DEVICE_PORT:'0'}, () => true));
});

test('APP标准三轨原邀请能验收，旧视频单轨不能静默升级', () => {
  const event = invitation();
  assert.equal(validateInvite(event,'endpoint-1','entry'),event);
  event.data.invitation.tracks = [event.data.invitation.tracks[2]];
  assert.throws(() => validateInvite(event,'endpoint-1','entry'));
});

test('APP错误方向相机或额外身份字段均不能获得媒体授权', () => {
  for (const mutate of [
    e => {e.data.endpointId='other';}, e => {e.data.invitation.admission='direct';},
    e => {e.data.invitation.tracks[2].direction='sendrecv';}, e => {e.data.invitation.tracks[2].sourceId='front';},
    e => {e.data.invitation.tracks[0].sourceType='camera';}, e => {delete e.data.invitation.tracks[2].sourceType;},
    e => {e.data.invitation.callGroupId='changed';}, e => {e.msgToken='';},
  ]) {
    const event = invitation(); mutate(event);
    assert.throws(() => validateInvite(event,'endpoint-1','entry'));
  }
});

test('SDP和connected不能代替双向实际PCM及胜者视频像素', () => {
  assert.equal(mediaPass(...facts()),true);
  for (const [group, field, value] of [[0,'active',false],[0,'playing',false],[0,'audioSamples',0],
    [0,'audioEnergy',0],[0,'frames',0],[1,'samples',0],[1,'peak',0],[1,'nonBlackPixels',0],
    [1,'frames',0],[2,'peak',0],[2,'run',1]]) {
    const values = facts(); values[group][field]=value;
    assert.equal(mediaPass(...values),false);
  }
  assert.equal(mediaPass(undefined,undefined,undefined),false);
});

test('分析器已关闭或失败后保留的历史能量不能通过真实媒体验收', () => {
  for (const group of [1, 2]) for (const field of ['closed', 'failed']) {
    const values = facts(); values[group][field] = true;
    assert.equal(mediaPass(...values), false);
  }
});

test('过期邀请或空原事件身份不能混入下一轮', () => {
  const expired = invitation(); expired.data.invitation.expiresAt = 1000;
  assert.throws(() => validateInvite(expired, 'endpoint-1', 'entry', 1001));
  assert.equal(validateInvite(expired, 'endpoint-1', 'entry', 999), expired);
  for (const field of ['callGroupId', 'sourceEventId']) {
    const empty = invitation(); empty.data.invitation[field] = '';
    if (field === 'callGroupId') empty.data.callGroupId = '';
    assert.throws(() => validateInvite(empty, 'endpoint-1', 'entry', 0));
  }
});

test('事实等待使用独立截止，不能用未await Promise视为通过', async () => {
  let attempts = 0;
  await waitFor(async () => ++attempts === 2, 500);
  assert.equal(attempts,2);
  await assert.rejects(waitFor(async () => false, 1),{message:'doorbell_fact_timeout'});
});
