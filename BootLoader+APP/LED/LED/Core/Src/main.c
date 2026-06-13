/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdint.h"
#include "uds.h"
#include "isotp.h"
#include "flash_if.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SHARED_RAM_MAGIC_ADDR  0x2001FFFC 
#define ENTER_BOOTLOADER_MAGIC 0xDEADBEEF 
#define APP_START_ADDRESS      0x08010000
typedef void (*pFunction)(void);
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
#include <stdio.h>

// 重定向 printf 的底层函数
int fputc(int ch, FILE *f)
{
		// 把字符通过串口1发送出去
		HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
		return ch;
}
void Jump_To_App(void)
{
    // 0. 固件签名校验: 防止跳入传输一半的残缺 APP
    if (!Flash_If_Verify_Signature())
    {
        printf("APP签名无效! 留在Bootloader等待刷写...\r\n");
        return; // 签名未写入或已被擦除 → APP 无效 → 死守 Bootloader
    }

    // 1. 检查 APP 存放地址有没有合法的栈顶指针 (防变砖极其重要)
    if (((*(__IO uint32_t*)APP_START_ADDRESS) & 0x2FFE0000) == 0x20000000)
    {
        // 2. 封印一切！关闭所有中断，把外设恢复到上电默认状态
        HAL_RCC_DeInit();
        HAL_DeInit();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL = 0;
        __disable_irq();

        // 3. 获取 APP 的 Reset_Handler 地址 (在首地址偏移 4 字节处)
        uint32_t jump_addr = *(__IO uint32_t*)(APP_START_ADDRESS + 4);
        pFunction JumpToApplication = (pFunction)jump_addr;

        // 4. 重置栈顶指针
        __set_MSP(*(__IO uint32_t*)APP_START_ADDRESS);

        // 5. 信仰一跃！跳进 APP！
        JumpToApplication();
    }
    else
    {
        // 如果发现 APP 区域是空的（或者损坏了），绝对不能跳！
        // 留在 Bootloader 里，点亮一个红灯报警，或者通过串口打印错误
			printf("APP区域空着%d", 10);
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
	// 1. 第一时间！开启电源和后备区域时钟
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();
	
	// 2. 从绝对安全的物理寄存器读取暗号
	uint32_t magic_word = RTC->BKP0R;

	if (magic_word != 0xDEADBEEF)
	{
			// 没有暗号，正常上电，极速跳转！
			Jump_To_App(); 
	}
	else 
	{
			// 收到暗号，进 Boot！擦除暗号！
			RTC->BKP0R = 0x00000000;
	}
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
  /* USER CODE BEGIN 2 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	// Bootloader 唯一使命是刷写 → 直接进编程会话
	extern uint8_t g_current_session;
	g_current_session = 0x02; // 跳过发 10 02 步骤
	
	uint32_t s3_timeout_last_tick = HAL_GetTick();
  while (1)
  {
    if (uds_rx_flag == 1)
		{
		    // 临界区: 关中断 → 拷贝 → 清标志 → 开中断
				uint8_t rx_copy[8];
				__disable_irq();
				for (uint8_t i = 0; i < 8; i++) rx_copy[i] = UDS_RxData[i];
				uds_rx_flag = 0;
				__enable_irq();
				
				// 收到任何 UDS 报文，视为通信活跃，重置 S3 计时器 (续命 10 秒)
				s3_timeout_last_tick = HAL_GetTick();
				
				// 调用你复制过来的 ISO-TP 接收处理函数
				ISOTP_Receive_Handler(rx_copy);
		}

		// 2. S3 超时监控逻辑 (30秒，覆盖手动操盘+擦除盲区)
		if ((HAL_GetTick() - s3_timeout_last_tick) >= 30000)
		{
				Jump_To_App();
				// 如果 Jump_To_App 返回 (APP 是空的), 重置定时器，不再死锁
				s3_timeout_last_tick = HAL_GetTick();
		}

		// 3. 非阻塞指示灯 (证明 Bootloader 在运行，而不是死机了)
		static uint32_t led_tick = 0;
		if ((HAL_GetTick() - led_tick) >= 500)
		{
				led_tick = HAL_GetTick();
				HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9);
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
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
