#include "uds.h"
#include "main.h" // 为了使用 SHARED_RAM_MAGIC_ADDR 和 NVIC_SystemReset
#include "can.h"
#include "isotp.h" // 引入底层发送引擎
#include "flash_if.h"

uint8_t g_current_session = UDS_SESSION_DEFAULT;

// 刷写传输全局状态
uint8_t  g_transfer_state     = TRANSFER_STATE_IDLE;
uint32_t g_download_addr      = 0;
uint32_t g_download_total     = 0;
uint32_t g_bytes_received     = 0;
uint8_t  g_expected_block_seq = 1;
uint32_t g_running_crc         = 0xFFFFFFFF;  // 实时 CRC

// 安全访问 + 刷写前置条件
uint8_t g_security_state       = SECURITY_LOCKED;   // 每次进 Bootloader 默认锁定
uint8_t g_flash_erased         = 0;                 // 进入编程会话后重置

// 安全访问: 种子/密钥 + 防暴力破解
uint32_t g_security_seed       = 0;
uint8_t  g_security_retry      = 0;                 // 本次会话失败次数
uint32_t g_security_lock_time  = 0;                 // 锁定开始时间戳
#define SECURITY_MAX_RETRY      3                    // 最多重试3次
#define SECURITY_LOCKOUT_MS     10000                // 锁定10秒

// 发送否定响应 (NRC)
void Send_Negative_Response(uint8_t request_sid, uint8_t nrc_code)
{
    uint8_t resp_data[3];
    resp_data[0] = 0x7F;
    resp_data[1] = request_sid;
    resp_data[2] = nrc_code;
    
    // 一共就3个字节，底层会自动用 单帧 发出去
    ISOTP_Send_Data(resp_data, 3);
}

// 发送肯定响应 (简易单帧版，假设回复数据不超过 6 个字节)
void Send_Positive_Response(uint8_t request_sid, uint8_t sub_func_or_did_len, uint8_t *data, uint8_t data_len)
{
    static uint8_t resp_data[256]; // 准备一个够大的缓冲
    
    resp_data[0] = request_sid + 0x40; // 肯定响应规矩：原SID + 0x40
    
    // 有些服务不需要附带数据，有些需要
    uint8_t total_len = 1;
    
    // 如果有附带数据，全部抄进去
    if (data_len > 0 && data != NULL) {
        for (uint8_t i = 0; i < data_len; i++) {
            resp_data[total_len++] = data[i];
        }
    }
    
    // 直接扔给底层，不管它是2个字节还是200个字节，底层会自动处理！
    ISOTP_Send_Data(resp_data, total_len);
}

