/**
  ******************************************************************************
  * @file    doip.c
  * @brief   ISO 13400 (DoIP) 裸机实现
  *          TCP raw API + UDS 转发
  ******************************************************************************
  */

#include "doip.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "main.h"
#include "uds.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
   全局状态
   ================================================================ */
volatile uint8_t  g_doip_routing_active = 0;
volatile uint8_t  g_doip_logical_addr[2] = {0x0E, 0x80};

static struct tcp_pcb *g_doip_listen_pcb = NULL;
static struct tcp_pcb *g_doip_client_pcb = NULL;
static struct udp_pcb *g_doip_udp_pcb    = NULL;

/* 接收缓冲: 最大 DOIP 消息 64KB, 实际用 8KB */
#define DOIP_RX_BUF_SIZE    8192
static uint8_t  doip_rx_buf[DOIP_RX_BUF_SIZE];
static uint16_t doip_rx_len = 0;
static uint8_t  doip_rx_complete = 0;

/* UDS 响应缓冲 */
#define DOIP_TX_BUF_SIZE   4096
static uint8_t  doip_tx_buf[DOIP_TX_BUF_SIZE];
static uint16_t doip_tx_len = 0;
static uint8_t  doip_tx_pending = 0;

/* 客户端源地址 */
static ip_addr_t doip_client_ip;

/* ================================================================
   辅助: 大端读写
   ================================================================ */
static inline uint16_t be16(uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
static inline uint32_t be32(uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}
static inline void set_be16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}
static inline void set_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF; p[3] = v & 0xFF;
}

/* ================================================================
   发送 DOIP 报文 (带 header 封装)
   ================================================================ */
static err_t doip_tcp_send(uint16_t payload_type, uint8_t *payload, uint32_t len)
{
    if (g_doip_client_pcb == NULL) return ERR_CONN;

    doip_hdr_t hdr;
    hdr.version     = DOIP_PROTOCOL_VERSION;
    hdr.version_inv = (uint8_t)(~DOIP_PROTOCOL_VERSION);
    hdr.payload_type = payload_type;
    hdr.payload_len  = len;

    /* 先发 header */
    uint8_t hdr_buf[8];
    hdr_buf[0] = hdr.version;
    hdr_buf[1] = hdr.version_inv;
    set_be16(&hdr_buf[2], hdr.payload_type);
    set_be32(&hdr_buf[4], hdr.payload_len);

    err_t err = tcp_write(g_doip_client_pcb, hdr_buf, 8, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("[DOIP] tcp_write hdr err=%d\r\n", err);
        return err;
    }

    /* 再发 payload */
    if (len > 0 && payload != NULL) {
        err = tcp_write(g_doip_client_pcb, payload, len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            printf("[DOIP] tcp_write payload err=%d\r\n", err);
            return err;
        }
    }

    err = tcp_output(g_doip_client_pcb);
    return err;
}

/* ================================================================
   车辆识别响应 (VIN + EID + GID + Logical Address)
   ================================================================ */
static void doip_vehicle_ident_response(void)
{
    uint8_t payload[33];
    uint8_t *p = payload;

    /* VIN (17 bytes) */
    memcpy(p, DOIP_VIN, 17); p += 17;
    /* Logical Address (2 bytes) */
    p[0] = g_doip_logical_addr[0]; p[1] = g_doip_logical_addr[1]; p += 2;
    /* EID (6 bytes) */
    for (int i = 5; i >= 0; i--) *p++ = (uint8_t)(DOIP_EID >> (i * 8));
    /* GID (6 bytes) */
    for (int i = 5; i >= 0; i--) *p++ = (uint8_t)(DOIP_GID >> (i * 8));
    /* Further action required (1 byte) */
    *p++ = 0x00;  /* 0x00 = no further action */
    /* VIN/GID sync status (1 byte) */
    *p++ = 0x00;

    doip_tcp_send(DOIP_VEHICLE_ANNOUNCE, payload, p - payload);
    printf("[DOIP] Vehicle ident sent: VIN=%s\r\n", DOIP_VIN);
}

/* ================================================================
   路由激活处理
   ================================================================ */
static void doip_routing_activation(uint8_t *payload, uint32_t len)
{
    uint8_t resp[9];
    uint16_t client_la;

    if (len < 7) {
        client_la = 0;
    } else {
        client_la = be16(&payload[0]);
    }

    /* 检查激活类型 (默认 0x00) */
    uint8_t act_type = (len >= 8) ? payload[7] : 0x00;

    printf("[DOIP] Routing activation: src=0x%04X type=%d\r\n", client_la, act_type);

    /* 响应 */
    resp[0] = g_doip_logical_addr[0];
    resp[1] = g_doip_logical_addr[1];
    resp[2] = (uint8_t)(client_la >> 8);
    resp[3] = (uint8_t)(client_la & 0xFF);
    resp[4] = DOIP_ROUTING_SUCCESS;
    resp[5] = 0x00; /* reserved ISO 13400 */
    resp[6] = 0x00; /* OEM specific, unused */
    resp[7] = 0x00;
    resp[8] = 0x00;

    doip_tcp_send(DOIP_ROUTING_RSP, resp, 9);

    g_doip_routing_active = 1;
    printf("[DOIP] Routing ACTIVATED (src=0x%04X)\r\n", client_la);
}

