#!/bin/bash
# Watcher 固件构建脚本：固定 ESP-IDF 6 镜像摘要并校验板型、分区与安全配置
#
# 用法：在固件工程目录执行，或被 resource-guard 包裹调用。
# 联犀三元组通过环境变量注入（不写死在源码/脚本中）：
#   UR_PRODUCT_ID         联犀产品 ID（必填，测试环境取值见 ur-api skill 的 device-firmware/references/test-envs.md）
#   UR_PRODUCT_SECRET     联犀产品密钥（必填，动态注册签名用，来源同上或平台产品详情）
#   UR_PLATFORM_BASE_URL  联犀平台 HTTP 基址（必填，用于解析 OTA 相对 OSS 路径）
#   ENABLE_BT             是否启用蓝牙协议栈（默认 1；正式 Watcher 固件必须保持开启）
#   ENABLE_UR_AI_E2E_TEST 是否嵌入语音测试音频并注册 ur_ai_e2e（默认 0）
#   ENABLE_UR_AI_SIGNALING 语音会话是否走统一信令新版协议（默认 0）
#   ENABLE_UR_AI_SIGNALING_DEVICE_AEC 是否声明设备侧软件回采 AEC（默认 0；需回灌实测后开启）
#   ENABLE_UR_AI_SIGNALING_WEBRTC 信令媒体走 WebRTC esp_peer（默认 0；48k/20ms）
#   Watcher 固定采用 partitions/v2/32m-video.csv：OTA 6M、assets@0xE00000。
#   旧 4M 分区设备不能通过普通 OTA 切换分区表，须先核实设备布局并使用受控迁移流程。
#   坦克统一信令正式形态固定为信令 + 设备 AEC + WebRTC + UDP 音视频；E2E 仅在测试包显式开启。
#   ENABLE_UR_UDP_TX_TRACE 是否观察固定SDK的netif交接与Wi-Fi回调（默认0，仅诊断包）
#   UR_FIRMWARE_VERSION   固件版本（默认 2.8.3；保留平台旧版记录，不覆盖同版本镜像）
#   SDKCONFIG_EXTRA       额外 sdkconfig 追加项（可选，调试用）
set -e
set -o pipefail
# 构建配置及产物包含设备接入凭据，限制新建文件的默认访问权限。
umask 077

FW_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$FW_DIR"

: "${UR_PRODUCT_ID:?需要设置 UR_PRODUCT_ID 环境变量}"
: "${UR_PRODUCT_SECRET:?需要设置 UR_PRODUCT_SECRET 环境变量}"
: "${UR_PLATFORM_BASE_URL:?需要设置 UR_PLATFORM_BASE_URL 环境变量}"
case "$UR_PLATFORM_BASE_URL" in
  http://*|https://*) ;;
  *) echo "UR_PLATFORM_BASE_URL 必须以 http:// 或 https:// 开头" >&2; exit 1 ;;
