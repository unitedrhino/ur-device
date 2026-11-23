// cJSON共享短解析门：每次仅持锁调用原解析器，网络、归一化、树访问与事件等待均在门外。
#include "ur/device/json_parse.hpp"
#include <cJSON.h>
#include <mutex>

namespace ur::device {
namespace {
// parse_gate 统一保护所有生产解析入口写入的cJSON global_error，不改变第三方库或分配器。
std::mutex parse_gate;
} // namespace

// parse_json 仅串行化原解析器；返回树交调用方持有，后续访问不占用解析门。
cJSON* parse_json(const char* bytes) {
    const std::lock_guard<std::mutex> lock(parse_gate);
    return cJSON_Parse(bytes);
}

// parse_json_with_length_opts 原样保留长度、结束位置和严格终止合同，不替调用方补NUL或过滤尾部。
cJSON* parse_json_with_length_opts(const char* bytes, std::size_t length,
                                   const char** end, int require_null_terminated) {
    const std::lock_guard<std::mutex> lock(parse_gate);
    return cJSON_ParseWithLengthOpts(bytes, length, end, require_null_terminated);
}
} // namespace ur::device
