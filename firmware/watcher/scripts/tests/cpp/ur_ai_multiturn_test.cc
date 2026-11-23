/* 编译生产多轮测试入口与就绪快照，替换时钟/硬件，验证测试不会把重开会话算作多轮。 */
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 模拟串口任务时钟，不依赖真实等待；日志只用于检查生产函数失败原因。
using TickType_t = int;
static int64_t now_us;
static std::string logs;
static int scenario;
// selected_photo 记录样本选择，防止生产入口选错资源却通过关键词门禁。
static bool selected_photo;
// selected_control证明测试确实选择设置音量音频，而不是复用查询样本。
static bool selected_control;
// selected_forward记录坦克前进样本，防止新增参数与测试边界失配。
static bool selected_forward;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(int ms) { now_us += ms * 1000LL; }
#define pdMS_TO_TICKS(ms) (ms)
void Record(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs += std::string(buffer) + "\n";
}
#define ESP_LOGI(tag, ...) Record(__VA_ARGS__)
#define ESP_LOGE(tag, ...) Record(__VA_ARGS__)

// 只保留生产函数调用的边界；每一轮 fixture 都有独立读取游标。
struct UrAiE2eAudioFrame { const uint8_t* data; size_t size; };
struct AudioStreamPacket {
    int sample_rate, frame_duration;
    uint32_t timestamp;
    std::vector<uint8_t> payload;
};
struct UrAiE2eFixture {
    explicit UrAiE2eFixture(bool photo = false, bool control = false, bool forward = false) : take_photo(photo) {
        selected_photo = photo;
        selected_control = control;
        selected_forward = forward;
    }
    bool take_photo;
    int read = 0;
    bool IsValid() const { return true; }
    int sample_rate() const { return 16000; }
    int frame_duration_ms() const { return 60; }
    int frame_count() const { return 1; }
    bool Next(UrAiE2eAudioFrame* frame) {
        static const uint8_t packet[] = {1};
        if (read++) return false;
        *frame = {packet, sizeof(packet)};
        return true;
    }
};
struct UrAiE2eTurn { bool ready; std::string session_id, audio_token; };
struct UrAiE2eResult {
    bool stt_done = true, stt_nonempty = true, stt_matches_fixture = true;
    bool text_done = true, text_nonempty = true, first_audio = true, audio_done = true;
    bool service_error = false;  // 模拟服务错误被当作正常文本和语音返回。
};
enum { kDeviceStateIdle, kDeviceStateListening };

// 替换编解码硬件读数；实际应用函数仍必须拒绝未准备或未改变的状态。
struct AudioCodec { int volume=55; int output_volume() const { return volume; } };
struct Board {
    AudioCodec codec;
    static Board& GetInstance() { static Board board; return board; }
    AudioCodec* GetAudioCodec() { return &codec; }
};

// 场景 1 旧 token、2 会话变化、3 第二轮识别失败、4 上传失败、5 未就绪、6 音频确认超时。
struct UrAiProtocol {
    mutable std::mutex state_mutex_;
    bool audio_upload_ready_ = true, audio_start_pending_ = false, audio_stop_sent_ = false;
    std::string session_id_ = "session-a", expected_audio_token_;
    bool open = false;
    int completed = 0, sent = 0, closes = 0, vad = 0;
    std::vector<bool> clear_requests;
    std::function<void()> on_result;
    UrAiE2eTurn GetE2eTurn() const;
    bool IsAudioChannelOpened() const { return open; }
    void CloseAudioChannel() { open = false; ++closes; }
    void ResetE2eObservation(bool clear = true) { clear_requests.push_back(clear); }
    bool WaitForAudioUploadReady(int) {
        expected_audio_token_ = "audio-" + std::to_string(scenario == 1 ? 0 : completed);
        if (scenario == 2 && completed) session_id_ = "session-b";
        audio_start_pending_ = scenario == 5 && completed;
        return scenario != 6;
    }
    void OnLocalVadState(bool) { ++vad; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket>) { ++sent; return scenario != 4; }
    UrAiE2eResult WaitForE2eResult(int timeout, const std::string& expected_text, const std::string& expected_stt) {
        assert(expected_text.empty());
        assert(expected_stt == (selected_forward ? "前" : (selected_photo ? "拍照" : "音量")));
        assert(timeout == (selected_photo ? 120000 : 60000));
        ++completed;
        on_result();
        UrAiE2eResult result;
        if (scenario == 3 && completed == 2) result.stt_matches_fixture = false;
        if (scenario == 9) result.service_error = true;
        return result;
    }
};
// PRODUCTION_READY_SNAPSHOT

