/**
 * @file app_led.c
 * @brief LED 应用层实现（非阻塞闪烁状态机 + 图案表）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 核心思路：把「闪烁」表示成一个时长序列，用相位计数器推进，不做任何阻塞延时。
 *
 *       图案的语义（关键，看图别看代码）：
 *         steps[] 是【以毫秒为单位的亮/灭交替时长】，偶数下标 = 亮，奇数下标 = 灭。
 *         一个「周期」= repeats 轮 × (steps 顺放一遍) + 轮间 gap_ms。
 *         周期走完回到起点，除非 once = 1（只播一轮，停在末态）。
 *
 *         两个特判（为了不改图案结构体就表达常亮/常灭）：
 *           len == 0        → 常灭
 *           steps[0] == 0   → 常亮
 *
 *       举例：
 *         {100, 100}  len=2    → 亮100 灭100 反复            （快闪）
 *         {800, 800}  len=2    → 慢闪
 *         {120,150,120,1200}   → 亮120 灭150 亮120 灭1200    （双闪后长灭）
 *
 *       ★ 生命灯不在本文件的图案体系里 ★
 *         它由 Task_Heartbeat 直接调 App_LED_HeartbeatToggle() 翻转，理由见 app_led_cfg.h。
 *         本文件的 App_LED_Service() 会跳过生命灯，避免两边抢同一个引脚。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ds_common.h"
#include "app_led.h"
// 功能包含
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"     /* portMUX_TYPE / taskENTER_CRITICAL */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "APP_LED";

/*==============================================================================
 * 预置图案表
 *============================================================================*/
