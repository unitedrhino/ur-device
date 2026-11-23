// 文件说明：Linux实际WebAudio门控探针；正式PCM/桥接执行三段输出及连续合成采集，不持设备身份。
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include "browser_bridge.hpp"
#include "browser_pcm.hpp"
#include "browser_wire.hpp"
#include "ur/device/output_focus_driver.hpp"
#include "web_assets.hpp"

namespace {
// asset 只读取固定小型测试页面，路径不来自网络输入。
std::string asset(const char* name) {
    std::ifstream input(std::string(UR_GATE_PROBE_SOURCE) + "/" + name, std::ios::binary);
    std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return !input.bad() && result.size() <= 65536 ? result : std::string{};
}
// token 为本次独占回环桥生成随机凭据，绝不输出到日志。
std::string token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto value : bytes) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}
}  // namespace

// main
// 在同一驱动实例验证有声→静音→恢复，每段20帧且采集必须持续；超时失败不假报释放。
int main(int argc, char** argv) {
    const bool focus_mode =
        argc == 3 && std::string(argv[2]) == "--focus";  // 显式组件模式，不持生产许可。
    if (argc != 2 && !focus_mode)
        return 2;
    unsigned port = 0;
    const std::string raw = argv[1];
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || port < 1024 ||
        port > 65535)
        return 2;
    auto assets = ur::device::browser_assets();
    assets["/index.html"] = asset("audio_gate_probe.html");
    assets["/app.js"] = asset("audio_gate_probe.js");
    if (assets["/index.html"].empty() || assets["/app.js"].empty())
        return 2;
    ur::device::BrowserBridge bridge(port, token(), std::move(assets));
    if (!bridge.start())
        return 3;
    const auto epoch = std::chrono::steady_clock::now();
    const auto now = [&] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - epoch)
                                              .count());
    };
    ur::device::BrowserPcm pcm({48000, 1, 20});
    // 真实端口先完成原false，之后可信组件宿主一次绑定身份；构造不能预授云端租约。
    ur::device::OutputFocusDriver focus({{"", "", {"speaker"}, &pcm}}, 0,
                                        ur::device::FocusBindingMode::DeferredOnce);
    bool scope_bound = false;  // 绑定只发生一次，不因后续暂停/释放复用原路径。
    const std::array<const char*, 4> operations{"acquire", "suspend", "restore", "release"};
    const std::array<const char*, 4> states{"acquired", "suspended", "restored", "released"};
    unsigned focus_completed = 0;  // 仅实际共享驱动原事实推进，初始化不计修订号。
    bool focus_waiting = false;    // 每个操作最多一条在途，旧事实不能完成下一操作。
    // submit_focus 固定本探针原AI输出范围；经正式解析，不向设备或MQTT伪投递许可。
    const auto submit_focus = [&](unsigned index) {
        const std::string body =
            std::string("{\"msgToken\":\"") + operations[index] +
            "\",\"method\":\"focusControl\",\"sessionId\":\"voice\",\"eventSeq\":" +
            std::to_string(index + 1) + " ,\"params\":{\"operation\":\"" + operations[index] +
            "\",\"expectedRevision\":" + std::to_string(index) +
            ",\"focusId\":\"ai-output\",\"resources\":[\"speaker\"],\"priority\":"
            "\"conversation\","
            "\"resumePolicy\":\"explicit\"" +
            ((index == 0 || index == 2) ? ",\"expiresAt\":2000000000000" : "") + "}}";
        ur::device::FocusCommand command;
        focus_waiting = ur::device::decode_focus_control(ur::device::WireRoute::Signaling, body,
                                                         command) == ur::device::WireResult::Ok &&
                        focus.begin(command, index + 1);
        return focus_waiting;
    };
    std::array<unsigned, 3> captures{};  // 各段实际C++接纳的非静音合成输入帧数。
    unsigned phase = 0, played = 0;
    bool ready = false, queued = false;
    std::optional<std::uint64_t> gate;
    const std::vector<std::int16_t> samples(960, 8192);  // 非静音固定PCM，非伪造播放完成。
    std::cout << "{\"kind\":\"probeReady\"}" << std::endl;
    while (now() < 30000) {
        if (!pcm.poll_output_gate(now()))
            return 4;
        while (auto event = bridge.poll()) {
            if (event->kind == ur::device::BrowserEventKind::Connected)
                continue;
            if (event->kind != ur::device::BrowserEventKind::Message)
                return 4;
            ur::device::BrowserRequest request;
            if (!ur::device::decode_browser_request(event->body, request))
                return 4;
            if (request.kind == "ready") {
                if (ready || !pcm.ready(request.format, request.aec) ||
                    !pcm.begin_capture("synthetic-input") || !pcm.begin_playback("voice", 1))
                    return 4;
                ready = true;
                if (focus_mode &&
                    (!bridge.send("{\"kind\":\"focusProbe\"}") || !focus.initialize(now())))
                    return 4;
            } else if (request.kind == "captured") {
                if (!pcm.captured(request.id, request.pcm))
                    return 4;
                std::vector<std::int16_t> input;
                if (pcm.read_frame(input) != ur::device::PcmResult::Frame)
                    return 4;
                std::uint64_t energy = 0;
                for (auto value : input)
                    energy += static_cast<std::int64_t>(value) * value;
                if (energy / input.size() < 128 * 128)
                    return 4;
                if (!focus_mode || focus_completed != 0)
                    ++captures[phase];
            } else if (request.kind == "played") {
                if (!pcm.played(request.epoch, request.ticket))
                    return 4;
                ++played;
                queued = false;
            } else if (request.kind == "outputGated") {
                if (!pcm.output_gated(request.gate_ticket, request.output_enabled, now()))
                    return 4;
            } else
                return 4;
        }
        if (ready && focus_mode) {
            if (!focus.poll(now()))
                return 4;
            if (!scope_bound && focus.initial_idle()) {
                if (!focus.bind_scope(0, "voice", "ai-output", now()))
                    return 4;
                scope_bound = true;
            }
            if (focus_completed == 0 && !focus_waiting && focus.initial_idle() && !submit_focus(0))
                return 4;
            if (auto fact = focus.take_fact("voice", "ai-output")) {
                if (!focus_waiting || !fact->success || focus_completed >= states.size() ||
                    fact->generation != focus_completed + 1 ||
                    fact->revision != focus_completed + 1 ||
                    fact->state != states[focus_completed] ||
                    fact->resources != std::vector<std::string>{"speaker"})
                    return 4;
                ++focus_completed;
                focus_waiting = false;
            }
        }
        if (ready && played == 20 && !queued) {
            if (captures[phase] < 5)
                return 4;
            if (phase == 2) {
                if (focus_mode && focus_completed == 3 && !focus_waiting && !submit_focus(3))
                    return 4;
                if (!focus_mode || focus_completed == 4) {
                    std::cout << "{\"kind\":\"gateComplete\",\"frames\":[20,20,20],"
                                 "\"captureFrames\":["
                              << captures[0] << ',' << captures[1] << ',' << captures[2] << "]"
                              << (focus_mode ? ",\"focusStates\":[\"acquired\",\"suspended\","
                                               "\"restored\",\"released\"],\"focusRevision\":4"
                                             : "")
                              << "}" << std::endl;
                    bridge.stop();
                    return 0;
                }  // 释放在途继续泵原false票据及采集，不以playbackDone代替释放。
            } else {
                ++phase;
                played = 0;
                if (focus_mode) {
                    if (!submit_focus(phase))
                        return 4;
                } else {
                    gate = pcm.request_output_gate(phase == 2, now());
                    if (!gate)
                        return 4;
                }
            }
        }
        const bool confirmed = focus_mode ? !focus_waiting && focus_completed == phase + 1
                                          : !gate || pcm.output_gate_confirmed(*gate, phase == 2);
        if (ready && !queued && confirmed) {
            if (!pcm.write_frame(samples, 1))
                return 4;
            queued = true;
        }
        while (auto command = pcm.take_command())
            if (!bridge.send(ur::device::browser_command(*command)))
                return 4;
        while (auto frame = pcm.take_playback())
            if (!bridge.send(ur::device::browser_frame(*frame)))
                return 4;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 5;
}
