// 文件说明：受限历史宿主的可信清单与能力编码；不从网页/MQTT读取宿主路径或设备身份。
#pragma once
#include <map>
#include "browser_record_playback.hpp"

namespace ur::device {
// RecordHostConfiguration
// 是部署者显式启用的一份有限档案源，缺配置默认不可运行。
struct RecordHostConfiguration {
    BrowserRecordAsset asset;    // 原档案四元组、实际时长、格式和有界字节预算。
    std::string file;            // 仅本机可信配置中的绝对路径，不进入网页或设备报文。
    std::uint64_t lease_ms = 0;  // 每个原实例的有限本地授权，邀请重试不能延长。
};
// record_host_configuration
// 严格读取原UR_RECORD_*配置，非法／缺失返回空且不输出原值。
std::optional<RecordHostConfiguration> record_host_configuration(
    const std::map<std::string, std::string>& values);
// read_record_archive 仅加载固定绝对文件；超预算／读取失败返回false并清空data。
bool read_record_archive(const RecordHostConfiguration& config, std::string& data);
// record_host_capability
// 编码实际固定档案能力；调用者必须先取得浏览器真实格式探针。
WireResult record_host_capability(std::string token, const BrowserRecordAsset& asset,
                                  Envelope& out);
// record_host_local_request
// 仅识别严格ready/shutdown，不把网页指令直接变成平台播放控制。
bool record_host_local_request(std::string_view body, std::string& kind);
// record_host_projection
// 投影原核心阶段和已确认Actual，不夹带凭据、目录或原token。
std::string record_host_projection(const RecordPlaybackClient& client);
}  // namespace ur::device
