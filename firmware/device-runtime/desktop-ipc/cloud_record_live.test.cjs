// 文件说明：云录制真实验收入口的边界判据单测；离线通过不能替代真正C++媒体及私有文件E2E。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {configuration, contentMetadata, decodedPCM, retiredPrivateURL, probe, run} = require('./cloud_record_live.cjs');
const {createHash}=require('node:crypto');

test('导入不启动网络或设备，录制配置须显式本机带音频', () => {
  assert.equal(typeof probe, 'function'); assert.equal(typeof run, 'function');
  const env = {UR_CLOUD_RECORD_LIVE: '1', UR_IPC_LISTEN_AUDIO: '1', UR_IPC_BASE_URL: 'http://127.0.0.1:10000/', UR_CLOUD_RECORD_POLICY_ID: 'policy'};
  assert.deepEqual(configuration(env), {policy: 'policy'});
  for (const change of [{UR_CLOUD_RECORD_LIVE: '0'}, {UR_IPC_LISTEN_AUDIO: '0'}, {UR_CLOUD_RECORD_POLICY_ID: ''},
    {UR_IPC_BASE_URL: 'http://external.example/'}, {UR_IPC_BASE_URL: 'https://127.0.0.1/'}, {UR_IPC_BASE_URL: 'http://127.0.0.1/x'}])
    assert.throws(() => configuration({...env, ...change}));
});

test('私有内容必须短期、有界、实际摘要一致且限定隔离服务地址', () => {
  const digest = 'a'.repeat(64), now = 100000;
  const event = {method: 'interaction.result', code: 200, data: {operation: 'interaction.control', command: 'recordingContent', recordingId: 'rec',
    state: 'ready', sha256: digest, contentRef: `recording:${'b'.repeat(64)}:${digest}`, size: 42, durationMs: 1000,
    signedURL: 'http://127.0.0.1/private?signature=synthetic', expiresAt: now + 30000}};
  assert.equal(contentMetadata(event, 'rec', now).size, 42);
  for (const change of [{state: 'failed'}, {sha256: 'f'.repeat(64)}, {size: 0}, {size: 2 ** 31},
    {contentRef: `recording:../other:${digest}`}, {expiresAt: now}, {expiresAt: now + 60000}, {durationMs: 0},
    {signedURL: 'file:///private'}, {signedURL: 'http://external.example/private'}, {signedURL: 'http://user:password@127.0.0.1/private'}])
    assert.throws(() => contentMetadata({...event, data: {...event.data, ...change}}, 'rec', now));
  assert.throws(() => contentMetadata({...event, code: 403}, 'rec', now));
  assert.throws(() => contentMetadata(event, 'other', now));
});

test('声音判据必须检查真实s16le样本而非仅容器长度', () => {
  assert.throws(() => decodedPCM(Buffer.alloc(3840)));
  assert.throws(() => decodedPCM(Buffer.alloc(3839)));
  const samples = Buffer.alloc(9600);
  for (let index = 0; index < samples.length / 2; index++) samples.writeInt16LE(Math.round(Math.sin(index * 2 * Math.PI * 440 / 48000) * 4000), index * 2);
  const result = decodedPCM(samples);
  assert.equal(result.samples, 4800); assert.ok(result.rms > 0.01 && result.peak > 0.1);
});

test('撤权必须验证同一仍有效签名原文件不可下载，网络未知和自然签名过期不能冒充删除',async()=>{
  const body=Buffer.from('original-real-artifact-fixture');
  const content={signedURL:'http://127.0.0.1/private?sign=synthetic',expiresAt:Date.now()+30000,size:body.length,sha256:createHash('sha256').update(body).digest('hex')};
  let calls=0;
  const result=await retiredPrivateURL(content,async()=>++calls===1?new Response(body,{status:200}):new Response('removed',{status:404}));
  assert.equal(calls,2);assert.deepEqual(result,{status:404,bytes:7,originalArtifactUnavailable:true});
  await assert.rejects(()=>retiredPrivateURL({...content,expiresAt:Date.now()+500},async()=>new Response('',{status:404})));
  await assert.rejects(()=>retiredPrivateURL(content,async()=>{throw Error('transport-unknown');}));
  await assert.rejects(()=>retiredPrivateURL(content,async()=>new Response(body,{status:200}),100));
});
