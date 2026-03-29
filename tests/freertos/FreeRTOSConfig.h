/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdlib.h>

#define configUSE_PREEMPTION                        1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION     0
#define configUSE_IDLE_HOOK                         0
#define configUSE_TICK_HOOK                         0
#define configUSE_DAEMON_TASK_STARTUP_HOOK          0
#define configTICK_RATE_HZ                          ( 1000 )
#define configMAX_PRIORITIES                        ( 7 )
#define configMINIMAL_STACK_SIZE                    ( 256 )
#define configMAX_TASK_NAME_LEN                     ( 16 )
#define configIDLE_SHOULD_YIELD                     1
#define configQUEUE_REGISTRY_SIZE                   0
#define configUSE_16_BIT_TICKS                      0
#define configUSE_MUTEXES                           1
#define configUSE_RECURSIVE_MUTEXES                 1
#define configUSE_COUNTING_SEMAPHORES               1
#define configUSE_TASK_NOTIFICATIONS                1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES       2
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS     1
#define configCHECK_FOR_STACK_OVERFLOW              2
#define configUSE_MALLOC_FAILED_HOOK                0
#define configUSE_APPLICATION_TASK_TAG              0
#define configSUPPORT_STATIC_ALLOCATION             1
#define configSUPPORT_DYNAMIC_ALLOCATION            0
#define configKERNEL_PROVIDED_STATIC_MEMORY         1
#define configUSE_TIMERS                            1
#define configTIMER_TASK_PRIORITY                   ( configMAX_PRIORITIES - 3 )
#define configTIMER_QUEUE_LENGTH                    16
#define configTIMER_TASK_STACK_DEPTH                ( configMINIMAL_STACK_SIZE * 8 )
#define configUSE_CO_ROUTINES                       0
#define configUSE_TRACE_FACILITY                    0
#define configUSE_STATS_FORMATTING_FUNCTIONS        0
#define configUSE_POSIX_ERRNO                       1
#define configNUMBER_OF_CORES                       1
#define configSTACK_DEPTH_TYPE                      uint32_t

#define INCLUDE_vTaskPrioritySet                    1
#define INCLUDE_uxTaskPriorityGet                   1
#define INCLUDE_vTaskDelete                         1
#define INCLUDE_vTaskSuspend                        1
#define INCLUDE_vTaskDelayUntil                     1
#define INCLUDE_vTaskDelay                          1
#define INCLUDE_xTimerPendFunctionCall              1
#define INCLUDE_xTaskGetSchedulerState              1
#define INCLUDE_xTaskGetIdleTaskHandle              1
#define INCLUDE_eTaskGetState                       1
#define INCLUDE_xTaskAbortDelay                     1

#define configASSERT( x )                           \
  do {                                              \
    if( ( x ) == 0 ) {                              \
      abort();                                      \
    }                                               \
  } while( 0 )

#endif
