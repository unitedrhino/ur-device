// 文件说明：真实Call媒体适配与唯一焦点驱动的离线合同；不冒浏览器硬件或跨进程系统焦点验收。
#include <gtest/gtest.h>
#include "browser_call_media.hpp"
#include "browser_call_wire.hpp"

namespace ur::device {
namespace {
// applied 原命令只转换固定局部回执，测试必须显式提交而不是把命令排队当完成。
std::string applied(const std::string& command) {
    // 输入仅来自真实适配的固定命令，替换固定kind并附加唯一显式实际字段。
    const auto output = command.find("\"kind\":\"callOutputGate\"");
    const auto hold = command.find("\"kind\":\"callHoldGate\"");
    if (command.empty() || command.back() != '}' ||
        (output == std::string::npos && hold == std::string::npos))
        return {};
    std::string body = command;
    if (output != std::string::npos)
        body.replace(output, std::string_view("\"kind\":\"callOutputGate\"").size(),
                     "\"kind\":\"callOutputApplied\"");
    else
        body.replace(hold, std::string_view("\"kind\":\"callHoldGate\"").size(),
                     "\"kind\":\"callHoldApplied\"");
    body.pop_back();
    body += ",\"applied\":true}";
    return body;
}
// CallFocusFixture 真实Driver/Authority/Media通过显式原回执组合，不伪造provider或MQTT。
class CallFocusFixture : public ::testing::Test {
protected:
    BrowserCallMedia media;  // 实际端口的命令与回执在同一原事件线程处理。
    OutputFocusDriver driver{
        {{"", "", {"speaker", "display"}, &media}}, 0, FocusBindingMode::DeferredReleased};
    bool established = true;  // 测试的可信核心建立事实，网页不能修改此许可。
    // scope_override 仅回归当前原业务资格撤销；正常用例仍从真实Media读取原Peer事实。
    std::optional<BrowserCallFocusScope> scope_override;
    BrowserCallFocusAuthority authority{driver, 100000, [&]() {
                                            auto scope = scope_override ? scope_override
                                                                        : media.focus_scope();
                                            if (scope)
                                                scope->established = established;
                                            return scope;
                                        }};
    std::optional<std::string> peer_refresh;  // 保留新Peer唯一原false票据，测试不能自行构造新代次。
    // pending_connected 复现实际Connected先到的合法排序，初始基线已确认但新Peer刷新未完成。
    void pending_connected() {
        ASSERT_TRUE(media.enable_hold(driver, authority, 100000));
        ASSERT_TRUE(driver.initialize(1));
        ASSERT_TRUE(media.poll_control(1, 1000000));
        auto command = media.take_command();
        ASSERT_TRUE(command);
        ASSERT_TRUE(media.receive_control(applied(*command), 2));
        ASSERT_TRUE(media.poll_control(2, 1000001));
        ASSERT_TRUE(media.begin());
        ASSERT_TRUE(media.take_command());
        peer_refresh = media.take_command();
        ASSERT_TRUE(peer_refresh);
        ASSERT_TRUE(media.apply_answer("v=0\r\no=server\r\n"));
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.report_connected());
    }
    // acquiring
    // 保留真实acquire原票据，随后由用例独立推进两份毫秒时钟并提交实际回执。
    void acquiring() {
        ASSERT_NO_FATAL_FAILURE(pending_connected());
        ASSERT_TRUE(media.receive_control(applied(*peer_refresh), 3));
        ASSERT_TRUE(media.poll_control(3, 1000002));
        ASSERT_TRUE(media.bind_control_scope("original-leg"));
        ASSERT_TRUE(media.acquire_output(3));
    }
    // ready 显式收齐原物理静音，再创建Peer并刷新新代次输出基线。
    void ready() {
        ASSERT_TRUE(media.enable_hold(driver, authority, 100000));
        ASSERT_TRUE(driver.initialize(1));
        ASSERT_TRUE(media.poll_control(1, 1000000));
        auto command = media.take_command();
        ASSERT_TRUE(command);
        ASSERT_TRUE(media.receive_control(applied(*command), 2));
        ASSERT_TRUE(media.poll_control(2, 1000001));
        ASSERT_TRUE(driver.initial_idle());
        ASSERT_TRUE(media.begin());
        command = media.take_command();
        ASSERT_TRUE(command);
        EXPECT_NE(command->find("callConfigure"), std::string::npos);
        command = media.take_command();
        ASSERT_TRUE(command);
        ASSERT_TRUE(media.receive_control(applied(*command), 3));
        ASSERT_TRUE(media.poll_control(3, 1000002));
        ASSERT_TRUE(media.apply_answer("v=0\r\no=server\r\n"));
        ASSERT_TRUE(media.take_command());
        ASSERT_TRUE(media.report_connected());
        ASSERT_TRUE(media.bind_control_scope("original-leg"));
        ASSERT_TRUE(media.acquire_output(3));
        command = media.take_command();
        ASSERT_TRUE(command);
        EXPECT_FALSE(media.output_ready());
        ASSERT_TRUE(media.receive_control(applied(*command), 4));
        ASSERT_TRUE(media.poll_control(4, 1000003));
        ASSERT_TRUE(media.output_ready());
        EXPECT_EQ(driver.revision(), 1U);
    }
    // restoring
    // 先收齐真实保持的两份票据，再在原腿创建唯一恢复请求，不制造完成事实。
    void restoring() {
        ASSERT_NO_FATAL_FAILURE(ready());
        const auto ticket = media.request_hold(true, "hold", 1, 4);
        ASSERT_TRUE(ticket);
        const auto output = media.take_command(), upstream = media.take_command();
        ASSERT_TRUE(output);
        ASSERT_TRUE(upstream);
        ASSERT_TRUE(media.receive_control(applied(*output), 5));
        ASSERT_TRUE(media.receive_control(applied(*upstream), 5));
        ASSERT_TRUE(media.poll_control(5, 1000004));
        ASSERT_EQ(media.hold_result(*ticket, true), std::optional<bool>(true));
        ASSERT_TRUE(media.poll_control(6, 1000005));
        ASSERT_TRUE(media.request_hold(false, "resume", 2, 6));
    }
    // permit_command 只给独立Authority校验使用原已绑定腿，不执行新的媒体操作。
    FocusCommand permit_command() const {
        FocusCommand command;  // 全部公开固定字段必须随签发许可保持原样。
        command.envelope.session = "original-leg";
        command.envelope.token = "permit-check";
        command.envelope.method = "focusControl";
        command.envelope.request = true;
        command.operation = "acquire";
        command.expected_revision = driver.revision();
        command.focus_id = "call-output-1";
        command.resources = {"speaker", "display"};
        command.priority = "call";
        command.resume_policy = "explicit";
        command.expires_at = 1000013;
        return command;
    }
};
}  // namespace

