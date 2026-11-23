// 文件说明：显式Linux实时联调节目源；连续lavfi编码而非文件循环，不承载设备身份或业务授权。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const {spawn} = require('node:child_process');

// OggOpus 提取固定20ms Opus包；页和包均有界，错误不降级为空音频成功。
class OggOpus {
  constructor(emit) { this.emit = emit; this.buffer = Buffer.alloc(0); this.parts = []; this.size = 0; this.header = 0; this.serial = null; this.page = null; }
  // feed 消费任意分片，保留跨页原包；原编码顺序缺页时终止。
  feed(chunk) {
    assert.ok(this.buffer.length + chunk.length <= 131072, 'ogg_buffer_limit');
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 27) {
      const input = this.buffer;
      assert.equal(input.subarray(0, 4).toString(), 'OggS', 'ogg_header');
      assert.equal(input[4], 0);
      const count = input[26];
      if (input.length < 27 + count) return;
      const sizes = input.subarray(27, 27 + count), bytes = [...sizes].reduce((sum, value) => sum + value, 0);
      if (input.length < 27 + count + bytes) return;
      const serial = input.readUInt32LE(14), page = input.readUInt32LE(18);
      assert.ok(this.serial === null || this.serial === serial, 'ogg_source_changed');
      assert.ok(this.page === null || page === this.page + 1, 'ogg_page_gap');
      assert.equal(!!(input[5] & 1), this.parts.length > 0, 'ogg_continuation');
      this.serial = serial; this.page = page;
      let offset = 27 + count;
      for (const size of sizes) {
        this.parts.push(Buffer.from(input.subarray(offset, offset + size))); this.size += size; offset += size;
        assert.ok(this.size <= 65536, 'ogg_packet_limit');
        if (size === 255) continue;
        const packet = Buffer.concat(this.parts, this.size); this.parts = []; this.size = 0;
        if (this.header === 0) {
          assert.ok(packet.length >= 19 && packet.subarray(0, 8).toString() === 'OpusHead' && packet[9] === 1, 'mono_opus_required');
        } else if (this.header === 1) assert.equal(packet.subarray(0, 8).toString(), 'OpusTags');
        else {
          assert.ok(packet.length > 0 && packet.length <= 1275, 'opus_packet_limit');
          const config = packet[0] >> 3, code = packet[0] & 3;
          const duration = config >= 16 ? [2.5, 5, 10, 20][config & 3] : config >= 12 ? [10, 20][config & 1] : [10, 20, 40, 60][config & 3];
          const frames = code === 0 ? 1 : code === 3 ? (packet[1] & 63) : 2;
          assert.equal(duration * frames, 20, 'opus_20ms_required');
          this.emit(packet);
        }
        this.header++;
      }
      this.buffer = input.subarray(offset);
    }
  }
}

// AnnexFrames 以真实AUD分割访问单元；保留原SPS/PPS与SEI，不把编码回调当显示帧。
class AnnexFrames {
  constructor(emit) { this.emit = emit; this.buffer = Buffer.alloc(0); this.nals = []; this.size = 0; this.vcl = false; }
  // nal 消费一份去起始码NAL，只有实际VCL才形成完整访问单元。
  nal(value) {
    assert.ok(value.length >= 2 && !(value[0] & 128), 'annex_nal');
    const type = value[0] & 31;
    assert.ok(type >= 1 && type <= 23, 'annex_type');
    if (type === 9 && this.vcl) { this.emit(this.nals); this.nals = []; this.size = 0; this.vcl = false; }
    this.nals.push(Buffer.from(value)); this.size += value.length;
    assert.ok(this.size <= 262144 && this.nals.length <= 64, 'annex_frame_limit');
    this.vcl ||= type >= 1 && type <= 5;
  }
  // feed 起始码可跨任意stdout分片；保留最后原NAL直至下一边界。
  feed(chunk) {
    assert.ok(this.buffer.length + chunk.length <= 393216, 'annex_buffer_limit');
    this.buffer = Buffer.concat([this.buffer, chunk]);
    const starts = [];
    for (let i = 0; i + 2 < this.buffer.length; i++) {
      if (this.buffer[i] || this.buffer[i + 1]) continue;
      const width = this.buffer[i + 2] === 1 ? 3 : this.buffer[i + 2] === 0 && this.buffer[i + 3] === 1 ? 4 : 0;
      if (width) { starts.push({offset: i, width}); i += width - 1; }
    }
    if (starts.length < 2) return;
    assert.equal(starts[0].offset, 0, 'annex_prefix');
    for (let i = 0; i + 1 < starts.length; i++) this.nal(this.buffer.subarray(starts[i].offset + starts[i].width, starts[i + 1].offset));
    this.buffer = this.buffer.subarray(starts.at(-1).offset);
  }
}

