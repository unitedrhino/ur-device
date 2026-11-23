// 文件说明：Linux桌面动作账本适配；磁盘IO留在唯一worker，核心只读有界内存与固化事实。
#pragma once
#include <memory>
#include <string>
#include "ur/device/action_engine.hpp"

namespace ur::device {
// ActionFileLedger 将原设备身份、受理和终态追加到私有文件；不支持时显式失败，不降级内存。
class ActionFileLedger final : public ActionHistoryLedger {
public:
    ActionFileLedger(std::string path, std::string identity);
    ~ActionFileLedger() override;
    ActionFileLedger(const ActionFileLedger&) = delete;
    ActionFileLedger& operator=(const ActionFileLedger&) = delete;
    // healthy 表示启动已校验原文件且worker没有持久化错误，不代表任一在途记录已固化。
    bool healthy() const;
    bool recorded(std::string_view token) const override;
    std::optional<ActionRequest> find_request(std::string_view token) const override;
    std::optional<std::string> find_reply(std::string_view token) const override;
    bool record(const ActionRequest& request) override;
    bool record_reply(std::string_view token, std::string_view reply) override;
    ActionPersistence persistence(std::string_view token, bool terminal) const override;

private:
    struct Implementation;                  // 只在平台实现持有线程、锁和文件描述符。
    std::unique_ptr<Implementation> impl_;  // 长于ActionEngine，销毁时等待自己的worker退出。
};
// configured_action_ledger 仅显式环境路径启用；未配置保留内存语义，配置错误返回false禁止连接。
bool configured_action_ledger(const std::string& identity,
                              std::unique_ptr<ActionFileLedger>& ledger);
}  // namespace ur::device
