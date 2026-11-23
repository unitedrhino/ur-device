# 可移植设备核心；平台驱动单独链接，测试依赖固定版本。
cmake_minimum_required(VERSION 3.22)
project(ur_device_runtime VERSION 0.1.0 LANGUAGES C CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(UR_SANITIZER "" CACHE STRING "检查器：address 或 thread，禁止同时使用")
option(UR_COVERAGE "生成 GCC 核心覆盖率" OFF)
include(FetchContent)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cjson GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3)
FetchContent_MakeAvailable(cjson)
target_compile_definitions(cjson PRIVATE CJSON_NESTING_LIMIT=32)
add_library(ur_device_core core/session.cpp core/udp_channel.cpp core/requests.cpp core/envelope.cpp core/audio_codec.cpp core/udp_audio.cpp core/voice_wire.cpp core/voice_client.cpp core/reply_datagram.cpp core/udp_voice_media.cpp core/buffered_pcm.cpp)
target_sources(ur_device_core PRIVATE core/udp_reply_receipt_state.hpp)
target_sources(ur_device_core PRIVATE core/content_wire.cpp core/voice_runtime.cpp)
target_sources(ur_device_core PRIVATE core/http_port.cpp)
target_sources(ur_device_core PRIVATE core/content_upload.cpp)
target_sources(ur_device_core PRIVATE core/json_parse.cpp)
target_sources(ur_device_core PRIVATE core/client_tool.cpp core/client_tool_endpoint.cpp)
target_sources(ur_device_core PRIVATE core/action_wire.cpp)
target_sources(ur_device_core PRIVATE core/action_engine.cpp)
target_sources(ur_device_core PRIVATE core/doorbell_event.cpp)
target_sources(ur_device_core PRIVATE core/privacy_property.cpp)
target_sources(ur_device_core PRIVATE core/human_call.cpp)
target_sources(ur_device_core PRIVATE core/context_snapshot.cpp)
target_sources(ur_device_core PRIVATE core/ai_human_handoff.cpp)
target_sources(ur_device_core PRIVATE core/duplex_focus_driver.cpp)
target_sources(ur_device_core PRIVATE core/shared_mqtt_router.cpp core/shared_device_connection.cpp)
target_sources(ur_device_core PRIVATE core/shared_handoff_focus.cpp)
target_sources(ur_device_core PRIVATE core/ipc_wire.cpp)
target_sources(ur_device_core PRIVATE core/ipc_client.cpp)
target_sources(ur_device_core PRIVATE core/participant_roster.cpp)
target_sources(ur_device_core PRIVATE core/ipc_snapshot_wire.cpp)
target_sources(ur_device_core PRIVATE core/playback_wire.cpp)
target_sources(ur_device_core PRIVATE core/record_playback_wire.cpp)
target_sources(ur_device_core PRIVATE core/record_playback_client.cpp)
target_sources(ur_device_core PRIVATE core/recording_wire.cpp)
target_sources(ur_device_core PRIVATE core/live_wire.cpp)
target_sources(ur_device_core PRIVATE core/live_client.cpp)
target_sources(ur_device_core PRIVATE core/playback_engine.cpp)
target_sources(ur_device_core PRIVATE core/playback_session.cpp)
target_sources(ur_device_core PRIVATE core/playback_endpoint.cpp)
target_sources(ur_device_core PRIVATE core/focus_wire.cpp)
target_sources(ur_device_core PRIVATE core/endpoint_wire.cpp)
target_sources(ur_device_core PRIVATE core/endpoint_handoff_client.cpp)
target_sources(ur_device_core PRIVATE core/ai_endpoint_invite_wire.cpp)
target_sources(ur_device_core PRIVATE core/requests_control_recovery.cpp)
target_sources(ur_device_core PRIVATE core/ai_control_wire.cpp core/ai_control_recovery.cpp)
target_sources(ur_device_core PRIVATE core/voice_control_recovery.cpp core/voice_runtime_control.cpp)
target_sources(ur_device_core PRIVATE core/voice_endpoint.cpp core/voice_runtime_endpoint.cpp)
target_sources(ur_device_core PRIVATE core/focus_engine.cpp)
target_sources(ur_device_core PRIVATE core/focus_endpoint.cpp)
target_sources(ur_device_core PRIVATE core/output_focus_driver.cpp)
target_include_directories(ur_device_core PUBLIC include)
target_include_directories(ur_device_core PRIVATE ${cjson_SOURCE_DIR})
target_link_libraries(ur_device_core PRIVATE cjson)
if(MSVC)
  # 共享源码、中文测试数据及内嵌页面都按UTF-8编译，避免依赖Windows系统代码页。
  # 标准一致性模式修正MSVC旧式转换值类别，确保共享队列测试与GCC/Clang语义一致。
  # 标准预处理器正确识别宏参数中的JSON原始字符串，不能依赖传统预处理器的反斜杠解析。
  target_compile_options(ur_device_core PUBLIC /utf-8 /permissive- /Zc:preprocessor)
  target_compile_options(ur_device_core PRIVATE /W4 /WX /GR-)
