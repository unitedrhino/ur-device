# Windows交付只安装正式桌面程序，测试目标与设备凭据不会进入安装目录。
if(NOT WIN32 OR NOT UR_DESKTOP_BROWSER OR NOT UR_DESKTOP_TRANSPORT)
  message(FATAL_ERROR "Windows安装包要求启用桌面浏览器与传输目标")
endif()
# 第三方库仅作为链接依赖；其SDK安装规则含配置期绝对路径，不能写入系统目录或混入桌面包。
set_property(DIRECTORY "${cjson_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
if(DEFINED googletest_SOURCE_DIR)
  set_property(DIRECTORY "${googletest_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)
# CRT由上面的官方再分发清单独立安装；不参与递归扫描，避免大小写路径与系统已装版本冲突。
install(TARGETS device_browser device_video_call device_voice_file
  RUNTIME_DEPENDENCIES
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
      "[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee]140.*" "[Mm][Ss][Vv][Cc][Pp]140.*" "[Cc][Oo][Nn][Cc][Rr][Tt]140.*"
    POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
    DIRECTORIES "$<TARGET_FILE_DIR:device_browser>"
  RUNTIME DESTINATION .)
install(FILES packaging/windows/launch-device.ps1 DESTINATION .)
install(FILES SOURCES.md LICENSE.watcher LICENSE.IXWebSocket DESTINATION licenses)
install(FILES "${cjson_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME LICENSE.cJSON)
if(VCPKG_INSTALLED_DIR)
  foreach(ur_license_port mosquitto opus openssl pthreads curl)
    install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${ur_license_port}/copyright"
      DESTINATION licenses RENAME "LICENSE.${ur_license_port}")
  endforeach()
endif()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME UR-Device-Simulator)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "UR-Device-Simulator-${PROJECT_VERSION}-windows-x64")
include(CPack)
