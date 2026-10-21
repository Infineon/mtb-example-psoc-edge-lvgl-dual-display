/*******************************************************************************
* File Name        : shield_thermometer.c
*
* Description      : Shield ST7735S thermometer task for the CM55 application.
*                    Drives the ST7735S SPI TFT on the XENSIV shield with an
*                    LVGL thermometer UI fed by the on-shield DPS368 sensor,
*                    running alongside the 4.3-inch DSI display. LVGL access is
*                    serialized with lv_lock()/lv_unlock() and the shared I2C
*                    bus (SCB0) with disp_touch_i2c_mutex.
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
#include <stdbool.h>
#include <stdlib.h>

#include "cybsp.h"
#include "retarget_io_init.h"
#include "mtb_st7735s.h"
#include "mtb_xensiv_dps3xx.h"
#include "shield_thermometer.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lvgl.h"

/*******************************************************************************
* Macros
*******************************************************************************/
#define SHIELD_TASK_NAME            ("Shield Thermometer Task")
/* stack size in words */
#define SHIELD_TASK_STACK_SIZE      (configMINIMAL_STACK_SIZE * 8)
#define SHIELD_TASK_PRIORITY        (configMAX_PRIORITIES - 2)

/* The shield thermometer is displayed in landscape orientation. */
#define SHIELD_ORIENTATION          MTB_ST7735S_ORIENTATION_LANDSCAPE

/* Temperature sampling period */
#define TEMP_SAMPLE_PERIOD_MSEC     (500U)

/* Full-frame render buffer (RGB565, 2 bytes/pixel). The active pixel count is
 * orientation independent, so size it to the native panel. */
#define LVGL_BUF_SIZE_BYTES         (MTB_ST7735S_PANEL_WIDTH * MTB_ST7735S_PANEL_HEIGHT * 2)

/* Thermometer temperature range mapped to the bar (degrees Celsius) */
#define TEMP_MIN_C                  (0)
#define TEMP_MAX_C                  (50)

/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Shared I2C controller (SCB0) context, initialized by the graphics task. */
extern cy_stc_scb_i2c_context_t disp_touch_i2c_controller_context;

/* Serializes the shared SCB0 bus against the touch controller. Created in main(). */
extern SemaphoreHandle_t disp_touch_i2c_mutex;

static lv_obj_t* temp_bar;
static lv_obj_t* temp_label;

/* ST7735S driver instance for the shield TFT (SPI = SCB10). */
static mtb_st7735s_t shield_st7735s;

/* DPS368 temperature sensor on the XENSIV shield (I2C = SCB0). */
static mtb_hal_i2c_t   shield_i2c_hal;
static xensiv_dps3xx_t dps_sensor;

/* Signaled by the graphics task once LVGL and SCB0 are initialized */
static SemaphoreHandle_t lvgl_ready_sem;

/* LVGL display bound to the ST7735S. Its own screen handle must be used, since
 * lv_display_create() does not change the default display. */
static lv_display_t* shield_disp;

/* Shield render buffer. Must live in the GPU-accessible '.cy_gpu_buf' region;
 * the VG-Lite GPU cannot render into plain SRAM. That region is mapped
 * non-cacheable via the CM55 MPU, so no cache maintenance is needed between
 * the GPU write and the CPU read. */
CY_SECTION(".cy_gpu_buf") LV_ATTRIBUTE_MEM_ALIGN
static uint8_t shield_lvgl_draw_buf[LVGL_BUF_SIZE_BYTES];


/*******************************************************************************
* Function Name: dps_init
********************************************************************************
* Summary:
*  Sets up the mtb_hal I2C object on the shared SCB0 bus and initializes the
*  DPS368 sensor. Returns false if the sensor is absent.
*
* Parameters:
*  void
*
* Return:
*  bool : true if the DPS368 was initialized, false otherwise
*
*******************************************************************************/
static bool dps_init(void)
{
    cy_rslt_t result;

    (void)xSemaphoreTake(disp_touch_i2c_mutex, portMAX_DELAY);
    result = mtb_hal_i2c_setup(&shield_i2c_hal, &CYBSP_I2C_CONTROLLER_hal_config,
                               &disp_touch_i2c_controller_context, NULL);
    if (CY_RSLT_SUCCESS == result)
    {
        /* SDO on this shield is tied to GND, so the DPS368 answers at 0x76. */
        result = mtb_xensiv_dps3xx_init_i2c(&dps_sensor, &shield_i2c_hal,
                                            XENSIV_DPS3XX_I2C_ADDR_ALT);
    }
    (void)xSemaphoreGive(disp_touch_i2c_mutex);

    return (CY_RSLT_SUCCESS == result);
}