struct Application {
    std::atomic<bool> ur_ai_e2e_running_{false}, ur_ai_e2e_speaking_observed_{false};
    std::unique_ptr<UrAiProtocol> protocol_{new UrAiProtocol};
    struct Audio { bool IsPlaybackIdle() const { return scenario != 7; } } audio_service_;
    int state = kDeviceStateIdle, toggles = 0;
    int GetDeviceState() const { return state; }
    void ToggleChatState() { ++toggles; state = kDeviceStateListening; protocol_->open = true; }
    bool RunUrAiE2eTest(int turns = 1, bool take_photo = false, bool control_volume = false, bool tank_forward = false);
};
// PRODUCTION_MULTITURN

// 生产快照必须拒绝已停止、仍在等确认或没有会话的状态。
void TestSnapshot() {
    UrAiProtocol protocol;
    protocol.expected_audio_token_ = "ready";
    assert(protocol.GetE2eTurn().ready);
    protocol.audio_upload_ready_ = false;
    assert(!protocol.GetE2eTurn().ready);
    protocol.audio_upload_ready_ = true;
    protocol.audio_stop_sent_ = true;
    assert(!protocol.GetE2eTurn().ready);
    protocol.audio_stop_sent_ = false;
    protocol.audio_start_pending_ = true;
    assert(!protocol.GetE2eTurn().ready);
    protocol.audio_start_pending_ = false;
    protocol.session_id_.clear();
    assert(!protocol.GetE2eTurn().ready);
}

int main() {
    TestSnapshot();
    for (int repeat = 0; repeat < 10; ++repeat) {
        for (scenario = 0; scenario <= 9; ++scenario) {
            now_us = 0;
            logs.clear();
            Application app;
            app.protocol_->on_result = [&]() {
                assert(app.ur_ai_e2e_running_.load());
                app.ur_ai_e2e_speaking_observed_.store(true);
                if (scenario == 8) app.state = kDeviceStateIdle;
            };
            const bool passed = app.RunUrAiE2eTest(3);
            assert(passed == (scenario == 0));
            assert(app.toggles == 1 && app.protocol_->closes == 1);
            assert(!app.ur_ai_e2e_running_.load());
            assert(app.protocol_->clear_requests.front());
            for (size_t i = 1; i < app.protocol_->clear_requests.size(); ++i)
                assert(!app.protocol_->clear_requests[i]);
            if (scenario == 0) {
                assert(app.protocol_->completed == 3 && app.protocol_->sent == 3);
                assert(logs.find("TURN_PASS index=3") != std::string::npos);
            } else {
                assert(logs.find("TURN_PASS index=3") == std::string::npos);
            }
            if (scenario == 1 || scenario == 5)
                assert(logs.find("new_turn_not_ready") != std::string::npos);
            if (scenario == 2)
                assert(logs.find("session_changed_between_turns") != std::string::npos);
            if (scenario == 7 || scenario == 8)
                assert(logs.find("playback_timeout") != std::string::npos);
            if (scenario == 9)
                assert(logs.find("model_service_error") != std::string::npos);
        }
        scenario = 0;
        Application single;
        single.protocol_->on_result = [&]() { single.ur_ai_e2e_speaking_observed_.store(true); };
        assert(single.RunUrAiE2eTest() && single.protocol_->sent == 1 && single.toggles == 1);
        Application photo;
        photo.protocol_->on_result = [&]() { photo.ur_ai_e2e_speaking_observed_.store(true); };
        assert(photo.RunUrAiE2eTest(1, true));
        assert(logs.find("fixture=take_photo") != std::string::npos);
        Application forward;
        forward.protocol_->on_result = [&]() { forward.ur_ai_e2e_speaking_observed_.store(true); };
        assert(forward.RunUrAiE2eTest(1, false, false, true) && selected_forward);
        Application invalid;
        assert(!invalid.RunUrAiE2eTest(0) && !invalid.RunUrAiE2eTest(6));
        assert(!invalid.RunUrAiE2eTest(1, true, true) && !invalid.RunUrAiE2eTest(2, false, true));
        assert(!invalid.RunUrAiE2eTest(2, false, false, true));
        assert(!invalid.RunUrAiE2eTest(1, true, false, true));
        assert(invalid.toggles == 0 && !invalid.ur_ai_e2e_running_.load());
        for (scenario = 0; scenario <= 2; ++scenario) {
            logs.clear();
            Board::GetInstance().codec.volume = scenario == 2 ? 40 : 55;
            Application control;
            control.protocol_->on_result = [&]() {
                assert(selected_control && !selected_photo);
                control.ur_ai_e2e_speaking_observed_.store(true);
                if (scenario == 0) Board::GetInstance().codec.volume = 40;
            };
            assert(control.RunUrAiE2eTest(1, false, true) == (scenario == 0));
            assert(!control.ur_ai_e2e_running_.load());
            if (scenario == 0)
                assert(logs.find("before=55 target=40 after=40 changed=1") != std::string::npos);
            if (scenario == 1)
                assert(logs.find("control_not_applied") != std::string::npos);
            if (scenario == 2) {
                assert(control.toggles == 0);
                assert(logs.find("control_baseline_mismatch") != std::string::npos);
            }
        }
    }
}
