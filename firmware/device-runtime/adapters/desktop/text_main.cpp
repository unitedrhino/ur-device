// 文件说明：S21无媒体C++桌面入口；stdin文字及显式/image、/file附件，/cancel取消、/quit结束；原件worker不阻塞协议，默认不记录正文/路径/凭据。
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
#include "device_credentials.hpp"
#include "environment.hpp"
#include "mosquitto_port.hpp"
#include "ur/device/voice_runtime.hpp"
#ifdef UR_DESKTOP_CONTENT_ENABLED
#include "content_input_worker.hpp"
#include "curl_http_port.hpp"
#include <filesystem>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ur::device {
namespace {
// InputLine区分暂时无输入和真实EOF；不能阻塞MQTT或把半行当一轮提交。
struct InputLine {
    enum class Kind { Pending, Line, Eof, Failed };
    Kind kind = Kind::Pending;  // 当前本地stdin事实。
    std::string text;           // 只在明确Line时交协议，禁止写诊断日志。
};
// ConsoleInput保持有界部分行，Windows/Linux依赖仅属于桌面层；不构造任何媒体驱动。
class ConsoleInput {
public:
    // next先交付完整本地行，再非阻塞读取；超过60000字节保守拒绝，不截断用户意图。
    InputLine next() {
        const auto newline = pending_.find('\n');
        if (newline != std::string::npos) {
            auto line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return {InputLine::Kind::Line, std::move(line)};
        }
        if (eof_) {
            if (pending_.empty())
                return {InputLine::Kind::Eof, {}};
            auto line = std::move(pending_);
            pending_.clear();
            return {InputLine::Kind::Line, std::move(line)};
        }
        std::array<char, 1024> bytes{};  // 有界控制台读取块，不代表协议分帧。
        std::size_t count = 0;
#ifdef _WIN32
        const auto input = GetStdHandle(STD_INPUT_HANDLE);
        const auto type = GetFileType(input);
        if (type == FILE_TYPE_CHAR) {
            if (!_kbhit())
                return {};
            const auto ch = _getwch();
            if (ch == 0 || ch == 224) {
                (void)_getwch();
                return {};
            }
            if (ch == 26)
                eof_ = true;
            else if (ch == L'\r')
                pending_ += '\n';
            else if (ch == L'\b') {
                if (surrogate_)
                    surrogate_ = 0;
                else if (!pending_.empty()) {
                    auto last = pending_.size() - 1;
                    while (last && (static_cast<unsigned char>(pending_[last]) & 0xc0) == 0x80)
                        --last;
                    pending_.erase(last);
                }
            } else {
                if (ch >= 0xd800 && ch <= 0xdbff) {
                    if (surrogate_)
                        return {InputLine::Kind::Failed, {}};
                    surrogate_ = static_cast<wchar_t>(ch);
                    return {};
                }
                const wchar_t chars[] = {surrogate_ ? surrogate_ : static_cast<wchar_t>(ch),
                                         static_cast<wchar_t>(ch)};
                const auto length = surrogate_ ? 2 : 1;
                const auto size =
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, bytes.data(),
                                        static_cast<int>(bytes.size()), nullptr, nullptr);
                if (size <= 0)
                    return {InputLine::Kind::Failed, {}};
                surrogate_ = 0;
                count = static_cast<std::size_t>(size);
            }
        } else {
            DWORD available = static_cast<DWORD>(bytes.size());
            if (type == FILE_TYPE_PIPE &&
                !PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                    eof_ = true;
                else
                    return {InputLine::Kind::Failed, {}};
            }
            if (!eof_ && available) {
                DWORD read = 0;
                if (!ReadFile(input, bytes.data(),
                              std::min(available, static_cast<DWORD>(bytes.size())), &read,
                              nullptr)) {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                        return {InputLine::Kind::Failed, {}};
                    eof_ = true;
                } else if (read == 0)
                    eof_ = true;
                count = read;
            }
        }
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};  // 只探测stdin，不等待网络或用户下一字符。
        const auto ready = ::poll(&input, 1, 0);
        if (ready < 0)
            return errno == EINTR ? InputLine{} : InputLine{InputLine::Kind::Failed, {}};
        if (ready == 0)
            return {};
        if (input.revents & (POLLERR | POLLNVAL))
            return {InputLine::Kind::Failed, {}};
        const auto read = ::read(STDIN_FILENO, bytes.data(), bytes.size());
        if (read < 0)
            return errno == EINTR || errno == EAGAIN ? InputLine{}
                                                     : InputLine{InputLine::Kind::Failed, {}};
        if (read == 0)
            eof_ = true;
        else
            count = static_cast<std::size_t>(read);
#endif
        pending_.append(bytes.data(), count);
        if (pending_.size() > 60000)
            return {InputLine::Kind::Failed, {}};
        return {};
    }

