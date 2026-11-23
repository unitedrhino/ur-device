// Watcher相机：图像采集、JPEG直送及按需RGB解码；回调时间用于测量完整呈现延迟。
#include "sscma_camera.h"
#include "image_base64.h"
#include "camera_transport_trace.h"
#include "board.h"
#include "config.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"
#include "ur_photo_runtime.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "application.h"
#include "camera_dma_io.h"
#include "signaling/watcher_call_jpeg.h"
#include "sscma_client_commands.h"

/* DmaHeadroomOk 预检 DMA 可用内存余量：SSCMA 的 SPI 传输用 PSRAM 缓冲时,
 * 驱动按传输长度临时申请内部 DMA 弹性缓冲；通话媒体高压期申请失败会踩中
 * IDF spi_master 错误路径空指针直接崩机（真机 Core1 LoadProhibited 实证）。
 * 余量不足时由调用方跳过本轮操作（保活/拍照重试即可），绝不进入驱动。 */
static bool DmaHeadroomOk(size_t need) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (largest < need) {
        ESP_LOGW("SscmaCamera", "skip spi op: dma largest %u < %u", (unsigned)largest,
                 (unsigned)need);
        return false;
    }
    return true;
}

#define TAG "SscmaCamera"

#define IMG_JPEG_BUF_SIZE 48 * 1024

static bool __himax_keepalive_check(sscma_client_handle_t client) {
    esp_err_t ret = ESP_OK;
    sscma_client_reply_t reply = {0};
    int retry = 3;
    while (retry--) {
        ret = sscma_client_request(client, CMD_PREFIX CMD_AT_ID CMD_QUERY CMD_SUFFIX, &reply, true,
                                   pdMS_TO_TICKS(2000));
        if (reply.payload != NULL) {
            sscma_client_reply_clear(&reply);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Himax keepalive check failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            return true;
        }
    }
    return false;
}