esac
ENABLE_BT="${ENABLE_BT:-1}"
ENABLE_UR_AI_E2E_TEST="${ENABLE_UR_AI_E2E_TEST:-0}"
ENABLE_UR_AI_SIGNALING="${ENABLE_UR_AI_SIGNALING:-0}"
ENABLE_UR_AI_SIGNALING_DEVICE_AEC="${ENABLE_UR_AI_SIGNALING_DEVICE_AEC:-0}"
ENABLE_UR_AI_SIGNALING_WEBRTC="${ENABLE_UR_AI_SIGNALING_WEBRTC:-0}"
ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO="${ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO:-0}"
ENABLE_UR_UDP_TX_TRACE="${ENABLE_UR_UDP_TX_TRACE:-0}"
UR_FIRMWARE_VERSION="${UR_FIRMWARE_VERSION:-2.8.3}"
SDKCONFIG_EXTRA="${SDKCONFIG_EXTRA:-}"
# 坦克与 Watcher 共用工程，但板型、Flash 分区和构建形态必须分别校验。
UR_BOARD="${UR_BOARD:-sensecap-watcher}"
case "$UR_BOARD" in
  sensecap-watcher)
    UR_BOARD_CONFIG="CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=y"
    UR_BOARD_NAME="sensecap-watcher"
    UR_FLASHSIZE_CONFIG="CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y"
    UR_PARTITION_CSV="partitions/v2/32m-video.csv"
    UR_APP_PART_BYTES=$((6 * 1024 * 1024))
    UR_APP_OFFSET="0x200000"
    UR_TICK_CONFIG="CONFIG_FREERTOS_HZ=1000"
    # Watcher 表盘使用 48 号字体，构建入口必须与板型配置保持一致。
    UR_BOARD_SDKCONFIG="CONFIG_LV_FONT_MONTSERRAT_48=y\n"
    IDF_IMAGE="${IDF_IMAGE:-espressif/idf:release-v6.0@sha256:4cafa2bec24892983625ba6252c27f0dd7184f60de3c5e959380abec53c3951f}"
    ;;
  espressif/esp-sparkbot)
    if [ "$ENABLE_UR_UDP_TX_TRACE" != "0" ]; then
      echo "坦克统一信令不允许启用 Wi-Fi 发送诊断" >&2; exit 1
    fi
    # 坦克只有一套已核验的统一信令媒体组合；E2E 是可选测试入口，不再决定是否能生成正式固件。
    if [ "$ENABLE_UR_AI_SIGNALING" != "0" ] || [ "$ENABLE_UR_AI_SIGNALING_DEVICE_AEC" != "0" ] ||
       [ "$ENABLE_UR_AI_SIGNALING_WEBRTC" != "0" ] || [ "$ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO" != "0" ]; then
      if [ "$ENABLE_UR_AI_SIGNALING" != "1" ] || [ "$ENABLE_UR_AI_SIGNALING_DEVICE_AEC" != "1" ] ||
         [ "$ENABLE_UR_AI_SIGNALING_WEBRTC" != "1" ] || [ "$ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO" != "1" ]; then
        echo "坦克统一信令必须同时启用信令、设备 AEC、WebRTC 和视频" >&2; exit 1
      fi
    fi
    UR_BOARD_CONFIG="CONFIG_BOARD_TYPE_ESP_SPARKBOT=y"
    UR_BOARD_NAME="esp-sparkbot"
    UR_FLASHSIZE_CONFIG="CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"
    UR_PARTITION_CSV="partitions/v2/16m.csv"
    UR_APP_PART_BYTES=$((0x3f0000))
    UR_APP_OFFSET="0x20000"
    UR_TICK_CONFIG="CONFIG_FREERTOS_HZ=100"
    UR_BOARD_SDKCONFIG="CONFIG_CAMERA_OV2640=y\nCONFIG_CAMERA_OV2640_AUTO_DETECT_DVP_INTERFACE_SENSOR=y\nCONFIG_CAMERA_OV2640_DVP_YUV422_240X240_25FPS=y\nCONFIG_CAMERA_GC2145=y\nCONFIG_CAMERA_GC2145_AUTO_DETECT_DVP_INTERFACE_SENSOR=y\nCONFIG_CAMERA_GC2145_DVP_RGB565_BE_640X480_15FPS=y\nCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y\n# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set\n"
    IDF_IMAGE="${IDF_IMAGE:-espressif/idf@sha256:0451470225f5f44bd0dbdb40861050b517f7473a1b2acf031fad848c853322ab}"
    ;;
  *) echo "不支持的 UR_BOARD: $UR_BOARD" >&2; exit 1 ;;
esac
# 私有回调仅允许显式诊断包及已审查SDK；不在普通验收或其他镜像上自动启用。
case "$ENABLE_UR_UDP_TX_TRACE" in
  0) ;;
  1)
    if [ "$ENABLE_UR_AI_E2E_TEST" != "1" ] || [ "$IDF_IMAGE" != "espressif/idf:release-v6.0@sha256:4cafa2bec24892983625ba6252c27f0dd7184f60de3c5e959380abec53c3951f" ]; then
      echo "Wi-Fi发送诊断仅支持固定IDF6镜像且必须开启语音测试入口" >&2; exit 1
    fi ;;
  *) echo "ENABLE_UR_UDP_TX_TRACE 仅支持0或1" >&2; exit 1 ;;
esac

