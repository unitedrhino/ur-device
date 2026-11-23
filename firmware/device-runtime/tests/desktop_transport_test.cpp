// 原生桌面集成测试：真实 Broker、回环 UDP、真实 AES/Opus；不冒充平台模型验收。
#include <cJSON.h>
#include <gtest/gtest.h>
#include <mosquitto.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <thread>
#include "environment.hpp"
#include "mosquitto_port.hpp"
#include "openssl_ctr.hpp"
#include "opus_codec.hpp"
#include "udp_socket.hpp"
#include "ur/device/voice_runtime.hpp"

namespace ur::device {
namespace {
// wait_until 只用于有真实 IO 的集成测试，最多等3秒，失败不会无限重试。
bool wait_until(const std::function<bool()>& check) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (check())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
// event 等待一个传输层事件，不将网络确认当作业务应答。
bool event(MosquittoPort& client, MqttEvent::Kind kind) {
    return wait_until([&] {
        client.poll();
        while (auto next = client.take_event())
            if (next->kind == kind)
                return true;
        return false;
    });
}
TEST(DesktopMqtt, RejectsUnsafeOrMalformedConfiguration) {
    MosquittoPort client;
    EXPECT_FALSE(client.poll());
    EXPECT_FALSE(client.publish("topic", "{}"));
    EXPECT_EQ(client.subscribe("topic"), 0);
    MqttConfig config;
    EXPECT_FALSE(client.connect(config));
    config.host = "broker.invalid";
    config.client_id = "offline-test";
    EXPECT_FALSE(client.connect(config));
    config.host = std::string("127.0.0.1\0bad", 13);
    EXPECT_FALSE(client.connect(config));
    client.disconnect();
    client.disconnect();
}
// 实际网络回调独立于设备poll/take推进，分类计数不保存Topic或报文；PUBACK不是接收判据。
TEST(DesktopMqtt, ThreadProgressCountsActualCallbacksWithoutHostPolling) {
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    ASSERT_TRUE(port_env);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(std::strtol(port_env->c_str(), nullptr, 10));
    config.client_id = "progress-device-test";
    auto progress = std::make_shared<ThreadProgress>();
    MosquittoPort device(progress), platform;
    ASSERT_TRUE(device.connect(config));
    config.client_id = "progress-platform-test";
    ASSERT_TRUE(platform.connect(config));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Connected));
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Connected));
    const std::array<std::string, 3> topics = {"$thing/down/message/test/progress",
                                               "$thing/down/signaling/test/progress",
                                               "$thing/down/property/test/progress"};
    for (const auto& topic : topics) {
        ASSERT_GT(device.subscribe(topic), 0);
        ASSERT_TRUE(event(device, MqttEvent::Kind::Subscribed));
    }
    const auto host_takes = progress->take_entered.value();
    for (const auto& topic : topics)
        ASSERT_TRUE(platform.publish(topic, "{}"));
    ASSERT_TRUE(wait_until([&] { return progress->callbacks_exited.value() == 3; }));
    EXPECT_EQ(progress->callbacks_entered.value(), 3U);
    EXPECT_EQ(progress->callbacks_locked.value(), 3U);
    EXPECT_EQ(progress->take_entered.value(), host_takes);
    EXPECT_EQ(progress->rejected.value(), 0U);
    for (const auto& route : progress->routes) {
        EXPECT_EQ(route.valid.value(), 1U);
        EXPECT_EQ(route.enqueued.value(), 1U);
        EXPECT_EQ(route.taken.value(), 0U);
    }
    for (const auto& topic : topics) {
        const auto message = device.take_event();
        ASSERT_TRUE(message);
        EXPECT_EQ(message->kind, MqttEvent::Kind::Message);
        EXPECT_EQ(message->topic, topic);
        EXPECT_EQ(message->body, "{}");
    }
    EXPECT_FALSE(device.take_event());
    for (const auto& route : progress->routes)
        EXPECT_EQ(route.taken.value(), 1U);
    device.disconnect();
    platform.disconnect();
}
// 真实Broker转发空载荷进入原拒绝分支；观察不会把Failed改成业务消息或保存载荷。
TEST(DesktopMqtt, ThreadProgressRejectedActualPayloadKeepsFailure) {
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    ASSERT_TRUE(port_env);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(std::strtol(port_env->c_str(), nullptr, 10));
    config.client_id = "progress-reject-device";
    auto progress = std::make_shared<ThreadProgress>();
    MosquittoPort device(progress);
    ASSERT_TRUE(device.connect(config));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Connected));
    const char* topic = "$thing/down/message/test/progress-reject";
    ASSERT_GT(device.subscribe(topic), 0);
    ASSERT_TRUE(event(device, MqttEvent::Kind::Subscribed));
    // 使用原库实际网络发送合法MQTT空载荷，不能经适配器publish提前拒绝来伪造回调。
    std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)> publisher(
        mosquitto_new("progress-empty-publisher", true, nullptr), mosquitto_destroy);
    ASSERT_TRUE(publisher);
    ASSERT_EQ(mosquitto_connect(publisher.get(), "127.0.0.1", config.port, 30), MOSQ_ERR_SUCCESS);
    ASSERT_EQ(mosquitto_publish(publisher.get(), nullptr, topic, 0, nullptr, 1, false),
              MOSQ_ERR_SUCCESS);
    ASSERT_TRUE(wait_until([&] {
        (void)mosquitto_loop(publisher.get(), 1, 1);
        return progress->callbacks_exited.value() == 1;
    }));
    EXPECT_EQ(progress->callbacks_entered.value(), 1U);
    EXPECT_EQ(progress->rejected.value(), 1U);
    EXPECT_EQ(progress->routes[0].valid.value(), 0U);
    EXPECT_EQ(progress->routes[0].enqueued.value(), 0U);
    const auto failure = device.take_event();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->kind, MqttEvent::Kind::Failed);
    EXPECT_FALSE(device.poll());
    EXPECT_EQ(progress->routes[0].taken.value(), 0U);
    mosquitto_disconnect(publisher.get());
    device.disconnect();
}
// 原生产队列128条后仍按原失败语义退出，诊断只数Message失败，不混用宿主Failed入队槽。
TEST(DesktopMqtt, ThreadProgressActualQueueOverflowDoesNotChangeBusiness) {
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    ASSERT_TRUE(port_env);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(std::strtol(port_env->c_str(), nullptr, 10));
    config.client_id = "progress-overflow-device";
    auto progress = std::make_shared<ThreadProgress>();
    MosquittoPort device(progress), platform;
    ASSERT_TRUE(device.connect(config));
    config.client_id = "progress-overflow-platform";
    ASSERT_TRUE(platform.connect(config));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Connected));
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Connected));
    const auto topic = "$thing/down/message/test/progress-overflow";
    ASSERT_GT(device.subscribe(topic), 0);
    ASSERT_TRUE(event(device, MqttEvent::Kind::Subscribed));
    for (unsigned index = 0; index < 129; ++index) {
        ASSERT_TRUE(platform.publish(topic, "{}"));
        ASSERT_TRUE(event(platform, MqttEvent::Kind::Published));
    }
    ASSERT_TRUE(wait_until([&] { return progress->callbacks_exited.value() == 129; }));
    EXPECT_EQ(progress->routes[0].valid.value(), 129U);
    EXPECT_EQ(progress->routes[0].enqueued.value(), 128U);
    EXPECT_EQ(progress->enqueue_overflow.value(), 1U);
    const auto failure = device.take_event();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->kind, MqttEvent::Kind::Failed);
    EXPECT_FALSE(device.poll());
    EXPECT_EQ(progress->routes[0].taken.value(), 0U);
    device.disconnect();
    platform.disconnect();
}
TEST(DesktopMqtt, RealBrokerQosOneBidirectionalAndReconnect) {
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    if (!port_env)
        GTEST_SKIP() << "需要隔离 Broker 测试入口";
    const auto port = std::strtol(port_env->c_str(), nullptr, 10);
    ASSERT_GT(port, 0);
    ASSERT_LT(port, 65536);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(port);
    config.client_id = "runtime-device-test";
    MosquittoPort device;
    MosquittoPort platform;
    ASSERT_TRUE(device.connect(config));
    config.client_id = "runtime-platform-test";
    ASSERT_TRUE(platform.connect(config));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Connected));
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Connected));
    const std::string up = "$thing/up/signaling/test/device";
    const std::string down = "$thing/down/signaling/test/device";
    ASSERT_GT(device.subscribe(down), 0);
    ASSERT_GT(platform.subscribe(up), 0);
    ASSERT_TRUE(event(device, MqttEvent::Kind::Subscribed));
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Subscribed));
    EXPECT_EQ(device.subscribe("$thing/down/#"), 0);
    EXPECT_FALSE(device.publish(up, ""));
    EXPECT_FALSE(device.publish(up, std::string(65537, 'x')));
    const std::string request = R"({"msgToken":"t","method":"capabilityReport","params":{}})";
    const std::string response =
        R"({"msgToken":"t","method":"requestAck","code":422,"data":{"reason":"unsupportedCapability"}})";
    ASSERT_TRUE(device.publish(up, request));
    bool received = false;
    ASSERT_TRUE(wait_until([&] {
        device.poll();
        platform.poll();
        while (auto next = platform.take_event()) {
            if (next->kind != MqttEvent::Kind::Message)
                continue;
            EXPECT_EQ(next->topic, up);
            EXPECT_EQ(next->body, request);
            received = true;
        }
        return received;
    }));
    ASSERT_TRUE(platform.publish(down, response));
    received = false;
    ASSERT_TRUE(wait_until([&] {
        device.poll();
        platform.poll();
        while (auto next = device.take_event()) {
            if (next->kind != MqttEvent::Kind::Message)
                continue;
            EXPECT_EQ(next->topic, down);
            EXPECT_EQ(next->body, response);
            received = true;
        }
        return received;
    }));
    device.disconnect();
    EXPECT_FALSE(device.publish(up, request));
    config.client_id = "runtime-device-test";
    ASSERT_TRUE(device.connect(config));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Connected));
    ASSERT_GT(device.subscribe(down), 0);
    ASSERT_TRUE(event(device, MqttEvent::Kind::Subscribed));
    // PUBACK 尚未被事件线程消费时，不能无限向 libmosquitto 队列追加 QoS1 消息。
    for (int index = 0; index < 32; ++index)
        ASSERT_TRUE(device.publish(up, request));
    EXPECT_FALSE(device.publish(up, request));
    ASSERT_TRUE(event(device, MqttEvent::Kind::Published));
    EXPECT_TRUE(device.publish(up, request));
    device.disconnect();
    platform.disconnect();
}
// 原Broker真实挤掉旧连接后，网络线程先自然退出；销毁仍须join，不能由库threaded标志推断已回收。
TEST(DesktopMqtt, NaturallyExitedNetworkThreadIsJoinedBeforeDestroyAndReconnect) {
#if defined(__linux__)
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    ASSERT_TRUE(port_env);
    // 只读取本进程实际线程数量，确定旧网络线程已退出；不睡眠猜测、不扫描其它进程或修改驱动。
    const auto thread_count = []() -> std::optional<std::size_t> {
        std::error_code error;
        std::filesystem::directory_iterator next("/proc/self/task", error), end;
        if (error)
            return {};
        std::size_t count = 0;
        for (; next != end; next.increment(error)) {
            if (error)
                return {};
            ++count;
        }
        return error ? std::nullopt : std::optional<std::size_t>(count);
    };
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(std::strtol(port_env->c_str(), nullptr, 10));
    config.client_id = "natural-exit-same-client";
    MosquittoPort victim,
        replacement;  // 同ID的新原连接由Broker挤掉旧连接，不本地伪造Disconnected。
    ASSERT_TRUE(victim.connect(config));
    ASSERT_TRUE(event(victim, MqttEvent::Kind::Connected));
    // 第一真实网络线程启动后再取基线，包含TSan自身可能懒创建的辅助线程，不猜总数。
    const auto first_connected_threads = thread_count();
    ASSERT_TRUE(first_connected_threads);
    ASSERT_GT(*first_connected_threads, 0U);
    ASSERT_TRUE(replacement.connect(config));
    ASSERT_TRUE(event(replacement, MqttEvent::Kind::Connected));
    ASSERT_TRUE(event(victim, MqttEvent::Kind::Disconnected));
    ASSERT_TRUE(wait_until([&] {
        const auto current = thread_count();
        return current && *current == *first_connected_threads;
    }));
    EXPECT_FALSE(victim.poll());
    EXPECT_FALSE(victim.publish("$thing/up/signaling/test/natural-exit", "{}"));
    // TSan在原未join实现中能确定看到已退出线程的mutex访问与销毁缺少同步，而非碰重连时序运气。
    victim.disconnect();
    victim.disconnect();
    EXPECT_TRUE(replacement.poll());
    config.client_id = "natural-exit-reconnected";
    ASSERT_TRUE(victim.connect(config));
    ASSERT_TRUE(event(victim, MqttEvent::Kind::Connected));
    ASSERT_TRUE(victim.publish("$thing/up/signaling/test/natural-exit", "{}"));
    ASSERT_TRUE(event(victim, MqttEvent::Kind::Published));
    victim.disconnect();
    replacement.disconnect();
    ASSERT_TRUE(wait_until([&] {
        const auto current = thread_count();
        return current && *current == *first_connected_threads - 1;
    }));
#else
    GTEST_SKIP() << "原网络线程自然退出取证仅适用于Linux本进程线程目录";
#endif
}
// 真实Broker只代替网络，不伪装真实模型；纯文字运行时没有音频对象或媒体ready。
TEST(DesktopMqtt, TextOnlyRuntimeRealBrokerMultiTurnCancelEndAndReconnect) {
    const auto port_env = environment_value("UR_TEST_MQTT_PORT");
    if (!port_env)
        GTEST_SKIP() << "需要隔离Broker测试入口";
    const auto port = std::strtol(port_env->c_str(), nullptr, 10);
    ASSERT_GT(port, 0);
    ASSERT_LT(port, 65536);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(port);
    config.client_id = "runtime-text-platform";
    MosquittoPort platform, device;
    ASSERT_TRUE(platform.connect(config));
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Connected));
    ASSERT_GT(platform.subscribe("$thing/up/signaling/test/text"), 0);
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Subscribed));
    ASSERT_GT(platform.subscribe("$thing/up/message/test/text"), 0);
    ASSERT_TRUE(event(platform, MqttEvent::Kind::Subscribed));
    VoiceCapability capability;
    capability.transport = VoiceTransport::None;
    capability.text_input = true;
    VoiceRuntime runtime(device, "test/text", capability, "real-text-test");
    const auto start = std::chrono::steady_clock::now();
    // now只驱动现有协议时钟，网络等待最多原3秒。
    const auto now = [&] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
    };
    std::deque<Envelope> requests;
    // pump消费真实QoS1事件；真实生产解析器生成请求，测试只保留公开关联。
    const auto pump = [&] {
        runtime.poll(now());
        platform.poll();
        while (auto next = platform.take_event()) {
            if (next->kind != MqttEvent::Kind::Message)
                continue;
            Envelope request;
            EXPECT_EQ(decode_envelope(next->body, request), DecodeResult::Ok);
            EXPECT_EQ(next->topic, request.method == "inputSend" ? "$thing/up/message/test/text"
                                                                 : "$thing/up/signaling/test/text");
            requests.push_back(std::move(request));
        }
        return !requests.empty();
    };
    // publish只生成下行协议；不调用模型、不替代端云真实验收。
    const auto publish = [&](const Envelope& response, bool message) {
        std::string body;
        EXPECT_EQ(encode_envelope(response, body), DecodeResult::Ok);
        return platform.publish(
            message ? "$thing/down/message/test/text" : "$thing/down/signaling/test/text", body);
    };
    // ack保持实际上行token/session，交真实网络ACK后仍需独立Established。
    const auto ack = [&](const Envelope& request, std::string result) {
        Envelope response;
        response.token = request.token;
        response.session = request.session;
        response.method = "requestAck";
        response.request = false;
        response.code = 200;
        response.payload =
            "{\"requestMethod\":\"" + request.method + "\",\"result\":" + result + "}";
        return publish(response, request.method == "inputSend");
    };
    config.client_id = "runtime-text-device";
    for (unsigned connection = 0; connection < 2; ++connection) {
        ASSERT_EQ(runtime.start(config, now()), Result::Applied);
        ASSERT_TRUE(wait_until(pump));
        const auto report = requests.front();
        requests.pop_front();
        ASSERT_EQ(report.method, "capabilityReport");
        ASSERT_TRUE(ack(report, R"({"acceptedRevision":1})"));
        ASSERT_TRUE(wait_until(pump));
        const auto create = requests.front();
        requests.pop_front();
        ASSERT_EQ(create.method, "sessionCreate");
        Envelope response;
        response.token = create.token;
        response.method = "sessionCreated";
        response.session = "real-text-session-" + std::to_string(connection);
        response.request = false;
        response.code = 200;
        response.payload =
            R"({"state":"negotiating","selected":{"transport":"none","inputModalities":["text"],"outputModalities":["text"]},"tracks":[]})";
        ASSERT_TRUE(publish(response, false));
        ASSERT_TRUE(wait_until([&] {
            pump();
            return runtime.voice().state() == VoiceState::Negotiating;
        }));
        response.token = "established";
        response.method = "sessionEstablished";
        response.code.reset();
        response.event_sequence = 1;
        response.payload = R"({"state":"established","mediaRevision":0})";
        ASSERT_TRUE(publish(response, false));
        ASSERT_TRUE(wait_until([&] {
            pump();
            return runtime.voice().state() == VoiceState::Established;
        }));
        EXPECT_TRUE(requests.empty());
        EXPECT_FALSE(runtime.voice().capturing());
        EXPECT_FALSE(runtime.voice().playing());
        for (unsigned round = 0; round < 2; ++round) {
            ASSERT_EQ(runtime.send_text("公开网络文字", false, now()), Result::Applied);
            ASSERT_TRUE(wait_until(pump));
            const auto input = requests.front();
            requests.pop_front();
            ASSERT_EQ(input.method, "inputSend");
            auto* params = cJSON_Parse(input.payload.c_str());
            const auto* field = cJSON_GetObjectItemCaseSensitive(params, "inputId");
            const std::string input_id = cJSON_IsString(field) ? field->valuestring : "";
            cJSON_Delete(params);
            ASSERT_FALSE(input_id.empty());
            ASSERT_TRUE(ack(input, "{\"inputId\":\"" + input_id + "\",\"state\":\"accepted\"}"));
            const auto reply = "reply-" + std::to_string(round);
            response.token = "created";
            response.method = "respCreated";
            response.event_sequence.reset();
            response.message_sequence = round * 3 + 1;
            response.payload = "{\"respId\":\"" + reply + "\",\"inputId\":\"" + input_id + "\"}";
            ASSERT_TRUE(publish(response, true));
            bool bound = false;
            ASSERT_TRUE(wait_until([&] {
                pump();
                while (auto content = runtime.take_content())
                    bound |=
                        content->envelope.method == "respCreated" && content->response_id == reply;
                return bound;
            }));
            ASSERT_EQ(runtime.send_text("不得提前下一轮", false, now()), Result::Busy);
            if (round == 0) {
                response.token = "delta";
                response.method = "respTextDelta";
                response.message_sequence = 2;
                response.payload =
                    "{\"respId\":\"" + reply + "\",\"contentIndex\":0,\"delta\":\"真实网络增量\"}";
                ASSERT_TRUE(publish(response, true));
                response.token = "done";
                response.method = "respDone";
                response.message_sequence = 3;
                response.payload =
                    "{\"respId\":\"" + reply + "\",\"inputId\":\"" + input_id +
                    "\",\"status\":\"completed\",\"contentCount\":1,\"audioState\":\"none\"}";
                ASSERT_TRUE(publish(response, true));
                ASSERT_TRUE(wait_until([&] {
                    pump();
                    return !runtime.voice().text_input_pending();
                }));
                bool actual_delta = false, actual_done = false;
                while (auto content = runtime.take_content()) {
                    actual_delta |= content->text == "真实网络增量";
                    actual_done |= content->envelope.method == "respDone";
                }
                EXPECT_TRUE(actual_delta);
                EXPECT_TRUE(actual_done);
            } else {
                ASSERT_EQ(runtime.cancel_response(now()), Result::Applied);
                ASSERT_TRUE(wait_until(pump));
                const auto cancel = requests.front();
                requests.pop_front();
                ASSERT_EQ(cancel.method, "respCancel");
                ASSERT_TRUE(ack(cancel, "{\"respId\":\"" + reply + "\",\"state\":\"cancelled\"}"));
                ASSERT_TRUE(wait_until([&] {
                    pump();
                    return !runtime.voice().text_input_pending();
                }));
            }
            ASSERT_TRUE(runtime.take_text_result());
        }
        ASSERT_EQ(runtime.end(now()), Result::Applied);
        ASSERT_TRUE(wait_until(pump));
        const auto end = requests.front();
        requests.pop_front();
        ASSERT_EQ(end.method, "sessionEnd");
        response.token = end.token;
        response.method = "sessionEnded";
        response.code = 200;
        response.message_sequence.reset();
        response.event_sequence.reset();
        response.payload = R"({"state":"ended","reason":"userStopped"})";
        ASSERT_TRUE(publish(response, false));
        ASSERT_TRUE(wait_until([&] {
            pump();
            return runtime.state() == RuntimeState::Idle;
        }));
        EXPECT_TRUE(runtime.voice().session_id().empty());
        EXPECT_TRUE(requests.empty());
    }
    runtime.close(now());
    platform.disconnect();
}
TEST(DesktopMqtt, SilentBrokerConnectionTimesOutWithoutBlockingDeviceLoop) {
    const auto value = environment_value("UR_TEST_SILENT_MQTT_PORT");
    if (!value)
        GTEST_SKIP() << "需要无响应端点测试入口";
    const auto port = std::strtol(value->c_str(), nullptr, 10);
    ASSERT_GT(port, 0);
    ASSERT_LT(port, 65536);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(port);
    config.client_id = "runtime-timeout-test";
    config.connect_timeout_ms = 50;
    MosquittoPort client;
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(client.connect(config));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    ASSERT_TRUE(wait_until([&] { return !client.poll(); }));
    EXPECT_FALSE(client.publish("topic", "{}"));
    bool failed = false;
    while (auto message = client.take_event())
        failed = failed || message->kind == MqttEvent::Kind::Failed;
    EXPECT_TRUE(failed);
    client.disconnect();
    client.disconnect();
    EXPECT_FALSE(client.poll());
    config.connect_timeout_ms = 0;
    EXPECT_FALSE(client.connect(config));
    config.connect_timeout_ms = 30001;
    EXPECT_FALSE(client.connect(config));
}

