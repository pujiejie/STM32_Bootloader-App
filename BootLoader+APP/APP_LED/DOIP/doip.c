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

/* 存储最近一次诊断请求的客户端源地址 (用于响应时正确填写TA) */
static uint16_t g_doip_client_sa = 0x0E00;

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

    if (len < 2) {
        /* 最小路由激活请求: 仅SA(2字节) */
        printf("[DOIP] Routing activation: payload too short (%lu)\r\n", len);
        resp[0] = g_doip_logical_addr[0];
        resp[1] = g_doip_logical_addr[1];
        resp[2] = 0x00; resp[3] = 0x00;
        resp[4] = DOIP_ROUTING_WRONG_TYPE;
        resp[5] = 0x00; resp[6] = 0x00; resp[7] = 0x00; resp[8] = 0x00;
        doip_tcp_send(DOIP_ROUTING_RSP, resp, 9);
        return;
    }
    client_la = be16(&payload[0]);

    /* Activation Type 在 payload[2-5] (4字节), ISO 13400-2 Table 12 */
    uint32_t act_type = (len >= 6) ? be32(&payload[2]) : 0x00000000;

    printf("[DOIP] Routing activation: src=0x%04X type=0x%08lX\r\n", client_la, act_type);

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

    /* 诊断消息最小长度: SA(2) + TA(2) = 4字节 (UDS可为0) */
    if (len < 4) {
        uint8_t nack = DOIP_DIAG_NACK_INVALID_LEN;
        doip_tcp_send(DOIP_DIAG_NACK, &nack, 1);
        return;
    }

    /* SA + TA (4 bytes) + UDS payload */
    uint16_t sa = be16(&payload[0]);
    uint16_t ta = be16(&payload[2]);
    uint8_t  *uds_data = &payload[4];
    uint16_t uds_len   = (uint16_t)(len - 4);  /* 现在保证 len >= 4，不会溢出 */

    /* 存储客户端SA，响应时TA应等于请求SA (源地址互换) */
    g_doip_client_sa = sa;

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
        /* ISO 13400-2: 不支持的消息类型回复通用NACK */
        {
            uint8_t nack_code = 0x03;  /* message type not supported */
            doip_tcp_send(DOIP_GEN_NACK, &nack_code, 1);
        }
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
        /* 对端正常关闭连接 */
        printf("[DOIP] Client disconnected\r\n");
        g_doip_client_pcb = NULL;       /* 先清指针，再关PCB */
        g_doip_routing_active = 0;
        tcp_close(pcb);
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
   TCP 错误回调 — 连接异常断开时清理状态
   ================================================================ */
static void doip_error_callback(void *arg, err_t err)
{
    printf("[DOIP] TCP error=%d, cleanup\r\n", err);
    /* 关键: 清除全局状态，否则会永久死锁 */
    g_doip_client_pcb = NULL;
    g_doip_routing_active = 0;
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
    tcp_err(new_pcb, doip_error_callback);   /* ← 注册错误回调，防死锁 */
    tcp_poll(new_pcb, NULL, 4);              /* ← 每8秒保活检测半开连接 */

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

    /* DoIP 头部 (8B) + 公告 payload (32B) = 40B */
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 40, PBUF_RAM);
    if (p == NULL) return;

    uint8_t *pl = (uint8_t *)p->payload;
    uint8_t *pp = pl;

    /* ─── DoIP Header (8 bytes) ─── */
    *pp++ = DOIP_PROTOCOL_VERSION;              /* version = 0x02 */
    *pp++ = (uint8_t)(~DOIP_PROTOCOL_VERSION);  /* version_inv = ~0x02 */
    set_be16(pp, DOIP_VEHICLE_ANNOUNCE);        /* payload_type = 0x0004 */
    pp += 2;
    set_be32(pp, 32);                           /* payload_len = 32 */
    pp += 4;

    /* ─── Vehicle Announce Payload (32 bytes) ─── */
    memcpy(pp, DOIP_VIN, 17); pp += 17;
    *pp++ = g_doip_logical_addr[0]; *pp++ = g_doip_logical_addr[1];
    for (int i = 5; i >= 0; i--) *pp++ = (uint8_t)(DOIP_EID >> (i * 8));
    for (int i = 5; i >= 0; i--) *pp++ = (uint8_t)(DOIP_GID >> (i * 8));
    *pp++ = 0x00;  /* Further action */

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
    /* DOIP 诊断消息格式: SA(2) + TA(2) + UDS payload
       SA = ECU逻辑地址, TA = 客户端源地址 (SA/TA互换)
       使用 static 缓冲避免 4KB 栈分配导致栈溢出 */
    static uint8_t buf[4096];

    if (len > 4092) return;  /* 安全阀: SA+TA=4, 剩余4092给UDS */

    buf[0] = g_doip_logical_addr[0];
    buf[1] = g_doip_logical_addr[1];
    buf[2] = (uint8_t)(g_doip_client_sa >> 8);
    buf[3] = (uint8_t)(g_doip_client_sa & 0xFF);
    if (len > 0 && uds_data != NULL) {
        memcpy(&buf[4], uds_data, len);
    }
    /* 使用 0x8001 Diagnostic Message 承载 UDS 响应
       (非 0x8002 ACK，后者仅含1字节确认码) */
    doip_tcp_send(DOIP_DIAG_MSG, buf, len + 4);
}

void DOIP_Send_Diag_NegResponse(uint8_t sid, uint8_t nrc)
{
    /* DOIP 诊断消息格式: SA(2) + TA(2) + UDS否定响应(0x7F+SID+NRC)
       使用 0x8001 Diagnostic Message 承载 (非 0x8003，后者是传输层NACK) */
    uint8_t buf[8];
    buf[0] = g_doip_logical_addr[0];
    buf[1] = g_doip_logical_addr[1];
    buf[2] = (uint8_t)(g_doip_client_sa >> 8);
    buf[3] = (uint8_t)(g_doip_client_sa & 0xFF);
    buf[4] = 0x7F;
    buf[5] = sid;
    buf[6] = nrc;
    doip_tcp_send(DOIP_DIAG_MSG, buf, 7);
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
    /* UDP 车辆公告: 网络link up后每5秒广播一次 (ISO 13400-2) */
    static uint32_t last_announce = 0;
    static uint8_t  first_run = 1;
    extern volatile uint8_t g_eth_link_up;

    if (first_run) {
        first_run = 0;
        printf("[DOIP] DOIP_Process started, link=%d tick=%lu pcb=%p\r\n",
               g_eth_link_up, HAL_GetTick(), (void*)g_doip_udp_pcb);
    }

    if (g_eth_link_up && (HAL_GetTick() - last_announce >= 5000)) {
        DOIP_Announce_UDP();
        last_announce = HAL_GetTick();
    }
}
