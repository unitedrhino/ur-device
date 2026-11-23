/* Watcher 保留绑定换网桥接：C 模块只操作有界凭据副本和非敏感状态。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 启动模式：普通联网、独占 BLE、独占候选验证、存储故障。 */
typedef enum { UR_NETWORK_NORMAL, UR_NETWORK_BLE, UR_NETWORK_TRIAL, UR_NETWORK_HALT, UR_NETWORK_BIND } ur_network_boot_t;
/* 对应运行协调结果，禁止 MQTT 层凭数字猜测是否可以重启。 */
enum { UR_NETWORK_IGNORED, UR_NETWORK_CONTINUE, UR_NETWORK_COMMITTED,
       UR_NETWORK_ROLLBACK, UR_NETWORK_FAULT };
/* 对应持久化错误码；鉴权错误只终止换网，不清设备密钥。 */
enum { UR_NETWORK_ERROR_NONE, UR_NETWORK_ERROR_RESTART, UR_NETWORK_ERROR_TIMEOUT,
       UR_NETWORK_ERROR_CANCELLED, UR_NETWORK_ERROR_AUTH, UR_NETWORK_ERROR_NETWORK,
       UR_NETWORK_ERROR_STORAGE };
/* 当前事务快照，不含 SSID、密码或设备密钥。阶段数字与持久化合同一致。 */
typedef struct {
    char request_id[17]; /* 本次换网关联 ID。 */
    uint8_t stage; /* 0 空闲，1..6 活动，7 成功，8 失败，9 等待物理确认。 */
    uint8_t error; /* 非敏感 NetworkError 编码。 */
    bool blocked; /* 存储故障时禁止普通联网及凭据自愈。 */
} ur_network_status_t;

/* 初始化必须先于正常网络管理器；只消费本模块授权的软件重启标记。 */
ur_network_boot_t ur_network_boot(void);
/* 正常取得 IP 后记录已保存网络的最近顺序；失败不修改凭据或触发重新绑定。 */
bool ur_network_remember_connection(const char *ssid);
/* 复制最多十个已保存 SSID 的最近顺序，不含密码；失败返回 0，调用方保留信号排序。 */
uint8_t ur_network_copy_wifi_order(char ssids[10][33]);
/* 物理确认且资源互斥通过后持久化事务；成功仍需调用受控重启。 */
bool ur_network_begin(const char *request_id);
/* 云端请求仅预留物理确认，不进入换网；重复同一待确认请求返回 200，不延长窗口。 */
int ur_network_prepare_request(const char *request_id);
/* actionReply 发布成功后才允许展示确认；返回 false 表示重复或已失效，不重复弹窗。 */
bool ur_network_activate_request(const char *request_id);
/* 取消未确认请求，仅更新非敏感状态，不修改 Wi-Fi 或密钥。 */
void ur_network_cancel_request(const char *request_id, uint8_t error);
/* 本模块受控重启，保存跨软件复位的一次性标记；不用于普通重启或 OTA。 */
void ur_network_restart(void);
/* 配网绑定仅延续下一次受控软件启动；冷启动仍按事务规则恢复旧网络。 */
void ur_network_restart_binding(void);
/* 复制/受理候选，固定 33/65 字节缓冲，仅供独占 STA 使用。 */
bool ur_network_candidate(char ssid[33], char password[65]);
bool ur_network_accept(const char *ssid, const char *password);
/* 获取 IP 仅推进验证启动，不代表换网成功。 */
bool ur_network_got_ip(void);
/* 候选取得 IP 后可靠保存正式列表；仅 COMMITTED 允许反馈 BLE 最终成功。 */
int ur_network_commit_connected(const char *connected_ssid);
/* 活动或存储故障均禁止动态注册、清密钥及并行 OTA/相机。 */
bool ur_network_protect_credentials(void);
/* MQTT 发布成功后登记 token，断线只更新原子连接代次，不在回调写 NVS。 */
void ur_network_report_sent(const char *token);
void ur_network_disconnected(void);
/* 匹配回执才提交；返回 2 已成功、3 需恢复启动、4 故障，其余不需重启。 */
int ur_network_report_reply(const char *token, int code, bool connected);
/* 独立 worker 定时调用，处理超时；失败码与返回值同上。 */
int ur_network_tick(void);
int ur_network_fail(uint8_t error);
/* 线程安全读取非敏感状态。 */
void ur_network_status(ur_network_status_t *status);
#ifdef __cplusplus
}
#endif