SscmaCamera::SscmaCamera(esp_io_expander_handle_t io_exp_handle) {
    sscma_client_io_spi_config_t spi_io_config = {0};
    spi_io_config.sync_gpio_num = BSP_SSCMA_CLIENT_SPI_SYNC;
    spi_io_config.cs_gpio_num = BSP_SSCMA_CLIENT_SPI_CS;
    spi_io_config.pclk_hz = BSP_SSCMA_CLIENT_SPI_CLK;
    spi_io_config.spi_mode = 0;
    // 上游注释要求传输间隔至少 4ms；1000Hz tick 下以 5ms 保留裕量，实测收包和呈现延迟。
    spi_io_config.wait_delay = 5;
    spi_io_config.user_ctx = NULL;
    spi_io_config.io_expander = io_exp_handle;
    spi_io_config.flags.sync_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)BSP_SSCMA_CLIENT_SPI_NUM,
                                &spi_io_config, &sscma_client_io_handle_);

    sscma_client_config_t sscma_client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    sscma_client_config.event_queue_size = CONFIG_SSCMA_EVENT_QUEUE_SIZE;
    sscma_client_config.tx_buffer_size = CONFIG_SSCMA_TX_BUFFER_SIZE;
    sscma_client_config.rx_buffer_size = CONFIG_SSCMA_RX_BUFFER_SIZE;
    sscma_client_config.process_task_stack = CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE;
    // 相机读取和回调迁移到核0，给核1的信令、音频和LVGL留出调度空间。
    sscma_client_config.process_task_affinity = 0;
    // 相机读取高于信令轮询，尽快形成完整JPEG；解码任务仍由低优先级队列保护音频。
    sscma_client_config.process_task_priority = 5;
    sscma_client_config.monitor_task_stack = CONFIG_SSCMA_MONITOR_TASK_STACK_SIZE;
    // 图像回调包含解析和发布，与读取任务同核，避免挤占信令显示核。
    sscma_client_config.monitor_task_affinity = 0;
    sscma_client_config.monitor_task_priority = 4;
    sscma_client_config.reset_gpio_num = BSP_SSCMA_CLIENT_RST;
    sscma_client_config.io_expander = io_exp_handle;
    sscma_client_config.flags.reset_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    dma_io_ = std::make_unique<CameraDmaIo>(sscma_client_io_handle_);
    transport_trace_ = std::make_unique<CameraTransportTrace>(dma_io_.get(), &streaming_);
    sscma_client_new(transport_trace_.get(), &sscma_client_config, &sscma_client_handle_);

    sscma_data_queue_ = xQueueCreate(1, sizeof(SscmaData));

    sscma_client_callback_t callback = {0};

    detection_state = SscmaCamera::IDLE;
    state_start_time = 0;
    need_start_cooldown = false;
    callback.on_event = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                           void* user_ctx) {
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (!self)
            return;

        char* img = NULL;
        int img_size = 0;
        int box_count = 0;
        sscma_client_box_t* boxes = NULL;
        int class_count = 0;
        sscma_client_class_t* classes = NULL;
        int point_count = 0;
        sscma_client_point_t* points = NULL;
        int model_type = 0;
        int obj_cnt = 0;

        int width = 0, height = 0;
        cJSON* data = cJSON_GetObjectItem(reply->payload, "data");
        if (data != NULL && cJSON_IsObject(data)) {
            cJSON* resolution = cJSON_GetObjectItem(data, "resolution");
            if (data != NULL && cJSON_IsArray(resolution) && cJSON_GetArraySize(resolution) == 2) {
                width = cJSON_GetArrayItem(resolution, 0)->valueint;
                height = cJSON_GetArrayItem(resolution, 1)->valueint;
            }
        }

        // 流预览接受原生 240 档及协处理器可能返回的 416 图，始终跳过自动唤醒检测。
        if (self->streaming_.load()) {
            if ((width == 240 && height == 240) || (width == 416 && height == 416)) {
                self->HandleStreamImage(reply);
            }
            return;
        }
        switch ((width + height)) {
            case (416 + 416): {
                bool is_object_detected = false;
                bool is_need_wake = false;

                // 定期更新检测配置参数，避免频繁NVS访问
                int64_t cur_tm = esp_timer_get_time();

                // 尝试获取检测框数据（目标检测模型）
                if (sscma_utils_fetch_boxes_from_reply(reply, &boxes, &box_count) == ESP_OK &&
                    box_count > 0) {
                    for (int i = 0; i < box_count; i++) {
                        ESP_LOGI(TAG, "[box %d]: x=%d, y=%d, w=%d, h=%d, score=%d, target=%d", i,
                                 boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h, boxes[i].score,
                                 boxes[i].target);
                        if (boxes[i].target == self->detect_target &&
                            boxes[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 0;
                            obj_cnt++;
                            break;
                        }
                    }
                    free(boxes);
                } else if (sscma_utils_fetch_classes_from_reply(reply, &classes, &class_count) ==
                               ESP_OK &&
                           class_count > 0) {
                    // 尝试获取分类数据（分类模型）
                    for (int i = 0; i < class_count; i++) {
                        ESP_LOGI(TAG, "[class %d]: target=%d, score=%d", i, classes[i].target,
                                 classes[i].score);
                        if (classes[i].target == self->detect_target &&
                            classes[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 1;
                            obj_cnt++;
                        }
                    }
                    free(classes);
                } else if (sscma_utils_fetch_points_from_reply(reply, &points, &point_count) ==
                               ESP_OK &&
                           point_count > 0) {
                    // 尝试获取关键点数据（姿态估计模型）
                    for (int i = 0; i < point_count; i++) {
                        ESP_LOGI(TAG, "[point %d]: x=%d, y=%d, z=%d, score=%d, target=%d", i,
                                 points[i].x, points[i].y, points[i].z, points[i].score,
                                 points[i].target);
                        if (points[i].target == self->detect_target &&
                            points[i].score > self->detect_threshold) {
                            is_object_detected = true;
                            model_type = 2;
                            obj_cnt++;
                        }
                    }
                    free(points);
                }

                // 如果需要开始冷却期，现在开始计时
                if (self->need_start_cooldown) {  // 回调暂停，标志保持，等待回调恢复后开始计时
                    self->state_start_time = cur_tm;
                    self->need_start_cooldown = false;
                    ESP_LOGI(TAG, "Starting cooldown timer");
                }

                // 状态机驱动的检测逻辑 - 只在人员出现时触发
                switch (self->detection_state) {
                    case SscmaCamera::IDLE:
                        if (is_object_detected) {
                            // 人员出现，开始验证（这是从无到有的转换）
                            self->detection_state = SscmaCamera::VALIDATING;
                            self->state_start_time = cur_tm;    // 记录物体出现时间
                            self->last_detected_time = cur_tm;  // 初始化最后检测时间
                            ESP_LOGI(TAG, "object appeared, starting validation");
                        }
                        break;

                    case SscmaCamera::VALIDATING:
                        if (is_object_detected) {
                            // 更新最后检测到的时间
                            self->last_detected_time = cur_tm;
                            // 检查是否验证足够时间
                            if ((cur_tm - self->state_start_time) >=
                                (self->detect_duration_sec * 1000000)) {
                                is_need_wake = true;
                            }
                        } else {
                            // 验证期间人员离开，检查去抖动时间
                            if (self->last_detected_time > 0 &&
                                (cur_tm - self->last_detected_time) >=
                                    self->detect_debounce_sec * 1000000LL) {
                                // 去抖动时间已过，确认人员已离开，回到空闲
                                self->detection_state = SscmaCamera::IDLE;
                                self->last_detected_time = 0;
                                ESP_LOGI(TAG,
                                         "object left during validation (debounced), back to idle");
                            }
                        }
                        break;

                    case SscmaCamera::COOLDOWN:
                        // 冷却期，需要满足两个条件：1)object离开 2)过了15秒
                        if (!is_object_detected &&
                            (cur_tm - self->state_start_time) >=
                                (self->detect_invoke_interval_sec * 1000000LL)) {
                            // object离开且冷却时间到，回到空闲状态
                            self->detection_state = SscmaCamera::IDLE;
                            ESP_LOGI(TAG,
                                     "Cooldown complete and object left, back to idle - ready for "
                                     "next appearance");
                        }
                        // 其他情况继续保持冷却状态
                        break;
                }

                if (is_need_wake) {
                    ESP_LOGI(TAG,
                             "Validation complete, triggering conversation (type=%d, res=%dx%d)",
                             self->detect_target, width, height);

                    // 触发对话
                    std::string wake_word;
                    if (model_type == 0) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    } else if (model_type == 1) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    } else if (model_type == 2) {
                        std::string cached_target_name = "object";
                        if (self->model != NULL &&
                            self->model->classes[self->detect_target] != NULL) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " +
                                    cached_target_name + " detected </detect>";
                    }
                    printf("wake_word:%s\n", wake_word.c_str());
                    Application::GetInstance().WakeWordInvoke(wake_word);

                    // 进入冷却状态，标记需要开始冷却期；如下变量将在会话结束后被使用，等待回调恢复后开始计时
                    self->detection_state = SscmaCamera::COOLDOWN;
                    self->need_start_cooldown = true;
                }
            } break;
            case (640 + 480):

                if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK) {
                    ESP_LOGI(TAG, "image_size: %d\n", img_size);
                    // 将数据通过队列发送出去
                    SscmaData data;
                    data.img = (uint8_t*)img;
                    data.len = img_size;

                    // 清空队列，保证只保存最新的数据
                    SscmaData dummy;
                    while (xQueueReceive(self->sscma_data_queue_, &dummy, 0) == pdPASS) {
                        if (dummy.img) {
                            heap_caps_free(dummy.img);
                        }
                    }
                    xQueueSend(self->sscma_data_queue_, &data, 0);
                    // 注意：img 的释放由接收方负责
                }
                break;
            default:
                ESP_LOGI(TAG, "unknown resolution");
                break;
        }
    };
    callback.on_connect = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                             void* user_ctx) {
        ESP_LOGI(TAG, "SSCMA client connected");
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (self) {
            self->sscma_restarted_ = true;
        }
    };

    callback.on_log = [](sscma_client_handle_t client, const sscma_client_reply_t* reply,
                         void* user_ctx) { ESP_LOGI(TAG, "log: %s\n", reply->data); };

    sscma_client_register_callback(sscma_client_handle_, &callback, this);

    sscma_client_init(sscma_client_handle_);

    ESP_LOGI(TAG, "SSCMA client initialized");
    // 设置分辨率
    // 3 = 640x480
    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        sscma_client_del(sscma_client_handle_);
        sscma_client_handle_ = NULL;
        return;
    }

    // 获取设备信息
    sscma_client_info_t* info;
    if (sscma_client_get_info(sscma_client_handle_, &info, true) == ESP_OK) {
        ESP_LOGI(TAG, "Device Info - ID: %s, Name: %s", info->id ? info->id : "NULL",
                 info->name ? info->name : "NULL");
    }
    // 初始化JPEG数据的内存
    jpeg_data_.len = 0;
    jpeg_data_.buf = (uint8_t*)heap_caps_malloc(IMG_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ;
    if (jpeg_data_.buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG buffer");
        return;
    }

    // 初始化JPEG解码
    jpeg_error_t err;
    jpeg_dec_config_t config = {.output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
                                .rotate = JPEG_ROTATE_0D};
    err = jpeg_dec_open(&config, &jpeg_dec_);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        return;
    }
    jpeg_io_ = (jpeg_dec_io_t*)heap_caps_malloc(sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM);
    if (!jpeg_io_) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG IO");
        jpeg_dec_close(jpeg_dec_);
        return;
    }
    memset(jpeg_io_, 0, sizeof(jpeg_dec_io_t));

    jpeg_out_ = (jpeg_dec_header_info_t*)heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t),
                                                                 MALLOC_CAP_SPIRAM);
    if (!jpeg_out_) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output header");
        heap_caps_free(jpeg_io_);
        jpeg_dec_close(jpeg_dec_);
        return;
    }
    memset(jpeg_out_, 0, sizeof(jpeg_dec_header_info_t));

    // 初始化预览图片的内存
    memset(&preview_image_, 0, sizeof(preview_image_));
    preview_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image_.header.flags = LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE;
    preview_image_.header.w = 640;
    preview_image_.header.h = 480;

    preview_image_.header.stride = preview_image_.header.w * 2;
    preview_image_.data_size = preview_image_.header.w * preview_image_.header.h * 2;
    preview_image_.data = (uint8_t*)jpeg_calloc_align(preview_image_.data_size, 16);
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for preview image");
        return;
    }

    sscma_client_set_model(sscma_client_handle_, 4);
    model_class_cnt = 0;
    if (sscma_client_get_model(sscma_client_handle_, &model, true) == ESP_OK) {
        printf("ID: %d\n", model->id ? model->id : -1);
        printf("UUID: %s\n", model->uuid ? model->uuid : "N/A");
        printf("Name: %s\n", model->name ? model->name : "N/A");
        printf("Version: %s\n", model->ver ? model->ver : "N/A");
        printf("URL: %s\n", model->url ? model->url : "N/A");
        printf("Checksum: %s\n", model->checksum ? model->checksum : "N/A");
        printf("Classes:\n");
        if (model->classes[0] != NULL) {
            for (int i = 0; model->classes[i] != NULL; i++) {
                printf("  - %s\n", model->classes[i]);
                model_class_cnt++;
            }
        } else {
            printf("  N/A\n");
        }
    } else {
        printf("get model failed\n");
    }

    ESP_LOGI(TAG, "initialize mcp tools");
    InitializeMcpTools();

    xTaskCreate(
        [](void* arg) {
            auto this_ = (SscmaCamera*)arg;
            bool is_inference = false;
            int64_t last_keepalive_time = esp_timer_get_time();
            while (true) {
                if (this_->sscma_restarted_) {
                    ESP_LOGI(TAG, "SSCMA restarted detected");
                    this_->sscma_restarted_ = false;
                    is_inference = false;
                }

                if (esp_timer_get_time() - last_keepalive_time > 10 * 1000000) {
                    last_keepalive_time = esp_timer_get_time();
                    if (DmaHeadroomOk(4096) &&
                        !__himax_keepalive_check(this_->sscma_client_handle_)) {
                        ESP_LOGE(TAG, "restart himax");
                        sscma_client_reset(this_->sscma_client_handle_);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }

                if (this_->inference_en &&
                    Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (!is_inference) {
                        ESP_LOGI(TAG, "Start inference (enable=1)");
                        sscma_client_break(this_->sscma_client_handle_);
                        sscma_client_set_model(this_->sscma_client_handle_, 4);
                        sscma_client_set_sensor(this_->sscma_client_handle_, 1, 1,
                                                true);  // 设置分辨率 416X416
                        sscma_client_invoke(this_->sscma_client_handle_, -1, false, true);
                        is_inference = true;
                    }
                } else if (is_inference &&
                           (!this_->inference_en ||
                            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)) {
                    ESP_LOGI(TAG, "Stop inference (enable=%d state=%d)", this_->inference_en,
                             Application::GetInstance().GetDeviceState());
                    is_inference = false;
                    sscma_client_break(this_->sscma_client_handle_);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        },
        "sscma_camera", 4096, this, 1, nullptr);
}

SscmaCamera::~SscmaCamera() {
    if (preview_image_.data) {
        heap_caps_free((void*)preview_image_.data);
        preview_image_.data = nullptr;
    }
    for (int i = 0; i < 2; ++i) {
        if (stream_frame_[i] != nullptr) {
            heap_caps_free(stream_frame_[i]);
            stream_frame_[i] = nullptr;
        }
    }
    if (sscma_client_handle_) {
        sscma_client_del(sscma_client_handle_);
    }
    heap_caps_free(stream_timings_);
    stream_timings_ = nullptr;
    if (sscma_data_queue_) {
        vQueueDelete(sscma_data_queue_);
    }
    if (jpeg_data_.buf) {
        heap_caps_free(jpeg_data_.buf);
        jpeg_data_.buf = nullptr;
    }
    if (jpeg_dec_) {
        jpeg_dec_close(jpeg_dec_);
        jpeg_dec_ = nullptr;
    }
    if (jpeg_io_) {
        heap_caps_free(jpeg_io_);
        jpeg_io_ = nullptr;
    }
    if (jpeg_out_) {
        heap_caps_free(jpeg_out_);
        jpeg_out_ = nullptr;
    }
}

void SscmaCamera::InitializeMcpTools() {
    Settings settings("model", false);
    detect_threshold = settings.GetInt("threshold", 75);
    detect_invoke_interval_sec = settings.GetInt("interval", 8);
    detect_duration_sec = settings.GetInt("duration", 2);
    detect_target = settings.GetInt("target", 0);
    inference_en = settings.GetInt("enable", 0);

    auto& mcp_server = McpServer::GetInstance();
    // 获取模型参数配置
    mcp_server.AddTool("self.model.param_get",
                       "获取当前视觉模型检测的参数配置信息。\n"
                       "返回结果包含：\n"
                       "  `threshold`: 检测置信度阈值 (0-100)，低于此值的检测结果将被忽略；\n"
                       "  `interval`: 触发对话后的冷却时间(秒)，防止频繁打断；\n"
                       "  `duration`: 持续检测确认时间(秒)；\n"
                       "  `target`: 当前关注的检测目标索引。",
                       PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                           Settings settings("model", false);
                           int threshold = settings.GetInt("threshold", 75);
                           int interval = settings.GetInt("interval", 8);
                           int duration = settings.GetInt("duration", 2);
                           int target_type = settings.GetInt("target", 0);

                           std::string result = "{\"threshold\":" + std::to_string(threshold) +
                                                ",\"interval\":" + std::to_string(interval) +
                                                ",\"duration\":" + std::to_string(duration) +
                                                ",\"target_type\":" + std::to_string(target_type) +
                                                "}";
                           return result;
                       });

    // 设置模型参数配置
    mcp_server.AddTool(
        "self.model.param_set",
        "配置视觉模型检测参数。当用户希望调整检测灵敏度、频率或特定目标时使用。\n"
        "参数(均为可选，未提供的参数将保持当前设置不变)：\n"
        "  `threshold`: 置信度阈值 (0-100)。提高此值可减少误报，但可能漏检；\n"
        "  `interval`: 冷却时间(秒)。设置对话结束后多久内不再触发检测；\n"
        "  `duration`: 持续检测时间(秒)。\n"
        "  `target`: 设置检测目标的索引 ID。",
        PropertyList({Property("threshold", kPropertyTypeInteger, -1, -1, 100),
                      Property("interval", kPropertyTypeInteger, -1, -1, 60),
                      Property("duration", kPropertyTypeInteger, -1, -1, 60),
                      Property("target", kPropertyTypeInteger, -1, -1,
                               this->model_class_cnt > 0 ? this->model_class_cnt - 1 : 255)}),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            int threshold = properties["threshold"].value<int>();
            if (threshold != -1) {
                settings.SetInt("threshold", threshold);
                this->detect_threshold = threshold;
                ESP_LOGI(TAG, "Set detection threshold to %d", threshold);
            }

            int interval = properties["interval"].value<int>();
            if (interval != -1) {
                settings.SetInt("interval", interval);
                this->detect_invoke_interval_sec = interval;
                ESP_LOGI(TAG, "Set detection interval to %d", interval);
            }

            int duration = properties["duration"].value<int>();
            if (duration != -1) {
                settings.SetInt("duration", duration);
                this->detect_duration_sec = duration;
            }

            int target = properties["target"].value<int>();
            if (target != -1) {
                settings.SetInt("target", target);
                this->detect_target = target;
                ESP_LOGI(TAG, "Set detection target to %d", target);
            }

            return "{\"status\": \"success\", \"message\": \"Detection configuration updated\"}";
        });

    // 推理开关获取
    mcp_server.AddTool(
        "self.model.enable",
        "控制视觉推理(摄像头检测)功能的开启与关闭，或查询当前状态。\n"
        "当用户指令涉及'开启/关闭推理'、'开始/停止检测'时使用。\n"
        "参数：\n"
        "  `enable`: (可选) 整数。1=开启推理，0=关闭推理。若省略则返回当前开关状态。",
        PropertyList({Property("enable", kPropertyTypeInteger, inference_en, 0, 1)}),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            int en = properties["enable"].value<int>();
            settings.SetInt("enable", en);
            this->inference_en = en;
            ESP_LOGI(TAG, "Set inference enable to %d", en);
            // 返回当前配置
            int cur_en = settings.GetInt("enable", this->inference_en);
            return std::string("{\"enable\":") + std::to_string(cur_en) + "}";
        });
}

void SscmaCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

// EnsureJpegDecoder 按需恢复RGB解码上下文；调用者持有stream_mutex_，失败可安全重试。
bool SscmaCamera::EnsureJpegDecoder() {
    // JPEG直送期间禁止在途单拍重新占用RGB工作区。
    if (jpeg_only_.load())
        return false;
    if (jpeg_dec_ != nullptr)
        return true;
    // decoder 先由本作用域持有，只有完整创建成功才发布给后续预览或单拍。
    jpeg_dec_handle_t decoder = nullptr;
    // config 保持既有RGB565小端和零旋转，模式切换不改变图像合同。
    jpeg_dec_config_t config = {.output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
                                .rotate = JPEG_ROTATE_0D};
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) {
        if (decoder != nullptr)
            jpeg_dec_close(decoder);
        return false;
    }
    jpeg_dec_ = decoder;
    return true;
}

/* EnsureStreamBuffers 为 240 预览及 416 兼容输出预分配两块最大尺寸 RGB565 PSRAM 双缓冲。
 * 解码器要求输出缓冲 16 字节对齐，必须用 aligned_alloc，否则后续帧解码直接失败。 */
bool SscmaCamera::EnsureStreamBuffers() {
    const size_t bytes = static_cast<size_t>(416) * 416 * 2;
    for (int i = 0; i < 2; ++i) {
        if (stream_frame_[i] == nullptr) {
            stream_frame_[i] = static_cast<uint8_t*>(heap_caps_aligned_alloc(
                16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (stream_frame_[i] == nullptr) {
                ESP_LOGE(TAG, "stream frame buffer %d alloc failed", i);
                return false;
            }
        }
    }
    // 已发布帧的尺寸由解码成功时更新，重复检查容量不能临时改回 416。
    return true;
}

/* HandleStreamImage 在图像回调内转换Base64；原生JPEG直送只校验头与边界，RGB预览再解码：
 * 与官方 view_image_preview_flush 同构，省掉队列与另一任务取帧的一拍延迟。
 * 双缓冲保证显示/视频读取的是完整帧，解码不会覆盖正在使用的那块。 */
void SscmaCamera::HandleStreamImage(const sscma_client_reply_t* reply) {
    const int64_t received_us =
        esp_timer_get_time();  // 相机图像回调入口，用于测量到 DMA 完成的延迟。
    const auto interval_ms = stream_frame_interval_ms_.load(std::memory_order_relaxed);
    if (jpeg_only_.load(std::memory_order_relaxed) && interval_ms != 0) {
        const auto last = stream_last_published_us_.load(std::memory_order_relaxed);
        if (last != 0 && received_us - last < static_cast<int64_t>(interval_ms) * 1000)
            return;  // 通话只保留最新节拍，避免对过期相机帧重复做Base64和JPEG解析。
    }
    if (transport_trace_) transport_trace_->MarkFrame(received_us);
    char* img = nullptr;
    int img_size = 0;
    if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) != ESP_OK || img == nullptr ||
        img_size <= 0) {
        return;
    }
    const int64_t fetched_us = esp_timer_get_time();  // 图像字符串提取已完成。
    std::lock_guard<std::mutex> lock(stream_mutex_);
    const int64_t locked_us = esp_timer_get_time();  // 已取得共用解码器所有权。
    // jpeg_only 固定本帧所有权；直送不创建RGB解码器，停流后的在途回调不重新申请它。
    const bool jpeg_only = jpeg_only_.load();
    if (!streaming_.load() || !EnsureStreamBuffers() || jpeg_data_.buf == nullptr ||
        jpeg_io_ == nullptr || jpeg_out_ == nullptr || (!jpeg_only && !EnsureJpegDecoder())) {
        free(img);
        return;
    }
    const int64_t prepared_us = esp_timer_get_time();  // 缓冲与解码器检查结束。
    size_t out_len = 0;
    if (jpeg_only_.load()) stream_jpeg_size_ = 0;  // 失败转换会覆盖输入缓冲，不能再发布旧JPEG。
    const bool image_decoded = watcher::DecodeImageBase64(
        jpeg_data_.buf, IMG_JPEG_BUF_SIZE, &out_len,
        reinterpret_cast<const unsigned char*>(img), static_cast<size_t>(img_size));  // 仅图像路径查表转换。
    free(img);
    const int64_t base64_done_us = esp_timer_get_time();  // Base64 转换和临时字符串释放结束。
    if (!image_decoded || out_len == 0) {
        return;
    }
    if (jpeg_only) {
        // 压缩直送只检查完整边界、受支持的帧头和240尺寸，不为读尺寸持有RGB工作区。
        const size_t jpeg_size = UrPhotoJpegPayloadSize(jpeg_data_.buf, out_len);
        int width = 0, height = 0;  // width/height 仅发布固定240原生JPEG，拒绝异常大图。
        if (jpeg_size == 0 || jpeg_size > 48 * 1024 ||
            !WatcherRomJpegDimensions(jpeg_data_.buf, jpeg_size, width, height) || width != 240 ||
            height != 240)
            return;
        stream_jpeg_size_ = jpeg_size;
        stream_seq_.fetch_add(1, std::memory_order_release);
        stream_last_published_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
        return;
    }
    jpeg_io_->inbuf = jpeg_data_.buf;
    jpeg_io_->inbuf_len = static_cast<int>(out_len);
    if (jpeg_dec_parse_header(jpeg_dec_, jpeg_io_, jpeg_out_) < 0) {
        return;
    }
    // 先核对解码输出尺寸，禁止把异常大图写进固定 416 上限的双缓冲。
    int output_bytes = 0;  // 解码器要求的实际输出容量，包含其内部对齐要求。
    if (jpeg_out_->width == 0 || jpeg_out_->height == 0 || jpeg_out_->width > 416 ||
        jpeg_out_->height > 416 ||
        jpeg_dec_get_outbuf_len(jpeg_dec_, &output_bytes) != JPEG_ERR_OK || output_bytes <= 0 ||
        output_bytes > 416 * 416 * 2) {
        ESP_LOGW(TAG, "stream JPEG dimensions exceed frame buffer");
        return;
    }
    const int next = 1 - stream_write_.load(std::memory_order_relaxed);
    jpeg_io_->outbuf = stream_frame_[next];
    const int consumed = jpeg_io_->inbuf_len - jpeg_io_->inbuf_remain;
    jpeg_io_->inbuf = jpeg_data_.buf + consumed;
    jpeg_io_->inbuf_len = jpeg_io_->inbuf_remain;
    const int64_t header_done_us = esp_timer_get_time();  // 头解析和输出容量/参数检查结束。
    if (jpeg_dec_process(jpeg_dec_, jpeg_io_) != ESP_OK) {
        return;
    }
    const int64_t decoded_us = esp_timer_get_time();  // 仅此前 jpeg_dec_process 属于 JPEG 解码段。
    stream_w_ = jpeg_out_->width;
    stream_h_ = jpeg_out_->height;
    if (stream_seq_.load(std::memory_order_relaxed) == 0) {
        ESP_LOGI(TAG, "stream decoded frame %ux%u bytes=%d", stream_w_, stream_h_, output_bytes);
    }
    stream_write_.store(next, std::memory_order_release);
    const uint32_t sequence =
        stream_seq_.fetch_add(1, std::memory_order_release) + 1;  // 与本次发布对应的相机序号。
    const int64_t published_us = esp_timer_get_time();            // 原始帧已可供显示和视频读取。
    auto display = preview_enabled_ ? Board::GetInstance().GetDisplay() : nullptr;
    if (display != nullptr) {
        /* 与官方同构：回调解码完成后直接持 LVGL 锁刷新图像，零拷贝、无队列、
         * 无 50ms 轮询等待；下帧解码写另一块缓冲，不影响本次显示。 */
        display->SetStreamPreview(stream_frame_[next], stream_w_, stream_h_, received_us);
    }
    const int64_t finished_us = esp_timer_get_time();  // 显示入口已经返回，异步呈现由 LCD 另计。
    RecordStreamTiming({sequence, static_cast<uint32_t>(fetched_us - received_us),
                        static_cast<uint32_t>(locked_us - fetched_us),
                        static_cast<uint32_t>(prepared_us - locked_us),
                        static_cast<uint32_t>(base64_done_us - prepared_us),
                        static_cast<uint32_t>(header_done_us - base64_done_us),
                        static_cast<uint32_t>(decoded_us - header_done_us),
                        static_cast<uint32_t>(published_us - decoded_us),
                        static_cast<uint32_t>(finished_us - published_us),
                        static_cast<uint32_t>(finished_us - received_us), 0,
                        static_cast<uint32_t>(img_size), static_cast<uint32_t>(out_len)},
                       received_us);
}