// 单调钟前进而墙钟仍在同一毫秒时，原acquire没有过期，不能用剩余长度比较撤销许可。
TEST_F(CallFocusFixture, AcquireAllowsMonotonicAdvanceWithoutWallAdvance) {
    ASSERT_NO_FATAL_FAILURE(acquiring());
    const auto output = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(media.receive_control(applied(*output), 4));
    ASSERT_TRUE(media.poll_control(4, 1000002));
    EXPECT_TRUE(media.output_ready());
    EXPECT_EQ(driver.revision(), 1U);
}

// 墙钟前进而单调钟仍在同一毫秒时，原两份绝对截止都未到期，acquire仍需真实票据。
TEST_F(CallFocusFixture, AcquireAllowsWallAdvanceWithoutMonotonicAdvance) {
    ASSERT_NO_FATAL_FAILURE(acquiring());
    const auto output = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(media.receive_control(applied(*output), 3));
    ASSERT_TRUE(media.poll_control(3, 1000003));
    EXPECT_TRUE(media.output_ready());
    EXPECT_EQ(driver.revision(), 1U);
}

// 恢复沿原腿、原用户意图和两份真实票据，不能因单调钟独立前进一毫秒变成失败。
TEST_F(CallFocusFixture, RestoreAllowsMonotonicAdvanceWithoutWallAdvance) {
    ASSERT_NO_FATAL_FAILURE(restoring());
    const auto output = media.take_command(), upstream = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(upstream);
    ASSERT_TRUE(media.receive_control(applied(*output), 7));
    ASSERT_TRUE(media.receive_control(applied(*upstream), 7));
    ASSERT_TRUE(media.poll_control(7, 1000005));
    EXPECT_EQ(media.hold_result(2, false), std::optional<bool>(true));
    EXPECT_TRUE(media.output_ready());
    EXPECT_EQ(driver.revision(), 3U);
}

