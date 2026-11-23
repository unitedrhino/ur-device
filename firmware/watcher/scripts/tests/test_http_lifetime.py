"""编译真实析构/断开方法，复现被动断连回调晚于成员销毁的生命周期错误。"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_http_lifetime import HTTP_OLD, TCP_OLD, transform

# 替身只观察资源销毁次序与 join；执行的析构/Disconnect 正文来自构建生成器。
HARNESS = r'''
#include <memory>
#include <cstdlib>
#include <atomic>
#include <thread>
static std::atomic<bool> lock_alive{true}, joined{false}, event_deleted{false}, finish{false};
constexpr int ESP_TCP_EVENT_RECEIVE_TASK_EXIT=1, pdFALSE=0, portMAX_DELAY=-1;
int xTaskGetCurrentTaskHandle(){return 2;}
int xEventGroupWaitBits(int,int,int,int,int timeout){
 if(timeout!=portMAX_DELAY) std::_Exit(17); joined=true; return 1;
}
void vEventGroupDelete(int){event_deleted=true;}
struct Guard {~Guard(){lock_alive=false;}};
struct Tcp {
 // 模拟已断连但仍未完成的接收回调；销毁开始才放行，稳定触发旧顺序。
 std::thread receive_{[]{
   while(!finish.load()) std::this_thread::yield();
   if(!lock_alive || event_deleted) std::_Exit(18);
 }};
 ~Tcp(){finish=true; receive_.join();}
};
struct HttpClient {
 std::unique_ptr<Tcp> tcp_{new Tcp};
 int event_group_handle_=1;
 Guard mutex_;
 bool connected_=false;
 void Close(){connected_=false;}
 ~HttpClient();
};
struct EspTcp {
 bool connected_=false;
 int receive_task_handle_=1,event_group_=1;
 void DoDisconnect(bool){connected_=false;}
 void Disconnect();
};
'''


class HttpLifetimeTest(unittest.TestCase):
    """旧方法必须可复现失败；修复必须保留主动断开和无接收任务分支。"""

    def run_source(self, http, tcp, body):
        """编译生产方法到独立进程，避免故意触发的旧析构失败终止测试解释器。"""
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "lifetime")
            subprocess.run(["g++", "-std=c++17", "-pthread", "-x", "c++", "-", "-o", binary],
                           input=HARNESS + http + tcp + "\nint main(){" + body + "}",
                           text=True, check=True, capture_output=True)
            return subprocess.run([binary], capture_output=True).returncode

    def test_passive_disconnect_lifecycle(self):
        """旧实现提前析构锁/跳过 join；补丁必须在连接已断开的场景仍安全释放。"""
        cases = ["{HttpClient client;} return 0;",
                 "EspTcp tcp; tcp.Disconnect(); return joined?0:9;"]
        for body in cases:
            self.assertNotEqual(self.run_source(HTTP_OLD, TCP_OLD, body), 0)
            self.assertEqual(self.run_source(transform(HTTP_OLD, "http"),
                                             transform(TCP_OLD, "tcp"), body), 0)

    def test_disconnect_branches(self):
        """主动关闭要 join；未创建任务和接收线程自身不能自等死锁。"""
        for body in ["EspTcp t; t.connected_=true; t.Disconnect(); return !joined || t.connected_;",
                     "EspTcp t; t.receive_task_handle_=0; t.Disconnect(); return joined;",
                     "EspTcp t; t.receive_task_handle_=2; t.Disconnect(); return joined;"]:
            self.assertEqual(self.run_source(transform(HTTP_OLD, "http"),
                                             transform(TCP_OLD, "tcp"), body), 0)

    def test_dependency_drift_fails_closed(self):
        """上游结构变化或重复定义不得静默生成未修复固件。"""
        for kind, old in [("http", HTTP_OLD), ("tcp", TCP_OLD)]:
            for source in ["", old + old, old.replace("connected_", "changed_")]:
                with self.assertRaises(ValueError):
                    transform(source, kind)
