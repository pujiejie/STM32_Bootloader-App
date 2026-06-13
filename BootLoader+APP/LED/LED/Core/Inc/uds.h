#ifndef __UDS_H
#define __UDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h> // 为了使用 NULL

// ============================================================================
// 1. UDS 常用服务 ID (SID) 宏定义
// ============================================================================
#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL  0x10  // 10 服务：会话控制
#define UDS_SID_ECU_RESET                   0x11  // 11 服务：ECU 复位
#define UDS_SID_READ_DATA_BY_IDENTIFIER     0x22  // 22 服务：读数据
#define UDS_SID_SECURITY_ACCESS             0x27  // 27 服务：安全访问
#define UDS_SID_ROUTINE_CONTROL             0x31  // 31 服务：例行程序 (如擦除Flash)
#define UDS_SID_REQUEST_DOWNLOAD            0x34  // 34 服务：请求下载
#define UDS_SID_TRANSFER_DATA               0x36  // 36 服务：传输数据
#define UDS_SID_REQUEST_TRANSFER_EXIT       0x37  // 37 服务：请求退出传输
#define UDS_SID_TESTER_PRESENT              0x3E  // 3E 服务：诊断仪在线保持 (心跳)

// ============================================================================
// 2. UDS 响应规则宏定义
// ============================================================================
#define UDS_POSITIVE_RESPONSE_OFFSET        0x40  // 肯定响应偏移量 (请求SID + 0x40)
#define UDS_NEGATIVE_RESPONSE_SID           0x7F  // 否定响应标志 (固定为 7F)

// ============================================================================
// 3. UDS 常见否定响应码 (NRC - Negative Response Code)
// ============================================================================
#define NRC_SERVICE_NOT_SUPPORTED           0x11  // 11: 不支持该服务
#define NRC_SUB_FUNCTION_NOT_SUPPORTED      0x12  // 12: 不支持该子功能
#define NRC_INCORRECT_MESSAGE_LENGTH        0x13  // 13: 报文长度格式不对
#define NRC_CONDITIONS_NOT_CORRECT          0x22  // 22: 条件不满足 (如未解锁安全访问)
#define NRC_REQUEST_SEQUENCE_ERROR          0x24  // 24: 请求顺序错误 (如没发34就发36)
#define NRC_REQUEST_OUT_OF_RANGE            0x31  // 31: 请求超出范围 (地址非法/超出Flash)
#define NRC_SECURITY_ACCESS_DENIED          0x33  // 33: 安全拒绝 (权限不够)
#define NRC_INVALID_KEY                     0x35  // 35: 密钥无效 (0x27挑战失败)
#define NRC_EXCEEDED_NUMBER_OF_ATTEMPTS     0x36  // 36: 重试次数超限
#define NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED 0x37  // 37: 锁定等待时间未到
#define NRC_RESPONSE_PENDING                0x78  // 78: 响应挂起 (操作进行中,请轮询)

// ============================================================================
// 4. 对外开放的 API 函数声明 (让 main.c 等其他文件可以调用)
// ============================================================================

/**
 * @brief UDS 总机调度函数 (接收到纯 UDS 数据后调用此函数)
 * @param payload 指向纯 UDS 数据的指针 (首字节为 SID)
 * @param length  UDS 数据的总长度
 */
void Process_UDS_Request(uint8_t *payload, uint16_t length);

/**
 * @brief 发送 UDS 肯定响应 (SF 单帧短回复)
 * @param request_sid 请求的服务 ID
 * @param sub_func    子功能 (如果没有填 0)
 * @param data        附带的数据指针 (如果没有填 NULL)
 * @param data_len    附带数据的长度 (如果没有填 0)
 */
void Send_Positive_Response(uint8_t request_sid, uint8_t sub_func, uint8_t *data, uint8_t data_len);

/**
 * @brief 发送 UDS 否定响应 (NRC)
 * @param request_sid 失败的请求服务 ID
 * @param nrc_code    失败原因码 (NRC)
 */
void Send_Negative_Response(uint8_t request_sid, uint8_t nrc_code);

/**
 * @brief 处理 10 服务的具体业务逻辑 (Boot端专用版)
 */
void UDS_Service_10_Boot(uint8_t *payload, uint16_t length);

/**
 * @brief 刷写下载服务 (34/36/37) - Bootloader 专用
 */
void UDS_Service_34_Boot(uint8_t *payload, uint16_t length);
void UDS_Service_36_Boot(uint8_t *payload, uint16_t length);
void UDS_Service_37_Boot(uint8_t *payload, uint16_t length);

/**
 * @brief 例行程序 (31) - Flash 擦除等
 */
void UDS_Service_31_Boot(uint8_t *payload, uint16_t length);

/**
 * @brief 安全访问 (27) - 种子/密钥挑战
 */
void UDS_Service_27_Boot(uint8_t *payload, uint16_t length);

/**
 * @brief ECU 复位 (11) - 硬复位/软复位
 */
void UDS_Service_11_Boot(uint8_t *payload, uint16_t length);

// ============================================================================
// 5. 诊断会话状态枚举
// ============================================================================
#define UDS_SESSION_DEFAULT       0x01  // 默认会话
#define UDS_SESSION_PROGRAMMING   0x02  // 编程会话（刷写专用）
#define UDS_SESSION_EXTENDED      0x03  // 扩展会话

// 全局会话状态变量（供 Process_UDS_Request 内部控制使用）
extern uint8_t g_current_session;

// ============================================================================
// 6. 刷写传输状态机
// ============================================================================
#define TRANSFER_STATE_IDLE           0  // 空闲
#define TRANSFER_STATE_READY          1  // 已收到 0x34，等待数据
#define TRANSFER_STATE_IN_PROGRESS    2  // 正在接收 0x36 数据块

extern uint8_t  g_transfer_state;
extern uint32_t g_download_addr;       // 当前下载起始地址
extern uint32_t g_download_total;      // 下载总字节数
extern uint32_t g_bytes_received;      // 已接收字节数
extern uint8_t  g_expected_block_seq;  // 期望的下一个 Block 序号
extern uint32_t g_running_crc;         // 实时 CRC (在 36 中累积)

// ============================================================================
// 7. 安全访问状态
// ============================================================================
#define SECURITY_LOCKED   0
#define SECURITY_UNLOCKED 1

extern uint8_t g_security_state;       // 安全访问状态
extern uint8_t g_flash_erased;         // Flash 是否已擦除

extern uint32_t g_security_seed;       // 当前挑战种子
extern uint8_t  g_security_retry;      // 失败重试计数 (最多3次)
extern uint32_t g_security_lock_time;  // 锁定开始时间戳 (HAL tick)

#ifdef __cplusplus
}
#endif

#endif /* __UDS_H */