// 恢复的墙钟可以独立前进；实际完成仍由原两份票据和唯一CAS共同证明。
TEST_F(CallFocusFixture, RestoreAllowsWallAdvanceWithoutMonotonicAdvance) {
    ASSERT_NO_FATAL_FAILURE(restoring());
    const auto output = media.take_command(), upstream = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(upstream);
    ASSERT_TRUE(media.receive_control(applied(*output), 6));
    ASSERT_TRUE(media.receive_control(applied(*upstream), 6));
    ASSERT_TRUE(media.poll_control(6, 1000006));
    EXPECT_EQ(media.hold_result(2, false), std::optional<bool>(true));
    EXPECT_TRUE(media.output_ready());
    EXPECT_EQ(driver.revision(), 3U);
}

// 任一原绝对截止到达都拒绝许可，另一份时钟未前进不能把到期解释为新的授权。
TEST_F(CallFocusFixture, EitherAbsolutePermitDeadlineRejectsWithoutRenewal) {
    ASSERT_NO_FATAL_FAILURE(ready());
    FocusCommand command;  // 仅使用原已绑定腿和固定完整资源，不执行新的输出或网络请求。
    command.envelope.session = "original-leg";
    command.envelope.token = "deadline-check";
    command.envelope.method = "focusControl";
    command.envelope.request = true;
    command.operation = "acquire";
    command.expected_revision = driver.revision();
    command.focus_id = "call-output-1";
    command.resources = {"speaker", "display"};
    command.priority = "call";
    command.resume_policy = "explicit";
    command.expires_at = 1000013;
    const auto permit = authority.authorize(command, 4, 1000003);
    ASSERT_TRUE(permit);
    ASSERT_EQ(permit->deadline, 14U);
    EXPECT_TRUE(authority.valid(*permit, 13, 1000012));
    EXPECT_FALSE(authority.valid(*permit, 14, 1000012));
    EXPECT_FALSE(authority.valid(*permit, 13, 1000013));
    EXPECT_FALSE(authority.valid(*permit, 100000, 1000012));
}

// 原签发的每个信封、操作和期限字段都不可被调用者换成新的许可，包括缩短期限。
TEST_F(CallFocusFixture, EverySingleIssuedPermitFieldRejectsTampering) {
    ASSERT_NO_FATAL_FAILURE(ready());
    const auto permit = authority.authorize(permit_command(), 4, 1000003);
    ASSERT_TRUE(permit);
    ASSERT_TRUE(authority.valid(*permit, 4, 1000003));
    // mutations 每次只改一个字段，覆盖完整Envelope、FocusCommand及Permit两份固定水位。
    const std::vector<std::function<void(FocusPermit&)>> mutations{
        [](FocusPermit& value) { value.scope.envelope.token += "-changed"; },
        [](FocusPermit& value) { value.scope.envelope.session += "-changed"; },
        [](FocusPermit& value) { value.scope.envelope.method = "other"; },
        [](FocusPermit& value) { value.scope.envelope.request = false; },
        [](FocusPermit& value) { value.scope.envelope.payload = "{\"changed\":true}"; },
        [](FocusPermit& value) { value.scope.envelope.code = 409; },
        [](FocusPermit& value) { value.scope.envelope.event_sequence = 1; },
        [](FocusPermit& value) { value.scope.envelope.message_sequence = 1; },
        [](FocusPermit& value) { value.scope.operation = "restore"; },
        [](FocusPermit& value) { ++value.scope.expected_revision; },
        [](FocusPermit& value) { value.scope.focus_id += "-changed"; },
        [](FocusPermit& value) { value.scope.resources.pop_back(); },
        [](FocusPermit& value) { value.scope.priority = "emergency"; },
        [](FocusPermit& value) { value.scope.resume_policy = "never"; },
        [](FocusPermit& value) { ++*value.scope.expires_at; },
        [](FocusPermit& value) { ++value.deadline; },
        [](FocusPermit& value) { --value.deadline; },
        [](FocusPermit& value) { ++value.intent_version; },
    };
    for (std::size_t index = 0; index < mutations.size(); ++index) {
        SCOPED_TRACE(index);
        auto changed = *permit;  // 只篡改独立副本，不替换Authority持有的原签发记录。
        mutations[index](changed);
        EXPECT_FALSE(authority.valid(changed, 4, 1000003));
        EXPECT_TRUE(authority.valid(*permit, 4, 1000003));
    }
}

