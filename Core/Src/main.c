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
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define EVT_SOUND      1
#define EVT_BUTTON     2
#define EVT_HEARTBEAT  3
#define EVT_LORA       4
#define SND_N          1024     /* 1024 samples / 8 kHz = 128 ms window */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

LPTIM_HandleTypeDef hlptim1;

UART_HandleTypeDef hlpuart1;
UART_HandleTypeDef huart2;

TIM_HandleTypeDef htim6;

/* Definitions for SentinelTask */
osThreadId_t SentinelTaskHandle;
const osThreadAttr_t SentinelTask_attributes = {
  .name = "SentinelTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for evtQueue */
osMessageQueueId_t evtQueueHandle;
const osMessageQueueAttr_t evtQueue_attributes = {
  .name = "evtQueue"
};
/* Definitions for heartbeatTimer */
osTimerId_t heartbeatTimerHandle;
const osTimerAttr_t heartbeatTimer_attributes = {
  .name = "heartbeatTimer"
};
/* USER CODE BEGIN PV */
uint32_t sound_count  = 0;
uint32_t button_count = 0;
uint32_t lora_count   = 0;
uint32_t false_alarm_count = 0;

/* Tickless statistics and Stop 2 lock (defined in tickless_lptim.c) */
extern volatile uint32_t tl_stop2_count, tl_timer_wakeups, tl_event_wakeups, tl_sleep_ms;
extern volatile uint8_t  tl_stop_lock;
extern volatile uint32_t tl_hold_until;

extern volatile uint32_t tl_dbg_busy, tl_dbg_hold, tl_dbg_lock;

/* LoRa receive (LPUART1, byte by byte) */
volatile uint32_t lora_byte_count  = 0;
volatile uint32_t lora_error_count = 0;
uint8_t  lora_rx_byte;          /* HAL writes each received byte here       */
char     lora_buf[64];          /* incoming line is assembled here          */
uint8_t  lora_len = 0;
char     lora_msg[64];          /* completed line, copied for the task      */

/* Sound samples (ADC1 via DMA) */
uint16_t snd_buf[SND_N];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_LPTIM1_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM6_Init(void);
void StartSentinelTask(void *argument);
void HeartbeatCallback(void *argument);

/* USER CODE BEGIN PFP */
void tickless_lptim_init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Redirect printf() to USART2 -> ST-LINK virtual COM port */
int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

/* EXTI callback: do no work in the ISR, just post an event to the queue
 * (timeout 0 = never block inside an interrupt). */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    uint8_t evt;
    if (GPIO_Pin == SOUND_DO_Pin) {
        HAL_NVIC_DisableIRQ(EXTI0_IRQn);   /* comparator chatter lockout */
        evt = EVT_SOUND;
    } else if (GPIO_Pin == B1_Pin) {
        evt = EVT_BUTTON;
    } else {
        return;
    }
    osMessageQueuePut(evtQueueHandle, &evt, 0, 0);
}

/* Main power: PA8 -> IRLZ44N -> load (LED). LD2 mirrors the state.
 * Turned ON by a wake event, stays ON until a "sleep" feedback
 * (LoRa command or button) or the safety timeout. */
#define POWER_MAX_MS   (10UL * 60UL * 1000UL)   /* safety: auto-off after 10 min */

static uint8_t  main_power_on    = 0;   /* current state: 1 = ON */
static uint32_t main_power_since = 0;   /* tick when power was turned ON */

static void main_power_set(uint8_t on, const char *reason)
{
    if (on == main_power_on) {
        printf("         main power already %s (%s)\r\n", on ? "ON" : "OFF", reason);
        return;
    }
    main_power_on = on;
    HAL_GPIO_WritePin(PWR_EN_GPIO_Port, PWR_EN_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LD2_GPIO_Port,    LD2_Pin,    on ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (on) {
        main_power_since = osKernelGetTickCount();
        printf("         MAIN POWER: ON  (%s)\r\n", reason);
    } else {
        printf("         MAIN POWER: OFF (%s, was on for %lu ms)\r\n",
               reason, osKernelGetTickCount() - main_power_since);
    }
}

void HAL_UARTEx_WakeupCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == LPUART1) {
    /* LoRa traffic started: stay out of Stop 2 until the whole message is in */
    tl_hold_until = osKernelGetTickCount() + 200;
  }
}

