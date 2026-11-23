# 固定版IXWebSocket的接收锁和监听FD生命周期修补；只生成构建副本，不改变依赖源与pin。
# 接收调用未持传输层socket锁；监听FD关闭后未失效，重复stop/析构会误关后来复用的FD。
# 只替换构建目录中的源文件；原始依赖、许可证和其他代码不变，升级时必须重新核对匹配点。
set(ur_ix_source "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXWebSocketTransport.cpp")
file(READ "${ur_ix_source}" ur_ix_body)
set(ur_ix_before "            ssize_t ret = _socket->recv((char*) &_readbuf[0], _readbuf.size());")
set(ur_ix_after "            ssize_t ret = 0;\n            {\n                // Serialize nonblocking receive with transport send/close.\n                std::lock_guard<std::mutex> lock(_socketMutex);\n                ret = _socket->recv((char*) &_readbuf[0], _readbuf.size());\n            }")
string(FIND "${ur_ix_body}" "${ur_ix_before}" ur_ix_match)
if(ur_ix_match EQUAL -1)
  message(FATAL_ERROR "IXWebSocket接收锁修补点不匹配，必须重新审查依赖版本")
endif()
string(REPLACE "${ur_ix_before}" "${ur_ix_after}" ur_ix_patched "${ur_ix_body}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/IXWebSocketTransport.cpp" CONTENT "${ur_ix_patched}")
# 修补副本在生成阶段才存在；必须在第三方目标目录声明，避免冷配置按普通源码查找。
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/IXWebSocketTransport.cpp"
  TARGET_DIRECTORY ixwebsocket PROPERTIES GENERATED TRUE)
get_target_property(ur_ix_sources ixwebsocket SOURCES)
list(REMOVE_ITEM ur_ix_sources ixwebsocket/IXWebSocketTransport.cpp "${ur_ix_source}")
set_property(TARGET ixwebsocket PROPERTY SOURCES "${ur_ix_sources};${CMAKE_CURRENT_BINARY_DIR}/IXWebSocketTransport.cpp")
target_include_directories(ixwebsocket PRIVATE "${ixwebsocket_SOURCE_DIR}/ixwebsocket")

# 原固定版本恰好七处关闭监听FD；先退休原值，再关闭，保持原stop/join及错误返回顺序。
# 严格匹配数量，不允许依赖升级后把未知新路径当作已审核的七处安全修补。
set(ur_ix_server_source "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXSocketServer.cpp")
file(READ "${ur_ix_server_source}" ur_ix_server_body)
set(ur_ix_server_before "Socket::closeSocket(_serverFd);")
string(REGEX MATCHALL "Socket::closeSocket\\(_serverFd\\);" ur_ix_server_matches "${ur_ix_server_body}")
# CMake列表把匹配文本内的分号也当分隔符；只剔除由这些分号产生的空项，保留逐处严格计数。
list(FILTER ur_ix_server_matches EXCLUDE REGEX "^$")
list(LENGTH ur_ix_server_matches ur_ix_server_count)
if(NOT ur_ix_server_count EQUAL 7)
  message(FATAL_ERROR "IXWebSocket监听FD关闭修补点数量不匹配，必须重新审查依赖版本")
endif()
set(ur_ix_server_after "const auto urRetiredServerFd = _serverFd;\n        _serverFd = static_cast<socket_t>(-1);\n        Socket::closeSocket(urRetiredServerFd);")
string(REPLACE "${ur_ix_server_before}" "${ur_ix_server_after}" ur_ix_server_patched "${ur_ix_server_body}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/IXSocketServer.cpp" CONTENT "${ur_ix_server_patched}")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/IXSocketServer.cpp"
  TARGET_DIRECTORY ixwebsocket PROPERTIES GENERATED TRUE)
get_target_property(ur_ix_sources ixwebsocket SOURCES)
list(REMOVE_ITEM ur_ix_sources ixwebsocket/IXSocketServer.cpp "${ur_ix_server_source}")
set_property(TARGET ixwebsocket PROPERTY SOURCES "${ur_ix_sources};${CMAKE_CURRENT_BINARY_DIR}/IXSocketServer.cpp")
