[Click here](../README.md) to view the README.

## Design and implementation

This code example drives two displays at the same time from a single PSOC&trade; Edge MCU, each rendering its own LVGL user interface:

- A **Waveshare 4.3-inch MIPI-DSI display** (Chipone ICN6211 controller) driven by the PSOC&trade; Edge graphics subsystem (GFXSS), which houses an independent 2.5D GPU, a display controller (DC), and a MIPI DSI host with a D-PHY interface. It runs the LVGL Widgets demo with VG-Lite GPU acceleration.
- An **ST7735S 0.96-inch SPI TFT display** on the [SHIELD_XENSIV_A](https://www.infineon.com/evaluation-board/SHIELD-XENSIV-A), refreshed by the CPU over an SCB SPI block. It runs a thermometer UI fed by the on-shield XENSIV&trade; DPS368 sensor over I2C.

All PSOC&trade; Edge E84 MCU applications have a dual-CPU three-project structure to develop code for the CM33 and CM55 cores. The CM33 core has two separate projects for the secure processing environment (SPE) and the non-secure processing environment (NSPE). The three project folders are as follows:

**Table 1. Application projects**

Project | Description
--------|------------------------
*proj_cm33_s* | Project for CM33 secure processing environment (SPE)
*proj_cm33_ns* | Project for CM33 non-secure processing environment (NSPE)
*proj_cm55* | CM55 project – implements the dual-display application

<br>

At device reset, the secure boot process starts from the ROM boot with the secure enclave (SE) as the root of trust (RoT). From the secure enclave, the boot flow is passed to the system CPU subsystem where the secure CM33 application starts. After the necessary secure configuration, the flow is passed to the non-secure CM33 application, which configures the system clocks, pins, clock-to-peripheral connections, and other platform resources. It then enables the CM55 core using `Cy_SysEnableCM55()` and puts the CM33 CPU into Deep Sleep. The CM55 application drives both displays.

### CM55 application flow

The application creates three FreeRTOS tasks (listed in Table 2); LVGL spawns an additional software draw thread because `LV_USE_OS` is set to `LV_OS_FREERTOS`. These are coordinated around a **single LVGL instance** that manages two displays. The application creates no FreeRTOS software timer: periodic input handling runs on LVGL's indev read timer (an `lv_timer` set in `lv_port_indev.c`), while the LPTimer (tickless idle) and TCPWM (run-time stats) are the only hardware timers used.

**Table 2. Tasks**

Task | Priority | Responsibility
-----|----------|---------------
`cm55_gfx_task` | Highest | Initializes the GFXSS, DC/GPU interrupts, the shared I2C controller (SCB0), the 4.3-inch DSI panel, VG-Lite, and LVGL. Registers the DSI display and touch input, launches the Widgets demo, then runs `lv_timer_handler()` in a loop. This is the **only** caller of `lv_timer_handler()`, so it services the flush callbacks of *both* displays.
`shield_thermometer_task` | High | Waits on a semaphore until the graphics task has brought up LVGL and SCB0, then initializes the ST7735S panel (SCB SPI), registers the second `lv_display`, builds the thermometer UI, initializes the DPS368 sensor, and periodically samples the temperature and updates the UI.
Touch polling task (`lv_port_indev.c`) | Low | Polls the FT5406 touch controller over the shared I2C bus at a fixed interval and caches the result, so the LVGL indev read callback never performs blocking I2C in the rendering thread.

<br>

The `shield_thermometer_task` blocks on a binary semaphore that the graphics task signals (`shield_thermometer_notify_lvgl_ready()`) only after `lv_init()` and the shared I2C controller are up. This guarantees the second display is registered and the sensor is accessed only after LVGL and the bus are ready.

### Two LVGL displays from one instance

LVGL is initialized once. The 4.3-inch display is created in `lv_port_disp_init()` and becomes the default display; the Widgets demo is built on it. The shield task then calls `lv_display_create()` to register the ST7735S as a second display.

> **Note:** `lv_display_create()` does not change the default display. The thermometer UI is therefore built on the screen obtained from the shield's own display handle (`lv_display_get_screen_active(shield_disp)`), not on `lv_screen_active()`, which still targets the 4.3-inch display.

Because a single `lv_timer_handler()` (in the graphics task) services both displays, their flush callbacks run in that one context:

- **4.3-inch DSI flush (`disp_flush`)** uses double-buffered `LV_DISPLAY_RENDER_MODE_FULL`. Two full-screen frame buffers alternate: LVGL renders the back buffer while the DC scans out the front buffer. The flush commits the new front-buffer address with `Cy_GFXSS_Set_FrameBuffer()` and waits for the vsync interrupt (raised by the DC ISR through a task notification) before returning, giving tear-free output.
- **ST7735S SPI flush (`lvgl_disp_flush_cb`)** renders a full frame into a GPU-accessible buffer, swaps the pixels to big-endian RGB565 (`lv_draw_sw_rgb565_swap`, since the ST7735S expects big-endian while LVGL renders little-endian), programs the panel address window (CASET/RASET with the module's start offsets), and streams the pixels with a RAMWR command over SPI.


### GPU buffer memory placement and coherency

Both the 4.3-inch frame buffers and the ST7735S render buffer are placed in the `.cy_gpu_buf` linker section (the `gfx_mem` / SOCMEM region), because LVGL dispatches rendering to the VG-Lite GPU, which can only draw into that region. A plain SRAM buffer causes the GPU render to silently fail.

For the 4.3-inch display the DC (hardware) scans the buffer out, so there is no CPU-cache involvement. For the ST7735S display the GPU writes the buffer and the flush callback reads it back with the CPU for the SPI transfer. The CM55 runs with the D-cache enabled, but the `gfx_mem` / `.cy_gpu_buf` region is mapped non-cacheable through the CM55 MPU (configured in design.modus), so no cache maintenance is required between the GPU write and the CPU read.

### Arm&reg; Helium (M-Profile Vector Extension) acceleration for LVGL

This example enables Arm&reg; Helium acceleration for LVGL's software blend operations. The following macros in *lv_conf.h* control this:

```c
#define LV_USE_NATIVE_HELIUM_ASM  1
#define LV_USE_DRAW_SW_ASM        LV_DRAW_SW_ASM_HELIUM
```

To revert to scalar C code, set `LV_USE_NATIVE_HELIUM_ASM` to `0` and `LV_USE_DRAW_SW_ASM` to `LV_DRAW_SW_ASM_NONE`.

> **Note (IAR toolchain):** The IAR assembler cannot process the GCC-style `.S` Helium blend assembly, so those files are excluded from the build for `TOOLCHAIN=IAR` (see the Makefile) and software rendering falls back to scalar C code with no code changes required.
