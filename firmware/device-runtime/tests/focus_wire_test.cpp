// 文件说明：焦点契约的字段、整组、原关联与畸形输入回归；解析单测不冒称驱动焦点完成。
#include "ur/device/focus_wire.hpp"
#include "ur/device/endpoint_wire.hpp"
#include <cJSON.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>
#include "ur/device/playback_wire.hpp"

using namespace ur::device;
namespace {
std::size_t allocation_index = 0;   // 仅本用例控制cJSON第N次分配，不替换C++或第三方网络分配器。
std::size_t failed_allocation = 0;  // 一次失败位置；零表示未开启注入。
bool allocation_failed = false;     // 区分已触发故障与超出本操作分配总数。
// allocate 在指定一次分配返回空，其余仍走系统分配器，验证所有失败出口正确释放。
void* allocate(std::size_t size) {
    if (++allocation_index == failed_allocation) {
        allocation_failed = true;
        return nullptr;
    }
    return std::malloc(size);
}
// AllocationFailure 在唯一测试线程临时安装钩子，析构无条件恢复默认分配器。
struct AllocationFailure {
    explicit AllocationFailure(std::size_t index) {
        allocation_index = 0;
        failed_allocation = index;
        allocation_failed = false;
        cJSON_Hooks hooks{allocate, std::free};
        cJSON_InitHooks(&hooks);
    }
    ~AllocationFailure() {
        cJSON_InitHooks(nullptr);
        failed_allocation = 0;
    }
};
// control 使用完整SIG下行夹具，expire字段只在取得/恢复中存在。
std::string control(std::string_view operation = "acquire",
                    std::string_view group = R"(["speaker","display"])") {
    return std::string(
               R"({"msgToken":"f-1","method":"focusControl","sessionId":"child","eventSeq":1,"params":{"operation":")") +
           std::string(operation) + R"(","expectedRevision":0,"focusId":"focus-1","resources":)" +
           std::string(group) + R"(,"priority":"conversation","resumePolicy":"explicit")" +
           ((operation == "acquire" || operation == "restore") ? R"(,"expiresAt":1900000000000)"
                                                               : "") +
           "}}";
}
// modify 改写单个原字段，用于精确错误路径，而非生产字符串协议处理。
std::string modify(std::string json, std::string_view before, std::string_view after) {
    const auto start = json.find(before);
    EXPECT_NE(start, std::string::npos);
    if (start != std::string::npos)
        json.replace(start, before.size(), after);
    return json;
}
// encoded 将原协议信封独立序列化，测试不绕过正式编码器。
std::string encoded(const Envelope& envelope) {
    std::string json;
    EXPECT_EQ(encode_envelope(envelope, json), DecodeResult::Ok);
    return json;
}
}  // namespace

// 显式组合不改变文件端用途/输出；支持原地调用，不为未装配的宿主自动上报焦点。
TEST(FocusWire, AddsExplicitCapabilityAndPreservesOriginal) {
    PlaybackCapability capability;
    capability.audio_output = true;
    capability.video_output = true;
    Envelope source;
    ASSERT_EQ(playback_capability_command("cap-focus", capability, source), WireResult::Ok);
    const auto original = source.payload;
    EXPECT_EQ(original.find("controlCapabilities"), std::string::npos);
    Envelope out;
    ASSERT_EQ(add_focus_capability(source, {"speaker", "display"}, out), WireResult::Ok);
    EXPECT_EQ(source.payload, original);
    EXPECT_EQ(out.token, source.token);
    EXPECT_EQ(out.method, source.method);
    EXPECT_NE(out.payload.find("focusControl"), std::string::npos);
    EXPECT_NE(out.payload.find("mediaPlayback"), std::string::npos);
    EXPECT_NE(out.payload.find("outputModalities"), std::string::npos);
    ASSERT_EQ(add_focus_capability(source, {"speaker", "display"}, source), WireResult::Ok);
    EXPECT_EQ(source.payload, out.payload);
    EXPECT_EQ(add_focus_capability(source, {"speaker"}, out), WireResult::Invalid);
    EXPECT_TRUE(out.token.empty());
}

