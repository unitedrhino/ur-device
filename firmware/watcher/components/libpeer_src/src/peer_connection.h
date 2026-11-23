/**
 * @file peer_connection.h
 * @brief Struct PeerConnection
 */
#ifndef PEER_CONNECTION_H_
#define PEER_CONNECTION_H_

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SdpType {
  SDP_TYPE_OFFER = 0,
  SDP_TYPE_ANSWER,
} SdpType;

typedef enum PeerConnectionState {
  PEER_CONNECTION_NEW,
  PEER_CONNECTION_CHECKING,
  PEER_CONNECTION_CONNECTED,
  PEER_CONNECTION_DISCONNECTED,
  PEER_CONNECTION_FAILED,
  PEER_CONNECTION_CLOSED,
} PeerConnectionState;

typedef enum DataChannelType {

  DATA_CHANNEL_NONE = 0,
  DATA_CHANNEL_STRING,
  DATA_CHANNEL_BINARY,

} DataChannelType;

typedef enum DecpChannelType {
  DATA_CHANNEL_RELIABLE = 0x00,
  DATA_CHANNEL_RELIABLE_UNORDERED = 0x80,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT = 0x01,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED = 0x02,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED = 0x82,
} DecpChannelType;

typedef enum MediaCodec {

  CODEC_NONE = 0,

  /* Video */
  CODEC_H264,
  CODEC_VP8,    // not implemented yet
  CODEC_MJPEG,  // not implemented yet

  /* Audio */
  CODEC_OPUS,  // not implemented yet
  CODEC_PCMA,
  CODEC_PCMU,

} MediaCodec;

typedef struct IceServer {
  const char* urls;
  const char* username;
  const char* credential;

} IceServer;

typedef struct PeerConfiguration {
  IceServer ice_servers[5];

  MediaCodec audio_codec;
  MediaCodec video_codec;
  DataChannelType datachannel;
  /* separate_audio_tracks 生成三轨 offer（video sendonly + audio sendonly +
   * audio recvonly），适配平台 vision 严格校验（各 m-line 恰一方向、全 SAVPF）。 */
  int separate_audio_tracks;

  void (*onaudiotrack)(uint8_t* data, size_t size, void* userdata);
  void (*onvideotrack)(uint8_t* data, size_t size, void* userdata);
  void (*on_request_keyframe)(void* userdata);
  void* user_data;

} PeerConfiguration;

typedef struct PeerConnection PeerConnection;

const char* peer_connection_state_to_string(PeerConnectionState state);

PeerConnectionState peer_connection_get_state(PeerConnection* pc);

void* peer_connection_get_sctp(PeerConnection* pc);

PeerConnection* peer_connection_create(PeerConfiguration* config);

void peer_connection_destroy(PeerConnection* pc);

void peer_connection_close(PeerConnection* pc);

int peer_connection_loop(PeerConnection* pc);

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol);

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid);

/**
 * @brief send message to data channel
 * @param[in] peer connection
 * @param[in] message buffer
 * @param[in] length of message
 */
int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len);

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid);

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* packet, size_t bytes);

int peer_connection_send_video(PeerConnection* pc, const uint8_t* packet, size_t bytes);

/* peer_connection_request_downlink_keyframe 对下行视频流（远端发送方）发送
 * RTCP PLI 请求即时关键帧；未连通或未学习到远端视频 SSRC 时返回 -1。
 * 需在 peer_connection_loop 所属任务上下文调用（agent_send 非线程安全）。 */
int peer_connection_request_downlink_keyframe(PeerConnection* pc);

/* peer_connection_remote_video_ssrc 返回从远端 SDP 学习到的视频 SSRC，供真机诊断
 * PLI 目标是否有效；0 表示尚未解析到远端视频流。 */
uint32_t peer_connection_remote_video_ssrc(PeerConnection* pc);

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType sdp_type);

void peer_connection_set_local_description(PeerConnection* pc, const char* sdp, SdpType sdp_type);

const char* peer_connection_create_offer(PeerConnection* pc);

const char* peer_connection_create_answer(PeerConnection* pc);

/**
 * @brief register callback function to handle packet loss from RTCP receiver report
 * @param[in] peer connection
 * @param[in] callback function void (*cb)(float fraction_loss, uint32_t total_loss, void *userdata)
 * @param[in] userdata for callback function
 */
void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata));

/**
 * @brief Set the callback function to handle onicecandidate event.
 * @param A PeerConnection.
 * @param A callback function to handle onicecandidate event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp_text, void* userdata));

/**
 * @brief Set the callback function to handle oniceconnectionstatechange event.
 * @param A PeerConnection.
 * @param A callback function to handle oniceconnectionstatechange event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata));

/**
 * @brief register callback function to handle event of datachannel
 * @param[in] peer connection
 * @param[in] callback function when message received
 * @param[in] callback function when connection is opened
 * @param[in] callback function when connection is closed
 */
void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata));

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid);

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid);

/**
 * @brief adds a new remote candidate to the peer connection
 * @param[in] peer connection
 * @param[in] ice candidate
 */
int peer_connection_add_ice_candidate(PeerConnection* pc, char* ice_candidate);

#ifdef __cplusplus
}
#endif

#endif  // PEER_CONNECTION_H_
