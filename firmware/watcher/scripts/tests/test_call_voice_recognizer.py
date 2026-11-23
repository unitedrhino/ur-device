"""用可控 MultiNet 边界执行生产接听词识别器，验证所有权和分帧。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CallVoiceRecognizerTest(unittest.TestCase):
    """不加载真实模型；验证失败收尾、单例所有权和检测结果约束。"""

    def test_recognizer_lifecycle_and_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'model_path.h').write_text('''#pragma once
struct srmodel_list_t{};
char* esp_srmodel_filter(srmodel_list_t*,const char*,const char*);
''')
            (root / 'esp_err.h').write_text('#pragma once\n#define ESP_OK 0\n')
            (root / 'esp_mn_iface.h').write_text('''#pragma once
#include <cstdint>
#define ESP_MN_PREFIX "mn"
#define ESP_MN_RESULT_MAX_NUM 5
struct model_iface_data_t{};
enum esp_mn_state_t{ESP_MN_STATE_DETECTING,ESP_MN_STATE_DETECTED,ESP_MN_STATE_TIMEOUT};
struct esp_mn_results_t{int num;int command_id[5];};
struct esp_mn_iface_t{model_iface_data_t*(*create)(const char*,int);int(*get_samp_chunksize)(model_iface_data_t*);int(*get_samp_rate)(model_iface_data_t*);int(*set_det_threshold)(model_iface_data_t*,float);esp_mn_state_t(*detect)(model_iface_data_t*,int16_t*);esp_mn_results_t*(*get_results)(model_iface_data_t*);void(*clean)(model_iface_data_t*);void(*destroy)(model_iface_data_t*);};
''')
            (root / 'esp_mn_models.h').write_text('#pragma once\n#include "esp_mn_iface.h"\nesp_mn_iface_t* esp_mn_handle_from_name(char*);\n')
            (root / 'esp_mn_speech_commands.h').write_text('''#pragma once
int esp_mn_commands_clear();int esp_mn_commands_add(int,const char*);void* esp_mn_commands_update();
''')
            code = r'''
#include <cassert>
#include <cstring>
#include "call_voice_recognizer.h"
static model_iface_data_t model_data;
static int creates=0,destroys=0,cleans=0,calls=0,fail=0,chunk=4,rate=16000;
static esp_mn_state_t next=ESP_MN_STATE_DETECTING;
static esp_mn_results_t result{1,{1}};
char* esp_srmodel_filter(srmodel_list_t*,const char*,const char*){static char name[]="mn5q8_cn";return fail==1?nullptr:name;}
model_iface_data_t* create(const char*,int){++creates;return fail==2?nullptr:&model_data;}
int chunks(model_iface_data_t*){return chunk;}int sample_rate(model_iface_data_t*){return rate;}
int threshold(model_iface_data_t*,float t){assert(t==0.5f);return 0;}
esp_mn_state_t detect(model_iface_data_t*,int16_t* p){++calls;assert(p[0]==1 && p[3]==4);return next;}
esp_mn_results_t* results(model_iface_data_t*){return &result;}
void clean(model_iface_data_t*){++cleans;}void destroy(model_iface_data_t*){++destroys;}
esp_mn_iface_t iface{create,chunks,sample_rate,threshold,detect,results,clean,destroy};
esp_mn_iface_t* esp_mn_handle_from_name(char*){return fail==3?nullptr:&iface;}
int esp_mn_commands_clear(){return fail==4?1:0;}
int esp_mn_commands_add(int id,const char* command){assert(id==1 && !std::strcmp(command,"jie ting dian hua"));return fail==5?1:0;}
void* esp_mn_commands_update(){return fail==6?&model_data:nullptr;}
int main(){
 srmodel_list_t models;
 for(int step=1;step<=6;++step){fail=step;CallVoiceRecognizer bad;assert(!bad.Start(&models));}
 fail=0;chunk=1025;{CallVoiceRecognizer bad;assert(!bad.Start(&models));}chunk=4;
 rate=8000;{CallVoiceRecognizer bad;assert(!bad.Start(&models));}rate=16000;
 CallVoiceRecognizer first,other;assert(first.Start(&models));assert(!other.Start(&models));
 int16_t pcm[]={1,2,3,4};assert(!first.Feed(pcm,2));assert(calls==0);
 assert(!first.Feed(pcm+2,2));assert(calls==1);
 next=ESP_MN_STATE_DETECTED;result.command_id[0]=2;assert(!first.Feed(pcm,4));assert(cleans==1);
 result.command_id[0]=1;assert(first.Feed(pcm,4));assert(!first.Feed(pcm,4));
 first.Stop();assert(other.Start(&models));other.Stop();assert(creates==destroys+1);
}
'''
            binary = root / 'test'
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(root), '-I', str(ROOT / 'main/audio/call_voice'),
                                     str(ROOT / 'main/audio/call_voice/call_voice_recognizer.cc'),
                                     '-x', 'c++', '-', '-o', str(binary)], input=code,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