// 畸形资源或错误信封不得转成能力；摄像头必须来自原快照中的明确cameraId。
TEST(FocusWire, CapabilityRejectsMalformedResourceAndMetadata) {
    PlaybackCapability capability;
    capability.audio_output = true;
    Envelope source;
    ASSERT_EQ(playback_capability_command("cap-focus", capability, source), WireResult::Ok);
    const std::vector<std::vector<std::string>> groups = {{},
                                                          {"speaker", "speaker"},
                                                          {"video"},
                                                          {"camera:front"},
                                                          {"camera:/private"},
                                                          {std::string("speaker\0hidden", 14)},
                                                          {std::string("camera:\xff", 8)},
                                                          std::vector<std::string>(17, "speaker")};
    for (const auto& group : groups) {
        Envelope out = source;
        EXPECT_EQ(add_focus_capability(source, group, out), WireResult::Invalid);
        EXPECT_TRUE(out.token.empty());
    }
    for (int index = 0; index < 6; ++index) {
        auto invalid = source;
        if (index == 0)
            invalid.method = "sessionCreate";
        if (index == 1)
            invalid.session = "session";
        if (index == 2)
            invalid.code = 200;
        if (index == 3)
            invalid.event_sequence = 1;
        if (index == 4)
            invalid.message_sequence = 1;
        if (index == 5)
            invalid.request = false;
        Envelope out;
        EXPECT_EQ(add_focus_capability(invalid, {"speaker"}, out), WireResult::Invalid);
    }
    source.payload = R"({"cameras":[{"cameraId":"front"}]})";
    Envelope out;
    EXPECT_EQ(add_focus_capability(source, {"camera:front"}, out), WireResult::Ok);
}

// 逐个注入cJSON分配失败，确认不会留下半份声明或误报成功。
TEST(FocusWire, CapabilityAllocationFailuresClearOutput) {
    PlaybackCapability capability;
    capability.audio_output = true;
    Envelope source;
    ASSERT_EQ(playback_capability_command("cap-focus", capability, source), WireResult::Ok);
    bool exhausted = false;
    for (std::size_t index = 1; index < 1000; ++index) {
        Envelope out;
        AllocationFailure failure(index);
        const auto result = add_focus_capability(source, {"speaker"}, out);
        if (!allocation_failed) {
            EXPECT_EQ(result, WireResult::Ok);
            exhausted = true;
            break;
        }
        EXPECT_EQ(result, WireResult::Invalid);
        EXPECT_TRUE(out.token.empty());
    }
    EXPECT_TRUE(exhausted);
}

// 首次基线保留实际零/非零/最大水位和完整原组，不从能力revision推断资源revision。
TEST(FocusWire, AddsExplicitIdleBaselineWithoutOverwriting) {
    PlaybackCapability capability;
    capability.audio_output = capability.video_output = true;
    Envelope source, focused;
    ASSERT_EQ(playback_capability_command("baseline", capability, source), WireResult::Ok);
    ASSERT_EQ(add_focus_capability(source, {"speaker", "display"}, focused), WireResult::Ok);
    const auto original = focused.payload;
    for (auto revision : {0ULL, 17ULL, 9007199254740991ULL}) {
        Envelope out;
        ASSERT_EQ(add_focus_baseline(focused, {"display", "speaker"}, revision, out),
                  WireResult::Ok);
        EXPECT_EQ(out.token, focused.token);
        EXPECT_EQ(focused.payload, original);
        EXPECT_NE(out.payload.find("\"state\":\"idle\""), std::string::npos);
        EXPECT_NE(out.payload.find("\"revision\":" + std::to_string(revision)), std::string::npos);
        EXPECT_EQ(add_focus_baseline(out, {"speaker", "display"}, revision, source),
                  WireResult::Invalid);
        EXPECT_TRUE(source.token.empty());
    }
    ASSERT_EQ(add_focus_baseline(focused, {"speaker", "display"}, 17, focused), WireResult::Ok);
    EXPECT_NE(focused.payload.find("baseline"), std::string::npos);
}

