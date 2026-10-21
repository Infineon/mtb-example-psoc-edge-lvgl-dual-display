/*******************************************************************************
* File Name        : lv_port_disp.c
*
* Description      : This file provides implementation of low level display
*                    device driver for LVGL.
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
#include "lv_draw_sw.h"
#include <stdbool.h>
#include <string.h>
#include "cy_graphics.h"


#if LV_COLOR_DEPTH == 16
    #define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#elif LV_COLOR_DEPTH == 32
    #define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_ARGB8888))
#endif


/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Two full-screen frame buffers for double-buffered FULL render mode: LVGL
 * renders one while the display controller scans out the other, swapped on
 * vsync. */

CY_SECTION(".cy_gpu_buf") LV_ATTRIBUTE_MEM_ALIGN uint8_t disp_buf1[MY_DISP_HOR_RES *
                                               MY_DISP_VER_RES * BYTE_PER_PIXEL];
CY_SECTION(".cy_gpu_buf") LV_ATTRIBUTE_MEM_ALIGN uint8_t disp_buf2[MY_DISP_HOR_RES *
                                               MY_DISP_VER_RES * BYTE_PER_PIXEL];
/* Frame buffer used by GFXSS to render UI */
void *frame_buffer1 = &disp_buf1;

cy_stc_gfx_context_t gfx_context;


/*******************************************************************************
* Function Name: disp_flush
********************************************************************************
* Summary:
*  Flush the content of the internal buffer to the specific area on the display.
*  You can use DMA or any hardware acceleration to do this operation in the
*  background but 'lv_display_flush_ready()' has to be called when finished.
*
* Parameters:
*  *disp_drv: Pointer to the display driver structure to be registered by HAL.
*  *area: Pointer to the area of the screen (not used).
*  *color_p: Pointer to the frame buffer address.
*
* Return:
*  void
*
*******************************************************************************/
static void LV_ATTRIBUTE_FAST_MEM disp_flush(lv_display_t *disp_drv,
                                             const lv_area_t *area,
                                             uint8_t *color_p)
{
    CY_UNUSED_PARAMETER(area);

    /* Commit the new front buffer first, then drain any pre-commit vsync and
     * wait for the next one. Draining before the commit could wake us a frame
     * early while the DC still scans the old buffer, causing flicker. */
    Cy_GFXSS_Set_FrameBuffer((GFXSS_Type*) GFXSS, (uint32_t*) color_p,
            &gfx_context);

    ulTaskNotifyTakeIndexed(DISP_VSYNC_NOTIFY_IDX, pdTRUE, 0);

    if (ulTaskNotifyTakeIndexed(DISP_VSYNC_NOTIFY_IDX, pdTRUE, portMAX_DELAY))
    {
        /* Inform the graphics library that you are ready with the flushing */
        lv_display_flush_ready(disp_drv);
    }
}


/*******************************************************************************
* Function Name: lv_port_disp_init
********************************************************************************
* Summary:
*  Initializes the LVGL display: creates it, registers the flush callback, and
*  sets the double frame buffers with an explicit stride.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void lv_port_disp_init(void)
{
    memset(disp_buf1, 0, sizeof(disp_buf1));
    memset(disp_buf2, 0, sizeof(disp_buf2));

    lv_display_t * disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);

    lv_display_set_flush_cb(disp, disp_flush);

    lv_tick_set_cb(xTaskGetTickCount);

    /* Explicit stride: the buffer is MY_DISP_HOR_RES (832) wide for VG-Lite
     * 16px alignment while the logical display is ACTUAL_DISP_HOR_RES (800).
     * Without it, LVGL recomputes stride from 800 and garbles the output. */
    uint32_t buf_stride = MY_DISP_HOR_RES * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_NATIVE);
    lv_display_set_buffers_with_stride(disp, disp_buf1, disp_buf2, sizeof(disp_buf1),
                                       buf_stride, LV_DISPLAY_RENDER_MODE_FULL);

    /* Match LVGL's resolution to the physical display resolution. */
    lv_display_set_resolution(disp, ACTUAL_DISP_HOR_RES, ACTUAL_DISP_VER_RES);

    Cy_GFXSS_Clear_DC_Interrupt((GFXSS_Type*) GFXSS, &gfx_context);
}


/* [] END OF FILE */
