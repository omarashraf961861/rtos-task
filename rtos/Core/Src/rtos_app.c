#include <stdio.h>
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"


#define SWITCH_PIN              GPIO_PIN_0
#define COUNTER_RESET_VALUE     9U
#define SWITCH_DEBOUNCE_TICKS   50U
#define SHARED_MIN              100
#define SHARED_MAX              200
#define EVT_SWITCH_PRESSED      (1U << 0)
#define TASK_PRIORITY           (tskIDLE_PRIORITY + 1)
#define UART_QUEUE_LENGTH       16U


typedef enum
{
    SRC_COUNTER = 0,
    SRC_SHARED
} DataSource_t;
typedef struct
{
    DataSource_t source;
    uint32_t     value;
} UARTData_t;


extern UART_HandleTypeDef huart1;
volatile uint32_t counter = 0;
static int32_t    shared  = SHARED_MIN;
static QueueHandle_t      UARTDataQueue;
static SemaphoreHandle_t  sharedMutex;
static EventGroupHandle_t switchEvents;
static void App_Fatal(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

/* UM1850, Page 228 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    static TickType_t lastPressTick = 0;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (GPIO_Pin != SWITCH_PIN)
    {
        return;
    }


    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - lastPressTick) < pdMS_TO_TICKS(SWITCH_DEBOUNCE_TICKS))
    {
        return;
    }
    lastPressTick = now;


    counter++;
    if (counter >= COUNTER_RESET_VALUE)
    {
        counter = 0;
    }

/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/12-Event-groups-or-flags/06-xEventGroupSetBitsFromISR*/
    xEventGroupSetBitsFromISR(switchEvents, EVT_SWITCH_PRESSED,
                              &xHigherPriorityTaskWoken);


    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}


/*Task 1*/
static void CalcSwitch1_Task(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
/*https://freertos.org/Documentation/02-Kernel/04-API-references/12-Event-groups-or-flags/04-xEventGroupWaitBits*/
        EventBits_t bits = xEventGroupWaitBits(switchEvents,
                                               EVT_SWITCH_PRESSED,
                                               pdTRUE,
                                               pdFALSE,
                                               2);

        if (bits & EVT_SWITCH_PRESSED)
        {
            UARTData_t msg;

            uint32_t counterCopy = counter;

            msg.source = SRC_COUNTER;
            msg.value  = counterCopy * 10U;
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/03-xQueueSend*/
            xQueueSend(UARTDataQueue, &msg, 0);
        }

    }
}

/*Task 2*/
static void IncreaseVar_Task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {/*https://freertos.org/Documentation/02-Kernel/04-API-references/02-Task-control/02-vTaskDelayUntil*/
        vTaskDelayUntil(&lastWake, 10);

        UARTData_t msg;
        msg.source = SRC_SHARED;
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/10-Semaphore-and-Mutexes/12-xSemaphoreTake*/
        xSemaphoreTake(sharedMutex, portMAX_DELAY);
        shared++;
        if (shared > SHARED_MAX)
        {
            shared = SHARED_MIN;
        }
        msg.value = (uint32_t)shared;
        /*https://www.freertos.org/Documentation/02-Kernel/04-API-references/10-Semaphore-and-Mutexes/15-xSemaphoreGive*/
        xSemaphoreGive(sharedMutex);
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/03-xQueueSend*/
        xQueueSend(UARTDataQueue, &msg, 0);
    }
}

/*Task 3*/
static void DecreaseVar_Task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {/*https://freertos.org/Documentation/02-Kernel/04-API-references/02-Task-control/02-vTaskDelayUntil*/
        vTaskDelayUntil(&lastWake, 12);

        UARTData_t msg;
        msg.source = SRC_SHARED;
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/10-Semaphore-and-Mutexes/12-xSemaphoreTake*/
        xSemaphoreTake(sharedMutex, portMAX_DELAY);
        shared--;
        if (shared < SHARED_MIN)
        {
            shared = SHARED_MAX;
        }
        msg.value = (uint32_t)shared;
        /*https://www.freertos.org/Documentation/02-Kernel/04-API-references/10-Semaphore-and-Mutexes/15-xSemaphoreGive*/
        xSemaphoreGive(sharedMutex);
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/03-xQueueSend*/
        xQueueSend(UARTDataQueue, &msg, 0);
    }
}

/*Task 4*/
static void UART_Task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t lastWake = xTaskGetTickCount();
    UARTData_t msg;
    char       text[32];

    for (;;)
    {
        vTaskDelayUntil(&lastWake, 5);
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/09-xQueueReceive*/
        while (xQueueReceive(UARTDataQueue, &msg, 0) == pdPASS)
        {

            const char *label = (msg.source == SRC_COUNTER) ? "Counter" : "Shared";

            int len = snprintf(text, sizeof(text), "%s: %lu\r\n",
                               label, (unsigned long)msg.value);

            if (len > 0)
            { /*U1850, Page556*/
                HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)len, 20);
            }
        }
    }
}



void App_Start(void)
{/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/01-xQueueCreate*/
    UARTDataQueue = xQueueCreate(UART_QUEUE_LENGTH, sizeof(UARTData_t));
    /*https://www.freertos.org/Documentation/02-Kernel/04-API-references/10-Semaphore-and-Mutexes/06-xSemaphoreCreateMutex*/
    sharedMutex   = xSemaphoreCreateMutex();
    /*https://freertos.org/Documentation/02-Kernel/04-API-references/12-Event-groups-or-flags/01-xEventGroupCreate*/
    switchEvents  = xEventGroupCreate();

    if ((UARTDataQueue == NULL) || (sharedMutex == NULL) || (switchEvents == NULL))
    {
        App_Fatal();
    }

/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/01-xTaskCreate*/
    if ((xTaskCreate(CalcSwitch1_Task, "CalcSwitch1", 128, NULL, TASK_PRIORITY, NULL) != pdPASS) ||
        (xTaskCreate(IncreaseVar_Task, "IncreaseVar", 128, NULL, TASK_PRIORITY, NULL) != pdPASS) ||
        (xTaskCreate(DecreaseVar_Task, "DecreaseVar", 128, NULL, TASK_PRIORITY, NULL) != pdPASS) ||
        (xTaskCreate(UART_Task,        "UART",        256, NULL, TASK_PRIORITY, NULL) != pdPASS))
    {
        App_Fatal();
    }
/*https://www.freertos.org/Documentation/02-Kernel/04-API-references/04-RTOS-kernel-control/03-vTaskStartScheduler*/
    vTaskStartScheduler();

    App_Fatal();
}
