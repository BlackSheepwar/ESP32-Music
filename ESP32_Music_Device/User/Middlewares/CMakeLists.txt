set(src_dirs
            KS)

set(include_dirs
                KS)

# ★ 只依赖 Common，不依赖 APP ★
#   KS 合成器是纯运算中间件：输入 music_t（定义在 Common/ds_music.h）、
#   输出 PCM。它不应该认识任何业务概念。
#   ★ 千万不要在这里加 APP ★
#     因为 APP 需要本组件（APP_MUSIC 要调 KS_Synth_*），
#     加上就会形成 APP ↔ Middlewares 的组件循环依赖，CMake 直接报错。
#     共享数据类型放 Common 就是为了避免这件事，详见 ds_music.h 的说明。
set(requires
            Common)

idf_component_register(
    SRC_DIRS ${src_dirs}            # 这个组件要编译哪些 .c
    INCLUDE_DIRS ${include_dirs}    # 对外公开哪些头文件目录
    REQUIRES ${requires})           # 依赖哪些其它组件

# ★ 必须开 -ffast-math 和 -O3 ★
#   KS 合成的内层循环是 frames × voices 次浮点乘加（16kHz 下每秒约 50 万次），
#   在 -Og（当前 sdkconfig 的调试优化等级）下会明显吃 CPU，
#   严重时 I2S DMA 供不上数据 → 爆音。
#   -ffast-math 让编译器自由重排浮点运算，对这种无精度要求的音频合成是净收益。
#   注意：Common 组件刻意没开 -ffast-math（它的 float_to_str 依赖精确舍入），
#   这里是组件级选项，只影响 KS 自己。
component_compile_options(-O3 -ffast-math -Wno-error=format -Wno-format)
