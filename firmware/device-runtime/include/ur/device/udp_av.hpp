// 文件说明：人际音视频 UDP
// 媒体帧；复用16字节头与AES-CTR，重组有界且不阻塞音频。
#pragma once

#include "ur/device/udp_channel.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ur::device {
// UdpAV 使用独立的上下行nonce及同一短期密钥；调用者必须先经可信信令安装参数。
// 调用方须串行化全部方法；分片批次预留连续序号，音频可在分片之间发送。
class UdpAV {
public:
  static constexpr std::size_t kFragment = 1200;     // 单数据报不超过1224字节。
  static constexpr std::size_t kMaxJPEG = 48 * 1024; // MCU固定单帧预算。
  static constexpr std::uint32_t kStride =
      128; // 每包占用最多80个AES块，避免相邻CTR区间重叠。
  static constexpr std::uint64_t kAssemblyMs =
      200; // 缺片整帧放弃，不重传旧画面。
  // UdpAV 借用密码端口；tx/rx必须属于本次会话且不同，关闭时由宿主擦除密钥。
  UdpAV(AesCtrPort &cipher, std::array<std::uint8_t, 8> tx,
        std::array<std::uint8_t, 8> rx)
      : cipher_(cipher), tx_(tx), rx_(rx) {}

  // audio 编码单Opus帧；失败时packet为空，序号耗尽须新建会话。
  bool audio(const std::vector<std::uint8_t> &opus,
             std::vector<std::uint8_t> &packet) {
    packet.clear();
    if (opus.empty() || opus.size() > 1275 || !reserve(1))
      return false;
    return seal(0x0100, sequence_, opus.data(), opus.size(), packet);
  }

  // jpeg 一次生成完整分片批次；失败不交付残批次，原JPEG只读且不持有。
  bool jpeg(const std::uint8_t *data, std::size_t size,
            std::vector<std::vector<std::uint8_t>> &packets) {
    packets.clear();
    if (!data || !size || size > kMaxJPEG)
      return false;
    const auto count =
        static_cast<std::uint16_t>((size + kFragment - 1) / kFragment);
    if (!reserve(count))
      return false;
    const auto first = sequence_ - (count - 1) * kStride;
    for (std::uint16_t index = 0; index < count; ++index) {
      const auto offset = index * kFragment;
      const auto length = std::min(kFragment, size - offset);
      std::vector<std::uint8_t> plain(8 + length); // 分片头随图像一起加密。
      put16(plain.data(), index);
      put16(plain.data() + 2, count);
      put16(plain.data() + 4, static_cast<std::uint16_t>(size));
      std::copy_n(data + offset, length, plain.data() + 8);
      std::vector<std::uint8_t> packet; // 成功后移交本批次。
      if (!seal(0x0200, first + index * kStride, plain.data(), plain.size(),
                packet)) {
        packets.clear();
        return false;
      }
      packets.push_back(std::move(packet));
    }
    return true;
  }

  // receive 严格核对会话、长度和128步长；audio/JPEG分别交付，缺片时JPEG为空。
  // now使用单调毫秒；视频允许同帧乱序，音频丢弃重复/迟到包，不建立无界抖动队列。
  bool receive(const std::vector<std::uint8_t> &packet, std::uint64_t now,
               std::vector<std::uint8_t> &opus,
               std::vector<std::uint8_t> &image) {
    opus.clear();
    image.clear();
    if (packet.size() <= 16 || packet.size() > 1291 || tx_ == rx_)
      return false;
    const auto type = get16(packet.data());
    const auto length = get16(packet.data() + 2);
    const auto seq = get32(packet.data() + 12);
    if (length != packet.size() - 16 || !seq || seq % kStride ||
        !std::equal(rx_.begin(), rx_.end(), packet.begin() + 4) ||
        (type != 0x0100 && type != 0x0200))
      return false;
    std::array<std::uint8_t, 16>
        iv{}; // 原包头即CTR初始计数块，不跨数据报复用状态。
    std::copy_n(packet.begin(), 16, iv.begin());
    plain_.resize(length); // 复用解密暂存，解密失败不会发布结果。
    auto &plain = plain_;
    if (!cipher_.transform(iv, packet.data() + 16, plain.data(), length))
      return false;
    if (type == 0x0100) {
      if (length > 1275 || seq <= audio_seen_)
        return false;
      audio_seen_ = seq;
      opus.assign(plain.begin(), plain.end());
      return true;
    }
    if (length <= 8 || length > 8 + kFragment)
      return false;
    const auto index = get16(plain.data());
    const auto count = get16(plain.data() + 2);
    const auto size = get16(plain.data() + 4);
    if (!size || size > kMaxJPEG ||
        count != (size + kFragment - 1) / kFragment || index >= count ||
        plain[6] || plain[7] || seq <= index * kStride ||
        static_cast<std::size_t>(length - 8) != std::min(kFragment, size - index * kFragment))
      return false;
    const auto first = seq - index * kStride;
    if (first <= video_done_ || (video_first_ && first < video_first_)) {
      return false;
    }
    if (first != video_first_ || now < video_started_ ||
        now - video_started_ >= kAssemblyMs) {
      if (first == video_first_) {
        video_done_ = first;
        clear_frame();
        return false;
      }
      video_first_ = first;
      video_started_ = now;
      video_size_ = size;
      frame_.resize(size);
      fragment_seen_.fill(false);
      fragment_count_ = count;
      received_ = 0;
    }
    if (size != video_size_ || count != fragment_count_ ||
        fragment_seen_[index]) {
      return false;
    }
    std::copy(plain.begin() + 8, plain.end(), frame_.begin() + index * kFragment);
    fragment_seen_[index] = true;
    if (++received_ == count) {
      image = std::move(frame_);
      video_done_ = first;
      clear_frame();
    }
    return true;
  }

private:
  // reserve 按最大CTR块数分隔IV，并在生成批次前一次检查溢出。
  bool reserve(std::uint32_t count) {
    if (tx_ == rx_ ||
        count >
            (std::numeric_limits<std::uint32_t>::max() - sequence_) / kStride)
      return false;
    sequence_ += count * kStride;
    return true;
  }
  // seal 使用本会话上行nonce生成16字节头，失败清空输出。
  bool seal(std::uint16_t type, std::uint32_t seq, const std::uint8_t *data,
            std::size_t size, std::vector<std::uint8_t> &packet) {
    std::array<std::uint8_t, 16> iv{}; // 所有头字段都参与CTR初始值。
    put16(iv.data(), type);
    put16(iv.data() + 2, static_cast<std::uint16_t>(size));
    std::copy(tx_.begin(), tx_.end(), iv.begin() + 4);
    put32(iv.data() + 12, seq);
    packet.resize(16 + size);
    std::copy(iv.begin(), iv.end(), packet.begin());
    if (!cipher_.transform(iv, data, packet.data() + 16, size)) {
      packet.clear();
      return false;
    }
    return true;
  }
  // clear_frame 只释放未完成帧，保持已完成水位，拒绝旧图像重放。
  void clear_frame() {
    std::vector<std::uint8_t>().swap(frame_);
    fragment_count_ = 0;
    video_first_ = 0;
    received_ = 0;
  }
  static std::uint16_t get16(const std::uint8_t *p) {
    return (std::uint16_t(p[0]) << 8) | p[1];
  }
  static std::uint32_t get32(const std::uint8_t *p) {
    return (std::uint32_t(get16(p)) << 16) | get16(p + 2);
  }
  static void put16(std::uint8_t *p, std::uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
  }
  static void put32(std::uint8_t *p, std::uint32_t n) {
    put16(p, n >> 16);
    put16(p + 2, n);
  }
  AesCtrPort &cipher_;                  // 借用宿主硬件密码端口。
  std::array<std::uint8_t, 8> tx_, rx_; // 不同方向独立nonce，禁止反射本端发包。
  std::uint32_t sequence_ = 0, audio_seen_ = 0; // 发送全局水位、音频接收水位。
  std::uint32_t video_first_ = 0, video_done_ = 0; // 当前批次、最近已消费批次。
  std::uint64_t video_started_ = 0; // 当前帧首个到达分片的单调时间。
  std::uint16_t video_size_ = 0, received_ = 0; // 已验证的总字节与收到片数。
  std::vector<std::uint8_t> plain_, frame_; // 有界解密暂存和一帧连续数据。
  std::array<bool, (kMaxJPEG + kFragment - 1) / kFragment> fragment_seen_{};
  std::uint16_t fragment_count_ = 0;
};