/* ================================================================
   诊断消息处理: 解包 UDS → 调 Process_UDS_Request
   ================================================================ */
static void doip_diagnostic_handler(uint8_t *payload, uint32_t len)
{
    if (!g_doip_routing_active) {
        uint8_t nack = DOIP_DIAG_NACK_NO_ROUTING;
        doip_tcp_send(DOIP_DIAG_NACK, &nack, 1);
        printf("[DOIP] Diag rejected: routing not active\r\n");
        return;
    }

    if (len < 2) {
        uint8_t nack = DOIP_DIAG_NACK_INVALID_LEN;
        doip_tcp_send(DOIP_DIAG_NACK, &nack, 1);
        return;
    }

    /* SA + TA (4 bytes) + UDS payload */
    uint16_t sa = be16(&payload[0]);
    uint16_t ta = be16(&payload[2]);
    uint8_t  *uds_data = &payload[4];
    uint16_t uds_len   = (uint16_t)(len - 4);

    printf("[DOIP] Diag: SA=0x%04X TA=0x%04X len=%u\r\n", sa, ta, uds_len);

    /* 转发给现有 UDS 处理引擎 (会话由 UDS 0x10 服务管理) */
    Process_UDS_Request(uds_data, uds_len);
}

/* ================================================================
   心跳响应
   ================================================================ */
static void doip_alive_check_response(void)
{
    uint8_t sa[2];
    sa[0] = g_doip_logical_addr[0];
    sa[1] = g_doip_logical_addr[1];
    doip_tcp_send(DOIP_ALIVE_CHECK_RSP, sa, 2);
}

/* ================================================================
   DOIP 消息分发
   ================================================================ */
static void doip_dispatch(uint8_t *payload, uint32_t payload_len, uint16_t payload_type)
{
    switch (payload_type) {
    case DOIP_VEHICLE_IDENT_REQ:
    case DOIP_VEHICLE_IDENT_EID:
    case DOIP_VEHICLE_IDENT_VIN:
        doip_vehicle_ident_response();
        break;

    case DOIP_ROUTING_REQ:
        doip_routing_activation(payload, payload_len);
        break;

    case DOIP_ALIVE_CHECK_REQ:
        doip_alive_check_response();
        break;

    case DOIP_DIAG_MSG:
        doip_diagnostic_handler(payload, payload_len);
        break;

    default:
        printf("[DOIP] Unknown payload type: 0x%04X\r\n", payload_type);
        break;
    }
}

/* ================================================================
   TCP 接收回调 (raw API, 在 ethernetif_input 上下文中调用)
   ================================================================ */
static err_t doip_recv_callback(void *arg, struct tcp_pcb *pcb,
                                 struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* 对端关闭连接 */
        printf("[DOIP] Client disconnected\r\n");
        g_doip_routing_active = 0;
        g_doip_client_pcb = NULL;
        tcp_close(pcb);
        tcp_arg(pcb, NULL);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    /* 把收到的数据追加到缓冲区 */
    struct pbuf *q = p;
    while (q != NULL) {
        uint16_t copy_len = q->len;
        if (doip_rx_len + copy_len > DOIP_RX_BUF_SIZE) {
            printf("[DOIP] RX overflow!\r\n");
            doip_rx_len = 0;
            pbuf_free(p);
            return ERR_MEM;
        }
        memcpy(&doip_rx_buf[doip_rx_len], q->payload, copy_len);
        doip_rx_len += copy_len;
        q = q->next;
    }
    pbuf_free(p);

    /* 尝试解析 DoIP 报文 */
    while (doip_rx_len >= DOIP_HEADER_SIZE) {
        uint16_t ptype  = be16(&doip_rx_buf[2]);
        uint32_t plen   = be32(&doip_rx_buf[4]);
        uint32_t total  = DOIP_HEADER_SIZE + plen;

        if (plen > DOIP_RX_BUF_SIZE - DOIP_HEADER_SIZE) {
            printf("[DOIP] Payload too large: %lu\r\n", plen);
            doip_rx_len = 0;
            return ERR_OK;
        }

        if (doip_rx_len < total) {
            /* 数据不完整, 等下一包 */
            break;
        }

        /* 完整报文已收到 → 分发 */
        doip_dispatch(&doip_rx_buf[DOIP_HEADER_SIZE], plen, ptype);

        /* 从缓冲区移除已处理数据 */
        uint32_t remaining = doip_rx_len - total;
        if (remaining > 0) {
            memmove(doip_rx_buf, &doip_rx_buf[total], remaining);
        }
        doip_rx_len = (uint16_t)remaining;
    }

    return ERR_OK;
}

