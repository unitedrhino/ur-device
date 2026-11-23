// 文件说明：实时联调源解析/编码计划离线回归；固定包夹具不是真实解码或端云播放。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {OggOpus, AnnexFrames, packetize, encoderPlan, startSource} = require('./live_encoded_source.cjs');

// page 构造受控Ogg容器；codec内容另由真实编码验收验证，不能用本夹具声称音频有效。
function page(number, packet, serial = 143) {
  const laces = []; for (let size = packet.length; size >= 255; size -= 255) laces.push(255);
  laces.push(packet.length % 255);
  const head = Buffer.alloc(27); head.write('OggS'); head.writeUInt32LE(serial,14); head.writeUInt32LE(number,18); head[26] = laces.length;
  return Buffer.concat([head, Buffer.from(laces), packet]);
}
// headers 固定mono头和标签，仅包边界测试使用。
function headers() { const head = Buffer.alloc(19); head.write('OpusHead'); head[8]=1; head[9]=1; return Buffer.concat([page(0,head), page(1,Buffer.from('OpusTags'))]); }
test('Ogg任意单字节分片仍恰输出一份原20ms包', () => {
  const output=[], parser=new OggOpus(value=>output.push(value));
  for(const byte of Buffer.concat([headers(),page(2,Buffer.from([0xf8,1,2]))])) parser.feed(Buffer.from([byte]));
  assert.deepEqual(output,[Buffer.from([0xf8,1,2])]);
});
test('Ogg超长包跨laces保持原字节而非分段新样本', () => {
  const output=[], parser=new OggOpus(value=>output.push(value)), original=Buffer.alloc(300,23); original[0]=0xf8;
  parser.feed(headers()); parser.feed(page(2,original)); assert.deepEqual(output,[original]);
});
test('Ogg缺页、换源、错误时长和容量拒绝而非静音成功', () => {
  for(const input of [page(3,Buffer.from([0xf8,1])), page(2,Buffer.from([0xf8,1]),144), page(2,Buffer.from([0xf0,1]))]) {
    const parser=new OggOpus(()=>assert.fail('unexpected packet')); parser.feed(headers()); assert.throws(()=>parser.feed(input));
  }
  assert.throws(()=>new OggOpus(()=>{}).feed(Buffer.alloc(131073)));
});
test('Annex真实AUD分组包含原SPS/PPS和实际NAL5', () => {
  const output=[], parser=new AnnexFrames(value=>output.push(value)), prefix=Buffer.from([0,0,0,1]);
  const nals=[[9,0xf0],[0x67,1],[0x68,1],[0x65,1,2],[9,0xf0],[0x41,3],[9,0xf0],[0x41,4]];
  for(const byte of Buffer.concat(nals.map(value=>Buffer.concat([prefix,Buffer.from(value)])))) parser.feed(Buffer.from([byte]));
  assert.equal(output.length,2); assert.deepEqual(output[0].map(nal=>nal[0]&31),[9,7,8,5]);
  assert.deepEqual(output[1].map(nal=>nal[0]&31),[9,1]);
});
test('H264 FU-A原顺序、同时间戳、末marker与16位回绕保持完整', () => {
  const original=Buffer.alloc(3000,3); original[0]=0x65;
  const packets=packetize([original],90000,65535);
  assert.equal(packets.length,3); assert.deepEqual(packets.map(packet=>packet.readUInt16BE(2)),[65535,0,1]);
  assert.ok(packets.every(packet=>packet.readUInt32BE(4)===90000 && packet.length<=1212));
  assert.equal(packets[0][13],0x85); assert.equal(packets[2][13],0x45);
  assert.deepEqual(Buffer.concat([Buffer.from([0x65]),...packets.map(packet=>packet.subarray(14))]),original);
  assert.deepEqual(packets.map(packet=>!!(packet[1]&128)),[false,false,true]);
});
test('NAL禁止非法类型、空访问单元和无界包数量', () => {
  for(const nals of [[],[Buffer.from([0x80,1])],[Buffer.from([24,1])],[Buffer.alloc(400000,3)]]) assert.throws(()=>packetize(nals,0,0));
  assert.throws(()=>new AnnexFrames(()=>{}).feed(Buffer.alloc(393217)));
});
test('编码计划只使用连续实时lavfi，无文件循环、URL和输入采集', () => {
  for(const kind of ['audio','video']) {
    const args=encoderPlan(kind,60); assert.ok(args.includes('-re')&&args.includes('lavfi')&&args.at(-1)==='pipe:1');
    assert.equal(args.some(value=>/https?:|rtsp:|loop|\.mp4/.test(value)),false);
  }
  assert.throws(()=>encoderPlan('file',60)); assert.throws(()=>encoderPlan('audio',601));
});
test('实际源时钟分别进入音调与五位像素，不使用固定纯音冒称恢复到了当前位置',()=>{
  assert.ok(encoderPlan('audio',60).some(value=>value.includes('440+40*mod(floor(t),32)')));
  const video=encoderPlan('video',60);assert.ok(video[video.indexOf('-vf')+1].includes('floor(t)/16'));
});
test('非本任务socket在任何网络或编码副作用前拒绝', async () => {
  await assert.rejects(startSource({socketPath:'/tmp/not-task143.sock',sourceId:'test',epoch:1,seconds:5}));
});