// packetize 将原完整NAL组拆为有限单NAL/FU-A RTP；实际NAL5决定keyFrame，PLI不假造IDR。
function packetize(nals, timestamp, sequence) {
  assert.ok(Number.isInteger(timestamp) && timestamp >= 0 && timestamp <= 0xffffffff);
  const payloads = [];
  for (const nal of nals) {
    assert.ok(Buffer.isBuffer(nal) && nal.length >= 2 && (nal[0] & 31) >= 1 && (nal[0] & 31) <= 23 && !(nal[0] & 128));
    if (nal.length <= 1200) payloads.push(nal);
    else for (let offset = 1; offset < nal.length; offset += 1198) {
      const end = Math.min(offset + 1198, nal.length);
      payloads.push(Buffer.concat([Buffer.from([(nal[0] & 0xe0) | 28, (nal[0] & 31) | (offset === 1 ? 128 : 0) | (end === nal.length ? 64 : 0)]), nal.subarray(offset, end)]));
    }
  }
  assert.ok(payloads.length >= 1 && payloads.length <= 256, 'rtp_frame_limit');
  return payloads.map((payload, index) => {
    const header = Buffer.alloc(12); header[0] = 128; header[1] = 102 | (index === payloads.length - 1 ? 128 : 0);
    header.writeUInt16BE((sequence + index) & 65535, 2); header.writeUInt32BE(timestamp, 4); header.writeUInt32BE(143, 8);
    return Buffer.concat([header, payload]);
  });
}

// encoderPlan 固定实时合成时钟、20ms单声道Opus及15fps baseline运动画面；无输入URL/文件。
function encoderPlan(kind, seconds) {
  assert.ok(['audio', 'video'].includes(kind) && Number.isInteger(seconds) && seconds >= 5 && seconds <= 600);
  const args = ['-hide_banner', '-loglevel', 'error', '-nostdin', '-re', '-f', 'lavfi', '-i'];
  // 测试音调随真实源时钟逐秒换频；与视频五位秒标对应，恢复后可用实际PCM区别旧位置，而非逻辑代次。
  if (kind === 'audio') return [...args, "aevalsrc='0.125*sin(2*PI*(440+40*mod(floor(t),32))*t)':s=48000", '-t', String(seconds), '-c:a', 'libopus', '-ac', '1', '-ar', '48000', '-frame_duration', '20', '-b:a', '64k', '-page_duration', '20000', '-flush_packets', '1', '-f', 'ogg', 'pipe:1'];
  // 五位画面秒标记按32秒回绕；后续暂停/恢复必须从实际像素读取，而非从目标时间推断。
  const markers=['drawbox=x=0:y=0:w=320:h=64:color=black:t=fill'];
  for(let bit=0;bit<5;bit++) markers.push(`drawbox=x=${16+bit*40}:y=16:w=24:h=24:color=white:t=fill:enable='gte(mod(floor(t)/${2**bit},2),1)'`);
  return [...args, 'testsrc2=size=320x240:rate=15', '-t', String(seconds), '-an', '-vf', markers.join(','), '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast', '-tune', 'zerolatency', '-profile:v', 'baseline', '-pix_fmt', 'yuv420p', '-g', '15', '-keyint_min', '15', '-sc_threshold', '0', '-x264-params', 'aud=1:repeat-headers=1', '-flush_packets', '1', '-f', 'h264', 'pipe:1'];
}