// 两份期限同时加长也不可绕过原签发记录，不能把与宿主上界一致当成新授权。
TEST_F(CallFocusFixture, CoupledExpiryAndMonotonicDeadlineCannotExtendIssuedPermit) {
    ASSERT_NO_FATAL_FAILURE(ready());
    const auto permit = authority.authorize(permit_command(), 4, 1000003);
    ASSERT_TRUE(permit);
    auto changed = *permit;  // 保留在宿主上界内仍不得续原许可。
    ++*changed.scope.expires_at;
    ++changed.deadline;
    EXPECT_FALSE(authority.valid(changed, 4, 1000003));
    EXPECT_FALSE(authority.valid(changed, 14, 1000013));
}

// 仅一个最近许可可用，新原请求签发后旧token不能继续拿此前能力当当前意图。
TEST_F(CallFocusFixture, NewlyIssuedPermitInvalidatesPreviousPermit) {
    ASSERT_NO_FATAL_FAILURE(ready());
    auto command = permit_command();  // 新请求保留原腿和资源，但必须使用不同原token。
    const auto previous = authority.authorize(command, 4, 1000003);
    ASSERT_TRUE(previous);
    command.envelope.token = "next-permit";
    const auto current = authority.authorize(command, 4, 1000003);
    ASSERT_TRUE(current);
    EXPECT_FALSE(authority.valid(*previous, 4, 1000003));
    EXPECT_TRUE(authority.valid(*current, 4, 1000003));
}

// 新签发仍严格检查TTL上界；授权失败不清除或替换最近一份原许可。
TEST_F(CallFocusFixture, FailedIssuancePreservesPermitAndHostTTLBound) {
    ASSERT_NO_FATAL_FAILURE(ready());
    auto command = permit_command();  // 在原宿主范围内的短许可用于验证失败不覆盖。
    const auto permit = authority.authorize(command, 4, 1000003);
    ASSERT_TRUE(permit);
    command.envelope.token = "denied-permit";
    command.expires_at = 1100000;  // 剩余TTL99997大于宿主剩余99996。
    EXPECT_FALSE(authority.authorize(command, 4, 1000003));
    EXPECT_TRUE(authority.valid(*permit, 4, 1000003));
    established = false;
    command.expires_at = 1000013;
    EXPECT_FALSE(authority.authorize(command, 4, 1000003));
    established = true;
    EXPECT_TRUE(authority.valid(*permit, 4, 1000003));
    command.expires_at = 1099999;  // TTL等于原宿主剩余时可以签发，但到点必须拒绝。
    const auto boundary = authority.authorize(command, 4, 1000003);
    ASSERT_TRUE(boundary);
    EXPECT_EQ(boundary->deadline, 100000U);
    EXPECT_FALSE(authority.valid(*boundary, 100000, 1000003));
    EXPECT_FALSE(authority.valid(*boundary, 4, 1099999));
}

