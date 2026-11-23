// 桌面环境读取适配：立即复制进程环境值，Windows使用安全CRT，不把平台头文件带入核心。
#pragma once
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace ur::device {
// environment_value读取name；缺失、非法名称或平台错误返回空optional，存在时返回独立字符串副本。
inline std::optional<std::string> environment_value(const char* name) {
    if (!name || !*name || std::string(name).find('=') != std::string::npos)
        return std::nullopt;
#ifdef _WIN32
    char* buffer = nullptr;  // 安全CRT分配的短期副本，交给同一CRT释放。
    std::size_t length = 0;  // 平台返回的含终止符长度。
    const auto result = _dupenv_s(&buffer, &length, name);  // 不使用Windows已弃用的getenv。
    const std::unique_ptr<char, decltype(&std::free)> owned(buffer,
                                                            &std::free);  // 所有出口回收缓冲区。
    if (result != 0 || !owned)
        return std::nullopt;
    return std::string(owned.get());
#else
    const auto* value = std::getenv(name);  // Linux环境地址只在本次复制中使用，不向调用者泄漏。
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}
// environment_flag读取显式0/1能力开关；省略为关闭，非法值返回空值供宿主拒绝启动。
inline std::optional<bool> environment_flag(const char* name) {
    if (!name || !*name || std::string(name).find('=') != std::string::npos)
        return std::nullopt;
    const auto value = environment_value(name);  // 不把空字符串或拼写错误当成缺省授权。
    if (!value || *value == "0")
        return false;
    if (*value == "1")
        return true;
    return std::nullopt;
}
}  // namespace ur::device