// RecordStreamTiming 只在诊断已启用时保存成功帧；调用者持有 stream_mutex_，实时路径不打印。
void SscmaCamera::RecordStreamTiming(const StreamFrameTiming& sample, int64_t received_us) {
    if (!stream_timings_)
        return;
    stream_timings_[stream_timing_write_] = sample;
    stream_timings_[stream_timing_write_].interval_us =
        stream_previous_received_us_
            ? static_cast<uint32_t>(received_us - stream_previous_received_us_)
            : 0;
    stream_previous_received_us_ = received_us;
    stream_timing_write_ = (stream_timing_write_ + 1) % kStreamTimingCapacity;
    stream_timing_count_ = std::min(stream_timing_count_ + 1, kStreamTimingCapacity);
}

// PrintStreamPerformance 持锁取得快照后再导出；reset 仅控制诊断，不改变取帧和图像参数。
void SscmaCamera::PrintStreamPerformance(bool reset) {
    std::vector<StreamFrameTiming> samples;  // 串口输出使用独立快照，避免占用解码器互斥锁。
    bool enabled = false;                    // PSRAM 分配失败时显式标记未启用，仍允许相机运行。
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (reset) {
            if (!stream_timings_)
                stream_timings_ = static_cast<StreamFrameTiming*>(
                    heap_caps_calloc(kStreamTimingCapacity, sizeof(StreamFrameTiming),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            stream_timing_write_ = stream_timing_count_ = 0;
            stream_previous_received_us_ = 0;
        } else if (stream_timings_) {
            samples.reserve(stream_timing_count_);
            const size_t first =
                (stream_timing_write_ + kStreamTimingCapacity - stream_timing_count_) %
                kStreamTimingCapacity;  // 最旧样本位置。
            for (size_t index = 0; index < stream_timing_count_; ++index)
                samples.push_back(stream_timings_[(first + index) % kStreamTimingCapacity]);
        }
        enabled = stream_timings_ != nullptr;
    }
    printf("[UR_CAMERA_PERF] BEGIN count=%u enabled=%d reset=%d\n",
           static_cast<unsigned>(samples.size()), enabled, reset);
    for (const auto& sample : samples) {
        printf("[UR_CAMERA_PERF] %u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               static_cast<unsigned>(sample.sequence), static_cast<unsigned>(sample.extract_us),
               static_cast<unsigned>(sample.mutex_us), static_cast<unsigned>(sample.prepare_us),
               static_cast<unsigned>(sample.base64_us), static_cast<unsigned>(sample.header_us),
               static_cast<unsigned>(sample.jpeg_us), static_cast<unsigned>(sample.publish_us),
               static_cast<unsigned>(sample.display_submit_us),
               static_cast<unsigned>(sample.callback_us), static_cast<unsigned>(sample.interval_us),
               static_cast<unsigned>(sample.base64_bytes),
               static_cast<unsigned>(sample.jpeg_bytes));
    }
    printf("[UR_CAMERA_PERF] END\n");
    if (transport_trace_) transport_trace_->Print(reset);
}

// Capture 取得一张有效JPEG，按需恢复RGB预览；失败返回false，预览失败不丢弃有效JPEG。
bool SscmaCamera::Capture() {
    SscmaData data;
    int ret = 0;

    if (sscma_client_handle_ == nullptr) {
        ESP_LOGE(TAG, "SSCMA client handle is not initialized");
        return false;
    }

    /* 拍照 SPI 读回 JPEG 需 DMA 弹性缓冲；余量不足跳过本轮，由视频任务重试。 */
    if (!DmaHeadroomOk(16384)) {
        return false;
    }
    if (streaming_.load()) {
        /* 流模式解码与显示已在图像回调内完成，这里只等待新帧序号推进，
         * 让视频上行拿到最新一帧；数据未到最多等 1 秒。 */
        const uint32_t start_seq = stream_seq_.load(std::memory_order_acquire);
        const int64_t deadline = esp_timer_get_time() + 1000000;
        while (streaming_.load() && esp_timer_get_time() < deadline) {
            if (stream_seq_.load(std::memory_order_acquire) != start_seq) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        ESP_LOGE(TAG, "Failed to receive JPEG data from SSCMA client");
        return false;
    }
    // 非流模式按需单拍：设置 640x480 后触发一次 SAMPLE，再从队列取图。
    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        return false;
    }
    ESP_LOGI(TAG, "Capturing image...");
    // himax 可能有缓存数据, 只获取最新的照片即可.
    if (sscma_client_sample(sscma_client_handle_, 1)) {
        ESP_LOGE(TAG, "Failed to capture image from SSCMA client");
        return false;
    }
    /* 原固定等待 500ms 把取帧节拍压到 ~2fps；SSCMA 数据到达会进队列，
     * 直接阻塞等队列即可，数据未到最多等 1 秒，实测可提升上行帧率。 */
    if (xQueueReceive(sscma_data_queue_, &data, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to receive JPEG data from SSCMA client");
        return false;
    }

    if (jpeg_data_.buf == nullptr) {
        heap_caps_free(data.img);
        return false;
    }
    /* 单拍与预览流共用 JPEG 解码器：流停后仍可能在途回调，串行化解码段。 */
    std::lock_guard<std::mutex> decode_lock(stream_mutex_);

    ret = mbedtls_base64_decode(jpeg_data_.buf, IMG_JPEG_BUF_SIZE, &jpeg_data_.len, data.img,
                                data.len);
    if (ret != 0 || jpeg_data_.len == 0) {
        ESP_LOGE(TAG, "Failed to decode base64 image data, ret: %d, output_len: %zu", ret,
                 jpeg_data_.len);
        heap_caps_free(data.img);
        return false;
    }
    heap_caps_free(data.img);

    // SSCMA 的零填充属于传输缓冲，不是 JPEG；预览和上传统一使用真实图片边界。
    const size_t payload_size = UrPhotoJpegPayloadSize(jpeg_data_.buf, jpeg_data_.len);
    if (payload_size == 0) {
        ESP_LOGW(TAG, "Invalid camera JPEG boundary");
        jpeg_data_.len = 0;
        return false;
    }
    ESP_LOGI(TAG, "photo stage=jpeg_decoded bytes=%u padding=%u",
             static_cast<unsigned>(payload_size),
             static_cast<unsigned>(jpeg_data_.len - payload_size));
    jpeg_data_.len = payload_size;
    if (!EnsureJpegDecoder() || !jpeg_io_ || !jpeg_out_ || !preview_image_.data) {
        return true;
    }
    jpeg_io_->inbuf = jpeg_data_.buf;
    jpeg_io_->inbuf_len = jpeg_data_.len;
    ret = jpeg_dec_parse_header(jpeg_dec_, jpeg_io_, jpeg_out_);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to parse JPEG header, ret: %d", ret);
        return true;
    }
    jpeg_io_->outbuf = (unsigned char*)preview_image_.data;
    int inbuf_consumed = jpeg_io_->inbuf_len - jpeg_io_->inbuf_remain;
    jpeg_io_->inbuf = jpeg_data_.buf + inbuf_consumed;
    jpeg_io_->inbuf_len = jpeg_io_->inbuf_remain;

    ret = jpeg_dec_process(jpeg_dec_, jpeg_io_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG image, ret: %d", ret);
        preview_valid_ = false;
        return true;
    }
    preview_valid_ = true;
    /* 流模式返回实际预览尺寸，单拍是 640x480；这里按实际解码尺寸更新
     * lv_img 头，预览缩放和视频转换都依赖它。缓冲按 640x480 预分配，两种尺寸均安全。 */
    preview_image_.header.w = jpeg_out_->width;
    preview_image_.header.h = jpeg_out_->height;
    preview_image_.header.stride = preview_image_.header.w * 2;
    preview_image_.data_size = static_cast<uint32_t>(preview_image_.header.stride) *
                               preview_image_.header.h;
    ESP_LOGI(TAG, "photo stage=preview_decoded");

    // 通话编码期间跳过预览显示：每帧 614KB 拷贝和 LCD 刷新会挤占编码任务。
    auto display = preview_enabled_ ? Board::GetInstance().GetDisplay() : nullptr;
    if (display != nullptr) {
        uint16_t w = preview_image_.header.w;
        uint16_t h = preview_image_.header.h;
        size_t image_size = w * h * 2;
        size_t stride = preview_image_.header.w * 2;

        uint8_t* data = (uint8_t*)heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for display image");
            return true;
        }
        memcpy(data, preview_image_.data, image_size);

        /* 直接交显示层：CustomLcdDisplay::SetPreviewImage 只登记快照并唤醒
         * LVGL 定时器，真正的 lv_image_set_src 在显示任务执行；
         * 不再经 Application::Schedule 多等一轮主任务事件循环。 */
        display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
            data, image_size, w, h, stride, LV_COLOR_FORMAT_RGB565));
    }
    return true;
}
bool SscmaCamera::SetHMirror(bool enabled) { return false; }