// 恢复在途必须持续匹配原显式意图、控制token/修订与Peer，双绝对期限不放宽业务资格。
TEST_F(CallFocusFixture, RestorePermitRejectsRevokedOriginalIntentAndPeer) {
    ASSERT_NO_FATAL_FAILURE(restoring());
    auto command = permit_command();  // 仅校验真实恢复在途的原业务资格，不提交额外媒体命令。
    command.operation = "restore";
    command.envelope.token = "resume";
    command.expires_at = 1000015;
    const auto permit = authority.authorize(command, 6, 1000005);
    ASSERT_TRUE(permit);
    ASSERT_TRUE(authority.valid(*permit, 6, 1000005));
    const auto original = media.focus_scope();
    ASSERT_TRUE(original);
    scope_override = original;
    scope_override->explicit_restore = false;
    EXPECT_FALSE(authority.valid(*permit, 6, 1000005));
    scope_override = original;
    scope_override->control_token = "other";
    EXPECT_FALSE(authority.valid(*permit, 6, 1000005));
    scope_override = original;
    scope_override->control_revision = 0;
    EXPECT_FALSE(authority.valid(*permit, 6, 1000005));
    scope_override = original;
    ++scope_override->peer_epoch;
    EXPECT_FALSE(authority.valid(*permit, 6, 1000005));
    scope_override = original;
    established = false;
    EXPECT_FALSE(authority.valid(*permit, 6, 1000005));
}

// 两条实际回执任意顺序均需齐全，只推进一次call保持结果与独立输出CAS。
TEST_F(CallFocusFixture, HoldAndExplicitResumeRequireBothOriginalActualReceipts) {
    ASSERT_NO_FATAL_FAILURE(ready());
    for (const bool held : {true, false}) {
        const auto now = held ? 5U : 9U;
        ASSERT_TRUE(media.poll_control(now, 1000000 + now));
        const auto ticket =
            media.request_hold(held, held ? "original-hold" : "original-resume", held ? 1 : 2, now);
        ASSERT_TRUE(ticket);
        EXPECT_FALSE(media.hold_result(*ticket, held).has_value());
        auto output = media.take_command(), upstream = media.take_command();
        ASSERT_TRUE(output);
        ASSERT_TRUE(upstream);
        if (held) {
            ASSERT_TRUE(media.receive_control(applied(*output), now + 1));
            ASSERT_TRUE(media.poll_control(now + 1, 1000000 + now + 1));
            EXPECT_FALSE(media.hold_result(*ticket, held).has_value());
            ASSERT_TRUE(media.receive_control(applied(*upstream), now + 2));
        } else {
            ASSERT_TRUE(media.receive_control(applied(*upstream), now + 1));
            ASSERT_TRUE(media.poll_control(now + 1, 1000000 + now + 1));
            EXPECT_FALSE(media.hold_result(*ticket, held).has_value());
            ASSERT_TRUE(media.receive_control(applied(*output), now + 2));
        }
        ASSERT_TRUE(media.poll_control(now + 2, 1000000 + now + 2));
        ASSERT_TRUE(media.hold_result(*ticket, held).has_value());
        EXPECT_TRUE(*media.hold_result(*ticket, held));
        EXPECT_EQ(media.output_ready(), !held);
        EXPECT_EQ(driver.revision(), held ? 2U : 3U);
        EXPECT_FALSE(media.receive_control(applied(*upstream), now + 3));
        EXPECT_FALSE(media.receive_control(applied(*output), now + 3));
        EXPECT_FALSE(media.take_command());
    }
}

// 任一实际证明缺失，两秒截止都不能将未知改为成功或以旧票据再次打开。
TEST_F(CallFocusFixture, MissingActualReceiptExpiresWithoutReplaying) {
    ASSERT_NO_FATAL_FAILURE(ready());
    const auto ticket = media.request_hold(true, "hold", 1, 4);
    ASSERT_TRUE(ticket);
    auto output = media.take_command(), upstream = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(upstream);
    ASSERT_TRUE(media.receive_control(applied(*output), 5));
    ASSERT_TRUE(media.poll_control(5, 1000004));
    EXPECT_FALSE(media.hold_result(*ticket, true).has_value());
    EXPECT_FALSE(media.poll_control(2004, 1002003));
    ASSERT_TRUE(media.hold_result(*ticket, true).has_value());
    EXPECT_FALSE(*media.hold_result(*ticket, true));
    EXPECT_FALSE(media.receive_control(applied(*upstream), 2005));
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.request_hold(false, "resume", 2, 2005));
}

