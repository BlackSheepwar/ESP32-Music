set(src_dirs
            APP_AUDIO
            APP_KEY
            APP_LED
            APP_LEDTEST
            APP_MUSIC
            APP_WIFI)

set(include_dirs
                APP_AUDIO
                APP_KEY
                APP_LED
                APP_LEDTEST
                APP_MUSIC
                APP_WIFI)

set(requires
            BSP
            Middlewares     # APP_MUSIC 通过 app_music_app.c 用 KS 合成器
            Common
            json)           # APP_MUSIC 用 cJSON 解析后端响应

idf_component_register(
    SRC_DIRS ${src_dirs}            # 这个组件要编译哪些 .c
    INCLUDE_DIRS ${include_dirs}    # 对外公开哪些头文件目录
    REQUIRES ${requires})           # 依赖哪些其它组件
component_compile_options(-O2 -Wno-error=format -Wno-format)
