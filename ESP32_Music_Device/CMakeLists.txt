# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ★ 每个带 CMakeLists.txt 的子目录都会成为一个独立组件 ★
#   新增一层目录（如 User/APP、User/TASK）必须加到这里，
#   否则该目录下的 .c 根本不会被编译，表现为「链接时报 undefined reference」。
set(EXTRA_COMPONENT_DIRS
                User/Common
                User/Middlewares
                User/BSP
                User/APP
                User/TASK)

# ★ 源码里有大量中文注释 ★
#   不显式声明字符集时，GCC 按 locale 猜源文件编码，
#   在 GBK 环境（中文 Windows）下会把 UTF-8 的注释解成乱码，
#   严重时字符串字面量后面紧跟的代码会被吃掉，报出莫名其妙的语法错误。
#   这两个选项保证「源文件按 UTF-8 读、字符串按 UTF-8 执行」。
add_compile_options(
                -finput-charset=UTF-8
                -fexec-charset=UTF-8
                -fdiagnostics-color=always)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_Music_Device)