// 上行停止成功也不能替代原输出音频/画面票据，缺失整组实态保持未知而非已释放。
TEST_F(CallFocusFixture, MissingOutputReceiptNeverCompletesFromUpstreamAlone) {
    ASSERT_NO_FATAL_FAILURE(ready());
    const auto ticket = media.request_hold(true, "hold", 1, 4);
    ASSERT_TRUE(ticket);
    auto output = media.take_command(), upstream = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(upstream);
    ASSERT_TRUE(media.receive_control(applied(*upstream), 5));
    ASSERT_TRUE(media.poll_control(5, 1000004));
    EXPECT_FALSE(media.hold_result(*ticket, true).has_value());
    EXPECT_FALSE(media.poll_control(2004, 1002003));
    ASSERT_TRUE(media.hold_result(*ticket, true).has_value());
    EXPECT_FALSE(*media.hold_result(*ticket, true));
    EXPECT_FALSE(media.receive_control(applied(*output), 2005));
    EXPECT_FALSE(media.output_ready());
}

// 恢复必须获得当前原独立Call许可，平台active不能借前一次acquire许可重新打开。
TEST_F(CallFocusFixture, RevokedEstablishedScopeRefusesExplicitRestore) {
    ASSERT_NO_FATAL_FAILURE(ready());
    const auto ticket = media.request_hold(true, "hold", 1, 4);
    ASSERT_TRUE(ticket);
    auto output = media.take_command(), upstream = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(upstream);
    ASSERT_TRUE(media.receive_control(applied(*output), 5));
    ASSERT_TRUE(media.receive_control(applied(*upstream), 5));
    ASSERT_TRUE(media.poll_control(5, 1000004));
    ASSERT_TRUE(media.hold_result(*ticket, true).has_value());
    established = false;
    EXPECT_FALSE(media.request_hold(false, "resume", 2, 5));
    EXPECT_FALSE(media.take_command());
    EXPECT_FALSE(media.output_ready());
    EXPECT_EQ(driver.revision(), 2U);
}

// 结束Peer只请求停止，原显式release票据实际收录之前不能把下一腿绑在旧资源上。
TEST_F(CallFocusFixture, ClosedPeerWaitsForOriginalReleaseBeforeNewBinding) {
    ASSERT_NO_FATAL_FAILURE(ready());
    media.close();
    EXPECT_FALSE(media.available());
    EXPECT_FALSE(media.prepare());
    EXPECT_FALSE(media.connected());
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.begin());
    auto stop = media.take_command();
    ASSERT_TRUE(stop);
    EXPECT_NE(stop->find("callStop"), std::string::npos);
    auto output = media.take_command();
    ASSERT_TRUE(output);
    ASSERT_TRUE(media.receive_control(applied(*output), 5));
    ASSERT_TRUE(media.poll_control(5, 1000004));
    EXPECT_EQ(driver.revision(), 2U);
    EXPECT_TRUE(media.available());
    EXPECT_TRUE(media.begin());
    EXPECT_EQ(media.peer_epoch(), 2U);
    EXPECT_FALSE(media.receive_control(applied(*output), 6));
    EXPECT_FALSE(media.output_ready());
}

// 许可撤回或同session新Peer都不能借平台active取得本端输出，缺初始实际静音不启动Peer。
TEST_F(CallFocusFixture, RevokedScopeAndUninitializedOutputCannotGrantPlayback) {
    ASSERT_TRUE(media.enable_hold(driver, authority, 100000));
    EXPECT_FALSE(media.begin());
    EXPECT_FALSE(media.bind_control_scope("original-leg"));
    EXPECT_FALSE(media.acquire_output(1));
    EXPECT_FALSE(media.take_command());
}