bool SscmaCamera::SetVFlip(bool enabled) { return false; }

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 *
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 *
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return 成功时返回服务器的 JSON 响应，失败时返回可读错误信息
 *
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
// SendRawCommand 向 HX6538 发送原始 AT 探测命令并打印应答，仅用于本地能力确认。
void SscmaCamera::SendRawCommand(const char* command) {
    if (sscma_client_handle_ == nullptr || command == nullptr) {
        printf("[SSCMA] unavailable\n");
        return;
    }
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+%s\r\n", command);
    sscma_client_reply_t reply = {};
    const esp_err_t err = sscma_client_request(sscma_client_handle_, cmd, &reply, true, pdMS_TO_TICKS(2000));
    if (err != ESP_OK) {
        printf("[SSCMA] %s err=%d\n", cmd, static_cast<int>(err));
        return;
    }
    char* text = reply.payload != nullptr ? cJSON_PrintUnformatted(reply.payload) : nullptr;
    printf("[SSCMA] %s -> %s\n", cmd, text != nullptr ? text : "(empty)");
    if (text != nullptr) cJSON_free(text);
    sscma_client_reply_clear(&reply);
}

// StartStream 本地镜子连续采样 240 图像，省去无关推理；独立视频仍从 416 推理流启动。
// 保留既有本地模型与单拍、待机检测配置，所有回调按实际 JPEG 尺寸交付。
void SscmaCamera::StartStream() {
    if (sscma_client_handle_ == nullptr || streaming_.exchange(true))
        return;
    if (!EnsureStreamBuffers()) {
        ESP_LOGW(TAG, "stream buffers unavailable");
        streaming_.store(false);
        return;
    }
    sscma_client_break(sscma_client_handle_);
    sscma_client_set_model(sscma_client_handle_, 4);
    // 本地镜子与JPEG直送使用240档；既有H264上行仍从416档启动。
    const bool local_preview = preview_enabled_ || jpeg_only_.load();  // JPEG直送也使用240采样。
    const int sensor_option = local_preview ? 0 : 1;  // 镜子与JPEG直送选择低分辨率。
    if (sscma_client_set_sensor(sscma_client_handle_, 1, sensor_option, true) != ESP_OK) {
        ESP_LOGW(TAG, "stream set sensor failed");
        streaming_.store(false);
        return;
    }
    // 镜子不消费识别结果，直接连续 SAMPLE 减少相机芯片侧的帧间等待。
    const esp_err_t start_result =
        local_preview
            ? sscma_client_sample(sscma_client_handle_, -1)
            : sscma_client_invoke(sscma_client_handle_, -1, false, true);  // 视频保留原取帧方式。
    if (start_result != ESP_OK) {
        ESP_LOGW(TAG, "stream start failed: %d", static_cast<int>(start_result));
        streaming_.store(false);
        return;
    }
    ESP_LOGI(TAG, "preview stream started sensor_option=%d mode=%s", sensor_option,
             local_preview ? "sample" : "invoke");
}