/* Called for every byte received on LPUART1.
 * In Stop 2 the start bit of the first byte wakes the MCU. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1) {
        lora_byte_count++;
        tl_hold_until = osKernelGetTickCount() + 200;   /* extend the 200 ms RX window, no Stop 2 */
        char c = (char)lora_rx_byte;
        if (c == '\n') {                                    /* end of line */
            lora_buf[lora_len] = '\0';
            memcpy(lora_msg, lora_buf, lora_len + 1);
            lora_len = 0;
            uint8_t evt = EVT_LORA;
            osMessageQueuePut(evtQueueHandle, &evt, 0, 0);
        } else if (c != '\r' && lora_len < sizeof(lora_buf) - 1) {
            lora_buf[lora_len++] = c;
        }
        HAL_UART_Receive_IT(&hlpuart1, &lora_rx_byte, 1);   /* arm for next byte */
    }
}

/* LPUART error flags (noise/framing): with a 32.768 kHz LSE kernel clock at
 * 9600 baud there are only ~3.4 clocks per bit, so these flags are raised
 * often although the byte itself is usually correct.
 * Do NOT clear the line buffer; only re-arm reception if it was stopped. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1) {
        lora_error_count++;
        if (huart->RxState == HAL_UART_STATE_READY) {
            HAL_UART_Receive_IT(&hlpuart1, &lora_rx_byte, 1);
        }
    }
}

/* DMA finished the 1024-sample buffer: wake the task */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        osThreadFlagsSet(SentinelTaskHandle, 0x01);
    }
}

/* ---- Sound classification: decision tree ---- */
typedef enum { SND_CLAP, SND_KNOCK, SND_MOTOR, SND_OTHER, SND_ERROR } sound_class_t;
static const char *class_name[] = { "CLAP", "KNOCK", "MOTOR", "OTHER", "ERROR" };

#define CREST_IMPULSE_X10  27   /* crest >= 2.7 -> impulsive sound (clap/knock)     */
#define CREST_MOTOR_X10    25   /* crest <= 2.5 -> sustained sound                  */
#define HF_CLAP            50   /* impulsive + HF >= 50 % -> clap, otherwise knock  */
#define HF_MOTOR           50   /* sustained + HF >= 50 % -> motor (speech is low)  */

/* Goertzel algorithm: power of a single frequency component
 * (equivalent to one DFT bin, computed with a 2nd-order resonator). */
