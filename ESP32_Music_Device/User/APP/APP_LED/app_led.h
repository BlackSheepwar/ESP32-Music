/**
 * @file app_led.h
 * @brief LED 应用层（句柄式接口 + 非阻塞闪烁状态机）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 业务层用「逻辑编号」控制灯，不直接抓句柄：
 *
 *           App_LED_Set(APP_LED_MIC, &APP_LED_PAT_ON);      // 开始录音，录音灯亮
 *           App_LED_Set(APP_LED_MIC, &APP_LED_PAT_OFF);     // 停止录音
 *           App_LED_Flash(APP_LED_AUDIO, APP_LED_AUDIO_FLASH_CNT);  // 新音频，闪三下
 *
 *       ★ 为什么要有这一层？
 *         直接在业务代码里写 vTaskDelay + Toggle 会阻塞调用者，
 *         而且同一个灯被两处控制时状态会打架。这里把「闪烁」抽象成
 *         「一个周期图案 + 一个相位计数器」，由 Task 层每 10ms 调一次
 *         App_LED_Service() 推进相位，全程非阻塞，业务任务永远不等待。
 *
 *       ★ 图案表的写法见 app_led.c 顶部说明，加新图案只需在表里加一行。
 *       ★ 点位与语义见 app_led_cfg.h。硬件约定：LED 高电平点亮。
 */

#ifndef __APP_LED_H__
#define __APP_LED_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
// 功能包含
#include "app_led_cfg.h"

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief LED 图案
 * @note  图案是「以 ms 为单位的亮/灭交替时长序列」，循环播放：
 *          常亮   {0}                      —— 只亮不灭
 *          常灭   {}  (len = 0)            —— 全灭
 *          快闪   {100, 100}               —— 亮 100 / 灭 100
 *
 *        特判约定（为了少加一个 enum，图案表保持纯数据）：
 *          len    == 0        → 常灭
 *          steps[0] == 0      → 常亮
 */
typedef struct {
    const uint16_t *steps;      // 时长数组（ms），偶数下标为亮、奇数下标为灭
    uint8_t         len;        // 数组长度（0 = 常灭）
    uint8_t         repeats;    // 一个周期内重复几轮（1 = 不重复）
    uint16_t        gap_ms;     // 每轮之间的熄灭间隔（repeats > 1 时才有意义）
    uint8_t         once;       // 1 = 只播一轮然后停在末态；0 = 无限循环
} App_LED_Pattern_t;

/*==============================================================================
 * 预置图案（业务层直接用这套，不要自己造）
 *============================================================================*/
extern const App_LED_Pattern_t APP_LED_PAT_OFF;          /* 常灭 */
extern const App_LED_Pattern_t APP_LED_PAT_ON;           /* 常亮 */
extern const App_LED_Pattern_t APP_LED_PAT_FAST_BLINK;   /* 快闪：亮100 / 灭100 */
extern const App_LED_Pattern_t APP_LED_PAT_SLOW_BLINK;   /* 慢闪：亮800 / 灭800 */
extern const App_LED_Pattern_t APP_LED_PAT_DOUBLE_BLINK; /* 双闪后长灭：连接失败用 */
extern const App_LED_Pattern_t APP_LED_PAT_TRIPLE_BLINK; /* 三闪后长灭：新音频/错误提示用 */
extern const App_LED_Pattern_t APP_LED_PAT_HEARTBEAT;    /* 心跳：短亮80 + 长灭920 */

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief LED 应用层初始化
 * @note  内部调用 BSP_GPIO_InitOutputs() 配置 6 个指示灯引脚，并把所有灯置为常灭，
 *        保证上电状态确定（避免复位瞬间的随机电平让灯乱亮）。
 *
 *        ★ 本函数是幂等的，重复调用会直接返回 ★
 *          实测踩过：main.c 调一次、Task_LED_Service 又调一次，
 *          第二次会把已经在闪的灯重新置灭，表现为「WiFi 灯该闪却不闪」。
 *          所以谁先调用谁负责初始化，后来者不破坏已有状态。
 */
void App_LED_Init(void);

/*==============================================================================
 * 图案控制（业务层接口）
 *============================================================================*/
/**
 * @brief 按逻辑编号设置图案（从相位 0 重新开始）
 * @param id      逻辑 LED 编号（App_LED_Id_t）
 * @param pattern 图案指针（用预置图案，或调用方自己的结构体 —— 必须一直存活）
 * @retval 0: 成功
 * @retval -1: 参数错误（编号越界或图案为 NULL）
 * @note  重复设置同一个图案会重启相位。需要「不重启」的场合先用 App_LED_Get 比一下。
 */
