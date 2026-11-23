// 文件说明：仅测试构建的C++对讲驱动探针；验证共享状态机到真实浏览器指令，不模拟设备鉴权或完整后端。
#include "browser_ipc_media.hpp"

#include <chrono>
#include <iostream>

namespace ur::device {
// ProbePublish 保存唯一接听报文；此端口是显式测试夹具，不声称为真实MQTT链路。
class ProbePublish final : public PublishPort {
public:
    Envelope accepted;  // 共享核心生成的原token及两条授权轨。
    bool publish(std::string_view, std::string_view body) override {
        return decode_envelope(body, accepted) == DecodeResult::Ok;
    }
};
}  // namespace ur::device

// main 读取单条受限邀请，经过核心受理及测试ACK交接后输出真实工厂生成的配置。
int main(int argc, char** argv) {
    using namespace ur::device;
    std::string body;
    // 纯本地桥解码探针不建立会话/媒体；供真实可执行契约测试核查ipcSync严格shape。
    if (argc == 2 && std::string_view(argv[1]) == "--decode-browser") {
        while (std::getline(std::cin, body)) {
            IPCBrowserRequest request;
            const bool valid = body.size() <= 65536 && decode_ipc_browser(body, request);
            std::cout << (valid ? "accepted\n" : "rejected\n") << std::flush;
        }
        return 0;
    }
    if (argc != 1)
        return 2;
    if (!std::getline(std::cin, body) || body.size() > 65536)
        return 2;
    const auto wall =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    ProbePublish mqtt;
    BrowserIPCMediaFactory factory(true);
    IPCCapability capability;
    capability.microphone = capability.conference_talk = true;
    IPCClient client(mqtt, factory, "up", "down", "media-probe", capability);
    if (!client.receive("down", body, 0, wall) || mqtt.accepted.method != "sessionAccept")
        return 2;
    Envelope ack;
    ack.token = mqtt.accepted.token;
    ack.session = mqtt.accepted.session;
    ack.method = "requestAck";
    ack.request = false;
    ack.code = 200;
    ack.payload = R"({"requestMethod":"sessionAccept","result":{"state":"negotiating"}})";
    std::string response;
    if (encode_envelope(ack, response) != DecodeResult::Ok ||
        !client.receive("down", response, 1, wall))
        return 2;
    const auto command = factory.take_command();
    if (!command || client.phase(ack.session) != IPCPhase::Negotiating)
        return 2;
    std::cout << *command << '\n';
    return 0;
}
