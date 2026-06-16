/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "lwip.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uds.h"
#include "isotp.h"
#include "lcd.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "doip.h"
#include "nm.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// 全局 UI 控件指针
static lv_obj_t *label_soc_val;
static lv_obj_t *label_voltage_val;
static lv_obj_t *label_current_val;
static lv_obj_t *label_status_val;
static lv_obj_t *label_dtc_bar;
static lv_obj_t *label_can_cnt;
static lv_obj_t *label_soh_val;
static lv_obj_t *arc_soc;

// ─── 辅助函数: KV 行 ───
static void info_kv(lv_obj_t *parent, const char *key, const char *val, lv_obj_t **val_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, lv_color_hex(0xF8FAFC), 0); // ← 白色！之前 0x1E293B=背景色导致看不见
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    if (val_label) *val_label = v;
}

// ─── 主 UI 构建 ───
void GUI_Build_Dashboard(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ==========================================
    // 1. 顶部标题栏 (48px)
    // ==========================================
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 800, 46);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 品牌 Logo
    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "FCXC@TeacherOrange");
    lv_obj_set_style_text_color(brand, lv_color_hex(0xF97316), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_16, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 16, 0);

    // separator
    lv_obj_t *sep = lv_label_create(header);
    lv_label_set_text(sep, "|");
    lv_obj_set_style_text_color(sep, lv_color_hex(0x475569), 0);
    lv_obj_align_to(sep, brand, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // subtitle
    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "BMS Diag v2.1");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_align_to(subtitle, sep, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // CAN 状态指示
    lv_obj_t *led_can = lv_led_create(header);
    lv_obj_set_size(led_can, 10, 10);
    lv_led_set_color(led_can, lv_palette_main(LV_PALETTE_GREEN));
    lv_led_on(led_can);
    lv_obj_align(led_can, LV_ALIGN_RIGHT_MID, -140, 0);

    lv_obj_t *lbl_can = lv_label_create(header);
    lv_label_set_text(lbl_can, "CAN");
    lv_obj_set_style_text_color(lbl_can, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_text_font(lbl_can, &lv_font_montserrat_12, 0);
    lv_obj_align_to(lbl_can, led_can, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // UDS 状态指示
    lv_obj_t *led_uds = lv_led_create(header);
    lv_obj_set_size(led_uds, 10, 10);
    lv_led_set_color(led_uds, lv_palette_main(LV_PALETTE_GREEN));
    lv_led_on(led_uds);
    lv_obj_align_to(led_uds, lbl_can, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    lv_obj_t *lbl_uds = lv_label_create(header);
    lv_label_set_text(lbl_uds, "UDS");
    lv_obj_set_style_text_color(lbl_uds, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_text_font(lbl_uds, &lv_font_montserrat_12, 0);
    lv_obj_align_to(lbl_uds, led_uds, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    // ==========================================
    // 2. 主内容区 (380px) — 左侧仪表盘 + 右侧数据
    // ==========================================
    lv_obj_t *main_area = lv_obj_create(scr);
    lv_obj_set_size(main_area, 800, 374);
    lv_obj_align(main_area, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(main_area, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 12, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    // ─── 2a. 左侧面板: SOC 仪表盘 (290px 宽) ───
    lv_obj_t *left_panel = lv_obj_create(main_area);
    lv_obj_set_size(left_panel, 290, 348);
    lv_obj_align(left_panel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(left_panel, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_set_style_radius(left_panel, 12, 0);
    lv_obj_set_style_pad_all(left_panel, 12, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    // SOC title
    lv_obj_t *soc_title = lv_label_create(left_panel);
    lv_label_set_text(soc_title, "SOC");
    lv_obj_set_style_text_color(soc_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(soc_title, &lv_font_montserrat_14, 0);
    lv_obj_align(soc_title, LV_ALIGN_TOP_MID, 0, 4);

    // SOC Arc 仪表
    arc_soc = lv_arc_create(left_panel);
    lv_obj_set_size(arc_soc, 180, 180);
    lv_obj_align(arc_soc, LV_ALIGN_TOP_MID, 0, 30);
    lv_arc_set_range(arc_soc, 0, 100);
    lv_arc_set_value(arc_soc, 78);
    lv_arc_set_bg_angles(arc_soc, 135, 45);
    lv_arc_set_rotation(arc_soc, 135);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_soc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_soc, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_soc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_soc, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc_soc, LV_OBJ_FLAG_CLICKABLE);

    // SOC 百分比数字
    label_soc_val = lv_label_create(left_panel);
    lv_label_set_text(label_soc_val, "78%");
    lv_obj_set_style_text_color(label_soc_val, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(label_soc_val, &lv_font_montserrat_32, 0);
    lv_obj_center(label_soc_val);
    lv_obj_set_y(label_soc_val, -90);

    // SOH 显示
    label_soh_val = lv_label_create(left_panel);
    lv_label_set_text(label_soh_val, "SOH: 95%");
    lv_obj_set_style_text_color(label_soh_val, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(label_soh_val, &lv_font_montserrat_12, 0);
    lv_obj_align(label_soh_val, LV_ALIGN_BOTTOM_MID, 0, -50);

    // 电芯数
    lv_obj_t *lbl_cells = lv_label_create(left_panel);
    lv_label_set_text(lbl_cells, "Cells: 4S");
    lv_obj_set_style_text_color(lbl_cells, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(lbl_cells, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_cells, LV_ALIGN_BOTTOM_MID, 0, -32);

    // ─── 2b. 右侧面板: 数据详情 (480px 宽) ───
    lv_obj_t *right_panel = lv_obj_create(main_area);
    lv_obj_set_size(right_panel, 480, 348);
    lv_obj_align(right_panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(right_panel, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(right_panel, 0, 0);
    lv_obj_set_style_radius(right_panel, 12, 0);
    lv_obj_set_style_pad_all(right_panel, 16, 0);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 使用 Flex 列布局
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 分区标题
    lv_obj_t *sec_title = lv_label_create(right_panel);
    lv_label_set_text(sec_title, "  Battery Live Data");
    lv_obj_set_style_text_color(sec_title, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(sec_title, &lv_font_montserrat_16, 0);
    lv_obj_set_width(sec_title, lv_pct(100));

    // 分隔线
    lv_obj_t *line = lv_obj_create(right_panel);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    // 数据行 (第3个参数是指针，用于后续动态刷新)
    info_kv(right_panel, "Total Volt",   "12.60 V",  &label_voltage_val);
    info_kv(right_panel, "Current",      "-2.3 A",   &label_current_val);
    info_kv(right_panel, "Cell Max",     "3.65 V",   NULL);
    info_kv(right_panel, "Cell Min",     "3.58 V",   NULL);
    info_kv(right_panel, "Temp Max",     "35 C",     NULL);
    info_kv(right_panel, "Temp Min",     "28 C",     NULL);
    info_kv(right_panel, "Chg/Dischg",   "Dischg",   &label_status_val);

    // ─── 分隔线 ───
    lv_obj_t *div2 = lv_obj_create(right_panel);
    lv_obj_set_size(div2, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div2, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(div2, 0, 0);

    // ==========================================
    // 3. 底部状态栏 (60px) — DTC 列表 + CAN 计数
    // ==========================================
    lv_obj_t *bottom = lv_obj_create(scr);
    lv_obj_set_size(bottom, 800, 60);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_radius(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 8, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    // DTC 标签
    lv_obj_t *dtc_title = lv_label_create(bottom);
    lv_label_set_text(dtc_title, "DTC:");
    lv_obj_set_style_text_color(dtc_title, lv_color_hex(0xF97316), 0);
    lv_obj_set_style_text_font(dtc_title, &lv_font_montserrat_12, 0);
    lv_obj_align(dtc_title, LV_ALIGN_LEFT_MID, 0, 0);

    // DTC 内容
    label_dtc_bar = lv_label_create(bottom);
    lv_label_set_text(label_dtc_bar, "0xF100 [ACT]  0xF101  0xF102  0xF103");
    lv_obj_set_style_text_color(label_dtc_bar, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(label_dtc_bar, &lv_font_montserrat_12, 0);
    lv_obj_align_to(label_dtc_bar, dtc_title, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // CAN 计数
    label_can_cnt = lv_label_create(bottom);
    lv_label_set_text(label_can_cnt, "CAN: 0");
    lv_obj_set_style_text_color(label_can_cnt, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(label_can_cnt, &lv_font_montserrat_12, 0);
    lv_obj_align(label_can_cnt, LV_ALIGN_RIGHT_MID, 0, 0);
}

// ─── UI 数据刷新 (每500ms 调用) ───
void GUI_Refresh_BMS(void)
{
    char buf[32];

    // SOC
    snprintf(buf, sizeof(buf), "%d%%", g_bms_soc);
    lv_label_set_text(label_soc_val, buf);
    lv_arc_set_value(arc_soc, g_bms_soc);

    // SOH
    snprintf(buf, sizeof(buf), "SOH: %d%%", g_bms_soh);
    lv_label_set_text(label_soh_val, buf);

    // 电压 (整数运算，避免 Keil V5 不支持浮点 snprintf)
    snprintf(buf, sizeof(buf), "%d.%02d V",
             (int)(g_bms_voltage / 1000), (int)(g_bms_voltage % 1000));
    lv_label_set_text(label_voltage_val, buf);

    // 电流 (带符号)
    {
        int16_t abs_ma = (g_bms_current < 0) ? (-g_bms_current) : g_bms_current;
        snprintf(buf, sizeof(buf), "%s%d.%d A",
                 (g_bms_current < 0) ? "-" : "",
                 (int)(abs_ma / 1000), (int)((abs_ma % 1000) / 100));
    }
    lv_label_set_text(label_current_val, buf);

    // 状态
    const char *status_str = (g_bms_status == 1) ? "Charging" :
                             (g_bms_status == 2) ? "Discharging" : "Idle";
    lv_label_set_text(label_status_val, status_str);

    // DTC
    char dtc_buf[128] = "";
    for (uint8_t i = 0; i < g_dtc_count; i++) {
        char dtc_entry[32];
        if (g_dtc_list[i].active) {
            snprintf(dtc_entry, sizeof(dtc_entry), "0x%04lX [ACT]  ", g_dtc_list[i].dtc_code & 0xFFFFFF);
        } else {
            snprintf(dtc_entry, sizeof(dtc_entry), "0x%04lX  ", g_dtc_list[i].dtc_code & 0xFFFFFF);
        }
        strcat(dtc_buf, dtc_entry);
    }
    if (g_dtc_count == 0) {
        strcpy(dtc_buf, "No DTC");
    }
    lv_label_set_text(label_dtc_bar, dtc_buf);

    // CAN
    snprintf(buf, sizeof(buf), "CAN: %lu", g_can_rx_count);
    lv_label_set_text(label_can_cnt, buf);
}

// ─── printf 重定向 ───
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  SCB->VTOR = FLASH_BASE | 0x10000;
  __enable_irq();
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN1_Init();
  MX_USART1_UART_Init();
  MX_FSMC_Init();
  MX_LWIP_Init();
  /* USER CODE BEGIN 2 */

  /* DOIP TCP/UDP server */
  DOIP_Init();

  /* CAN 网络管理 */
  NM_Init();

  /* ─── YT8512C MAC 补丁: 手动纠正速度和双工 ─── */
  {
      extern ETH_HandleTypeDef heth;
      uint32_t phyreg = 0;
      HAL_ETH_ReadPHYRegister(&heth, 0x00, 0x11, &phyreg);

      ETH_MACConfigTypeDef mac_cfg;
      HAL_ETH_GetMACConfig(&heth, &mac_cfg);

      /* YT8512C 寄存器 0x11 Bit14=速度(1=100M), Bit13=双工(1=全双工) */
      mac_cfg.Speed      = (phyreg & 0x4000) ? ETH_SPEED_100M  : ETH_SPEED_10M;
      mac_cfg.DuplexMode = (phyreg & 0x2000) ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;

      HAL_ETH_SetMACConfig(&heth, &mac_cfg);
      printf("[PATCH] PHY=0x%04lX, Speed=%s, Duplex=%s\r\n",
             phyreg,
             (mac_cfg.Speed == ETH_SPEED_100M) ? "100M" : "10M",
             (mac_cfg.DuplexMode == ETH_FULLDUPLEX_MODE) ? "FULL" : "HALF");
  }

  uint32_t led_last_tick  = HAL_GetTick();
  uint32_t lvgl_last_tick = HAL_GetTick();
  uint32_t ui_refresh_tick = HAL_GetTick();
  uint32_t dbg_tick = HAL_GetTick();
  uint32_t current_tick;

  lcd_init();
  lv_init();
  lv_port_disp_init();
  GUI_Build_Dashboard();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    current_tick = HAL_GetTick();

    // ─── 任务1: UDS/ISOTP 处理（最高优先级）───
    if (uds_rx_flag == 1)
    {
        // 临界区: 关中断 → 拷贝 → 清标志 → 开中断 (防 ISR 中途覆盖)
        uint8_t rx_copy[8];
        __disable_irq();
        for (uint8_t i = 0; i < 8; i++) rx_copy[i] = UDS_RxData[i];
        uds_rx_flag = 0;
        __enable_irq();
        g_can_rx_count++;
        ISOTP_Receive_Handler(rx_copy);
    }

    // ─── 任务2: LVGL 事务 (每5ms) ───
    if ((current_tick - lvgl_last_tick) >= 5)
    {
        lv_tick_inc(current_tick - lvgl_last_tick);
        lv_timer_handler();
        lvgl_last_tick = current_tick;
    }

    // ─── 任务3: UI 数据刷新 (每500ms) ───
    if ((current_tick - ui_refresh_tick) >= 500)
    {
        GUI_Refresh_BMS();
        ui_refresh_tick = current_tick;
    }

    // ─── 任务4: 心跳 LED (每100ms) ───
    if ((current_tick - led_last_tick) >= 100)
    {
        HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10);
        led_last_tick = current_tick;
    }

    // ─── 任务5: LwIP 协议栈轮询 (每周期执行, 内部非阻塞) ───
    MX_LWIP_Process();

    // ─── DOIP TCP 回调(在 ethernetif_input 中已处理, 这里做超时等) ───
    DOIP_Process();

    // ─── 网络管理: 周期性发送 NM PDU + 离线检测 ───
    NM_Process();

    // ─── 诊断: 每2秒打印网络状态 ───
    extern volatile uint8_t  g_eth_link_up;
    extern volatile uint32_t g_eth_irq_cnt;
    if ((current_tick - dbg_tick) >= 2000)
    {
        dbg_tick = current_tick;
        printf("[NET] tick=%lu link=%d eth_irq=%lu\r\n",
               current_tick, g_eth_link_up, g_eth_irq_cnt);
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
