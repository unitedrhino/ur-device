// 文件说明：MQTT协商+UDP双向音视频的显式硬件验收入口。
// 控制回调仅交接有界配置，媒体事件任务优先处理音频；JPEG解码使用独立低优先级任务。
#include "ur_av_probe.h"
#include "sdkconfig.h"
#if CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include <freertos/idf_additions.h>
#include <mqtt_client.h>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "application.h"
#include "board.h"
#include "esp_jpeg_dec.h"
#include "lcd_display.h"
#include "peer.h"
#include "peer_connection.h"
#include "sscma_camera.h"
#include "ur/device/udp_av.hpp"
#include "ur_signaling_call.h"
#include "ur_signaling_call_video.h"
#include "ur_signaling_cipher.h"
#include "ur_signaling_opus.h"
#include "ur_signaling_pcm.h"
#include "ur_signaling_protocol.h"
#include "ur_signaling_udp_port.h"
#include "ur_signaling_video.h"

namespace {
constexpr const char* kTag = "ur_av_probe";  // 验收计数不包含凭据或原媒体。
std::atomic<bool> active{false};             // 显式验收入口互斥，不得并发抢相机与音频。

// ProbeContext 只属于一次90秒验收；MQTT销毁后才能释放，不存储到NVS。
struct ProbeContext {
    std::string host, id;                     // 用户指定的私网主机与公开测试标识。
    uint16_t port = 0;                        // 临时Broker端口。
    std::mutex mutex;                         // 只保护MQTT配置交接，不在锁内编解码或操作屏幕。
    std::string configuration;                // 只保留一次性媒体参数，最多1024字节。
    std::atomic<int> published{0};            // 最后已确认的QoS1消息，结束后等回执再销毁连接。
    std::atomic<bool> disconnected{false};    // 控制连接断开立即退出媒体。
    bool received = false;                    // 第一条配置后不允许替换本会话参数。
    std::string answer;                       // WebRTC对照组至多一条完整应答，由事件任务消费。
    esp_mqtt_client_handle_t mqtt = nullptr;  // 借用ProbeTask持有的测试连接。
    char* pending = nullptr;  // 仅在PSRAM保存一个有界MQTT正文，避免扩充内部接收缓冲。
    size_t pending_size = 0, pending_total = 0;  // 连续分块长度，拒绝乱序或超限正文。
    ~ProbeContext() { heap_caps_free(pending); }
};

// Hex 读取固定长度十六进制会话参数；非法字符不进入密码端口。
template <size_t N>
bool Hex(const cJSON* root, const char* name, std::array<uint8_t, N>& out) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || std::strlen(value->valuestring) != N * 2)
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < N; i++) {
        int a = digit(value->valuestring[i * 2]), b = digit(value->valuestring[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (a << 4) | b;
    }
    return true;
}

// MqttEvent 仅订阅/发布本次精确测试Topic；配置不跨回调借用、不执行耗时硬件动作。
void MqttEvent(void* arg, esp_event_base_t, int32_t event_id, void* event_data) {
    auto& ctx = *static_cast<ProbeContext*>(arg);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    const std::string base = "ur/avprobe/" + ctx.id;
    if (event_id == MQTT_EVENT_CONNECTED)
        esp_mqtt_client_subscribe(event->client, (base + "/down").c_str(), 1);
    if (event_id == MQTT_EVENT_SUBSCRIBED) {
        constexpr auto body =
            R"({"method":"hello","audio":{"codec":"opus","sampleRate":16000,"frameMs":20},"video":{"codec":"jpeg","width":240,"height":240}})";
        esp_mqtt_client_publish(event->client, (base + "/up").c_str(), body, 0, 1, 0);
    }
    if (event_id == MQTT_EVENT_DISCONNECTED || event_id == MQTT_EVENT_ERROR)
        ctx.disconnected.store(true);
    if (event_id == MQTT_EVENT_PUBLISHED)
        ctx.published.store(event->msg_id);
    if (event_id == MQTT_EVENT_DATA && event->data_len > 0 && event->total_data_len > 0 &&
        event->total_data_len <= 16384) {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        if (event->current_data_offset == 0) {
            heap_caps_free(ctx.pending);
            ctx.pending = nullptr;
            ctx.pending_size = ctx.pending_total = 0;
            if (event->topic_len != int(base.size() + 5) ||
                std::string_view(event->topic, event->topic_len) != base + "/down")
                return;
            ctx.pending = static_cast<char*>(
                heap_caps_malloc(event->total_data_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!ctx.pending)
                return;
            ctx.pending_total = event->total_data_len;
        }
        if (!ctx.pending || event->current_data_offset != int(ctx.pending_size) ||
            event->total_data_len != int(ctx.pending_total) ||
            event->data_len > int(ctx.pending_total - ctx.pending_size))
            return;
        std::memcpy(ctx.pending + ctx.pending_size, event->data, event->data_len);
        ctx.pending_size += event->data_len;
        if (ctx.pending_size != ctx.pending_total)
            return;
        ctx.pending[ctx.pending_size] = '\0';
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(
            cJSON_ParseWithLength(ctx.pending, ctx.pending_size), cJSON_Delete);
        const auto* method = cJSON_GetObjectItemCaseSensitive(body.get(), "method");
        if (cJSON_IsString(method) && std::strcmp(method->valuestring, "answer") == 0) {
            if (ctx.answer.empty())
                ctx.answer.assign(ctx.pending, ctx.pending_size);
            return;
        }
        if (!ctx.received) {
            ctx.configuration.assign(ctx.pending, ctx.pending_size);
            ctx.received = true;
        }
    }
}

// JpegOutput 只保留最新帧；连续慢帧本次会话停显，500ms无新帧清屏，音频不等待解码。
class JpegOutput {
public:
    explicit JpegOutput(LcdDisplay* display) : display_(display) {}
    ~JpegOutput() { Stop(); }
    bool Start() {
        return xTaskCreateWithCaps(
                   [](void* p) {
                       static_cast<JpegOutput*>(p)->Run();
                       vTaskDeleteWithCaps(nullptr);
                   },
                   "av_jpeg", 12288, this, 3, &task_,
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    void Submit(std::vector<uint8_t> image) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(image);
        received_ = esp_timer_get_time();
    }
    // ProtectAudio 仅置位停显门，不在音频/网络任务等待显示锁。
    void ProtectAudio() { audio_overload_.store(true); }
    void Stop() {
        stop_.store(true);
        while (task_ && !done_.load())
            vTaskDelay(pdMS_TO_TICKS(10));
        task_ = nullptr;
    }

private:
    // Run 独占JPEG解码器与两个PSRAM输出缓冲，退出时先在显示锁内清源再释放缓冲。
    void Run() {
        jpeg_dec_handle_t decoder = nullptr;
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        cfg.rotate = JPEG_ROTATE_0D;
        uint8_t* buffers[2] = {};
        bool initialized = jpeg_dec_open(&cfg, &decoder) == JPEG_ERR_OK;
        for (auto& p : buffers) {
            p = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(16, 240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            initialized = initialized && p;
        }
        ur::device::RemoteVideoGate gate;
        int index = 0;
        unsigned frames = 0, errors = 0, slow = 0;
        bool shown = false, blocked = false;
        if (initialized && display_) {
            display_->SetRemotePreview(true);
            display_->SetPreviewFullscreen(true);
        }
        while (initialized && !stop_.load()) {
            std::vector<uint8_t> image;
            int64_t received = 0;
            const uint64_t now = esp_timer_get_time() / 1000;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                image.swap(latest_);
                received = received_;
            }
            if (!blocked &&
                (audio_overload_.load() || (display_ && display_->RemotePreviewSlow()))) {
                blocked = true;
                ++slow;
                ESP_LOGI(kTag, "video disabled for slow DMA or capture gap; audio continues");
            }
            if (!image.empty() && !blocked && gate.accept(now)) {
                const auto start = esp_timer_get_time();
                jpeg_dec_io_t io{};
                jpeg_dec_header_info_t header{};
                io.inbuf = image.data();
                io.inbuf_len = image.size();
                int bytes = 0;
                if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
                    header.width != 240 || header.height != 240 ||
                    jpeg_dec_get_outbuf_len(decoder, &bytes) != JPEG_ERR_OK ||
                    bytes > 240 * 240 * 2 || bytes <= 0) {
                    ++errors;
                    continue;
                }
                const int consumed = io.inbuf_len - io.inbuf_remain;
                io.inbuf += consumed;
                io.inbuf_len = io.inbuf_remain;
                io.outbuf = buffers[index];
                if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
                    ++errors;
                    continue;
                }
                if (display_)
                    display_->SetStreamPreview(buffers[index], 240, 240, received);
                const uint64_t elapsed = (esp_timer_get_time() - start) / 1000;
                if (!gate.completed(now, elapsed)) {
                    ++slow;
                    blocked = true;
                }
                ++frames;
                shown = true;
                index = 1 - index;
                if (frames % 10 == 1)
                    ESP_LOGI(kTag,
                             "down_video frames=%u errors=%u decode_display_ms=%llu slow_hide=%u",
                             frames, errors, static_cast<unsigned long long>(elapsed), slow);
            }
            if (shown && (blocked || !gate.visible(now)) && display_) {
                display_->LcdDisplay::SetPreviewImage(nullptr);
                shown = false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (display_) {
            display_->LcdDisplay::SetPreviewImage(nullptr);
            display_->SetPreviewFullscreen(false);
            display_->SetRemotePreview(false);
        }
        for (auto p : buffers)
            heap_caps_free(p);
        if (decoder)
            jpeg_dec_close(decoder);
        ESP_LOGI(kTag, "video_end frames=%u errors=%u slow_hide=%u", frames, errors, slow);
        done_.store(true);
    }
    LcdDisplay* display_;  // 借用板级显示，生命周期覆盖验收。
    std::mutex mutex_;
    std::vector<uint8_t> latest_;
    int64_t received_ = 0;                     // 最新完整JPEG单槽。
    std::atomic<bool> audio_overload_{false};  // 收音出现长空窗后，本次会话不再解码对端视频。
    std::atomic<bool> stop_{false}, done_{false};
    TaskHandle_t task_ = nullptr;  // 停止后等待真实退出。
};

// FinishAudio 停止采集后最多等250ms驱动完成回执，返回真实已完成帧数，不把尾部清队列当完成。
uint32_t FinishAudio(ur::device::UrSignalingPcm& pcm) {
    pcm.end_capture();
    const auto deadline = esp_timer_get_time() + 250000;
    while (!pcm.playback_empty(1) && esp_timer_get_time() < deadline) {
        pcm.PumpPlayback();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    pcm.PumpPlayback();
    return pcm.underlying().completed_playback_frames();
}

// RtcProbeState 由libpeer事件任务推进；解码回调只更新原子视频事实及自己的双缓冲。
struct RtcProbeState {
    PeerConnection* peer = nullptr;                     // 本次显式对照组的真实设备Peer。
    ur::device::UrSignalingPcm* pcm = nullptr;          // 借用事件任务持有的PCM桥。
    ur::device::UrSignalingOpus* codec = nullptr;       // Opus编解码只在事件任务调用。
    ur::device::UrSignalingVideo video;                 // 既有H264软编码管线，不改产线参数。
    ur::device::CallVideoRenderer* renderer = nullptr;  // 既有下行H264解码器。
    std::atomic<bool> connected{false}, video_disabled{false};  // 连通和停显相互独立。
    std::shared_ptr<std::atomic<uint64_t>> last_display_ms =
        std::make_shared<std::atomic<uint64_t>>(0);  // 只发布完成时间，清屏不阻塞网络任务。
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);        // 延迟UI工作在本次退出后失效。
    std::atomic<unsigned> displayed{0};                   // 解码回调成功交付帧数。
    unsigned audio_rx = 0, play_errors = 0, nalu_rx = 0;  // 仅事件任务访问。
    uint8_t* buffers[2] = {};
    int write = 0;                                   // 回调双PSRAM缓冲，不异步借用解码器单缓冲。
    ur::device::RemoteVideoGate gate;                // 连续慢帧永久退让本次通话。
    uint64_t decode_wall_us = 0, decode_max_us = 0;  // Opus解码段墙钟，包含被其他任务抢占的等待。
    uint64_t write_wall_us = 0, encode_wall_us = 0;  // PCM交接与上行Opus编码的墙钟累计。
    uint64_t network_wall_us = 0,
             video_send_wall_us = 0;  // peer循环包含其音视频回调，不能与解码段重复相加。
    unsigned capture_frames = 0, encode_failures = 0;  // 区分采集缺帧和编码失败，不从网络收包反推。
    LcdDisplay* display = nullptr;                     // 生命周期覆盖本次验收。
    ~RtcProbeState() {
        alive->store(false);
        for (auto* p : buffers)
            heap_caps_free(p);
    }
};

// ValidProbeSDP 在进入libpeer固定数组解析器前检查全部行和候选预算。
// 私网验收只接纳IPv4 UDP候选；多网卡/过长SDP应安全拒绝，不能让解析器写穿其他堆对象。
bool ValidProbeSDP(std::string_view sdp) {
    if (sdp.empty() || sdp.size() > 16384 || sdp.find('\0') != std::string_view::npos)
        return false;
    unsigned candidates = 0;
    while (!sdp.empty()) {
        const auto end = sdp.find("\r\n");
        if (end == std::string_view::npos || end >= 256)
            return false;
        const auto line = sdp.substr(0, end);
        if (line.starts_with("a=candidate:")) {
            // 预留一个空槽，避免libpeer在去重前写入数组末尾之外。
            if (++candidates > 8)
                return false;
            std::vector<std::string_view> parts;
            auto fields = line.substr(12);
            while (!fields.empty()) {
                const auto space = fields.find(' ');
                parts.push_back(fields.substr(0, space));
                if (space == std::string_view::npos)
                    break;
                fields.remove_prefix(space + 1);
            }
            if (parts.size() < 8 || parts[0].size() > 32 || parts[2].size() > 32 ||
                (parts[2] != "udp" && parts[2] != "UDP") || parts[4].size() > 15 ||
                parts[7].size() > 15)
                return false;
            in_addr address{};
            if (lwip_inet_pton(AF_INET, std::string(parts[4]).c_str(), &address) != 1)
                return false;
        }
        sdp.remove_prefix(end + 2);
    }
    return candidates > 0;
}

// RunRTC 用同一测试Broker交换完整SDP，音视频均经ICE/DTLS/SRTP，不经过UDP/JPEG旁路。
void RunRTC(ProbeContext& ctx) {
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SuspendDecoder();
    ur::device::UrSignalingOpus codec;
    ur::device::AudioFormat format;
    format.sample_rate = 16000;
    format.channels = 1;
    format.frame_ms = 20;
    if (!codec.configure_for_call(format)) {
        audio.ResumeDecoder();
        return;
    }
    ur::device::UrSignalingPcm pcm(audio, format);
    RtcProbeState state;
    state.pcm = &pcm;
    state.codec = &codec;
    auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
    state.display = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    state.renderer = ur::device::GetCallVideoRenderer();
    for (auto& p : state.buffers)
        p = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, 240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const bool receive_video =
        state.renderer && state.buffers[0] && state.buffers[1] &&
        state.renderer->Start([&state](const uint8_t* rgb, int w, int h) {
            if (state.video_disabled.load() || w != 240 || h != 240)
                return;
            const auto started = esp_timer_get_time();
            if (state.display) {
                DisplayLockGuard lock(state.display);
                if (lock) {
                    std::memcpy(state.buffers[state.write], rgb, 240 * 240 * 2);
                    state.display->SetStreamPreview(state.buffers[state.write], 240, 240, started);
                    state.write = 1 - state.write;
                    state.last_display_ms->store(esp_timer_get_time() / 1000);
                }
            }
            const auto elapsed =
                (esp_timer_get_time() - started + state.renderer->LastFrameProcessingUs()) / 1000;
            if (!state.gate.completed(esp_timer_get_time() / 1000, elapsed))
                state.video_disabled.store(true);
            const auto count = state.displayed.fetch_add(1) + 1;
            if (count % 10 == 1)
                ESP_LOGI(kTag, "rtc_down_video frames=%u processing_display_ms=%llu disabled=%d",
                         count, static_cast<unsigned long long>(elapsed),
                         state.video_disabled.load());
        });
    if (!receive_video)
        state.video_disabled.store(true);
    if (state.display) {
        state.display->SetRemotePreview(true);
        state.display->SetPreviewFullscreen(true);
    }
    PeerConfiguration config{};
    config.audio_codec = CODEC_OPUS;
    config.video_codec = CODEC_H264;
    config.datachannel = DATA_CHANNEL_NONE;
    config.user_data = &state;
    config.onaudiotrack = [](uint8_t* data, size_t size, void* arg) {
        auto& s = *static_cast<RtcProbeState*>(arg);
        std::vector<uint8_t> opus(data, data + size);
        std::vector<int16_t> samples;
        const auto decode_start = esp_timer_get_time();
        const bool decoded = s.codec->decode(opus, samples);
        const uint64_t elapsed = esp_timer_get_time() - decode_start;
        s.decode_wall_us += elapsed;
        s.decode_max_us = std::max(s.decode_max_us, elapsed);
        const auto write_start = esp_timer_get_time();
        if (decoded && s.pcm->write_frame(samples, 1))
            ++s.audio_rx;
        else
            ++s.play_errors;
        s.write_wall_us += esp_timer_get_time() - write_start;
    };
    config.onvideotrack = [](uint8_t* data, size_t size, void* arg) {
        auto& s = *static_cast<RtcProbeState*>(arg);
        ++s.nalu_rx;
        if (!s.video_disabled.load() && s.renderer)
            s.renderer->OnNalu(data, size);
    };
    config.on_request_keyframe = [](void* arg) {
        static_cast<RtcProbeState*>(arg)->video.RequestIDR();
    };
    if (peer_init() == 0)
        state.peer = peer_connection_create(&config);
    if (state.peer) {
        peer_connection_oniceconnectionstatechange(state.peer, [](PeerConnectionState status,
                                                                  void* arg) {
            static_cast<RtcProbeState*>(arg)->connected.store(status == PEER_CONNECTION_CONNECTED);
        });
        const char* offer = peer_connection_create_offer(state.peer);
        if (offer) {
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> message(cJSON_CreateObject(),
                                                                    cJSON_Delete);
            cJSON_AddStringToObject(message.get(), "method", "offer");
            cJSON_AddStringToObject(message.get(), "sdp", offer);
            char* body = cJSON_PrintUnformatted(message.get());
            if (body) {
                esp_mqtt_client_publish(ctx.mqtt, ("ur/avprobe/" + ctx.id + "/up").c_str(), body, 0,
                                        1, 0);
                cJSON_free(body);
            }
            const auto deadline = esp_timer_get_time() + 15000000;
            std::string answer;
            while (!ctx.disconnected.load() && esp_timer_get_time() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(ctx.mutex);
                    answer = ctx.answer;
                }
                if (!answer.empty())
                    break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> parsed(cJSON_Parse(answer.c_str()),
                                                                   cJSON_Delete);
            const auto* sdp = cJSON_GetObjectItemCaseSensitive(parsed.get(), "sdp");
            if (cJSON_IsString(sdp) && ValidProbeSDP(sdp->valuestring))
                peer_connection_set_remote_description(state.peer, sdp->valuestring,
                                                       SDP_TYPE_ANSWER);
            while (!ctx.disconnected.load() && esp_timer_get_time() < deadline &&
                   !state.connected.load()) {
                peer_connection_loop(state.peer);
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }
    if (state.connected.load()) {
        audio.EnableVoiceProcessing(true);
        pcm.begin_capture("rtc-probe");
        pcm.begin_playback("rtc-probe", 1);
        if (camera)
            state.video.Start(
                camera, [camera] { return camera->preview_frame(); },
                ur::device::UrSignalingVideo::Profile::HumanCall);
        const auto started = esp_timer_get_time() / 1000;
        uint64_t last_video = 0, last_stat = 0, last_audio = started;
        unsigned sent_audio = 0, sent_video = 0, pending_max = 0;
        bool renderer_stopped = !receive_video;
        ur::device::AudioCaptureGate audio_gate;  // 相同窗口保护与正式驱动一致。
        bool video_pause_sent = false;            // 本会话停显须同步停止服务端转发。
        while (!ctx.disconnected.load() && state.connected.load() &&
               esp_timer_get_time() / 1000 - started < 60000) {
            const uint64_t now = esp_timer_get_time() / 1000;
            for (int step = 0; step < 8 && state.connected.load(); ++step) {
                const auto network_start = esp_timer_get_time();
                peer_connection_loop(state.peer);
                state.network_wall_us += esp_timer_get_time() - network_start;
                pcm.PumpPlayback();
                std::vector<int16_t> samples;
                std::vector<uint8_t> opus;
                if (pcm.read_frame(samples) == ur::device::PcmResult::Frame) {
                    ++state.capture_frames;
                    const auto encoded_at = esp_timer_get_time();
                    const bool encoded = codec.encode(samples, opus);
                    state.encode_wall_us += esp_timer_get_time() - encoded_at;
                    if (!encoded)
                        ++state.encode_failures;
                    if (encoded &&
                        peer_connection_send_audio(state.peer, opus.data(), opus.size()) == 0) {
                        ++sent_audio;
                        last_audio = esp_timer_get_time() / 1000;
                    }
                }
            }
            pending_max = std::max(
                pending_max, static_cast<unsigned>(pcm.underlying().pending_playback_frames()));
            std::vector<uint8_t> frame;
            if ((now - started > 3000 && (now > last_audio && now - last_audio > 120)) ||
                heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < 8192)
                state.video_disabled.store(true);
            if (audio_gate.overloaded(esp_timer_get_time() / 1000, sent_audio))
                state.video_disabled.store(true);
            if (state.video_disabled.load())
                state.video.PrioritizeAudio();
            if (now - last_video >= 200 && state.video.TakeFrame(frame)) {
                const auto send_start = esp_timer_get_time();
                if (peer_connection_send_video(state.peer, frame.data(), frame.size()) == 0)
                    ++sent_video;
                state.video_send_wall_us += esp_timer_get_time() - send_start;
                last_video = now;
            }
            if (state.renderer && !renderer_stopped && state.renderer->TakeKeyframeRequest())
                peer_connection_request_downlink_keyframe(state.peer);
            auto last_display = state.last_display_ms->load();
            if (state.display && last_display && now >= last_display && now - last_display >= 500 &&
                state.last_display_ms->compare_exchange_strong(last_display, 0)) {
                // 网络线程只投递清理，不等待LVGL锁；共享生存标记防止跨会话的延迟清屏。
                Application::GetInstance().Schedule(
                    [display = state.display, last = state.last_display_ms, alive = state.alive] {
                        if (!alive->load() || last->load() != 0)
                            return;
                        DisplayLockGuard lock(display);
                        if (lock && alive->load() && last->load() == 0)
                            display->LcdDisplay::SetPreviewImage(nullptr);
                    });
            }
            if (state.display && state.display->RemotePreviewSlow())
                state.video_disabled.store(true);
            if (state.video_disabled.load() && !video_pause_sent) {
                const std::string topic = "ur/avprobe/" + ctx.id + "/up";
                esp_mqtt_client_publish(ctx.mqtt, topic.c_str(), R"({"method":"videoOff"})", 0, 1,
                                        0);
                video_pause_sent = true;
            }
            if (state.video_disabled.load() && !renderer_stopped) {
                state.renderer->Stop();
                renderer_stopped = true;
                if (state.display)
                    state.display->LcdDisplay::SetPreviewImage(nullptr);
                ESP_LOGI(kTag, "rtc video disabled for slow frames; audio continues");
            }
            if (now - last_stat >= 5000) {
                last_stat = now;
                ESP_LOGI(kTag,
                         "rtc_media ms=%llu audio_tx=%u audio_rx=%u video_tx=%u video_rx_nalu=%u "
                         "displayed=%u video_disabled=%d play_errors=%u stack_free=%u",
                         static_cast<unsigned long long>(now - started), sent_audio, state.audio_rx,
                         sent_video, state.nalu_rx, state.displayed.load(),
                         state.video_disabled.load(), state.play_errors,
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        ESP_LOGI(kTag,
                 "rtc_wall_us decode=%llu decode_max=%llu pcm_write=%llu encode=%llu network=%llu "
                 "video_send=%llu capture_frames=%u encode_failures=%u",
                 state.decode_wall_us, state.decode_max_us, state.write_wall_us,
                 state.encode_wall_us, state.network_wall_us, state.video_send_wall_us,
                 state.capture_frames, state.encode_failures);
        ESP_LOGI(kTag,
                 "rtc_media_end audio_tx=%u audio_rx=%u video_tx=%u video_rx_nalu=%u displayed=%u "
                 "video_disabled=%d play_errors=%u played=%u pending_max=%u",
                 sent_audio, state.audio_rx, sent_video, state.nalu_rx, state.displayed.load(),
                 state.video_disabled.load(), state.play_errors,
                 static_cast<unsigned>(FinishAudio(pcm)), pending_max);
    } else
        ESP_LOGE(kTag, "rtc media did not connect");
    state.alive->store(false);
    state.video.Stop();
    if (camera)
        camera->SetPreviewEnabled(true);
    if (state.renderer)
        state.renderer->Stop();
    if (state.peer) {
        peer_connection_close(state.peer);
        peer_connection_destroy(state.peer);
    }
    pcm.end_capture();
    pcm.end_playback(1);
    audio.EnableVoiceProcessing(false);
    if (state.display) {
        state.display->LcdDisplay::SetPreviewImage(nullptr);
        state.display->SetPreviewFullscreen(false);
        state.display->SetRemotePreview(false);
    }
    codec.reset();
    audio.ResumeDecoder();
}

// RunMedia 只在明确start参数与双向nonce就绪后采集；退出作用域自动停止音频/相机并擦除密钥。
void RunMedia(ProbeContext& ctx, const std::string& body) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    const auto* method = cJSON_GetObjectItemCaseSensitive(root.get(), "method");
    const auto* transport = cJSON_GetObjectItemCaseSensitive(root.get(), "transport");
    if (cJSON_IsString(method) && std::strcmp(method->valuestring, "start") == 0 &&
        cJSON_IsString(transport) && std::strcmp(transport->valuestring, "webrtc") == 0) {
        RunRTC(ctx);
        return;
    }
    const auto* port = cJSON_GetObjectItemCaseSensitive(root.get(), "port");
    std::array<uint8_t, 16> key{};
    std::array<uint8_t, 8> up{}, down{};
    if (!cJSON_IsString(method) || std::strcmp(method->valuestring, "start") ||
        !cJSON_IsNumber(port) || port->valuedouble != port->valueint || port->valueint < 1 ||
        port->valueint > 65535 || !Hex(root.get(), "key", key) || !Hex(root.get(), "up", up) ||
        !Hex(root.get(), "down", down) || up == down) {
        ESP_LOGE(kTag, "configuration rejected");
        return;
    }
    ur::device::UrSignalingCipher cipher;
    if (!cipher.install(key))
        return;
    key.fill(0);
    ur::device::UdpAV channel(cipher, up, down);
    ur::device::UrSignalingUdpPort socket;
    if (!socket.open("0.0.0.0", 0) || !socket.set_peer(ctx.host, port->valueint))
        return;
    auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
    auto* display = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SuspendDecoder();
    ur::device::UrSignalingOpus codec;
    ur::device::AudioFormat format;
    format.sample_rate = 16000;
    format.channels = 1;
    format.frame_ms = 20;
    if (!codec.configure_for_call(format)) {
        audio.ResumeDecoder();
        ESP_LOGE(kTag, "codec allocation failed");
        return;
    }
    ur::device::UrSignalingPcm pcm(audio, format);
    JpegOutput output(display);
    if (!pcm.valid() || !output.Start()) {
        audio.ResumeDecoder();
        return;
    }
    if (camera) {
        camera->SetPreviewEnabled(false);
        camera->StartJpegStream();
    }
    audio.EnableVoiceProcessing(true);
    pcm.begin_capture("av-probe");
    pcm.begin_playback("av-probe", 1);
    uint32_t image_sequence = 0;
    uint64_t started = esp_timer_get_time() / 1000, last_image = 0, last_stat = 0;
    uint64_t last_audio = started;  // 冷启动3秒后，收音120ms空窗即让出呈现资源。
    unsigned sent_audio = 0, received_audio = 0, sent_video = 0, received_video = 0, invalid = 0,
             send_errors = 0, play_errors = 0;
    unsigned pending_max = 0;  // 已接纳但尚无驱动完成回执的最大帧数。
    while (!ctx.disconnected.load() && esp_timer_get_time() / 1000 - started < 60000) {
        const uint64_t now = esp_timer_get_time() / 1000;
        std::vector<int16_t> samples;
        std::vector<uint8_t> opus, packet, image;
        pcm.PumpPlayback();
        pending_max = std::max(pending_max,
                               static_cast<unsigned>(pcm.underlying().pending_playback_frames()));
        for (int pump = 0; pump < 3; ++pump) {
            if (pcm.read_frame(samples) != ur::device::PcmResult::Frame)
                break;
            if (codec.encode(samples, opus) && channel.audio(opus, packet)) {
                last_audio = now;
                if (socket.send(packet))
                    ++sent_audio;
                else
                    ++send_errors;
            }
        }
        if (now - started > 3000 && now - last_audio > 120)
            output.ProtectAudio();
        for (int i = 0; i < 16; i++) {
            auto result = socket.receive(packet);
            if (result == ur::device::ReceiveResult::Empty)
                break;
            if (result != ur::device::ReceiveResult::Received ||
                !channel.receive(packet, now, opus, image)) {
                ++invalid;
                continue;
            }
            if (!opus.empty()) {
                if (codec.decode(opus, samples) && pcm.write_frame(samples, 1))
                    ++received_audio;
                else
                    ++play_errors;
            }
            if (!image.empty()) {
                ++received_video;
                output.Submit(std::move(image));
            }
        }
        if (camera && now - last_image >= 200 && camera->TakeStreamJPEG(image, image_sequence)) {
            std::vector<std::vector<uint8_t>> packets;
            if (channel.jpeg(image.data(), image.size(), packets)) {
                bool sent = true;
                for (const auto& p : packets)
                    if (!socket.send(p)) {
                        sent = false;
                        ++send_errors;
                        break;
                    }
                if (sent)
                    ++sent_video;
            }
            last_image = now;
        }
        if (now - last_stat >= 5000) {
            last_stat = now;
            ESP_LOGI(kTag,
                     "media ms=%llu audio_tx=%u audio_rx=%u video_tx=%u video_rx=%u invalid=%u "
                     "send_errors=%u play_errors=%u stack_free=%u",
                     static_cast<unsigned long long>(now - started), sent_audio, received_audio,
                     sent_video, received_video, invalid, send_errors, play_errors,
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    const auto played = FinishAudio(pcm);
    pcm.end_playback(1);
    audio.EnableVoiceProcessing(false);
    if (camera) {
        camera->StopStream();
        camera->SetPreviewEnabled(true);
    }
    output.Stop();
    socket.close();
    codec.reset();
    audio.ResumeDecoder();
    ESP_LOGI(kTag,
             "media_end audio_tx=%u audio_rx=%u video_tx=%u video_rx=%u invalid=%u send_errors=%u "
             "play_errors=%u played=%u pending_max=%u",
             sent_audio, received_audio, sent_video, received_video, invalid, send_errors,
             play_errors, static_cast<unsigned>(played), pending_max);
}

// ProbeTask 使用独立临时Broker连接，退出销毁后再释放回调上下文；不自动重连或恢复采集。
void ProbeTask(void* arg) {
    std::unique_ptr<ProbeContext> ctx(static_cast<ProbeContext*>(arg));
    // 串口验收不进入正式通话状态机，须显式暂停调制解调省电，退出恢复原模式。
    wifi_ps_type_t previous_ps = WIFI_PS_NONE;
    const bool restore_ps = esp_wifi_get_ps(&previous_ps) == ESP_OK;
    if (restore_ps)
        esp_wifi_set_ps(WIFI_PS_NONE);
    const auto uri = "mqtt://" + ctx->host + ":" + std::to_string(ctx->port);
    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = uri.c_str();
    cfg.session.keepalive = 10;
    cfg.network.disable_auto_reconnect = true;
    cfg.buffer.size = 2048;
    auto mqtt = esp_mqtt_client_init(&cfg);
    if (mqtt) {
        ctx->mqtt = mqtt;
        esp_mqtt_client_register_event(mqtt, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                       MqttEvent, ctx.get());
        if (esp_mqtt_client_start(mqtt) == ESP_OK) {
            const auto deadline = esp_timer_get_time() + 15000000;
            std::string body;
            while (!ctx->disconnected.load() && esp_timer_get_time() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(ctx->mutex);
                    body = ctx->configuration;
                }
                if (!body.empty())
                    break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!body.empty())
                RunMedia(*ctx, body);
            else
                ESP_LOGE(kTag, "MQTT negotiation timeout");
            const int end_id = esp_mqtt_client_publish(
                mqtt, ("ur/avprobe/" + ctx->id + "/up").c_str(), "{\"method\":\"end\"}", 0, 1, 0);
            const auto ack_deadline = esp_timer_get_time() + 1000000;
            while (end_id >= 0 && ctx->published.load() != end_id && !ctx->disconnected.load() &&
                   esp_timer_get_time() < ack_deadline)
                vTaskDelay(pdMS_TO_TICKS(5));
        }
        esp_mqtt_client_stop(mqtt);
        esp_mqtt_client_destroy(mqtt);
    }
    if (restore_ps)
        esp_wifi_set_ps(previous_ps);
    ESP_LOGI(kTag, "probe stopped");
    active.store(false);
}
}  // namespace

// UrStartAVProbe 只允许用户串口显式指定的私网IPv4与安全Topic标识，通话/AI占用时拒绝。
bool UrStartAVProbe(const char* host, uint16_t port, const char* id) {
    auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
    if (!camera || camera->StreamActive())
        return false;
    in_addr address{};
    if (!host || !id || !port || lwip_inet_pton(AF_INET, host, &address) != 1)
        return false;
    const uint32_t ip = ntohl(address.s_addr);
    if ((ip >> 24) != 10 && (ip >> 16) != 0xc0a8 && (ip >> 20) != 0xac1)
        return false;
    const size_t length = std::strlen(id);
    if (length < 8 || length > 48)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '-'))
            return false;
    auto* protocol = SignalingProtocol::Instance();
    if ((protocol && protocol->IsAudioChannelOpened()) || UrSignalingCall::Instance().Busy() ||
        active.exchange(true))
        return false;
    auto* ctx = new (std::nothrow) ProbeContext;
    if (!ctx) {
        active.store(false);
        return false;
    }
    ctx->host = host;
    ctx->port = port;
    ctx->id = id;
    TaskHandle_t task = nullptr;
    // 真机12KiB栈曾溢出，保留32KiB PSRAM栈；优先级2低于AFE(3)，连续收流先保证收音。
    if (xTaskCreateWithCaps(
            [](void* context) {
                ProbeTask(context);
                vTaskDeleteWithCaps(nullptr);
            },
            "av_probe", 32768, ctx, 2, &task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete ctx;
        active.store(false);
        return false;
    }
    return true;
}

// UrAVProbeActive 不取得音频或网络锁，只提供媒体预约占用事实。
bool UrAVProbeActive() { return active.load(); }
#endif