// 合法Connected可以先于新Peer静音回执；原宿主不能把两秒内的刷新在途当永久失败。
TEST_F(CallFocusFixture, ConnectedBeforePeerRefreshWaitsWithoutAcquiringOutput) {
    ASSERT_NO_FATAL_FAILURE(pending_connected());
    ASSERT_TRUE(media.poll_control(3, 1000002));
    EXPECT_EQ(media.output_readiness(), CallOutputReadiness::Pending);
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.take_command());
    EXPECT_EQ(driver.revision(), 0U);
    // 原截止前收录当前false票据，才允许绑定和开始唯一acquire；排队本身仍不是放音。
    ASSERT_TRUE(media.receive_control(applied(*peer_refresh), 2001));
    ASSERT_TRUE(media.poll_control(2001, 1002000));
    ASSERT_EQ(media.output_readiness(), CallOutputReadiness::Ready);
    ASSERT_TRUE(media.bind_control_scope("original-leg"));
    ASSERT_TRUE(media.acquire_output(2001));
    const auto command = media.take_command();
    ASSERT_TRUE(command);
    EXPECT_NE(command->find("\"enabled\":true"), std::string::npos);
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.take_command());
    ASSERT_TRUE(media.receive_control(applied(*command), 2002));
    ASSERT_TRUE(media.poll_control(2002, 1002001));
    EXPECT_TRUE(media.output_ready());
    EXPECT_EQ(driver.revision(), 1U);
}

// 刷新缺失保持失败；不能借Connected重置两秒截止或补发acquire。
TEST_F(CallFocusFixture, ConnectedPeerRefreshExpiryNeverGrantsOutput) {
    ASSERT_NO_FATAL_FAILURE(pending_connected());
    ASSERT_EQ(media.output_readiness(), CallOutputReadiness::Pending);
    ASSERT_TRUE(media.poll_control(2001, 1002000));
    EXPECT_EQ(media.output_readiness(), CallOutputReadiness::Pending);
    EXPECT_FALSE(media.poll_control(2002, 1002001));
    EXPECT_EQ(media.output_readiness(), CallOutputReadiness::Failed);
    EXPECT_FALSE(media.receive_control(applied(*peer_refresh), 2002));
    EXPECT_FALSE(media.bind_control_scope("original-leg"));
    EXPECT_FALSE(media.acquire_output(2002));
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.take_command());
    EXPECT_EQ(driver.revision(), 0U);
}

// 原网页明确applied=false是失败，不将失败静音或当前Connected枚举当刷新成功。
TEST_F(CallFocusFixture, ConnectedPeerRefreshRejectedNeverGrantsOutput) {
    ASSERT_NO_FATAL_FAILURE(pending_connected());
    auto rejection = applied(*peer_refresh);  // 仅改原实态结果，全部原票据关联仍沿真实适配命令。
    const auto offset = rejection.find("\"applied\":true");
    ASSERT_NE(offset, std::string::npos);
    rejection.replace(offset, std::string_view("\"applied\":true").size(), "\"applied\":false");
    ASSERT_TRUE(media.receive_control(rejection, 3));
    EXPECT_FALSE(media.poll_control(3, 1000002));
    EXPECT_EQ(media.output_readiness(), CallOutputReadiness::Failed);
    EXPECT_FALSE(media.bind_control_scope("original-leg"));
    EXPECT_FALSE(media.acquire_output(3));
    EXPECT_FALSE(media.output_ready());
    EXPECT_FALSE(media.take_command());
    EXPECT_EQ(driver.revision(), 0U);
}

// 本地控制严格固定操作，端不能自授平台token/revision或传输任意method。
TEST(BrowserCallControlWire, FixedIntentAndActualProjectionRejectExtraIdentity) {
    CallBrowserRequest out;
    EXPECT_TRUE(decode_call_browser(R"({"kind":"callControl","operation":"hold"})", out));
    EXPECT_EQ(out.operation, "hold");
    EXPECT_FALSE(
        decode_call_browser(R"({"kind":"callControl","operation":"hold","token":"self"})", out));
    EXPECT_FALSE(decode_call_browser(R"({"kind":"callControl","operation":"transfer"})", out));
    EXPECT_TRUE(decode_call_browser(
        R"({"kind":"callOutputApplied","sessionId":"","peerEpoch":"0","gateTicket":"1","enabled":false,"applied":true})",
        out));
    EXPECT_FALSE(decode_call_browser(
        R"({"kind":"callOutputApplied","sessionId":"","peerEpoch":"0","gateTicket":"1","enabled":false,"applied":true,"credential":"private"})",
        out));
}
}  // namespace ur::device
