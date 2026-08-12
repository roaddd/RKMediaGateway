#!/bin/sh
#
# 在 WebRTC 测试模式和外网模式之间切换默认路由。
# Firefox 会优先使用默认路由对应网卡生成 host candidate，测试模式使其选择设备网卡。
#
# 用法：sudo ./switch_webrtc_route.sh <webrtc|internet> [外网网卡] [外网网关] [设备网卡] [设备网段] [设备网卡地址]
# 示例：sudo ./switch_webrtc_route.sh webrtc
#       sudo ./switch_webrtc_route.sh internet

set -eu

MODE="${1:-}"
WAN_INTERFACE="${2:-ens33}"
WAN_GATEWAY="${3:-192.168.124.1}"
DEVICE_INTERFACE="${4:-ens37}"
DEVICE_NETWORK="${5:-192.168.1.0/24}"
DEVICE_ADDRESS="${6:-192.168.1.2}"

print_usage()
{
    echo "用法：sudo $0 <webrtc|internet> [外网网卡] [外网网关] [设备网卡] [设备网段] [设备网卡地址]" >&2
}

remove_device_default_routes()
{
    # 删除误配置的设备网卡默认路由，例如：default dev ens37 scope link metric 10。
    while ip route show default dev "$DEVICE_INTERFACE" | grep -q '^default'; do
        ip route del default dev "$DEVICE_INTERFACE"
    done
}

if [ "$(id -u)" -ne 0 ]; then
    echo "错误：请使用 sudo 执行本脚本。" >&2
    exit 1
fi

if [ "$MODE" != "webrtc" ] && [ "$MODE" != "internet" ]; then
    print_usage
    exit 1
fi

if ! ip link show dev "$WAN_INTERFACE" >/dev/null 2>&1; then
    echo "错误：找不到外网网卡：$WAN_INTERFACE" >&2
    exit 1
fi

if ! ip link show dev "$DEVICE_INTERFACE" >/dev/null 2>&1; then
    echo "错误：找不到设备网卡：$DEVICE_INTERFACE" >&2
    exit 1
fi

if ! ip -4 addr show dev "$DEVICE_INTERFACE" | grep -F "inet $DEVICE_ADDRESS/" >/dev/null 2>&1; then
    echo "错误：设备网卡 $DEVICE_INTERFACE 未配置地址 $DEVICE_ADDRESS" >&2
    exit 1
fi

# 无论处于哪种模式，访问 IPC 网段的流量都必须固定经过设备网卡。
ip route replace "$DEVICE_NETWORK" dev "$DEVICE_INTERFACE" src "$DEVICE_ADDRESS"

if [ "$MODE" = "webrtc" ]; then
    # 测试时令 Firefox 将 ens37 作为默认出口，从而生成 192.168.1.2 host candidate。
    ip route replace default dev "$DEVICE_INTERFACE" metric 10
    echo "已切换至 WebRTC 测试模式：默认路由走 $DEVICE_INTERFACE。"
    echo "注意：该模式下 Git 和外网访问不可用。测试完成后执行 internet 模式恢复。"
else
    # 恢复外网默认路由，避免 Git 等流量错误发往 IPC 网段。
    remove_device_default_routes
    ip route replace default via "$WAN_GATEWAY" dev "$WAN_INTERFACE" metric 100
    echo "已切换至外网模式：默认路由走 $WAN_INTERFACE -> $WAN_GATEWAY。"
fi

echo
echo "当前默认路由："
ip route show default
echo "当前设备路由："
ip route get "${DEVICE_NETWORK%/*}" || true
