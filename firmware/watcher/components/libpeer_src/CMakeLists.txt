# libpeer 上游核心源码化组件（MIT）：替换 esp_peer 预编译库中存在
# nominate 校验缺陷的 agent/peer 状态机。DTLS-SRTP 在上游 dtls_srtp.c
# 基础上做 mbedTLS4/IDF6 适配。sctp 为内置软件实现（无 datachannel
# 需求，仅满足编译依赖）；peer_signaling/ssl_transport（websocket/http
# 信令）不用于本协议，未纳入源清单。
idf_component_register(
    SRCS "src/address.c" "src/agent.c" "src/base64.c" "src/dtls_srtp.c"
         "src/ice.c" "src/peer.c" "src/peer_connection.c"
         "src/ports.c" "src/rtcp.c" "src/rtp.c" "src/sctp.c" "src/sdp.c"
         "src/socket.c" "src/stun.c" "src/utils.c"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES mbedtls esp_netif esp_timer espressif__esp_libsrtp
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation -Wno-sign-compare)
# LOG_LEVEL=1（LEVEL_WARN）：置 0 会把 LOGW 一并编掉，srflx 收集失败/保活超时等
# 关键诊断在镜像中完全消失（仅剩 LOGE）；WARN 保留这些证据且不含 INFO/DEBUG 噪声。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_USE_LWIP=1 CONFIG_USE_USRSCTP=0
    CONFIG_AUDIO_BUFFER_SIZE=8096 CONFIG_DATA_BUFFER_SIZE=102400
    __BYTE_ORDER=__LITTLE_ENDIAN LOG_LEVEL=1)