// startSource 只供显式本机验收：单一固定消费者，断开不重建授权；队列满直接失败，不缓冲重播。
async function startSource({socketPath, sourceId, epoch, seconds = 60}) {
  const temp = fs.realpathSync(path.resolve('.temp')), file = path.resolve(socketPath || '');
  assert.ok(file.startsWith(temp + path.sep) && file.length <= 100 && !fs.existsSync(file), 'new_task_socket_required');
  assert.equal(fs.realpathSync(path.dirname(file)), path.dirname(file), 'socket_symlink_forbidden');
  assert.ok(typeof sourceId === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(sourceId) && Number.isSafeInteger(epoch) && epoch > 0);
  const tracks = [{trackId:'broadcast-out',kind:'audio',sourceId:'broadcast-audio',direction:'recvonly',codec:'opus',state:'requested'},
    {trackId:'broadcast-video-out',kind:'video',sourceId:'broadcast-video',direction:'recvonly',codec:'H264',state:'requested'}];
  const metrics = {audio:0,video:0,keyFrames:0,connections:0,failed:false};
  let client = null, ending = false, rtpSequence = 0, closePromise = null; const children = [], exits = [];
  // write 只把当前编码样本交给唯一原连接，无接收者仍推进原时钟，绝不保存历史样本重放。
  const write = sample => { if (client && !client.destroyed) { assert.ok(client.writableLength <= 1048576, 'source_backpressure'); client.write(JSON.stringify(sample) + '\n'); } };
  const server = net.createServer(socket => {
    if (ending || metrics.connections > 0) { socket.destroy(); return; }
    metrics.connections++; client = socket;
    socket.write(JSON.stringify({sourceId, sourceEpoch:epoch, tracks}) + '\n');
    let commands = '';
    socket.on('data', bytes => {
      try {
        commands += bytes.toString(); assert.ok(commands.length <= 1024);
        let at;
        while ((at = commands.indexOf('\n')) !== -1) {
          const command = JSON.parse(commands.slice(0, at)); commands = commands.slice(at + 1);
          assert.deepEqual(command, {method:'requestKeyFrame'}); // 固定1秒自然IDR；不伪造额外编码事实。
        }
      } catch { metrics.failed = true; void close(); }
    });
    socket.on('error', () => { if (!ending) { metrics.failed = true; void close(); } });
    socket.on('close', () => { client = null; });
  });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(file, resolve); });
  fs.chmodSync(file, 0o600); const owned = fs.statSync(file);
  // close 等待原编码进程与监听器实际退出；只删除同inode的本任务socket，不清理目录/数据。
  function close() {
    if (closePromise) return closePromise;
    ending = true;
    closePromise = (async () => {
      client?.destroy();
      for (const child of children) if (child.exitCode === null) child.kill('SIGTERM');
      const timer = setTimeout(() => { for (const child of children) if (child.exitCode === null) child.kill('SIGKILL'); }, 2000);
      await Promise.all(exits); clearTimeout(timer);
      await new Promise(resolve => server.close(resolve));
      if (fs.existsSync(file) && fs.statSync(file).ino === owned.ino) fs.unlinkSync(file);
    })();
    return closePromise;
  }
  try {
    for (const kind of ['audio', 'video']) {
      const parser = kind === 'audio' ? new OggOpus(packet => { metrics.audio++;
        write({epoch, sequence:metrics.audio, kind, timestamp:((metrics.audio - 1) * 960) >>> 0, audio:packet.toString('base64'),video:null,keyFrame:false});
      }) : new AnnexFrames(nals => { metrics.video++;
        const timestamp = ((metrics.video - 1) * 6000) >>> 0, packets = packetize(nals, timestamp, rtpSequence);
        rtpSequence = (rtpSequence + packets.length) & 65535;
        const keyFrame = nals.some(nal => (nal[0] & 31) === 5); if (keyFrame) metrics.keyFrames++;
        write({epoch, sequence:metrics.video,kind,timestamp,audio:null,video:packets.map(packet => packet.toString('base64')),keyFrame});
      });
      const child = spawn('ffmpeg', encoderPlan(kind, seconds), {stdio:['ignore','pipe','pipe']}); children.push(child);
      exits.push(new Promise(resolve => { child.once('error', () => { metrics.failed = true; resolve(); void close(); }); child.once('close', code => { if (!ending && code !== 0) metrics.failed = true; resolve(); if (!ending) void close(); }); }));
      child.stderr.on('data', () => {}); // 编码器自由正文不进入报告。
      child.stdout.on('data', bytes => { try { if (!ending) parser.feed(bytes); } catch { metrics.failed = true; void close(); } });
    }
    return {metrics, tracks, close};
  } catch { await close(); throw Error('live_encoder_start_failed'); }
}

module.exports = {OggOpus, AnnexFrames, packetize, encoderPlan, startSource};