// StopStream 用 AT+BREAK 中止连续图像流；streaming_ 清空后 Capture 回到按需单拍，
// 避免退出镜子/视频后协处理器仍持续推理占带宽。
void SscmaCamera::StopStream() {
    if (sscma_client_handle_ == nullptr || !streaming_.exchange(false)) return;
    sscma_client_break(sscma_client_handle_);
    jpeg_only_.store(false);
    ESP_LOGI(TAG, "camera stream stopped");
}

// StartJpegStream 只能在相机空闲时调用，避免接管正在运行的镜子或通话。
void SscmaCamera::StartJpegStream() {
    if (streaming_.load()) return;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        stream_jpeg_size_ = 0;
        jpeg_only_.store(true);  // 模式与解码器释放在同一互斥区发布。
        // 原生JPEG上行不做RGB解码，归还闲置上下文为音频DMA和对端画面保留内部内存。
        if (jpeg_dec_ != nullptr) {
            jpeg_dec_close(jpeg_dec_);
            jpeg_dec_ = nullptr;
        }
    }
    StartStream();
    if (!streaming_.load()) jpeg_only_.store(false);
}

// StartCompressedStream 只接管空闲相机；不能把已有RGB预览误报成压缩流。
bool SscmaCamera::StartCompressedStream() {
    if (streaming_.load())
        return false;
    StartJpegStream();
    return streaming_.load() && jpeg_only_.load();
}