// 部分、重复、扩展组和越界实际水位不得变成首次idle；缺能力/既有基线亦不覆盖。
TEST(FocusWire, BaselineRejectsMalformedGroupAndControls) {
    PlaybackCapability capability;
    capability.audio_output = capability.video_output = true;
    Envelope source, focused, out;
    ASSERT_EQ(playback_capability_command("baseline", capability, source), WireResult::Ok);
    ASSERT_EQ(add_focus_capability(source, {"speaker", "display"}, focused), WireResult::Ok);
    for (const auto& group :
         std::vector<std::vector<std::string>>{{},
                                               {"speaker"},
                                               {"speaker", "speaker"},
                                               {"speaker", "microphone"},
                                               {"speaker", "display", "microphone"},
                                               {std::string("speaker\0hidden", 14), "display"},
                                               std::vector<std::string>(17, "speaker")}) {
        EXPECT_EQ(add_focus_baseline(focused, group, 17, out), WireResult::Invalid);
        EXPECT_TRUE(out.token.empty());
    }
    EXPECT_EQ(add_focus_baseline(focused, {"speaker", "display"}, 9007199254740992ULL, out),
              WireResult::Invalid);
    EXPECT_EQ(add_focus_baseline(source, {"speaker", "display"}, 17, out), WireResult::Invalid);
    for (const auto& change : std::vector<std::pair<std::string, std::string>>{
             {"focusControl", "playbackControl"},
             {R"("operations":["focusControl"])", R"("operations":null)"},
             {R"("operations":["focusControl"])",
              R"("operations":["focusControl"],"baseline":null)"},
             {R"("focusResources":["speaker","display"])",
              R"("focusResources":["speaker","speaker"])"},
             {R"("operations":["focusControl"])",
              R"("operations":["focusControl"],"trusted":true)"}}) {
        auto invalid = focused;
        invalid.payload = modify(invalid.payload, change.first, change.second);
        EXPECT_EQ(add_focus_baseline(invalid, {"speaker", "display"}, 17, out),
                  WireResult::Invalid);
        EXPECT_TRUE(out.token.empty());
    }
    source.payload = R"({"cameras":[{"cameraId":"front"}]})";
    ASSERT_EQ(add_focus_capability(source, {"camera:front"}, focused), WireResult::Ok);
    EXPECT_EQ(add_focus_baseline(focused, {"camera:front"}, 3, out), WireResult::Ok);
    focused.payload = modify(focused.payload, R"("cameraId":"front")", R"("cameraId":"back")");
    EXPECT_EQ(add_focus_baseline(focused, {"camera:front"}, 3, out), WireResult::Invalid);
}

// 对每一个JSON分配位置注入失败，不能留下半份首次证明或泄漏解析树。
TEST(FocusWire, BaselineAllocationFailuresClearOutput) {
    PlaybackCapability capability;
    capability.audio_output = true;
    Envelope source, focused;
    ASSERT_EQ(playback_capability_command("baseline", capability, source), WireResult::Ok);
    ASSERT_EQ(add_focus_capability(source, {"speaker"}, focused), WireResult::Ok);
    bool exhausted = false;
    for (std::size_t index = 1; index < 1000; ++index) {
        Envelope out;
        AllocationFailure failure(index);
        const auto result = add_focus_baseline(focused, {"speaker"}, 17, out);
        if (!allocation_failed) {
            EXPECT_EQ(result, WireResult::Ok);
            exhausted = true;
            break;
        }
        EXPECT_EQ(result, WireResult::Invalid);
        EXPECT_TRUE(out.token.empty());
    }
    EXPECT_TRUE(exhausted);
}

// 能力与拒绝回执同样经过中间JSON树，最大安全水位必须精确保留。
TEST(FocusWire, MaximumRevisionSurvivesIntermediateSerialization) {
    PlaybackCapability capability;
    capability.audio_output = true;
    Envelope source, focused, out;
    ASSERT_EQ(playback_capability_command("maximum", capability, source), WireResult::Ok);
    source.payload = modify(source.payload, R"("revision":1)", R"("revision":9007199254740991)");
    ASSERT_EQ(add_focus_capability(source, {"speaker"}, focused), WireResult::Ok);
    EXPECT_NE(focused.payload.find(R"("revision":9007199254740991)"), std::string::npos);
    ASSERT_EQ(add_focus_baseline(focused, {"speaker"}, 9007199254740991ULL, out), WireResult::Ok);
    EXPECT_NE(out.payload.find(R"("revision":9007199254740991)"), std::string::npos);
    FocusCommand command;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(), command), WireResult::Ok);
    ASSERT_EQ(focus_reply(command, "denied", 9007199254740991ULL, 409, "staleRevision", out),
              WireResult::Ok);
    FocusState state;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, encoded(out), state), WireResult::Ok);
    EXPECT_EQ(state.revision, 9007199254740991ULL);
}

TEST(FocusWire, AllOperationsAndOriginalGroup) {
    const std::array<std::pair<const char*, const char*>, 4> cases = {{{"acquire", "acquired"},
                                                                       {"suspend", "suspended"},
                                                                       {"restore", "restored"},
                                                                       {"release", "released"}}};
    for (const auto& item : cases) {
        FocusCommand command;
        ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(item.first), command),
                  WireResult::Ok);
        EXPECT_EQ(command.envelope.session, "child");
        EXPECT_EQ(command.envelope.token, "f-1");
        EXPECT_EQ(command.expected_revision, 0U);
        EXPECT_EQ(command.resources, (std::vector<std::string>{"speaker", "display"}));
        EXPECT_EQ(command.expires_at.has_value(),
                  command.operation == "acquire" || command.operation == "restore");
        Envelope out;
        ASSERT_EQ(focus_reply(command, item.second, 1, 200, "", out), WireResult::Ok);
        FocusState fact;
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, encoded(out), fact), WireResult::Ok);
        EXPECT_TRUE(focus_reply_matches(command, fact));
        EXPECT_EQ(fact.resources, command.resources);
        EXPECT_FALSE(out.event_sequence);
    }
}