else()
  target_compile_options(ur_device_core PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
  # UBSan 的 vptr 检查需要被测多态类生成 RTTI；发布/嵌入式构建仍不依赖 RTTI。
  if(NOT UR_SANITIZER STREQUAL "address")
    target_compile_options(ur_device_core PRIVATE -fno-rtti)
  endif()
endif()
option(UR_DESKTOP_CRYPTO "构建桌面 OpenSSL 密码适配" ON)
option(UR_DESKTOP_TRANSPORT "构建真实 MQTT/UDP/Opus 桌面传输" ON)
option(UR_DESKTOP_HTTP "构建有界流式HTTPS桌面上传端口" ON)
option(UR_DESKTOP_BROWSER "构建回环浏览器媒体桥（桌面界面开发中）" OFF)
option(UR_WINDOWS_PACKAGE "安装Windows桌面程序及依赖，生成便携ZIP" OFF)
if(UR_DESKTOP_BROWSER)
  set(USE_TLS OFF CACHE BOOL "回环浏览器桥不接外部TLS端点" FORCE)
  set(USE_ZLIB OFF CACHE BOOL "媒体消息不使用压缩" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "只在本目标内链接固定依赖" FORCE)
  FetchContent_Declare(ixwebsocket GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG 2efe037c9cc96fd536774f17bdb5215161ee5087)
  FetchContent_MakeAvailable(ixwebsocket)
  include(cmake/ixwebsocket_socket_guard.cmake)
  add_library(ur_browser_bridge adapters/desktop/browser_bridge.cpp adapters/desktop/browser_pcm.cpp
    adapters/desktop/browser_webrtc_media.cpp adapters/desktop/browser_call_media.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_wire.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_call_wire.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_call_focus.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_file_media.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_playback_admission.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_voice_focus.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_duplex_focus.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_human_voice_media.cpp)
  # AI转人工宿主借唯一网络与完整组仲裁；平台身份和状态机不移入网页。
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/shared_handoff_host.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_input_ownership_gate.cpp
    adapters/desktop/browser_input_ownership_wire.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_file_lane.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_file_catalog.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_media_focus.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_live_lane.cpp adapters/desktop/browser_live_media.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_live_focus.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_live_endpoint.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_live_volume.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_client_tool.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_ai_control.cpp)
  target_sources(ur_browser_bridge PRIVATE adapters/desktop/browser_ai_endpoint_focus.cpp)
  target_include_directories(ur_browser_bridge PRIVATE ${cjson_SOURCE_DIR})
  target_include_directories(ur_browser_bridge PUBLIC adapters/desktop)
  target_link_libraries(ur_browser_bridge PUBLIC ur_device_core PRIVATE ixwebsocket cjson)
  if(MSVC)
    target_compile_options(ur_browser_bridge PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_browser_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_DESKTOP_TRANSPORT)
  if(NOT UR_DESKTOP_CRYPTO)
    message(FATAL_ERROR "桌面传输需要 UR_DESKTOP_CRYPTO")
  endif()
  find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)
  find_library(MOSQUITTO_LIBRARY NAMES mosquitto REQUIRED)
  find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus REQUIRED)
  find_library(OPUS_LIBRARY NAMES opus REQUIRED)
  add_library(ur_desktop_transport adapters/desktop/mosquitto_port.cpp
    adapters/desktop/udp_socket.cpp adapters/desktop/opus_codec.cpp adapters/desktop/wave_file.cpp
    adapters/desktop/file_pcm.cpp adapters/desktop/file_actions.cpp)
  target_include_directories(ur_desktop_transport PUBLIC adapters/desktop PRIVATE ${MOSQUITTO_INCLUDE_DIR} ${OPUS_INCLUDE_DIR})
  target_link_libraries(ur_desktop_transport PUBLIC ur_device_core PRIVATE ${MOSQUITTO_LIBRARY} ${OPUS_LIBRARY})
  if(WIN32)
    target_link_libraries(ur_desktop_transport PRIVATE ws2_32)
  endif()
  if(MSVC)
    target_compile_options(ur_desktop_transport PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_desktop_transport PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
option(UR_FUZZ "构建 Clang libFuzzer 解码器测试" OFF)
if(UR_DESKTOP_HTTP)
  # 协议限定字符串需要libcurl 7.85以上；只链接桌面适配，ESP核心不依赖curl。
  find_package(CURL 7.85 REQUIRED)
  add_library(ur_desktop_http adapters/desktop/curl_http_port.cpp)
  target_include_directories(ur_desktop_http PUBLIC adapters/desktop)
  target_link_libraries(ur_desktop_http PUBLIC ur_device_core PRIVATE CURL::libcurl)
  if(MSVC)
    target_compile_options(ur_desktop_http PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_desktop_http PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT UR_SANITIZER STREQUAL "address")
    message(FATAL_ERROR "模糊测试需要 Clang 和 UR_SANITIZER=address")
  endif()
  target_compile_options(ur_device_core PRIVATE -fsanitize=fuzzer-no-link)
  target_compile_options(cjson PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(ur_device_core INTERFACE -fsanitize=fuzzer-no-link)
  add_executable(device_decode_fuzz tests/fuzz_decode.cpp)
  target_link_libraries(device_decode_fuzz PRIVATE ur_device_core)
  target_compile_options(device_decode_fuzz PRIVATE -fsanitize=fuzzer)
  target_compile_options(device_decode_fuzz PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_options(device_decode_fuzz PRIVATE -fsanitize=fuzzer)
endif()
if(UR_DESKTOP_CRYPTO)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  add_library(ur_desktop_crypto adapters/desktop/openssl_ctr.cpp adapters/desktop/device_credentials.cpp)
  target_include_directories(ur_desktop_crypto PUBLIC adapters/desktop)
  target_link_libraries(ur_desktop_crypto PUBLIC ur_device_core PRIVATE OpenSSL::Crypto)
  if(MSVC)
    target_compile_options(ur_desktop_crypto PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_desktop_crypto PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_DESKTOP_HTTP AND UR_DESKTOP_CRYPTO)
  # 附件原件IO只在桌面单任务worker，不给共享核心或ESP引入线程/OpenSSL依赖。
  find_package(Threads REQUIRED)
  add_library(ur_desktop_content adapters/desktop/content_input_worker.cpp)
  target_include_directories(ur_desktop_content PUBLIC adapters/desktop)
  target_link_libraries(ur_desktop_content PUBLIC ur_device_core PRIVATE OpenSSL::Crypto Threads::Threads)
  if(MSVC)
    target_compile_options(ur_desktop_content PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_desktop_content PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_DESKTOP_TRANSPORT)
  # 动作日志属于桌面驱动层；共享核心仅使用非阻塞固化探针，不依赖文件或线程。
  find_package(Threads REQUIRED)
  add_library(ur_desktop_action_ledger adapters/desktop/action_file_ledger.cpp)
  target_include_directories(ur_desktop_action_ledger PUBLIC adapters/desktop PRIVATE ${cjson_SOURCE_DIR})
  target_link_libraries(ur_desktop_action_ledger PUBLIC ur_device_core PRIVATE OpenSSL::Crypto Threads::Threads)
  if(MSVC)
    target_compile_options(ur_desktop_action_ledger PRIVATE /W4 /WX)
  else()
    target_compile_options(ur_desktop_action_ledger PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_executable(device_voice_file adapters/desktop/voice_file_main.cpp)
  add_executable(device_text adapters/desktop/text_main.cpp)
  target_link_libraries(device_voice_file PRIVATE ur_desktop_transport ur_desktop_crypto ur_desktop_action_ledger OpenSSL::Crypto)
  target_link_libraries(device_text PRIVATE ur_desktop_transport ur_desktop_crypto OpenSSL::Crypto)
  if(TARGET ur_desktop_content)
    target_link_libraries(device_text PRIVATE ur_desktop_content ur_desktop_http)
    target_compile_definitions(device_text PRIVATE UR_DESKTOP_CONTENT_ENABLED=1)
  endif()
  if(MSVC)
    target_compile_options(device_voice_file PRIVATE /W4 /WX)
    target_compile_options(device_text PRIVATE /W4 /WX)
  else()
    target_compile_options(device_voice_file PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(device_text PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_DESKTOP_BROWSER AND UR_DESKTOP_TRANSPORT)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(UR_ASSET_TEST_ARGUMENTS)
  if(BUILD_TESTING)
    list(APPEND UR_ASSET_TEST_ARGUMENTS --test-fixtures)
  endif()
  execute_process(COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_web_assets.py
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${UR_ASSET_TEST_ARGUMENTS}
    COMMAND_ERROR_IS_FATAL ANY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    adapters/desktop/web/index.html adapters/desktop/web/app.css
    adapters/desktop/web/app.js adapters/desktop/web/audio-worklet.js adapters/desktop/web/client-theme.js
    adapters/desktop/web/input-ownership.js adapters/desktop/web/human-voice-media.js)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    adapters/desktop/web/video.html adapters/desktop/web/video.js adapters/desktop/web/call-worklet.js
    adapters/desktop/web_assets.hpp.in adapters/desktop/video_assets.hpp.in scripts/embed_web_assets.py)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    adapters/desktop/web/file-player.js adapters/desktop/web/file-worklet.js adapters/desktop/web/file-lane.js adapters/desktop/web/file-host.js adapters/desktop/file_assets.hpp.in)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    adapters/desktop/web/live-probe.js adapters/desktop/web/live-player.js
    adapters/desktop/web/live-host.js adapters/desktop/web/live-worklet.js
    adapters/desktop/web/live-transform-worker.js adapters/desktop/live_assets.hpp.in)
  add_executable(device_browser adapters/desktop/browser_main.cpp)
  add_executable(device_video_call adapters/desktop/video_call_main.cpp)
  add_executable(device_ai_handoff adapters/desktop/shared_handoff_main.cpp)
  target_include_directories(device_browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_include_directories(device_video_call PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_include_directories(device_ai_handoff PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(device_browser PRIVATE ur_browser_bridge ur_desktop_transport ur_desktop_crypto ur_desktop_action_ledger OpenSSL::Crypto)
  # 宿主只解析固定本地桥报文；cJSON依赖不进入共享设备核心或其他目标。
  target_include_directories(device_browser PRIVATE ${cjson_SOURCE_DIR})
  target_link_libraries(device_video_call PRIVATE ur_browser_bridge ur_desktop_transport ur_desktop_crypto OpenSSL::Crypto)
  target_link_libraries(device_ai_handoff PRIVATE ur_browser_bridge ur_desktop_transport ur_desktop_crypto ur_desktop_action_ledger OpenSSL::Crypto)
  if(MSVC)
    target_compile_options(device_browser PRIVATE /W4 /WX)
    target_compile_options(device_video_call PRIVATE /W4 /WX)
    target_compile_options(device_ai_handoff PRIVATE /W4 /WX)
  else()
    target_compile_options(device_browser PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(device_video_call PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(device_ai_handoff PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
if(UR_DESKTOP_BROWSER AND UR_DESKTOP_TRANSPORT)
  # IPC媒体与AI/人际状态机独立，共享网络、密码和回环桥驱动。
  add_subdirectory(desktop-ipc)
endif()
if(UR_SANITIZER)
  if(MSVC OR NOT UR_SANITIZER MATCHES "^(address|thread)$")
    message(FATAL_ERROR "UR_SANITIZER 仅支持 GCC/Clang 的 address 或 thread")
  endif()
  set(UR_SAN_FLAGS "-fsanitize=${UR_SANITIZER}")
  if(UR_SANITIZER STREQUAL "address")
    set(UR_SAN_FLAGS "-fsanitize=address,undefined")
  endif()
  target_compile_options(ur_device_core PUBLIC ${UR_SAN_FLAGS} -fno-omit-frame-pointer)
  target_link_options(ur_device_core PUBLIC ${UR_SAN_FLAGS})
  target_compile_options(cjson PRIVATE ${UR_SAN_FLAGS} -fno-omit-frame-pointer)
  if(TARGET ur_browser_bridge)
    target_compile_options(ur_browser_bridge PUBLIC ${UR_SAN_FLAGS} -fno-omit-frame-pointer)
    target_link_options(ur_browser_bridge PUBLIC ${UR_SAN_FLAGS})
  endif()
endif()
if(UR_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR UR_SANITIZER)
    message(FATAL_ERROR "覆盖率使用独立 GCC 构建，不与 sanitizer 混用")
  endif()
  target_compile_options(ur_device_core PUBLIC --coverage -O0 -g)
  target_link_options(ur_device_core PUBLIC --coverage)
endif()
if(BUILD_TESTING)
  if(WIN32)
    # Windows时钟偏差回归直接验证正式PowerShell入口，不启动重复构建。
    find_program(UR_POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
    add_test(NAME desktop_windows_build_script COMMAND ${UR_POWERSHELL_EXECUTABLE}
      -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_build_script_test.ps1)
  endif()
  find_package(GTest 1.16.0 EXACT CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(device_core_tests tests/session_test.cpp tests/fixed_queue_test.cpp tests/udp_channel_test.cpp tests/requests_test.cpp tests/envelope_test.cpp tests/udp_audio_test.cpp tests/voice_wire_test.cpp tests/voice_client_test.cpp tests/reply_datagram_test.cpp tests/udp_voice_media_test.cpp tests/buffered_pcm_test.cpp)
  target_sources(device_core_tests PRIVATE tests/voice_reply_install_test.cpp tests/audio_capture_gate_test.cpp)
  target_sources(device_core_tests PRIVATE tests/content_wire_test.cpp tests/voice_runtime_test.cpp)
  target_sources(device_core_tests PRIVATE tests/voice_runtime_shared_test.cpp)
  target_sources(device_core_tests PRIVATE tests/voice_endpoint_test.cpp)
  target_sources(device_core_tests PRIVATE tests/voice_runtime_endpoint_release_test.cpp)
  target_sources(device_core_tests PRIVATE tests/text_only_test.cpp)
  # AI视觉仍复用语音核心；独立核摄像头绑定、初始暂停和能力未就绪时的拒绝。
  target_sources(device_core_tests PRIVATE tests/voice_vision_test.cpp)
  # 摄像头发布/停止以异步驱动实态回执为准，覆盖原token、超时及迟到资源清理。
  target_sources(device_core_tests PRIVATE tests/voice_vision_control_test.cpp)
  target_sources(device_core_tests PRIVATE tests/http_port_test.cpp)
  target_sources(device_core_tests PRIVATE tests/content_upload_test.cpp)
  target_sources(device_core_tests PRIVATE tests/json_parse_test.cpp)
  target_sources(device_core_tests PRIVATE tests/client_tool_test.cpp tests/client_tool_endpoint_test.cpp tests/voice_runtime_client_tool_test.cpp)
  target_sources(device_core_tests PRIVATE tests/action_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/action_engine_test.cpp)
  target_sources(device_core_tests PRIVATE tests/doorbell_event_test.cpp)
  target_sources(device_core_tests PRIVATE tests/privacy_property_test.cpp)
  target_sources(device_core_tests PRIVATE tests/human_call_test.cpp)
  target_sources(device_core_tests PRIVATE tests/context_snapshot_test.cpp)
  target_sources(device_core_tests PRIVATE tests/human_call_voice_test.cpp)
  target_sources(device_core_tests PRIVATE tests/human_endpoint_candidate_test.cpp)
  target_sources(device_core_tests PRIVATE tests/ai_human_handoff_test.cpp)
  target_sources(device_core_tests PRIVATE tests/duplex_focus_driver_test.cpp)
  target_sources(device_core_tests PRIVATE tests/shared_mqtt_router_test.cpp tests/shared_device_connection_test.cpp)
  target_sources(device_core_tests PRIVATE tests/shared_handoff_focus_test.cpp)
  # 单设备共享连接与物理输入占用独立于逻辑会话，不借用输出焦点来授予采集权限。
  target_sources(device_core_tests PRIVATE tests/capture_gate_test.cpp tests/human_call_shared_test.cpp)
  target_sources(device_core_tests PRIVATE tests/ipc_client_test.cpp)
  target_sources(device_core_tests PRIVATE tests/participant_roster_test.cpp)
  target_sources(device_core_tests PRIVATE tests/ipc_snapshot_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/playback_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/record_playback_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/record_playback_client_test.cpp)
  target_sources(device_core_tests PRIVATE tests/recording_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/live_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/live_client_test.cpp)
  target_sources(device_core_tests PRIVATE tests/playback_engine_test.cpp)
  target_sources(device_core_tests PRIVATE tests/playback_session_test.cpp)
  target_sources(device_core_tests PRIVATE tests/playback_endpoint_test.cpp)
  target_sources(device_core_tests PRIVATE tests/focus_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/endpoint_handoff_client_test.cpp)
  target_sources(device_core_tests PRIVATE tests/ai_endpoint_invite_wire_test.cpp)
  target_sources(device_core_tests PRIVATE tests/requests_control_recovery_test.cpp)
  target_sources(device_core_tests PRIVATE tests/ai_control_wire_test.cpp tests/ai_control_recovery_test.cpp)
  target_sources(device_core_tests PRIVATE tests/voice_control_recovery_test.cpp tests/voice_runtime_control_test.cpp)
  target_sources(device_core_tests PRIVATE tests/focus_engine_test.cpp)
  target_sources(device_core_tests PRIVATE tests/focus_endpoint_test.cpp)
  target_sources(device_core_tests PRIVATE tests/output_focus_driver_test.cpp)
  target_link_libraries(device_core_tests PRIVATE ur_device_core GTest::gtest_main)
  # 分配失败用例只对cJSON设置临时钩子，验证低内存设备仍切断输出。
  target_include_directories(device_core_tests PRIVATE ${cjson_SOURCE_DIR})
  if(MSVC)
    target_compile_options(device_core_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(device_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  include(GoogleTest)
  if(UR_DESKTOP_HTTP)
    add_executable(desktop_http_tests tests/curl_http_port_test.cpp)
    target_link_libraries(desktop_http_tests PRIVATE ur_desktop_http GTest::gtest_main)
    if(MSVC)
      target_compile_options(desktop_http_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(desktop_http_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME desktop_real_https COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_https_test.py $<TARGET_FILE:desktop_http_tests>)
    set_tests_properties(desktop_real_https PROPERTIES TIMEOUT 45 LABELS integration)
  endif()
  if(UR_DESKTOP_BROWSER)
    # AI候选的原双向HAL屏障直接使用正式适配器，不能将scope就绪当资源已释放。
    add_executable(browser_ai_endpoint_focus_tests tests/browser_ai_endpoint_focus_test.cpp)
    target_link_libraries(browser_ai_endpoint_focus_tests PRIVATE ur_browser_bridge GTest::gtest_main)
    # 真实Runtime与候选HAL共享显式时钟，排队旧快照不得毒化当前资源权限。
    target_sources(browser_ai_endpoint_focus_tests PRIVATE tests/browser_ai_endpoint_scope_clock_test.cpp)
    target_include_directories(browser_ai_endpoint_focus_tests PRIVATE ${cjson_SOURCE_DIR})
    if(MSVC)
      target_compile_options(browser_ai_endpoint_focus_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(browser_ai_endpoint_focus_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    gtest_discover_tests(browser_ai_endpoint_focus_tests)
    # 独立验证真实输入票据和严格本地报文，不依赖浏览器服务或共享网络事件循环。
    add_executable(browser_input_ownership_tests tests/browser_input_ownership_gate_test.cpp
      adapters/desktop/browser_input_ownership_gate.cpp
      adapters/desktop/browser_input_ownership_wire.cpp)
    target_include_directories(browser_input_ownership_tests PRIVATE adapters/desktop ${cjson_SOURCE_DIR})
    target_link_libraries(browser_input_ownership_tests PRIVATE ur_device_core cjson GTest::gtest_main)
    if(MSVC)
      target_compile_options(browser_input_ownership_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(browser_input_ownership_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    gtest_discover_tests(browser_input_ownership_tests)
    if(UR_DESKTOP_TRANSPORT)
      # 直接包含正式宿主入口，验证视觉配置与原始回执分派，不另建测试协议实现。
      add_executable(browser_vision_host_tests tests/browser_vision_host_test.cpp)
      target_include_directories(browser_vision_host_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${cjson_SOURCE_DIR})
      target_link_libraries(browser_vision_host_tests PRIVATE ur_browser_bridge ur_desktop_transport
        ur_desktop_crypto ur_desktop_action_ledger OpenSSL::Crypto GTest::gtest_main)
      if(MSVC)
        target_compile_options(browser_vision_host_tests PRIVATE /W4 /WX)
      else()
        target_compile_options(browser_vision_host_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      gtest_discover_tests(browser_vision_host_tests)
      add_executable(desktop_assets_tests tests/desktop_assets_test.cpp)
      target_include_directories(desktop_assets_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
      target_compile_definitions(desktop_assets_tests PRIVATE UR_ASSET_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/adapters/desktop/web")
      # 真正生成资产必须通过原回环桥白名单与启动集成，字节一致不能代替正式程序可启动。
      target_link_libraries(desktop_assets_tests PRIVATE ur_browser_bridge ixwebsocket GTest::gtest_main)
      if(MSVC)
        target_compile_options(desktop_assets_tests PRIVATE /W4 /WX)
      else()
        target_compile_options(desktop_assets_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      gtest_discover_tests(desktop_assets_tests)
    endif()
    # 验证正式第三方修补脚本在无生成副本的冷目录以及重复配置时均可用。
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME desktop_socket_guard_configure COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_socket_guard.py ${CMAKE_COMMAND}
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ixwebsocket_socket_guard.cmake)
    find_program(UR_NODE_EXECUTABLE NAMES node REQUIRED)
    if(TARGET device_ai_handoff)
      # 固定无凭据原生入口，只核严格开关与纯Human拒绝，不启动媒体/网络/模型。
      add_test(NAME desktop_shared_udp_reply_receipt_native COMMAND ${CMAKE_COMMAND} -E env
        TASK143_HANDOFF_BINARY=$<TARGET_FILE:device_ai_handoff> ${UR_NODE_EXECUTABLE} --test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_udp_reply_receipt_native.test.cjs)
      set_tests_properties(desktop_shared_udp_reply_receipt_native PROPERTIES TIMEOUT 30)
      add_test(NAME desktop_shared_half_duplex_native COMMAND ${CMAKE_COMMAND} -E env
        TASK143_HANDOFF_BINARY=$<TARGET_FILE:device_ai_handoff> ${UR_NODE_EXECUTABLE} --test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_half_duplex_native.test.cjs)
      set_tests_properties(desktop_shared_half_duplex_native PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME desktop_audio_worklet COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_worklet.test.cjs)
    add_test(NAME desktop_ai_endpoint_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_app.test.cjs)
    # 源端本人prepare/commit/abort只发意图，完整平台身份始终由原C++控制器负责。
    add_test(NAME desktop_ai_endpoint_source_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_source_app.test.cjs)
    # 实物UDP旁证必须在首次泵接收前启用，结构检查不替代真实UDP用例。
    add_test(NAME desktop_udp_receive_diagnostic_main COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_udp_receive_diagnostic_main.test.cjs)
    # 三端接续验收器只跑离线合同与失败脱敏，不在CTest自动连接平台或收费模型。
    add_test(NAME desktop_ai_endpoint_live_contract_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_live.test.cjs)
    # 目标恢复与过期真实停轨判据只跑离线合同，不启动浏览器/网络或收费模型。
    add_test(NAME desktop_ai_endpoint_target_recovery_contract_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_target_recovery_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_expired_capture_contract.test.cjs)
    # 停轨验收器在CTest只检查离线判据；真实Chromium须显式开关及独立资源门禁。
    add_test(NAME desktop_browser_resource_release_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_resource_release_live.test.cjs)
    # 冻结C++合成错误ACK验收器只离线检查编码，绝不在CTest启动MQTT或真实浏览器。
    add_test(NAME desktop_browser_cpp_ack_release_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_cpp_ack_release_live.test.cjs)
    # 无网络浏览器桥仍为合法最小配置；仅实际设备进程存在时注册其装配拒绝用例。
    if(TARGET device_browser)
      add_test(NAME desktop_ai_endpoint_control_sync_configuration COMMAND ${CMAKE_COMMAND} -E env
        UR_DEVICE_BROWSER_TEST_BINARY=$<TARGET_FILE:device_browser> ${UR_NODE_EXECUTABLE} --test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_endpoint_control_sync_configuration.test.cjs)
    endif()
    # 原生时钟事实只供测量，缺失或重叠不能由页面补造首音延迟。
    add_test(NAME desktop_reply_timing_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_reply_timing_app.test.cjs)
    # 独立宿主scope首因只按闭集互解；不连接浏览器、平台或读取授权身份。
    add_test(NAME desktop_scope_failure_diagnostic COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/scope_failure_diagnostic.test.cjs)
    # 独立UDP预信令计数只在本机观察，不改变MQTT消息或未知包缓存上限。
    add_test(NAME desktop_presignal_failure_diagnostic COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/presignal_failure_diagnostic.test.cjs)
    # 单音轨候选与专属输出图只借原共享采集源；真实RTP及非静音另由组件入口验收。
    add_test(NAME desktop_human_voice_media COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/human_voice_media.test.cjs)
    # 仅离线证据验收器回归；实际浏览器入口必须另走资源门禁。
    add_test(NAME desktop_audio_gate_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_gate_live.test.cjs)
    add_test(NAME desktop_file_worklet COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_worklet.test.cjs)
    add_test(NAME desktop_file_player COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_player.test.cjs)
    add_test(NAME desktop_file_lane COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_lane.test.cjs)
    add_test(NAME desktop_file_host COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_host.test.cjs)
    add_test(NAME desktop_live_worklet COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_worklet.test.cjs)
    add_test(NAME desktop_live_transform COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_transform_worker.test.cjs)
    add_test(NAME desktop_live_probe COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_probe.test.cjs)
    add_test(NAME desktop_live_player COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_player.test.cjs)
    add_test(NAME desktop_live_host COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_host.test.cjs)
    add_test(NAME desktop_live_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_live_app.test.cjs)
    add_test(NAME desktop_live_encoded_source COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_encoded_source.test.cjs)
    # 显式启动路径选择器只做离线校验，不自动启动真实浏览器或MQTT。
    add_test(NAME desktop_voice_startup_modes COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/voice_focus_startup_modes.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/voice_focus_startup_observation.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_startup_observation.test.cjs)
    # 仅语料单测；真实浏览器解码另走受资源门禁约束的file_media_live入口。
    add_test(NAME desktop_file_media_fixture COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_media_live.test.cjs)
    # 正式浏览器页的首可播放帧判定；DOM仅在隔离VM内模拟。
    add_test(NAME desktop_browser_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_app.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_text_only.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_input_assembly.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_theme.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_client_theme_app.test.cjs)
    # 客户端工具真实验收器只在此执行离线控制面回归，不启动浏览器、平台或模型。
    add_test(NAME desktop_client_tool_model_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_client_tool_live.test.cjs)
    # 正式MCP设备动作验收必须同时核对驱动效果、原回复关联和实际播放消费。
    add_test(NAME desktop_device_action_model_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_device_action_live.test.cjs)
    set_tests_properties(desktop_device_action_model_report PROPERTIES LABELS unit TIMEOUT 20)
    # 取消竞态只核验原事实延迟与判据；真实收费模型链另由显式开关运行。
    add_test(NAME desktop_client_tool_cancel_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_client_tool_cancel_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_udp_control.test.cjs)
    set_tests_properties(desktop_client_tool_cancel_report PROPERTIES LABELS unit TIMEOUT 20)
    # 透明MQTT延迟桥的真实回环与拒绝判据，不生成收费模型或生产设备报文。
    add_test(NAME desktop_client_tool_execute_late_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mqtt_execute_delay.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_client_tool_execute_late_live.test.cjs)
    set_tests_properties(desktop_client_tool_execute_late_report PROPERTIES LABELS unit TIMEOUT 20)
    # 人际保持的实际发送轨/输出块合同及正式双端验收器离线检查，不自动启用真实设备或平台。
    add_test(NAME desktop_call_media_control COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/video_call_control.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/call_worklet.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/device_pair_live.test.cjs)
    # 共享转人工端云验收器仅跑离线控制端口与页面VM，不启动设备、浏览器、后端或真实模型。
    add_test(NAME desktop_shared_handoff_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_handoff_live.test.cjs)
    # 纯人际三宿主调度只用离线端口；正式入口仍需独立真实产品和平台权威旁证。
    add_test(NAME desktop_native_human_three_host_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_human_three_host_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_human_transfer_observer.test.cjs)
    # 原生语音与长保持仅校验离线判据及逐轮计时，不启动模型、浏览器或设备。
    add_test(NAME desktop_native_temporary_voice_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/temporary_call_voice_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/temporary_call_long_hold_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/temporary_call_live.test.cjs)
    # 点击准备仅验证闭集视口/命中，不调用DOM click或启动真实会话。
    add_test(NAME desktop_actual_click_preparation COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_click.test.cjs)
    add_test(NAME desktop_browser_webrtc_app COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_webrtc_app.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_webrtc_text_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_webrtc_voice_fixture.test.cjs)
    # WebRTC焦点旁证只验证正式页面函数与VM夹具，不发网络或创建实际媒体资源。
    add_test(NAME desktop_webrtc_focus_observation COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_webrtc_focus_observation.test.cjs)
    # 真实文件设备日志验收器的离线回归，不启动模型或访问网络。
    add_test(NAME desktop_voice_live_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/voice_live_report.test.cjs)
    # 真实模型与文件媒体入口的离线契约验证，不自动启用收费API或媒体副作用。
    # 输入字面旁观测试随原媒体验收器进入同一CTest，不靠手工单独发现新增用例。
    add_test(NAME desktop_mcp_file_media_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_file_media_live.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_input_literal_diagnostic.test.cjs)
    # 实时节目只读证据与逐轮意图验证独立于文件播放，离线测试不调用收费模型。
    add_test(NAME desktop_mcp_live_broadcast_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_live_broadcast_live.test.cjs)
    # 模型工具诊断的固定脱敏投影；只跑离线报文，不开启后端或收费模型。
    add_test(NAME desktop_ai_tool_trace_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_trace_projection.test.cjs
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/media_branch_projection.test.cjs)
    # Broker实际logger投影仅观察原pipe，不启动服务或变更投递。
    add_test(NAME desktop_mqtt_delivery_trace_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mqtt_delivery_projection.test.cjs)
    # 独立线程取证合同含临时回环HTTP，原实例未知时不借旧状态或控制设备。
    add_test(NAME desktop_thread_progress_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_progress_observer.test.cjs)
    # 初始化前检查正式身份和模型工具契约，不写数据或启动收费模型。
    add_test(NAME desktop_mcp_media_prepare_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_media_prepare.test.cjs)
    add_executable(browser_bridge_tests tests/browser_bridge_test.cpp tests/browser_pcm_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_udp_reply_receipt_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_ai_control_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_bridge_start_diagnostic_test.cpp)
    set_source_files_properties(tests/browser_bridge_start_diagnostic_test.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
    target_sources(browser_bridge_tests PRIVATE tests/browser_pcm_input_route_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_human_voice_media_test.cpp)
    if(UNIX AND TARGET ur_desktop_crypto AND TARGET device_browser)
      # 只提供实际浏览器组件验收入口，不安装或作为生产设备客户端发布。
      add_executable(file_media_probe tests/file_media_probe.cpp)
      target_include_directories(file_media_probe PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
      target_compile_definitions(file_media_probe PRIVATE UR_FILE_PROBE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests")
      target_compile_options(file_media_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
      target_link_libraries(file_media_probe PRIVATE ur_browser_bridge OpenSSL::Crypto)
      # 实际WebAudio门控探针仅用于软件验收，不持MQTT身份或授予业务焦点。
      add_executable(audio_gate_probe tests/audio_gate_probe.cpp)
      target_include_directories(audio_gate_probe PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
      target_compile_definitions(audio_gate_probe PRIVATE UR_GATE_PROBE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests")
      target_compile_options(audio_gate_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
      target_link_libraries(audio_gate_probe PRIVATE ur_browser_bridge OpenSSL::Crypto)
    endif()
    target_sources(browser_bridge_tests PRIVATE tests/browser_media_focus_test.cpp tests/browser_file_catalog_test.cpp tests/browser_file_lane_test.cpp tests/browser_voice_focus_test.cpp tests/browser_wire_test.cpp
      tests/browser_webrtc_media_test.cpp tests/browser_call_media_test.cpp tests/browser_call_shared_test.cpp tests/browser_file_media_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_lane_test.cpp tests/browser_live_media_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_startup_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_focus_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_endpoint_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_volume_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_release_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_client_tool_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_call_focus_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/browser_duplex_focus_test.cpp)
    target_sources(browser_bridge_tests PRIVATE tests/shared_handoff_host_test.cpp)
    if(TARGET ur_desktop_transport AND TARGET ur_desktop_crypto)
      # 新合同只用真正BrowserPcm/Opus/原生UDP/AES组件，不能以通用fake声明硬件或替代旧Human测试。
      target_sources(browser_bridge_tests PRIVATE tests/shared_udp_reply_receipt_test.cpp)
      target_link_libraries(browser_bridge_tests PRIVATE ur_desktop_transport ur_desktop_crypto)
    endif()
    target_include_directories(browser_bridge_tests PRIVATE ${cjson_SOURCE_DIR})
    target_link_libraries(browser_bridge_tests PRIVATE ur_browser_bridge ixwebsocket GTest::gtest_main)
    if(MSVC)
      target_compile_options(browser_bridge_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(browser_bridge_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    # 正式Runtime与原Live端点的路由/终态释放集成，不以直接组件调用代替宿主闭环。
    target_sources(browser_bridge_tests PRIVATE tests/browser_live_runtime_cleanup_test.cpp)
    add_test(NAME desktop_browser_bridge COMMAND browser_bridge_tests)
    set_tests_properties(desktop_browser_bridge PROPERTIES TIMEOUT 60 LABELS integration)
  endif()
  gtest_discover_tests(device_core_tests)
  add_executable(device_wire_contract tests/wire_contract_driver.cpp)
  target_link_libraries(device_wire_contract PRIVATE ur_device_core)
  # 新文字契约读取原JSON对象字段，仅此测试工具使用固定cJSON依赖头。
  target_include_directories(device_wire_contract PRIVATE ${cjson_SOURCE_DIR})
  if(MSVC)
    target_compile_options(device_wire_contract PRIVATE /W4 /WX)
  else()
    target_compile_options(device_wire_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(UR_DESKTOP_TRANSPORT)
    add_executable(device_udp_contract tests/udp_contract_driver.cpp)
    target_link_libraries(device_udp_contract PRIVATE ur_desktop_transport ur_desktop_crypto)
    if(MSVC)
      target_compile_options(device_udp_contract PRIVATE /W4 /WX)
    else()
      target_compile_options(device_udp_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    # 安装确认跨进程夹具依赖POSIX私有管道，需由Go父测试提供协议输入，不单独登记空CTest。
    if(UNIX)
      add_executable(device_reply_install_contract tests/udp_reply_install_kernel_driver.cpp)
      target_link_libraries(device_reply_install_contract PRIVATE ur_desktop_transport ur_desktop_crypto)
      target_compile_options(device_reply_install_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_executable(desktop_transport_tests tests/desktop_transport_test.cpp tests/desktop_environment_test.cpp)
    target_link_libraries(desktop_transport_tests PRIVATE ur_desktop_transport ur_desktop_crypto GTest::gtest_main)
    # 纯文字真实网络回归读取生产请求的inputId；不暴露测试头路径到其它目标。
    target_include_directories(desktop_transport_tests PRIVATE ${cjson_SOURCE_DIR})
    if(MSVC)
      target_compile_options(desktop_transport_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(desktop_transport_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME desktop_real_transport COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_broker_test.py $<TARGET_FILE:desktop_transport_tests>)
    set_tests_properties(desktop_real_transport PROPERTIES TIMEOUT 60 LABELS integration)
    # 真正pipe驱动文字设备与受控MQTT TCP对端，不启用媒体或真实模型。
    add_test(NAME desktop_text_entry COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/text_entry_test.py $<TARGET_FILE:device_text>)
    set_tests_properties(desktop_text_entry PROPERTIES TIMEOUT 40 LABELS integration)
    if(TARGET ur_desktop_content)
      # 真TLS＋受控MQTT验证附件宿主完整上传/typed提交及取消；不是实际平台/模型验收。
      add_test(NAME desktop_content_text_entry COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/content_text_entry_test.py $<TARGET_FILE:device_text>)
      set_tests_properties(desktop_content_text_entry PROPERTIES TIMEOUT 60 LABELS integration)
    endif()
    # 只验证真实模型验收器的离线控制面，CTest不自动调用供应商或使用设备凭据。
    find_program(UR_NODE_EXECUTABLE NAMES node REQUIRED)
    # AI原连接恢复只跑离线页面/验收器合同，不自动连接MQTT、真实模型或桌面驱动。
    add_test(NAME desktop_ai_control_browser_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_control_app.test.cjs)
    add_test(NAME desktop_ai_control_live_contract_unit COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_ai_control_live.test.cjs)
    add_test(NAME desktop_text_model_report COMMAND ${UR_NODE_EXECUTABLE} --test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/text_only_live.test.cjs)
    if(UNIX AND TARGET ur_desktop_content)
      # 附件真实验收器只跑离线控制面及回环TLS转发，不启平台/S3或收费模型；Windows另验。
      add_test(NAME desktop_content_model_report COMMAND ${UR_NODE_EXECUTABLE} --test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/device_content_live.test.cjs)
      set_tests_properties(desktop_content_model_report PROPERTIES TIMEOUT 30 LABELS integration)
    endif()
    add_executable(desktop_wave_tests tests/wave_file_test.cpp tests/file_pcm_test.cpp tests/file_actions_test.cpp)
    target_link_libraries(desktop_wave_tests PRIVATE ur_desktop_transport GTest::gtest_main)
    if(MSVC)
      target_compile_options(desktop_wave_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(desktop_wave_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    gtest_discover_tests(desktop_wave_tests)
  endif()
  if(UR_DESKTOP_CRYPTO)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ur_desktop_action_ledger)
      # 跨进程崩溃/重放使用独立软件驱动；不将文件标记称为硬件或MQTT验收。
      add_executable(action_ledger_process tests/action_ledger_process.cpp)
      target_link_libraries(action_ledger_process PRIVATE ur_desktop_action_ledger)
      target_compile_options(action_ledger_process PRIVATE -Wall -Wextra -Wpedantic -Werror)
      # 链接包装仅故障测试进程的原write/fsync；不注入生产程序、全局环境或Linux外平台。
      add_executable(action_ledger_fault_process tests/action_ledger_fault_process.cpp)
      target_link_libraries(action_ledger_fault_process PRIVATE ur_desktop_action_ledger)
      target_compile_options(action_ledger_fault_process PRIVATE -Wall -Wextra -Wpedantic -Werror)
      target_link_options(action_ledger_fault_process PRIVATE -Wl,--wrap=write,--wrap=fsync)
      add_executable(action_file_ledger_tests tests/action_file_ledger_test.cpp)
      target_link_libraries(action_file_ledger_tests PRIVATE ur_desktop_action_ledger GTest::gtest_main)
      target_compile_options(action_file_ledger_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
      target_compile_definitions(action_file_ledger_tests PRIVATE UR_ACTION_LEDGER_HELPER="$<TARGET_FILE:action_ledger_process>")
      target_compile_definitions(action_file_ledger_tests PRIVATE UR_ACTION_LEDGER_FAULT_HELPER="$<TARGET_FILE:action_ledger_fault_process>")
      add_dependencies(action_file_ledger_tests action_ledger_process action_ledger_fault_process)
      gtest_discover_tests(action_file_ledger_tests)
    endif()
    if(TARGET ur_desktop_content)
      # Worker真实原文件/SHA256/取消回归使用可控HTTP端口，不启动平台或OSS。
      add_executable(desktop_content_input_tests tests/content_input_worker_test.cpp)
      target_include_directories(desktop_content_input_tests PRIVATE ${cjson_SOURCE_DIR})
      target_link_libraries(desktop_content_input_tests PRIVATE ur_desktop_content OpenSSL::Crypto GTest::gtest_main)
      if(MSVC)
        target_compile_options(desktop_content_input_tests PRIVATE /W4 /WX)
      else()
        target_compile_options(desktop_content_input_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      gtest_discover_tests(desktop_content_input_tests)
    endif()
    add_executable(device_crypto_tests tests/openssl_ctr_test.cpp)
    target_link_libraries(device_crypto_tests PRIVATE ur_desktop_crypto GTest::gtest_main)
    if(MSVC)
      target_compile_options(device_crypto_tests PRIVATE /W4 /WX)
    else()
      target_compile_options(device_crypto_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    gtest_discover_tests(device_crypto_tests)
  endif()
endif()
if(UR_WINDOWS_PACKAGE)
  include(cmake/windows_package.cmake)
endif()
