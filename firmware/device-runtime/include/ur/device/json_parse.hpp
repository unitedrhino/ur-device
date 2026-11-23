// 共享JSON解析入口：只保护cJSON内部全局解析错误状态，不暴露第三方头或锁住树/业务操作。
#pragma once
#include <cstddef>

struct cJSON; // 解析树沿用cJSON所有权；调用方仍负责释放。

namespace ur::device {
// parse_json 等价于cJSON_Parse；bytes可为空，返回解析树或nullptr，调用方拥有返回树。
// 自定义cJSON分配Hooks只能在所有解析线程启动前设置，线程全部退出后才可恢复。
cJSON* parse_json(const char* bytes);
// parse_json_with_length_opts 等价于cJSON_ParseWithLengthOpts，原样传递长度、结束指针及严格NUL标志。
// end指向原输入内的解析结束/错误位置，不能跨线程借用cJSON_GetErrorPtr的全局错误指针。
cJSON* parse_json_with_length_opts(const char* bytes, std::size_t length,
                                   const char** end, int require_null_terminated);
} // namespace ur::device
