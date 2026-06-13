#include "isotp.h"
#include "uds.h"
#include "can.h"

uint8_t  isotp_rx_buffer[ISOTP_RX_BUFFER_SIZE]; // 大仓库
uint16_t isotp_total_len = 0;                   // 记录总长度
uint16_t isotp_rx_index = 0;                    // 记录已经抄写了多少个字节
uint8_t  isotp_expected_sn = 1;                 // 期待的下一个连续帧序号(SN)

uint8_t  isotp_tx_buffer[ISOTP_RX_BUFFER_SIZE]; // 发货大仓库
uint16_t isotp_tx_len = 0;                      // 要发多少货
uint16_t isotp_tx_index = 0;                    // 已经发了多少
uint8_t  isotp_tx_sn = 1;                       // 发送连续帧的序号

void ISOTP_Send_Data(uint8_t *payload, uint16_t len)
{
    CAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA}; // 默认填充 0xAA
    uint32_t TxMailbox;

    TxHeader.StdId = 0x7E8; // 单片机的默认响应 ID
    TxHeader.RTR = CAN_RTR_DATA;
    TxHeader.IDE = CAN_ID_STD;
    TxHeader.DLC = 8;

    if (len <= 7) 
    {
        // ------------------------------------------------
        // 数据很少 (<=7 字节)，直接用 单帧 (SF) 秒发
        // ------------------------------------------------
        TxData[0] = len; // 高4位为0代表单帧，低4位是长度
        for (uint8_t i = 0; i < len; i++) {
            TxData[1 + i] = payload[i];
        }
        HAL_CAN_AddTxMessage(&hcan1, &TxHeader, TxData, &TxMailbox);
    } 
    else 
    {
        // ------------------------------------------------
        // 数据很大 (>7 字节)，发送 首帧 (FF)，剩下的存进仓库
        // ------------------------------------------------
        // 1. 发送首帧 (包含前 6 个字节的数据)
        TxData[0] = 0x10 | ((len >> 8) & 0x0F); // 高4位为1(首帧)，低4位为长度的高位
        TxData[1] = (uint8_t)(len & 0xFF);      // 长度的低8位
        
        for (uint8_t i = 0; i < 6; i++) {
            TxData[2 + i] = payload[i];
        }
        HAL_CAN_AddTxMessage(&hcan1, &TxHeader, TxData, &TxMailbox);

        // 2. 把剩下的数据存进发货仓库，等待对方发来 流控帧 (FC)
        isotp_tx_len = len;
        isotp_tx_index = 6; // 已经发了头6个字节了
        isotp_tx_sn = 1;    // 接下来连续帧从序号 1 开始
        
        for (uint16_t i = 6; i < len; i++) {
            isotp_tx_buffer[i] = payload[i];
        }
    }
}
// =======================================================
// 函数：主动发送流控帧 (Flow Control)
// 作用：告诉诊断仪“我准备好了，请开始发送碎片数据吧！”
// =======================================================
void ISOTP_Send_FlowControl(void)
{
    CAN_TxHeaderTypeDef TxHeader;
    uint8_t TxData[8];
    uint32_t TxMailbox;

    TxHeader.StdId = 0x7E8;      // 单片机的回复 ID
    TxHeader.RTR = CAN_RTR_DATA;
    TxHeader.IDE = CAN_ID_STD;
    TxHeader.DLC = 8;

    // 流控帧 (FC) 固定格式
    TxData[0] = 0x30; // 3代表FC，0代表 Clear To Send (允许发送)
    TxData[1] = 0x00; // Block Size = 0 (不限制发包数量，一口气发完)
    TxData[2] = 0x00; // STmin = 0 (诊断仪尽量快发, CAN 500kbps 极限 ~31KB/s)
    
    // 后面全部用 0xAA 填充
    TxData[3] = 0xAA; TxData[4] = 0xAA; TxData[5] = 0xAA; TxData[6] = 0xAA; TxData[7] = 0xAA;

    HAL_CAN_AddTxMessage(&hcan1, &TxHeader, TxData, &TxMailbox);
}