docker run --rm \
  -v "$FW_DIR":/project -w /project \
  -v "$FW_DIR/../device-runtime":/device-runtime:ro \
  -e UR_PRODUCT_ID -e UR_PRODUCT_SECRET -e UR_PLATFORM_BASE_URL \
  -e UR_BOARD_CONFIG="$UR_BOARD_CONFIG" -e UR_BOARD_NAME="$UR_BOARD_NAME" \
  -e UR_FLASHSIZE_CONFIG="$UR_FLASHSIZE_CONFIG" -e UR_PARTITION_CSV="$UR_PARTITION_CSV" \
  -e UR_APP_PART_BYTES="$UR_APP_PART_BYTES" -e UR_APP_OFFSET="$UR_APP_OFFSET" \
  -e UR_TICK_CONFIG="$UR_TICK_CONFIG" -e UR_BOARD_SDKCONFIG="$UR_BOARD_SDKCONFIG" \
  -e UR_FIRMWARE_VERSION="$UR_FIRMWARE_VERSION" \
  -e ENABLE_BT="$ENABLE_BT" -e ENABLE_UR_AI_E2E_TEST="$ENABLE_UR_AI_E2E_TEST" \
  -e ENABLE_UR_AI_SIGNALING="$ENABLE_UR_AI_SIGNALING" \
  -e ENABLE_UR_AI_SIGNALING_DEVICE_AEC="$ENABLE_UR_AI_SIGNALING_DEVICE_AEC" \
  -e ENABLE_UR_AI_SIGNALING_WEBRTC="$ENABLE_UR_AI_SIGNALING_WEBRTC" \
  -e ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO="$ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO" \
  -e ENABLE_UR_UDP_TX_TRACE="$ENABLE_UR_UDP_TX_TRACE" \
  -e SDKCONFIG_EXTRA="$SDKCONFIG_EXTRA" \
  -e WATCHER_BUILD_UID="$(id -u)" -e WATCHER_BUILD_GID="$(id -g)" \
  "$IDF_IMAGE" bash -c '
# 容器不会继承宿主 shell 的 umask，必须再次限制生成配置与产物权限。
umask 077
# 无论构建是否成功，仅将本次生成目录和配置交还宿主调用者，保留受限权限。
restore_output_owner() {
  watcher_build_status=$?
  for watcher_output in build sdkconfig sdkconfig.old; do
    if [ -e "$watcher_output" ]; then
      chown -R "$WATCHER_BUILD_UID:$WATCHER_BUILD_GID" "$watcher_output" || watcher_build_status=1
    fi
  done
  exit "$watcher_build_status"
}
trap restore_output_owner EXIT
# 板型、分区和镜像上限在宿主侧确定，避免误将 Watcher 分区用于坦克。
PARTITION_CSV="$UR_PARTITION_CSV"
APP_PART_SIZE_BYTES="$UR_APP_PART_BYTES"
if [ "$ENABLE_UR_UDP_TX_TRACE" = "1" ]; then
  python scripts/check_wifi_tx_trace_owner.py --idf-root "$IDF_PATH" || exit 1