/*******************************************************************************
* Function Name: dps_read_temp
********************************************************************************
* Summary:
*  Reads the compensated temperature (degrees Celsius) from the DPS368.
*  Returns false on I2C failure.
*
* Parameters:
*  temp_c : pointer that receives the temperature in degrees Celsius
*
* Return:
*  bool : true on success, false on I2C failure
*
*******************************************************************************/
static bool dps_read_temp(float* temp_c)
{
    cy_rslt_t result;

    (void)xSemaphoreTake(disp_touch_i2c_mutex, portMAX_DELAY);
    result = xensiv_dps3xx_read(&dps_sensor, NULL, temp_c);
    (void)xSemaphoreGive(disp_touch_i2c_mutex);

    return (CY_RSLT_SUCCESS == result);
}


/*******************************************************************************
* Function Name: display_init
********************************************************************************
* Summary:
*  Initializes the SCB10 SPI controller and the ST7735S display. Returns false
*  on failure so the caller can skip the shield UI without halting the app.
*
* Parameters:
*  void
*
* Return:
*  bool : true if the SPI and ST7735S were initialized, false otherwise
*
*******************************************************************************/
static bool display_init(void)
{
    static cy_stc_scb_spi_context_t spi_context;

    const mtb_st7735s_config_t tft_config =
    {
        .pins =
        {
            .dc_port  = CYBSP_DISP_DC_PORT,
            .dc_pin   = CYBSP_DISP_DC_PIN,
            .rst_port = CYBSP_SHIELD_DISP_RST_PORT,
            .rst_pin  = CYBSP_SHIELD_DISP_RST_PIN,
        },
        .orientation = SHIELD_ORIENTATION,
    };

    /* SHIELD_XENSIV_A: select the display on the shared Arduino SPI CS mux */
    Cy_GPIO_Write(CYBSP_DISP_CS_SEL_PORT, CYBSP_DISP_CS_SEL_PIN, 1UL);

    /* Initialize and enable the SPI controller (SCB10) using the BSP config */
    if (CY_SCB_SPI_SUCCESS != Cy_SCB_SPI_Init(CYBSP_SPI_CONTROLLER_HW,
                                              &CYBSP_SPI_CONTROLLER_config, &spi_context))
    {
        return false;
    }
    Cy_SCB_SPI_Enable(CYBSP_SPI_CONTROLLER_HW);

    /* Bring up the panel: reset, configuration, orientation, and display on */
    if (CY_RSLT_SUCCESS != mtb_st7735s_init(&shield_st7735s, CYBSP_SPI_CONTROLLER_HW, &tft_config))
    {
        return false;
    }

    return true;
}


/*******************************************************************************
* Function Name: lvgl_disp_flush_cb
********************************************************************************
* Summary:
*  LVGL flush callback. Pushes a rendered area to the ST7735S over SPI. Invoked
*  from the graphics task's lv_timer_handler().
*
* Parameters:
*  disp   : LVGL display being flushed
*  area   : rectangular area to copy to the panel
*  px_map : rendered pixel buffer for the area
*
* Return:
*  void
*
*******************************************************************************/
static void lvgl_disp_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    /* ST7735S expects big-endian RGB565; LVGL renders little-endian */
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));

    /* Program the window (offsets applied) and issue RAMWR, then stream pixels */
    mtb_st7735s_set_address_window(&shield_st7735s, (uint16_t)area->x1, (uint16_t)area->y1,
                                   (uint16_t)area->x2, (uint16_t)area->y2);
    mtb_st7735s_write_data_stream(&shield_st7735s, px_map, (uint32_t)(w * h * 2));

    lv_display_flush_ready(disp);
}