// =======================================================
// 函数：网络层接收总机 (放进主循环里被调用)
// 参数：传入从 CAN 硬件里读出的那 8 个字节
// =======================================================
void ISOTP_Receive_Handler(uint8_t *can_data)
{
    // 提取网络层帧类型 (第0字节高4位)
    uint8_t frame_type = (can_data[0] >> 4);

    switch (frame_type)
    {
        // -----------------------------------------------------
        // 1. 单帧 (Single Frame)
        // -----------------------------------------------------
        case 0x00: 
        {
            uint8_t data_len = can_data[0] & 0x0F; // 提取有效长度
            
            if (data_len > 0 && data_len <= 7)
            {
                // 把有效数据搬到大仓库的最前面
                for (uint8_t i = 0; i < data_len; i++) {
                    isotp_rx_buffer[i] = can_data[1 + i];
                }
                
                // 单帧一个包就发完了，直接扔给 UDS 去处理！
                Process_UDS_Request(isotp_rx_buffer, data_len);
            }
            break;
        }

        // -----------------------------------------------------
        // 2. 首帧 (First Frame) - 超大包裹的预告单
        // -----------------------------------------------------
        case 0x01: 
        {
            // 拼凑大包裹的总长度 (位移拼接魔法)
            isotp_total_len = ((can_data[0] & 0x0F) << 8) | can_data[1];
            
            // 安全性校验：超大包裹总长度必须大于7，且不能超过我们的仓库容量
            if (isotp_total_len > 7 && isotp_total_len <= ISOTP_RX_BUFFER_SIZE)
            {
                // 把首帧附带的头 6 个字节真数据放进仓库
                for (uint8_t i = 0; i < 6; i++) {
                    isotp_rx_buffer[i] = can_data[2 + i];
                }
                
                isotp_rx_index = 6;       // 进度条推进 6 格
                isotp_expected_sn = 1;    // 期待下一个包裹的序号是 1
                
                // 🔥 绝杀操作：立刻回发流控帧！让诊断仪继续发货！
                ISOTP_Send_FlowControl();
            }
            else
            {
                // 如果对端发来的包比我们的仓库还大，直接重置进度条，丢弃处理
                isotp_total_len = 0;
                isotp_rx_index = 0;
            }
            break;
        }

        // -----------------------------------------------------
        // 3. 连续帧 (Consecutive Frame) - 疯狂拼接碎片
        // -----------------------------------------------------
        case 0x02: 
        {
            uint8_t current_sn = can_data[0] & 0x0F; // 提取当前序号
            
            // 校验：是不是我期待的那个包裹？(防丢包)
            if (current_sn == isotp_expected_sn)
            {
                // 计算这包还要抄写几个字节 (通常是7，最后一包可能少于7)
                uint16_t bytes_left = isotp_total_len - isotp_rx_index;
                uint8_t copy_len = (bytes_left >= 7) ? 7 : bytes_left;
                
                // 疯狂搬运，拼接到大仓库的尾部
                for (uint8_t i = 0; i < copy_len; i++) {
                    isotp_rx_buffer[isotp_rx_index++] = can_data[1 + i];
                }
                
                // 更新期待的下一个序号 (如果到了 15 就轮回成 0)
                isotp_expected_sn++;
                if (isotp_expected_sn > 15) {
                    isotp_expected_sn = 0;
                }
                
                // 🏆 终极判断：我是不是全部拼完了？！
                if (isotp_rx_index >= isotp_total_len)
                {
                    // 拼装完成！直接把大仓库丢给 UDS 去处理！
                    Process_UDS_Request(isotp_rx_buffer, isotp_total_len);
                    
                    // 打扫战场，为下一次接收大包裹做准备
                    isotp_total_len = 0;
                    isotp_rx_index = 0;
                }
            }
            else
            {
                // 🚨 完蛋，序号对不上！说明总线丢包了，整个大包裹作废重来！
                isotp_total_len = 0;
                isotp_rx_index = 0;
            }
            break;
        }

        // -----------------------------------------------------
        // 4. 流控帧 (Flow Control)
        // -----------------------------------------------------
        case 0x03:
            // 我们收到了诊断仪发来的“绿灯”！可以开始疯狂发射剩下的连续帧了！
            // (企业级代码在这里要解析对端要求的块大小和时间间隔，为了上手实战，我们用最简单暴力的连发模式)
            while (isotp_tx_index < isotp_tx_len) 
            {
                CAN_TxHeaderTypeDef TxHeader;
                uint8_t TxData[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
                uint32_t TxMailbox;
                TxHeader.StdId = 0x7E8; TxHeader.RTR = CAN_RTR_DATA; TxHeader.IDE = CAN_ID_STD; TxHeader.DLC = 8;

                // 1. 打包连续帧的第 0 字节 (2代表连续帧，跟上序号)
                TxData[0] = 0x20 | (isotp_tx_sn & 0x0F);

                // 2. 计算这一包还能装几个字节 (最多装7个)
                uint16_t bytes_left = isotp_tx_len - isotp_tx_index;
                uint8_t copy_len = (bytes_left >= 7) ? 7 : bytes_left;

                // 3. 把剩下的货塞进去
                for (uint8_t i = 0; i < copy_len; i++) {
                    TxData[1 + i] = isotp_tx_buffer[isotp_tx_index++];
                }

                // 4. 发射！
                HAL_CAN_AddTxMessage(&hcan1, &TxHeader, TxData, &TxMailbox);

                // 5. 更新序号
                isotp_tx_sn++;
                if (isotp_tx_sn > 15) isotp_tx_sn = 0;

                // 🌟 绝杀细节：这里必须给单片机的邮箱留点喘息时间，同时也是为了满足诊断仪的接收速度。
                // 因为我们在前台 while(1) 里调用的网络层，所以用 HAL_Delay 是完全没问题的！
                HAL_Delay(10); 
            }
            break;
            
        default:
            break;
    }
}