// RemoteVideoGate
// 连续慢帧后本次通话停显，避免反复试显抢占音频；新会话重新建门。
class RemoteVideoGate {
public:
  // completed
  // 在解码/渲染完成时报告真实耗时；连续三帧超过预算就停显至本会话结束。
  // JPEG路径包含一次跨任务的RGB565交付，调用方可使用更宽预算；默认仍保持H264旧门限。
  bool completed(std::uint64_t now, std::uint64_t elapsed_ms,
                 std::uint64_t budget_ms = 100) {
    if (disabled_)
      return false;
    if (elapsed_ms > budget_ms)
      ++slow_;
    else
      slow_ = 0;
    if (slow_ >= 3) {
      disabled_ = true;
      slow_ = 0;
      visible_ = false;
      return false;
    }
    visible_ = true;
    last_ = now;
    return true;
  }
  // accept 停显后拒绝新解码工作；音频不受此门控制。
  bool accept(std::uint64_t) const { return !disabled_; }
  // visible 超过500ms没有完成新帧就停显，避免冻结的画面被误认为实时视频。
  bool visible(std::uint64_t now) const {
    return visible_ && now >= last_ && now - last_ < 500;
  }

private:
  std::uint64_t last_ = 0; // 最后成功显示时间。
  bool disabled_ = false;  // 本次会话连续慢帧后的停显事实。
  unsigned slow_ = 0;      // 连续慢帧数量。
  bool visible_ = false;   // 当前是否有新鲜画面。
};
// AudioCaptureGate 按一秒窗口检测持续采集吞吐不足；静音/新会话有三秒预热，不把本人静音当过载。
class AudioCaptureGate {
public:
  // overloaded 的frames是本腿成功发送的20ms帧数，不含静音保活；阈值35fps低于正常50fps。
  bool overloaded(std::uint64_t now, std::uint64_t frames, bool muted = false) {
    if (!initialized_ || muted || now < window_ || frames < frames_) {
      initialized_ = true; started_ = window_ = now; frames_ = frames; return false;
    }
    // 媒体腿先建立、采集任务后启动时，首个非零帧批次不能继承空采集期间的时钟；
    // 从真实音频出现的时刻重新计三秒预热，避免把启动爬坡误判为持续过载。
    if (frames_ == 0 && frames > 0) {
      started_ = window_ = now;
      frames_ = frames;
      return false;
    }
    const auto elapsed = now - window_;
    if (elapsed < 1000) return false;
    const bool slow = now - started_ >= 3000 && (frames - frames_) * 1000 < elapsed * 35;
    window_ = now; frames_ = frames; return slow;
  }
private:
  bool initialized_ = false; // 单调零值也是合法首样本，不能用时间零作哨兵。
  std::uint64_t started_ = 0, window_ = 0; // 会话/本人静音后重新预热与当前窗口起点。
  std::uint64_t frames_ = 0; // 当前窗口的成功帧计数水位。
};
} // namespace ur::device
