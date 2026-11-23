#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include "udp_audio_channel.h"
#include <mqtt.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <memory>
#include <atomic>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // Alive flag for safe scheduled callbacks - set to false in destructor
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<UdpAudioChannel> audio_channel_;
    UdpAudioChannelConfig audio_channel_config_;
    esp_timer_handle_t reconnect_timer_;

    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
