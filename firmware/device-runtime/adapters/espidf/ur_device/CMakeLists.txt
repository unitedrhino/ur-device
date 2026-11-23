# ESP-IDF复用全部可移植核心；不引入桌面MQTT、OpenSSL、文件或操作系统驱动。
set(UR_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../core")
idf_component_register(
  SRCS "${UR_CORE_DIR}/session.cpp" "${UR_CORE_DIR}/udp_channel.cpp"
    "${UR_CORE_DIR}/requests.cpp" "${UR_CORE_DIR}/envelope.cpp"
    "${UR_CORE_DIR}/audio_codec.cpp" "${UR_CORE_DIR}/udp_audio.cpp"
    "${UR_CORE_DIR}/voice_wire.cpp" "${UR_CORE_DIR}/voice_client.cpp"
    "${UR_CORE_DIR}/reply_datagram.cpp" "${UR_CORE_DIR}/udp_voice_media.cpp"
    "${UR_CORE_DIR}/buffered_pcm.cpp" "${UR_CORE_DIR}/content_wire.cpp"
    "${UR_CORE_DIR}/http_port.cpp"
    "${UR_CORE_DIR}/content_upload.cpp"
    "${UR_CORE_DIR}/json_parse.cpp"
    "${UR_CORE_DIR}/voice_runtime.cpp" "${UR_CORE_DIR}/action_wire.cpp"
    "${UR_CORE_DIR}/client_tool.cpp" "${UR_CORE_DIR}/client_tool_endpoint.cpp"
    "${UR_CORE_DIR}/action_engine.cpp" "${UR_CORE_DIR}/doorbell_event.cpp"
    "${UR_CORE_DIR}/privacy_property.cpp"
    "${UR_CORE_DIR}/human_call.cpp"
    "${UR_CORE_DIR}/context_snapshot.cpp"
    "${UR_CORE_DIR}/ai_human_handoff.cpp"
    "${UR_CORE_DIR}/participant_roster.cpp"
    "${UR_CORE_DIR}/ipc_wire.cpp"
    "${UR_CORE_DIR}/ipc_snapshot_wire.cpp"
    "${UR_CORE_DIR}/ipc_client.cpp"
    "${UR_CORE_DIR}/playback_wire.cpp"
    "${UR_CORE_DIR}/record_playback_wire.cpp"
    "${UR_CORE_DIR}/recording_wire.cpp"
    "${UR_CORE_DIR}/record_playback_client.cpp"
    "${UR_CORE_DIR}/live_wire.cpp"
    "${UR_CORE_DIR}/live_client.cpp"
    "${UR_CORE_DIR}/playback_engine.cpp"
    "${UR_CORE_DIR}/playback_session.cpp"
    "${UR_CORE_DIR}/playback_endpoint.cpp"
    "${UR_CORE_DIR}/focus_wire.cpp"
    "${UR_CORE_DIR}/endpoint_wire.cpp"
    "${UR_CORE_DIR}/ai_endpoint_invite_wire.cpp"
    "${UR_CORE_DIR}/endpoint_handoff_client.cpp"
    "${UR_CORE_DIR}/requests_control_recovery.cpp"
    "${UR_CORE_DIR}/ai_control_wire.cpp" "${UR_CORE_DIR}/ai_control_recovery.cpp"
    "${UR_CORE_DIR}/voice_control_recovery.cpp" "${UR_CORE_DIR}/voice_runtime_control.cpp"
    "${UR_CORE_DIR}/voice_endpoint.cpp" "${UR_CORE_DIR}/voice_runtime_endpoint.cpp"
    "${UR_CORE_DIR}/focus_engine.cpp"
    "${UR_CORE_DIR}/focus_endpoint.cpp"
    "${UR_CORE_DIR}/output_focus_driver.cpp"
    "${UR_CORE_DIR}/duplex_focus_driver.cpp"
    "${UR_CORE_DIR}/shared_mqtt_router.cpp"
    "${UR_CORE_DIR}/shared_device_connection.cpp"
    "${UR_CORE_DIR}/shared_handoff_focus.cpp"
  INCLUDE_DIRS "../../../include"
  PRIV_REQUIRES espressif__cjson esp_timer)
target_sources(${COMPONENT_LIB} PRIVATE "${UR_CORE_DIR}/udp_reply_receipt_state.hpp")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
# IDF在CMake标准标志之后追加自己的gnu++标准和降级告警，必须在组件末尾显式收紧。
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
  -Werror=unused-function -Werror=unused-variable -Werror=unused-but-set-variable
  -Werror=deprecated-declarations -Werror=extra -Werror=unused-parameter
  -Werror=sign-compare -Werror=enum-conversion
  -include "${CMAKE_CURRENT_LIST_DIR}/cpp17_guard.hpp")