static float goertzel(const uint16_t *x, int32_t mean, float f)
{
    float k = 2.0f * cosf(2.0f * 3.14159265f * f / 8000.0f);   /* fs = 8 kHz */
    float s1 = 0.0f, s2 = 0.0f;
    for (uint32_t i = 0; i < SND_N; i++) {
        float s0 = (float)((int32_t)x[i] - mean) + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

/* Sample the sensor's analog output for 128 ms, extract features, classify */
static sound_class_t sound_measure_classify(void)
{
    /* ---- 1. Sample (Stop 2 locked: ADC + DMA do not run in Stop 2) ---- */

    osThreadFlagsClear(0x01);
    tl_stop_lock = 1;
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)snd_buf, SND_N);
    HAL_TIM_Base_Start(&htim6);
    uint32_t r = osThreadFlagsWait(0x01, osFlagsWaitAny, 500);
    HAL_TIM_Base_Stop(&htim6);
    HAL_ADC_Stop_DMA(&hadc1);
    tl_stop_lock = 0;

    if (r & osFlagsError) {
        printf("         sound: sampling ERROR\r\n");
        return SND_ERROR;
    }

    /* ---- 2. Feature extraction (duration measured with DWT) ---- */
    uint32_t c0 = DWT->CYCCNT;

    uint32_t sum = 0;
    uint16_t mn = 4095, mx = 0;
    for (uint32_t i = 0; i < SND_N; i++) {
        sum += snd_buf[i];
        if (snd_buf[i] < mn) mn = snd_buf[i];
        if (snd_buf[i] > mx) mx = snd_buf[i];
    }
    int32_t mean = (int32_t)(sum / SND_N);             /* DC offset */

    float energy = 0.0f;
    float block[16] = {0};                             /* envelope: 16 blocks x 8 ms */
    for (uint32_t i = 0; i < SND_N; i++) {
        float v = (float)((int32_t)snd_buf[i] - mean);
        energy += v * v;
        block[i / (SND_N / 16)] += v * v;
    }
    float bmax = 0.0f, bsum = 0.0f;
    for (int b = 0; b < 16; b++) {
        bsum += block[b];
        if (block[b] > bmax) bmax = block[b];
    }

    /* Frequency scan: 100..3000 Hz in 100 Hz steps */
    float pmax = 0.0f, p_low = 0.0f, p_high = 0.0f;
    uint32_t f_dom = 0;
    for (uint32_t f = 100; f <= 3000; f += 100) {
        float p = goertzel(snd_buf, mean, (float)f);
        if (p > pmax) { pmax = p; f_dom = f; }
        if (f < 1000) p_low += p; else p_high += p;
    }

    uint32_t rms_mv = (uint32_t)(sqrtf(energy / SND_N) * 3300.0f / 4095.0f);
    uint32_t pp_mv  = (uint32_t)(mx - mn) * 3300U / 4095U;
    uint32_t crest  = (bsum > 0.0f) ? (uint32_t)(10.0f * bmax / (bsum / 16.0f)) : 0;  /* x10 */
    uint32_t hf_pct = (uint32_t)(100.0f * p_high / (p_low + p_high + 1.0f));
    uint32_t cycles = DWT->CYCCNT - c0;

    printf("         sound: rms=%lu mV  pp=%lu mV  crest=%lu.%lu  f_dom=%lu Hz  hf=%lu%%  analysis=%lu cycles (%lu us)\r\n",
           rms_mv, pp_mv, crest / 10, crest % 10, f_dom, hf_pct, cycles, cycles / 4);

    /* ---- 3. Decision tree ---- */
    sound_class_t s;
    if (crest >= CREST_IMPULSE_X10)                         s = (hf_pct >= HF_CLAP) ? SND_CLAP : SND_KNOCK;
    else if (crest <= CREST_MOTOR_X10 && hf_pct >= HF_MOTOR) s = SND_MOTOR;
    else                                                    s = SND_OTHER;
    printf("         decision: %s\r\n", class_name[s]);
    return s;
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_LPTIM1_Init();
  MX_LPUART1_UART_Init();
  MX_ADC1_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */

  HAL_DBGMCU_DisableDBGStopMode();                            /* no debug clocks in Stop */
  __HAL_RCC_WAKEUPSTOP_CLK_CONFIG(RCC_STOP_WAKEUPCLOCK_MSI);  /* wake up on MSI 4 MHz    */
  tickless_lptim_init();

  /* LPUART1: keep running in Stop mode and wake up on START BIT
   * (method recommended by ST). Kernel clock must be LSE. */
  UART_WakeUpTypeDef wk = {0};
  wk.WakeUpEvent = UART_WAKEUP_ON_STARTBIT;
  HAL_UARTEx_StopModeWakeUpSourceConfig(&hlpuart1, wk);
  __HAL_UART_ENABLE_IT(&hlpuart1, UART_IT_WUF);              /* wake-up interrupt      */
  HAL_UARTEx_EnableStopMode(&hlpuart1);
  EXTI->IMR1 |= EXTI_IMR1_IM31;                               /* LPUART1 wake-up line   */
  HAL_StatusTypeDef rx_status = HAL_UART_Receive_IT(&hlpuart1, &lora_rx_byte, 1);

  printf("\r\n=== GHOST SENTINEL LITE - FreeRTOS tickless + LPTIM ===\r\n");
  printf("LPUART1 RX started: %s\r\n", (rx_status == HAL_OK) ? "OK" : "ERROR");
  printf("LPUART1 clock source: %s\r\n",
         (__HAL_RCC_GET_LPUART1_SOURCE() == RCC_LPUART1CLKSOURCE_LSE) ? "LSE (ok)" : "NOT LSE!");

  HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

  /* DWT cycle counter: used to measure the analysis time */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* creation of heartbeatTimer */
  heartbeatTimerHandle = osTimerNew(HeartbeatCallback, osTimerPeriodic, NULL, &heartbeatTimer_attributes);

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of evtQueue */
  evtQueueHandle = osMessageQueueNew (8, sizeof(uint8_t), &evtQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of SentinelTask */
  SentinelTaskHandle = osThreadNew(StartSentinelTask, NULL, &SentinelTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable MSI Auto calibration
  */
  HAL_RCCEx_EnableMSIPLLMode();
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief LPTIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPTIM1_Init(void)
{

  /* USER CODE BEGIN LPTIM1_Init 0 */

  /* USER CODE END LPTIM1_Init 0 */

  /* USER CODE BEGIN LPTIM1_Init 1 */

  /* USER CODE END LPTIM1_Init 1 */
  hlptim1.Instance = LPTIM1;
  hlptim1.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
  hlptim1.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV16;
  hlptim1.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
  hlptim1.Init.OutputPolarity = LPTIM_OUTPUTPOLARITY_HIGH;
  hlptim1.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
  hlptim1.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
  hlptim1.Init.Input1Source = LPTIM_INPUT1SOURCE_GPIO;
  hlptim1.Init.Input2Source = LPTIM_INPUT2SOURCE_GPIO;
  if (HAL_LPTIM_Init(&hlptim1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPTIM1_Init 2 */

  /* USER CODE END LPTIM1_Init 2 */

}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */

  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */

  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 4800;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */

  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief TIM6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM6_Init(void)
{

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 0;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 499;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */

  /* USER CODE END TIM6_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LD2_Pin|PWR_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PC2 PC3 PC4 PC5
                           PC6 PC7 PC8 PC9
                           PC10 PC11 PC12 */
  GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5
                          |GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9
                          |GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA1 PA4 PA6 PA7
                           PA9 PA10 PA11 PA12
                           PA15 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_6|GPIO_PIN_7
                          |GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12
                          |GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : LD2_Pin PWR_EN_Pin */
  GPIO_InitStruct.Pin = LD2_Pin|PWR_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : SOUND_DO_Pin */
  GPIO_InitStruct.Pin = SOUND_DO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SOUND_DO_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PB1 PB2 PB10 PB11
                           PB12 PB13 PB14 PB15
                           PB4 PB5 PB6 PB7
                           PB8 PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_10|GPIO_PIN_11
                          |GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15
                          |GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7
                          |GPIO_PIN_8|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PD2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartSentinelTask */
/**
  * @brief  Function implementing the SentinelTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartSentinelTask */
void StartSentinelTask(void *argument)
{
  /* USER CODE BEGIN 5 */
	osTimerStart(heartbeatTimerHandle, 10000);     /* heartbeat every 10 s */
  printf("FreeRTOS running. Waiting for events...\r\n");

  uint8_t evt;
  for (;;)
  {
    /* Block until an event arrives -> idle task -> tickless -> Stop 2 */
    if (osMessageQueueGet(evtQueueHandle, &evt, NULL, osWaitForever) != osOK) continue;

    uint32_t t = osKernelGetTickCount();
    switch (evt)
    {
      case EVT_SOUND: {
        sound_count++;
        printf("[%6lu ms] WAKE reason=SOUND #%lu\r\n", t, sound_count);
        sound_class_t s = sound_measure_classify();
        if (s == SND_CLAP || s == SND_MOTOR) {
            main_power_set(1, "sound");
        } else {
            false_alarm_count++;
            printf("         main power NOT enabled -> false alarm #%lu, back to sleep\r\n",
                   false_alarm_count);
        }
        osDelay(500);                                   /* extra quiet time */
        __HAL_GPIO_EXTI_CLEAR_IT(SOUND_DO_Pin);         /* drop queued edges */
        HAL_NVIC_ClearPendingIRQ(EXTI0_IRQn);
        HAL_NVIC_EnableIRQ(EXTI0_IRQn);                 /* re-enable sound interrupt */
        break;
      }

      case EVT_BUTTON:
        /* Local feedback: the button toggles main power */
        button_count++;
        printf("[%6lu ms] BUTTON #%lu\r\n", t, button_count);
        main_power_set(!main_power_on, "button");
        break;

      case EVT_LORA:
        lora_count++;
        printf("[%6lu ms] LORA #%lu msg=\"%s\"\r\n", t, lora_count, lora_msg);
        /* check "sleep" first */
        if (strstr(lora_msg, "sleep") != NULL || strstr(lora_msg, "uyu") != NULL) {
            main_power_set(0, "lora sleep");
        } else if (strstr(lora_msg, "wake") != NULL || strstr(lora_msg, "uyan") != NULL) {
            main_power_set(1, "lora wake");
        } else {
            printf("         unknown command, ignored\r\n");
        }
        break;

      case EVT_HEARTBEAT:
        /* HAL_GetTick() only advances while awake -> "awake" = total awake time */
    	  /* Safety: never leave main power on forever */
    	  if (main_power_on && (t - main_power_since) >= POWER_MAX_MS) {
    	              main_power_set(0, "safety timeout");
    	          }

    	  printf("[%6lu ms] HEARTBEAT power=%s sound=%lu button=%lu lora=%lu | awake=%lu ms | "
                 "stop2=%lu (timer=%lu event=%lu) sleep=%lu ms | lora_bytes=%lu err=%lu\r\n",
                 t, main_power_on ? "ON" : "OFF", sound_count, button_count, lora_count, HAL_GetTick(),
                 tl_stop2_count, tl_timer_wakeups, tl_event_wakeups, tl_sleep_ms,
                 lora_byte_count, lora_error_count);
        printf("          dbg busy=%lu hold=%lu lock=%lu\r\n", tl_dbg_busy, tl_dbg_hold, tl_dbg_lock);
        break;

    }   /* end switch */
  }     /* end for */
  /* USER CODE END 5 */
}

/* HeartbeatCallback function */
void HeartbeatCallback(void *argument)
{
  /* USER CODE BEGIN HeartbeatCallback */
	  uint8_t evt = EVT_HEARTBEAT;
	  osMessageQueuePut(evtQueueHandle, &evt, 0, 0);
  /* USER CODE END HeartbeatCallback */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM7 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM7)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
