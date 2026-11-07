#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "netcheck_policy.h"

/* ============================================================================
 * DEF-NET-001 联网验证(netcheck)。
 *
 * 回答一个问题:"当前这条 Wi-Fi 链路,能不能算可用?" —— 拿到 IP 只是开始:
 * 必须 TCP 可达 broker(互联网/出口)且当前链路对应的 esp-mqtt 会话已回
 * CONNACK,才算 OK。结果绑定链路代次(见 wifi_runtime_t.link_gen):
 * 断链/重新拿到 IP 都换代,旧代次的结论一律不作数。
 *
 * 时序预算(评审 P1-1 + 核验 P3-1 修订):
 *   总预算 25s = DNS ≤3s(独立子预算,异步可取消) + TCP ≤2×2.5s
 *   + MQTT 等待占其余(含 4s 宽限);app_netcheck_wait 调用方超时传 25s;
 *   accepted_ttl=45s 覆盖 15s 连接 + 25s 验证。取消收敛 ≤3s
 *  (最长的有界步是单次 TCP select 2.5s)。
 *
 * P2-3(MQTT 拉起):阶段 2 先给 4s 宽限(boot/换网路径 mqtt.c 自己的
 * GOT_IP 处理器已在拉起),宽限内仍不在线才 app_mqtt_poke_restart()
 * (退避归零+schedule)—— 高退避档 20/30s 卡住时光 schedule 没用。
 * ========================================================================== */

/* 验证结果(任务书 3.1/3.2):不把 CANCELLED/TIMEOUT/GEN_MISMATCH 折叠成 bool */
typedef struct {
    app_netcheck_outcome_t outcome;
    uint32_t gen;   /* 产生该结果的任务代次(与等待者比对即 GEN_MISMATCH 判据) */
    char stage[12]; /* 失败/成功阶段,词汇同 net_verify_stage;TIMEOUT/GEN_MISMATCH 恒 "none" */
} app_netcheck_result_t;

/* 启动一次验证(单飞)。同一代次重复 start 是幂等 no-op;
 * 有在跑的旧代次时记录为 pending,旧代次收尾(或被取代)后自动接续,
 * 绝不让事件处理器阻塞等待。 */
esp_err_t app_netcheck_start(uint32_t link_gen);

/* 有界等待指定代次的判决,完整结果写 *result_out(result_out 可为 NULL);
 * true 仅当 outcome == APP_NETCHECK_OUT_OK。
 * TIMEOUT/GEN_MISMATCH/CANCELLED 均为独立结果,不伪装成败。 */
bool app_netcheck_wait(uint32_t link_gen, uint32_t timeout_ms,
                       app_netcheck_result_t *result_out);

/* 请求取消指定代次在跑的验证(任务书 3.4:取消绑定 generation —— 旧事务
 * 清理传入其验证代次,绝不误杀换代后的新一轮)。≤3s 收敛。 */
void app_netcheck_cancel(uint32_t link_gen);

/* 指定代次是否已有 OK 结论(apply 提交前复核用)。 */
bool app_netcheck_ok_for_gen(uint32_t gen);

/* 指定代次是否有 netcheck 在飞(选 B 条件 1:推迟拆链只挂这个前提)。 */
bool app_netcheck_running_for_gen(uint32_t gen);

/* 选 B 条件 4:读取按代次归档的**不可变终局快照**(B-条件 4)。
 * 快照在 netcheck 任务收尾时写入,只读、直到下一次收尾才被覆盖;
 * true = 存在且 gen 匹配(outcome/stage 为该代次的最终判决);
 * false = 该代次无终局(被取消且未收尾/从未运行)——worker 不得读 live 状态。 */
bool app_netcheck_read_done(uint32_t gen, app_netcheck_result_t *out);

app_netcheck_state_t app_netcheck_state(void);

/* 最近一次收尾的阶段名(拷贝出参,锁内快照):"none"/"internet"/"mqtt"/"skipped"。 */
void app_netcheck_last_stage(char *out, size_t out_size);

/* F-1:最近一次 netcheck 的 TCP 探测失败次数与最后 errno(只读;
 * start 清零;network.status 以 nc_tcp_fails_last/nc_tcp_last_errno 暴露) */
void app_netcheck_tcp_stats(uint32_t *fails_out, int *last_errno_out);

/* outcome → 阶段名(同一词汇表):FAIL_INTERNET→"internet"、FAIL_MQTT→"mqtt"、
 * 其余(CANCELLED/TIMEOUT/GEN_MISMATCH)→"none"。apply 拒绝细分用。 */
const char *app_netcheck_outcome_stage(app_netcheck_outcome_t o);

/* 完成观察者:netcheck 任务上下文回调,不得长阻塞。
 * 仅在收尾为 OK/FAIL_* 且未被新代次取代时调用;CANCELLED 不回调。
 * wifi 用它把"验证通过 → 清 mask/退避归零"落到运行态(评审修订:
 * GOT_IP 不再等于可用,成功奖励也移到验证通过之后)。 */
void app_netcheck_set_observer(void (*fn)(uint32_t gen, bool ok, const char *stage));
