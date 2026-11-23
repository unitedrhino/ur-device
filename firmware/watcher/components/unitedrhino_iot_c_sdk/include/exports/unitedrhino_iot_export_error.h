/*
 * Tencent is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2016 Tencent. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

/*
 * UnitedRhino is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2016 UnitedRhino. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

#ifndef UNITEDRHINO_IOT_EXPORT_ERROR_H_
#define UNITEDRHINO_IOT_EXPORT_ERROR_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IOT SDK return/error code
 * Enumeration of return code in UnitedRhino IoT C-SDK.
 * Values less than 0 are specific error codes
 * Value of 0 is successful return
 * Values greater than 0 are specific non-error return codes
 */
typedef enum {
    UNITEDRHINO_RET_MQTT_ALREADY_CONNECTED           = 4,  // Already connected with MQTT server
    UNITEDRHINO_RET_MQTT_CONNACK_CONNECTION_ACCEPTED = 3,  // MQTT connection accepted by server
    UNITEDRHINO_RET_MQTT_MANUALLY_DISCONNECTED       = 2,  // Manually disconnected with MQTT server
    UNITEDRHINO_RET_MQTT_RECONNECTED                 = 1,  // Reconnected with MQTT server successfully

    UNITEDRHINO_RET_SUCCESS = 0,  // Successful return

    UNITEDRHINO_ERR_FAILURE  = -1001,  // Generic failure return
    UNITEDRHINO_ERR_INVAL    = -1002,  // Invalid parameter
    UNITEDRHINO_ERR_DEV_INFO = -1003,  // Fail to get device info
    UNITEDRHINO_ERR_MALLOC   = -1004,  // Fail to malloc memory

    UNITEDRHINO_ERR_HTTP_CLOSED         = -3,   // HTTP server close the connection
    UNITEDRHINO_ERR_HTTP                = -4,   // HTTP unknown error
    UNITEDRHINO_ERR_HTTP_PRTCL          = -5,   // HTTP protocol error
    UNITEDRHINO_ERR_HTTP_UNRESOLVED_DNS = -6,   // HTTP DNS resolve failed
    UNITEDRHINO_ERR_HTTP_PARSE          = -7,   // HTTP URL parse failed
    UNITEDRHINO_ERR_HTTP_CONN           = -8,   // HTTP connect failed
    UNITEDRHINO_ERR_HTTP_AUTH           = -9,   // HTTP auth failed
    UNITEDRHINO_ERR_HTTP_NOT_FOUND      = -10,  // HTTP 404
    UNITEDRHINO_ERR_HTTP_TIMEOUT        = -11,  // HTTP timeout

    UNITEDRHINO_ERR_MQTT_PUSH_TO_LIST_FAILED                   = -102,  // Fail to push node to MQTT waiting list
    UNITEDRHINO_ERR_MQTT_NO_CONN                               = -103,  // Not connected with MQTT server
    UNITEDRHINO_ERR_MQTT_UNKNOWN                               = -104,  // MQTT unknown error
    UNITEDRHINO_ERR_MQTT_ATTEMPTING_RECONNECT                  = -105,  // Reconnecting with MQTT server
    UNITEDRHINO_ERR_MQTT_RECONNECT_TIMEOUT                     = -106,  // MQTT reconnect timeout
    UNITEDRHINO_ERR_MQTT_MAX_SUBSCRIPTIONS                     = -107,  // MQTT topic subscription out of range
    UNITEDRHINO_ERR_MQTT_SUB                                   = -108,  // MQTT topic subscription fail
    UNITEDRHINO_ERR_MQTT_NOTHING_TO_READ                       = -109,  // MQTT nothing to read
    UNITEDRHINO_ERR_MQTT_PACKET_READ                           = -110,  // Something wrong when reading MQTT packet
    UNITEDRHINO_ERR_MQTT_REQUEST_TIMEOUT                       = -111,  // MQTT request timeout
    UNITEDRHINO_ERR_MQTT_CONNACK_UNKNOWN                       = -112,  // MQTT connection refused: unknown error
    UNITEDRHINO_ERR_MQTT_CONNACK_UNACCEPTABLE_PROTOCOL_VERSION = -113,  // MQTT connection refused: protocol version invalid
    UNITEDRHINO_ERR_MQTT_CONNACK_IDENTIFIER_REJECTED           = -114,  // MQTT connection refused: identifier rejected
    UNITEDRHINO_ERR_MQTT_CONNACK_SERVER_UNAVAILABLE            = -115,  // MQTT connection refused: service not available
    UNITEDRHINO_ERR_MQTT_CONNACK_BAD_USERDATA                  = -116,  // MQTT connection refused: bad user name or password
    UNITEDRHINO_ERR_MQTT_CONNACK_NOT_AUTHORIZED                = -117,  // MQTT connection refused: not authorized
    UNITEDRHINO_ERR_RX_MESSAGE_INVAL                           = -118,  // MQTT received invalid msg
    UNITEDRHINO_ERR_BUF_TOO_SHORT                              = -119,  // MQTT recv buffer not enough
    UNITEDRHINO_ERR_MQTT_QOS_NOT_SUPPORT                       = -120,  // MQTT QoS level not supported
    UNITEDRHINO_ERR_MQTT_UNSUB_FAIL                            = -121,  // MQTT unsubscribe failed
    UNITEDRHINO_ERR_MQTT_RECONNECTING                          = -122,  // MQTT reconnecting
    /* MQTT 读包字节流已错位/不可恢复：包边界不再可信，必须断开重连复位，不能按瞬态重试，
     * 否则会一直在错误偏移上解析出假包（真机表现为假 PUBREL 与巨大长度字段）。 */
    UNITEDRHINO_ERR_MQTT_RX_STREAM_DESYNC                      = -123,  // MQTT rx stream desynced

    UNITEDRHINO_ERR_JSON_PARSE            = -132,  // JSON parsing error
    UNITEDRHINO_ERR_JSON_BUFFER_TRUNCATED = -133,  // JSON buffer truncated
    UNITEDRHINO_ERR_JSON_BUFFER_TOO_SMALL = -134,  // JSON parsing buffer not enough
    UNITEDRHINO_ERR_JSON                  = -135,  // JSON generation error
    UNITEDRHINO_ERR_MAX_JSON_TOKEN        = -136,  // JSON token out of range

    UNITEDRHINO_ERR_MAX_APPENDING_REQUEST = -137,  // appending request out of range
    UNITEDRHINO_ERR_MAX_TOPIC_LENGTH      = -138,  // Topic length oversize

    UNITEDRHINO_ERR_COAP_NULL              = -150,  // COAP null pointer
    UNITEDRHINO_ERR_COAP_DATA_SIZE         = -151,  // COAP data size out of range
    UNITEDRHINO_ERR_COAP_INTERNAL          = -152,  // COAP interval error
    UNITEDRHINO_ERR_COAP_BADMSG            = -153,  // COAP bad msg
    UNITEDRHINO_ERR_DTLS_PEER_CLOSE_NOTIFY = -160,  // DTLS connection is closed

    UNITEDRHINO_ERR_PROPERTY_EXIST     = -201,  // property already exist
    UNITEDRHINO_ERR_NOT_PROPERTY_EXIST = -202,  // property not exist
    UNITEDRHINO_ERR_REPORT_TIMEOUT     = -203,  // update timeout
    UNITEDRHINO_ERR_REPORT_REJECTED    = -204,  // update rejected by server
    UNITEDRHINO_ERR_GET_TIMEOUT        = -205,  // get timeout
    UNITEDRHINO_ERR_GET_REJECTED       = -206,  // get rejected by server

    UNITEDRHINO_ERR_ACTION_EXIST     = -210,  // acion already exist
    UNITEDRHINO_ERR_NOT_ACTION_EXIST = -211,  // acion not exist

    UNITEDRHINO_ERR_GATEWAY_CREATE_SESSION_FAIL = -221,  // Gateway fail to create sub-device session
    UNITEDRHINO_ERR_GATEWAY_SESSION_NO_EXIST    = -222,  // Gateway sub-device session not exist
    UNITEDRHINO_ERR_GATEWAY_SESSION_TIMEOUT     = -223,  // Gateway sub-device session timeout
    UNITEDRHINO_ERR_GATEWAY_SUBDEV_ONLINE       = -224,  // Gateway sub-device online
    UNITEDRHINO_ERR_GATEWAY_SUBDEV_OFFLINE      = -225,  // Gateway sub-device offline

    UNITEDRHINO_ERR_TCP_SOCKET_FAILED   = -601,  // TLS TCP socket connect fail
    UNITEDRHINO_ERR_TCP_UNKNOWN_HOST    = -602,  // TCP unknown host (DNS fail)
    UNITEDRHINO_ERR_TCP_CONNECT         = -603,  // TCP/UDP socket connect fail
    UNITEDRHINO_ERR_TCP_READ_TIMEOUT    = -604,  // TCP read timeout
    UNITEDRHINO_ERR_TCP_WRITE_TIMEOUT   = -605,  // TCP write timeout
    UNITEDRHINO_ERR_TCP_READ_FAIL       = -606,  // TCP read error
    UNITEDRHINO_ERR_TCP_WRITE_FAIL      = -607,  // TCP write error
    UNITEDRHINO_ERR_TCP_PEER_SHUTDOWN   = -608,  // TCP server close connection
    UNITEDRHINO_ERR_TCP_NOTHING_TO_READ = -609,  // TCP socket nothing to read

    UNITEDRHINO_ERR_SSL_INIT            = -701,  // TLS/SSL init fail
    UNITEDRHINO_ERR_SSL_CERT            = -702,  // TLS/SSL certificate issue
    UNITEDRHINO_ERR_SSL_CONNECT         = -703,  // TLS/SSL connect fail
    UNITEDRHINO_ERR_SSL_CONNECT_TIMEOUT = -704,  // TLS/SSL connect timeout
    UNITEDRHINO_ERR_SSL_WRITE_TIMEOUT   = -705,  // TLS/SSL write timeout
    UNITEDRHINO_ERR_SSL_WRITE           = -706,  // TLS/SSL write error
    UNITEDRHINO_ERR_SSL_READ_TIMEOUT    = -707,  // TLS/SSL read timeout
    UNITEDRHINO_ERR_SSL_READ            = -708,  // TLS/SSL read error
    UNITEDRHINO_ERR_SSL_NOTHING_TO_READ = -709,  // TLS/SSL nothing to read

    UNITEDRHINO_ERR_BIND_PARA_ERR        = -801,  // bind sub device param error
    UNITEDRHINO_ERR_BIND_SUBDEV_ERR      = -802,  // sub device not exist or illegal
    UNITEDRHINO_ERR_BIND_SIGN_ERR        = -803,  // signature check err
    UNITEDRHINO_ERR_BIND_SIGN_METHOD_RRR = -804,  // signmethod not supporte
    UNITEDRHINO_ERR_BIND_SIGN_EXPIRED    = -805,  // signature expired
    UNITEDRHINO_ERR_BIND_BEEN_BINDED     = -806,  // sub device has been binded by other gateway
    UNITEDRHINO_ERR_BIND_SUBDEV_FORBID   = -807,  // sub device not allow to bind
    UNITEDRHINO_ERR_BIND_OP_FORBID       = -808,  // operation not permit
    UNITEDRHINO_ERR_BIND_REPEATED_REQ    = -809,  // repeated bind request,has been binded
} IoT_Return_Code;

#ifdef __cplusplus
}
#endif

#endif /* UNITEDRHINO_IOT_EXPORT_ERROR_H_ */