fi
rm -rf build sdkconfig sdkconfig.old
cp sdkconfig.defaults sdkconfig
echo "CONFIG_IDF_TARGET=\"esp32s3\"" >> sdkconfig
# set-target 已写入大量“未启用”项；Kconfig 遇到同名重复项会保留旧值，因此注入前必须同时删除旧的 y/n 形式。
sed -i -E "/^(# )?CONFIG_(FREERTOS_HZ|LV_FONT_MONTSERRAT_48|BOARD_TYPE_.*|ESPTOOLPY_FLASHSIZE.*|ESPTOOLPY_FLASHMODE.*|PARTITION_TABLE_CUSTOM|PARTITION_TABLE_CUSTOM_FILENAME|MBEDTLS_SSL_DTLS_SRTP|MBEDTLS_SSL_PROTO_DTLS|MBEDTLS_X509_CRT_WRITE_C|MBEDTLS_X509_CREATE_C|MBEDTLS_SSL_KEEP_PEER_CERTIFICATE|MBEDTLS_PLATFORM_MEMORY|MBEDTLS_PLATFORM_C|BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH|ESPTOOLPY_FLASH_MODE_AUTO_DETECT|IDF_EXPERIMENTAL_FEATURES|BOOTLOADER_APP_ROLLBACK_ENABLE|UR_PLATFORM_OTA|UR_AI|UR_AI_E2E_TEST|UR_AI_SIGNALING|UR_AI_SIGNALING_DEVICE_AEC|UR_AI_SIGNALING_WEBRTC|UR_AI_SIGNALING_WEBRTC_VIDEO|UR_SOFTWARE_AEC|UR_UDP_TX_TRACE|UR_PLATFORM_BASE_URL|UR_PRODUCT_ID|UR_PRODUCT_SECRET|UR_BLE_PROVISION|OTA_URL|WAKE_WORD_DISABLED|USE_ESP_WAKE_WORD|USE_AFE_WAKE_WORD|USE_CUSTOM_WAKE_WORD|USE_MULTILINE_CHAT_MESSAGE|SEND_WAKE_WORD_DATA|BT_ENABLED|BT_BLUEDROID_ENABLED|BT_CONTROLLER_ENABLED|BT_BLE_ENABLED|BT_GATTS_ENABLE|BT_BLE_42_FEATURES_SUPPORTED|BT_BLE_50_FEATURES_SUPPORTED)(=.*| is not set)$/d" sdkconfig &&
# 坦克 USB 控制台选择与默认 UART 配置互斥。
if [ "$UR_BOARD_NAME" = "esp-sparkbot" ]; then
  sed -i -E "/^(# )?CONFIG_ESP_CONSOLE_(UART_DEFAULT|UART_CUSTOM|USB_SERIAL_JTAG|USB_CDC|NONE)(=.*| is not set)$/d" sdkconfig
  # 防止复用其他板型 sdkconfig 时继承不适用于坦克的通话传输选择。
  sed -i -E "/^(# )?CONFIG_UR_HUMAN_CALL_(WEBRTC|UDP_AV)(=.*| is not set)$/d" sdkconfig
fi &&
printf "\n$UR_TICK_CONFIG\n$UR_BOARD_CONFIG\n$UR_FLASHSIZE_CONFIG\nCONFIG_ESPTOOLPY_FLASHMODE_DIO=y\nCONFIG_PARTITION_TABLE_CUSTOM=y\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"$PARTITION_CSV\"\nCONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y\nCONFIG_ESPTOOLPY_FLASH_MODE_AUTO_DETECT=n\nCONFIG_IDF_EXPERIMENTAL_FEATURES=y\nCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\nCONFIG_UR_PLATFORM_OTA=y\nCONFIG_UR_AI=y\nCONFIG_OTA_URL=\"\"\nCONFIG_USE_AFE_WAKE_WORD=y\nCONFIG_USE_MULTILINE_CHAT_MESSAGE=y\n# CONFIG_SEND_WAKE_WORD_DATA is not set\n" >> sdkconfig &&
printf "%b" "$UR_BOARD_SDKCONFIG" >> sdkconfig &&
if [ "$ENABLE_BT" = "1" ]; then
  # LLSync 使用 Bluedroid 4.2 的传统广播接口；IDF 6 默认只启用 5.0，必须显式保留 4.2 能力。
  printf "CONFIG_BT_ENABLED=y\nCONFIG_BT_BLUEDROID_ENABLED=y\nCONFIG_BT_CONTROLLER_ENABLED=y\nCONFIG_BT_BLE_ENABLED=y\nCONFIG_BT_GATTS_ENABLE=y\nCONFIG_BT_BLE_42_FEATURES_SUPPORTED=y\n# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set\nCONFIG_UR_BLE_PROVISION=y\n" >> sdkconfig
fi &&
if [ "$ENABLE_UR_AI_E2E_TEST" = "1" ]; then
  printf "CONFIG_UR_AI_E2E_TEST=y\n" >> sdkconfig
else
  printf "# CONFIG_UR_AI_E2E_TEST is not set\n" >> sdkconfig
