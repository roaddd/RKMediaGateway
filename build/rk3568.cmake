# build/rk3568.cmake
# RK3568 交叉编译工具链配置
set(TOOLCHAIN_PATH "/home/topeet/source_code/linux/rk356x_linux/rk356x_linux/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin")
set(CROSS_COMPILE_PREFIX "aarch64-linux-gnu-")

# 设置目标系统为 Linux
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 设置交叉编译器
set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/${CROSS_COMPILE_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/${CROSS_COMPILE_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PATH}/${CROSS_COMPILE_PREFIX}as")
set(CMAKE_LINKER "${TOOLCHAIN_PATH}/${CROSS_COMPILE_PREFIX}ld")

# 设置编译器标志（适配RK3568，开启优化+警告）
set(CMAKE_C_FLAGS "-O2 -Wall -fPIC ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-O2 -Wall -fPIC ${CMAKE_CXX_FLAGS}")

# 设置系统根目录（可选，根据你的工具链调整）
# set(CMAKE_SYSROOT "${TOOLCHAIN_PATH}/../aarch64-linux-gnu/sysroot")

# 禁用编译器检查（交叉编译时避免报错）
# set(CMAKE_C_COMPILER_FORCED TRUE)
# set(CMAKE_CXX_COMPILER_FORCED TRUE)

# 设置查找库和头文件的路径优先级
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)