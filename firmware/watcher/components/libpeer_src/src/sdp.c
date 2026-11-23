#include <stdarg.h>
#include <stdio.h>

#include "sdp.h"

int sdp_append(char* sdp, const char* format, ...) {
  va_list argptr;

  va_start(argptr, format);

  if (sdp[0] == '\0') {
    vsnprintf(sdp, CONFIG_SDP_BUFFER_SIZE, format, argptr);
  } else {
    vsnprintf(sdp + strlen(sdp), CONFIG_SDP_BUFFER_SIZE - strlen(sdp), format, argptr);
  }

  if (sdp[strlen(sdp) - 1] != '\n') {
    strcat(sdp, "\r\n");
  }

  va_end(argptr);
  return 0;
}

void sdp_reset(char* sdp) {
  memset(sdp, 0, CONFIG_SDP_BUFFER_SIZE);
}

void sdp_append_h264(char* sdp) {
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 96");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtcp-fb:96 nack");
  sdp_append(sdp, "a=rtcp-fb:96 nack pli");
  sdp_append(sdp, "a=fmtp:96 profile-level-id=42e01f;level-asymmetry-allowed=1");
  sdp_append(sdp, "a=rtpmap:96 H264/90000");
  sdp_append(sdp, "a=ssrc:1 cname:webrtc-h264");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:1");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_vp8(char* sdp) {
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 97");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtcp-fb:97 nack");
  sdp_append(sdp, "a=rtcp-fb:97 nack pli");
  sdp_append(sdp, "a=rtpmap:97 VP8/90000");
  sdp_append(sdp, "a=ssrc:1 cname:webrtc-vp8");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:1");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_pcma(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 8");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:8 PCMA/8000");
  sdp_append(sdp, "a=ssrc:4 cname:webrtc-pcma");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:2");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_pcmu(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 0");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:0 PCMU/8000");
  sdp_append(sdp, "a=ssrc:5 cname:webrtc-pcmu");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:2");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_opus(char* sdp) {
  /* SAVPF（含反馈）：Pion 按 MediaEngine 匹配 m-line 时 SAVP（无F）不匹配
   * opus 编解码，音频轨被挪到附加 m-line（真机 answer 出现双 m=audio：
   * mid:2 recvonly + mid:3 sendonly），桥接音频方向全部错位。 */
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 111");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:111 opus/48000/2");
  sdp_append(sdp, "a=ssrc:6 cname:webrtc-opus");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:2");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_datachannel(char* sdp) {
  sdp_append(sdp, "m=application 50712 UDP/DTLS/SCTP webrtc-datachannel");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=mid:0");
  sdp_append(sdp, "a=sctp-port:5000");
  sdp_append(sdp, "a=max-message-size:262144");
}

/* vision 三轨：平台只收设备上行 H264，fmtp 显式 mode1（FU-A 非交错）匹配平台校验。 */
void sdp_append_h264_sendonly(char* sdp) {
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 96");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtcp-fb:96 nack");
  sdp_append(sdp, "a=rtcp-fb:96 nack pli");
  sdp_append(sdp, "a=fmtp:96 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1");
  sdp_append(sdp, "a=rtpmap:96 H264/90000");
  sdp_append(sdp, "a=ssrc:1 cname:webrtc-h264");
  sdp_append(sdp, "a=sendonly");
  sdp_append(sdp, "a=mid:1");
  sdp_append(sdp, "a=rtcp-mux");
}

/* 麦克风上行段：单一 ssrc 主流，方向恰一。 */
void sdp_append_opus_sendonly(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 111");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:111 opus/48000/2");
  sdp_append(sdp, "a=ssrc:6 cname:webrtc-opus");
  sdp_append(sdp, "a=sendonly");
  sdp_append(sdp, "a=mid:2");
  sdp_append(sdp, "a=rtcp-mux");
}

/* 扬声器下行段：recvonly 不声明本地主流，远端 ssrc 由 answer 回填。 */
void sdp_append_opus_recvonly(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 111");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:111 opus/48000/2");
  sdp_append(sdp, "a=recvonly");
  sdp_append(sdp, "a=mid:3");
  sdp_append(sdp, "a=rtcp-mux");
}

/* vision 三轨会话头：BUNDLE 1 2 3，无 datachannel。 */
void sdp_create_vision(char* sdp) {
  sdp_append(sdp, "v=0");
  sdp_append(sdp, "o=- 1495799811084970 1495799811084970 IN IP4 0.0.0.0");
  sdp_append(sdp, "s=-");
  sdp_append(sdp, "t=0 0");
  sdp_append(sdp, "a=msid-semantic: iot");
#if ICE_LITE
  sdp_append(sdp, "a=ice-lite");
#endif
  sdp_append(sdp, "a=group:BUNDLE 1 2 3");
}

void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel) {
  char bundle[64];
  sdp_append(sdp, "v=0");
  sdp_append(sdp, "o=- 1495799811084970 1495799811084970 IN IP4 0.0.0.0");
  sdp_append(sdp, "s=-");
  sdp_append(sdp, "t=0 0");
  sdp_append(sdp, "a=msid-semantic: iot");
#if ICE_LITE
  sdp_append(sdp, "a=ice-lite");
#endif
  memset(bundle, 0, sizeof(bundle));

  strcat(bundle, "a=group:BUNDLE");

  if (b_datachannel) {
    strcat(bundle, " 0");
  }

  if (b_video) {
    strcat(bundle, " 1");
  }

  if (b_audio) {
    strcat(bundle, " 2");
  }

  sdp_append(sdp, bundle);
}
