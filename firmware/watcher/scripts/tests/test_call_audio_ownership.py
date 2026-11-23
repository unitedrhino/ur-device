"""执行真实应用调度体，验证按通话预算与硬件参考启用AEC、暂停唤醒及归位恢复。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_call_audio_decode import method

ROOT = Path(__file__).resolve().parents[2]

class CallAudioOwnershipTest(unittest.TestCase):
    """只替换应用和音频端口，生产控制方法和调度体保持原样。"""
    def test_actual_call_audio_ownership(self):
        """回放真实启停闭包，验证AEC预算、重复操作及AI接管后的迟到关闭。"""
        source = (ROOT / 'main/signaling/ur_signaling_call.cc').read_text()
        begin = source.split('void UrSignalingCall::Impl::StartMedia()',1)[1]
        end = source.split('void UrSignalingCall::Impl::StopMedia()',1)[1]
        start_body = method(begin, 'Application::GetInstance().Schedule(') + ')'
        # 同时提取调度前的真实编译条件，避免只测独立辅助函数而漏掉通话类型。
        policy_end = begin.index('    Application::GetInstance().Schedule(')
        policy_start = begin.rfind('#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER',0,policy_end)
        policy = begin[policy_start:policy_end]
        stop_body = method(end, 'Application::GetInstance().Schedule(') + ')'
        helper = method(source, 'void SetHumanCallAudioProcessing(') if 'void SetHumanCallAudioProcessing(' in source else ''
        harness = r'''
#include <cassert>
#define ESP_LOGI(...) ((void)0)
// 人际回采由实际参考能力决定，AI半双工能力声明可独立关闭。
constexpr bool expected_aec=CONFIG_UR_AI_SIGNALING_DEVICE_AEC || CONFIG_UR_SOFTWARE_AEC;
constexpr int kDeviceStateIdle=0,kDeviceStateListening=1;
struct Audio {bool wake=true,aec=false,processing=false,low_latency=false;int writes=0;
 bool IsWakeWordRunning(){return wake;}bool IsDeviceAecEnabled(){return aec;}
 void EnableWakeWordDetection(bool b){wake=b;++writes;}void EnableDeviceAec(bool b){aec=b;++writes;}
 void EnableVoiceProcessing(bool b){processing=b;++writes;}
 void EnableLowLatencyCapture(bool b){low_latency=b;}
};
struct Application {Audio audio;int state=kDeviceStateIdle;bool owned=false;
 void SetHumanCallAudioOwned(bool value){owned=value;}static Application& GetInstance(){static Application a;return a;}
 Audio& GetAudioService(){return audio;}int GetDeviceState(){return state;}
 template<class F> void Schedule(F f){f();}
};
enum class PowerSaveLevel{PERFORMANCE,LOW_POWER};
// Codec 只替换板级能力查询，真实StartMedia决策仍从生产源码提取。
struct Codec {bool reference=false;bool input_reference(){return reference;}};
struct Board {Codec codec;bool missing_codec=false;
 static Board& GetInstance(){static Board b;return b;}
 Codec* GetAudioCodec(){return missing_codec ? nullptr : &codec;}
 void SetPowerSaveLevel(PowerSaveLevel){}
};
HELPER
void start(bool use_aec=true){(void)use_aec;START;}
// 保留StartMedia真实配置选择，覆盖视频预算及显式AEC能力。
bool policy(bool voice_call_){(void)voice_call_;
POLICY
return use_aec;}
void stop(){STOP;}
int main(){
 auto& app=Application::GetInstance();auto& a=app.audio;
 assert(policy(true));
#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER && CONFIG_UR_HUMAN_CALL_UDP_AV && CONFIG_UR_SOFTWARE_AEC && !CONFIG_UR_AI_SIGNALING_DEVICE_AEC
 // 软件参考仍限制视频；硬件参考允许视频AEC，无音频端口时退回原策略。
 auto& board=Board::GetInstance();
 assert(!policy(false));
 board.codec.reference=true;assert(policy(false));
 board.missing_codec=true;assert(!policy(false)&&policy(true));
 board.missing_codec=false;board.codec.reference=false;
#else
 assert(policy(false));
#endif
 start();assert(app.owned&&!a.wake&&a.aec==expected_aec&&a.processing&&a.low_latency);
 stop();assert(!app.owned&&a.wake&&!a.aec&&!a.processing&&!a.low_latency);
 start(false);assert(!a.wake&&!a.aec&&a.processing&&a.low_latency);stop();assert(a.wake&&!a.aec&&!a.processing);
 // 重复开/关及快速重入不能覆盖第一份原始状态。
 start();start();stop();assert(a.wake&&!a.aec&&!a.processing);stop();
 a.wake=false;a.aec=true;start();stop();assert(!a.wake&&a.aec&&!a.processing);
 // 应用已经取得AI音频时，迟到的人际关闭不能关闭其处理管线或AEC。
 a.wake=true;a.aec=false;start();app.state=kDeviceStateListening;a.processing=true;
 int writes=a.writes;stop();assert(a.writes==writes&&a.processing&&a.aec==expected_aec&&!a.wake&&!a.low_latency);
 app.state=kDeviceStateIdle;a.wake=true;a.aec=false;start();stop();assert(a.wake&&!a.aec);
}
'''.replace('HELPER',helper).replace('START',start_body).replace('STOP',stop_body).replace('POLICY',policy)
        output=ROOT.parents[1]/'.temp';output.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            binary=Path(directory)/'ownership'
            for watcher,udp,claim,software in [(1,1,1,0),(1,1,0,1),(1,1,0,0),(1,1,1,1),(0,1,0,1),(1,0,0,1)]:
                with self.subTest(watcher=watcher,udp=udp,ai_aec_claim=claim,software_reference=software):
                    result=subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-Wno-unused-but-set-variable',
                        f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',
                        f'-DCONFIG_UR_HUMAN_CALL_UDP_AV={udp}',
                        f'-DCONFIG_UR_AI_SIGNALING_DEVICE_AEC={claim}',
                        f'-DCONFIG_UR_SOFTWARE_AEC={software}',
                        '-x','c++','-','-o',str(binary)],input=harness,text=True,capture_output=True)
                    self.assertEqual(result.returncode,0,result.stderr)
                    result=subprocess.run([str(binary)],capture_output=True)
                    self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))


    def test_watcher_software_reference_display_budget(self):
        """执行真实LVGL端口配置，回采构建保留内部DMA余量并验证单核不越界。"""
        source = (ROOT / 'main/display/lcd_display.cc').read_text()
        start = source.index('    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();')
        end = source.index('    lvgl_port_init(&port_cfg);', start)
        setup = source[start:end]
        rows_start = source.rfind('#if CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER', end, source.index('DisplayLockGuard initialization_lock', end))
        rows_end = source.index('    // 注册显示与替换回调', rows_start)
        setup += source[rows_start:rows_end]
        code = r'''
#include <cassert>
struct lvgl_port_cfg_t {int task_priority=2,task_max_sleep_ms=20,task_affinity=-1;};
#define ESP_LVGL_PORT_INIT_CONFIG() lvgl_port_cfg_t{}
int main(){
SETUP
#if CONFIG_SOC_CPU_CORES_NUM > 1
 constexpr int expected = 0;
 assert(port_cfg.task_affinity==expected);
#else
 assert(port_cfg.task_affinity==-1);
#endif
 // 按整屏行数约束DMA预算，不要求改变现有显示核配置。
 constexpr int expected_rows=CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER ? (CONFIG_UR_SOFTWARE_AEC && CONFIG_UR_HUMAN_CALL_UDP_AV ? 12:24):16;
 assert(render_rows==expected_rows);
 assert(!full_frame);
}
'''.replace('SETUP',setup)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory)/'display-affinity')
            for watcher,software,udp,silicon,cores in [(1,1,1,2,2),(1,0,1,2,2),(1,1,0,2,2),(0,1,1,2,2),(1,1,1,2,1),(1,1,1,1,1)]:
                with self.subTest(watcher=watcher,software=software,udp=udp,cores=cores):
                    flags=[f'-DCONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER={watcher}',f'-DCONFIG_UR_SOFTWARE_AEC={software}',f'-DCONFIG_UR_HUMAN_CALL_UDP_AV={udp}',f'-DCONFIG_SOC_CPU_CORES_NUM={silicon}',f'-DCONFIG_FREERTOS_NUMBER_OF_CORES={cores}']
                    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-x','c++','-','-o',binary],input=code,text=True,check=True)
                    result=subprocess.run([binary],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stderr)

if __name__ == '__main__':unittest.main()
