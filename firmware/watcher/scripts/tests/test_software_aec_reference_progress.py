"""执行真实AFE方法，验证DSP阻塞时播放参考仍可入队，且数据顺序及上限不变。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    """提取实际生产方法，不维护另一份队列或锁实现。"""
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


class SoftwareAecReferenceProgressTest(unittest.TestCase):
    """将AFE硬件调用替换为确定性阻塞，保留原Feed/FeedReference方法。"""

    def test_reference_progress_while_afe_feed_waits(self):
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        methods = '\n'.join(method(source, signature) for signature in [
            'void AfeAudioEngine::Feed(', 'void AfeAudioEngine::FeedReference(',
        ])
        code = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <vector>
#define CONFIG_UR_SOFTWARE_AEC 1
using EventBits_t = unsigned;
constexpr unsigned kAfeActive = 1, kVoiceProcessingEnabled = 2;
constexpr bool kUseAfeForVoiceProcessing = true;
unsigned xEventGroupGetBits(int) { return kAfeActive; }
// 假AFE只提供硬件阻塞和参考快照，不复制生产缓冲逻辑。
struct FeedGate {
    bool block = true;
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
    std::vector<int16_t> last;
};
struct FakeAfe {
    size_t get_feed_chunksize(void*) { return 4; }
    void feed(void* data, const int16_t* samples) {
        auto& gate = *static_cast<FeedGate*>(data);
        gate.last.assign(samples, samples + 8);
        if (gate.block) { gate.entered.set_value(); gate.released.wait(); }
    }
};
struct FakeCodec { unsigned input_channels() { return 1; } };
struct AfeAudioEngine {
    int event_group_ = 1;
    FeedGate gate;
    FakeAfe afe;
    FakeAfe* afe_iface_ = &afe;
    void* afe_data_ = &gate;
    FakeCodec codec;
    FakeCodec* codec_ = &codec;
    std::mutex input_buffer_mutex_, reference_buffer_mutex_;
    std::vector<int16_t> mic_staging_, input_buffer_, reference_interleaved_;
    std::deque<int16_t> reference_ring_;
    static constexpr size_t kReferenceRingLowWater = 1600;
    static constexpr size_t kReferenceRingCapacity = 8000;
    bool UsesReferenceChannel() { return true; }
    void OutputRawAudio(std::vector<int16_t>&) {}
    void Feed(std::vector<int16_t>&&);
    void FeedReference(std::vector<int16_t>&&);
};
METHODS
int main() {
    // 真实Feed进入阻塞后，参考必须独立完成；总是先释放线程再作失败断言。
    AfeAudioEngine engine;
    auto entered = engine.gate.entered.get_future();
    auto feeding = std::async(std::launch::async, [&] { engine.Feed({1,2,3,4}); });
    assert(entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto reference = std::async(std::launch::async, [&] { engine.FeedReference({5,6,7,8}); });
    bool progressed = reference.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
    engine.gate.release.set_value();
    feeding.get(); reference.get();
    if (!progressed) { std::cerr << "reference enqueue waited for blocked AFE feed\n"; return 1; }
    assert(engine.reference_ring_.size() == 4);
    // 低水位、参考顺序和有界丢旧策略不因锁拆分而改变。
    AfeAudioEngine ordered; ordered.gate.block = false;
    std::vector<int16_t> samples(1604);
    for (int i = 0; i < 1604; ++i) samples[i] = i + 10;
    ordered.FeedReference(std::move(samples));
    ordered.Feed({1,2,3,4});
    assert(ordered.gate.last == std::vector<int16_t>({1,10,2,11,3,12,4,13}));
    assert(ordered.reference_ring_.size() == 1600);
    ordered.FeedReference(std::vector<int16_t>(9000,99));
    assert(ordered.reference_ring_.size() == 8000);
    assert(ordered.reference_ring_.front() == 99);
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-aec-progress-') as directory:
            binary = str(Path(directory) / 'reference-progress')
            compiled = subprocess.run(['g++', '-std=c++17', '-pthread', '-x', 'c++', '-', '-o', binary],
                                      input=code, text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interleaving_reuses_storage_without_changing_samples(self):
        """回放10ms输入，核对完整AFE块、参考欠载和停启后的数据，并统计交错块分配。"""
        source = (ROOT / 'main/audio/engines/afe_audio_engine.cc').read_text()
        methods = '\n'.join(method(source, signature) for signature in [
            'void AfeAudioEngine::Feed(', 'void AfeAudioEngine::FeedReference(',
        ])
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <new>
#include <vector>
#ifndef CONFIG_UR_SOFTWARE_AEC
#define CONFIG_UR_SOFTWARE_AEC 1
#endif
using EventBits_t = unsigned;
constexpr unsigned kAfeActive = 1, kVoiceProcessingEnabled = 2;
constexpr bool kUseAfeForVoiceProcessing = true;
// 只计AFE交错块大小；输入向量为320字节，不纳入该计数。
static bool count_allocations = false;
static size_t interleaving_allocations = 0;
void* operator new(size_t size) {
    if (count_allocations && size == 2048) ++interleaving_allocations;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
unsigned xEventGroupGetBits(unsigned bits) { return bits; }
// 假AFE同步检查调用方的采样，不另行分配或复制音频缓冲。
struct FeedCheck {
    bool reference = true;
    size_t samples = 0, calls = 0;
    bool expect_reference_prefix = false;
};
struct FakeAfe {
    size_t get_feed_chunksize(void*) { return 512; }
    void feed(void* data, const int16_t* samples) {
        auto& check = *static_cast<FeedCheck*>(data);
        for (size_t i = 0; i < 512; ++i) {
            const size_t position = check.samples + i;
            assert(samples[i * (check.reference ? 2 : 1)] == int16_t(position % 30000));
            if (check.reference) {
                const int16_t expected = check.expect_reference_prefix && position < 101 ? 700 : 0;
                assert(samples[i * 2 + 1] == expected);
            }
        }
        check.samples += 512;
        ++check.calls;
    }
};
struct FakeCodec { unsigned input_channels() { return 1; } };
struct AfeAudioEngine {
    unsigned event_group_ = kAfeActive;
    FeedCheck check;
    FakeAfe afe;
    FakeAfe* afe_iface_ = &afe;
    void* afe_data_ = &check;
    FakeCodec codec;
    FakeCodec* codec_ = &codec;
    std::mutex input_buffer_mutex_, reference_buffer_mutex_;
    std::vector<int16_t> mic_staging_, input_buffer_, reference_interleaved_;
    std::deque<int16_t> reference_ring_;
    static constexpr size_t kReferenceRingLowWater = 1600;
    static constexpr size_t kReferenceRingCapacity = 8000;
    bool UsesReferenceChannel() { return check.reference; }
    void OutputRawAudio(std::vector<int16_t>&) {}
    void Feed(std::vector<int16_t>&&);
    void FeedReference(std::vector<int16_t>&&);
};
METHODS
// 每次只供应10ms输入；跨调用剩余样本必须延续到下一AFE块。
void replay(AfeAudioEngine& engine, size_t count, size_t start = 0) {
    for (size_t part = 0; part < count; ++part) {
        std::vector<int16_t> input(160);
        for (size_t i = 0; i < 160; ++i) input[i] = (start + part * 160 + i) % 30000;
        engine.Feed(std::move(input));
    }
}
int main() {
    AfeAudioEngine engine;
#if !CONFIG_UR_SOFTWARE_AEC
    engine.check.reference = false;
#endif
    count_allocations = true;
    // 无完整块不应分配交错缓冲；禁用时不能吞入样本。
    engine.event_group_ = 0;
    engine.Feed({999});
    assert(interleaving_allocations == 0 && engine.check.calls == 0);
    engine.event_group_ = kAfeActive;
    replay(engine, 3);
    assert(interleaving_allocations == 0 && engine.check.calls == 0);
    replay(engine, 997, 480);
    count_allocations = false;
    assert(engine.check.calls == 312 && engine.check.samples == 159744);
#if CONFIG_UR_SOFTWARE_AEC
    assert(engine.mic_staging_.size() == 256);
    std::cout << "interleaving_allocations=" << interleaving_allocations << "\n";
    if (interleaving_allocations != 1) return 1;
    // 跨停启复用存储，上一块的非零参考必须全部被当前静音覆盖。
    AfeAudioEngine reference;
    reference.check.expect_reference_prefix = true;
    reference.FeedReference(std::vector<int16_t>(1700, 700));
    replay(reference, 4);
    reference.event_group_ = 0;
    reference.Feed({999});
    reference.event_group_ = kAfeActive;
    replay(reference, 4, 640);
    assert(reference.check.calls == 2 && reference.reference_ring_.size() == 1599);
    // 软件AEC编入但板级不使用软件参考时，原单通道路径保持有效。
    AfeAudioEngine hardware;
    hardware.check.reference = false;
    replay(hardware, 1000);
    assert(hardware.check.samples == 159744 && hardware.input_buffer_.size() == 256);
#else
    assert(engine.input_buffer_.size() == 256 && interleaving_allocations == 0);
#endif
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='watcher-aec-storage-') as directory:
            for enabled in (1, 0):
                with self.subTest(software_aec=enabled):
                    binary = str(Path(directory) / f'feed-storage-{enabled}')
                    compiled = subprocess.run([
                        'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        f'-DCONFIG_UR_SOFTWARE_AEC={enabled}', '-x', 'c++', '-', '-o', binary,
                    ], input=code, text=True, capture_output=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([binary], text=True, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
