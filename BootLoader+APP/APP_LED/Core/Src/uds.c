#include "uds.h"
#include "main.h"
#include "can.h"
#include "isotp.h"
#include "doip.h"
#include <string.h>

// ============================================================================
// BMS 数据模型
// ============================================================================
uint16_t g_bms_voltage    = 12600;
int16_t  g_bms_current    = -2300;
uint8_t  g_bms_soc        = 78;
uint8_t  g_bms_soh        = 95;
uint16_t g_bms_cell_max   = 3650;
uint16_t g_bms_cell_min   = 3580;
int8_t   g_bms_temp_max   = 35;
int8_t   g_bms_temp_min   = 28;
uint8_t  g_bms_status     = 2;
uint8_t  g_bms_cell_count = 4;
char     g_bms_vin[18]    = "FCX2026BMS7890123";
char     g_bms_sw_version[16] = "V2.1.0_RC2";
uint32_t g_bms_uptime     = 0;
uint8_t  g_charge_enable  = 1;
uint8_t  g_balance_enable = 0;

// DTC
DTC_Record_t g_dtc_list[MAX_DTC_COUNT] = {
    { .dtc_code = 0x0000F100, .status = DTC_STATUS_CONFIRMED | DTC_STATUS_WARNING_INDICATOR, .active = 1 },
    { .dtc_code = 0x0000F101, .status = DTC_STATUS_CONFIRMED, .active = 0 },
    { .dtc_code = 0x0000F102, .status = DTC_STATUS_PENDING, .active = 1 },
    { .dtc_code = 0x0000F103, .status = DTC_STATUS_CONFIRMED, .active = 0 },
};
uint8_t  g_dtc_count = 4;
uint8_t  g_dtc_enabled = 1;
uint32_t g_can_rx_count = 0;

// 会话 & 安全
uint8_t g_app_session    = 0x01;  // default
uint8_t g_app_security   = SECURITY_LOCKED;
uint8_t g_app_comm_state = COMM_NORMAL;

uint32_t g_app_seed      = 0;
uint8_t  g_app_retry     = 0;
uint32_t g_app_lock_time = 0;

// ============================================================================
// 通用响应
// ============================================================================
void Send_Negative_Response(uint8_t request_sid, uint8_t nrc_code)
{
    if (g_doip_routing_active)
    {
        DOIP_Send_Diag_NegResponse(request_sid, nrc_code);
        return;
    }
    uint8_t resp[3] = {0x7F, request_sid, nrc_code};
    ISOTP_Send_Data(resp, 3);
}

void Send_Positive_Response(uint8_t request_sid, uint8_t sub_func, uint8_t *data, uint8_t data_len)
{
    if (g_doip_routing_active)
    {
        static uint8_t resp[256];
        resp[0] = request_sid + 0x40;
        uint8_t total_len = 1;
        if (data_len > 0 && data != NULL) {
            for (uint8_t i = 0; i < data_len; i++)
                resp[total_len++] = data[i];
        }
        DOIP_Send_Diag_PosResponse(resp, total_len);
        return;
    }
    static uint8_t resp[256];
    resp[0] = request_sid + 0x40;
    uint8_t total_len = 1;
    if (data_len > 0 && data != NULL) {
        for (uint8_t i = 0; i < data_len; i++)
            resp[total_len++] = data[i];
    }
    ISOTP_Send_Data(resp, total_len);
}

