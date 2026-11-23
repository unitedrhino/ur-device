// 环境适配回归：不读取设备凭据，只修改测试进程自己的固定隔离变量并恢复原值。
#include <gtest/gtest.h>
#include "environment.hpp"

namespace ur::device {
namespace {
// ScopedEnvironment保存并恢复name；set仅更改当前测试进程，不影响宿主或用户配置。
class ScopedEnvironment {
public:
    // 构造时保存name对应的独立副本，后续写入不影响原值。
    explicit ScopedEnvironment(const char* name)
        : name_(name), original_(environment_value(name)) {}
    // 退出测试作用域时恢复原值，不修改父进程环境。
    ~ScopedEnvironment() { set(original_); }
    // set写入value或删除缺失值，返回当前进程的平台操作是否成功。
    bool set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        return _putenv_s(name_, value ? value->c_str() : "") == 0;
#else
        return (value ? setenv(name_, value->c_str(), 1) : unsetenv(name_)) == 0;
#endif
    }

private:
    const char* name_;                     // 本轮独占测试变量名。
    std::optional<std::string> original_;  // 调用者进程中的原始值，销毁时恢复。
};
// 非法名称在调用CRT前拒绝，避免Windows无效参数终止进程。
TEST(DesktopEnvironment, RejectsInvalidNames) {
    EXPECT_FALSE(environment_value(nullptr));
    EXPECT_FALSE(environment_value(""));
    EXPECT_FALSE(environment_value("invalid=name"));
}
// 能力开关省略即关闭；非0/1值拒绝，不能把误配置转为采集授权。
TEST(DesktopEnvironment, ExplicitCapabilityFlagDoesNotGuessPermission) {
    const ScopedEnvironment variable("UR_RUNTIME_TEST_ENV_FLAG");  // 不读写真实设备能力配置。
    ASSERT_TRUE(variable.set(std::nullopt));
    EXPECT_EQ(environment_flag("UR_RUNTIME_TEST_ENV_FLAG"), std::optional<bool>(false));
    for (const auto* value : {"0", "1"}) {
        ASSERT_TRUE(variable.set(std::string(value)));
        EXPECT_EQ(environment_flag("UR_RUNTIME_TEST_ENV_FLAG"),
                  std::optional<bool>(std::string(value) == "1"));
    }
    for (const auto* value : {"true", "false", "2", " 1", "1 ", "01"}) {
        ASSERT_TRUE(variable.set(std::string(value)));
        EXPECT_FALSE(environment_flag("UR_RUNTIME_TEST_ENV_FLAG"));
    }
    EXPECT_FALSE(environment_flag(nullptr));
    EXPECT_FALSE(environment_flag(""));
    EXPECT_FALSE(environment_flag("invalid=name"));
}

// 缺失值与独立副本生命周期均明确，不借用被后续覆写的环境内存。
TEST(DesktopEnvironment, MissingValueAndIndependentCopy) {
    const ScopedEnvironment variable("UR_RUNTIME_TEST_ENV_VALUE");  // 不使用真实业务变量。
    ASSERT_TRUE(variable.set(std::nullopt));
    EXPECT_FALSE(environment_value("UR_RUNTIME_TEST_ENV_VALUE"));
    ASSERT_TRUE(variable.set(std::string("first value")));
    const auto first =
        environment_value("UR_RUNTIME_TEST_ENV_VALUE");  // 不能指向随后被平台覆写的内存。
    ASSERT_EQ(first, std::optional<std::string>("first value"));
    ASSERT_TRUE(variable.set(std::string("second value")));
    EXPECT_EQ(environment_value("UR_RUNTIME_TEST_ENV_VALUE"),
              std::optional<std::string>("second value"));
    EXPECT_EQ(first, std::optional<std::string>("first value"));
}
}  // namespace
}  // namespace ur::device