/*******************************************************************************
* Function Name: lvgl_display_create
********************************************************************************
* Summary:
*  Creates the LVGL display bound to the ST7735S flush callback. LVGL itself is
*  initialized once by the graphics task; this only registers an additional
*  display. Must be called with the LVGL lock held.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
static void lvgl_display_create(void)
{
    shield_disp = lv_display_create(shield_st7735s.width, shield_st7735s.height);
    lv_display_set_flush_cb(shield_disp, lvgl_disp_flush_cb);
    lv_display_set_buffers(shield_disp, shield_lvgl_draw_buf, NULL,
                           sizeof(shield_lvgl_draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
}


/*******************************************************************************
* Function Name: lvgl_ui_create
********************************************************************************
* Summary:
*  Builds the temperature UI on the ST7735S display: a horizontal blue-to-red
*  gradient thermometer bar plus a numeric readout label. Must be called with
*  the LVGL lock held.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
static void lvgl_ui_create(void)
{
    lv_obj_t* scr = lv_display_get_screen_active(shield_disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_label_set_text(title, "TEMPERATURE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Horizontal thermometer bar: blue (cold, left) to red (hot, right) */
    temp_bar = lv_bar_create(scr);
    lv_obj_set_size(temp_bar, 132, 16);
    lv_obj_align(temp_bar, LV_ALIGN_CENTER, 0, 2);
    lv_bar_set_range(temp_bar, 0, 100);
    lv_obj_set_style_bg_color(temp_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(temp_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(temp_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(temp_bar, lv_color_hex(0x2060FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(temp_bar, lv_color_hex(0xFF3020), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(temp_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(temp_bar, 8, LV_PART_INDICATOR);

    temp_label = lv_label_create(scr);
    lv_label_set_text(temp_label, "--.- C");
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(temp_label, LV_ALIGN_BOTTOM_MID, 0, -8);
}


/*******************************************************************************
* Function Name: lvgl_ui_update_temp
********************************************************************************
* Summary:
*  Animates the thermometer bar to the new temperature and updates the label.
*  Must be called with the LVGL lock held.
*
* Parameters:
*  temp_c : temperature to display, in degrees Celsius
*
* Return:
*  void
*
*******************************************************************************/
static void lvgl_ui_update_temp(float temp_c)
{
    int32_t pct = (int32_t)(((temp_c - (float)TEMP_MIN_C) * 100.0f) /
                            (float)(TEMP_MAX_C - TEMP_MIN_C));
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }
    lv_bar_set_value(temp_bar, pct, LV_ANIM_ON);

    /* Format sign separately so sub-zero readings (|T| < 1) keep the '-'. */
    int32_t tenths = (int32_t)(temp_c * 10.0f);
    const char* sign = (tenths < 0) ? "-" : "";
    int32_t abs_tenths = labs(tenths);
    lv_label_set_text_fmt(temp_label, "%s%d.%d C", sign,
                          (int)(abs_tenths / 10), (int)(abs_tenths % 10));
}


/*******************************************************************************
* Function Name: shield_thermometer_task
********************************************************************************
* Summary:
*  FreeRTOS task entry point. Waits until LVGL and SCB0 are ready, brings up the
*  ST7735S display and thermometer UI, initializes the DPS368 sensor, then
*  periodically samples the temperature and updates the UI.
*
* Parameters:
*  arg : unused FreeRTOS task argument
*
* Return:
*  void
*
*******************************************************************************/
static void shield_thermometer_task(void* arg)
{
    CY_UNUSED_PARAMETER(arg);

    /* Wait until the graphics task has initialized LVGL and the shared SCB0 */
    (void)xSemaphoreTake(lvgl_ready_sem, portMAX_DELAY);

    /* Bring up the ST7735S panel hardware (dedicated SPI SCB10). On failure,
     * leave the primary 4.3-inch display running and end this task. */
    if (!display_init())
    {
        printf("WARNING: ST7735S shield display init failed; skipping shield UI.\r\n\n");
        vTaskDelete(NULL);
    }

    /* Register this display and build its UI under the LVGL lock */
    lv_lock();
    lvgl_display_create();
    lvgl_ui_create();
    lv_unlock();

    /* Initialize the DPS368 temperature sensor on the shared I2C bus */
    if (!dps_init())
    {
        printf("WARNING: DPS368 temperature sensor not detected on I2C.\r\n\n");
        vTaskDelete(NULL);
    }
    printf("Shield thermometer display started.\r\n\n");

    for (;;)
    {
        float temp_c = 0.0f;

        if (dps_read_temp(&temp_c))
        {
            lv_lock();
            lvgl_ui_update_temp(temp_c);
            lv_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(TEMP_SAMPLE_PERIOD_MSEC));
    }
}


/*******************************************************************************
* Function Name: shield_thermometer_notify_lvgl_ready
********************************************************************************
* Summary:
*  Signals the shield thermometer task that LVGL and the shared I2C bus (SCB0)
*  are ready.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void shield_thermometer_notify_lvgl_ready(void)
{
    if (NULL != lvgl_ready_sem)
    {
        (void)xSemaphoreGive(lvgl_ready_sem);
    }
}


/*******************************************************************************
* Function Name: shield_thermometer_task_create
********************************************************************************
* Summary:
*  Creates the semaphore and FreeRTOS task for the shield thermometer. Call once
*  from main() before the scheduler starts.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void shield_thermometer_task_create(void)
{
    lvgl_ready_sem = xSemaphoreCreateBinary();
    if (NULL == lvgl_ready_sem)
    {
        printf("Error: Failed to create shield thermometer semaphore.\r\n");
        handle_app_error();
    }

    if (pdPASS != xTaskCreate(shield_thermometer_task, SHIELD_TASK_NAME,
                              SHIELD_TASK_STACK_SIZE, NULL,
                              SHIELD_TASK_PRIORITY, NULL))
    {
        printf("Error: Failed to create shield thermometer task.\r\n");
        handle_app_error();
    }
}

/* [] END OF FILE */
