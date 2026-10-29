/**
 * @file ds_utils.c
 * @brief 通用工具函数实现
 * @author 李嘉图
 * @date 2026-07-07
 *
 * @note 本组件刻意不加 -ffast-math（理由见 ds_common.h 里 float_to_str 的说明：
 *       该函数靠 frac *= 10 / frac -= digit 逐位取数，
 *       -ffast-math 允许编译器高精度重排这些运算，可能把 3.14 打成 "3.139"）。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stdio.h>
// 功能包含
#include "ds_common.h"

/*==============================================================================
 * 格式转换函数
 *============================================================================*/
/**
 * @brief float 格式转字符串
 * @param value    待转换的浮点数
 * @param buf      输出缓冲区
 * @param decimals 小数位数
 */
void float_to_str(float value, char *buf, int decimals)
{
    if (value < 0) {
        *buf++ = '-';
        value = -value;
    }

    int int_part = (int)value;
    float frac = value - int_part;

    sprintf(buf, "%d", int_part);
    while (*buf) buf++;

    *buf++ = '.';

    for (int i = 0; i < decimals; i++)
    {
        frac *= 10;
        int digit = (int)frac;
        *buf++ = digit + '0';
        frac -= digit;
    }

    *buf = '\0';
}

/*==============================================================================
 * 位运算工具
 *============================================================================*/
/**
 * @brief 取「仅一位为 1」掩码中该位的序号
 * @param mask 位掩码，要求仅一位为 1
 * @retval 0~63 : 置位序号（mask 的位 0 对应返回 0）
 * @retval 0xFF : 无效（mask 为 0，或有多个位为 1）
 * @note  mask & (mask - 1) 会清掉最低的置位，结果非 0 即说明还有别的位，
 *        用这一步在越界前挡掉多位掩码，避免语义不明确。
 *        序号用 GCC/Clang 内建 __builtin_ctzll 取最低置位的位置。
 */
uint8_t bit_onehot_index(uint64_t mask)
{
    if (mask == 0)
        return 0xFF;

    /* 只允许一位为 1，否则语义不明确 */
    if ((mask & (mask - 1)) != 0)
        return 0xFF;

    return (uint8_t)__builtin_ctzll(mask);
}
