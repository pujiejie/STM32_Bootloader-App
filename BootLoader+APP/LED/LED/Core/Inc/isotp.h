#ifndef __ISOTP_H
#define __ISOTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

// =======================================================
// 定义网络层大仓库的容量 
// (Bootloader 刷写时，一次 36 服务的包通常是 1024 字节左右)
// =======================================================
#define ISOTP_RX_BUFFER_SIZE  2048 

// 对外开放的接口函数
void ISOTP_Receive_Handler(uint8_t *can_data);
void ISOTP_Send_FlowControl(void);
void ISOTP_Send_Data(uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __ISOTP_H */
