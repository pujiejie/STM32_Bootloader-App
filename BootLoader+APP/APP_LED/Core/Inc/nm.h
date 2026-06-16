/**
 ******************************************************************************
 * @file    nm.h
 * @brief   轻量级 CAN 网络管理 (NM) — 裸机实现
 *
 *         NM 帧 ID = 0x500 + NodeID (每个 ECU 唯一)
 *         NM 帧周期 = 200ms
 *         节点离线超时 = 5 个周期 (1000ms)
 *
 *         NM PDU 格式 (8 字节):
 *           Byte 0: Control Byte
 *                   Bit0 = Repeat Request (1=需要网络, 0=可以休眠)
 *                   Bit1-7 = 保留
 *           Byte 1: Source Node ID
 *           Byte 2-7: 保留 (0x00)
 ******************************************************************************
 */

#ifndef __NM_H
#define __NM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ================================================================
   配置
   ================================================================ */
#define NM_NODE_ID              0x01    /* BMS 节点 ID */
#define NM_CANID_BASE           0x500   /* NM 帧 ID 基址 */
#define NM_CANID                (NM_CANID_BASE + NM_NODE_ID)
#define NM_CYCLE_MS             200     /* NM 帧发送周期 (ms) */
#define NM_TIMEOUT_CYCLES       5       /* 离线检测: 几个周期没收到 */
#define NM_TIMEOUT_MS           (NM_CYCLE_MS * NM_TIMEOUT_CYCLES)
#define NM_MAX_NODES            16      /* 最多监控的节点数 */

/* ─── NM 控制字节位定义 ─── */
#define NM_CTRL_REPEAT_REQ      0x01    /* Bit0: Repeat Request */

/* ─── NM 状态 ─── */
typedef enum {
    NM_STATE_INIT = 0,      /* 初始化, 尚未发送 NM */
    NM_STATE_ACTIVE,        /* 网络活跃, 周期性发送 NM */
    NM_STATE_PASSIVE        /* 被动模式, 不发送 NM (休眠中) */
} NM_State_t;

/* ─── 被监控的节点信息 ─── */
typedef struct {
    uint8_t  node_id;       /* 节点 ID */
    uint8_t  online;        /* 是否在线 */
    uint8_t  repeat_req;    /* 该节点是否需要网络 */
    uint32_t last_seen_ms;  /* 上次收到该节点 NM 的时间 */
} NM_NodeInfo_t;

/* ================================================================
   全局状态
   ================================================================ */
extern volatile uint8_t nm_network_need;  /* BMS 是否需要网络 (Bit0 状态) */
extern NM_State_t nm_state;               /* 当前 NM 状态 */
extern NM_NodeInfo_t nm_nodes[NM_MAX_NODES];
extern uint8_t nm_node_count;

/* ================================================================
   API
   ================================================================ */
void NM_Init(void);
void NM_Process(void);
void NM_SetNetworkRequest(uint8_t need_network);  /* 1=需要网络, 0=可以休眠 */
void NM_ReceiveHandler(uint32_t can_id, uint8_t *data);
uint8_t NM_IsNodeOnline(uint8_t node_id);
uint8_t NM_AllNodesCanSleep(void);  /* 所有节点都同意休眠? */

#ifdef __cplusplus
}
#endif

#endif /* __NM_H */