fi &&
if [ "$ENABLE_UR_AI_SIGNALING" = "1" ]; then
  printf "CONFIG_UR_AI_SIGNALING=y\n" >> sdkconfig
  if [ "$ENABLE_UR_AI_SIGNALING_DEVICE_AEC" = "1" ]; then
    printf "CONFIG_UR_AI_SIGNALING_DEVICE_AEC=y\nCONFIG_UR_SOFTWARE_AEC=y\n" >> sdkconfig
  else
    printf "# CONFIG_UR_AI_SIGNALING_DEVICE_AEC is not set\n" >> sdkconfig
    printf "# CONFIG_UR_SOFTWARE_AEC is not set\n" >> sdkconfig
  fi
  if [ "$ENABLE_UR_AI_SIGNALING_WEBRTC" = "1" ]; then
    printf "CONFIG_UR_AI_SIGNALING_WEBRTC=y\n" >> sdkconfig
  else
    printf "# CONFIG_UR_AI_SIGNALING_WEBRTC is not set\n" >> sdkconfig
  fi
  if [ "$ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO" = "1" ]; then
    printf "CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO=y\n" >> sdkconfig
    if [ "$UR_BOARD_NAME" = "esp-sparkbot" ]; then
      printf "CONFIG_UR_HUMAN_CALL_UDP_AV=y\n" >> sdkconfig
    fi
    # UDP AV 视频与音频共用信令任务，突发分片需要更大的 lwIP 接收邮箱，避免
    # 任务短时执行编解码时整帧缺片；仅视频形态启用，不改变纯音频默认值。
    printf "CONFIG_LWIP_UDP_RECVMBOX_SIZE=64\n" >> sdkconfig
  else
    printf "# CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO is not set\n" >> sdkconfig
  fi
else
  printf "# CONFIG_UR_AI_SIGNALING is not set\n" >> sdkconfig
fi &&
if [ "$ENABLE_UR_UDP_TX_TRACE" = "1" ]; then
  printf "CONFIG_UR_UDP_TX_TRACE=y\n" >> sdkconfig
else
  printf "# CONFIG_UR_UDP_TX_TRACE is not set\n" >> sdkconfig
fi &&
# esp_peer 为组件级依赖（所有形态都会编译）：mbedTLS DTLS-SRTP 链
# use_srtp ← PROTO_DTLS（IDF6 默认关）与自签证书链 X509 CRT WRITE ←
# X509 CREATE 必须始终开启；非 WebRTC 形态不链接 esp_peer 符号不进镜像。
printf "CONFIG_MBEDTLS_PLATFORM_MEMORY=y
" >> sdkconfig &&
printf "CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y\n" >> sdkconfig &&
printf "CONFIG_MBEDTLS_SSL_PROTO_DTLS=y\nCONFIG_MBEDTLS_SSL_DTLS_SRTP=y\nCONFIG_MBEDTLS_X509_CREATE_C=y\nCONFIG_MBEDTLS_X509_CRT_WRITE_C=y\n" >> sdkconfig &&
printf "CONFIG_UR_PRODUCT_ID=\"%s\"\nCONFIG_UR_PRODUCT_SECRET=\"%s\"\nCONFIG_UR_PLATFORM_BASE_URL=\"%s\"\n%s\n" "$UR_PRODUCT_ID" "$UR_PRODUCT_SECRET" "$UR_PLATFORM_BASE_URL" "$SDKCONFIG_EXTRA" >> sdkconfig &&
IDF_TARGET=esp32s3 idf.py -DBOARD_NAME="$UR_BOARD_NAME" -DPROJECT_VER="$UR_FIRMWARE_VERSION" build &&
# 依赖管理器可能在配置时更新源码；交付前再次检查，不只信任旧缓存。
if [ "$ENABLE_UR_UDP_TX_TRACE" = "1" ]; then
  python scripts/check_wifi_tx_trace_owner.py --idf-root "$IDF_PATH"
fi &&
for required in \
  "$UR_TICK_CONFIG" \
  "$UR_BOARD_CONFIG" \
  "$UR_FLASHSIZE_CONFIG" \
  "CONFIG_ESPTOOLPY_FLASHMODE_DIO=y" \
  "CONFIG_PARTITION_TABLE_CUSTOM=y" \
  "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"$PARTITION_CSV\"" \
  "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" \
  "CONFIG_UR_PLATFORM_OTA=y" \
  "CONFIG_UR_AI=y" \
  "CONFIG_OTA_URL=\"\"" \
  "CONFIG_USE_AFE_WAKE_WORD=y" \
  "CONFIG_USE_MULTILINE_CHAT_MESSAGE=y" \
  "# CONFIG_SEND_WAKE_WORD_DATA is not set" \
  "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y"; do
  grep -Fqx "$required" sdkconfig || { echo "缺少关键配置: $required" >&2; exit 1; }
