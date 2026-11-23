#ifndef CONFIG_H_
#define CONFIG_H_

/* DTLS-SRTP 自签证书默认使用 ECDSA P-256（见下方 CONFIG_DTLS_USE_ECDSA）：
 * 配合 sdkconfig 的 CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM=y，P-256 签名
 * 的定点乘法使用预计算表，签名成本低于 RSA-1024；验签是任意点双乘，
 * 不受该优化影响。证书生成与复用以 dtls_srtp 的全局缓存 + 开机预热承担。 */

#define SCTP_MTU (1200)
/* CONFIG_MTU 兼作收包与 SDP 缓冲（agent_buf/temp_buf/sctp buf）：必须不小于
 * 对端实际网络 MTU（浏览器 ~1200B 视频包与 ~2KB SDP），缩到 700 会缓冲溢出
 * 导致信令任务冻结（真机 .34 实证）。保持 1300。
 * 发送侧视频分片单独用 CONFIG_SEND_MTU 缩小以提高 WiFi 突发接受率。 */
#define CONFIG_MTU (1300)
/* 真机实测 ~1.2KB 发送分片在设备 WiFi/NAT 链路 40-70% 丢失而 50pps 小包音频
 * 零丢失；发送分片缩小到 700。 */
#define CONFIG_SEND_MTU (700)

#ifndef CONFIG_USE_ZEPHYR
#ifdef __ZEPHYR__
#define CONFIG_USE_ZEPHYR 1
#else
#define CONFIG_USE_ZEPHYR 0
#endif
#endif

#ifndef CONFIG_USE_LWIP
#define CONFIG_USE_LWIP 0
#endif

#ifndef CONFIG_MBEDTLS_DEBUG
#define CONFIG_MBEDTLS_DEBUG 0
#endif

#ifndef CONFIG_MBEDTLS_2_X
#define CONFIG_MBEDTLS_2_X 0
#endif

#if CONFIG_MBEDTLS_2_X
#define RSA_KEY_LENGTH 512
#else
#define RSA_KEY_LENGTH 1024
#endif

/* 使用 ECDSA P-256 自签证书：开启 CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM 后
 * 签名（定点乘）显著快于 RSA-1024；验签（任意点双乘）则明显更慢。设备作为
 * DTLS 服务端时，验签成本发生在校验对端证书，与本地证书算法无关。
 * 密钥/证书生成由全局缓存 + 开机预热消除（见 dtls_srtp.c）。
 * 指纹校验仍走 SDP 中的 sha-256；对端不支持 ECDSA 时改回 0 使用 RSA-1024。 */
#ifndef CONFIG_DTLS_USE_ECDSA
#define CONFIG_DTLS_USE_ECDSA 1
#endif

#ifndef CONFIG_USE_USRSCTP
#define CONFIG_USE_USRSCTP 1
#endif

#ifndef CONFIG_SDP_BUFFER_SIZE
#define CONFIG_SDP_BUFFER_SIZE 8096
#endif

#ifndef CONFIG_MQTT_BUFFER_SIZE
#define CONFIG_MQTT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_HTTP_BUFFER_SIZE
#define CONFIG_HTTP_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_TLS_READ_TIMEOUT
#define CONFIG_TLS_READ_TIMEOUT 3000
#endif

#ifndef CONFIG_STUN_KEEPALIVE_INTERVAL
/* ICE consent 保活（RFC 7675）：connected 后周期 binding request，防止
 * 平台 Pion 因无入向事务判 disconnected 回收会话（真机 vision 会话
 * 23~107 秒随机断开的根因）。 */
#define CONFIG_STUN_KEEPALIVE_INTERVAL 3000
#endif

#ifndef CONFIG_STUN_KEEPALIVE_TIMEOUT
#define CONFIG_STUN_KEEPALIVE_TIMEOUT 15000
#endif

#if CONFIG_STUN_KEEPALIVE_INTERVAL > 0 && \
    CONFIG_STUN_KEEPALIVE_TIMEOUT <= CONFIG_STUN_KEEPALIVE_INTERVAL
#error "CONFIG_STUN_KEEPALIVE_TIMEOUT must be greater than CONFIG_STUN_KEEPALIVE_INTERVAL"
#endif

#ifndef CONFIG_AUDIO_DURATION
#define CONFIG_AUDIO_DURATION 20
#endif

#ifndef CONFIG_MAX_NALU_SIZE
#define CONFIG_MAX_NALU_SIZE (100 * 1024)  // 100KB
#endif

#define CONFIG_IPV6 0
// empty will use first active interface
#define CONFIG_IFACE_PREFIX ""

// #define LOG_LEVEL LEVEL_DEBUG
#ifndef LOG_REDIRECT
#define LOG_REDIRECT 0
#endif

// Disable MQTT and HTTP signaling
// #define DISABLE_PEER_SIGNALING 1

#endif  // CONFIG_H_
