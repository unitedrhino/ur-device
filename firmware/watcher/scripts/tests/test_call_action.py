"""联编真实MQTT行为入队与C++桥，防止拨号参数在跨任务复制中丢失。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

# ROOT定位生产源码，回归直接抽取实际方法而非复制实现。
ROOT = Path(__file__).resolve().parents[2]


class CallActionTest(unittest.TestCase):
    def test_mqtt_queue_preserves_dial_parameters(self):
        """正常下行完整到达拨号桥，超长、非法参数及占用不产生错误拨号。"""
        device = (ROOT / 'main/unitedrhino/ur_device.c').read_text()
        bridge = (ROOT / 'main/unitedrhino/ur_bridge.cc').read_text()
        cjson = Path(os.environ.get('UR_TEST_CJSON_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))
        code = r'''
#include "cJSON.h"
#include <cassert>
#include <cstring>
#include <string>
#define pdTRUE 1
#define CONFIG_UR_AI_SIGNALING 1
#define CONFIG_UR_AI_SIGNALING_WEBRTC 1
struct ur_action_item_t {
    char token[128], action_id[32], content[129], request_id[17], params[256];
};
ur_action_item_t queued;
int queue_token = 0, sends = 0;
void *s_action_queue = &queue_token;
size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = std::strlen(src);
    if (n) {
        std::strncpy(dst, src, n - 1);
        dst[n - 1] = 0;
    }
    return len;
}
const char *ur_json_string(cJSON *root, const char *a, const char *b) {
    auto *value = cJSON_GetObjectItemCaseSensitive(root, a);
    if (!cJSON_IsString(value) && b)
        value = cJSON_GetObjectItemCaseSensitive(root, b);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}
int xQueueSend(void *, const void *item, int) {
    queued = *static_cast<const ur_action_item_t *>(item);
    ++sends;
    return pdTRUE;
}
bool protected_network = false;
bool ur_network_protect_credentials() {
    return protected_network;
}
struct UrSignalingCall {
    int calls = 0;
    bool accept = true, voice = false;
    std::string target;
    static UrSignalingCall &Instance() {
        static UrSignalingCall call;
        return call;
    }
    bool Dial(const char *t, bool v) {
        ++calls;
        target = t;
        voice = v;
        return accept;
    }
};
struct Board {
    static Board &GetInstance() {
        static Board board;
        return board;
    }
    int HandleUrAction(const char *, const char *) {
        return 404;
    }
};
PRODUCTION
int main() {
    auto *root = cJSON_Parse(
        R"({"method":"action","actionID":"CallDevice","params":{"target":"8:44B176B05C2E","callType":"video"}})");
    assert(ur_enqueue_action("request", root));
    cJSON_Delete(root);
    assert(ur_bridge_handle_action(queued.action_id, queued.params) == 200);
    auto &call = UrSignalingCall::Instance();
    assert(call.calls == 1 && call.target == "8:44B176B05C2E" && !call.voice);
    assert(ur_bridge_handle_action("CallDevice", R"({"target":"p:d","callType":"voice"})") == 200 &&
           call.voice);
    call.accept = false;
    assert(ur_bridge_handle_action("CallDevice", queued.params) == 409);
    const auto calls = call.calls;
    assert(ur_bridge_handle_action("CallDevice", R"({"target":"p:d","callType":"other"})") == 400 &&
           call.calls == calls);
    protected_network = true;
    assert(ur_bridge_handle_action("CallDevice", queued.params) == 423 && call.calls == calls);
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "actionID", "CallDevice");
    auto *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddStringToObject(params, "target", std::string(300, 'a').c_str());
    cJSON_AddStringToObject(params, "callType", "voice");
    assert(!ur_enqueue_action("request", root) && sends == 1);
    cJSON_ReplaceItemInObject(root, "actionID", cJSON_CreateString("takePhoto"));
    assert(ur_enqueue_action("request", root));
    cJSON_Delete(root);
}
'''.replace('PRODUCTION', method(device, 'static bool ur_enqueue_action(') + '\n' + method(bridge, 'int ur_bridge_handle_action('))
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            driver = folder / 'call.cc'
            driver.write_text(code)
            library = folder / 'cjson.o'
            subprocess.run(['gcc', '-I', str(cjson), '-c', str(cjson / 'cJSON.c'), '-o', str(library)], check=True)
            # C生产函数的{0}完整清零写法在C++联编时仅关闭对应的风格警告。
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-Wno-missing-field-initializers', '-I', str(cjson),
                                     str(driver), str(library), '-o', str(folder / 'call')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(folder / 'call')], check=True)
