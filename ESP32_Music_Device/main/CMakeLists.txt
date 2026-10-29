idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP TASK Common)   # 用到 BSP（bsp_sys）、TASK（各任务入口）、Common
