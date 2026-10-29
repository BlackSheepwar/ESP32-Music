/**
 * @file esp_log.h（离线测试桩，不属于固件）
 * @brief 把 ESP-IDF 的日志宏映射到 printf，并提供一个日志等级开关。
 *
 * @note  ★ 这里定义 esp_log_level_t 并用条件判断，而不是简单的 #define ★
 *        因为 ks_synth.c 里的 ESP_LOGD 参数含函数调用（如 audio_target_name），
 *        如果宏无条件展开，离线测试会产生大量无用输出，也拖慢测试。
 *        条件判断能让 -v 时才输出 INFO/DEBUG。
 */
#ifndef __ESP_LOG_STUB_H__
#define __ESP_LOG_STUB_H__

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
} esp_log_level_t;

/* 由 ks_test.c 定义；桩头文件只声明 */
extern esp_log_level_t g_log_level;

#define ESP_LOGE(tag, fmt, ...) do { if (g_log_level >= ESP_LOG_ERROR) printf("[E %s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (g_log_level >= ESP_LOG_WARN)  printf("[W %s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (g_log_level >= ESP_LOG_INFO)  printf("[I %s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (g_log_level >= ESP_LOG_DEBUG) printf("[D %s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#endif /* __ESP_LOG_STUB_H__ */