// ============================================================================
// 10 服务：会话控制
// ============================================================================
void UDS_Service_10_App(uint8_t *payload, uint16_t length)
{
    if (length < 2) { Send_Negative_Response(0x10, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    uint8_t sub = payload[1] & 0x7F;
    uint8_t p2 = 0x32, p2s_h = 0x13, p2s_l = 0x88;

    switch (sub) {
        case 0x01: // 默认会话
            g_app_session  = 0x01;
            g_app_security = SECURITY_LOCKED;
            break;
        case 0x02: // 编程会话 → 跳 Bootloader
            {
                uint8_t r[4] = {0x02, 0x00, p2, p2s_h};
                Send_Positive_Response(0x10, 0, r, 4);
                __HAL_RCC_PWR_CLK_ENABLE();
                HAL_PWR_EnableBkUpAccess();
                RTC->BKP0R = 0xDEADBEEF;
                HAL_Delay(20);
                NVIC_SystemReset();
                return;
            }
        case 0x03: // 扩展会话
            g_app_session  = 0x03;
            g_app_security = SECURITY_LOCKED;
            break;
        default:
            Send_Negative_Response(0x10, NRC_SUB_FUNCTION_NOT_SUPPORTED);
            return;
    }
    uint8_t r[4] = {sub, 0x00, p2, p2s_h};
    Send_Positive_Response(0x10, 0, r, 4);
}

// ============================================================================
// 11 服务：ECU 复位
// ============================================================================
void UDS_Service_11_App(uint8_t *payload, uint16_t length)
{
    if (length < 2) { Send_Negative_Response(0x11, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    uint8_t rt = payload[1] & 0x7F;

    if (rt == 0x01 || rt == 0x03) {
        uint8_t r[1] = {rt};
        Send_Positive_Response(0x11, 0, r, 1);
        HAL_Delay(20);
        NVIC_SystemReset();
    } else {
        Send_Negative_Response(0x11, NRC_SUB_FUNCTION_NOT_SUPPORTED);
    }
}

// ============================================================================
// 14 服务：清除诊断信息
// ============================================================================
void UDS_Service_14_App(uint8_t *payload, uint16_t length)
{
    if (length < 4) { Send_Negative_Response(0x14, NRC_INCORRECT_MESSAGE_LENGTH); return; }

    uint32_t group = ((uint32_t)payload[1] << 16) | ((uint32_t)payload[2] << 8) | payload[3];

    if (group == 0xFFFFFF) {
        g_dtc_count = 0;
        memset(g_dtc_list, 0, sizeof(g_dtc_list));
        Send_Positive_Response(0x14, 0, NULL, 0);
        return;
    }
    for (uint8_t i = 0; i < g_dtc_count; i++) {
        if (g_dtc_list[i].dtc_code == group) {
            for (uint8_t j = i; j < g_dtc_count - 1; j++)
                g_dtc_list[j] = g_dtc_list[j + 1];
            g_dtc_count--;
            Send_Positive_Response(0x14, 0, NULL, 0);
            return;
        }
    }
    Send_Negative_Response(0x14, NRC_REQUEST_OUT_OF_RANGE);
}

// ============================================================================
// 19 服务：读 DTC
// ============================================================================
void UDS_Service_19_App(uint8_t *payload, uint16_t length)
{
    if (!g_dtc_enabled) { Send_Negative_Response(0x19, NRC_CONDITIONS_NOT_CORRECT); return; }
    if (length < 2) { Send_Negative_Response(0x19, NRC_INCORRECT_MESSAGE_LENGTH); return; }

    uint8_t sf = payload[1];

    switch (sf) {
        case 0x01: {
            uint8_t mask = (length >= 3) ? payload[2] : 0xFF;
            uint8_t cnt = 0;
            for (uint8_t i = 0; i < g_dtc_count; i++)
                if (g_dtc_list[i].status & mask) cnt++;
            uint8_t r[4] = {0x01, 0x00, 0x00, cnt};
            Send_Positive_Response(0x19, 0, r, 4);
            break;
        }
        case 0x02: {
            uint8_t mask = (length >= 3) ? payload[2] : 0xFF;
            uint8_t r[256]; uint16_t ri = 0;
            r[ri++] = 0x02; r[ri++] = 0x00; r[ri++] = 0x00;
            for (uint8_t i = 0; i < g_dtc_count; i++) {
                if (g_dtc_list[i].status & mask) {
                    r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code >> 16);
                    r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code >> 8);
                    r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code);
                    r[ri++] = g_dtc_list[i].status;
                }
            }
            Send_Positive_Response(0x19, 0, r, ri);
            break;
        }
        case 0x06: {
            uint8_t r[256]; uint16_t ri = 0;
            r[ri++] = 0x06; r[ri++] = 0x00; r[ri++] = 0x00;
            for (uint8_t i = 0; i < g_dtc_count; i++) {
                r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code >> 16);
                r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code >> 8);
                r[ri++] = (uint8_t)(g_dtc_list[i].dtc_code);
                r[ri++] = g_dtc_list[i].status;
            }
            Send_Positive_Response(0x19, 0, r, ri);
            break;
        }
        default:
            Send_Negative_Response(0x19, NRC_SUB_FUNCTION_NOT_SUPPORTED);
    }
}