int App_LED_Set(App_LED_Id_t id, const App_LED_Pattern_t *pattern);

/**
 * @brief 按逻辑编号设置常亮 / 常灭
 * @param id 逻辑 LED 编号
 * @param on 1 = 常亮，0 = 常灭
 * @retval 0: 成功
 * @retval -1: 参数错误
 */
int App_LED_SetOnOff(App_LED_Id_t id, uint8_t on);

/**
 * @brief 闪 N 次，然后停在指定末态
 * @param id       逻辑 LED 编号
 * @param count    闪烁次数（1 ~ APP_LED_FLASH_MAX_STEPS/2）
 * @param end_on   末态：1 = 闪完留在常亮，0 = 闪完留在常灭
 * @retval 0: 成功
 * @retval -1: 参数错误
 *
 * @note  ★ 为什么需要 end_on（实测踩过的坑）★
 *        原来只有「闪 N 次 → 常灭」一种语义，于是下面这段**调用方代码
 *        根本达不到预期效果**：
 *            App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_ON);            // 有音频 → 常亮
 *            App_LED_Flash(APP_LED_AUDIO, APP_LED_AUDIO_FLASH_CNT);  // 新音频 → 闪三下
 *        第二行会把第一行刚设好的 PAT_ON **整体替换掉**，
 *        而且旧实现的末态是"灭" —— 用户看到的是"闪三下然后灯灭了"，
 *        误以为没有音频。
 *
 *        现在用 end_on 表达完整意图：
 *          · 有新音频可播 → end_on = 1（闪三下 → 常亮）
 *          · "没有音频"的提示 → end_on = 0（闪两下 → 灭）
 *        一次调用即可，不再需要"先 Set 再 Flash"那种会互相覆盖的写法。
 *
 * @note  内部按 count 生成「亮-灭」交替的图案，末尾补一个末态步。
 *        图案结构体必须常驻，所以用一张静态图案槽（同一个灯连续触发会复用
 *        同一个槽位，不会无限增长）。每个灯一个槽，最多 APP_LED_FLASH_SLOTS 个。
 */
int App_LED_FlashEx(App_LED_Id_t id, uint8_t count, uint8_t end_on);

/**
 * @brief 闪 N 次，然后回到常灭（= App_LED_FlashEx(id, count, 0)）
 * @param id    逻辑 LED 编号
 * @param count 闪烁次数（1 ~ APP_LED_FLASH_MAX_STEPS/2）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  用于「提示一下然后回到无状态」的场合，例如按 KEY2 但还没有音频时闪两下。
 */
int App_LED_Flash(App_LED_Id_t id, uint8_t count);

/** @brief 闪灯图案槽的数量（每个槽服务一个逻辑灯） */
#define APP_LED_FLASH_SLOTS     6

/** @brief 单个闪烁图案支持的最大步数（= 2 × 最大闪烁次数 + 1 个收尾） */
#define APP_LED_FLASH_MAX_STEPS 17

/**
 * @brief 查询 LED 当前图案
 * @param id 逻辑 LED 编号
 * @retval 当前图案指针（编号越界时为 NULL）
 */
const App_LED_Pattern_t *App_LED_Get(App_LED_Id_t id);

/**
 * @brief 查询 LED 当前是否处于点亮状态
 * @param id 逻辑 LED 编号
 * @retval 1: 亮
 * @retval 0: 灭 / 编号越界
 */
uint8_t App_LED_IsOn(App_LED_Id_t id);

/**
 * @brief 把所有灯置为常灭（复位用）
 */
void App_LED_AllOff(void);

/*==============================================================================
 * 周期服务
 *============================================================================*/
/**
 * @brief LED 状态机推进（必须按 APP_LED_TICK_MS 周期调用）
 * @note  建议放在 Task_LED 里，周期 10ms。本函数不会阻塞，也不会动态分配内存。
 *        生命灯（APP_LED_HB）不走图案表，由 Task_Heartbeat 直接翻转，见 app_led_cfg.h。
 */
void App_LED_Service(void);

/**
 * @brief 生命灯翻转一次（由独立任务按 APP_LED_HB_PERIOD_MS 调用）
 * @note  ★ 不做任何状态机判断，纯粹翻转 ★
 *        这正是它的价值：它的代码路径短到几乎不可能自己出错，
 *        所以「它停了」才真的说明系统调度出了问题。
 */
void App_LED_HeartbeatToggle(void);

#endif /* __APP_LED_H__ */
