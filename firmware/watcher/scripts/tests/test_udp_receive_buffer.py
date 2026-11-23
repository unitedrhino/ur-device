"""用真实UDP套接字回放生产端口，验证空轮询复用缓冲且数据报边界不变。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UdpReceiveBufferTest(unittest.TestCase):
    """只把lwIP名字映射到POSIX；编译完整生产端口，不重写收发方法。"""

    def test_empty_polls_allocate_nothing_and_keep_packet_boundaries(self):
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <vector>
#include "ur_signaling_udp_port.h"
std::atomic<unsigned>allocations{0},releases{0};
void*operator new(size_t n){++allocations;if(void*p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void*p)noexcept{if(p){++releases;std::free(p);}}
void operator delete(void*p,size_t)noexcept{::operator delete(p);}
int main(){using namespace ur::device;UrSignalingUdpPort port;assert(port.open("127.0.0.1",0));
 int sender=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(sender>=0);
 sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(::bind(sender,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==0);
 socklen_t len=sizeof(peer);assert(::getsockname(sender,reinterpret_cast<sockaddr*>(&peer),&len)==0);
 assert(port.set_peer("127.0.0.1",ntohs(peer.sin_port)));
 sockaddr_in target=peer;target.sin_port=htons(port.local_port());
 auto send=[&](const std::vector<uint8_t>&v){assert(::sendto(sender,v.data(),v.size(),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==static_cast<ssize_t>(v.size()));};
 std::vector<uint8_t>packet;
 assert(port.receive(packet)==ReceiveResult::Empty);const auto before=allocations.load();
 for(int i=0;i<2000;++i)assert(port.receive(packet)==ReceiveResult::Empty&&packet.empty());
 assert(allocations==before&&"empty receive allocated a temporary 2KiB buffer");
 for(size_t n:{size_t(1),size_t(80),size_t(1200),size_t(2047),size_t(2048),size_t(2049),size_t(0)}){
  std::vector<uint8_t>data(n);for(size_t i=0;i<n;++i)data[i]=static_cast<uint8_t>(i*13+n);
  send(data);const auto result=port.receive(packet);
  if(n==0)assert(result==ReceiveResult::Empty&&packet.empty());
  else if(n>=2048)assert(result==ReceiveResult::Invalid&&packet.empty());
  else assert(result==ReceiveResult::Received&&packet==data);
 }
 // 调用者复用输出向量时，持续收包也无需重新申请端口临时缓冲。
 std::vector<uint8_t>data(80,7);send(data);assert(port.receive(packet)==ReceiveResult::Received);
 const auto warm=allocations.load();
 for(int i=0;i<100;++i){send(data);assert(port.receive(packet)==ReceiveResult::Received&&packet==data);}
 assert(allocations==warm);
 const auto freed=releases.load();port.close();assert(releases>freed&&"close kept the receive scratch buffer resident");
 assert(port.local_port()==0&&port.receive(packet)==ReceiveResult::Failed&&packet.empty());
 port.close();::close(sender);
}
'''
        with tempfile.TemporaryDirectory(prefix='watcher-udp-buffer-') as directory:
            directory = Path(directory)
            (directory / 'lwip').mkdir()
            aliases = ['socket', 'bind', 'close', 'fcntl', 'getsockname', 'inet_pton', 'sendto', 'recvfrom']
            (directory / 'lwip/sockets.h').write_text(
                '#pragma once\n#include <sys/socket.h>\n#include <netinet/in.h>\n#include <arpa/inet.h>\n#include <fcntl.h>\n#include <unistd.h>\n#include <cerrno>\n' +
                '\n'.join('#define lwip_' + name + ' ::' + name for name in aliases))
            (directory / 'esp_log.h').write_text('#pragma once\ntemplate<class...T>void host_log(T&&...){}\n#define ESP_LOGI(...) host_log(__VA_ARGS__)\n#define ESP_LOGE(...) host_log(__VA_ARGS__)\n')
            (directory / 'esp_timer.h').write_text('#pragma once\ninline long long esp_timer_get_time(){return 0;}\n')
            binary = str(directory / 'udp')
            built = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(directory), '-I', str(ROOT / 'main/signaling'),
                                    '-I', str(ROOT.parent / 'device-runtime/include'),
                                    str(ROOT / 'main/signaling/ur_signaling_udp_port.cc'),
                                    '-x', 'c++', '-', '-o', binary], input=code,
                                   text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
