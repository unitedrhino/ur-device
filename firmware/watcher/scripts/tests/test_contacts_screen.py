"""联编生产菜单和真实JSON查询器，验证分页、选择和迟到结果不能跨页面拨号。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

# ROOT固定为生产固件工程，测试只替换网络传输，不复制状态机。
ROOT = Path(__file__).resolve().parents[2]


class ContactsScreenTest(unittest.TestCase):
    """通讯录层级与只读MQTT应答的宿主回归。"""

    def test_menu_selection_paging_and_stale_reply(self):
        """加载只允许返回，翻页保持原目标，退出后旧结果不重开页面。"""
        code = r'''
#include "ui/menu_model.h"
#include <cassert>
int main() {
    using namespace watcher;
    MenuModel model;
    assert(model.Count() == 5 && TouchBackIndex > model.Count());
    auto revision = model.Revision();
    assert(model.TouchTarget(MenuTarget::Directory, revision, 1) == MenuAction::LoadContacts);
    assert(model.Page() == MenuPage::Contacts && model.ContactsLoading() && model.Count() == 1);
    revision = model.Revision();
    assert(!model.ApplyContacts(revision-1, 0, 4, true, true));
    assert(model.ApplyContacts(revision, 0, 4, true, true));
    assert(model.Count() == 7);
    assert(model.TouchItem(2, revision, 2) == MenuAction::None && model.Page() == MenuPage::Contacts);
    model.TouchItem(4, model.Revision(), 3);
    assert(model.Page() == MenuPage::Contact && model.SelectedContact() == 2);
    assert(model.TouchItem(1, model.Revision(), 4) == MenuAction::DialVoice);
    assert(model.TouchItem(2, model.Revision(), 5) == MenuAction::DialVideo);
    model.Handle(MenuInput::Back, 6);
    assert(model.Page() == MenuPage::Contacts && model.Focus() == 4);
    assert(model.TouchItem(6, model.Revision(), 7) == MenuAction::LoadContacts);
    assert(model.ContactsOffset() == 4 && model.ContactsLoading());
    assert(model.ApplyContacts(model.Revision(), 4, 2, false, true));
    assert(model.Count() == 5);
    assert(model.TouchItem(4, model.Revision(), 8) == MenuAction::LoadContacts);
    assert(model.ContactsOffset() == 0);
    revision = model.Revision(); model.Handle(MenuInput::Back, 9);
    assert(!model.ApplyContacts(revision, 0, 1, false, true) && model.Page() == MenuPage::Home);
    model.TouchTarget(MenuTarget::Directory, model.Revision(), 10);
    assert(model.ApplyContacts(model.Revision(), 0, 0, false, false));
    assert(model.ContactsFailed() && model.Count() == 2);
    assert(model.TouchItem(1, model.Revision(), 11) == MenuAction::LoadContacts);
    assert(model.ApplyContacts(model.Revision(), 0, 0, false, true));
    assert(!model.ContactsFailed() && model.Count() == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'menu'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-x', 'c++',
                            '-I', str(ROOT / 'main/boards/sensecap-watcher'), '-', '-o', str(binary)],
                           input=code, text=True, capture_output=True, check=True)
            subprocess.run([str(binary)], check=True)

    def test_query_reply_timeout_and_rebind(self):
        """只有原Topic/token可完成一次，发布失败、超时、卸载和错误分页均返回失败。"""
        code = r'''
#include "ur_signaling_contacts.h"
#include "ur/device/mqtt_port.hpp"
#include "ur/device/envelope.hpp"
#include <cassert>
struct Port : ur::device::MqttPort {
    bool accepted=true;
    std::string body;
    bool connect(const ur::device::MqttConfig&) override { return true; }
    int subscribe(std::string_view) override { return 1; }
    bool poll() override { return true; }
    std::optional<ur::device::MqttEvent> take_event() override { return {}; }
    void disconnect() override {}
    bool publish(std::string_view topic, std::string_view value) override {
        assert(topic == "$thing/up/signaling/p/self"); body=value; return accepted;
    }
};
int main() {
    Port port;
    UrSignalingContacts client;
    std::vector<UrContactsResult> results;
    client.SetCallback([&](UrContactsResult result) { results.push_back(std::move(result)); });
    assert(!client.Query(1,0)); client.Attach(&port,"p/self","first");
    assert(!client.Query(1,1)); assert(client.Query(1,0)); client.Poll(100);
    ur::device::Envelope request;
    assert(ur::device::decode_envelope(port.body,request)==ur::device::DecodeResult::Ok);
    assert(request.method=="callContactsGet" && request.session.empty());
    ur::device::Envelope reply;
    reply.token=request.token; reply.method="requestAck"; reply.code=200; reply.request=false;
    reply.payload=R"({"requestMethod":"callContactsGet","result":{"offset":0,"hasMore":false,"contacts":[{"name":"小王","target":"p:other","deviceName":"客厅"}]}})";
    std::string body;
    assert(ur::device::encode_envelope(reply,body)==ur::device::DecodeResult::Ok);
    assert(!client.Handle("$thing/down/signaling/p/other",body));
    assert(client.Handle("$thing/down/signaling/p/self",body));
    assert(results.size()==1 && results.back().ok && results.back().revision==1 && results.back().contacts[0].target=="p:other");
    assert(!client.Handle("$thing/down/signaling/p/self",body));
    assert(client.Query(2,4)); client.Poll(200); client.Poll(10200);
    assert(results.size()==2 && !results.back().ok && results.back().offset==4);
    port.accepted=false; assert(client.Query(3,0)); client.Poll(10300);
    assert(results.size()==3 && !results.back().ok);
    port.accepted=true; assert(client.Query(4,0)); client.Poll(10400); client.Detach();
    assert(results.size()==4 && !results.back().ok && !client.Query(5,0));
    client.Attach(&port,"p/self","second"); assert(client.Query(6,0)); client.Poll(10500);
    assert(!client.Handle("$thing/down/signaling/p/self",body));
    assert(ur::device::decode_envelope(port.body,request)==ur::device::DecodeResult::Ok);
    reply.token=request.token; reply.payload=R"({"requestMethod":"callContactsGet","result":{"offset":4,"hasMore":false,"contacts":[]}})";
    assert(ur::device::encode_envelope(reply,body)==ur::device::DecodeResult::Ok);
    assert(client.Handle("$thing/down/signaling/p/self",body));
    assert(results.size()==5 && !results.back().ok);
    assert(client.Query(7,0)); client.Detach();
    assert(results.size()==6 && !results.back().ok && results.back().revision==7);
}
'''
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        runtime = ROOT.parent / 'device-runtime'
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            driver = folder / 'contacts.cc'
            driver.write_text(code)
            library = folder / 'cjson.o'
            subprocess.run(['gcc', '-I', str(cjson), '-c', str(cjson / 'cJSON.c'), '-o', str(library)], check=True)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(ROOT / 'main/signaling'), '-I', str(runtime / 'include'),
                            '-I', str(cjson), str(driver), str(ROOT / 'main/signaling/ur_signaling_contacts.cc'),
                            str(runtime / 'core/envelope.cpp'), str(runtime / 'core/json_parse.cpp'), str(library), '-o', str(folder / 'contacts')],
                           capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(folder / 'contacts')], check=True)
