/**
  ******************************************************************************
  * @file    doip.h
  * @brief   ISO 13400 (DoIP) 协议实现头文件
  *          - TCP 端口 13400: 诊断通信 + 路由激活
  *          - UDP 端口 13400: 车辆发现/公告
  ******************************************************************************
  */

#ifndef __DOIP_H
#define __DOIP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ================================================================
   DoIP 协议常量 (ISO 13400-2)
   ================================================================ */
#define DOIP_PROTOCOL_VERSION       0x02    /* ISO 13400:2019 */
#define DOIP_TCP_PORT               13400
#define DOIP_UDP_PORT               13400

/* DoIP 通用头部: 8 字节 */
#define DOIP_HEADER_SIZE            8

/* ─── Payload Type (大端) ─── */
#define DOIP_GEN_NACK               0x0000
#define DOIP_VEHICLE_IDENT_REQ      0x0001
#define DOIP_VEHICLE_IDENT_EID      0x0002
#define DOIP_VEHICLE_IDENT_VIN      0x0003
#define DOIP_VEHICLE_ANNOUNCE       0x0004
#define DOIP_ROUTING_REQ            0x0005
#define DOIP_ROUTING_RSP            0x0006
#define DOIP_ALIVE_CHECK_REQ        0x0007
#define DOIP_ALIVE_CHECK_RSP        0x0008
#define DOIP_DIAG_MSG               0x8001
#define DOIP_DIAG_ACK               0x8002
#define DOIP_DIAG_NACK              0x8003

/* ─── DoIP Header ─── */
#pragma pack(1)
typedef struct {
    uint8_t  version;
    uint8_t  version_inv;       /* ~version */
    uint16_t payload_type;      /* big-endian */
    uint32_t payload_len;       /* big-endian */
} doip_hdr_t;
#pragma pack()

/* ─── Routing Activation Response Code ─── */
#define DOIP_ROUTING_SUCCESS        0x10
#define DOIP_ROUTING_UNKNOWN_SRC    0x00
#define DOIP_ROUTING_NO_SOCKET      0x01
#define DOIP_ROUTING_WRONG_TYPE     0x03
#define DOIP_ROUTING_ALREADY_ACTIVE 0x05

/* ─── Diagnostic NACK Codes ─── */
#define DOIP_DIAG_NACK_MEMORY           0x01
#define DOIP_DIAG_NACK_WRONG_STATE      0x02
#define DOIP_DIAG_NACK_INVALID_LEN      0x03
#define DOIP_DIAG_NACK_UNKNOWN_SRC      0x05
#define DOIP_DIAG_NACK_NO_ROUTING       0x06

/* ================================================================
   BMS 车辆信息 (用于 0x0001 车辆识别响应)
   ================================================================ */
#define DOIP_VIN          "VIN1234567890ABCD"
#define DOIP_EID          0x1234567890ABCDEFULL
#define DOIP_GID          0xFEDCBA0987654321ULL
#define DOIP_LOGICAL_ADDR 0x0E80       /* BMS 逻辑地址 */
#define DOIP_EID_LEN      6
#define DOIP_GID_LEN      6

/* ================================================================
   全局状态
   ================================================================ */
extern volatile uint8_t g_doip_routing_active;
extern volatile uint8_t g_doip_logical_addr[2];

/* ================================================================
   API
   ================================================================ */
void DOIP_Init(void);
void DOIP_Process(void);
void DOIP_Send_Diag_PosResponse(uint8_t *uds_data, uint16_t len);
void DOIP_Send_Diag_NegResponse(uint8_t sid, uint8_t nrc);
void DOIP_Announce_UDP(void);

#ifdef __cplusplus
}
#endif

#endif /* __DOIP_H */
