"""使用生产入队函数验证坦克动作参数及非坦克兼容边界。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SparkBotActionValidationTest(unittest.TestCase):
    """坦克超长参数不得排队，原有动作不受新增缓冲长度限制。"""

    def test_tank_parameters_are_not_truncated(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/unitedrhino/ur_device.c').read_text()
        begin = source.index('static bool ur_enqueue_action(')
        end = source.index('\n/* ur_drain_action_queue', begin)
        cjson = Path(os.environ.get('UR_TEST_CJSON_DIR', root / 'managed_components/espressif__cjson/cJSON'))
        code = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define pdTRUE 1
struct ur_action_item { char token[128], action_id[32], content[129], request_id[17], params[256]; };
typedef struct ur_action_item ur_action_item_t;
static int enabled=1, sends=0;
static void *s_action_queue=&enabled;
static ur_action_item_t sent;
static size_t strlcpy(char *dst, const char *src, size_t size) {
 size_t len=strlen(src); if (size) {size_t n=len < size-1 ? len : size-1; memcpy(dst,src,n);dst[n]=0;} return len;
}
static const char* ur_json_string(cJSON *root,const char *a,const char *b) {
 cJSON *x=cJSON_GetObjectItemCaseSensitive(root,a);
 if (!cJSON_IsString(x) && b) x=cJSON_GetObjectItemCaseSensitive(root,b);
 return cJSON_IsString(x) ? x->valuestring : NULL;
}
static int xQueueSend(void* queue,const void *item,int wait) {
 (void)queue;(void)wait;memcpy(&sent,item,sizeof(sent));sends++;return pdTRUE;
}
METHOD
int main(void) {
 cJSON *r=cJSON_Parse("{\"actionID\":\"tankSetLight\",\"params\":{\"LightMode\":3}}");
 if (!ur_enqueue_action("t",r) || sends!=1 || !strstr(sent.params,"LightMode")) return 1;
 cJSON_Delete(r);
 char large[500];memset(large,'a',sizeof(large)-1);large[sizeof(large)-1]=0;
 r=cJSON_CreateObject();cJSON_AddStringToObject(r,"actionID","tankForward");
 cJSON *p=cJSON_AddObjectToObject(r,"params");cJSON_AddStringToObject(p,"padding",large);
 if (ur_enqueue_action("t",r) || sends!=1) return 2;
 cJSON_ReplaceItemInObject(r,"actionID",cJSON_CreateString("takePhoto"));
 if (!ur_enqueue_action("t",r) || sends!=2) return 3;
 cJSON_Delete(r);return 0;
}
'''.replace('METHOD', source[begin:end])
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory) / 'test.c'
            driver.write_text(code)
            binary = Path(directory) / 'test'
            result = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(cjson), str(driver), str(cjson / 'cJSON.c'),
                                     '-lm', '-o', str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
