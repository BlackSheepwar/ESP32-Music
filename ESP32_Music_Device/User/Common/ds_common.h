/**
 * @file ds_common.h
 * @brief 平台无关通用定义
 * @author 李嘉图
 * @date 2026-06-04
 *
 * @note 换 MCU 平台时这一层不动，只改 BSP 和 OS Layer。
 *       包含基础类型别名、编译器宏、通用错误码。
 *
 *==============================================================================
 * ★ 为什么这个文件叫 ds_common.h 而不是 common.h ★
 *==============================================================================
 *  原来叫 common.h，结果被 ESP-IDF 的 include 路径静默顶掉了：
 *  一旦组件的 REQUIRES 里出现 mbedtls（本工程为了 esp_crt_bundle 而依赖它），
 *  IDF 就会把 mbedtls/library/ 加进 -I 列表，而那个目录下正好有一个
 *  mbedtls 自己的 common.h。
 *
 *  #include "ds_common.h" 的查找顺序是：
 *      1. 包含它的那个源文件所在目录（User/BSP/BSP_GPIO/，没有）
 *      2. -I 列表，按顺序（mbedtls/library 排在 User/Common 之前）
 *  → 于是 GCC 老老实实包含了 mbedtls 的 common.h，本文件全程没被读，
 *    ARRAY_SIZE / ErrorCode_t 全部消失。
 *
 *  症状非常误导人：报的是「ARRAY_SIZE 隐式声明」，看起来像忘了 include，
 *  但 include 明明写着。用 `gcc -H` 打印包含树才看出来：
 *      . .../components/mbedtls/mbedtls/library/common.h
 *
 *  教训：工程自己的通用头文件要用不会和第三方库撞名的前缀（ds_ = device side）。
 *  ★ 以后新增通用头文件也请遵守这个前缀约定 ★
 */

#ifndef __DS_COMMON_H__
#define __DS_COMMON_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>

/*==============================================================================
 * 平台抽象
 *============================================================================*/
/* 计算静态数组元素个数 */
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))

/*==============================================================================
 * 通用返回状态
 *============================================================================*/
/**
 * @brief 通用返回码
 * @note  ★ 名字前面必须带 DS_ 前缀 ★
 *
 *        原来叫 ERR_OK / ERR_PARAM / ERR_TIMEOUT ... 结果是：
 *        ESP-IDF 的 lwIP 在 components/lwip/lwip/src/include/lwip/err.h 里
 *        也定义了一整套同名的枚举（ERR_OK / ERR_MEM / ERR_TIMEOUT / ...）。
 *        只要某个 .c 同时（直接或间接）包含 lwIP 和本头文件 —— 例如
 *        bsp_http.c 通过 esp_http_client / esp_netif 间接引入 lwIP ——
 *        编译器立刻报：
 *            error: redeclaration of enumerator 'ERR_OK'
 *            error: redeclaration of enumerator 'ERR_TIMEOUT'
 *
 *        这是和 common.h 撞名同一类的坑：工程自己的全局可见标识符
 *        必须带独特前缀，否则第三方库迟早撞上来。
 */
typedef enum {
    DS_OK          =  0,    // 成功
    DS_ERR_PARAM   = -1,    // 参数错误
    DS_ERR_BUSY    = -2,    // 资源忙
    DS_ERR_TIMEOUT = -3,    // 超时
    DS_ERR_LOCKED  = -4,    // 全局锁定（紧急停止/致命错误）
    DS_ERR_UNKNOWN = -5,    // 未知错误
} DsError_t;

/*==============================================================================
 * 通用工具函数（原来在 common_utils.h，已合并到本文件，避免第三个撞名风险）
 *============================================================================*/
/**
 * @brief float 格式转字符串
 * @param value    待转换的浮点数
 * @param buf      输出缓冲区（调用方确保足够大）
 * @param decimals 小数位数
 * @note  不使用 sprintf("%f")，避免依赖 libc 浮点格式化（占用大）。
 *        输出示例: value=3.14, decimals=3 → buf="3.140"
 *        ★ 实现所在的组件刻意没开 -ffast-math ★
 *          该函数靠 frac *= 10 / frac -= digit 逐位取数，
 *          -ffast-math 允许编译器按更高精度重排这些运算，可能把 3.14 打成 "3.139"。
 */
void float_to_str(float value, char *buf, int decimals);

/**
 * @brief 取「仅一位为 1」掩码中该位的序号
 * @param mask 位掩码，要求仅一位为 1
 * @retval 0~63 : 置位序号（mask 的位 0 对应返回 0）
 * @retval 0xFF : 无效（mask 为 0，或有多个位为 1）
 * @note  用于把寄存器位域、ESP-IDF pin_bit_mask 这类 one-hot 掩码还原成编号。
 *        若掩码可能有多位、而调用方只需要最低位，先做 mask & (-mask) 收敛成
 *        one-hot 再调用本函数。
 */
uint8_t bit_onehot_index(uint64_t mask);

#endif /* __DS_COMMON_H__ */