TEST(FocusWire, AllResourcesPrioritiesAndPolicies) {
    for (const auto* group :
         {R"(["speaker"])", R"(["display"])",
          R"(["microphone","speaker","display","camera:front","camera:back"] )"}) {
        for (const auto* priority : {"background", "conversation", "call", "emergency"}) {
            for (const auto* policy : {"never", "explicit", "ifUnchanged"}) {
                auto json = modify(control("acquire", group), "conversation", priority);
                json = modify(json, "explicit", policy);
                FocusCommand command;
                ASSERT_EQ(decode_focus_control(WireRoute::Signaling, json, command),
                          WireResult::Ok);
                EXPECT_EQ(command.priority, priority);
                EXPECT_EQ(command.resume_policy, policy);
            }
        }
    }
}

TEST(FocusWire, RejectsIllegalFieldsAndClearsOriginal) {
    const std::array<std::pair<const char*, const char*>, 21> cases = {
        {{"\"acquire\"", "\"start\""},
         {"\"expectedRevision\":0", "\"expectedRevision\":null"},
         {"\"expectedRevision\":0", "\"expectedRevision\":-1"},
         {"\"expectedRevision\":0", "\"expectedRevision\":0.5"},
         {"\"expectedRevision\":0", "\"expectedRevision\":\"0\""},
         {"\"expectedRevision\":0", "\"expectedRevision\":9007199254740992"},
         {"\"expectedRevision\":0", "\"expectedRevision\":0,\"expectedRevision\":1"},
         {"\"focus-1\"", "\"\""},
         {"\"child\"", "\"\""},
         {"\"conversation\"", "\"urgent\""},
         {"\"explicit\"", "\"automatic\""},
         {"\"expiresAt\":1900000000000", "\"expiresAt\":0"},
         {"\"expiresAt\":1900000000000", "\"expiresAt\":null"},
         {"\"expiresAt\":1900000000000", "\"otherField\":1"},
         {"\"expiresAt\":1900000000000", "\"expiresAt\":1900000000000,\"deviceName\":\"other\""},
         {"\"eventSeq\":1", "\"eventSeq\":0"},
         {"\"eventSeq\":1", "\"eventSeq\":null"},
         {"\"eventSeq\":1", "\"eventSeq\":1,\"code\":200"},
         {"\"eventSeq\":1", "\"msgSeq\":1"},
         {"\"params\"", "\"data\""},
         {"\"focusControl\"", "\"playbackControl\""}}};
    for (const auto& item : cases) {
        FocusCommand command;
        command.focus_id = "old";
        command.resources = {"speaker"};
        command.expires_at = 3;
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling,
                                       modify(control(), item.first, item.second), command),
                  WireResult::Invalid);
        EXPECT_TRUE(command.focus_id.empty());
        EXPECT_TRUE(command.resources.empty());
        EXPECT_FALSE(command.expires_at);
    }
    FocusCommand command;
    EXPECT_EQ(decode_focus_control(WireRoute::Message, control(), command), WireResult::WrongRoute);
    for (const auto* operation : {"suspend", "release"}) {
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling,
                                       modify(control(operation), "\"expectedRevision\":0",
                                              "\"expectedRevision\":0,\"expiresAt\":19"),
                                       command),
                  WireResult::Invalid);
    }
}

TEST(FocusWire, RejectsIllegalResourcesAndBoundaries) {
    for (const auto* group :
         {"[]", "null", R"(["speaker","speaker"])", R"(["video"])", R"(["camera:"])",
          R"(["camera:front room"])", R"(["camera:/private"])", R"(["camera:back\\door"])",
          R"([true])", R"(["camera"])", R"(["speaker",null])"}) {
        FocusCommand command;
        EXPECT_EQ(decode_focus_control(WireRoute::Signaling, control("acquire", group), command),
                  WireResult::Invalid);
    }
    std::string group = "[";
    for (unsigned index = 0; index < 17; ++index) {
        if (index)
            group += ',';
        group += "\"camera:" + std::to_string(index) + "\"";
    }
    group += ']';
    FocusCommand command;
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, control("acquire", group), command),
              WireResult::Invalid);
    group = "[\"camera:" + std::string(122, 'a') + "\"]";
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, control("acquire", group), command),
              WireResult::Invalid);
    group = "[\"camera:" + std::string(121, 'a') + "\"]";
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling, control("acquire", group), command),
              WireResult::Ok);
    EXPECT_EQ(decode_focus_control(WireRoute::Signaling,
                                   modify(control(), "\"expectedRevision\":0",
                                          "\"expectedRevision\":9007199254740991"),
                                   command),
              WireResult::Ok);
    Envelope out;
    EXPECT_EQ(focus_reply(command, "acquired", 9007199254740991ULL, 200, "", out),
              WireResult::Invalid);
}

