"""执行主动拨号的生产队列与让位流程，验证取消、超时及并发意图不会迟到拨号。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

# ROOT定位生产源码，回归直接抽取实际方法而非复制实现。
ROOT = Path(__file__).resolve().parents[2]


class OutgoingCallTest(unittest.TestCase):
    """验证跨任务拨号意图与每条客户端媒体腿的生命周期。"""
    def test_recreated_legs_have_distinct_create_prefixes(self):
        """复用宿主时创建请求前缀不重用，纯语音不选择视频接收。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        actual = method(source, 'bool UrSignalingCall::Impl::EnsureLeg(')
        code = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>
#define CONFIG_UR_HUMAN_CALL_UDP_AV 1
#define ESP_LOGI(...) ((void)0)
std::string RandomCallInstance() {
    static int serial = 0;
    return "call-" + std::to_string(++serial);
}
namespace ur::device {
enum class HumanCallMode { Voice, Video };
struct HumanCallConfig {
    std::string camera_id;
    bool borrowed_connection = false, voice_and_video = false, receive_video = false,
         udp_av = false, udp_only = false;
    HumanCallMode mode;
};
struct HumanCallClient {
    static std::vector<std::string> prefixes;
    static std::vector<HumanCallConfig> configs;
    template <class Port, class Media>
    HumanCallClient(Port &, Media &, const std::string &, const std::string &prefix,
                    HumanCallConfig config) {
        prefixes.push_back(prefix);
        configs.push_back(config);
    }
    bool start_shared(std::uint64_t) {
        return true;
    }
};
std::vector<std::string> HumanCallClient::prefixes;
std::vector<HumanCallConfig> HumanCallClient::configs;
} // namespace ur::device
struct UrSignalingCall {
    struct Impl {
        struct PeerMedia {
            explicit PeerMedia(Impl &) {
            }
            bool video_receive_supported() {
                return true;
            }
        };
        int port = 0;
        int *port_ = &port;
        std::string instance_, identity_ = "p/d";
        std::unique_ptr<PeerMedia> media_;
        std::unique_ptr<ur::device::HumanCallClient> client_;
        bool EnsureUiQueue() {
            return true;
        }
        bool EnsureLeg(std::uint64_t, bool = false);
    };
};
PRODUCTION
int main() {
    UrSignalingCall::Impl call;
    assert(call.EnsureLeg(1));
    call.client_.reset();
    call.media_.reset();
    assert(call.EnsureLeg(2));
    const auto &prefixes = ur::device::HumanCallClient::prefixes;
    assert(prefixes.size() == 2 && prefixes[0] != prefixes[1]);
    call.client_.reset();
    call.media_.reset();
    assert(call.EnsureLeg(3, true));
    const auto &configs = ur::device::HumanCallClient::configs;
    assert(configs[0].receive_video);
    assert(configs[2].mode == ur::device::HumanCallMode::Voice && !configs[2].receive_video);
}
'''.replace('PRODUCTION', actual)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'prefixes')
            result = subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-', '-o', binary], input=code, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)

    def test_queue_yield_cancel_and_timeout(self):
        """队列满、旧代次、取消与让位超时均不能产生迟到拨号。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        functions = '\n'.join(method(source, signature) for signature in (
            'bool UrSignalingCall::Impl::PostDial(',
            'void UrSignalingCall::Impl::AdvanceDial(',
            'void UrSignalingCall::Impl::DrainUiCommands(',
        ))
        apply = method(source, 'void UrSignalingCall::Impl::ApplyUiCommand(')
        apply = apply[:apply.index('    const auto state = client_->state();')] + '    (void)now;\n}\n'
        functions += '\n' + apply
        command = method(source, 'struct QueuedUiCommand') + ';'
        code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int pdTRUE = 1;
constexpr std::uint64_t kYieldTimeoutMs = 10000;
namespace ur::device {
enum class DecodeResult { Ok };
struct Envelope {};
DecodeResult decode_envelope(const std::string &, Envelope &) {
    return DecodeResult::Ok;
}
} // namespace ur::device
struct PendingCallTombstones {
    enum class Decision { Declined };
};
struct Queue {
    std::deque<std::vector<char>> values;
    unsigned size = 0;
};
int xQueueSend(Queue *q, const void *item, int) {
    if (q->values.size() == 8)
        return 0;
    const auto *p = static_cast<const char *>(item);
    q->values.emplace_back(p, p + q->size);
    return pdTRUE;
}
int xQueueReceive(Queue *q, void *item, int) {
    if (q->values.empty())
        return 0;
    std::memcpy(item, q->values.front().data(), q->size);
    q->values.pop_front();
    return pdTRUE;
}
struct Client {
    int calls = 0;
    std::string target;
    bool dial(const char *value, std::uint64_t) {
        ++calls;
        target = value;
        return true;
    }
};
struct UrSignalingCall {
    enum class UiState { Hidden, Connecting, Ended };
    struct Impl {
        enum { kUiAccept = 1, kUiReject = 2, kUiHangUp = 3, kUiDial = 6 };
        COMMAND
        Queue queue;
        Queue *ui_queue_ = nullptr;
        Client storage;
        Client *client_ = nullptr;
        struct PendingInvite {
            std::string body;
        };
        std::optional<QueuedUiCommand> pending_dial_;
        std::optional<PendingInvite> pending_invite_;
        std::atomic<bool> dial_queued_{false};
        std::atomic<std::uint32_t> ui_generation_{0};
        std::atomic<bool> dial_ready_{false};
        bool voice_call_ = false;
        std::uint64_t dial_requested_at_ = 0, leg_started_ms_ = 0;
        UiState ui_state_ = UiState::Hidden;
        int yields = 0, allocations = 0, applied = 0;
        bool host_busy = false, yield_ok = true, allocate_ok = true, voice = false,
             pending_accept_ = false;
        bool HostBusy() {
            return host_busy;
        }
        bool RequestHostYield() {
            ++yields;
            return yield_ok;
        }
        bool Busy() {
            return client_ != nullptr || pending_dial_.has_value();
        }
        bool EnsureLeg(std::uint64_t, bool only) {
            ++allocations;
            voice = only;
            if (allocate_ok)
                client_ = &storage;
            return allocate_ok;
        }
        void TeardownLeg() {
            client_ = nullptr;
            dial_queued_.store(false);
        }
        void FireState(UiState) {
        }
        void RejectInvite(const ur::device::Envelope &, const char *) {
        }
        void RetirePending(PendingCallTombstones::Decision) {
        }
        bool PostDial(const std::string &, bool);
        void AdvanceDial(std::uint64_t);
        void DrainUiCommands(std::uint64_t);
        void ApplyUiCommand(int, std::uint64_t);
        void ready() {
            ui_queue_ = &queue;
            queue.size = sizeof(QueuedUiCommand);
            dial_ready_.store(true);
        }
    };
};
PRODUCTION
int main() {
    UrSignalingCall::Impl call;
    assert(!call.PostDial("p:b", false));
    call.ready();
    for (const auto *target : {"", ":b", "p:", "p/b", "p:+", "p:b\n"})
        assert(!call.PostDial(target, false));
    call.host_busy = true;
    assert(call.PostDial("p:b", true));
    assert(!call.PostDial("p:c", false));
    call.DrainUiCommands(100);
    call.AdvanceDial(100);
    assert(call.pending_dial_ && call.yields == 1 && call.allocations == 0);
    call.AdvanceDial(500);
    assert(call.allocations == 0);
    call.host_busy = false;
    call.AdvanceDial(600);
    assert(!call.pending_dial_ && call.client_ && call.storage.calls == 1 && call.voice);
    assert(call.storage.target == "p:b" && call.leg_started_ms_ == 600 && call.dial_queued_);
    UrSignalingCall::Impl cancel;
    cancel.ready();
    cancel.host_busy = true;
    assert(cancel.PostDial("p:b", false));
    cancel.DrainUiCommands(100);
    cancel.ApplyUiCommand(cancel.kUiHangUp, 101);
    cancel.host_busy = false;
    cancel.AdvanceDial(200);
    assert(cancel.allocations == 0 && !cancel.dial_queued_);
    UrSignalingCall::Impl expired;
    expired.ready();
    expired.host_busy = true;
    assert(expired.PostDial("p:b", false));
    expired.DrainUiCommands(100);
    expired.AdvanceDial(10100);
    assert(expired.allocations == 0 && !expired.pending_dial_ && !expired.dial_queued_);
    UrSignalingCall::Impl occupied;
    occupied.ready();
    occupied.pending_invite_.emplace();
    assert(occupied.PostDial("p:b", false));
    occupied.DrainUiCommands(1);
    assert(!occupied.pending_dial_ && !occupied.dial_queued_);
    UrSignalingCall::Impl stale;
    stale.ready();
    assert(stale.PostDial("p:b", false));
    ++stale.ui_generation_;
    stale.DrainUiCommands(1);
    assert(!stale.pending_dial_ && !stale.dial_queued_);
    UrSignalingCall::Impl full;
    full.ready();
    for (int i = 0; i < 8; ++i) {
        UrSignalingCall::Impl::QueuedUiCommand q{full.kUiAccept, 0};
        assert(xQueueSend(full.ui_queue_, &q, 0) == pdTRUE);
    }
    assert(!full.PostDial("p:b", false) && !full.dial_queued_);
}
'''.replace('COMMAND', command).replace('PRODUCTION', functions)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'outgoing')
            result = subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-', '-o', binary], input=code, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)