/* 常灭：len = 0 */
static const uint16_t PAT_OFF_STEPS[1] = {0};
const App_LED_Pattern_t APP_LED_PAT_OFF = {
    .steps = PAT_OFF_STEPS, .len = 0, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 常亮：len = 1 且 steps[0] = 0 → 特判为「亮无限久」 */
static const uint16_t PAT_ON_STEPS[1] = {0};
const App_LED_Pattern_t APP_LED_PAT_ON = {
    .steps = PAT_ON_STEPS, .len = 1, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 快闪：亮 100 / 灭 100 —— 连接中、请求中、新音频提示 */
static const uint16_t PAT_FAST_STEPS[2] = {100, 100};
const App_LED_Pattern_t APP_LED_PAT_FAST_BLINK = {
    .steps = PAT_FAST_STEPS, .len = 2, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 慢闪：亮 800 / 灭 800 —— 重连等待中、后端请求失败 */
static const uint16_t PAT_SLOW_STEPS[2] = {800, 800};
const App_LED_Pattern_t APP_LED_PAT_SLOW_BLINK = {
    .steps = PAT_SLOW_STEPS, .len = 2, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 双闪后长灭：亮120 灭150 亮120 灭1200 —— 连接失败进入退避 */
static const uint16_t PAT_DOUBLE_STEPS[4] = {120, 150, 120, 1200};
const App_LED_Pattern_t APP_LED_PAT_DOUBLE_BLINK = {
    .steps = PAT_DOUBLE_STEPS, .len = 4, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 三闪后长灭：亮120 灭120 ×3 + 长灭 —— 错误提示（非一次性） */
static const uint16_t PAT_TRIPLE_STEPS[6] = {120, 120, 120, 120, 120, 1500};
const App_LED_Pattern_t APP_LED_PAT_TRIPLE_BLINK = {
    .steps = PAT_TRIPLE_STEPS, .len = 6, .repeats = 1, .gap_ms = 0, .once = 0,
};

/* 心跳：亮 80 / 灭 920 —— 空闲但活着（本工程生命灯不走这个，留给以后用） */
static const uint16_t PAT_HEARTBEAT_STEPS[2] = {80, 920};
const App_LED_Pattern_t APP_LED_PAT_HEARTBEAT = {
    .steps = PAT_HEARTBEAT_STEPS, .len = 2, .repeats = 1, .gap_ms = 0, .once = 0,
};

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief LED 运行时实例
 * @note  只在 App_LED_Service / App_LED_Set* 里读写。这些函数都在同一个任务
 *        （Task_LED）里调用，所以单线程访问，不需要锁。
 *        ★ 例外：生命灯由另一个任务翻转，但它不走本结构体，见 App_LED_HeartbeatToggle。
 */
typedef struct {
    const BSP_GPIO_Handle_t *gpio;      // 硬件句柄
    const App_LED_Pattern_t *pattern;   // 当前图案（只存指针，不拷贝）
    uint8_t  total_steps;               // 一个周期内总步数
    uint8_t  step;                      // 当前是第几步
    uint16_t elapsed_ms;                // 当前步已过去多少毫秒
    uint8_t  level;                     // 当前电平（1 = 亮，0 = 灭，0xFF = 未知）
    uint8_t  used;                      // 该槽位是否已注册
} App_LED_Inst_t;

/*==============================================================================
 * 静态资源池
 *============================================================================*/
static App_LED_Inst_t app_led_pool[APP_LED_COUNT];
static uint8_t        app_led_count = 0;

/** @brief 是否已初始化（幂等保护，见 App_LED_Init） */
static uint8_t        s_led_inited = 0;

/*==============================================================================
 * ★ 池的并发保护 ★
 *============================================================================*/
/**
 * @brief 保护 app_led_pool 的跨核自旋锁
 *
 * @note  ★ 为什么需要它（这是审计发现的真实竞态）★
 *
 *        最初的设计假设是「只有 LED 任务会调 App_LED_*」。但实际有【三个】任务在调，
 *        而且分布在两个核心上：
 *
 *          任务              核心   优先级   调用的 LED
 *          ----------------  ----   ------   ------------------------
 *          Task_LED_Service   1      1       推进所有灯的相位（写 step/elapsed/level）
 *          Task_Key_Cmd       1      5       录音灯、音频灯
 *          Task_WiFi_Service  0      4       WiFi 灯、后端灯（经 APP_WIFI 回调）
 *
 *        `App_LED_Set` 在写 `inst->pattern / step / elapsed_ms / level` 的同时，
 *        `App_LED_Service` 正在读改写同样的字段 → 数据竞争。两个具体后果：
 *
 *          1) **相位被回退**：Service 在推进相位的中途，Set 把 step 重置为 0；
 *             接着 Service 又把它写回旧值 +1。表现为"图案换了但闪了一下旧的"，
 *             或闪烁节奏看起来卡住。
 *          2) **指针撕裂**：Service 读到 `pattern` 指针的一半（新地址）和另一半
 *             （旧地址），拼出一个野指针再解引用 → 崩溃或乱闪。
 *             ESP32-S3 上 32 位对齐指针本身是原子写的，所以这一条是理论风险；
 *             但配合第 1) 条，加锁是明确正确的做法。
 *
 *        ★ 为什么用 portMUX 自旋锁而不是 FreeRTOS 互斥量 ★
 *          两个任务在不同核心上，普通互斥量需要内核参与、还可能优先级反转
 *          （LED 任务是优先级 1 的低优先级任务，持有互斥量时会被中优先级任务抢占，
 *           导致 WiFi 任务干等）。自旋锁在这两个临界区都极短
 *          （只有几条赋值，没有函数调用、没有阻塞），是最合适的选择。
 *
 *        ★ 纪律：临界区里绝对不能做这些 ★
 *          调用 ESP_LOGx（会走串口、可能阻塞）、调用 BSP_GPIO_*（会操作寄存器，
 *          且 gpio_set_level 内部可能取别的锁 → 死锁风险）、任何 vTaskDelay。
 *          所以下面的实现把「改池状态」和「写 GPIO」拆开了：
 *          锁里只改数据，出了锁再施加到硬件。
 */
static portMUX_TYPE s_led_mux = portMUX_INITIALIZER_UNLOCKED;

/*==============================================================================
 * 点位表：逻辑编号 → BSP 句柄
 *============================================================================*/
/**
 * @brief 指示灯点位表（运行期初始化，见 App_LED_Init 末尾的填充）
 * @note  ★ 顺序必须与 app_led_cfg.h 的 App_LED_Id_t 一一对应 ★
 *        必须是可写的运行期数组：g_led_hb 等是 bsp_gpio.c 的指针变量，
 *        值在运行期才确定，不能用于初始化文件作用域的 static const 数组
 *        （C 语言会报 "initializer element is not constant"）。
 */
static const BSP_GPIO_Handle_t *s_led_table[APP_LED_COUNT];

const BSP_GPIO_Handle_t *const *App_LED_GetTable(void)
{
    return s_led_table;
}

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 校验逻辑编号
 */
static int app_led_valid_id(App_LED_Id_t id)
{
    if (id < 0 || id >= APP_LED_COUNT)
        return 0;

    /* 读 used 也要进临界区：Init 会在别的任务里写它（理论上），
     * 而且和 Service 的读形成并发读，加锁保证看到的值是一致的 */
    portENTER_CRITICAL(&s_led_mux);
    int ok = app_led_pool[id].used ? 1 : 0;
    portEXIT_CRITICAL(&s_led_mux);

    return ok;
}

/**
 * @brief 求图案一个周期的总步数
 */
static uint8_t app_led_total_steps(const App_LED_Pattern_t *p)
{
    if (p == NULL || p->len == 0)
        return 0;

    if (p->steps[0] == 0)       /* 常亮特判：只有一步 */
        return 1;

    uint16_t repeats = (p->repeats == 0) ? 1 : p->repeats;

    /* ★ 审计修复：有轮间间隙时，每轮要多算 1 步（那个间隙步）★
     * 原来漏算，导致 step 会走到 >= len*repeats 而 app_led_step_to_index
     * 的下标公式算出越界的下标。 */
    uint32_t steps_per_cycle = (uint32_t)p->len + ((p->gap_ms > 0 && repeats > 1) ? 1u : 0u);
    uint32_t total = steps_per_cycle * repeats;

    return (total > 255) ? 255 : (uint8_t)total;
}

/**
 * @brief 把「周期内第 step 步」映射到图案数组下标
 * @retval 0xFF: 该步是轮间间隙（gap），应当熄灭
 */
static uint8_t app_led_step_to_index(const App_LED_Pattern_t *p, uint8_t step)
{
    if (p == NULL || p->len == 0)
        return 0xFF;

    if (p->steps[0] == 0)       /* 常亮 */
        return 0;

    /* 轮间间隙独立占一步。
     * ★ 审计修复：原来的公式是 (step / cycle) * len + (step % cycle)，
     *   当 step >= cycle 时结果会 >= len，越界读 steps[]。
     *   而 total_steps 原本漏算了间隙那一步，所以 step 一定会走到 >= cycle。
     *   正确做法：一轮之内用 step % cycle 取下标就够了（间隙返回 0xFF），
     *   不需要再加上"已经过了几轮"的偏移 —— 因为每轮的下标序列完全一样。 */
    if (p->gap_ms > 0 && p->repeats > 1)
    {
        uint16_t cycle = (uint16_t)p->len + 1;          /* 每轮 = len 步 + 1 个间隙 */
        uint16_t in_cycle = (uint16_t)(step % cycle);
        if (in_cycle == p->len)
            return 0xFF;                                /* 该步是间隙 */
        return (uint8_t)in_cycle;
    }

    return (uint8_t)(step % p->len);
}

/**
 * @brief 取某一步的持续时长（毫秒）
 */
static uint16_t app_led_step_duration(const App_LED_Pattern_t *p, uint8_t step)
{
    if (p == NULL || p->len == 0)
        return APP_LED_TICK_MS;     /* 常灭：时长无所谓 */

    if (p->steps[0] == 0)
        return 1;                   /* 常亮：永不切换，给个 1 防止死循环 */

    if (p->gap_ms > 0 && p->repeats > 1 && (step % (p->len + 1)) == p->len)
        return p->gap_ms;

    uint8_t idx = app_led_step_to_index(p, step);
    if (idx == 0xFF)
        return p->gap_ms;

    uint16_t d = p->steps[idx];
    return (d == 0) ? APP_LED_TICK_MS : d;   /* 步长为 0 会让状态机卡住，兜成 1 个 tick */
}

/**
 * @brief 算出某个实例当前步该有的电平
 * @retval 1 = 亮，0 = 灭
 * @note  偶数下标 = 亮，奇数下标 = 灭；间隙 = 灭。LED 高电平点亮。
 *        ★ 纯计算，不碰硬件、不碰共享状态 ★
 */
static uint8_t app_led_want_level(const App_LED_Inst_t *inst)
{
    const App_LED_Pattern_t *p = inst->pattern;

    if (p == NULL || p->len == 0)
        return 0;                                   /* 常灭 */

    if (p->steps[0] == 0)
        return 1;                                   /* 常亮 */

    uint8_t idx = app_led_step_to_index(p, inst->step);
    return (idx == 0xFF) ? 0 : ((idx & 1) ? 0 : 1);
}

/**
 * @brief 把当前电平施加到硬件（★ 必须在临界区里调用 ★）
 *
 * @note  ★ 为什么必须放在锁内（审计修复 M1）★
 *
 *        原来的写法是「锁内改池状态 → 锁外写 GPIO」。这会产生一个
 *        让灯【永久停在错误电平】的交错：
 *
 *          T1(Service, core1)                     T2(App_LED_Set, core0)
 *          ─────────────────────────────────     ────────────────────────
 *          锁内算出 want=0，level=0，出锁
 *                                                锁内 pattern=ON, level=0xFF，出锁
 *                                                app_led_apply → level=1, GPIO=HIGH
 *          用【过期的快照 level=0】写 GPIO=LOW
 *          → 池里说 pattern=ON/level=1，实际灯是灭的
 *          → 之后对"常亮"图案 advance 会直接 return（不再写硬件）
 *          → 【永久停在错误状态】，直到下一次图案切换
 *
 *        修法有两条路：
 *          (a) 只让 LED 任务写 GPIO（App_LED_Set 只置脏）—— 但那样按键响应
 *              要多等一个 tick（10ms），而且 Init 时点灯也要绕一圈；
 *          (b) 把「比较 + 写硬件」整体放进同一个临界区 —— 交错就不可能发生。
 *
 *        选 (b)，因为它不改可见行为。这么做的前提是 gpio_set_level 足够轻：
 *
 *        ★ 已核对 ESP-IDF 5.4.4 的 gpio.c（审计复核后修正了措辞）★
 *          正常路径确实只是 `gpio_hal_set_level()` 直写寄存器，纳秒级、不取锁。
 *          但严格说，函数开头的 `GPIO_CHECK(...)` 展开成 `ESP_RETURN_ON_FALSE`，
 *          **参数非法时会走 ESP_LOGE（会取 console 锁）** ——
 *          也就是说这里存在一条"在自旋锁里打日志"的潜在路径。
 *
 *          本工程的调用点让这条路径不可达：LED 句柄都是编译期硬编码的
 *          `g_led_*` 常量，且都经过 `BSP_GPIO_IsOutput()` 校验，
 *          `gpio_num` 恒为合法输出脚。所以实务上安全。
 *
 *          ⚠️ 但措辞必须诚实：不能断言"gpio_set_level 绝不取锁"。
 *             若将来有调用者传了非法/未初始化的句柄，这里就会变成
 *             自旋锁内打日志（可能导致长时间关中断甚至死锁）。
 *             保持这个前提成立的办法：只把 `g_led_*` 常量句柄交给本函数。
 */
static void app_led_apply_locked(App_LED_Inst_t *inst)
{
    uint8_t on = app_led_want_level(inst);

    if (on != inst->level)
    {
        inst->level = on;

        if (inst->gpio != NULL)
            BSP_GPIO_SetLevel(inst->gpio, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief LED 应用层初始化
 */
void App_LED_Init(void)
{
    /* ★ 幂等保护：重复调用必须直接返回 ★
     * 实测踩过：main.c 里调了一次，Task_LED_Service 里又调了一次，
     * 于是串口出现两遍 "init done"，而且第二次会把所有灯重新置为常灭 ——
     * 如果那时已经有灯在按图案闪（比如 WiFi 灯刚开始快闪），就被无声地灭掉了。
     * 加上这个判断后，谁先调用谁负责初始化，后来者不会破坏已有状态。
     * 顺带也省掉了几次多余的 gpio_config() 日志（原日志里能看到重复的 GPIO 配置行）。 */
    if (s_led_inited)
        return;

    /* 只配置指示灯那一段引脚，不碰按键（按键归 APP_KEY 初始化） */
    BSP_GPIO_InitOutputs();

    /* 填点位表：逻辑编号 → BSP 具名句柄 */
    s_led_table[APP_LED_HB]      = g_led_hb;
    s_led_table[APP_LED_WIFI]    = g_led_wifi;
    s_led_table[APP_LED_MIC]     = g_led_mic;
    s_led_table[APP_LED_AUDIO]   = g_led_audio;
    s_led_table[APP_LED_BACKEND] = g_led_backend;
    s_led_table[APP_LED_SPARE]   = g_led_spare;

    memset(app_led_pool, 0, sizeof(app_led_pool));
    app_led_count = 0;

    uint8_t ok = 0;
    for (int i = 0; i < APP_LED_COUNT; i++)
    {
        const BSP_GPIO_Handle_t *h = s_led_table[i];
        if (h == NULL || !BSP_GPIO_IsOutput(h))
        {
            ESP_LOGE(TAG, "led %d: invalid handle or not an output", i);
            continue;
        }

        App_LED_Inst_t *inst = &app_led_pool[i];
        inst->gpio    = h;
        inst->pattern = &APP_LED_PAT_OFF;
        inst->used    = 1;
        inst->level   = 0xFF;                   /* 强制第一次 apply 一定写硬件 */

        /* 上电确定态：全部熄灭（高电平点亮 → 写低） */
        BSP_GPIO_SetLevel(h, GPIO_LEVEL_LOW);
        inst->level = 0;

        app_led_count++;
        ok++;
    }

    s_led_inited = 1;

    ESP_LOGI(TAG, "init done: %u/%d LEDs ready (active-high, all OFF)",
             ok, (int)APP_LED_COUNT);
}

/*==============================================================================
 * 图案控制
 *============================================================================*/
int App_LED_Set(App_LED_Id_t id, const App_LED_Pattern_t *pattern)
{
    if (!app_led_valid_id(id) || pattern == NULL)
        return -1;

    App_LED_Inst_t *inst = &app_led_pool[id];
    uint8_t total = app_led_total_steps(pattern);

    /* ★ 临界区：只改池里的状态，不碰 GPIO、不打日志 ★
     *   这样 Service 不会看到"图案换了但 step 还是旧的"这种中间态。 */
    portENTER_CRITICAL(&s_led_mux);
    inst->pattern     = pattern;
    inst->total_steps = total;
    inst->step        = 0;
    inst->elapsed_ms  = 0;
    inst->level       = 0xFF;       /* 强制重新算一次电平 */

    /* ★ 在同一个临界区里把电平也写下去（审计修复 M1）★
     * 不能"锁内改状态、锁外写 GPIO"—— 那会让 Service 用过期快照覆盖掉
     * 这里的写入，灯永久停在错误电平。理由见 app_led_apply_locked 的注释。 */
    app_led_apply_locked(inst);

    portEXIT_CRITICAL(&s_led_mux);

    return 0;
}

int App_LED_SetOnOff(App_LED_Id_t id, uint8_t on)
{
    return App_LED_Set(id, on ? &APP_LED_PAT_ON : &APP_LED_PAT_OFF);
}

/**
 * @brief 静态闪烁图案槽
 * @note  图案结构体必须常驻（App_LED_Set 只存指针不拷贝），所以用静态槽。
 *        每个逻辑灯一个固定槽位 → 同一个灯连续触发不会无限增长。
 */
static uint16_t          s_flash_steps[APP_LED_FLASH_SLOTS][APP_LED_FLASH_MAX_STEPS];
static App_LED_Pattern_t s_flash_pat[APP_LED_FLASH_SLOTS];

int App_LED_FlashEx(App_LED_Id_t id, uint8_t count, uint8_t end_on)
{
    if (!app_led_valid_id(id) || count == 0)
        return -1;

    /* 步数上限：count 次亮 + count 次灭 + 1 个末态步 */
    if (count > (APP_LED_FLASH_MAX_STEPS - 1) / 2)
        count = (APP_LED_FLASH_MAX_STEPS - 1) / 2;

    uint8_t slot = (uint8_t)id % APP_LED_FLASH_SLOTS;
    uint16_t *steps = s_flash_steps[slot];

    /* ★ 写图案槽必须在临界区里（审计修复）★
     *
     * 原因：`app_led_pool[id].pattern` 会指向 `s_flash_pat[slot]`，
     * 而 `App_LED_Service` / `App_LED_Set` 在**持锁**状态下会解引用
     * `pattern->steps[...]` / `pattern->len`。这边如果在锁外改它们，
     * 那边就可能读到"新 len + 旧 steps 内容"这种半更新状态
     * （后果轻微 —— 某轮闪烁下标跳一下，不会越界 —— 但仍是真数据竞争）。
     *
     * ★ 注意：这里不能简单地"在 App_LED_Set 外面再包一层锁" ★
     *   `portENTER_CRITICAL` **不递归**，套两层会直接死锁。
     *   所以做法是：先把图案准备好（锁内），出锁后再调 App_LED_Set
     *   （它会自己再进一次锁去改池状态）。两次临界区是顺序的，不嵌套。 */
    portENTER_CRITICAL(&s_led_mux);

    uint8_t n = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        steps[n++] = 120;               /* 亮 */
        steps[n++] = 150;               /* 灭（两次闪之间的间隔） */
    }

    /* ★★★ 末态必须由调用方指定（实测踩过的坑）★★★
     *
     * 旧实现把末态写死成"灭"（最后一步 700ms 灭，once=1 停在末态）。
     * 于是调用方这段代码达不到预期：
     *      App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_ON);            // 有音频 → 常亮
     *      App_LED_Flash(APP_LED_AUDIO, APP_LED_AUDIO_FLASH_CNT);  // 新音频 → 闪三下
     * 第二行整体替换掉第一行的图案，且闪完停在灭 →
     * 用户看到"闪三下然后灯灭了"，误以为没有可播放音频。
     *
     * ★★ 末态步数必须凑对奇偶（这里也踩过）★★
     *    下标规则是「偶数 = 亮，奇数 = 灭」（见 app_led_want_level）。
     *    而索引是 `step % len`，所以「最后一步的下标」= len-1。
     *    要停在亮 → len-1 必须是偶数 → len 要是**奇数**。
     *    要停在灭 → len-1 必须是奇数 → len 要是**偶数**。
     *
     *    第一版只补了 1 步，于是 count=2/end_on=0 得到 len=5（奇数）
     *    → 最后一步下标 4 是偶数 → **闪完停在亮**，和意图相反。
     *    （这个错误是被离线测试 tools/led_test.c 测试 7b 抓到的。）
     *
     *    正确做法：
     *      end_on = 1（停在亮）→ 补 1 步（0 = 该步一直亮）→ len 为奇数 ✔
     *      end_on = 0（停在灭）→ 补 2 步（灭 + 灭）→ len 为偶数 ✔
     *    steps 值 0 是特判，表示"该步一直亮"（见 app_led_want_level）。 */
    if (end_on)
    {
        steps[n++] = 0;                 /* 末态：一直亮 */
    }
    else
    {
        /* 补两步而不是一步，目的是把 len 凑成偶数（否则末步会落在"亮"的下标上） */
        steps[n++] = 150;               /* 倒数第二步：灭 */
        steps[n++] = 200;               /* 末步：灭（2N+2 步，len 为偶数） */
    }

    s_flash_pat[slot].steps   = steps;
    s_flash_pat[slot].len     = n;
    s_flash_pat[slot].repeats = 1;
    s_flash_pat[slot].gap_ms  = 0;
    s_flash_pat[slot].once    = 1;      /* 只播一轮，停在末态 */

    portEXIT_CRITICAL(&s_led_mux);

    ESP_LOGD(TAG, "led %d flash x%u -> end_%s (%u steps)",
             (int)id, count, end_on ? "ON" : "OFF", n);
    return App_LED_Set(id, &s_flash_pat[slot]);
}

int App_LED_Flash(App_LED_Id_t id, uint8_t count)
{
    return App_LED_FlashEx(id, count, 0);
}

const App_LED_Pattern_t *App_LED_Get(App_LED_Id_t id)
{
    if (!app_led_valid_id(id))
        return NULL;
    return app_led_pool[id].pattern;
}

uint8_t App_LED_IsOn(App_LED_Id_t id)
{
    if (!app_led_valid_id(id))
        return 0;
    return (app_led_pool[id].level == 1) ? 1 : 0;
}

void App_LED_AllOff(void)
{
    for (int i = 0; i < APP_LED_COUNT; i++)
    {
        if (app_led_pool[i].used)
            App_LED_Set((App_LED_Id_t)i, &APP_LED_PAT_OFF);
    }
}

/*==============================================================================
 * 周期服务
 *============================================================================*/
/**
 * @brief LED 状态机推进
 * @note  ★ 一个容易写错的点：如果一次调用的间隔大于某一步的时长（比如任务被
 *          高优先级任务挤了 300ms，而当前步只有 100ms），不能只走一步就返回，
 *          否则闪烁节奏会被永久拖慢。所以这里用 while 把欠下的时间一次走完，
 *          并且加了 total_steps 上限防止死循环。
 *       ★ 跳过生命灯：它由 Task_Heartbeat 独立翻转，这里再推会两边抢引脚。
 */
/**
 * @brief 推进一个灯的相位（★ 必须在临界区里调用 ★）
 * @param inst 实例
 * @retval 1 : 需要写硬件（相位或电平变了）
 * @retval 0 : 不需要写硬件
 * @note  ★ 这个函数【只改池里的数据、不碰 GPIO】★
 *        设计成这样是为了把"决定灯该是什么电平"和"真的去写寄存器"分开。
 *        GPIO 写入在 app_led_apply_locked() 里 —— 它同样必须在临界区内调用，
 *        两者的调用点都安排在同一段 portENTER_CRITICAL 里。
 *        （历史：最初是"锁内改状态、锁外写 GPIO"，那会产生交错让灯永久停错，
 *          审计后已改为锁内一并完成，详见 app_led_apply_locked 的注释。）
 */
/**
 * @brief 推进一个灯的相位（★ 必须在临界区里调用 ★）
 * @param inst 实例
 * @retval 1 : 相位发生了推进（调用方应接着调 app_led_apply_locked）
 * @retval 0 : 不需要处理（常亮/常灭/未注册）
 *
 * @note  ★★ 这个函数【绝对不能】读写 inst->level（踩过血的教训）★★
 *
 *        它曾经在末尾自己算了一次目标电平、并在"电平变了"时把
 *        `inst->level` 更新掉然后返回 1。而调用方 App_LED_Service 是这样写的：
 *
 *            if (app_led_advance_locked(inst))     // 内部已把 level 改成新值
 *                app_led_apply_locked(inst);       // 再比一次：on == level → 不写 GPIO
 *
 *        → **电平永远写不到硬件**。
 *        后果：PAT_ON（常亮）和 PAT_OFF（常灭）看起来正常（它们被上面的早退拦住、
 *        level 交给 apply 写），但**所有闪烁图案全部失效** —— 灯只会停在第一次
 *        算出来的电平上。
 *        真机现象就是「WiFi 断线后状态机切到 RETRY_WAIT，但 GPIO5 一直常亮不闪」，
 *        和 PAT_SLOW_BLINK 无关，是引擎坏了。
 *
 *        ★ 正确分工 ★
 *          本函数：只推进 step / elapsed_ms（纯相位）
 *          app_led_apply_locked：算目标电平 + 比较 + 写 GPIO（唯一碰 level 的地方）
 *        职责单一，就不会出现"两份电平判断互相吃掉对方结果"的问题。
 */
static int app_led_advance_locked(App_LED_Inst_t *inst)
{
    const App_LED_Pattern_t *p = inst->pattern;

    if (p == NULL || !inst->used)
        return 0;

    /* 常亮 / 常灭不推进相位（它们的电平由 app_led_apply_locked 的特判直接给出） */
    if (p->len == 0 || p->steps[0] == 0)
        return 0;

    inst->elapsed_ms = (uint16_t)(inst->elapsed_ms + APP_LED_TICK_MS);

    /* ★ 用 while 把"欠下的时间"一次走完 ★
     * 如果一次调用的间隔大于某一步时长（任务被挤了 300ms，而当前步只有 100ms），
     * 只走一步会让闪烁节奏被永久拖慢。guard 用 total_steps+1 防死循环。 */
    /* ★ guard 用 uint16 避免 total_steps==255 时 +1 回绕成 0 导致图案卡死 ★ */
    uint16_t guard = (uint16_t)inst->total_steps + 1;
    while (guard-- > 0)
    {
        uint16_t dur = app_led_step_duration(p, inst->step);
        if (inst->elapsed_ms < dur)
            break;

        inst->elapsed_ms = (uint16_t)(inst->elapsed_ms - dur);

        if (inst->total_steps == 0)
            break;

        inst->step++;

        if (inst->step >= inst->total_steps)
        {
            if (p->once)
            {
                /* 只播一轮：停在最后一步（末态通常是灭），不再推进 */
                inst->step = (uint8_t)(inst->total_steps - 1);
                inst->elapsed_ms = 0;
                break;
            }
            inst->step = 0;
        }
    }

    /* ★ 只回一句"相位推进过了"，不碰 level ★
     *   是否真的需要写 GPIO，由 app_led_apply_locked 比较后决定
     *   （它也比较 inst->level，所以电平没变时同样不会多写寄存器）。 */
    return 1;
}

/**
 * @brief LED 状态机推进
 * @note  ★ 并发说明（审计后加固）★
 *        本函数由 Task_LED_Service（core 1, prio 1）调用，
 *        而 App_LED_Set 由 Task_Key_Cmd（core 1）和 Task_WiFi_Service（core 0）调用。
 *        所以「推进相位」和「切换图案」必须互斥，否则会出现
 *        "图案换了但 step 被 Service 用旧值覆盖"的相位回退。
 *
 *        ★ 审计后加固（M1）★
 *          最初是"锁内推进相位、锁外写 GPIO"，但那样仍有一个交错会让灯
 *          【永久停在错误电平】（Service 用过期快照覆盖 App_LED_Set 的写入）。
 *          现在已经把「推进相位 + 比较电平 + 写 GPIO」全部放进同一个临界区。
 *          这么做的前提是 gpio_set_level 内部不取锁 —— 已核对 ESP-IDF 源码确认。
 *          详见 app_led_apply_locked() 的注释。
 *
 *       ★ 跳过生命灯：它由 Task_Heartbeat 独立翻转，这里再推会两边抢引脚 ★
 */
void App_LED_Service(void)
{
    for (int i = 0; i < APP_LED_COUNT; i++)
    {
#if APP_LED_HB_SEPARATE_TASK
        if (i == APP_LED_HB)
            continue;               /* 生命灯不归本状态机管 */
#endif
        App_LED_Inst_t *inst = &app_led_pool[i];

        /* ★ 锁内完成"推进相位 + 比较电平 + 写硬件"（审计修复 M1）★
         * 三段必须在一个临界区里：只要中间出锁，App_LED_Set 就可能插进来
         * 改掉 level，而这边再用旧值写 GPIO → 池状态与硬件不一致，
         * 对"常亮/常灭"图案来说就是【永久停错】。 */
        portENTER_CRITICAL(&s_led_mux);

        if (app_led_advance_locked(inst))
            app_led_apply_locked(inst);

        portEXIT_CRITICAL(&s_led_mux);
    }
}
/*==============================================================================
 * 生命灯
 *============================================================================*/
/**
 * @brief 生命灯翻转一次
 * @note  ★ 整个函数只有一次 BSP 调用，没有状态机、没有时间比较、没有日志 ★
 *        这是故意的：它的代码路径越短，就越不可能「因为它自己出错而停」，
 *        于是「它停了」才真的能说明系统出了问题。
 *        ★ 不用 BSP_GPIO_Toggle() 而是读回再写，是因为 Toggle 内部也是读回再写，
 *          但这里我们想显式控制，避免以后 Toggle 语义变化影响诊断信号的可靠性。
 */
void App_LED_HeartbeatToggle(void)
{
    if (s_led_table[APP_LED_HB] == NULL)
        return;

    uint8_t lv = BSP_GPIO_ReadLevel(s_led_table[APP_LED_HB]);
    if (lv == 0xFF)
        lv = 0;

    BSP_GPIO_SetLevel(s_led_table[APP_LED_HB], lv ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH);
}