TEST(FocusWire, DenialKeepsActualRevisionAndSanitizesReason) {
    FocusCommand command;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(), command), WireResult::Ok);
    for (const auto* why : {"permissionDenied", "resourceBusy", "unsupportedResource",
                            "staleRevision", "leaseExpired", "invalidState", "userIntentChanged",
                            "sessionEnded", "timeout", "driverFailed"}) {
        Envelope reply;
        ASSERT_EQ(focus_reply(command, "denied", 5, 409, why, reply), WireResult::Ok);
        FocusState state;
        ASSERT_EQ(decode_focus_state(WireRoute::Signaling, encoded(reply), state), WireResult::Ok);
        EXPECT_EQ(state.revision, 5U);
        EXPECT_TRUE(focus_reply_matches(command, state));
    }
    for (const auto* why : {"", "https://private.example/token", "driver: raw secret"}) {
        Envelope reply;
        reply.method = "old";
        EXPECT_EQ(focus_reply(command, "denied", 0, 409, why, reply), WireResult::Invalid);
        EXPECT_TRUE(reply.method.empty());
    }
    Envelope out;
    EXPECT_EQ(focus_reply(command, "denied", 0, 200, "resourceBusy", out), WireResult::Invalid);
    EXPECT_EQ(focus_reply(command, "denied", 409, 0, "resourceBusy", out), WireResult::Invalid);
    EXPECT_EQ(focus_reply(command, "acquired", 1, 200, "resourceBusy", out), WireResult::Invalid);
    EXPECT_EQ(focus_reply(command, "suspended", 1, 200, "", out), WireResult::Invalid);
}

TEST(FocusWire, OriginalReplyCannotBeReplacedByOtherGroupOrAutonomousEvent) {
    FocusCommand command;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(), command), WireResult::Ok);
    Envelope reply;
    ASSERT_EQ(focus_reply(command, "acquired", 1, 200, "", reply), WireResult::Ok);
    FocusState original;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, encoded(reply), original), WireResult::Ok);
    const std::array<std::function<void(FocusState&)>, 7> changes = {
        {[](auto& value) { value.envelope.token = "other"; },
         [](auto& value) { value.envelope.session = "other"; },
         [](auto& value) { value.focus_id = "other"; },
         [](auto& value) { value.resources = {"speaker"}; },
         [](auto& value) { value.resources = {"speaker", "camera:front"}; },
         [](auto& value) { value.revision = 0; },
         [](auto& value) { value.envelope.code.reset(); }}};
    for (const auto& change : changes) {
        auto state = original;
        change(state);
        EXPECT_FALSE(focus_reply_matches(command, state));
    }
    ASSERT_EQ(decode_focus_state(
                  WireRoute::Signaling,
                  modify(encoded(reply), R"(["speaker","display"])", R"(["display","speaker"])"),
                  original),
              WireResult::Ok);
    EXPECT_TRUE(focus_reply_matches(command, original));
    reply.code.reset();
    FocusState autonomous;
    ASSERT_EQ(decode_focus_state(WireRoute::Signaling, encoded(reply), autonomous), WireResult::Ok);
    EXPECT_FALSE(focus_reply_matches(command, autonomous));
    command.resources = {"speaker"};
    EXPECT_EQ(focus_reply(command, "acquired", 1, 200, "", reply), WireResult::Invalid);
}

TEST(FocusWire, RejectsMalformedStateAndWrongRoute) {
    FocusCommand command;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(), command), WireResult::Ok);
    Envelope reply;
    ASSERT_EQ(focus_reply(command, "acquired", 1, 200, "", reply), WireResult::Ok);
    const auto json = encoded(reply);
    for (const auto& item : std::vector<std::pair<std::string, std::string>>{
             {"\"acquired\"", "\"pending\""},
             {"\"revision\":1", "\"revision\":null"},
             {"\"revision\":1", "\"revision\":9007199254740992"},
             {"\"code\":200", "\"code\":201"},
             {"\"code\":200", "\"code\":200,\"eventSeq\":1"},
             {"\"focus-1\"", "\"\""},
             {"\"data\"", "\"params\""},
             {"\"revision\":1", "\"revision\":1,\"reason\":\"resourceBusy\""}}) {
        FocusState state;
        state.focus_id = "old";
        EXPECT_EQ(
            decode_focus_state(WireRoute::Signaling, modify(json, item.first, item.second), state),
            WireResult::Invalid);
        EXPECT_TRUE(state.focus_id.empty());
    }
    FocusState state;
    EXPECT_EQ(decode_focus_state(WireRoute::Message, json, state), WireResult::WrongRoute);
}

