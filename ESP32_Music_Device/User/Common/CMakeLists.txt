set(src_dirs
            .)

set(include_dirs
                .)

idf_component_register(
    SRC_DIRS ${src_dirs}            # 这个组件要编译哪些 .c
    INCLUDE_DIRS ${include_dirs})   # 对外公开哪些头文件目录（别人 include 时能找到）

# 注意：这里不加 -ffast-math。
# ds_utils.c 的 float_to_str() 靠 frac *= 10 / frac -= digit 逐位取数，
# -ffast-math 会允许编译器按更高精度重新结合这些浮点运算，可能把 3.14 打成 "3.139"。
component_compile_options(-O3 -Wno-error=format -Wno-format)

# ★ 头文件命名约定（重要，别踩第二次）★
# 本组件对外只暴露 ds_common.h（原 common.h / common_utils.h 已合并）。
# 为什么不叫 common.h：一旦某个组件的 REQUIRES 里出现 mbedtls，
# IDF 会把 mbedtls/library/ 加进 -I 列表，而那里有一个 mbedtls 自己的 common.h，
# 它会静默顶掉我们的同名头文件，报出「ARRAY_SIZE 隐式声明」这种误导性错误。
# 详见 ds_common.h 的注释。★ 以后新增通用头文件请统一用 ds_ 前缀 ★