// 实际QoS1发布只在事件线程消费PUBACK后排空；排队/断连不能声称网络已确认。
TEST(DesktopMqtt, PublicationsDrainOnlyAfterConsumedBrokerAck) {
    const auto value = environment_value("UR_TEST_MQTT_PORT");
    if (!value)
        GTEST_SKIP() << "需要隔离Broker测试入口";
    const auto port = std::strtol(value->c_str(), nullptr, 10);
    ASSERT_GT(port, 0);
    ASSERT_LT(port, 65536);
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(port);
    config.client_id = "runtime-drain-test";
    MosquittoPort client;
    EXPECT_FALSE(client.publications_drained());
    ASSERT_TRUE(client.connect(config));
    ASSERT_TRUE(event(client, MqttEvent::Kind::Connected));
    ASSERT_TRUE(client.publications_drained());
    ASSERT_TRUE(client.publish("$thing/up/signaling/test/drain", R"({"method":"testFact"})"));
    EXPECT_FALSE(client.publications_drained());
    ASSERT_TRUE(event(client, MqttEvent::Kind::Published));
    EXPECT_TRUE(client.publications_drained());
    client.disconnect();
    EXPECT_FALSE(client.publications_drained());
}
TEST(DesktopUdp, RejectsInvalidEndpointsAndReleasesPort) {
    UdpSocket socket;
    std::vector<std::uint8_t> packet{1};
    EXPECT_EQ(socket.receive(packet), ReceiveResult::Failed);
    EXPECT_TRUE(packet.empty());
    EXPECT_FALSE(socket.send({1}));
    EXPECT_FALSE(socket.open("invalid", 0));
    ASSERT_TRUE(socket.open("127.0.0.1", 0));
    EXPECT_GT(socket.local_port(), 0);
    EXPECT_FALSE(socket.set_peer("127.0.0.1", 0));
    EXPECT_FALSE(socket.set_peer("bad", 9999));
    socket.close();
    socket.close();
    EXPECT_EQ(socket.local_port(), 0);
}
// 旁证不能事后猜测首因；禁用路径的实际失败仍按原策略返回Failed。
TEST(DesktopUdp, ReceiveFailureDiagnosticsAreOptInAndCannotBeRetrospective) {
    UdpSocket socket;
    std::vector<std::uint8_t> packet{1};  // 旧缓冲必须在失败时清空。
    EXPECT_FALSE(socket.receive_failure_diagnostic());
    EXPECT_EQ(socket.receive(packet), ReceiveResult::Failed);
    EXPECT_TRUE(packet.empty());
    EXPECT_FALSE(socket.enable_receive_failure_diagnostics());
    EXPECT_FALSE(socket.receive_failure_diagnostic());
    UdpSocket observed;
    ASSERT_TRUE(observed.enable_receive_failure_diagnostics());
    EXPECT_EQ(observed.receive(packet), ReceiveResult::Failed);
    ASSERT_TRUE(observed.receive_failure_diagnostic());
    EXPECT_EQ(*observed.receive_failure_diagnostic(), "notConnected");
    observed.close();
    EXPECT_EQ(*observed.receive_failure_diagnostic(), "notConnected");
}
#ifdef __linux__
// 实际内核回环：已成功收发后对端关闭产生ICMP拒绝；不是Mock错误或业务成功验收。
TEST(DesktopUdp, ActualPeerClosureRecordsRefusedWithoutConvertingFailureToEmpty) {
    UdpSocket socket, peer;
    ASSERT_TRUE(socket.enable_receive_failure_diagnostics());
    ASSERT_TRUE(socket.open("127.0.0.1", 0));
    ASSERT_TRUE(peer.open("127.0.0.1", 0));
    ASSERT_TRUE(socket.set_peer("127.0.0.1", peer.local_port()));
    ASSERT_TRUE(peer.set_peer("127.0.0.1", socket.local_port()));
    std::vector<std::uint8_t> packet;  // 当前真实数据报，不复用前次内容。
    EXPECT_EQ(socket.receive(packet), ReceiveResult::Empty);
    EXPECT_FALSE(socket.receive_failure_diagnostic());
    ASSERT_TRUE(peer.send({1, 2, 3}));
    ASSERT_TRUE(wait_until([&] { return socket.receive(packet) == ReceiveResult::Received; }));
    EXPECT_EQ(packet, (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_FALSE(socket.receive_failure_diagnostic());
    peer.close();
    ASSERT_TRUE(socket.send({4, 5}));
    ASSERT_TRUE(wait_until([&] { return socket.receive(packet) == ReceiveResult::Failed; }));
    EXPECT_TRUE(packet.empty());
    ASSERT_TRUE(socket.receive_failure_diagnostic());
    EXPECT_EQ(*socket.receive_failure_diagnostic(), "peerRefused");
    socket.close();
    EXPECT_EQ(socket.receive(packet), ReceiveResult::Failed);
    EXPECT_EQ(*socket.receive_failure_diagnostic(), "peerRefused");
}
#endif
TEST(DesktopUdp, RealBidirectionalEncryptedOpusForBothFrameFormats) {
    for (AudioFormat format : {AudioFormat{16000, 1, 20}, AudioFormat{24000, 1, 60}}) {
        UdpSocket left_socket, right_socket;
        ASSERT_TRUE(left_socket.open("127.0.0.1", 0));
        ASSERT_TRUE(right_socket.open("127.0.0.1", 0));
        ASSERT_TRUE(left_socket.set_peer("127.0.0.1", right_socket.local_port()));
        ASSERT_TRUE(right_socket.set_peer("127.0.0.1", left_socket.local_port()));
        EXPECT_FALSE(left_socket.set_peer("127.0.0.1", right_socket.local_port()));
        EXPECT_FALSE(left_socket.send({}));
        EXPECT_FALSE(left_socket.send(std::vector<std::uint8_t>(65508, 1)));
        const std::array<std::uint8_t, 16> test_key{};  // 公开离线测试密钥，非平台凭据。
        OpenSslCtr left_cipher(test_key), right_cipher(test_key);
        UdpChannel left(left_cipher, {1, 2, 3, 4, 5, 6, 7, 8}),
            right(right_cipher, {1, 2, 3, 4, 5, 6, 7, 8});
        OpusCodec left_codec, right_codec;
        ASSERT_TRUE(left_codec.configure(format));
        ASSERT_TRUE(right_codec.configure(format));
        SpeechOnset left_onset, right_onset;
        const auto samples = static_cast<std::size_t>(format.sample_rate * format.frame_ms / 1000);
        std::vector<std::int16_t> pcm(samples);
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::int16_t>(
                5000 *
                std::sin(6.283185307179586 * 440 * static_cast<double>(i) / format.sample_rate));
        for (int frame = 0; frame < 10; ++frame) {
            std::vector<std::uint8_t> encoded, packet, received, decoded;
            ASSERT_TRUE(left_codec.encode(pcm, encoded));
            ASSERT_EQ(left.encode(encoded, packet), UdpResult::Ok);
            ASSERT_TRUE(left_socket.send(packet));
            ASSERT_TRUE(right_codec.encode(pcm, encoded));
            ASSERT_EQ(right.encode(encoded, packet), UdpResult::Ok);
            ASSERT_TRUE(right_socket.send(packet));
            // 两向均先发后收，证明同一时间窗口内不以播放阻塞上行。
            ASSERT_TRUE(wait_until(
                [&] { return right_socket.receive(received) == ReceiveResult::Received; }));
            ASSERT_EQ(right.decode(received.data(), received.size(), decoded), UdpResult::Ok);
            std::vector<std::int16_t> audio;
            ASSERT_TRUE(right_codec.decode(decoded, audio));
            EXPECT_EQ(audio.size(), samples);
            EXPECT_EQ(right_onset.observe(audio), frame > 0);
            ASSERT_TRUE(wait_until(
                [&] { return left_socket.receive(received) == ReceiveResult::Received; }));
            ASSERT_EQ(left.decode(received.data(), received.size(), decoded), UdpResult::Ok);
            ASSERT_TRUE(left_codec.decode(decoded, audio));
            EXPECT_EQ(left_onset.observe(audio), frame > 0);
        }
        std::vector<std::uint8_t> no_packet;
        EXPECT_EQ(left_socket.receive(no_packet), ReceiveResult::Empty);
    }
}
TEST(DesktopOpus, SilenceInvalidPacketsResetAndFormatMismatch) {
    OpusCodec codec;
    std::vector<std::uint8_t> packet;
    std::vector<std::int16_t> decoded;
    EXPECT_FALSE(codec.encode({}, packet));
    EXPECT_FALSE(codec.decode({0}, decoded));
    EXPECT_FALSE(codec.configure({44100, 1, 20}));
    EXPECT_FALSE(codec.configure({16000, 2, 20}));
    EXPECT_FALSE(codec.configure({16000, 1, 10}));
    ASSERT_TRUE(codec.configure({16000, 1, 20}));
    EXPECT_FALSE(codec.encode({1}, packet));
    ASSERT_TRUE(codec.encode(std::vector<std::int16_t>(320, 0), packet));
    ASSERT_TRUE(codec.decode(packet, decoded));
    SpeechOnset onset;
    EXPECT_FALSE(onset.observe(decoded));
    EXPECT_FALSE(onset.observe({}));
    EXPECT_FALSE(onset.observe(std::vector<std::int16_t>(320, 128)));
    EXPECT_TRUE(onset.observe(std::vector<std::int16_t>(320, 128)));
    EXPECT_TRUE(onset.observe(std::vector<std::int16_t>(320, 128)));
    onset.reset();
    EXPECT_FALSE(onset.observe(std::vector<std::int16_t>(320, 128)));
    EXPECT_FALSE(onset.observe(std::vector<std::int16_t>(320, 127)));
    EXPECT_FALSE(onset.observe(std::vector<std::int16_t>(320, 128)));
    codec.reset();
    ASSERT_TRUE(codec.configure({24000, 1, 60}));
    EXPECT_FALSE(codec.decode(packet, decoded));
    EXPECT_TRUE(decoded.empty());
    EXPECT_FALSE(codec.decode({}, decoded));
    EXPECT_FALSE(codec.decode(std::vector<std::uint8_t>(1276, 0), decoded));
    EXPECT_FALSE(codec.decode({0xff}, decoded));
}
}  // namespace
}  // namespace ur::device
