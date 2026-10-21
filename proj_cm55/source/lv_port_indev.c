/*******************************************************************************
* File Name        : lv_port_indev.c
*
* Description      : This file provides implementation of low level input device
*                    driver for LVGL.
*
* Related Document : See README.md
*
********************************************************************************
* (c) 2026, Infineon Technologies AG, or an affiliate of Infineon
* Technologies AG. All rights reserved.
* This software, associated documentation and materials ("Software") is
* owned by Infineon Technologies AG or one of its affiliates ("Infineon")
* and is protected by and subject to worldwide patent protection, worldwide
* copyright laws, and international treaty provisions. Therefore, you may use
* this Software only as provided in the license agreement accompanying the
* software package from which you obtained this Software. If no license
* agreement applies, then any use, reproduction, modification, translation, or
* compilation of this Software is prohibited without the express written
* permission of Infineon.
*
* Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
* IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
* THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
* SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
* Infineon reserves the right to make changes to the Software without notice.
* You are responsible for properly designing, programming, and testing the
* functionality and safety of your intended application of the Software, as
* well as complying with any legal requirements related to its use. Infineon
* does not guarantee that the Software will be free from intrusion, data theft
* or loss, or other breaches ("Security Breaches"), and Infineon shall have
* no liability arising out of any Security Breaches. Unless otherwise
* explicitly approved by Infineon, the Software may not be used in any
* application where a failure of the Product or any consequences of the use
* thereof can reasonably be expected to result in personal injury.
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lv_indev_private.h"
#include "cy_utils.h"
#include "mtb_ctp_ft5406.h"
#include "cybsp.h"
#include "retarget_io_init.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"


/*******************************************************************************
* Macros
*******************************************************************************/
/* LVGL indev timer period - can be fast since callback is just a cache read */
#define INDEV_READ_PERIOD_MS       33U

/* Background touch I2C polling interval */
#define TOUCH_POLL_MS              33U
#define TOUCH_TASK_STACK_SIZE      256U
#define TOUCH_TASK_PRIORITY        (tskIDLE_PRIORITY + 1U)

#define RESETVAL                   (0)

/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Cached touch state - written by background task, read by LVGL callback.
 * Single-writer on Cortex-M, so no lock needed for atomic word-sized fields. */
static volatile int      touch_cache_x     = RESETVAL;
static volatile int      touch_cache_y     = RESETVAL;
static volatile uint8_t  touch_cache_state = LV_INDEV_STATE_REL;

static TaskHandle_t touch_task_handle = NULL;

/* Serializes access to the shared I2C controller (SCB0), which is also used by
 * the shield DPS368 sensor task. Created in main(). */
extern SemaphoreHandle_t disp_touch_i2c_mutex;

mtb_ctp_ft5406_config_t ctp_ft5406_cfg =
{
    .i2c_base        = CYBSP_I2C_CONTROLLER_HW,
    .i2c_context     = &disp_touch_i2c_controller_context,
};

/*******************************************************************************
* Function Name: touchpad_init
********************************************************************************
* Summary:
*  Initialization function for touchpad supported by LVGL.
*
* Parameters:
*  void
*
* Return:
*  cy_rslt_t: CY_RSLT_SUCCESS on success, else the driver error code.
*
*******************************************************************************/
static cy_rslt_t touchpad_init(void)
{
    cy_rslt_t result = (cy_rslt_t)mtb_ctp_ft5406_init(&ctp_ft5406_cfg);

    if (CY_RSLT_SUCCESS != result)
    {
        printf("ERROR: Touch controller init failed (0x%08lX)\r\n",
               (unsigned long)result);
    }

    return result;
}


/*******************************************************************************
* Function Name: touch_poll_task
********************************************************************************
* Summary:
*  Background FreeRTOS task that polls the touch controller via I2C at a fixed
*  interval. Results are stored in volatile cache variables so the LVGL indev
*  callback can read them without any I2C overhead (zero blocking of the
*  rendering thread).
*
* Parameters:
*  void *arg: Pointer to the argument passed to the task (not used)
*
* Return:
*  void
*
*******************************************************************************/
static void touch_poll_task(void *arg)
{
    (void)arg;
    int tx = RESETVAL, ty = RESETVAL;

    for (;;)
    {
        uint8_t st = LV_INDEV_STATE_REL;

        mtb_ctp_touch_event_t ev;
        /* Serialize this I2C transfer against the DPS368 reads on the same bus */
        (void)xSemaphoreTake(disp_touch_i2c_mutex, portMAX_DELAY);
        cy_rslt_t res = (cy_rslt_t)mtb_ctp_ft5406_get_single_touch(&ev, &tx, &ty);
        (void)xSemaphoreGive(disp_touch_i2c_mutex);
        if ((CY_RSLT_SUCCESS == res) &&
            ((MTB_CTP_TOUCH_DOWN == ev) || (MTB_CTP_TOUCH_CONTACT == ev)))
        {
            st = LV_INDEV_STATE_PR;
        }

        /* Update cache - single writer, word-sized stores are atomic on Cortex-M */
        touch_cache_x     = tx;
        touch_cache_y     = ty;
        touch_cache_state = st;

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}


/*******************************************************************************
* Function Name: touchpad_read
********************************************************************************
* Summary:
*  Touchpad read function called periodically by the LVGL library. It returns
*  the latest touch state and coordinates from the cache populated by the
*  background touch polling task, so it performs no I2C access and never blocks
*  the rendering thread.
*
* Parameters:
*  *indev_drv: Pointer to the input driver structure to be registered by LVGL.
*  *data: Pointer to the data buffer holding touch coordinates.
*
* Return:
*  void
*
*******************************************************************************/
LV_ATTRIBUTE_FAST_MEM void touchpad_read(lv_indev_t *indev_drv,
                                         lv_indev_data_t *data)
{
    (void)indev_drv;

    /* Read from cache - no I2C, no blocking, no context switch */
    data->state = (lv_indev_state_t)touch_cache_state;

    data->point.x = ACTUAL_DISP_HOR_RES - touch_cache_x;
    data->point.y = ACTUAL_DISP_VER_RES - touch_cache_y;
}


/*******************************************************************************
* Function Name: lv_port_indev_init
********************************************************************************
* Summary:
*  Initialization function for input devices supported by LVGL.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void lv_port_indev_init(void)
{
    /* Initialize the touch controller hardware */
    if (CY_RSLT_SUCCESS != touchpad_init())
    {
        handle_app_error();
    }

    /* Start background touch polling task - does I2C reads independently
     * of the LVGL rendering thread so touch never blocks rendering.
     * Guard against creating a duplicate task if this is ever called twice. */
    if (touch_task_handle == NULL)
    {
        if (pdPASS != xTaskCreate(touch_poll_task, "TouchPoll",
                    TOUCH_TASK_STACK_SIZE, NULL,
                    TOUCH_TASK_PRIORITY, &touch_task_handle))
        {
            /* Without the polling task the cache never updates; fail explicitly */
            printf("ERROR: Failed to create touch polling task\r\n");
            handle_app_error();
        }
    }

    /* Register a touchpad input device - callback just reads cache (instant) */
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);
    lv_timer_set_period(indev->read_timer, INDEV_READ_PERIOD_MS);
}


/* [] END OF FILE */
