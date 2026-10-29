/**
 * @file bsp_sys.c
 * @brief 系统级 BSP 封装实现
 * @author 李嘉图
 * @date 2026-06-18
 *
 * @note 时间基准用 esp_timer_get_time()：它基于 64bit 硬件定时器，
 *       不像 esp_log_timestamp() 那样受 FreeRTOS tick 精度影响，
 *       而且可在中断里调用。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stdio.h>
#include "bsp_sys.h"
// 功能包含
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "rom/ets_sys.h"

/*==============================================================================
 * 时间基准
 *============================================================================*/
/**
 * @brief 获取系统滴答计数值（单位：ms）
 * @return 系统上电后的毫秒数
 */
uint32_t BSP_SYS_GetTickMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/**
 * @brief 获取系统时间（单位：us）
 * @return 系统上电后的微秒数
 */
uint64_t BSP_SYS_GetTickUs(void)
{
    return (uint64_t)esp_timer_get_time();
}

/*==============================================================================
 * 忙等延时
 *============================================================================*/
/**
 * @brief 微秒级忙等延时
 * @param us 延时微秒数
 */
void BSP_SYS_DelayUs(uint32_t us)
{
    ets_delay_us(us);
}

/*==============================================================================
 * 芯片信息
 *============================================================================*/
/**
 * @brief 打印芯片型号、核心数、flash/PSRAM 容量、复位原因、MAC 地址
 * @note  启动时调一次。重点看两行：
 *         - "PSRAM size" 应该是 8MB（N16R8 的 R8）。如果是 0，说明 Octal PSRAM 没初始化成功，
 *           后续录音缓冲分配到 PSRAM 会失败。
 *         - "Reset reason" 如果是 RTCWDT/BROWNOUT 反复出现，说明供电或引脚配置有问题。
 */
void BSP_SYS_PrintChipInfo(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    printf("\n");
    printf("==================== ESP32 Music Device ====================\n");
    printf("[SYS] Chip        : %s, %d core(s), rev v%d.%d\n",
           (chip.model == CHIP_ESP32S3) ? "ESP32-S3" :
           (chip.model == CHIP_ESP32S2) ? "ESP32-S2" :
           (chip.model == CHIP_ESP32)   ? "ESP32"    : "unknown",
           chip.cores,
           chip.revision / 100, chip.revision % 100);

    printf("[SYS] Features    : %s%s%s\n",
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip.features & CHIP_FEATURE_BLE)      ? "BLE "  : "",
           (chip.features & CHIP_FEATURE_EMB_FLASH) ? "EmbeddedFlash " : "");

    printf("[SYS] Flash size  : %u MB\n", (unsigned)(flash_size / (1024 * 1024)));

    printf("[SYS] PSRAM size  : %u MB %s\n",
           (unsigned)(esp_psram_get_size() / (1024 * 1024)),
           (esp_psram_is_initialized()) ? "(initialized)" : "(NOT initialized!)");

    printf("[SYS] Free heap   : internal %u B, PSRAM %u B\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    printf("[SYS] Reset reason: %d\n", (int)esp_reset_reason());
    printf("============================================================\n\n");
}
