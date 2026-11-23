// 文件说明：builtin实际codec格式自检合同测试；mock decoder不替代正式Chrome实际decode验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');

// create 模拟明确decoder输出/关闭用于失败合同；实际素材摘要由真实生成输入固定。
function create(options = {}) {
  const closes = {audio: 0, video: 0, frames: 0};
  const sandbox = {Uint8Array, Float32Array, Number, Promise, setTimeout, clearTimeout,
    atob: value => Buffer.from(value, 'base64').toString('binary'),
    RTCRtpScriptTransform: class {}, Worker: class {},
    EncodedAudioChunk: class { constructor(value) { this.value = value; } },
    EncodedVideoChunk: class { constructor(value) { this.value = value; } }};
  sandbox.AudioDecoder = class {
    constructor(callback) { this.callback = callback; this.state = 'configured'; }
    configure(value) { assert.equal(value.codec, 'opus'); if (options.audioError) throw Error('fixed'); }
    decode() { if (options.audioNoOutput) return; this.callback.output({sampleRate: options.wrongRate ? 16000 : 48000,
      numberOfChannels: 1, numberOfFrames: 960, copyTo: target => target.fill(0), close: () => { closes.frames += 1; }}); }
    async flush() {}
    close() { this.state = 'closed'; closes.audio += 1; }
  };
  sandbox.VideoDecoder = class {
    constructor(callback) { this.callback = callback; this.state = 'configured'; }
    configure(value) { assert.equal(value.codec, 'avc1.42c00a'); if (options.videoError) throw Error('fixed'); }
    decode() { if (options.videoNoOutput) return; this.callback.output({displayWidth: options.wrongSize ? 64 : 32,
      displayHeight: 32, colorSpace: {matrix:options.matrix===undefined?'bt709':options.matrix}, copyTo: async target => { for (let index = 0; index < target.length; index += 4) {
        const color=['bt470bg','smpte170m'].includes(options.matrix)?[30,126,221]:[21,123,221];
        target[index] = options.wrongColor ? 255 : color[0]; target[index + 1] = color[1]; target[index + 2] = color[2]; target[index + 3] = options.wrongAlpha?0:255;
      } }, close: () => { closes.frames += 1; }}); }
    async flush() {}
    close() { this.state = 'closed'; closes.video += 1; }
  };
  if (options.noTransform) delete sandbox.RTCRtpScriptTransform;
  if (options.noDecoders) { delete sandbox.AudioDecoder; delete sandbox.VideoDecoder; }
  const context = vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/live-probe.js'), 'utf8'), context);
  return {context, closes};
}
test('fixed actual IDR bytes match centrally generated source and cannot become programme source', () => {
  const {context} = create();
  const bytes = Buffer.from(vm.runInContext('UR_LIVE_PROBE_H264', context), 'base64');
  assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), '5d1eb2e05252a396ed7050390f180d571740c43ac2018528152f8de236aba4a1');
});
test('actual matching decoder frames required and all decoders/frames close', async () => {
  const {context, closes} = create(); const result = await context.probeLiveReceive();
  assert.equal(result.audio, true); assert.equal(result.video, true);
  assert.deepEqual(closes, {audio: 1, video: 1, frames: 2});
});
test('interface alone or empty output cannot advertise actual format', async () => {
  const fixture = create({audioNoOutput: true, videoNoOutput: true});
  const result = await fixture.context.probeLiveReceive(); assert.equal(result.audio, false); assert.equal(result.video, false);
  assert.deepEqual(fixture.closes, {audio: 1, video: 1, frames: 0});
});
test('wrong actual rate size or pixels fail only corresponding media capability', async () => {
  for (const options of [{wrongRate: true}, {wrongSize: true}, {wrongColor: true}]) {
    const fixture = create(options); const result = await fixture.context.probeLiveReceive();
    assert.equal(result.audio, !options.wrongRate); assert.equal(result.video, !options.wrongSize && !options.wrongColor);
    assert.equal(fixture.closes.frames, 2);
  }
});
test('unsupported codecs or missing transform never fall back to AAC VP8 or capture', async () => {
  const fixture = create({audioError: true, videoError: true}); const result = await fixture.context.probeLiveReceive();
  assert.equal(result.audio, false); assert.equal(result.video, false);
  const next = await create({noTransform: true}).context.probeLiveReceive(); assert.equal(next.audio, true); assert.equal(next.video, false);
  const none = await create({noDecoders: true}).context.probeLiveReceive(); assert.equal(none.audio, false); assert.equal(none.video, false);
});
test('固定IDR按实际矩阵复核两种转换，未知矩阵及透明像素不能成功',async()=>{
  for(const matrix of ['bt709','bt470bg','smpte170m'])assert.equal((await create({matrix}).context.probeLiveReceive()).video,true);
  for(const options of [{matrix:null},{matrix:'bt2020-ncl'},{wrongAlpha:true}]){
    const fixture=create(options),result=await fixture.context.probeLiveReceive();assert.equal(result.audio,true);assert.equal(result.video,false);assert.deepEqual(fixture.closes,{audio:1,video:1,frames:2});
  }
});
