# Watcher 固件形态矩阵构建入口
#
# 维度：FLAVOR（协议×双工×媒体）× BOARD（sensecap-watcher 32MB / esp-sparkbot 16MB+坦克底盘）。
# 新板接入需先完成板级适配（docs/custom-board.md）并扩展 build-watcher.sh 的板级分支。
#
# 形态（FLAVOR）：
#   legacy          联犀旧版协议 $thing/up|down/ai，半双工语音（2.8.x 正式行）
#   signaling       联犀新版统一信令 $thing/up|down/signaling，仅声明半双工（aecMode=none）
#   signaling-aec   新版统一信令 + 设备软件回采 AEC 声明（可协商 fullDuplex）
#   webrtc-audio    新版统一信令 + WebRTC 纯音频（esp_peer，48k/20ms Opus）
#   webrtc-audio-video 新版统一信令 + WebRTC 音视频（H264 上行，专用 32m-video 分区表）
#
# 用法：
#   make list                                              查看形态矩阵
#   make build FLAVOR=signaling-aec VERSION=2.9.18-signaling-aec
#   make signaling-aec VERSION=...                         同上快捷目标
#   make ota FLAVOR=signaling-aec VERSION=... SRC=2.9.17-signaling-aec
#
# 敏感三元组不写入本文件：从环境变量或 UR_ENV_FILE 指向的脚本导出
# UR_PRODUCT_ID / UR_PRODUCT_SECRET / UR_PLATFORM_BASE_URL。
# 例：UR_ENV_FILE=../../../.temp/firmware-env-120.sh make signaling-aec VERSION=...

FLAVOR ?= legacy
VERSION ?=
BOARD ?= sensecap-watcher
SRC ?=
UR_ENV_FILE ?=
OTA_SCRIPT ?= ../../../.temp/push-ota.sh

SUPPORTED_FLAVORS := legacy signaling signaling-aec webrtc-audio webrtc-audio-video
ifeq ($(filter $(FLAVOR),$(SUPPORTED_FLAVORS)),)
$(error unsupported FLAVOR: "$(FLAVOR)" - 可选 legacy / signaling / signaling-aec / webrtc-audio / webrtc-audio-video)
endif
SUPPORTED_BOARDS := sensecap-watcher espressif/esp-sparkbot
ifeq ($(filter $(BOARD),$(SUPPORTED_BOARDS)),)
$(error unsupported BOARD: "$(BOARD)" - 可选 sensecap-watcher（32MB） / espressif/esp-sparkbot（16MB，含坦克底盘）)
endif

# FLAVOR -> 协议开关映射（ENABLE_UR_AI_SIGNALING / ENABLE_UR_AI_SIGNALING_DEVICE_AEC）
ifeq ($(FLAVOR),legacy)
SIGNALING_FLAG := 0
AEC_FLAG := 0
WEBRTC_FLAG := 0
VIDEO_FLAG := 0
SUFFIX :=
else ifeq ($(FLAVOR),signaling)
SIGNALING_FLAG := 1
AEC_FLAG := 0
WEBRTC_FLAG := 0
VIDEO_FLAG := 0
SUFFIX := -signaling
else ifeq ($(FLAVOR),webrtc-audio)
SIGNALING_FLAG := 1
AEC_FLAG := 0
WEBRTC_FLAG := 1
VIDEO_FLAG := 0
SUFFIX := -webrtc-audio
else ifeq ($(FLAVOR),webrtc-audio-video)
SIGNALING_FLAG := 1
# 坦克的人际 UDP 音视频依赖已核验的设备 AEC；Watcher 仍沿用独立的视频调度配置。
ifeq ($(BOARD),espressif/esp-sparkbot)
AEC_FLAG := 1
else
AEC_FLAG := 0
endif
WEBRTC_FLAG := 1
VIDEO_FLAG := 1
SUFFIX := -webrtc-audio-video
else
SIGNALING_FLAG := 1
AEC_FLAG := 1
WEBRTC_FLAG := 0
VIDEO_FLAG := 0
SUFFIX := -signaling-aec
endif

EFFECTIVE_VERSION := $(if $(VERSION),$(VERSION),2.8.3$(SUFFIX))

.PHONY: list build ota legacy signaling signaling-aec webrtc-audio webrtc-audio-video

list:
	@echo "形态矩阵（BOARD=$(BOARD)，可选 sensecap-watcher / espressif/esp-sparkbot）："
	@echo '  legacy          旧版协议 $$thing/up|down/ai，半双工       版本示例 2.8.3'
	@echo "  signaling       新版统一信令，仅声明半双工(aecMode=none)   版本示例 2.9.18-signaling"
	@echo "  signaling-aec   新版统一信令+设备AEC声明(可fullDuplex)    版本示例 2.9.18-signaling-aec"
	@echo '  webrtc-audio    新版信令+WebRTC纯音频(48k/20ms)         版本示例 2.10.0-webrtc-audio'
	@echo '  webrtc-audio-video 新版信令+WebRTC音视频(H264上行)      版本示例 2.11.0-webrtc-audio-video'
	@echo ""
	@echo "三元组来源：环境变量 或 UR_ENV_FILE=<脚本>（导出 UR_PRODUCT_ID/UR_PRODUCT_SECRET/UR_PLATFORM_BASE_URL）"

build:
	@if [ -n "$(UR_ENV_FILE)" ]; then set -a; . "$(UR_ENV_FILE)"; set +a; fi; \
	UR_BOARD=$(BOARD) \
	ENABLE_UR_AI_SIGNALING=$(SIGNALING_FLAG) \
	ENABLE_UR_AI_SIGNALING_DEVICE_AEC=$(AEC_FLAG) \
	ENABLE_UR_AI_SIGNALING_WEBRTC=$(WEBRTC_FLAG) \
	ENABLE_UR_AI_SIGNALING_WEBRTC_VIDEO=$(VIDEO_FLAG) \
	UR_FIRMWARE_VERSION=$(EFFECTIVE_VERSION) \
	bash build-watcher.sh

# 快捷目标名即 FLAVOR：经子 make 显式传递 FLAVOR（target-specific 变量无法影响
# 解析期已算出的形态开关，直接依赖 build 会落入默认 legacy 形态——构建产物无
# 统一信令，真机验证被阻断）。@ 隐藏回显，$(MAKE) 保留 make 的信号/变量语义。
legacy signaling signaling-aec webrtc-audio webrtc-audio-video:
	@$(MAKE) build FLAVOR=$@

ota:
	@if [ "$(BOARD)" != "sensecap-watcher" ]; then echo "坦克 OTA 尚未接入该 Watcher 发布入口"; exit 1; fi; \
	grep -Fxq 'CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER=y' sdkconfig || { echo "构建板型与 Watcher OTA 目标不一致"; exit 1; }; \
	if [ -n "$(UR_ENV_FILE)" ]; then set -a; . "$(UR_ENV_FILE)"; set +a; fi; \
	[ -n "$(SRC)" ] || { echo "ota 需要 SRC=<设备当前版本>"; exit 1; }; \
	OTASRCVERSION=$(SRC) bash $(OTA_SCRIPT) build/xiaozhi.bin $(EFFECTIVE_VERSION)