TEST(FocusWire, AllocationFailureNeverEmitsPartialResourceProof) {
    FocusCommand command;
    ASSERT_EQ(decode_focus_control(WireRoute::Signaling, control(), command), WireResult::Ok);
    unsigned failures = 0, successes = 0;
    std::size_t allocations = 0;  // 以原成功操作实际分配总数定位故障，不猜测调用数量。
    {
        AllocationFailure count(std::numeric_limits<std::size_t>::max());
        Envelope reply;
        ASSERT_EQ(focus_reply(command, "acquired", 1, 200, "", reply), WireResult::Ok);
        allocations = allocation_index;
    }
    ASSERT_LT(allocations, 10000U);
    for (std::size_t index = 1; index <= allocations + 1; ++index) {
        Envelope reply;
        reply.method = "old";
        WireResult result;
        bool failed;
        {
            AllocationFailure inject(index);
            result = focus_reply(command, "acquired", 1, 200, "", reply);
            failed = allocation_failed;
        }
        if (failed) {
            ++failures;
            EXPECT_EQ(result, WireResult::Invalid);
            EXPECT_TRUE(reply.method.empty());
        } else {
            ++successes;
            EXPECT_EQ(result, WireResult::Ok);
        }
    }
    EXPECT_GT(failures, 100U);
    EXPECT_GT(successes, 0U);
}

// endpoint_control 生成原上行接续请求；extra按操作附加条件字段，不代表平台已受理。
std::string endpoint_control(std::string_view operation, std::string_view extra = {}) {
    return std::string(
               R"({"msgToken":"s18-62","method":"endpointControl","sessionId":"s18-session","params":{"operation":")") +
           std::string(operation) + R"(","expectedRevision":0,"handoffId":"handoff-1")" +
           std::string(extra) + "}}";
}
constexpr std::string_view endpoint_target = R"(,"target":{"kind":"endpoint","id":"tablet-a"})";
constexpr std::string_view endpoint_candidate = R"(,"candidateSessionId":"s18-new")";
// endpoint_state 生成平台下行事实；extra放置code或事件水位，body按状态附加条件字段。
std::string endpoint_state(std::string_view state, std::string_view extra = "\"code\":200,",
                           std::string_view body = {}) {
    return std::string(
               R"({"msgToken":"s18-62","method":"endpointState","sessionId":"s18-session",)") +
           std::string(extra) + R"("data":{"state":")" + std::string(state) +
           R"(","revision":1,"handoffId":"handoff-1")" + std::string(body) + "}}";
}

// 三操作与条件字段完整原文互相解析；字段合法不代表接续获准或目标端已授权。
TEST(EndpointWire, AllOperationsAndConditionalFields) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"prepare", std::string(endpoint_target)},
        {"commit", std::string(endpoint_candidate)},
        {"abort", {}}};
    for (const auto& [operation, extra] : cases) {
        EndpointCommand command;
        ASSERT_EQ(decode_endpoint_control(WireRoute::Signaling,
                                          endpoint_control(operation, extra), command),
                  WireResult::Ok);
        EXPECT_EQ(command.envelope.session, "s18-session");
        EXPECT_EQ(command.envelope.token, "s18-62");
        EXPECT_EQ(command.operation, operation);
        EXPECT_EQ(command.expected_revision, 0U);
        EXPECT_EQ(command.handoff_id, "handoff-1");
        EXPECT_EQ(command.target.has_value(), operation == "prepare");
        if (command.target) {
            EXPECT_EQ(command.target->kind, "endpoint");
            EXPECT_EQ(command.target->id, "tablet-a");
        }
        EXPECT_EQ(command.candidate_session_id.empty(), operation != "commit");
    }
    EndpointCommand command;
    EXPECT_EQ(decode_endpoint_control(WireRoute::Message,
                                      endpoint_control("prepare", endpoint_target), command),
              WireResult::WrongRoute);
    ASSERT_EQ(decode_endpoint_control(
                  WireRoute::Signaling,
                  modify(endpoint_control("commit", endpoint_candidate), "\"expectedRevision\":0",
                         "\"expectedRevision\":9007199254740991"),
                  command),
              WireResult::Ok);
    EXPECT_EQ(command.expected_revision, 9007199254740991ULL);
}

