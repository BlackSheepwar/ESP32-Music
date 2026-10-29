/**
 * @file app_music_app.h
 * @brief 音乐业务应用层（上传录音 → 解析 → KS 合成 → 播放）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块是「一次完整交互」的编排者，把四个模块串成一条链：
 *
 *      录音(APP_AUDIO) ──WAV──▶ 上传(BSP_HTTP) ──JSON──▶ 解析(APP_MUSIC)
 *                                                            │
 *                                    播放(APP_AUDIO) ◀── KS合成(Middlewares/KS)
 *
 *       ★ 命名说明：本文件叫 app_music_app.h 而不是 app_music.h，
 *         因为 app_music.h 已经被「数据模型 + JSON 解析」占用了。
 *         两个文件的分工：
 *           app_music.h / .c      —— 静态的数据结构与解析（无状态）
 *           app_music_app.h / .c  —— 有状态的业务流程编排
 */

#ifndef __APP_MUSIC_APP_H__
#define __APP_MUSIC_APP_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
// 功能包含
#include "app_music.h"
#include "app_audio.h"

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 上传结果
 */
typedef enum {
    APP_MUSIC_OK = 0,               // 成功拿到音乐数据
    APP_MUSIC_ERR_WIFI,             // WiFi 没连上
    APP_MUSIC_ERR_NO_URL,           // 后端地址没配置（还是占位符）
    APP_MUSIC_ERR_NO_AUDIO,         // 录音是空的（没录到东西）
    APP_MUSIC_ERR_HTTP,             // HTTP 请求失败（连不上/超时/非200）
    APP_MUSIC_ERR_PARSE,            // JSON 解析失败
    APP_MUSIC_ERR_BUSY_SKIP,        // ★ 本次跳过（后端正被其它任务占用），不是失败 ★
    APP_MUSIC_ERR_SUPERSEDED,       // ★ 被新的上传取代（用户重新录了），不是失败 ★
} App_Music_Result_t;

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief 音乐业务初始化
 * @retval 0: 成功
 * @retval -1: 失败
 * @note  做三件事：
 *          1. BSP_HTTP_Init + 把 app_music_cfg.h 里的 URL/token 宏装进去
 *          2. 在 PSRAM 里分配「响应缓冲 + music_t」（都不小，不能放内部 RAM）
 *          3. 打印配置状态（特别是 URL 是否还是占位符）
 *        ★ 必须在 App_WiFi_Init 之后调用（HTTP 层不依赖 WiFi 初始化，
 *          但顺序清晰一点好排查）★
 */
int App_MusicApp_Init(void);

/*==============================================================================
 * 后端探活（驱动「后端连通」指示灯）
 *============================================================================*/
/**
 * @brief 探活一次后端 /api/v1/health
 * @retval 0: 后端可达
 * @retval -1: 不可达（返回码含义见串口日志）
 * @note  ★ 这是「是否连接后端」指示灯的数据来源 ★
 *        用 health 而不是自己发一个 POST：不调 AI、不耗额度、响应极小。
 *        ★ 本函数会阻塞（最长 30 秒超时），只能由专门的探活任务调用，
 *          不要放在按键任务里 ★
 */
int App_MusicApp_ProbeBackend(void);

/**
 * @brief 查询后端是否已配置过地址
 * @retval 1: 已配置
 * @retval 0: 还是占位符
 */
uint8_t App_MusicApp_IsBackendConfigured(void);

/*==============================================================================
 * 主流程：上传录音并准备播放
 *============================================================================*/
/**
 * @brief 把当前录音上传后端，解析响应，准备 KS 合成
 * @retval APP_MUSIC_OK: 成功（之后可以 App_MusicApp_Play 播放）
 * @retval 其它: 失败原因
 *
 * @note  ★ 阻塞函数（最长 30 秒），必须由业务任务调用 ★
 *
 *        内部流程：
 *          1. 检查 WiFi + URL 配置
 *          2. App_Audio_BuildRecordWav() —— 把录音封装成 44 字节头 + PCM
 *          3. BSP_HTTP_PostWav() —— 上传，接收 16KB 响应
 *          4. App_Music_Parse() —— 解析出 music_t
 *          5. KS_Synth_Init() —— 用 music_t 初始化合成器（换算衰减系数等）
 *          6. 返回成功 —— 此时「有可播放音频」灯应当点亮并闪三下
 */
App_Music_Result_t App_MusicApp_UploadAndPrepare(void);

/*==============================================================================
 * 播放控制
 *============================================================================*/
/**
 * @brief 播放已准备好的合成音乐（循环）
 * @retval 0: 已下发播放命令
 * @retval -1: 还没有可播放的音乐
 * @note  ★ 用「回调音源」把 KS 合成器接到播放任务上 ★
 *        合成是边播边算的，不预先渲染整首曲子：
 *        42 秒 × 16kHz × 2 字节 = 1.3MB，没必要占这么多 PSRAM。
 */
int App_MusicApp_Play(void);

/**
 * @brief 是否有可播放的音频（驱动「有音频」指示灯）
 * @retval 1: 有
 * @retval 0: 没有
 */
uint8_t App_MusicApp_HasAudio(void);

/**
 * @brief 取最近一次识别出的文本（后端返回的 text 字段）
 * @retval 非 NULL: 文本
 * @retval NULL: 还没请求过
 * @note  目前只用于日志。以后要显示到屏幕/播报，从这里取。
 */
const char *App_MusicApp_GetLastText(void);

/**
 * @brief 打印最近一次解析结果摘要
 */
void App_MusicApp_Dump(void);

#endif /* __APP_MUSIC_APP_H__ */
