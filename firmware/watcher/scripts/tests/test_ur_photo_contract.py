"""检查 Watcher 拍照生产接线、安全边界和构建入口没有回退。"""

import unittest
from pathlib import Path


WATCHER_ROOT = Path(__file__).resolve().parents[2]
UR_DEVICE = (WATCHER_ROOT / "main/unitedrhino/ur_device.c").read_text(encoding="utf-8")
UR_BRIDGE = (WATCHER_ROOT / "main/unitedrhino/ur_bridge.cc").read_text(encoding="utf-8")
CAMERA = (
    WATCHER_ROOT / "main/boards/sensecap-watcher/sscma_camera.cc"
).read_text(encoding="utf-8")
SDK_MQTT = (
    WATCHER_ROOT
    / "components/unitedrhino_iot_c_sdk/sdk_src/mqtt_client.c"
).read_text(encoding="utf-8")
CMAKE = (WATCHER_ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
APPLICATION = (WATCHER_ROOT / "main/application.cc").read_text(encoding="utf-8")
APPLICATION_HEADER = (WATCHER_ROOT / "main/application.h").read_text(encoding="utf-8")
AI_PROTOCOL = (WATCHER_ROOT / "main/protocols/ur_ai_protocol.cc").read_text(encoding="utf-8")
WATCHER_BOARD = (
    WATCHER_ROOT / "main/boards/sensecap-watcher/sensecap_watcher.cc"
).read_text(encoding="utf-8")


class UrPhotoContractTest(unittest.TestCase):
    """以源码合同补足 ESP-IDF/硬件边界，纯逻辑由 C++ 运行测试覆盖。"""

    def test_photo_memory_preserves_internal_dma(self):
        """拍照常驻队列和 worker 栈不能挤占已导致语音启动失败的内部 DMA。"""
        self.assertIn("xQueueCreateWithCaps(UR_ACTION_QUEUE_DEPTH", UR_DEVICE)
        self.assertIn("xQueueCreateWithCaps(UR_PHOTO_RESULT_QUEUE_DEPTH", UR_DEVICE)
        self.assertNotIn("xQueueCreate(UR_ACTION_QUEUE_DEPTH", UR_DEVICE)
        self.assertNotIn("xQueueCreate(UR_PHOTO_RESULT_QUEUE_DEPTH", UR_DEVICE)
        self.assertIn("xTaskCreateWithCaps(ur_photo_worker", UR_BRIDGE)
        worker = UR_BRIDGE.split("static void ur_photo_worker", 1)[1].split("#endif", 1)[0]
        self.assertIn("vTaskDeleteWithCaps(nullptr)", worker)
        self.assertNotIn("vTaskDelete(nullptr)", worker)
        self.assertIn('SetError("设备内存不足，请重启后重试")', AI_PROTOCOL)
        audio = (WATCHER_ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        codec_task = audio.split("/* Start the opus codec task */", 1)[1].split("#else", 1)[0]
        self.assertIn("#if CONFIG_UR_AI", codec_task)
        self.assertIn("xTaskCreateWithCaps(", codec_task)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", codec_task)
        self.assertIn("vTaskDeleteWithCaps(nullptr)", codec_task)

    def test_mqtt_callback_only_queues_action(self):
        """物模型回调只复制原文，解析、行为和拍照回执均由 MQTT 任务执行。"""
        callback = UR_DEVICE.split("static void ur_property_message_cb", 1)[1].split(
            "static void ur_process_property_payload", 1
        )[0]
        self.assertIn("xQueueSend(s_property_queue", callback)
        self.assertNotIn("cJSON_Parse", callback)
        self.assertNotIn("ur_enqueue_action", callback)
        self.assertIn("ur_drain_property_queue();", UR_DEVICE)
        self.assertIn("ur_enqueue_action(token, root)", UR_DEVICE)
        self.assertIn("ur_drain_action_queue();", UR_DEVICE)
        self.assertIn("ur_drain_photo_results();", UR_DEVICE)
        self.assertIn("ur_bridge_start_photo_action(item.token)", UR_DEVICE)
        self.assertNotIn("camera->Capture()", UR_DEVICE)

    def test_worker_owns_capture_upload_and_auth_clear(self):
        """相机与 HTTP 仅在独立 worker 中运行，认证在上传结束后立即清零。"""
        self.assertIn("static void ur_photo_worker", UR_BRIDGE)
        self.assertIn("camera->Capture()", UR_BRIDGE)
        self.assertIn("camera->UploadToPlatform", UR_BRIDGE)
        upload_pos = UR_BRIDGE.index("camera->UploadToPlatform")
        clear_pos = UR_BRIDGE.index("credentials.Clear();", upload_pos)
        finish_pos = UR_BRIDGE.index("finish(200", clear_pos)
        self.assertLess(upload_pos, clear_pos)
        self.assertLess(clear_pos, finish_pos)

    def test_camera_validates_jpeg_http_and_platform_response(self):
        """上传同时校验 JPEG、HTTP 200、平台 code 和 data.fileUri。"""
        self.assertIn("UrPhotoValidateJpeg", CAMERA)
        self.assertIn("*status_code != 200", CAMERA)
        self.assertIn('GetObjectItemCaseSensitive(root, "code")', CAMERA)
        self.assertIn('GetObjectItemCaseSensitive(data, "fileUri")', CAMERA)
        self.assertIn("UrPhotoValidateUploadResponse", CAMERA)

    def test_physical_e2e_stops_on_worker_failure(self):
        """上传明确失败立即结束本轮，不伪装为模型超时或继续等待播放。"""
        completion = APPLICATION.split("void Application::CompleteUrPhotoInput", 1)[1].split(
            "bool Application::RunUrPhotoE2eTest", 1
        )[0]
        self.assertIn("if (code != 200) ur_photo_e2e_error_.store(code)", completion)
        photo_test = APPLICATION.split("bool Application::RunUrPhotoE2eTest", 1)[1].split(
            "bool Application::RunUrAiE2eTest", 1
        )[0]
        self.assertIn("ur_photo_e2e_error_.store(0)", photo_test)
        self.assertIn("ur_photo_e2e_error_.load() == 0", photo_test)
        self.assertIn("while (result.audio_done", photo_test)

    def test_sdk_auth_does_not_export_device_secret(self):
        """SDK 只复制短期用户名和签名，不暴露 DeviceSecret。"""
        self.assertIn("IOT_MQTT_GenerateHTTPAuth", SDK_MQTT)
        self.assertIn("utils_hmac_sha256", SDK_MQTT)
        self.assertIn("memset(sign, 0, sizeof(sign))", SDK_MQTT)
        self.assertNotIn("Log_i(\"HTTP auth", SDK_MQTT)

    def test_photo_runtime_is_in_firmware_build(self):
        """可移植核心必须进入正式固件，而不只存在于测试程序。"""
        self.assertIn('"protocols/ur_photo_runtime.cc"', CMAKE)

    def test_image_input_uses_multimodal_envelope(self):
        """双击结果使用 devicesim 同款 text+image_url 信封并请求文字、语音。"""
        method = AI_PROTOCOL.split("bool UrAiProtocol::SendImageInput", 1)[1].split(
            "bool UrAiProtocol::SendAudio", 1
        )[0]
        self.assertIn('PublishMethod("inputSend"', method)
        self.assertIn('"请描述刚拍摄的画面"', method)
        self.assertIn('"image_url"', method)
        self.assertIn('"imageUrl"', method)
        self.assertIn('cJSON_CreateString("text")', method)
        self.assertIn('cJSON_CreateString("audio")', method)

    def test_double_click_has_dedicated_event_and_shared_worker(self):
        """双击不复用单击回调，并与 takePhoto 共享相机门禁和 worker。"""
        self.assertEqual(WATCHER_BOARD.count("BUTTON_DOUBLE_CLICK"), 1)
        double_click = WATCHER_BOARD.split("BUTTON_DOUBLE_CLICK", 1)[1].split("#endif", 1)[0]
        self.assertIn("DispatchInput(watcher::MenuInput::DoubleClick)", double_click)
        self.assertIn("case watcher::MenuAction::Photo: app.TakePhoto();", WATCHER_BOARD)
        self.assertIn("MAIN_EVENT_TAKE_PHOTO", APPLICATION_HEADER)
        self.assertIn("HandleTakePhotoEvent();", APPLICATION)
        self.assertIn("ContinueOpenPhotoChannel", APPLICATION)
        self.assertIn("ur_bridge_start_photo_input()", APPLICATION)
        self.assertIn("UrPhotoRequestKind::kAction", UR_BRIDGE)
        self.assertIn("UrPhotoRequestKind::kImageInput", UR_BRIDGE)
        self.assertEqual(UR_BRIDGE.count("static void ur_photo_worker"), 1)

    def test_busy_or_ota_rejects_before_interrupt(self):
        """相机忙或 OTA 时先提示，不取消正在进行的语音回复。"""
        handler = APPLICATION.split("void Application::HandleTakePhotoEvent", 1)[1].split(
            "void Application::ContinueOpenPhotoChannel", 1
        )[0]
        self.assertLess(handler.index("ur_bridge_photo_busy()"), handler.index("AbortSpeaking"))
        self.assertLess(handler.index("ota.state"), handler.index("AbortSpeaking"))


if __name__ == "__main__":
    unittest.main()
