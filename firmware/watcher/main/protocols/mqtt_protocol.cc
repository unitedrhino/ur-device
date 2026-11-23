#include "mqtt_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <charconv>
#include <cstring>
#include <system_error>
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();
    audio_channel_ = std::make_unique<UdpAudioChannel>();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback =
            [](void* arg) {
                MqttProtocol* protocol = (MqttProtocol*)arg;
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "Reconnecting to MQTT server");
                    auto alive = protocol->alive_;  // Capture alive flag
                    app.Schedule([protocol, alive]() {
                        if (*alive) {
                            protocol->StartMqttClient(false);
                        }
                    });
                }
            },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");

    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;

    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    audio_channel_->Close();
    audio_channel_.reset();
    mqtt_.reset();

    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() { return StartMqttClient(false); }

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds",
                 MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s",
                     cJSON_IsString(session_id) ? session_id->valuestring : "null");
            if (cJSON_IsString(session_id) && session_id_ == session_id->valuestring) {
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        // Server initiated goodbye, don't send goodbye back to avoid ping-pong
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        auto port_str = endpoint.substr(pos + 1);
        int parsed_port = 0;
        auto [ptr, ec] =
            std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed_port);
        if (ec == std::errc() && ptr == port_str.data() + port_str.size() && parsed_port > 0 &&
            parsed_port <= UINT16_MAX) {
            broker_port = parsed_port;
        } else {
            ESP_LOGW(TAG, "Invalid port in MQTT endpoint \"%s\", using %d", endpoint.c_str(),
                     broker_port);
        }
    } else {
        broker_address = endpoint;
    }
    ESP_LOGI(TAG, "Connecting to endpoint %s:%d", broker_address.c_str(), broker_port);
    if (Board::GetInstance().GetBoardType() == "wifi") {
        struct hostent* server = gethostbyname(broker_address.c_str());
        if (server != nullptr && server->h_addr != nullptr) {
            ESP_LOGI(TAG, "Resolved MQTT host %s -> %s", broker_address.c_str(),
                     inet_ntoa(*reinterpret_cast<struct in_addr*>(server->h_addr)));
        } else {
            ESP_LOGW(TAG, "Failed to resolve MQTT host %s", broker_address.c_str());
        }
    }
    if (auto connected = mqtt_->Connect(broker_address, broker_port, client_id, username, password);
        !connected) {
        ESP_LOGE(TAG, "Failed to connect to endpoint: %s", connected.error().ToString().c_str());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED,
                 broker_address + ":" + std::to_string(broker_port));
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    return audio_channel_ != nullptr && packet != nullptr && audio_channel_->Send(*packet);
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    audio_channel_->Close();

    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    // Only send goodbye when client initiates the close
    // Don't send if server already sent goodbye (to avoid ping-pong)
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (!audio_channel_->Open(
            Board::GetInstance().GetNetwork(), audio_channel_config_,
            [this](std::unique_ptr<AudioStreamPacket> packet) {
                last_incoming_time_ = std::chrono::steady_clock::now();
                if (on_incoming_audio_ != nullptr) {
                    on_incoming_audio_(std::move(packet));
                }
            })) {
        ESP_LOGE(TAG, "Failed to open UDP audio channel");
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported or missing transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key_item = cJSON_GetObjectItem(udp, "key");
    auto nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || port->valueint <= 0 ||
        port->valueint > UINT16_MAX || !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        ESP_LOGE(TAG, "Invalid UDP server, port, key, or nonce");
        return;
    }
    if (strlen(nonce_item->valuestring) != 32 || strlen(key_item->valuestring) != 32) {
        ESP_LOGE(TAG, "Invalid AES key or nonce length");
        return;
    }
    audio_channel_config_.server = server->valuestring;
    audio_channel_config_.port = port->valueint;
    audio_channel_config_.key_hex = key_item->valuestring;
    audio_channel_config_.nonce_hex = nonce_item->valuestring;
    audio_channel_config_.sample_rate = server_sample_rate_;
    audio_channel_config_.frame_duration = server_frame_duration_;
    audio_channel_config_.preserve_nonce_timestamp = false;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return audio_channel_ != nullptr && audio_channel_->IsOpen() && !error_occurred_ &&
           !IsTimeout();
}
