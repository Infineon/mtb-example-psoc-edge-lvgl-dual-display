/*******************************************************************************
* File Name        : shield_thermometer.h
*
* Description      : Public interface for the shield ST7735S thermometer task.
*                    This task drives the ST7735S SPI TFT on the Arduino/XENSIV
*                    shield with an LVGL thermometer UI fed by the on-shield
*                    DPS368 temperature sensor. It runs alongside the CM55
*                    graphics task that drives the 4.3-inch DSI display.
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

#pragma once

#if defined(__cplusplus)
extern "C"
{
#endif

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
void shield_thermometer_task_create(void);


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
void shield_thermometer_notify_lvgl_ready(void);

#if defined(__cplusplus)
}
#endif

/* [] END OF FILE */
