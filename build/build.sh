#!/bin/bash
# build/build.sh
# Usage:
#   ./build.sh v4l2_test Debug
#   ./build.sh mpp_test Release
#   ./build.sh rtsp_gateway Release
#   ./build.sh dual_output_test Release
#   ./build.sh gb28181_device Release
#   ./build.sh all Release
#   ./build.sh clean

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${PROJECT_ROOT}/build/output
TOOLCHAIN_FILE=${PROJECT_ROOT}/build/rk3568.cmake
RK3568_TOOLCHAIN_ROOT=/home/topeet/source_code/linux/rk356x_linux/rk356x_linux/prebuilts/gcc/linux-x86/aarch64/gcc-buildroot-9.3.0-2020.03-x86_64_aarch64-rockchip-linux-gnu
RK3568_TOOLCHAIN_HOST_LIB=${RK3568_TOOLCHAIN_ROOT}/lib

BUILD_TARGET=${1:-"all"}
BUILD_TYPE=${2:-"Release"}
CROSS_COMPILE=${3:-"ON"}

if [ "${BUILD_TARGET}" = "clean" ]; then
    echo -e "${YELLOW}=== Cleaning build output ===${NC}"
    rm -rf "${BUILD_DIR}"
    echo -e "${GREEN}Clean finished${NC}"
    exit 0
fi

if [ "${BUILD_TYPE}" != "Debug" ] && [ "${BUILD_TYPE}" != "Release" ]; then
    echo -e "${RED}BUILD_TYPE must be Debug or Release${NC}"
    exit 1
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo -e "${YELLOW}=== CMake configure: target=${BUILD_TARGET}, type=${BUILD_TYPE} ===${NC}"
cmake_cmd="cmake ${PROJECT_ROOT} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DBUILD_TARGET=${BUILD_TARGET}"

if [ "${CROSS_COMPILE}" = "ON" ]; then
    export LD_LIBRARY_PATH=${RK3568_TOOLCHAIN_HOST_LIB}:${LD_LIBRARY_PATH}
    cmake_cmd="${cmake_cmd} -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
    echo -e "${YELLOW}Cross compile enabled with RK3568 toolchain${NC}"
    echo "Toolchain host library path: ${RK3568_TOOLCHAIN_HOST_LIB}"
else
    cmake_cmd="${cmake_cmd} -DCMAKE_TOOLCHAIN_FILE=''"
    echo -e "${YELLOW}Using host compiler${NC}"
fi

echo "Running: ${cmake_cmd}"
${cmake_cmd}
if [ $? -ne 0 ]; then
    echo -e "${RED}CMake configure failed${NC}"
    exit 1
fi

echo -e "${YELLOW}=== Building ===${NC}"
make -j"$(nproc)"
if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed${NC}"
    exit 1
fi

echo -e "${GREEN}=== Build succeeded ===${NC}"
echo "Artifacts: ${BUILD_DIR}"
ls -l "${BUILD_DIR}" | grep -E "v4l2_test|mpp_test|rtsp_gateway|dual_output_test|gb28181_device"