// SetStreamFrameInterval 设置通话期间的JPEG采样节拍，停止限速时恢复为实时流。
void SscmaCamera::SetStreamFrameInterval(uint32_t interval_ms) {
    stream_frame_interval_ms_.store(interval_ms, std::memory_order_relaxed);
    if (interval_ms == 0)
        stream_last_published_us_.store(0, std::memory_order_relaxed);
}

// TakeStreamJPEG 与相机回调共享锁，只复制当前已完成JPEG；解码和网络发送均在锁外。
bool SscmaCamera::TakeStreamJPEG(std::vector<uint8_t>& jpeg, uint32_t& sequence) {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    const auto current = stream_seq_.load(std::memory_order_acquire);
    if (!jpeg_only_.load() || current == sequence || !stream_jpeg_size_)
        return false;
    jpeg.assign(jpeg_data_.buf, jpeg_data_.buf + stream_jpeg_size_);
    sequence = current;
    return true;
}

std::expected<std::string, std::string> SscmaCamera::UploadJpegMultipart(
    const std::string& url, const std::string& authorization, const std::string& question) {
    if (url.empty() || !UrPhotoValidateJpeg(jpeg_data_.buf, jpeg_data_.len)) {
        ESP_LOGW(TAG, "photo validation url_present=%d jpeg_valid=%d bytes=%u", !url.empty(),
                 UrPhotoValidateJpeg(jpeg_data_.buf, jpeg_data_.len),
                 static_cast<unsigned>(jpeg_data_.len));
        return std::unexpected("Invalid JPEG or upload URL");
    }
    auto network = Board::GetInstance().GetNetwork();
    ESP_LOGI(TAG, "photo stage=upload_start");
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    std::string question_field;
    if (!question.empty()) {
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
    }

    std::string file_header;
    file_header += "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n";
    file_header += "\r\n";

    std::string multipart_footer;
    multipart_footer += "\r\n--" + boundary + "--\r\n";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!authorization.empty()) {
        http->SetHeader("Authorization", authorization);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "Failed to connect to image upload endpoint: %s",
                 opened.error().ToString().c_str());
        return std::unexpected("Failed to connect to explain URL");
    }

    auto write_or_fail = [&http](const char* data, size_t size) -> bool {
        if (auto written = http->Write(data, size); !written) {
            ESP_LOGE(TAG, "Failed to upload photo: %s", written.error().ToString().c_str());
            return true;
        }
        return false;
    };

    if (!question_field.empty() && write_or_fail(question_field.c_str(), question_field.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    if (write_or_fail(file_header.c_str(), file_header.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    if (write_or_fail((const char*)jpeg_data_.buf, jpeg_data_.len)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    if (write_or_fail(multipart_footer.c_str(), multipart_footer.size())) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    if (write_or_fail("", 0)) {
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Image upload completed size=%u response_size=%u",
             static_cast<unsigned>(jpeg_data_.len), static_cast<unsigned>(result.size()));
    return result;
}

std::expected<std::string, std::string> SscmaCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }
    std::string authorization;
    if (!explain_token_.empty()) {
        authorization = "Bearer " + explain_token_;
    }
    return UploadJpegMultipart(explain_url_, authorization, question);
}

std::expected<std::string, std::string> SscmaCamera::UploadToPlatform(const std::string& url,
                                                                      const char* username,
                                                                      const char* password) {
    if (username == nullptr || password == nullptr || username[0] == '\0' || password[0] == '\0') {
        return std::unexpected("HTTP device authentication is unavailable");
    }
    const size_t plain_size = strlen(username) + strlen(password) + 2;
    auto plain = std::make_unique<unsigned char[]>(plain_size);
    int written =
        snprintf(reinterpret_cast<char*>(plain.get()), plain_size, "%s:%s", username, password);
    if (written <= 0 || static_cast<size_t>(written) >= plain_size) {
        mbedtls_platform_zeroize(plain.get(), plain_size);
        return std::unexpected("Failed to create HTTP device authentication");
    }
    const size_t encoded_capacity = ((static_cast<size_t>(written) + 2) / 3) * 4 + 1;
    auto encoded = std::make_unique<unsigned char[]>(encoded_capacity);
    size_t encoded_size = 0;
    int encode_result = mbedtls_base64_encode(encoded.get(), encoded_capacity, &encoded_size,
                                              plain.get(), static_cast<size_t>(written));
    mbedtls_platform_zeroize(plain.get(), plain_size);
    if (encode_result != 0 || encoded_size == 0 || encoded_size >= encoded_capacity) {
        mbedtls_platform_zeroize(encoded.get(), encoded_capacity);
        return std::unexpected("Failed to encode HTTP device authentication");
    }
    encoded[encoded_size] = '\0';
    std::string authorization = "Basic ";
    authorization.append(reinterpret_cast<char*>(encoded.get()), encoded_size);
    auto result = UploadJpegMultipart(url, authorization);
    mbedtls_platform_zeroize(encoded.get(), encoded_capacity);
    std::fill(authorization.begin(), authorization.end(), '\0');
    authorization.clear();
    if (!result) {
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(result->data(), result->size());
    cJSON* code = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "code") : nullptr;
    cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
    cJSON* file_uri =
        cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "fileUri") : nullptr;
    const int platform_code = cJSON_IsNumber(code) ? code->valueint : 0;
    const char* uri = cJSON_IsString(file_uri) ? file_uri->valuestring : nullptr;
    bool valid = UrPhotoValidateUploadResponse(200, platform_code, uri != nullptr ? uri : "");
    std::string output = valid ? std::string(uri) : std::string();
    cJSON_Delete(root);
    if (!valid) {
        return std::unexpected("Invalid platform upload response");
    }
    return output;
}
