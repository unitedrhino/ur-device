"""执行生产订阅等待器，验证旧缓存不能替代本次 SUBACK。"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class SubscriptionAckTest(unittest.TestCase):
    """对串行 MQTT 任务模拟成功、拒绝、迟到回复和网络错误。"""

    def test_current_packet_ack(self):
        """旧订阅始终存在，也必须等待当前 packet 的确认。"""
        source = (ROOT / 'main/unitedrhino/ur_device.c').read_text()
        body = source[source.index('static int ur_subscribe_topic('):
                      source.index('/* ur_subscribe_down_topics')]
        callback = source[source.index('static void ur_mqtt_event_handler('):
                          source.index('/* ur_setup_connect_params')]
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define CONFIG_UR_AI 0
#define ESP_LOGI(...) ((void)0)
#define UR_TOPIC_BUF_LEN 128
#define DEFAULT_SUB_PARAMS {}
#define HAL_Snprintf snprintf
enum { QOS0=0, UNITEDRHINO_RET_SUCCESS=0, UNITEDRHINO_RET_MQTT_RECONNECTED=1,
UNITEDRHINO_ERR_FAILURE=-1, MQTT_EVENT_RECONNECT=2, MQTT_EVENT_DISCONNECT=3,
MQTT_EVENT_SUBCRIBE_SUCCESS=4, MQTT_EVENT_SUBCRIBE_NACK=5, MQTT_EVENT_SUBCRIBE_TIMEOUT=6 };
using OnMessageHandler=void*;
struct SubscribeParams { int qos; void* on_message_handler; };
struct MQTTEventMsg { int event_type; void* msg; };
static bool s_connected=false, s_resubscribe_pending=false;
static int s_subscribe_packet_id=0, s_subscribe_result=0;
static const char *s_product_id="test", *s_device_name="device";
static int scenario=0, calls=0, sequence=40;
void ur_network_disconnected() {}
CALLBACK
int IOT_MQTT_Subscribe(void*, const char*, SubscribeParams*) { return ++sequence; }
bool IOT_MQTT_IsSubReady(void*, const char*) { return true; }
void ack(int kind, int packet) {
 MQTTEventMsg event{kind, reinterpret_cast<void*>(static_cast<uintptr_t>(packet))};
 ur_mqtt_event_handler(nullptr,nullptr,&event);
}
int IOT_MQTT_Yield(void*, int) {
 ++calls;
 if(scenario==5) { ack(MQTT_EVENT_DISCONNECT,0); return 0; }
 if(scenario==4) return -606;
 if(scenario==3) { ack(MQTT_EVENT_SUBCRIBE_SUCCESS,sequence-1); return 0; }
 if(calls==1) { ack(MQTT_EVENT_SUBCRIBE_SUCCESS,sequence-1); return 0; }
 if(scenario==1) { ack(MQTT_EVENT_SUBCRIBE_NACK,sequence); ack(MQTT_EVENT_SUBCRIBE_SUCCESS,sequence); }
 else if(scenario==2) ack(MQTT_EVENT_SUBCRIBE_TIMEOUT,sequence);
 else ack(MQTT_EVENT_SUBCRIBE_SUCCESS,sequence);
 return 0;
}
BODY
int main() {
 for(scenario=0;scenario<6;++scenario) {
  calls=0; int result=ur_subscribe_topic(nullptr,"topic",nullptr);
  assert(calls>0);
  if(scenario==0) assert(result==0 && calls==2);
  else assert(result<0);
 }
}
'''.replace('CALLBACK', callback).replace('BODY', body)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'subscription')
            built = subprocess.run(['g++', '-std=c++17', '-x', 'c++', '-', '-o', binary],
                                   input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            for _ in range(10):
                result = subprocess.run([binary], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