done &&
if [ "$ENABLE_BT" = "1" ]; then
  for required in \
    "CONFIG_BT_ENABLED=y" \
    "CONFIG_BT_BLUEDROID_ENABLED=y" \
    "CONFIG_BT_CONTROLLER_ENABLED=y" \
    "CONFIG_BT_BLE_ENABLED=y" \
    "CONFIG_BT_GATTS_ENABLE=y" \
    "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y" \
    "# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set" \
    "CONFIG_UR_BLE_PROVISION=y"; do
    grep -Fqx "$required" sdkconfig || { echo "缺少 BLE 配置: $required" >&2; exit 1; }
  done
fi &&
if [ "$ENABLE_UR_AI_E2E_TEST" = "1" ]; then
  grep -Fqx "CONFIG_UR_AI_E2E_TEST=y" sdkconfig || { echo "缺少语音 E2E 配置" >&2; exit 1; }
else
  grep -Fqx "# CONFIG_UR_AI_E2E_TEST is not set" sdkconfig || { echo "正式固件意外启用了语音 E2E" >&2; exit 1; }
fi &&
if [ "$ENABLE_UR_AI_SIGNALING" = "1" ]; then
  grep -Fqx "CONFIG_UR_AI_SIGNALING=y" sdkconfig || { echo "缺少统一信令配置" >&2; exit 1; }
  if [ "$UR_BOARD_NAME" = "esp-sparkbot" ]; then
    grep -Fqx "CONFIG_UR_AI_SIGNALING_WEBRTC_VIDEO=y" sdkconfig &&
    grep -Fqx "CONFIG_UR_HUMAN_CALL_UDP_AV=y" sdkconfig || { echo "坦克测试包必须使用 UDP 音视频" >&2; exit 1; }
  fi
  if [ "$ENABLE_UR_AI_SIGNALING_DEVICE_AEC" = "1" ]; then
    grep -Fqx "CONFIG_UR_AI_SIGNALING_DEVICE_AEC=y" sdkconfig || { echo "缺少信令 AEC 声明配置" >&2; exit 1; }
    grep -Fqx "CONFIG_UR_SOFTWARE_AEC=y" sdkconfig || { echo "缺少软件回采 AEC 配置" >&2; exit 1; }
  fi
else
  # 不满足板型依赖时 Kconfig 会省略此项；只要不存在开启配置即可。
  ! grep -Eq "^CONFIG_UR_AI_SIGNALING=" sdkconfig || { echo "默认固件意外启用了统一信令" >&2; exit 1; }
fi &&
if [ "$ENABLE_UR_UDP_TX_TRACE" = "1" ]; then
  grep -Fqx "CONFIG_UR_UDP_TX_TRACE=y" sdkconfig || { echo "缺少Wi-Fi发送诊断配置" >&2; exit 1; }
else
  # E2E依赖关闭时Kconfig会省略该选项；拒绝显式赋值，实际ELF继续校验诊断符号隔离。
  test -r sdkconfig && ! grep -Eq "^CONFIG_UR_UDP_TX_TRACE=" sdkconfig || { echo "默认固件意外启用了Wi-Fi发送诊断" >&2; exit 1; }
fi &&
grep -Fqx "CONFIG_UR_PRODUCT_ID=\"$UR_PRODUCT_ID\"" sdkconfig &&
grep -Fqx "CONFIG_UR_PRODUCT_SECRET=\"$UR_PRODUCT_SECRET\"" sdkconfig &&
grep -Fqx "CONFIG_UR_PLATFORM_BASE_URL=\"$UR_PLATFORM_BASE_URL\"" sdkconfig &&
python -c "import json; assert json.load(open(\"build/project_description.json\"))[\"project_version\"] == \"$UR_FIRMWARE_VERSION\"" &&
python scripts/check_udp_tx_trace_symbols.py --elf build/xiaozhi.elf --enabled "$ENABLE_UR_UDP_TX_TRACE" &&
# 镜像大小按所选分区表的 app 分区容量校验，
# 并确认 flash_args 仍从 ota_0 固定偏移 0x200000 烧写应用。
test "$(stat -c %s build/xiaozhi.bin)" -le "$APP_PART_SIZE_BYTES" &&
grep -q "$UR_APP_OFFSET xiaozhi.bin" build/flash_args' 2>&1 | perl -pe '
BEGIN { $secret = $ENV{"UR_PRODUCT_SECRET"} // "" }
s/\Q$secret\E/<redacted>/g if length $secret;
'
