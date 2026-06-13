#ifndef __UDS_H
#define __UDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// 1. UDS 服务 ID (SID)
// ============================================================================
#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL  0x10
#define UDS_SID_ECU_RESET                   0x11
#define UDS_SID_CLEAR_DIAGNOSTIC_INFO       0x14
#define UDS_SID_READ_DTC_INFORMATION         0x19
#define UDS_SID_READ_DATA_BY_IDENTIFIER     0x22
#define UDS_SID_SECURITY_ACCESS             0x27
#define UDS_SID_COMMUNICATION_CONTROL        0x28
#define UDS_SID_WRITE_DATA_BY_IDENTIFIER    0x2E
#define UDS_SID_ROUTINE_CONTROL             0x31
#define UDS_SID_REQUEST_DOWNLOAD            0x34
#define UDS_SID_TRANSFER_DATA               0x36
#define UDS_SID_REQUEST_TRANSFER_EXIT       0x37
#define UDS_SID_TESTER_PRESENT              0x3E
#define UDS_SID_CONTROL_DTC_SETTING         0x85

// ============================================================================
// 2. UDS 响应规则
// ============================================================================
#define UDS_POSITIVE_RESPONSE_OFFSET        0x40
#define UDS_NEGATIVE_RESPONSE_SID           0x7F

// ============================================================================
// 3. 否定响应码 (NRC)
// ============================================================================
#define NRC_GENERAL_REJECT                 0x10
#define NRC_SERVICE_NOT_SUPPORTED           0x11
#define NRC_SUB_FUNCTION_NOT_SUPPORTED      0x12
#define NRC_INCORRECT_MESSAGE_LENGTH        0x13
#define NRC_CONDITIONS_NOT_CORRECT          0x22
#define NRC_REQUEST_SEQUENCE_ERROR          0x24
#define NRC_REQUEST_OUT_OF_RANGE            0x31
#define NRC_SECURITY_ACCESS_DENIED          0x33
#define NRC_INVALID_KEY                     0x35
#define NRC_EXCEEDED_NUMBER_OF_ATTEMPTS     0x36
#define NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED 0x37
#define NRC_RESPONSE_PENDING                0x78

// ============================================================================
// 4. BMS 数据标识符 (DID)
// ============================================================================
#define DID_VIN                     0xF190
#define DID_SW_VERSION              0xF189
#define DID_BAT_TOTAL_VOLTAGE       0xF100
#define DID_BAT_CURRENT             0xF101
#define DID_BAT_SOC                 0xF102
#define DID_BAT_SOH                 0xF103
#define DID_BAT_CELL_VOLT_MAX       0xF104
#define DID_BAT_CELL_VOLT_MIN       0xF105
#define DID_BAT_TEMP_MAX            0xF106
#define DID_BAT_TEMP_MIN            0xF107
#define DID_BAT_STATUS              0xF108
#define DID_BAT_CELL_COUNT          0xF109
#define DID_CAN_RX_COUNT            0xF110
#define DID_DTC_COUNT               0xF111
#define DID_CHARGE_ENABLE           0xF180
#define DID_BALANCE_ENABLE          0xF181

// ============================================================================
// 5. DTC 故障码记录
// ============================================================================
#define DTC_STATUS_TEST_FAILED              0x01
#define DTC_STATUS_CONFIRMED                0x08
#define DTC_STATUS_PENDING                  0x20
#define DTC_STATUS_WARNING_INDICATOR        0x40

#define MAX_DTC_COUNT  8

typedef struct {
    uint32_t dtc_code;
    uint8_t  status;
    uint8_t  active;
} DTC_Record_t;

extern DTC_Record_t g_dtc_list[MAX_DTC_COUNT];
extern uint8_t      g_dtc_count;
extern uint32_t     g_can_rx_count;

// ============================================================================
// 6. BMS 数据
// ============================================================================
extern uint16_t g_bms_voltage;
extern int16_t  g_bms_current;
extern uint8_t  g_bms_soc;
extern uint8_t  g_bms_soh;
extern uint16_t g_bms_cell_max;
extern uint16_t g_bms_cell_min;
extern int8_t   g_bms_temp_max;
extern int8_t   g_bms_temp_min;
extern uint8_t  g_bms_status;
extern uint8_t  g_bms_cell_count;
extern char     g_bms_vin[18];
extern char     g_bms_sw_version[16];
extern uint32_t g_bms_uptime;

// ============================================================================
// 7. 会话 & 安全 & 通信状态
// ============================================================================
#define SECURITY_LOCKED   0
#define SECURITY_UNLOCKED 1
#define COMM_NORMAL       0
#define COMM_SILENT       1

extern uint8_t g_app_session;
extern uint8_t g_app_security;
extern uint8_t g_app_comm_state;
extern uint8_t g_dtc_enabled;

// ============================================================================
// 8. 函数声明
// ============================================================================
void Process_UDS_Request(uint8_t *payload, uint16_t length);
void Send_Positive_Response(uint8_t request_sid, uint8_t sub_func, uint8_t *data, uint8_t data_len);
void Send_Negative_Response(uint8_t request_sid, uint8_t nrc_code);

void UDS_Service_10_App(uint8_t *payload, uint16_t length);
void UDS_Service_11_App(uint8_t *payload, uint16_t length);
void UDS_Service_14_App(uint8_t *payload, uint16_t length);
void UDS_Service_19_App(uint8_t *payload, uint16_t length);
void UDS_Service_22_App(uint8_t *payload, uint16_t length);
void UDS_Service_27_App(uint8_t *payload, uint16_t length);
void UDS_Service_28_App(uint8_t *payload, uint16_t length);
void UDS_Service_2E_App(uint8_t *payload, uint16_t length);
void UDS_Service_31_App(uint8_t *payload, uint16_t length);
void UDS_Service_85_App(uint8_t *payload, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* __UDS_H */