// revision严格边界、未知字段、条件字段错位和信封形态错误必须整体拒绝并清空输出。
TEST(EndpointWire, RejectsIllegalFieldsAndClearsOriginal) {
    const std::array<std::pair<std::string, std::string>, 19> changes = {
        {{R"("prepare")", R"("restart")"},
         {R"("expectedRevision":0)", R"("expectedRevision":null)"},
         {R"("expectedRevision":0)", R"("expectedRevision":-1)"},
         {R"("expectedRevision":0)", R"("expectedRevision":0.5)"},
         {R"("expectedRevision":0)", R"("expectedRevision":"0")"},
         {R"("expectedRevision":0)", R"("expectedRevision":9007199254740992)"},
         {R"("expectedRevision":0)",
          R"("expectedRevision":0,"expectedRevision":1)"},
         {R"("handoff-1")", R"("")"},
         {R"("handoff-1")", R"("handoff-1","extra":1)"},
         {R"("kind":"endpoint")", R"("kind":"device")"},
         {R"("kind":"endpoint","id":"tablet-a")", R"("kind":"endpoint")"},
         {std::string(endpoint_target), {}},
         {std::string(endpoint_target), R"(,"target":null)"},
         {std::string(endpoint_target), std::string(endpoint_candidate)},
         {R"("endpointControl")", R"("endpointControl","eventSeq":1)"},
         {R"("endpointControl")", R"("endpointControl","code":200)"},
         {R"("s18-session")", std::string(65, 'x')},
         {R"("params")", R"("data")"},
         {R"("endpointControl")", R"("playbackControl")"}}};
    for (const auto& change : changes) {
        EndpointCommand command;
        command.operation = "old";
        command.handoff_id = "old";
        command.expected_revision = 7;
        command.target = EndpointTarget{"endpoint", "old"};
        command.candidate_session_id = "old";
        EXPECT_EQ(decode_endpoint_control(WireRoute::Signaling,
                                          modify(endpoint_control("prepare", endpoint_target),
                                                 change.first, change.second),
                                          command),
                  WireResult::Invalid)
            << change.first << " -> " << change.second;
        EXPECT_TRUE(command.handoff_id.empty());
        EXPECT_FALSE(command.target.has_value());
        EXPECT_TRUE(command.candidate_session_id.empty());
    }
    const std::array<std::pair<std::string, std::string>, 4> commit_changes = {
        {{std::string(endpoint_candidate), {}},
         {R"("handoff-1")", R"("handoff-1","extra":1)"},
         {std::string(endpoint_candidate),
          std::string(endpoint_candidate) + std::string(endpoint_target)},
         {R"("commit")", R"("abort")"}}};
    for (const auto& change : commit_changes) {
        EndpointCommand command;
        command.handoff_id = "old";
        EXPECT_EQ(decode_endpoint_control(WireRoute::Signaling,
                                          modify(endpoint_control("commit", endpoint_candidate),
                                                 change.first, change.second),
                                          command),
                  WireResult::Invalid);
        EXPECT_TRUE(command.handoff_id.empty());
    }
    for (const auto& extra : {std::string(endpoint_target), std::string(endpoint_candidate)}) {
        EndpointCommand command;
        EXPECT_EQ(decode_endpoint_control(WireRoute::Signaling,
                                          endpoint_control("abort", extra), command),
                  WireResult::Invalid);
    }
}

// 五状态原文形态与reason白名单互解；阶段性状态不代替完成，失败不透传内部错误细节。
TEST(EndpointWire, StateFormsReasonsAndConditionalFields) {
    const std::vector<std::pair<std::string, std::string>> accepted = {
        {"preparing", {}},
        {"ready", R"(,"candidateSessionId":"s18-new")"},
        {"committed", R"(,"candidateSessionId":"s18-new")"},
        {"aborted", R"(,"reason":"userCancelled")"}};
    for (const auto& [state, body] : accepted) {
        for (const auto* extra : {"\"code\":200,", "\"eventSeq\":3,"}) {
            EndpointState fact;
            ASSERT_EQ(decode_endpoint_state(WireRoute::Signaling,
                                            endpoint_state(state, extra, body), fact),
                      WireResult::Ok)
                << state << " " << extra;
            EXPECT_EQ(fact.envelope.session, "s18-session");
            EXPECT_EQ(fact.revision, 1U);
            EXPECT_EQ(fact.handoff_id, "handoff-1");
            EXPECT_EQ(fact.candidate_session_id.empty(),
                      state != "ready" && state != "committed");
            EXPECT_EQ(fact.reason.empty(), state != "failed" && state != "aborted");
        }
    }
    // failed不能以200应答伪装非失败；事件水位或4xx/5xx应答都是合法失败表达。
    for (const auto* extra : {"\"eventSeq\":3,", "\"code\":504,", "\"code\":409,"}) {
        EndpointState fact;
        ASSERT_EQ(decode_endpoint_state(WireRoute::Signaling,
                                        endpoint_state("failed", extra,
                                                       R"(,"reason":"handoffTimeout")"),
                                        fact),
                  WireResult::Ok)
            << extra;
    }
    EndpointState route;
    EXPECT_EQ(decode_endpoint_state(WireRoute::Message,
                                    endpoint_state("preparing"), route),
              WireResult::WrongRoute);
}

