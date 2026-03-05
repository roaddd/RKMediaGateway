#!/bin/bash
# build/build.sh
# 传参编译脚本，用法：
# 1. 编译v4l2测试程序（Debug模式）：./build.sh v4l2_test Debug
# 2. 编译所有程序（Release模式）：./build.sh all Release
# 3. 清理编译产物：./build.sh clean

# 颜色输出（可选，方便看日志）
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 项目根目录（build.sh 所在目录的上一级）
PROJECT_ROOT=$(cd $(dirname $0)/.. && pwd)
BUILD_DIR=${PROJECT_ROOT}/build/output # 编译产物输出目录
TOOLCHAIN_FILE=${PROJECT_ROOT}/build/rk3568.cmake

# 默认参数（不传参时用默认值）
BUILD_TARGET=${1:-"all"}       # 默认编译所有
BUILD_TYPE=${2:-"Release"}     # 默认Release模式
CROSS_COMPILE=${3:-"ON"}       # 默认开启交叉编译（RK3568）

# 第一步：参数校验
if [ "${BUILD_TARGET}" = "clean" ]; then
    echo -e "${YELLOW}=== 清理编译产物 ===${NC}"
    rm -rf ${BUILD_DIR}
    echo -e "${GREEN}清理完成！${NC}"
    exit 0
fi

if [ "${BUILD_TYPE}" != "Debug" ] && [ "${BUILD_TYPE}" != "Release" ]; then
    echo -e "${RED}错误：编译模式只能是 Debug 或 Release！${NC}"
    exit 1
fi

# 第二步：创建编译目录
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}

# 第三步：执行CMake配置
echo -e "${YELLOW}=== CMake 配置（目标：${BUILD_TARGET}，模式：${BUILD_TYPE}）===${NC}"
cmake_cmd="cmake ${PROJECT_ROOT} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DBUILD_TARGET=${BUILD_TARGET}"

# 如果开启交叉编译，加载工具链文件
if [ "${CROSS_COMPILE}" = "ON" ]; then
    cmake_cmd="${cmake_cmd} -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
    echo -e "${YELLOW}启用RK3568交叉编译工具链${NC}"
else
    cmake_cmd="${cmake_cmd} -DCMAKE_TOOLCHAIN_FILE='' "
    echo -e "${YELLOW}使用本地编译器编译${NC}"
fi

# 执行CMake
echo "执行命令：${cmake_cmd}"
${cmake_cmd}
if [ $? -ne 0 ]; then
    echo -e "${RED}CMake 配置失败！${NC}"
    exit 1
fi

# 第四步：执行编译
echo -e "${YELLOW}=== 开始编译 ===${NC}"
make -j$(nproc) # -j后面跟CPU核心数，加速编译
if [ $? -ne 0 ]; then
    echo -e "${RED}编译失败！${NC}"
    exit 1
fi

# 第五步：输出结果
echo -e "${GREEN}=== 编译成功！===${NC}"
echo "编译产物路径：${BUILD_DIR}"
ls -l ${BUILD_DIR} | grep -E "v4l2_test|main" # 显示编译出的可执行文件