// ============================================================================
// 22 服务：读数据
// ============================================================================
void UDS_Service_22_App(uint8_t *payload, uint16_t length)
{
    if (length < 3) { Send_Negative_Response(0x22, NRC_INCORRECT_MESSAGE_LENGTH); return; }

    uint16_t did = (payload[1] << 8) | payload[2];
    uint8_t r[128];

    switch (did) {
        case DID_VIN:
            r[0] = 0xF1; r[1] = 0x90;
            for (uint8_t i = 0; i < 17; i++) r[2+i] = (uint8_t)g_bms_vin[i];
            Send_Positive_Response(0x22, 0, r, 19);
            break;
        case DID_SW_VERSION: {
            uint8_t sl = (uint8_t)strlen(g_bms_sw_version);
            r[0] = 0xF1; r[1] = 0x89;
            for (uint8_t i = 0; i < sl; i++) r[2+i] = (uint8_t)g_bms_sw_version[i];
            Send_Positive_Response(0x22, 0, r, 2+sl);
            break; }
        case DID_BAT_TOTAL_VOLTAGE:
            r[0]=0xF1; r[1]=0x00; r[2]=(uint8_t)(g_bms_voltage>>8); r[3]=(uint8_t)g_bms_voltage;
            Send_Positive_Response(0x22, 0, r, 4); break;
        case DID_BAT_CURRENT:
            r[0]=0xF1; r[1]=0x01; r[2]=(uint8_t)(g_bms_current>>8); r[3]=(uint8_t)g_bms_current;
            Send_Positive_Response(0x22, 0, r, 4); break;
        case DID_BAT_SOC:
            r[0]=0xF1; r[1]=0x02; r[2]=g_bms_soc;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BAT_SOH:
            r[0]=0xF1; r[1]=0x03; r[2]=g_bms_soh;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BAT_CELL_VOLT_MAX:
            r[0]=0xF1; r[1]=0x04; r[2]=(uint8_t)(g_bms_cell_max>>8); r[3]=(uint8_t)g_bms_cell_max;
            Send_Positive_Response(0x22, 0, r, 4); break;
        case DID_BAT_CELL_VOLT_MIN:
            r[0]=0xF1; r[1]=0x05; r[2]=(uint8_t)(g_bms_cell_min>>8); r[3]=(uint8_t)g_bms_cell_min;
            Send_Positive_Response(0x22, 0, r, 4); break;
        case DID_BAT_TEMP_MAX:
            r[0]=0xF1; r[1]=0x06; r[2]=(uint8_t)g_bms_temp_max;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BAT_TEMP_MIN:
            r[0]=0xF1; r[1]=0x07; r[2]=(uint8_t)g_bms_temp_min;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BAT_STATUS:
            r[0]=0xF1; r[1]=0x08; r[2]=g_bms_status;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BAT_CELL_COUNT:
            r[0]=0xF1; r[1]=0x09; r[2]=g_bms_cell_count;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_CAN_RX_COUNT:
            r[0]=0xF1; r[1]=0x10; r[2]=(uint8_t)(g_can_rx_count>>24);
            r[3]=(uint8_t)(g_can_rx_count>>16); r[4]=(uint8_t)(g_can_rx_count>>8); r[5]=(uint8_t)g_can_rx_count;
            Send_Positive_Response(0x22, 0, r, 6); break;
        case DID_DTC_COUNT:
            r[0]=0xF1; r[1]=0x11; r[2]=g_dtc_count;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_CHARGE_ENABLE:
            r[0]=0xF1; r[1]=0x80; r[2]=g_charge_enable;
            Send_Positive_Response(0x22, 0, r, 3); break;
        case DID_BALANCE_ENABLE:
            r[0]=0xF1; r[1]=0x81; r[2]=g_balance_enable;
            Send_Positive_Response(0x22, 0, r, 3); break;
        default:
            Send_Negative_Response(0x22, NRC_REQUEST_OUT_OF_RANGE);
    }
}

