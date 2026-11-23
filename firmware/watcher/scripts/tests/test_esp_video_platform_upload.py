"""编译实际EspVideo平台上传方法，验证认证、分块返回值和真实JSON错误边界。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EspVideoPlatformUploadTest(unittest.TestCase):
    """只替换相机编码和网络边界，生产上传方法、JSON解析与校验原样执行。"""

    def test_platform_upload_contract(self):
        """带块头的发送字节数可通过，认证、编码、网络和平台失败均不可伪成功。"""
        source = (ROOT / 'main/boards/common/esp_video.cc').read_text()
        begin = source.index('std::expected<std::string, std::string> EspVideo::UploadToPlatform(')
        method = source[begin:source.index('\n#endif', begin)]
        cjson = Path(os.environ.get('UR_TEST_CJSON_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            target = Path(directory)
            driver = target / 'driver.cc'
            driver.write_text(r'''
// 上传边界替身；cJSON、UR图片校验及被测方法均来自生产源码。
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include "cJSON.h"
#include "ur_photo_runtime.h"
#define ESP_LOGI(...) ((void)0)
constexpr unsigned V4L2_PIX_FMT_JPEG=7;
struct State {
 bool open_error=false, write_error=false, read_error=false, encode_error=false, create_error=false;
 int status=200, creates=0;
 bool terminated=false, closed=false;
 std::string body=R"({"code":200,"data":{"fileUri":"https://example.invalid/image.jpg"}})";
 std::string uploaded;
 std::map<std::string,std::string> headers;
} state;
void heap_caps_free(void* p) { std::free(p); }
void mbedtls_platform_zeroize(void* p,size_t n) { std::memset(p,0,n); }
int mbedtls_base64_encode(unsigned char* dst,size_t capacity,size_t* size,const unsigned char* src,size_t n) {
 if(capacity<4*((n+2)/3)+1) return -1;
 *size=EVP_EncodeBlock(dst,src,static_cast<int>(n));return 0;
}
const unsigned char jpeg[]={0xff,0xd8,0xff,0xe0,0xff,0xd9};
bool image_to_jpeg(uint8_t*,size_t,uint16_t,uint16_t,unsigned,uint8_t,uint8_t** out,size_t* size) {
 if(state.encode_error) return false;
 *size=sizeof(jpeg);*out=static_cast<uint8_t*>(std::malloc(*size));std::memcpy(*out,jpeg,*size);return true;
}
struct Http {
 size_t offset=0;
 void SetHeader(const std::string& k,const std::string& v) { state.headers[k]=v; }
 std::expected<void,std::string> Open(const char*,const std::string&) {
  if(state.open_error) return std::unexpected("unavailable");
  return {};
 }
 void Close() { state.closed=true; }
 std::expected<int,std::string> Write(const char* p,size_t n) {
  if(state.write_error) return std::unexpected("failed");
  if(n==0) { state.terminated=true;return 5; }
  state.uploaded.append(p,n);return static_cast<int>(n+6); // 与chunked客户端一致，返回值含块头。
 }
 std::expected<int,std::string> GetStatusCode() { return state.status; }
 std::expected<int,std::string> Read(char* p,size_t n) {
  if(state.read_error) return std::unexpected("failed");
  size_t count=std::min(n,state.body.size()-offset);std::memcpy(p,state.body.data()+offset,count);offset+=count;return static_cast<int>(count);
 }
};
struct Network { std::unique_ptr<Http> CreateHttp(int) { ++state.creates;if(state.create_error) return {};return std::make_unique<Http>(); } };
struct Board {
 static Board& GetInstance() { static Board b;return b; }
 Network* GetNetwork() { static Network n;return &n; }
};
struct EspVideo {
 std::mutex frame_mutex_;
 bool compressed_stream_owned_=false;
 bool still_frame_valid_=true;
 struct Frame { uint8_t* data;size_t len;uint16_t width,height;unsigned format; };
 Frame frame_{const_cast<uint8_t*>(jpeg),sizeof(jpeg),2,2,1};
 std::expected<std::string,std::string> UploadToPlatform(const std::string&,const char*,const char*);
};
METHOD
int main() {
 EspVideo camera;
 auto upload=[&](){return camera.UploadToPlatform("https://example.invalid/upload","user","pass");};
 auto good=upload();assert(good && *good=="https://example.invalid/image.jpg");
 assert(state.headers["Authorization"]=="Basic dXNlcjpwYXNz");
 assert(state.terminated && state.closed && state.uploaded.find("filename=\"camera.jpg\"")!=std::string::npos);
 for(const auto& body:std::vector<std::string>{"{}","broken",R"({"code":500,"data":{"fileUri":"x"}})",R"({"code":200,"data":{"fileUri":""}})",R"({"code":"200","data":{"fileUri":"x"}})",std::string(8200,'x')}) {
  state=State{};state.body=body;assert(!upload() && state.closed);
 }
 state=State{};state.status=500;assert(!upload() && state.closed);
 state=State{};state.create_error=true;assert(!upload());
 state=State{};state.open_error=true;assert(!upload() && state.closed);
 state=State{};state.write_error=true;assert(!upload() && state.closed);
 state=State{};state.read_error=true;assert(!upload() && state.closed);
 state=State{};state.encode_error=true;assert(!upload() && state.creates==0);
 state=State{};assert(!camera.UploadToPlatform("https://example.invalid/upload",nullptr,"pass") && state.creates==0);
 camera.compressed_stream_owned_=true;assert(!upload() && state.creates==0);
 camera.compressed_stream_owned_=false;
 camera.still_frame_valid_=false;assert(!upload() && state.creates==0);
 camera.still_frame_valid_=true;
 camera.frame_.len=0;assert(!upload() && state.creates==0);
 camera.frame_.len=sizeof(jpeg);camera.frame_.format=V4L2_PIX_FMT_JPEG;
 assert(upload()); // 已编码JPEG不重复申请编码缓冲区。
}
'''.replace('METHOD', method))
            subprocess.run(['gcc', '-std=c99', '-I', str(cjson), '-c', str(cjson / 'cJSON.c'),
                            '-o', str(target / 'cjson.o')], check=True, capture_output=True)
            command = ['g++', '-std=c++23', '-Wall', '-Wextra', '-Werror',
                       '-I', str(cjson), '-I', str(ROOT / 'main/protocols'), str(driver),
                       str(ROOT / 'main/protocols/ur_photo_runtime.cc'), str(target / 'cjson.o'),
                       '-lcrypto', '-lm', '-o', str(target / 'upload-test')]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(target / 'upload-test')], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
