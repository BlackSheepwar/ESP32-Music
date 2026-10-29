/**
 * @file esp_random.h（离线测试桩，不属于固件）
 * @brief 用可复现的 xorshift32 代替 ESP32 的硬件随机数。
 *
 * @note  ★ 为什么要可复现 ★
 *        固件用的是真硬件随机，每次运行噪声都不同。离线测试里如果也用
 *        不可复现的随机源，一旦某个用例失败就无法重跑定位。
 *        这里固定种子，保证失败可复现。
 */
#ifndef __ESP_RANDOM_STUB_H__
#define __ESP_RANDOM_STUB_H__

#include <stdint.h>

uint32_t esp_random(void);

/** @brief 重置随机种子（测试用例之间隔离用） */
void esp_random_seed(uint32_t seed);

#endif /* __ESP_RANDOM_STUB_H__ */