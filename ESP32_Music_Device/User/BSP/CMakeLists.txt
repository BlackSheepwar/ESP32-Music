set(src_dirs 
            BSP_GPIO
            BSP_HTTP
            BSP_I2S
            BSP_SYS
            BSP_WIFI)

set(include_dirs
                BSP_GPIO
                BSP_HTTP
                BSP_I2S
                BSP_SYS
                BSP_WIFI)

set(requires
            driver
            esp_driver_i2s
            esp_driver_gpio
            esp_timer
            esp_wifi
            esp_netif
            esp_event
            esp_psram
            esp_http_client  # bsp_http.c
            mbedtls          # esp_crt_bundle（证书 bundle）
            nvs_flash        # ★ bsp_wifi.c 用 nvs_flash_init()：WiFi 驱动依赖 NVS 存 PHY 校准 ★
            spi_flash        # bsp_sys.c 用 esp_flash_get_size() 打印 flash 容量
            Common)          # 用到 ds_common.h 的 ARRAY_SIZE 和 bit_onehot_index

idf_component_register(
    SRC_DIRS ${src_dirs}            # 这个组件要编译哪些 .c
    INCLUDE_DIRS ${include_dirs}    # 对外公开哪些头文件目录（别人 include 时能找到）
    REQUIRES ${requires})           # 我这个组件需要依赖哪些其它组件
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
