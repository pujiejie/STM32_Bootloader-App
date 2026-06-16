/**
 ******************************************************************************
 * @file    nm.c
 * @brief   轻量级 CAN 网络管理 — 裸机实现
 ******************************************************************************
 */

#include "nm.h"
#include "main.h"
#include "can.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
   全局状态
   ================================================================ */
volatile uint8_t nm_network_need = 1;   /* 上电默认需要网络 */
NM_State_t       nm_state = NM_STATE_INIT;
NM_NodeInfo_t    nm_nodes[NM_MAX_NODES];
uint8_t          nm_node_count = 0;

static uint32_t  nm_last_send_ms = 0;   /* 上次发送 NM 的时间 */
static uint8_t   nm_first_send = 1;      /* 是否首次发送 */

/* ─── 辅助: 查找/创建节点信息 ─── */
static NM_NodeInfo_t* nm_find_node(uint8_t node_id)
{
    for (uint8_t i = 0; i < nm_node_count; i++) {
        if (nm_nodes[i].node_id == node_id) return &nm_nodes[i];
    }
    if (nm_node_count < NM_MAX_NODES) {
        NM_NodeInfo_t *n = &nm_nodes[nm_node_count++];
        n->node_id = node_id;
        n->online = 0;
        n->repeat_req = 1;
        n->last_seen_ms = 0;
        return n;
    }
    return NULL;
}

/* ================================================================
   发送 NM PDU
   ================================================================ */
static void NM_Send_PDU(void)
{
    CAN_TxHeaderTypeDef tx_header;
    uint8_t tx_data[8] = {0};
    uint32_t tx_mailbox;

    tx_header.StdId = NM_CANID;
    tx_header.ExtId = 0;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;
    tx_header.DLC = 8;
    tx_header.TransmitGlobalTime = DISABLE;

    /* Byte 0: 控制字节 — Bit0 = Repeat Request */
    tx_data[0] = nm_network_need ? NM_CTRL_REPEAT_REQ : 0x00;
    /* Byte 1: 源节点 ID */
    tx_data[1] = NM_NODE_ID;
    /* Byte 2-7: 保留 */

    if (HAL_CAN_AddTxMessage(&hcan1, &tx_header, tx_data, &tx_mailbox) != HAL_OK) {
        printf("[NM] TX error (mailbox full?)\r\n");
        return;
    }

    nm_last_send_ms = HAL_GetTick();
    if (nm_first_send) {
        nm_first_send = 0;
        printf("[NM] Started. ID=0x%03lX Node=%d Cycle=%dms\r\n",
               NM_CANID, NM_NODE_ID, NM_CYCLE_MS);
    }
}

/* ================================================================
   接收处理: 从 CAN ISR 传入 NM 帧
   ================================================================ */
void NM_ReceiveHandler(uint32_t can_id, uint8_t *data)
{
    /* 只处理 NM ID 范围 (0x500 ~ 0x5FF) */
    if (can_id < NM_CANID_BASE || can_id >= NM_CANID_BASE + NM_MAX_NODES) return;

    uint8_t node_id = (uint8_t)(can_id - NM_CANID_BASE);
    if (node_id == NM_NODE_ID) return;  /* 忽略自己的 NM */

    NM_NodeInfo_t *node = nm_find_node(node_id);
    if (node == NULL) return;

    node->repeat_req = data[0] & NM_CTRL_REPEAT_REQ;
    node->last_seen_ms = HAL_GetTick();

    if (!node->online) {
        node->online = 1;
        printf("[NM] Node 0x%02X ONLINE (online nodes: ", node_id);
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < nm_node_count; i++)
            if (nm_nodes[i].online) cnt++;
        printf("%d)\r\n", cnt);
    }
}

/* ================================================================
   检查指定节点是否在线
   ================================================================ */
uint8_t NM_IsNodeOnline(uint8_t node_id)
{
    if (node_id == NM_NODE_ID) return 1;  /* 自己永远在线 */
    for (uint8_t i = 0; i < nm_node_count; i++) {
        if (nm_nodes[i].node_id == node_id) return nm_nodes[i].online;
    }
    return 0;
}

/* ================================================================
   检查是否所有节点都同意休眠
   ================================================================ */
uint8_t NM_AllNodesCanSleep(void)
{
    for (uint8_t i = 0; i < nm_node_count; i++) {
        if (nm_nodes[i].online && nm_nodes[i].repeat_req) {
            /* 有在线节点还需要网络 → 不能睡 */
            return 0;
        }
    }
    return 1;  /* 所有在线节点都同意休眠 */
}

/* ================================================================
   离线检测: 遍历所有节点, 检查超时
   ================================================================ */
static void NM_CheckTimeouts(void)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t i = 0; i < nm_node_count; i++) {
        if (nm_nodes[i].online &&
            (now - nm_nodes[i].last_seen_ms) >= NM_TIMEOUT_MS) {
            nm_nodes[i].online = 0;
            nm_nodes[i].repeat_req = 0;
            printf("[NM] Node 0x%02X OFFLINE\r\n", nm_nodes[i].node_id);
        }
    }
}

/* ================================================================
   设置网络需求状态
   ================================================================ */
void NM_SetNetworkRequest(uint8_t need_network)
{
    nm_network_need = need_network ? 1 : 0;
}

/* ================================================================
   初始化
   ================================================================ */
void NM_Init(void)
{
    memset(nm_nodes, 0, sizeof(nm_nodes));
    nm_node_count = 0;
    nm_state = NM_STATE_INIT;
    nm_network_need = 1;
    nm_first_send = 1;
    printf("[NM] Initialized. NodeID=0x%02X CANID=0x%03lX\r\n", NM_NODE_ID, NM_CANID);
}

/* ================================================================
   周期性处理 (每主循环调用)
   ================================================================ */
void NM_Process(void)
{
    uint32_t now = HAL_GetTick();

    /* ─── 离线检测 ─── */
    NM_CheckTimeouts();

    /* ─── 状态机 ─── */
    switch (nm_state) {

    case NM_STATE_INIT:
        /* 立即发送首个 NM，进入 ACTIVE */
        NM_Send_PDU();
        nm_state = NM_STATE_ACTIVE;
        break;

    case NM_STATE_ACTIVE:
        /* 周期性发送 NM PDU */
        if ((now - nm_last_send_ms) >= NM_CYCLE_MS) {
            NM_Send_PDU();
        }
        break;

    case NM_STATE_PASSIVE:
        /* 休眠中, 收到任何 NM 帧时会由 NM_ReceiveHandler 触发切换
           这里不做任何事 */
        break;
    }
}