/* ================================================================
   TCP Accept 回调
   ================================================================ */
static err_t doip_accept_callback(void *arg, struct tcp_pcb *new_pcb, err_t err)
{
    if (err != ERR_OK || new_pcb == NULL) return ERR_ARG;

    if (g_doip_client_pcb != NULL) {
        /* 已有客户端, 拒绝新连接 */
        printf("[DOIP] Reject: already connected\r\n");
        tcp_abort(new_pcb);
        return ERR_ABRT;
    }

    g_doip_client_pcb = new_pcb;
    ip_addr_copy(doip_client_ip, new_pcb->remote_ip);

    tcp_arg(new_pcb, NULL);
    tcp_recv(new_pcb, doip_recv_callback);
    tcp_err(new_pcb, NULL);
    tcp_poll(new_pcb, NULL, 0);

    printf("[DOIP] Client connected: %d.%d.%d.%d\r\n",
           ip4_addr1(&new_pcb->remote_ip), ip4_addr2(&new_pcb->remote_ip),
           ip4_addr3(&new_pcb->remote_ip), ip4_addr4(&new_pcb->remote_ip));

    return ERR_OK;
}

/* ================================================================
   UDP 车辆公告
   ================================================================ */
void DOIP_Announce_UDP(void)
{
    if (g_doip_udp_pcb == NULL) return;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 33, PBUF_RAM);
    if (p == NULL) return;

    uint8_t *pl = (uint8_t *)p->payload;
    uint8_t *pp = pl;
    memcpy(pp, DOIP_VIN, 17); pp += 17;
    *pp++ = g_doip_logical_addr[0]; *pp++ = g_doip_logical_addr[1];
    for (int i = 5; i >= 0; i--) *pp++ = (uint8_t)(DOIP_EID >> (i * 8));
    for (int i = 5; i >= 0; i--) *pp++ = (uint8_t)(DOIP_GID >> (i * 8));
    *pp++ = 0x00;

    p->len = pp - pl;
    p->tot_len = p->len;

    ip_addr_t broadcast;
    IP4_ADDR(&broadcast, 255, 255, 255, 255);
    udp_sendto(g_doip_udp_pcb, p, &broadcast, DOIP_UDP_PORT);
    pbuf_free(p);

    printf("[DOIP] UDP announce sent\r\n");
}

/* ================================================================
   UDS over DOIP 响应 (被 uds.c 调用)
   ================================================================ */
void DOIP_Send_Diag_PosResponse(uint8_t *uds_data, uint16_t len)
{
    /* DOIP 诊断消息格式: SA(2) + TA(2) + UDS payload */
    uint8_t buf[4096];
    buf[0] = g_doip_logical_addr[0];
    buf[1] = g_doip_logical_addr[1];
    buf[2] = 0x0E;  /* 默认 TA */
    buf[3] = 0x00;
    if (len > 0 && uds_data != NULL) {
        memcpy(&buf[4], uds_data, len);
    }
    doip_tcp_send(DOIP_DIAG_ACK, buf, len + 4);
}

void DOIP_Send_Diag_NegResponse(uint8_t sid, uint8_t nrc)
{
    uint8_t buf[8];
    buf[0] = g_doip_logical_addr[0];
    buf[1] = g_doip_logical_addr[1];
    buf[2] = 0x0E;
    buf[3] = 0x00;
    buf[4] = 0x7F;
    buf[5] = sid;
    buf[6] = nrc;
    doip_tcp_send(DOIP_DIAG_NACK, buf, 7);
}

/* ================================================================
   初始化 DOIP TCP 服务器
   ================================================================ */
void DOIP_Init(void)
{
    /* TCP Server on port 13400 */
    g_doip_listen_pcb = tcp_new();
    if (g_doip_listen_pcb == NULL) {
        printf("[DOIP] tcp_new FAIL\r\n");
        return;
    }

    err_t err = tcp_bind(g_doip_listen_pcb, IP_ADDR_ANY, DOIP_TCP_PORT);
    if (err != ERR_OK) {
        printf("[DOIP] tcp_bind FAIL: %d\r\n", err);
        return;
    }

    g_doip_listen_pcb = tcp_listen(g_doip_listen_pcb);
    tcp_accept(g_doip_listen_pcb, doip_accept_callback);

    printf("[DOIP] TCP server on port %d\r\n", DOIP_TCP_PORT);

    /* UDP socket for vehicle announcement */
    g_doip_udp_pcb = udp_new();
    if (g_doip_udp_pcb != NULL) {
        udp_bind(g_doip_udp_pcb, IP_ADDR_ANY, DOIP_UDP_PORT);
        printf("[DOIP] UDP on port %d\r\n", DOIP_UDP_PORT);
    }
}

/* ================================================================
   周期性处理 (每主循环调用)
   ================================================================ */
void DOIP_Process(void)
{
    /* TCP raw API 回调在 ethernetif_input 中处理, 这里留空
       如有需要可添加超时处理、自动公告等 */
}
