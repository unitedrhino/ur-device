// 文件说明：真实C++设备→Pion云录制→正式私有内容读取验收；复用IPC核心流程，不伪造同意或媒体状态。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {randomUUID, createHash} = require('node:crypto');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const ipc = require('./ipc_live.cjs');

// configuration 在任何网络/进程副作用之前要求本机隔离和显式带音频录制，策略不可从媒体成功推断。
function configuration(env) {
  assert.equal(env.UR_CLOUD_RECORD_LIVE, '1', 'explicit_cloud_record_required');
  assert.equal(env.UR_IPC_LISTEN_AUDIO, '1', 'actual_audio_record_required');
  const base = new URL(env.UR_IPC_BASE_URL);
  assert.ok(base.protocol === 'http:' && base.hostname === '127.0.0.1' && !base.username && !base.password &&
    base.pathname === '/' && !base.search && !base.hash, 'isolated_record_backend_required');
  assert.ok(typeof env.UR_CLOUD_RECORD_POLICY_ID === 'string' && /^[A-Za-z0-9_.:-]{1,128}$/.test(env.UR_CLOUD_RECORD_POLICY_ID), 'explicit_record_policy_required');
  return {policy: env.UR_CLOUD_RECORD_POLICY_ID};
}

// contentMetadata 验证短期私有引用与实际摘要一致；不把URL本身持久化到报告。
function contentMetadata(event, recordingId, now) {
  assert.equal(event?.method, 'interaction.result', 'record_content_result_required');
  assert.equal(event.code, 200, 'record_content_success_required');
  const value = event.data;
  assert.ok(value?.command === 'recordingContent' && value.operation === 'interaction.control' &&
    value.recordingId === recordingId && value.state === 'ready', 'record_content_identity_required');
  assert.ok(/^[a-f0-9]{64}$/.test(value.sha256) && value.contentRef === `recording:${value.contentRef?.split(':')[1]}:${value.sha256}` &&
    /^[a-f0-9]{64}$/.test(value.contentRef.split(':')[1]), 'opaque_record_digest_required');
  assert.ok(Number.isSafeInteger(value.size) && value.size > 0 && value.size <= 1 << 30 &&
    Number.isSafeInteger(value.durationMs) && value.durationMs > 0 &&
    Number.isSafeInteger(value.expiresAt) && value.expiresAt > now && value.expiresAt <= now + 31000, 'finite_record_content_required');
  const url = new URL(value.signedURL);
  assert.ok(['http:', 'https:'].includes(url.protocol) && ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname) &&
    !url.username && !url.password && !url.hash && value.signedURL.length <= 8192, 'isolated_private_url_required');
  return value;
}

// decodedPCM 测量实际解码s16le，不用RTP数量、音频容器信息或模型状态冒充声音。
function decodedPCM(bytes) {
  assert.ok(Buffer.isBuffer(bytes) && bytes.length >= 3840 && bytes.length % 2 === 0, 'actual_decoded_pcm_required');
  let peak = 0, sum = 0;
  for (let index = 0; index < bytes.length; index += 2) {
    const value = bytes.readInt16LE(index) / 32768;
    peak = Math.max(peak, Math.abs(value)); sum += value * value;
  }
  const rms = Math.sqrt(sum / (bytes.length / 2));
  assert.ok(peak > 0.001 && rms > 0.001, 'recorded_pcm_must_be_audible');
  return {samples: bytes.length / 2, peak, rms};
}

// retiredPrivateURL 在签名仍有效的五秒内重读原地址，以实际字节摘要确认原产物已不可下载；网络未知不冒充已删除。
async function retiredPrivateURL(content, fetcher=fetch, budget=5000) {
  assert.ok(Number.isInteger(budget)&&budget>=100&&budget<=5000,'bounded_private_retirement_required');
  const deadline=Date.now()+budget,signal=AbortSignal.timeout(budget);
  assert.ok(content.expiresAt>deadline+1000,'signature_must_remain_valid_during_delete_probe');
  while(Date.now()<deadline){
    const response=await fetcher(content.signedURL,{redirect:'error',signal});
    assert.ok(response.body,'actual_private_response_required');
    const hash=createHash('sha256');let bytes=0;
    for await(const chunk of response.body){bytes+=chunk.length;assert.ok(bytes<=content.size+65536,'private_retirement_response_budget');hash.update(chunk);}
    const original=response.status===200&&bytes===content.size&&hash.digest('hex')===content.sha256;
    if(!original)return {status:response.status,bytes,originalArtifactUnavailable:true};
    await new Promise(resolve=>setTimeout(resolve,100));
  }
  throw Error('private_artifact_retirement_timeout');
}