private:
    std::string pending_;  // 最多60000字节部分行，完整行只提交一次。
    bool eof_ = false;     // 来自实际文件/管道事实，不因暂时无字符而推测。
#ifdef _WIN32
    wchar_t surrogate_ = 0;  // 非BMP控制台输入等待原低代理项，不输出残缺UTF8。
#endif
};
// milliseconds只计本进程单调超时，不用于设备签名的Unix秒。
std::uint64_t milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
// configure复用既有签名函数，身份凭据只从环境进入原MQTT适配器；不输出地址、密钥或签名。
// 保留既有调用签名兼容原宿主；设备身份与随机实例输出分别保留原用途。
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool configure(MqttConfig& config, std::string& identity, std::string& instance) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : random) {
        instance += hex[byte >> 4];
        instance += hex[byte & 15];
    }
    const auto product = environment_value("UR_DEVICE_PRODUCT").value_or("");
    const auto device = environment_value("UR_DEVICE_NAME").value_or("");
    const auto secret = environment_value("UR_DEVICE_SECRET").value_or("");
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        3600;
    if (expiry <= 0)
        return false;
    const auto credentials =
        device_credentials(product, device, secret, instance, static_cast<std::uint64_t>(expiry));
    const auto port = environment_value("UR_MQTT_PORT").value_or("");
    unsigned value = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
    config.host = environment_value("UR_MQTT_HOST").value_or("");
    config.ca_file = environment_value("UR_MQTT_CA").value_or("");
    if (!credentials || config.host.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port.data() + port.size() || value == 0 || value > 65535)
        return false;
    identity = product + "/" + device;
    config.port = static_cast<std::uint16_t>(value);
    config.client_id = credentials->client_id;
    config.username = credentials->username;
    config.password = credentials->password;
    return true;
}
// display仅--tui界面显示文字，过滤终端控制字符；默认诊断路径完全不接正文。
void display(std::string_view text) {
    for (const auto ch : text)
        std::cerr << ((static_cast<unsigned char>(ch) < 32 && ch != '\n' && ch != '\t') || ch == 127
                          ? '?'
                          : ch);
    std::cerr.flush();
}
// run以唯一事件线程处理stdin/MQTT；应用回复等待上限120秒，不改变协议请求30秒或自动重发用户意图。
int run(bool tui) {
    MqttConfig config;
    std::string identity, instance;
    if (!configure(config, identity, instance)) {
        std::cout << "{\"error\":\"configurationInvalid\"}\n";
        return 2;
    }
    MosquittoPort mqtt;
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
#ifdef UR_DESKTOP_CONTENT_ENABLED
    // HTTPS origin仅从可信启动环境注入；空CA使用Curl系统信任，不关闭证书/主机名校验。
    ContentInputConfig upload_config{environment_value("UR_DEVICE_CONTENT_ORIGIN").value_or(""),
                                     content_input_basic(config.username, config.password)};
    const auto upload_ca = environment_value("UR_DEVICE_CONTENT_CA").value_or("");
    std::error_code ca_error;
    const bool ca_configured =
        upload_ca.empty() ||
        std::filesystem::is_regular_file(std::filesystem::u8path(upload_ca), ca_error);
    const bool attachments = ca_configured && content_input_configured(upload_config);
    capability.image_input = capability.file_input = attachments;
    CurlHttpPort upload_http(upload_ca);
    ContentInputWorker uploads(upload_http, std::move(upload_config));
    std::string upload_session;  // 仅事件线程保存原任务session，失效时撤销atomic许可。
#endif
    VoiceRuntime runtime(mqtt, identity, capability, instance);
    auto now = milliseconds();
    if (runtime.start(config, now) != Result::Applied)
        return 1;
    ConsoleInput console;
    std::deque<std::string> inputs;
    bool eof = false, quitting = false, ending = false, active = false, announced = false;
    unsigned submitted = 0, completed = 0, cancelled = 0;  // 仅本应用计数，不输出动态身份。
    bool cancel_pending = false, displayed_delta = false;  // 等原终态/取消ACK；正文展示不影响协议。
    std::uint64_t reply_at = 0, end_at = 0;  // 应用有界等待基点，不参与模型或媒体延迟指标。
    while (true) {
        now = milliseconds();
        runtime.poll(now);
#ifdef UR_DESKTOP_CONTENT_ENABLED
        // 原连接/会话失效只撤销任务，不让worker读取协议对象，也不能恢复旧current许可。
        if (uploads.busy() && (ending || quitting || runtime.state() != RuntimeState::Running ||
                               runtime.voice().state() != VoiceState::Established ||
                               runtime.voice().session_id() != upload_session))
            uploads.cancel();
#endif
        if (runtime.state() == RuntimeState::Failed) {
            std::cout << "{\"error\":\"sessionFailed\"}\n";
            return 1;
        }
        if (!eof && inputs.size() < 8) {
            auto line = console.next();
            if (line.kind == InputLine::Kind::Failed) {
                runtime.close(now);
                return 1;
            }
            if (line.kind == InputLine::Kind::Eof)
                eof = true;
            if (line.kind == InputLine::Kind::Line) {
                if (line.text == "/quit") {
                    quitting = eof = true;
                    inputs.clear();
#ifdef UR_DESKTOP_CONTENT_ENABLED
                    uploads.cancel();
#endif
                } else
                    inputs.push_back(std::move(line.text));
            }
        }
        while (auto result = runtime.take_text_result()) {
            if (result->result != "accepted") {
                runtime.close(now);
                std::cout << "{\"error\":\"inputRejected\"}\n";
                return 1;
            }
            std::cout << "{\"inputAccepted\":true,\"round\":" << submitted << "}\n";
        }
        while (auto content = runtime.take_content()) {
            if (tui && content->envelope.method == "respTextDelta") {
                display(content->text);
                displayed_delta = true;
            }
            if (tui && content->envelope.method == "respTextDone" && !displayed_delta)
                display(content->text);
            if (content->envelope.method == "respDone") {
                active = false;
                if (cancel_pending && content->status == "cancelled") {
                    cancel_pending = false;
                    ++cancelled;
                    std::cout << "{\"replyCancelled\":true}\n";
                    continue;
                }
                if (content->status != "completed") {
                    runtime.close(now);
                    std::cout << "{\"error\":\"replyNotCompleted\"}\n";
                    return 1;
                }
                cancel_pending = false;
                ++completed;
                if (tui)
                    display("\n");
                std::cout << "{\"replyCompleted\":true,\"round\":" << completed << "}\n";
            }
        }
        if (runtime.voice().state() == VoiceState::Established && !ending) {
            if (cancel_pending && !runtime.voice().text_input_pending()) {
                cancel_pending = active = false;
                ++cancelled;
                std::cout << "{\"cancelAcknowledged\":true}\n";
            }
            // 当前输入被核心明确终止且尚无回复终态时及时失败；不把迟到错误或取消应答伪造为respDone。
            if (active && !cancel_pending && !runtime.voice().text_input_pending()) {
                runtime.close(now);
                std::cout << "{\"error\":\"responseFailed\"}\n";
                return 1;
            }
            if (!announced) {
                std::cout << "{\"sessionEstablished\":true,\"transport\":\"none\"}\n";
                announced = true;
            }
#ifdef UR_DESKTOP_CONTENT_ENABLED
            // 已排到队首的取消优先于Ready提交，不能在同次poll内先发送已撤销意图。
            if (uploads.busy() && !inputs.empty() && inputs.front() == "/cancel") {
                uploads.cancel();
                inputs.pop_front();
                std::cout << "{\"attachmentCancelRequested\":true}\n";
            }
            // 结果只在原事件线程提交；上传Ready不是平台输入受理，更不是模型已消费。
            if (auto uploaded = uploads.take()) {
                if (!quitting &&
                    content_input_current_result(*uploaded,
                                                 {runtime.voice().session_id(), instance},
                                                 runtime.supports_content(uploaded->part.kind)) &&
                    runtime.send_content({uploaded->part}, false, now) == Result::Applied) {
                    active = true;
                    displayed_delta = false;
                    reply_at = now;
                    ++submitted;
                } else
                    std::cout << "{\"attachmentRejected\":true}\n";
            }
#endif
            if (!inputs.empty() && inputs.front() == "/cancel") {
#ifdef UR_DESKTOP_CONTENT_ENABLED
                if (uploads.busy()) {
                    uploads.cancel();
                    inputs.pop_front();
                    std::cout << "{\"attachmentCancelRequested\":true}\n";
                } else {
#endif
                    const auto canceled = runtime.cancel_response(now);
                    inputs.pop_front();
                    if (canceled == Result::Applied) {
                        cancel_pending = true;
                        std::cout << "{\"cancelRequested\":true}\n";
                    }
#ifdef UR_DESKTOP_CONTENT_ENABLED
                }
#endif
            } else if (!active && !inputs.empty()) {
#ifdef UR_DESKTOP_CONTENT_ENABLED
                const auto command = parse_content_input(inputs.front());
                if (uploads.busy()) {
                    // 不弹出排队意图，单任务完成/取消后才受理下一项。
                } else if (command.attachment) {
                    if (!attachments || !runtime.supports_content(command.kind) ||
                        !uploads.start(command, {runtime.voice().session_id(), instance}))
                        std::cout << "{\"attachmentRejected\":true}\n";
                    else
                        upload_session = runtime.voice().session_id();
                    inputs.pop_front();
                } else {
#else
                // 未编入HTTPS适配器的产物不得把附件命令悄悄转换成模型文字。
                const auto& line = inputs.front();
                if (line == "/image" || line == "/file" || line.compare(0, 7, "/image ") == 0 ||
                    line.compare(0, 6, "/file ") == 0) {
                    inputs.pop_front();
                    std::cout << "{\"attachmentRejected\":true}\n";
                } else {
#endif
                    const auto sent = runtime.send_text(inputs.front(), false, now);
                    if (sent == Result::Applied) {
                        inputs.pop_front();
                        active = true;
                        displayed_delta = false;
                        reply_at = now;
                        ++submitted;
                    } else if (sent != Result::Busy) {
                        runtime.close(now);
                        return 1;
                    }
                }
            }
            if (quitting || (eof && inputs.empty() && !active
#ifdef UR_DESKTOP_CONTENT_ENABLED
                             && !uploads.busy()
#endif
                                 )) {
                if (runtime.end(now) != Result::Applied) {
                    runtime.close(now);
                    return 1;
                }
                ending = true;
                end_at = now;
            }
        }
        if ((active && now - reply_at >= 120000) || (ending && now - end_at >= 30000)) {
            runtime.close(now);
            std::cout << "{\"error\":\"responseTimeout\"}\n";
            return 1;
        }
        if (ending && runtime.state() == RuntimeState::Idle) {
            std::cout << "{\"sessionEnded\":true,\"completedRounds\":" << completed << "}\n";
            return quitting || completed + cancelled == submitted ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace
}  // namespace ur::device
// main默认只输出固定计数诊断；--tui是显式界面开关，不写对话日志或创建音视频资源。
int main(int argc, char** argv) {
    // 管道驱动也须在进程仍在线时收到每个真实事件，不能等退出冲刷缓冲再宣称已建立。
    std::cout << std::unitbuf;
    const bool tui = argc == 2 && std::string_view(argv[1]) == "--tui";
    if (argc != 1 && !tui)
        return 2;
    return ur::device::run(tui);
}