// ============================================================================
// 27 服务：安全访问
// ============================================================================
void UDS_Service_27_App(uint8_t *payload, uint16_t length)
{
    if (length < 2) { Send_Negative_Response(0x27, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    uint8_t sf = payload[1];

    if (sf == 0x01) {
        if (g_app_lock_time && (HAL_GetTick() - g_app_lock_time) < 10000) {
            Send_Negative_Response(0x27, NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED);
            return;
        }
        g_app_lock_time = 0; g_app_retry = 0;
        g_app_seed = (HAL_GetTick() ^ 0xA5C3E1F7);
        uint8_t r[6] = {0x01, (uint8_t)(g_app_seed>>24), (uint8_t)(g_app_seed>>16),
                        (uint8_t)(g_app_seed>>8), (uint8_t)g_app_seed};
        Send_Positive_Response(0x27, 0, r, 5);
        return;
    }
    if (sf == 0x02) {
        if (g_app_lock_time && (HAL_GetTick() - g_app_lock_time) < 10000) {
            Send_Negative_Response(0x27, NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED); return;
        }
        g_app_lock_time = 0;
        if (length < 6) { Send_Negative_Response(0x27, NRC_INCORRECT_MESSAGE_LENGTH); return; }
        uint32_t rk = ((uint32_t)payload[2]<<24)|((uint32_t)payload[3]<<16)|((uint32_t)payload[4]<<8)|payload[5];
        uint32_t ek = ((~g_app_seed) ^ 0x5A5A5A5A) + 0x3618BC91;
        if (rk == ek) {
            g_app_security = SECURITY_UNLOCKED; g_app_retry = 0;
            uint8_t r[1] = {0x02};
            Send_Positive_Response(0x27, 0, r, 1);
            return;
        }
        g_app_retry++;
        if (g_app_retry >= 3) { g_app_lock_time = HAL_GetTick(); g_app_retry = 0;
            Send_Negative_Response(0x27, NRC_EXCEEDED_NUMBER_OF_ATTEMPTS); return; }
        Send_Negative_Response(0x27, NRC_INVALID_KEY);
        return;
    }
    Send_Negative_Response(0x27, NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

// ============================================================================
// 28 服务：通信控制
// ============================================================================
void UDS_Service_28_App(uint8_t *payload, uint16_t length)
{
    if (length < 3) { Send_Negative_Response(0x28, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    uint8_t ct = payload[1] & 0x7F;
    uint8_t cm = payload[2];

    if (ct == 0x00 || ct == 0x01) {
        if (cm == 0x00)      g_app_comm_state = COMM_NORMAL;
        else if (cm == 0x03) g_app_comm_state = COMM_SILENT;
        else                 g_app_comm_state = COMM_NORMAL;
        uint8_t r[2] = {ct, cm};
        Send_Positive_Response(0x28, 0, r, 2);
    } else {
        Send_Negative_Response(0x28, NRC_SUB_FUNCTION_NOT_SUPPORTED);
    }
}

// ============================================================================
// 2E 服务：写数据（需安全解锁）
// ============================================================================
void UDS_Service_2E_App(uint8_t *payload, uint16_t length)
{
    if (g_app_security != SECURITY_UNLOCKED) {
        Send_Negative_Response(0x2E, NRC_SECURITY_ACCESS_DENIED); return;
    }
    if (length < 3) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }

    uint16_t did = (payload[1] << 8) | payload[2];

    switch (did) {
        case DID_VIN:
            if (length < 20) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            for (uint8_t i = 0; i < 17; i++) g_bms_vin[i] = (char)payload[3+i];
            g_bms_vin[17] = '\0'; break;
        case DID_SW_VERSION: {
            uint8_t sl = length - 3; if (sl > 15) sl = 15;
            for (uint8_t i = 0; i < sl; i++) g_bms_sw_version[i] = (char)payload[3+i];
            g_bms_sw_version[sl] = '\0'; break; }
        case DID_BAT_SOC:
            if (length < 4) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            g_bms_soc = payload[3]; break;
        case DID_BAT_STATUS:
            if (length < 4) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            g_bms_status = payload[3]; break;
        case DID_BAT_CURRENT:
            if (length < 5) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            g_bms_current = (int16_t)((payload[3]<<8)|payload[4]); break;
        case DID_CHARGE_ENABLE:
            if (length < 4) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            g_charge_enable = payload[3] ? 1 : 0; break;
        case DID_BALANCE_ENABLE:
            if (length < 4) { Send_Negative_Response(0x2E, NRC_INCORRECT_MESSAGE_LENGTH); return; }
            g_balance_enable = payload[3] ? 1 : 0; break;
        default:
            Send_Negative_Response(0x2E, NRC_REQUEST_OUT_OF_RANGE); return;
    }
    uint8_t r[2] = {(uint8_t)(did>>8), (uint8_t)did};
    Send_Positive_Response(0x2E, 0, r, 2);
}

// ============================================================================
// 31 服务：例行程序
// ============================================================================
void UDS_Service_31_App(uint8_t *payload, uint16_t length)
{
    if (length < 4) { Send_Negative_Response(0x31, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    if (g_app_security != SECURITY_UNLOCKED) {
        Send_Negative_Response(0x31, NRC_SECURITY_ACCESS_DENIED); return;
    }
    uint8_t  sf = payload[1];
    uint16_t rid = ((uint16_t)payload[2]<<8)|payload[3];

    if (sf != 0x01) { Send_Negative_Response(0x31, NRC_SUB_FUNCTION_NOT_SUPPORTED); return; }

    switch (rid) {
        case 0xE001: // BMS 自检
            { uint8_t r[3] = {0xE0, 0x01, 0x01}; Send_Positive_Response(0x31, 0, r, 3); break; }
        case 0xE002: // 复位 SOC 到 100%
            g_bms_soc = 100;
            { uint8_t r[3] = {0xE0, 0x02, 0x01}; Send_Positive_Response(0x31, 0, r, 3); break; }
        case 0xE003: // 复位 SOH 到 100%
            g_bms_soh = 100;
            { uint8_t r[3] = {0xE0, 0x03, 0x01}; Send_Positive_Response(0x31, 0, r, 3); break; }
        default:
            Send_Negative_Response(0x31, NRC_REQUEST_OUT_OF_RANGE);
    }
}

// ============================================================================
// 85 服务：控制 DTC 设置
// ============================================================================
void UDS_Service_85_App(uint8_t *payload, uint16_t length)
{
    if (length < 2) { Send_Negative_Response(0x85, NRC_INCORRECT_MESSAGE_LENGTH); return; }
    uint8_t sf = payload[1] & 0x7F;

    if (sf == 0x01) { g_dtc_enabled = 1; }
    else if (sf == 0x02) { g_dtc_enabled = 0; }
    else { Send_Negative_Response(0x85, NRC_SUB_FUNCTION_NOT_SUPPORTED); return; }

    uint8_t r[1] = {sf};
    Send_Positive_Response(0x85, 0, r, 1);
}

// ============================================================================
// UDS 调度器
// ============================================================================
void Process_UDS_Request(uint8_t *payload, uint16_t length)
{
    if (length == 0 || payload == NULL) return;

    uint8_t sid = payload[0];

    switch (sid) {
        case UDS_SID_DIAGNOSTIC_SESSION_CONTROL:   UDS_Service_10_App(payload, length); break;
        case UDS_SID_ECU_RESET:                    UDS_Service_11_App(payload, length); break;
        case UDS_SID_CLEAR_DIAGNOSTIC_INFO:        UDS_Service_14_App(payload, length); break;
        case UDS_SID_READ_DTC_INFORMATION:          UDS_Service_19_App(payload, length); break;
        case UDS_SID_READ_DATA_BY_IDENTIFIER:      UDS_Service_22_App(payload, length); break;
        case UDS_SID_SECURITY_ACCESS:              UDS_Service_27_App(payload, length); break;
        case UDS_SID_COMMUNICATION_CONTROL:         UDS_Service_28_App(payload, length); break;
        case UDS_SID_WRITE_DATA_BY_IDENTIFIER:     UDS_Service_2E_App(payload, length); break;
        case UDS_SID_ROUTINE_CONTROL:              UDS_Service_31_App(payload, length); break;
        case UDS_SID_TESTER_PRESENT:
            if (length < 2 || !(payload[1] & 0x80))
                Send_Positive_Response(0x3E, 0, NULL, 0);
            break;
        case UDS_SID_CONTROL_DTC_SETTING:          UDS_Service_85_App(payload, length); break;
        case UDS_SID_REQUEST_DOWNLOAD:
        case UDS_SID_TRANSFER_DATA:
        case UDS_SID_REQUEST_TRANSFER_EXIT:
            // 刷写服务由 Bootloader 处理，APP 不支持
            Send_Negative_Response(sid, NRC_CONDITIONS_NOT_CORRECT);
            break;
        default:
            Send_Negative_Response(sid, NRC_SERVICE_NOT_SUPPORTED);
    }
}