// 媒体失败、撤权和源代次变化各有固定原因，不能被改写为超时或用户取消。
TEST(EndpointWire, AccurateHandoffFailureReasons) {
    for (const auto* reason : {"mediaFailed", "permissionRevoked", "sourceChanged"}) {
        EndpointState fact;
        ASSERT_EQ(decode_endpoint_state(
                      WireRoute::Signaling,
                      endpoint_state("failed", "\"eventSeq\":3,",
                                     std::string(",\"reason\":\"") + reason + "\""),
                      fact),
                  WireResult::Ok);
        EXPECT_EQ(fact.reason, reason);
    }
}

// 非法revision、自由reason、候选腿错位和应答/事件混用必须整体拒绝并清空输出。
TEST(EndpointWire, RejectsIllegalStateAndWrongForms) {
    const std::array<std::pair<std::string, std::string>, 17> changes = {
        {{R"("committed")", R"("pending")"},
         {R"("revision":1)", R"("revision":null)"},
         {R"("revision":1)", R"("revision":-1)"},
         {R"("revision":1)", R"("revision":9007199254740992)"},
         {R"("revision":1)", R"("revision":"1")"},
         {R"("revision":1)", R"("revision":1,"revision":2)"},
         {R"("handoff-1")", R"("")"},
         {R"("handoff-1")", R"("handoff-1","extra":1)"},
         {R"("code":200)", R"("code":202)"},
         {R"("code":200)", R"("code":409)"},
         {R"("code":200)", R"("code":200,"eventSeq":1)"},
         {R"("code":200)", R"("code":200,"messageSeq":1)"},
         {R"("code":200,)", {}},
         {R"(,"candidateSessionId":"s18-new")", {}},
         {R"("s18-new")", R"("s18-new","reason":"userCancelled")"},
         {R"("data")", R"("params")"},
         {R"("endpointState")", R"("focusState")"}}};
    for (const auto& change : changes) {
        EndpointState state;
        state.state = "old";
        state.handoff_id = "old";
        state.revision = 7;
        state.candidate_session_id = "old";
        state.reason = "old";
        EXPECT_EQ(decode_endpoint_state(
                      WireRoute::Signaling,
                      modify(endpoint_state("committed", "\"code\":200,",
                                            R"(,"candidateSessionId":"s18-new")"),
                             change.first, change.second),
                      state),
                  WireResult::Invalid)
            << change.first << " -> " << change.second;
        EXPECT_TRUE(state.handoff_id.empty());
        EXPECT_TRUE(state.candidate_session_id.empty());
        EXPECT_TRUE(state.reason.empty());
    }
    const std::array<std::pair<std::string, std::string>, 8> failed_changes = {
        {{R"("handoffTimeout")", R"("timeout")"},
         {R"("handoffTimeout")", R"("handoffTimeouts")"},
         {R"("handoffTimeout")", R"("")"},
         {R"(,"reason":"handoffTimeout")", ""},
         {R"("eventSeq":3)", R"("code":200)"},
         {R"(,"reason":"handoffTimeout")",
          R"(,"candidateSessionId":"s18-new")"},
         {R"("handoffTimeout")", R"("handoffTimeout","extra":1)"},
         {R"("failed")", R"("aborted","reason":"userCancelled","reason":"userCancelled")"}}};
    for (const auto& change : failed_changes) {
        EndpointState state;
        state.handoff_id = "old";
        EXPECT_EQ(decode_endpoint_state(
                      WireRoute::Signaling,
                      modify(endpoint_state("failed", "\"eventSeq\":3,",
                                            R"(,"reason":"handoffTimeout")"),
                             change.first, change.second),
                      state),
                  WireResult::Invalid);
        EXPECT_TRUE(state.handoff_id.empty());
    }
    for (const auto& body : {std::string(R"(,"reason":"userCancelled")"),
                             std::string(R"(,"reason":"denied")"),
                             std::string(R"(,"candidateSessionId":"s18-new")")}) {
        EndpointState state;
        EXPECT_EQ(decode_endpoint_state(WireRoute::Signaling,
                                        endpoint_state("aborted", "\"code\":200,", body), state),
                  body == R"(,"reason":"userCancelled")" ? WireResult::Ok : WireResult::Invalid);
    }
}
