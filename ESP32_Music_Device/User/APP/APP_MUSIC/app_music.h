/**
 * @file app_music.h
 * @brief 音乐数据解析（后端 JSON → music_t）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块只做一件事：把后端返回的 JSON 里的 music 字段解析成 music_t。
 *
 *       ★ 设计约定（来自后端 API.md §3.1）：
 *           「设备只读 music，人只读 labels」
 *         所以本模块【完全忽略】labels / intent / intent_raw 这些人类可读字段，
 *         省掉一大堆无用的字符串处理。
 *
 *       ★ music_t 本身的定义不在本文件，而在 Common 层的 ds_music.h ★
 *         因为 KS 合成器（Middlewares 层）也要读这个结构体，
 *         定义放 APP 层会造成 APP ↔ Middlewares 组件循环依赖。
 *         理由详见 ds_music.h 的头部注释。
 *
 *       ★ 命名：不带 App_ 前缀的 Music_xxx 是 Common 层的纯换算函数（inline），
 *         带 App_ 前缀的 App_Music_xxx 是本模块的解析接口。
 */

#ifndef __APP_MUSIC_H__
#define __APP_MUSIC_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
// 功能包含
#include "ds_music.h"       /* music_t + Music_* 换算函数 */

/*==============================================================================
 * 解析
 *============================================================================*/
/**
 * @brief 把后端响应的 JSON 解析成 music_t
 * @param json 后端返回的完整响应体（UTF-8，以 '\0' 结尾）
 * @param out  输出结构体
 * @retval  0: 成功
 * @retval -1: 参数错误
 * @retval -2: JSON 语法错误
 * @retval -3: 顶层 ok 字段不是 true（此时串口会打印后端给的 error / hint）
 * @retval -4: 缺少 music 字段，或 chord_notes 为空
 * @retval -5: 曲长超过容量上限被截断（仍可使用，但曲子不完整）
 * @note  ★ 所有数组都按「先读长度、再逐个填、超容量就截断并报告」处理 ★
 *        因为曲长由后端环境变量 TOTAL_BARS 控制（4~32），编译期不确定，
 *        硬编码长度迟早越界。
 */
int App_Music_Parse(const char *json, music_t *out);

/**
 * @brief 打印解析结果的摘要（便于串口确认后端数据是否合理）
 */
void App_Music_Dump(const music_t *m);

#endif /* __APP_MUSIC_H__ */
