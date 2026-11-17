/**
 * ==========================================================================
 *  FreeRTOS Tickless Idle  +  LPTIM1  +  STM32L4 Stop 2
 * ==========================================================================
 *  By default FreeRTOS wakes the CPU every 1 ms with SysTick, and SysTick
 *  stops in Stop 2. With configUSE_TICKLESS_IDLE = 2 we provide our own
 *  vPortSuppressTicksAndSleep():
 *    1. When no task is ready, FreeRTOS calls us with the number of idle ticks
 *    2. Stop SysTick, program LPTIM1 (LSE / 16 = 2048 Hz) for that duration
 *    3. Enter Stop 2 (~1-2 uA)
 *    4. After wake-up, read the real sleep time from the LPTIM counter
 *    5. Advance the FreeRTOS tick with vTaskStepTick(), restart SysTick
 *  Resolution 0.49 ms, longest single sleep 65535 / 2048 = 32 s.
 * ==========================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

#define LPTIM_HZ        2048U        /* 32768 Hz / 16 */
#define LPTIM_MAX       0xFFFFU
#define TICK_TO_CNT(t)  ((uint32_t)(((uint64_t)(t) * LPTIM_HZ) / configTICK_RATE_HZ))
#define CNT_TO_TICK(c)  ((uint32_t)(((uint64_t)(c) * configTICK_RATE_HZ) / LPTIM_HZ))


extern UART_HandleTypeDef huart2;

/* Statistics (printed in the HEARTBEAT line in main.c) */
volatile uint32_t tl_stop2_count   = 0;  /* number of Stop 2 entries            */
volatile uint32_t tl_timer_wakeups = 0;  /* woke up because LPTIM expired       */
volatile uint32_t tl_event_wakeups = 0;  /* woke up early by EXTI / LPUART      */
volatile uint32_t tl_sleep_ms      = 0;  /* total time spent in Stop 2          */
volatile uint8_t  tl_stop_lock     = 0;  /* 1 = Stop 2 forbidden (ADC+DMA busy) */
volatile uint32_t tl_hold_until    = 0;  /* no Stop 2 until this tick (LoRa RX window) */
volatile uint32_t tl_dbg_busy = 0, tl_dbg_hold = 0, tl_dbg_lock = 0;

void tickless_lptim_init(void)
{
    /* Prescaler (DIV16) and LSE clock are set by CubeMX in MX_LPTIM1_Init().
     * IER may only be written while LPTIM is disabled. */
    LPTIM1->CR  = 0;
    LPTIM1->IER = LPTIM_IER_ARRMIE;          /* counter == ARR -> interrupt     */
    __HAL_LPTIM_LPTIM1_EXTI_ENABLE_IT();     /* EXTI line 32: wake from Stop    */
}

static void lptim_start(uint32_t counts)
{
    LPTIM1->CR  = LPTIM_CR_ENABLE;
    LPTIM1->ICR = LPTIM_ICR_ARROKCF | LPTIM_ICR_ARRMCF;
    LPTIM1->ARR = counts;
    while ((LPTIM1->ISR & LPTIM_ISR_ARROK) == 0) { }   /* wait for ARR write   */
    LPTIM1->ICR = LPTIM_ICR_ARROKCF;
    LPTIM1->CR |= LPTIM_CR_CNTSTRT;                     /* start counting      */
}

static uint32_t lptim_read(void)
{
    uint32_t a, b;   /* asynchronous counter: two consecutive reads must match */
    do { a = LPTIM1->CNT; b = LPTIM1->CNT; } while (a != b);
    return a;
}

#define TL_LPTIM_HZ   2048U      /* LSE 32768 Hz / 16            */
#define TL_MAX_TICKS  31000U     /* 16-bit LPTIM limit at 2048 Hz */

void vPortSuppressTicksAndSleep(TickType_t xExpectedIdleTime)
{
  /* 1. Let pending debug output finish (USART2 stops in Stop 2) */
  while ((USART2->ISR & USART_ISR_TC) == 0U) { }

  /* 2. Convert the idle time to LPTIM counts */
  uint32_t ticks = (uint32_t)xExpectedIdleTime;
  if (ticks > TL_MAX_TICKS) ticks = TL_MAX_TICKS;
  uint32_t arr = (ticks * TL_LPTIM_HZ) / configTICK_RATE_HZ;
  if (arr < 2U) arr = 2U;

  __disable_irq();
  __DSB();
  __ISB();

  /* 3. A task became ready in the meantime: do not sleep */
  if (eTaskConfirmSleepModeStatus() == eAbortSleep) {
    __enable_irq();
    return;
  }

  /* 4. Light sleep only (SysTick keeps running) when:
        - LPUART1 is receiving a byte right now
        - a LoRa message is in progress (hold window)
        - the ADC/DMA sound measurement is running        */
  uint8_t light = 0;
  if (LPUART1->ISR & USART_ISR_BUSY)                        { tl_dbg_busy++; light = 1; }
  if ((int32_t)(tl_hold_until - xTaskGetTickCount()) > 0)   { tl_dbg_hold++; light = 1; }
  if (tl_stop_lock)                                         { tl_dbg_lock++; light = 1; }
  if (light) {
    __DSB();
    __WFI();
    __enable_irq();
    return;
  }

  /* 5. Stop SysTick; if a tick is already pending, give up this time */
  SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
  if (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) {
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
    __enable_irq();
    return;
  }

  /* 6. Suspend the HAL timebase (TIM7) */
  HAL_SuspendTick();
  NVIC_ClearPendingIRQ(TIM7_IRQn);

  /* 7. Arm LPTIM1 and enter Stop 2 */
  lptim_start(arr);
  tl_stop2_count++;
  HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

  /* ---------------- woke up ---------------- */

  /* 8. How long did we sleep? */
  uint32_t elapsed_cnt;
  if (LPTIM1->ISR & LPTIM_ISR_ARRM) {
    elapsed_cnt = arr;              /* timer expired */
    tl_timer_wakeups++;
  } else {
    elapsed_cnt = lptim_read();     /* woken by an event (EXTI, LPUART...) */
    tl_event_wakeups++;
  }

  /* 9. Stop LPTIM1 and clear its flags */
  LPTIM1->ICR = LPTIM_ICR_ARRMCF;
  LPTIM1->CR  = 0U;
  NVIC_ClearPendingIRQ(LPTIM1_IRQn);

  /* 10. Advance the FreeRTOS tick count */
  uint32_t elapsed_ticks = (elapsed_cnt * configTICK_RATE_HZ) / TL_LPTIM_HZ;
  if (elapsed_ticks > ticks) elapsed_ticks = ticks;
  tl_sleep_ms += elapsed_ticks;
  vTaskStepTick(elapsed_ticks);

  /* 11. Restart SysTick and the HAL timebase */
  SysTick->VAL  = 0U;
  SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
  HAL_ResumeTick();

  __enable_irq();   /* the pending wake-up interrupt runs here */
}