// ==========================================
// 10 服务 (会话控制) - OEM 车规级
// 参照: ISO 14229-1 Table 40 + 主流 OEM 规范
// ==========================================
void UDS_Service_10_Boot(uint8_t *payload, uint16_t length)
{
    if (length < 2) { 
        Send_Negative_Response(0x10, NRC_INCORRECT_MESSAGE_LENGTH); 
        return; 
    }

    uint8_t sub_func = payload[1] & 0x7F; // 去掉 bit7 (suppressPosRsp)

    // ───── 请求: 默认会话 (0x01) ─────
    if (sub_func == UDS_SESSION_DEFAULT)
    {
        // 已经在默认会话 → 仅确认，不跳 APP（避免误触发）
        if (g_current_session == UDS_SESSION_DEFAULT)
        {
            uint8_t resp[5] = {0x01, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
        }
        // 从编程/扩展 → 退回默认 → Bootloader 使命结束，回到 APP
        else
        {
            // 如果在编程会话下，不切走！假装接受但保持 02
            if (g_current_session == UDS_SESSION_PROGRAMMING)
            {
                uint8_t resp[5] = {0x01, 0x00, 0x32, 0x27, 0x10};
                Send_Positive_Response(0x10, 0, resp, 5);
                return; // 不跳 APP，不切会话
            }
            uint8_t resp[5] = {0x01, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
            HAL_Delay(30); // 确保报文发完
            Jump_To_App(); // 🔚 退出 Bootloader
        }
        return;
    }

    // ───── 请求: 编程会话 (0x02) ─────
    if (sub_func == UDS_SESSION_PROGRAMMING)
    {
        // 从编程 → 编程: 仅确认（可能用于重置编程状态）
        if (g_current_session == UDS_SESSION_PROGRAMMING)
        {
            uint8_t resp[5] = {0x02, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
        }
        // 从默认/扩展 → 编程: 进入刷写模式
        else
        {
            g_current_session = UDS_SESSION_PROGRAMMING;
            g_security_state  = SECURITY_LOCKED;  // 重新锁定安全访问
            g_flash_erased    = 0;                 // 重置擦除标志
            g_security_retry  = 0;                 // 重置重试计数
            g_security_lock_time = 0;              // 清除锁定时间
            uint8_t resp[5] = {0x02, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
        }
        return;
    }

    // ───── 请求: 扩展会话 (0x03) ─────
    if (sub_func == UDS_SESSION_EXTENDED)
    {
        // OEM 规范: 编程会话下不允许直接跳扩展会话
        if (g_current_session == UDS_SESSION_PROGRAMMING)
        {
            Send_Negative_Response(0x10, NRC_CONDITIONS_NOT_CORRECT); // NRC 0x22
            return;
        }

        // 已在扩展 → 仅确认
        if (g_current_session == UDS_SESSION_EXTENDED)
        {
            uint8_t resp[5] = {0x03, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
        }
        // 从默认 → 扩展: 进入扩展诊断
        else
        {
            g_current_session = UDS_SESSION_EXTENDED;
            uint8_t resp[5] = {0x03, 0x00, 0x32, 0x27, 0x10};
            Send_Positive_Response(0x10, 0, resp, 5);
        }
        return;
    }

    // ───── 其他子功能: 不支持 ─────
    Send_Negative_Response(0x10, NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

// ==========================================
// 11 服务 (ECU 复位) - OEM 车规级
// 参照: ISO 14229-1 §9.3
// ==========================================
void UDS_Service_11_Boot(uint8_t *payload, uint16_t length)
{
    if (length < 2) {
        Send_Negative_Response(0x11, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    uint8_t reset_type = payload[1] & 0x7F;

    switch (reset_type)
    {
        case 0x01: // hardReset — 模拟上电复位
        {
            // 肯定响应: 51 01 + powerDownTime
            uint8_t resp[2] = { 0x01, 0x32 }; // powerDownTime = 50ms
            Send_Positive_Response(0x11, 0, resp, 2);
            HAL_Delay(30); // 确保报文发完
            NVIC_SystemReset();
            break;
        }

        case 0x03: // softReset — 软复位
        {
            uint8_t resp[1] = { 0x03 };
            Send_Positive_Response(0x11, 0, resp, 1);
            HAL_Delay(30);
            NVIC_SystemReset();
            break;
        }

        default:
            Send_Negative_Response(0x11, NRC_SUB_FUNCTION_NOT_SUPPORTED);
            break;
    }
}

// ==========================================
// 22 服务 (通过 ID 读数据)
// ==========================================
void UDS_Service_22_App(uint8_t *payload, uint16_t length)
{
    // 请求格式是：22 + DID高字节 + DID低字节 (所以总长度必须是3)
    if (length < 3) { Send_Negative_Response(0x22, 0x13); return; }

    // 提取要求读取的 DID (比如 0xF190 代表车架号 VIN)
    uint16_t did = (payload[1] << 8) | payload[2];

    if (did == 0xF190) // 诊断仪要求读 VIN 码！
    {
        uint8_t resp[20]; 
        resp[0] = 0xF1; // 肯定响应需要回带原来的 DID
        resp[1] = 0x90;
        
        // 假设我们的车架号是 17 位字符串 "VIN1234567890ABCD"
        uint8_t my_vin[17] = "VIN1234567890ABCD"; 
        for(uint8_t i=0; i<17; i++) {
            resp[2+i] = my_vin[i];
        }

        // 🌟 见证奇迹：我们要发 19 个字节的附加数据回去！
        // 因为大于 7 字节，ISOTP_Send_Data 会自动把它包成首帧发出，然后拦截对方的流控帧，再连发两包连续帧！
        Send_Positive_Response(0x22, 0, resp, 19);
    }
    else if (did == 0xF189) // 你还可以自己加别的，比如读取软件版本号
    {
        uint8_t resp[6] = {0xF1, 0x89, 'V', '1', '.', '0'};
        Send_Positive_Response(0x22, 0, resp, 6);
    }
    else
    {
        Send_Negative_Response(0x22, NRC_REQUEST_OUT_OF_RANGE);
    }
}

// ==========================================
// 27 服务 (安全访问) - OEM 车规级
// 参照: ISO 14229-1 §9.4
// 种子/密钥挑战应答协议 (Level 0x01)
// 防暴力破解: 3次失败锁定10秒
// ==========================================
void UDS_Service_27_Boot(uint8_t *payload, uint16_t length)
{
    if (length < 2) {
        Send_Negative_Response(0x27, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    // Session check (must be in programming)
    if (g_current_session != UDS_SESSION_PROGRAMMING)
    {
        Send_Negative_Response(0x27, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    uint8_t sub_func = payload[1];

    // ═══════════════════════════════════════════════
    // 子功能 0x01: requestSeed — 诊断仪请求种子
    // 请求: 27 01
    // 响应: 67 01 + seed[4]
    // ═══════════════════════════════════════════════
    if (sub_func == 0x01)
    {
        // ── 检查是否在锁定期 ──
        if (g_security_lock_time != 0)
        {
            if ((HAL_GetTick() - g_security_lock_time) < SECURITY_LOCKOUT_MS)
            {
                Send_Negative_Response(0x27, NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED);
                return;
            }
            // 锁定期已过，重置
            g_security_lock_time = 0;
            g_security_retry    = 0;
        }

        // ── 生成随机种子 ──
        // 熵源: HAL_GetTick() + SysTick当前值 混合
        uint32_t tick  = HAL_GetTick();
        uint32_t extra = (uint32_t)(SysTick->VAL);  // SysTick倒计数
        g_security_seed = (tick ^ (extra << 3) ^ 0xA5C3E1F7);

        // ── 肯定响应: 67 01 + seed[4] (大端) ──
        uint8_t resp[6] = {
            0x01,
            (uint8_t)(g_security_seed >> 24),
            (uint8_t)(g_security_seed >> 16),
            (uint8_t)(g_security_seed >> 8),
            (uint8_t)(g_security_seed)
        };
        Send_Positive_Response(0x27, 0, resp, 5);
        return;
    }

    // ═══════════════════════════════════════════════
    // 子功能 0x02: sendKey — 诊断仪发送密钥
    // 请求: 27 02 + key[4]
    // 响应: 67 02 (成功) / 7F 27 35 (失败)
    // ═══════════════════════════════════════════════
    if (sub_func == 0x02)
    {
        // ── 检查是否在锁定期 ──
        if (g_security_lock_time != 0)
        {
            if ((HAL_GetTick() - g_security_lock_time) < SECURITY_LOCKOUT_MS)
            {
                Send_Negative_Response(0x27, NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED);
                return;
            }
            g_security_lock_time = 0;
            g_security_retry    = 0;
        }

        // ── 长度校验: 27 + 02 + key[4] = 6 字节 ──
        if (length < 6)
        {
            Send_Negative_Response(0x27, NRC_INCORRECT_MESSAGE_LENGTH);
            return;
        }

        // ── 解析诊断仪发来的密钥 (大端) ──
        uint32_t received_key = ((uint32_t)payload[2] << 24) |
                                ((uint32_t)payload[3] << 16) |
                                ((uint32_t)payload[4] << 8)  |
                                ((uint32_t)payload[5]);

        // ── ECU 自己计算的期望密钥 ──
        // 算法: key = ((~seed) ^ 0x5A5A5A5A) + 0x3618BC91
        uint32_t expected_key = ((~g_security_seed) ^ 0x5A5A5A5A) + 0x3618BC91;

        // ── 对比 ──
        if (received_key == expected_key)
        {
            // ✅ 解锁成功
            g_security_state = SECURITY_UNLOCKED;
            g_security_retry = 0;
            g_security_lock_time = 0;

            uint8_t resp[1] = { 0x02 };
            Send_Positive_Response(0x27, 0, resp, 1);
            return;
        }
        else
        {
            // ❌ 密钥错误
            g_security_retry++;

            if (g_security_retry >= SECURITY_MAX_RETRY)
            {
                // 超过最大重试次数 → 锁定
                g_security_lock_time = HAL_GetTick();
                g_security_retry    = 0; // 锁定期不累计
                Send_Negative_Response(0x27, NRC_EXCEEDED_NUMBER_OF_ATTEMPTS);
                return;
            }

            Send_Negative_Response(0x27, NRC_INVALID_KEY);
            return;
        }
    }

    // ── 其他子功能: 不支持 ──
    Send_Negative_Response(0x27, NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

// ==========================================
// 34 服务 (请求下载) - OEM 车规级
// 参照: ISO 14229-1 §14.1
// 请求格式: 34 + dataFormatIdentifier + addressAndLengthFormatIdentifier
//           + memoryAddress[] + memorySize[]
// ==========================================
// ==========================================
// 31 服务 (例行程序) - OEM 车规级
// 参照: ISO 14229-1 §13
// 支持 routineID: 0xFF00 (Flash 擦除)
// ==========================================
void UDS_Service_31_Boot(uint8_t *payload, uint16_t length)
{
    // ── 长度: 31 + subFunc + routineID[2] = 4 字节 ──
    if (length < 4)
    {
        Send_Negative_Response(0x31, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    uint8_t  sub_func   = payload[1];       // 0x01 = startRoutine
    uint16_t routine_id = ((uint16_t)payload[2] << 8) | payload[3];

    // ── 只支持 startRoutine (0x01) ──
    if (sub_func != 0x01)
    {
        Send_Negative_Response(0x31, NRC_SUB_FUNCTION_NOT_SUPPORTED);
        return;
    }

    // ── 例行程序 0xFF00: Flash 擦除 ──
    if (routine_id == 0xFF00)
    {
        // 前置条件: 必须在编程会话 (0x02) 下
        if (g_current_session != UDS_SESSION_PROGRAMMING)
        {
            Send_Negative_Response(0x31, NRC_CONDITIONS_NOT_CORRECT);
            return;
        }
        // 前置条件: 必须已解锁安全访问
        if (g_security_state != SECURITY_UNLOCKED)
        {
            Send_Negative_Response(0x31, NRC_SECURITY_ACCESS_DENIED);
            return;
        }

        // 如果已经擦完了，直接回 71 (诊断仪轮询到了)
        if (g_flash_erased)
        {
            uint8_t resp[3] = { 0x01, 0xFF, 0x00 };  // 子功能 + routineID[2]
            Send_Positive_Response(0x31, 0, resp, 3);
            return;
        }

        // ── 首次请求: 先回 78 让诊断仪轮询，再执行擦除 ──
        Send_Negative_Response(0x31, NRC_RESPONSE_PENDING);  // 7F 31 78
        HAL_Delay(30);   // 确保 78 帧飞出去

        // 阻塞擦除 (期间 CAN FIFO 自动收轮询帧)
        if (!Flash_If_Erase_App_Area())
        {
            return; // 擦除失败
        }

        g_flash_erased = 1; // 擦除完成
        return;              // 返回后诊断仪的轮询帧在 FIFO 里等着
    }

    // ── 其他例行程序: 不支持 ──
    Send_Negative_Response(0x31, NRC_SUB_FUNCTION_NOT_SUPPORTED);
}

// ==========================================
// 34 服务 (请求下载) - OEM 车规级
// 参照: ISO 14229-1 §14.1
// 请求格式: 34 + dataFormatIdentifier + addressAndLengthFormatIdentifier
//           + memoryAddress[] + memorySize[]
// ⚠️ 不包含 Flash 擦除！擦除由 0x31 例行程序负责
// ==========================================
void UDS_Service_34_Boot(uint8_t *payload, uint16_t length)
{
    // ── 前置条件 1: 必须在编程会话 (0x02) 下 ──
    if (g_current_session != UDS_SESSION_PROGRAMMING)
    {
        Send_Negative_Response(0x34, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    if (g_security_state != SECURITY_UNLOCKED)
    {
        Send_Negative_Response(0x34, NRC_SECURITY_ACCESS_DENIED);
        return;
    }

    {
        if (!g_flash_erased)
        {
            Send_Negative_Response(0x34, NRC_CONDITIONS_NOT_CORRECT);
            return;
        }
    }

    // ── 长度校验: 最少 34 + 00 + 44 + 4字节地址 + 4字节大小 = 11 字节 ──
    if (length < 11)
    {
        Send_Negative_Response(0x34, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    uint8_t  dataFormatId  = payload[1];       // 0x00 = 不压缩不加密
    uint8_t  addrLenFmt    = payload[2];       // 0x44 = 4字节地址 + 4字节大小
    uint8_t  addrSize      = (addrLenFmt >> 4) & 0x0F;  // 高半字节
    uint8_t  sizeSize      = (addrLenFmt & 0x0F);       // 低半字节

    // ── 校验 dataFormatIdentifier ──
    if (dataFormatId != 0x00)
    {
        Send_Negative_Response(0x34, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    // ── 校验地址和大小字节数 ──
    if (addrSize != 4 || sizeSize != 4)
    {
        Send_Negative_Response(0x34, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    // ── 解析大端地址 ──
    uint32_t memAddress = ((uint32_t)payload[3] << 24) |
                          ((uint32_t)payload[4] << 16) |
                          ((uint32_t)payload[5] << 8)  |
                          ((uint32_t)payload[6]);

    // ── 解析大端大小 ──
    uint32_t memSize = ((uint32_t)payload[7] << 24) |
                       ((uint32_t)payload[8] << 16) |
                       ((uint32_t)payload[9] << 8)  |
                       ((uint32_t)payload[10]);

    // ── 安全阀: 地址必须在 APP 区域内 ──
    if (memAddress < APP_FLASH_START_ADDR ||
        memAddress + memSize > APP_FLASH_END_ADDR)
    {
        Send_Negative_Response(0x34, NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    // ── 保存下载参数 (不执行擦除！擦除已由 0x31 完成) ──
    g_download_addr      = memAddress;
    g_download_total     = memSize;
    g_bytes_received     = 0;
    g_expected_block_seq = 1;
    g_running_crc        = 0xFFFFFFFF;
    g_transfer_state     = TRANSFER_STATE_READY;

    // 初始化硬件 CRC 单元 (复位后重新施加 REV_IN/REV_OUT)
    CRC->CR = CRC_CR_RESET;
    CRC->CR = (1U << 6) | (1U << 5);

    // ── 肯定响应: 74 + lengthFormatIdentifier(0x44) + maxBlockLen[4] ──
    uint8_t resp[5] = {
        0x44,                         // lengthFormatIdentifier
        0x00, 0x00, 0x04, 0x00,       // maxNumberOfBlockLength = 1024 (大端)
    };
    Send_Positive_Response(0x34, 0, resp, 5);
}

// ==========================================
// 36 服务 (传输数据) - OEM 车规级
// 参照: ISO 14229-1 §14.2
// 请求格式: 36 + blockSequenceCounter + transferRequestParameterRecord[]
// ==========================================
void UDS_Service_36_Boot(uint8_t *payload, uint16_t length)
{
    // ── 前置条件: 必须在编程会话 (0x02) 下 ──
    if (g_current_session != UDS_SESSION_PROGRAMMING)
    {
        Send_Negative_Response(0x36, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    // ── 顺序校验: 必须先发 0x34 ──
    if (g_transfer_state == TRANSFER_STATE_IDLE)
    {
        Send_Negative_Response(0x36, NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    // ── 长度校验: 至少 36 + blockSeq = 2 字节 ──
    if (length < 2)
    {
        Send_Negative_Response(0x36, NRC_INCORRECT_MESSAGE_LENGTH);
        return;
    }

    uint8_t block_seq = payload[1];
    uint16_t data_len = length - 2;  // 去掉 SID 和 BlockSeq
    uint8_t *data_ptr = &payload[2];

    // ── Block 序号校验 ──
    if (block_seq != g_expected_block_seq)
    {
        // 序号不匹配 → 请求顺序错误
        Send_Negative_Response(0x36, NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    // ── 大小校验: 不能超出已申请的范围 ──
    if (g_bytes_received + data_len > g_download_total)
    {
        Send_Negative_Response(0x36, NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    // ── 写入 Flash ──
    uint32_t write_addr = g_download_addr + g_bytes_received;
    if (!Flash_If_Write_Byte(write_addr, data_ptr, data_len))
    {
        Send_Negative_Response(0x36, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    // ── 软件 CRC32 累积 (标准 Ethernet CRC32, 位操作版, 不依赖查表) ──
    for (uint16_t i = 0; i < data_len; i++)
    {
        g_running_crc ^= data_ptr[i];
        for (uint8_t j = 0; j < 8; j++)
            g_running_crc = (g_running_crc & 1) 
                ? ((g_running_crc >> 1) ^ 0xEDB88320)
                : (g_running_crc >> 1);
    }

    // ── 更新传输状态 ──
    g_bytes_received += data_len;
    g_expected_block_seq = (block_seq == 0xFF) ? 0x00 : (block_seq + 1);
    g_transfer_state = TRANSFER_STATE_IN_PROGRESS;

    // ── 肯定响应: 76 + blockSequenceCounter ──
    uint8_t resp[1] = { block_seq };
    Send_Positive_Response(0x36, 0, resp, 1);
}

// ==========================================
// 37 服务 (请求退出传输) - OEM 车规级
// 参照: ISO 14229-1 §14.3
// 请求格式: 37 + transferRequestParameterRecord[] (可选 CRC)
// ==========================================
void UDS_Service_37_Boot(uint8_t *payload, uint16_t length)
{
    if (g_current_session != UDS_SESSION_PROGRAMMING)
    {
        Send_Negative_Response(0x37, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    if (g_transfer_state == TRANSFER_STATE_IDLE)
    {
        Send_Negative_Response(0x37, NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    if (g_bytes_received != g_download_total)
    {
        Send_Negative_Response(0x37, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    if (length >= 5)
    {
        uint32_t expected_crc = ((uint32_t)payload[1] << 24) |
                                ((uint32_t)payload[2] << 16) |
                                ((uint32_t)payload[3] << 8)  |
                                ((uint32_t)payload[4]);
        uint32_t actual_crc = g_running_crc ^ 0xFFFFFFFF;

        if (expected_crc != actual_crc)
        {
            Send_Negative_Response(0x37, NRC_REQUEST_OUT_OF_RANGE);
            return;
        }
    }

    // ── 重置传输状态 ──
    g_transfer_state     = TRANSFER_STATE_IDLE;
    g_download_addr      = 0;
    g_download_total     = 0;
    g_bytes_received     = 0;
    g_expected_block_seq = 1;

    // ── 写入固件签名: 告诉上电时的 Bootloader "APP 是完整的！" ──
    if (!Flash_If_Write_Signature())
    {
        // 签名写入失败 — 下次上电会进 Bootloader，不会跳残缺 APP
        // 这里先回肯定响应，诊断仪可以重试刷写
        printf("WARN: 签名写入失败!\r\n");
    }

    // ── 肯定响应: 77 (无附加参数) ──
    Send_Positive_Response(0x37, 0, NULL, 0);
}
void Process_UDS_Request(uint8_t *payload, uint16_t length) 
{
    // 1. 安全校验：哪怕是最短的 UDS 请求，也至少要有 1 个字节 (SID)
    if (length == 0 || payload == NULL) return;

    // 2. 提取服务 ID (SID 永远是包裹的第 0 个字节)
    uint8_t sid = payload[0]; 

    // 3. 开始调度分发 (超级状态机)
    switch (sid) 
    {
        case UDS_SID_DIAGNOSTIC_SESSION_CONTROL: // 0x10 服务
            UDS_Service_10_Boot(payload, length);
            break;

        case UDS_SID_ECU_RESET:                  // 0x11 服务 (ECU复位)
            UDS_Service_11_Boot(payload, length);
            break;

        case UDS_SID_READ_DATA_BY_IDENTIFIER:    // 0x22 服务
            UDS_Service_22_App(payload, length);
            break;

        case UDS_SID_SECURITY_ACCESS:            // 0x27 服务 (安全访问)
            UDS_Service_27_Boot(payload, length);
            break;

        case UDS_SID_ROUTINE_CONTROL:            // 0x31 服务 (例行程序: Flash擦除等)
            UDS_Service_31_Boot(payload, length);
            break;

        case UDS_SID_REQUEST_DOWNLOAD:           // 0x34 服务 (请求下载)
            UDS_Service_34_Boot(payload, length);
            break;

        case UDS_SID_TRANSFER_DATA:              // 0x36 服务 (传输数据)
            UDS_Service_36_Boot(payload, length);
            break;

        case UDS_SID_REQUEST_TRANSFER_EXIT:      // 0x37 服务 (退出传输)
            UDS_Service_37_Boot(payload, length);
            break;
            
        case UDS_SID_TESTER_PRESENT:             // 0x3E 服务 (心跳报文)
            Send_Positive_Response(UDS_SID_TESTER_PRESENT, 0, NULL, 0);
            break;
            
        default:
            Send_Negative_Response(sid, NRC_SERVICE_NOT_SUPPORTED);
            break;
    }
}