// waitFor 固定预算不因轮询延长；所有错误均为自有固定分类，不输出接口自由正文。
async function waitFor(predicate, timeout) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw Error('cloud_record_timeout');
}

// probe 由IPC原run在实际双端Established和实际解码后调用；所有录制控制仍走正式Commands/Events。
async function probe(context, env, config) {
  const {viewers, metrics} = context;
  const viewer = viewers.find(value => value.listening);
  assert.ok(viewer?.sessionId && viewer.endpointId, 'real_audio_viewer_required');
  const recordingId = `cloud-${randomUUID()}`, cache = new Map();
  let current = null;
  async function api(route, body) {
    const response = await fetch(`${env.UR_IPC_BASE_URL}/api/v1/im/interaction/${route}`, {method: 'POST',
      headers: {'content-type': 'application/json', token: env.UR_IPC_APP_TOKEN,
        'tenant-code': env.UR_IPC_TENANT || 'platform', 'app-id': env.UR_IPC_APP_ID || '100'},
      body: JSON.stringify(body), signal: AbortSignal.timeout(15000)});
    const result = await response.json();
    const pending = route === 'commands' && response.status === 202 && result.code === 202 && result.data?.accepted;
    assert.ok(response.status === 200 && result.code === 200 || pending, 'formal_cloud_record_api_rejected');
    return result.data;
  }
  async function receive() {
    const response = await api('events', {endpointId: viewer.endpointId, limit: 32});
    assert.ok(Array.isArray(response.list) && response.list.length <= 32, 'bounded_record_events_required');
    for (const item of response.list) {
      const event = item.event;
      if (event.sessionId !== viewer.sessionId) continue;
      if (event.method === 'interaction.result') cache.set(event.msgToken, event);
      if (event.method === 'interaction.controlState' && event.data?.command === 'recordingState') {
        const fact = event.data.result;
        if (fact?.recordingId === recordingId && Number.isSafeInteger(fact.revision) && (!current || fact.revision > current.revision)) current = fact;
      }
    }
    // 私有内容保留一条未ACK事件用于撤权后重读；其他原事件立即确认，不能把读取旧签名当新的许可。
    const acknowledgements = response.list.filter(value => !(value.event?.method === 'interaction.result' && value.event.data?.command === 'recordingContent'));
    if (acknowledgements.length) await api('events/ack', {endpointId: viewer.endpointId, deliveryIds: acknowledgements.map(value => value.deliveryId)});
    assert.ok(cache.size <= 32, 'bounded_record_results_required');
  }
  async function command(commandName, args) {
    const token = randomUUID();
    const accepted = await api('commands', {endpointId: viewer.endpointId, command: {msgToken: token,
      method: 'interaction.control', sessionId: viewer.sessionId, params: {command: commandName, arguments: args}}});
    assert.ok(accepted.accepted === true && accepted.msgToken === token, 'original_record_acceptance_required');
    await waitFor(async () => { await receive(); return cache.has(token); }, 20000);
    const result = cache.get(token); cache.delete(token);
    assert.equal(result.code, 200, 'original_record_result_required');
    return result;
  }
  const consent = await command('recordingConsent', {operation: 'grant', recordingId,
    policyId: config.policy, expiresAt: Date.now() + 180000});
  assert.ok(/^[a-f0-9]{64}$/.test(consent.data?.consentId) && Array.isArray(consent.data.sourceTrackIds) &&
    consent.data.sourceTrackIds.length === 2 && consent.data.revision === 1, 'persistent_own_consent_required');
  const proof = consent.data.consentId;
  const started = await command('recordingControl', {operation: 'start', expectedRevision: 0, recordingId,
    sourceTrackIds: consent.data.sourceTrackIds, consentIds: [proof], policyId: config.policy});
  assert.equal(started.data.state, 'active');
  const before = await metrics(viewer);
  await new Promise(resolve => setTimeout(resolve, 1500));
  const paused = await command('recordingControl', {operation: 'pause', expectedRevision: started.data.revision, recordingId});
  assert.equal(paused.data.state, 'paused');
  await new Promise(resolve => setTimeout(resolve, 500));
  const resumed = await command('recordingControl', {operation: 'resume', expectedRevision: paused.data.revision, recordingId, consentIds: [proof]});
  assert.equal(resumed.data.state, 'active');
  await new Promise(resolve => setTimeout(resolve, 1500));
  const stopped = await command('recordingControl', {operation: 'stop', expectedRevision: resumed.data.revision, recordingId});
  assert.equal(stopped.data.state, 'finalizing');
  await waitFor(async () => { await receive(); return current?.state === 'ready' || current?.state === 'failed'; }, 65000);
  assert.equal(current.state, 'ready', 'actual_record_ready_required');
  const contentEvent = await command('recordingContent', {recordingId});
  const content = contentMetadata(contentEvent, recordingId, Date.now());
  const response = await fetch(content.signedURL, {signal: AbortSignal.timeout(15000), redirect: 'error'});
  assert.equal(response.status, 200, 'actual_private_artifact_required');
  const chunks = []; let size = 0;
  for await (const chunk of response.body) { size += chunk.length; assert.ok(size <= content.size, 'artifact_size_budget'); chunks.push(chunk); }
  const bytes = Buffer.concat(chunks);
  assert.equal(bytes.length, content.size); assert.equal(createHash('sha256').update(bytes).digest('hex'), content.sha256);
  const root = path.resolve('.temp/task143-remediation-20261008'); fs.mkdirSync(root, {recursive: true});
  assert.ok(fs.realpathSync(root).startsWith(`${fs.realpathSync('.temp')}${path.sep}`), 'record_evidence_must_stay_in_task_temp');
  const file = path.join(root, `cloud-record-${randomUUID()}.mkv`); fs.writeFileSync(file, bytes, {mode: 0o600, flag: 'wx'});
  const exec = promisify(execFile);
  const picture = await exec('ffmpeg', ['-v', 'error', '-nostdin', '-i', file, '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'], {encoding: 'buffer', timeout: 15000, maxBuffer: 16 << 20});
  assert.ok(picture.stdout.length >= 16 * 16 * 3 && picture.stdout.some(value => value !== 0), 'actual_recorded_video_decode_required');
  const audio = await exec('ffmpeg', ['-v', 'error', '-nostdin', '-i', file, '-map', '0:a:0', '-t', '10', '-f', 's16le', '-ac', '1', '-ar', '48000', 'pipe:1'], {encoding: 'buffer', timeout: 15000, maxBuffer: 2 << 20});
  const pcm = decodedPCM(audio.stdout);
  const wav=file.replace(/\.mkv$/,'.wav');fs.closeSync(fs.openSync(wav,'wx',0o600));
  await exec('ffmpeg',['-v','error','-nostdin','-y','-i',file,'-map','0:a:0','-ac','1','-ar','48000','-c:a','pcm_s16le',wav],{encoding:'buffer',timeout:15000,maxBuffer:1<<20});
  await command('recordingConsent', {operation: 'revoke', recordingId, consentId: proof, expectedRevision: consent.data.revision});
  const reread = await api('events', {endpointId: viewer.endpointId, limit: 32});
  const denied = reread.list.find(value => value.event?.msgToken === contentEvent.msgToken)?.event;
  assert.ok(denied?.code === 403 && denied.data?.state === 'failed' && denied.data.reason === 'permissionDenied' &&
    !Object.hasOwn(denied.data, 'signedURL') && !Object.hasOwn(denied.data, 'contentRef'), 'withdrawn_record_content_must_fail');
  if (reread.list.length) await api('events/ack', {endpointId: viewer.endpointId, deliveryIds: reread.list.map(value => value.deliveryId)});
  const retired=await retiredPrivateURL(content);
  const after = await metrics(viewer);
  assert.ok(after.connected && after.frames > before.frames && after.audioSamples > before.audioSamples, 'record_stop_must_preserve_main_media');
  return {ready: true, pauseResume: true, privateArtifactBytes: size, sha256: content.sha256,
    durationMs: content.durationMs, videoBytes: picture.stdout.length, decodedPCM: pcm, revokedReadDenied: true, retiredPrivateURL:retired, representativeWAV:true, mainMediaContinues: true};
}

// run 复用原IPC的全部精确清理；录制失败会使外层验收失败，不吞掉为可选附加项。
async function run() {
  const env = process.env, config = configuration(env);
  let recording;
  const outcome = await ipc.run({onEstablished: async context => { recording = await probe(context, env, config); }});
  if (outcome.result === 'pass') console.log(JSON.stringify({result: 'pass', scenario: 'S15-cloud-recording', recording}));
  return {...outcome, recording};
}

module.exports = {configuration, contentMetadata, decodedPCM, retiredPrivateURL, probe, run};
if (require.main === module) run().catch(() => { console.log(JSON.stringify({result: 'fail', scenario: 'S15-cloud-recording', reason: 'cloud_record_setup_failed'})); process.exitCode = 1; });
