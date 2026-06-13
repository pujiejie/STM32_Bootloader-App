#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Flash 物理布局 (STM32F407VE, 1MB)
// ============================================================================
#define FLASH_BASE              0x08000000
#define FLASH_TOTAL_SIZE        0x00100000  // 1MB

// Bootloader 保护区 (Sector 0-3, 64KB)
// ⚠️ 绝不擦写！擦写会导致变砖！
#define BOOTLOADER_START_ADDR   0x08000000
#define BOOTLOADER_SIZE         0x00010000  // 64KB
#define BOOTLOADER_END_ADDR     0x0800FFFF

// APP 可擦写区 (Sector 4-11, 960KB)
#define APP_FLASH_START_ADDR    0x08010000
#define APP_FLASH_SIZE          0x000F0000  // 960KB
#define APP_FLASH_END_ADDR      0x080FFFFF

// Sector 编号
#define APP_SECTOR_START        4
#define APP_SECTOR_END          11
#define APP_SECTOR_COUNT        8

// Flash 编程最小单位 (STM32F4: 32-bit word)
#define FLASH_WORD_SIZE         4

// ══════════════════════════════════════════════════════════════════════
// APP 固件签名 (防半刷变砖)
// 原理: 签名随 APP 区一起擦除 → 0xFFFFFFFF
//       只有0x37退出传输时才写入 magic → 签名有效
//       刷写到一半断电/重启 → 签名仍是擦除态 → Bootloader 拒跳
// ══════════════════════════════════════════════════════════════════════
#define APP_SIGNATURE_ADDR      0x080FFFF8  // APP 区末尾倒数第8字节
#define APP_SIGNATURE_MAGIC     0xB00710AD  // "Bootload" 谐音魔数

// ============================================================================
// API 函数声明
// ============================================================================

/**
 * @brief  根据地址计算所属的 Flash Sector 编号
 * @param  addr  Flash 地址 (0x08000000 ~ 0x080FFFFF)
 * @return Sector 编号 (0 ~ 11)，地址非法返回 0xFF
 */
uint8_t Flash_If_GetSector(uint32_t addr);

/**
 * @brief  擦除 APP 区域全部 (Sector 4~11)
 *         保护 Bootloader 不误擦
 * @return true=成功, false=失败
 */
bool Flash_If_Erase_App_Area(void);

/**
 * @brief  按地址范围擦除对应 Sector
 * @param  start_addr  起始地址 (必须 >= APP_FLASH_START_ADDR)
 * @param  size        擦除字节数
 * @return true=成功, false=失败
 */
bool Flash_If_Erase_Sectors(uint32_t start_addr, uint32_t size);

/**
 * @brief  按字 (Word) 写入 Flash
 * @param  dest_addr   目标地址 (必须 Word 对齐)
 * @param  src_data    源数据指针
 * @param  word_count  写入的 Word 数量
 * @return true=成功, false=失败
 */
bool Flash_If_Write(uint32_t dest_addr, uint32_t *src_data, uint32_t word_count);

/**
 * @brief  按字节写入 Flash (内部处理对齐)
 * @param  dest_addr   目标地址
 * @param  src_data    源数据指针
 * @param  byte_count  写入字节数
 * @return true=成功, false=失败
 */
bool Flash_If_Write_Byte(uint32_t dest_addr, uint8_t *src_data, uint32_t byte_count);

/**
 * @brief  CRC32 校验 (Ethernet/PKZIP 多项式 0x04C11DB7)
 * @param  data   数据指针
 * @param  len    数据长度 (字节)
 * @return CRC32 值
 */
uint32_t Flash_If_CRC32(const uint8_t *data, uint32_t len);
extern const uint32_t crc32_table[256];

/**
 * @brief  在 APP 签名区写入有效签名 (刷写完成后调用)
 * @return true=写入成功, false=写入失败
 */
bool Flash_If_Write_Signature(void);

/**
 * @brief  验证 APP 签名是否有效 (上电跳转前调用)
 * @return true=签名有效, false=签名无效(APP不完整/被擦除/未刷写)
 */
bool Flash_If_Verify_Signature(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_IF_H */
