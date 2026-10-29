set(src_dirs
            TASK_AUDIO
            TASK_KEY
            TASK_LED
            TASK_WIFI)

set(include_dirs
                TASK_AUDIO
                TASK_KEY
                TASK_LED
                TASK_WIFI)

set(requires
            BSP
            APP
            Middlewares    # TASK 通过 APP_MUSIC 间接用到 KS 合成器
            Common)

idf_component_register(
    SRC_DIRS ${src_dirs}            # 这个组件要编译哪些 .c
    INCLUDE_DIRS ${include_dirs}    # 对外公开哪些头文件目录
    REQUIRES ${requires})           # 依赖哪些其它组件
component_compile_options(-O2 -Wno-error=format -Wno-format)
