# RKMediaGateway 云端 WebRTC 服务部署指南

本文说明如何在 Ubuntu 22.04 云服务器上部署以下服务，并将 RK3568 设备接入云端：

- RKMediaGateway 云端 WebSocket 信令服务器；
- Nginx WebSocket 反向代理；
- coturn STUN/TURN 服务器；
- RK3568 设备端程序、配置和私密凭据。

本文使用文档专用公网示例地址 `203.0.113.10`、云主机私网示例地址 `10.0.0.7`。该公网地址不提供真实服务；执行部署和测试命令前，必须将示例地址替换为实际服务器地址。

信令服务器只负责设备登记、浏览器认证以及 SDP/ICE 消息转发，不转发 RTP/RTCP 媒体。STUN 用于发现 NAT 映射地址，TURN 在端到端路径无法打通时中继加密后的 WebRTC 数据。

## 1. 组件和网络关系

```text
RK3568 设备                         云服务器                         浏览器
-----------                       ---------                        -------
WebRTC PeerConnection  <------>   STUN/TURN :3478  <------>  WebRTC PeerConnection
        |                               |
        +-- WebSocket /device ----------+
                                        +---------- WebSocket /browser --+
                                  Nginx :18080（临时 WS）
                                        |
                              信令进程 127.0.0.1:8765
```

云服务器端口规划如下：

| 端口 | 协议 | 用途 | 建议来源范围 |
|---|---|---|---|
| `22` | TCP | SSH 管理 | 仅管理员地址 |
| `18080` | TCP | 临时明文 WS 联调入口 | 联调期间按需开放 |
| `8765` | TCP | Python 信令进程内部端口 | 不加入云安全组，仅监听回环地址 |
| `3478` | UDP/TCP | STUN/TURN 监听端口 | 需要使用 WebRTC 的客户端 |
| `49152-65535` | UDP | TURN 媒体中继端口 | 需要使用 WebRTC 的客户端 |
| `443` | TCP | 正式 WSS/HTTPS | 正式部署时开放 |

云安全组和系统防火墙是两层独立控制。即使 `ufw` 显示 `inactive`，仍需在云厂商控制台放行所需端口。

## 2. 信令服务器说明

### 2.1 代码职责

- `protocol.py`：协议版本、消息格式及字段长度校验；
- `router.py`：设备、浏览器与 `session_id` 的路由和状态机；
- `server.py`：WebSocket 连接、鉴权、保活及有界发送队列；
- `config.py`：监听端口和鉴权文件配置；
- `tests/`：路由单元测试和本机 WebSocket 集成测试。

运行环境为 Python 3.10 或更新版本，依赖由 `requirements.txt` 固定。服务固定监听 `127.0.0.1`，端口默认是 `8765`。

设备连接 `/device`，浏览器连接 `/browser`。详细消息格式参见[云端 WebRTC 信令协议 v1](../../bussiness/mediaOutput/src/webRTC/doc/信令协议/云端WebRTC信令协议.md)。

### 2.2 本地测试

在仓库根目录执行：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r cloud/signaling_server/requirements.txt
python -m unittest discover -s cloud/signaling_server/tests -v
```

Windows PowerShell 可以直接使用：

```powershell
python -m venv .venv
./.venv/Scripts/python.exe -m pip install -r cloud/signaling_server/requirements.txt
./.venv/Scripts/python.exe -m unittest discover -s cloud/signaling_server/tests -v
```

所有测试均显示 `ok`，最终结果为 `OK` 后再部署。

## 3. 在云服务器部署信令服务器

### 3.1 上传代码

从本地仓库根目录上传完整的 `cloud` 目录。普通用户通常没有 `/opt` 的写权限，因此先上传到 `/tmp`：

```bash
scp -r cloud ubuntu@203.0.113.10:/tmp/rkmgw-cloud
```

如果 `scp` 被服务器策略关闭，可以使用 `sftp`：

```text
sftp ubuntu@203.0.113.10
sftp> put -r cloud /tmp/rkmgw-cloud
sftp> bye
```

登录云服务器，确认 Python 包目录没有被压平：

```bash
test -f /tmp/rkmgw-cloud/__init__.py
test -f /tmp/rkmgw-cloud/signaling_server/__init__.py
test -f /tmp/rkmgw-cloud/signaling_server/requirements.txt
```

最终目录必须是 `/opt/rkmgw/cloud/signaling_server`，而不是 `/opt/rkmgw/cloud` 下直接出现 `server.py` 和 `requirements.txt`。否则 `python -m cloud.signaling_server` 无法按预期加载包。

### 3.2 安装系统依赖和代码

```bash
sudo apt update
sudo apt install -y python3-venv python3-pip nginx openssl

sudo useradd --system \
  --home /opt/rkmgw \
  --shell /usr/sbin/nologin \
  rkmgw-signaling 2>/dev/null || true

sudo install -d -o rkmgw-signaling -g rkmgw-signaling -m 0755 \
  /opt/rkmgw/cloud
sudo cp -a /tmp/rkmgw-cloud/. /opt/rkmgw/cloud/
sudo chown -R rkmgw-signaling:rkmgw-signaling /opt/rkmgw
```

复制命令末尾的 `/.` 表示复制 `rkmgw-cloud` 的内容，可避免额外产生一层 `cloud` 目录。首次部署前应确保 `/opt/rkmgw/cloud` 为空；后续完整升级应先备份旧版本，再替换整个目录，以免已删除的旧源码残留。

创建虚拟环境并安装依赖：

```bash
sudo -u rkmgw-signaling python3 -m venv /opt/rkmgw/.venv
sudo -u rkmgw-signaling /opt/rkmgw/.venv/bin/python -m pip install \
  -r /opt/rkmgw/cloud/signaling_server/requirements.txt

cd /opt/rkmgw
sudo -u rkmgw-signaling .venv/bin/python -m unittest \
  discover -s cloud/signaling_server/tests -v
```

### 3.3 创建信令鉴权文件

分别生成设备 Token 和浏览器测试 Token：

```bash
openssl rand -hex 32
openssl rand -hex 32
```

在仓库外创建 `/etc/rkmgw/signaling-auth.json`：

```json
{
  "device_tokens": {
    "rk3568-001": "REPLACE_WITH_RANDOM_DEVICE_TOKEN"
  },
  "browser_tokens": {
    "REPLACE_WITH_RANDOM_BROWSER_TOKEN": ["rk3568-001"]
  }
}
```

安装目录并限制权限：

```bash
sudo install -d -o root -g rkmgw-signaling -m 0750 /etc/rkmgw
sudo chown root:rkmgw-signaling /etc/rkmgw/signaling-auth.json
sudo chmod 0640 /etc/rkmgw/signaling-auth.json
```

`device_tokens` 将设备 ID 与设备凭据绑定；`browser_tokens` 将浏览器凭据与允许访问的设备列表绑定。两种 Token 不应相同，也不要提交到 Git、写入公开网页或复制到日志中。

当前静态浏览器 Token 只适合受控联调。正式产品应由业务鉴权服务在用户登录后签发短期凭据，而不是为每个用户长期共享同一个 Token。

### 3.4 配置 systemd

创建 `/etc/systemd/system/rkmgw-signaling.service`：

```ini
[Unit]
Description=RKMediaGateway WebRTC signaling server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=rkmgw-signaling
Group=rkmgw-signaling
WorkingDirectory=/opt/rkmgw
Environment=PYTHONUNBUFFERED=1
Environment=RKMGW_SIGNAL_AUTH_FILE=/etc/rkmgw/signaling-auth.json
Environment=RKMGW_SIGNAL_PORT=8765
ExecStart=/opt/rkmgw/.venv/bin/python -m cloud.signaling_server
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

启动并检查服务：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now rkmgw-signaling
sudo systemctl status rkmgw-signaling --no-pager
sudo journalctl -u rkmgw-signaling -n 100 --no-pager
sudo ss -lntp | grep ':8765'
```

正确结果应为信令进程仅监听 `127.0.0.1:8765`。不要在云安全组中开放 `8765`。

### 3.5 配置临时 WS 反向代理

仓库中的 `nginx-ws-test.conf.example` 是临时联调配置。复制并编辑：

```bash
sudo cp /opt/rkmgw/cloud/signaling_server/nginx-ws-test.conf.example \
  /etc/nginx/sites-available/rkmgw-signaling-ws
sudo editor /etc/nginx/sites-available/rkmgw-signaling-ws
```

配置应保留以下关键内容：

```nginx
server {
    listen 18080;
    server_name _;

    # 优先替换为 PC 和设备当前的公网出口地址。
    # 受控联调时可以临时使用 allow all，但必须使用可废弃的测试 Token。
    allow all;

    location /device {
        proxy_pass http://127.0.0.1:8765;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 90s;
    }

    location /browser {
        proxy_pass http://127.0.0.1:8765;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 90s;
    }
}
```

启用配置并检查监听：

```bash
sudo ln -sfn /etc/nginx/sites-available/rkmgw-signaling-ws \
  /etc/nginx/sites-enabled/rkmgw-signaling-ws
sudo nginx -t
sudo systemctl reload nginx
sudo ss -lntp | grep ':18080'
```

在云安全组中放行 TCP `18080` 后，临时地址为：

- 设备：`ws://203.0.113.10:18080/device`；
- 浏览器：`ws://203.0.113.10:18080/browser`。

反向代理并不意味着把 `8765` 暴露到公网。公网只能访问 Nginx；Nginx 负责路径限制、来源限制、TLS 终止、访问日志和连接超时，Python 进程仍只接受本机连接。

### 3.6 正式切换到 WSS

临时 WS 会明文传输 Token 和信令内容，只用于限定时间的功能联调。正式部署应执行以下调整：

1. 为域名或受证书颁发机构支持的公网地址申请客户端可信的 TLS 证书；
2. 由 Nginx 在 `443` 端口终止 TLS；
3. 将 `/device` 和 `/browser` 转发到 `127.0.0.1:8765`；
4. 设备配置和浏览器地址改为 `wss://...`；
5. 关闭云安全组中的 TCP `18080`，作废联调 Token；
6. 设备必须校验证书链和主机名，不能通过关闭证书校验规避证书问题。

## 4. 在云服务器部署 STUN/TURN

### 4.1 安装 coturn

```bash
sudo apt update
sudo apt install -y coturn
```

部分 Ubuntu 安装包要求在 `/etc/default/coturn` 中设置：

```text
TURNSERVER_ENABLED=1
```

### 4.2 确定公网和内网地址

腾讯云等环境通常由云平台进行公网 NAT。coturn 实际绑定云主机私网地址，同时在候选中公布公网地址。以下为示例映射，部署时必须替换为实际地址：

```text
公网地址：203.0.113.10
私网地址：10.0.0.7
```

私网地址可通过以下命令确认：

```bash
ip -4 address show
ip route
```

### 4.3 配置 coturn

备份并编辑 `/etc/turnserver.conf`，至少包含：

```ini
listening-port=3478
listening-ip=10.0.0.7
relay-ip=10.0.0.7
external-ip=203.0.113.10/10.0.0.7

min-port=49152
max-port=65535

fingerprint
lt-cred-mech
realm=rkmgw-turn
userdb=/var/lib/turn/turndb
stale-nonce

no-multicast-peers
no-cli
```

配置含义：

- `listening-ip`：coturn 实际监听的云主机私网地址；
- `relay-ip`：分配中继套接字的本机地址；
- `external-ip=公网/私网`：告诉 coturn 公网 NAT 映射关系；
- `lt-cred-mech`：启用 TURN 长期凭据认证；
- `realm`：参与 TURN 凭据摘要计算，创建用户时必须完全一致；
- `min-port/max-port`：限定 UDP 中继端口范围。

不要同时配置 `stun-only` 或 `no-auth`。前者会禁用 TURN 分配，后者会取消 TURN 用户鉴权。

### 4.4 创建 TURN 用户

以下示例用户名为 `user`。密码在终端中隐藏输入，并写入 coturn 数据库：

```bash
printf 'TURN password: '
read -s RKMGW_TURN_PASSWORD
printf '\n'

sudo turnadmin -a \
  -b /var/lib/turn/turndb \
  -u user \
  -r rkmgw-turn \
  -p "$RKMGW_TURN_PASSWORD"

unset RKMGW_TURN_PASSWORD
sudo turnadmin -l -b /var/lib/turn/turndb
```

如果修改了 `realm`，必须用相同的 realm 重新创建用户。日志出现 `Cannot find credentials of user` 时，优先检查用户名、realm 以及客户端是否误输入了其他字符。

### 4.5 开放端口并启动

云安全组至少放行：

- UDP `3478`：STUN 和 TURN UDP；
- TCP `3478`：TURN TCP 回退；
- UDP `49152-65535`：TURN 中继媒体端口。

若启用了 UFW，还要同步放行：

```bash
sudo ufw allow 3478/udp
sudo ufw allow 3478/tcp
sudo ufw allow 49152:65535/udp
```

启动并核对服务：

```bash
sudo systemctl enable --now coturn
sudo systemctl restart coturn
sudo systemctl status coturn --no-pager
sudo journalctl -u coturn -n 100 --no-pager
sudo ss -lunp | grep ':3478'
sudo ss -lntp | grep ':3478'
```

在采用公网 NAT 的云主机上，`ss` 只显示 `10.0.0.7:3478` 是正常现象；`external-ip` 负责向客户端公布 `203.0.113.10`。

### 4.6 验证 STUN 和 TURN 候选

将 `webrtc_peerconnection_test` 部署到 RK3568 后，先验证 STUN：

```bash
./webrtc_peerconnection_test stun 203.0.113.10 3478
```

成功时应看到：

```text
ICE candidate type=srflx ...
[OK] ICE srflx candidate test passed
```

再验证 TURN：

```bash
printf 'TURN password: '
read -s RKMGW_TURN_PASSWORD
printf '\n'
export RKMGW_TURN_PASSWORD

./webrtc_peerconnection_test turn 203.0.113.10 3478 user

unset RKMGW_TURN_PASSWORD
```

成功时应看到：

```text
ICE candidate type=relay ...
[OK] ICE relay candidate test passed
```

只有 `srflx` 说明 STUN 正常；出现 `relay` 才说明 TURN 的认证、Allocate 请求和中继端口均已工作。

## 5. 部署 RK3568 设备端

### 5.1 编译

在安装了 RK3568 SDK 和交叉工具链的编译服务器上执行：

```bash
cd /home/topeet/source_code/RKMediaGateway
./build/build.sh all Release
```

主要产物位于：

```text
build/output/all_services
build/output/rkmgw
build/output/webrtc_peerconnection_test
```

`all_services` 是网关进程，`rkmgw` 是运行状态查询工具，`webrtc_peerconnection_test` 用于单独验证 STUN/TURN 候选。只更新网关时也可以将构建目标改为 `all_services`。

### 5.2 安装程序和运行库

建议使用产品目录 `/opt/rkmedia_gateway`，不要把正式部署固定在个人目录下：

```text
/opt/rkmedia_gateway/
├── all_services
├── media_gateway.toml
└── lib/                         # 项目私有动态库，按实际需要放置

/etc/rkmedia-gateway/secrets/
├── webrtc_device.token
└── turn_password
```

设备已开启 SSH 时可以使用 `scp`。设备没有 SSH 时，可以先把文件放到局域网 TFTP 根目录，再由设备下载：

```bash
# 在提供 TFTP 的编译机或虚拟机上执行。
cp build/output/all_services /home/tftptest/
cp build/output/rkmgw /home/tftptest/
cp build/output/webrtc_peerconnection_test /home/tftptest/
cp main/media_gateway.toml /home/tftptest/
```

设备端执行：

```bash
mkdir -p /opt/rkmedia_gateway
cd /opt/rkmedia_gateway
tftp -gr all_services 192.168.1.2
tftp -gr rkmgw 192.168.1.2
tftp -gr webrtc_peerconnection_test 192.168.1.2
tftp -gr media_gateway.toml 192.168.1.2
chmod 0755 all_services rkmgw webrtc_peerconnection_test
```

将示例中的 `192.168.1.2` 替换为实际 TFTP 服务器地址。TFTP 不加密，不要用它传输 Token 或 TURN 密码；私密文件应在设备本机创建。

需要的 `.so` 文件（包括 ISP 依赖库）统一放在 `/opt/rkmedia_gateway/lib`，启动前设置：

```bash
export LD_LIBRARY_PATH=/opt/rkmedia_gateway/lib:${LD_LIBRARY_PATH:-}
```

文档仅使用不含个人信息的示例部署路径；请先按该布局部署运行库，或将示例替换为实际产品路径。

### 5.3 创建设备私密文件

设备 Token 必须与云端 `device_tokens` 中 `rk3568-001` 对应的值完全一致；TURN 密码必须与 `turnadmin` 创建的用户密码一致。

```bash
install -d -m 0700 /etc/rkmedia-gateway/secrets
umask 077

printf 'Device token: '
read -s RKMGW_DEVICE_TOKEN
printf '\n'
printf '%s\n' "$RKMGW_DEVICE_TOKEN" \
  > /etc/rkmedia-gateway/secrets/webrtc_device.token
unset RKMGW_DEVICE_TOKEN

printf 'TURN password: '
read -s RKMGW_TURN_PASSWORD
printf '\n'
printf '%s\n' "$RKMGW_TURN_PASSWORD" \
  > /etc/rkmedia-gateway/secrets/turn_password
unset RKMGW_TURN_PASSWORD

chmod 0600 /etc/rkmedia-gateway/secrets/webrtc_device.token
chmod 0600 /etc/rkmedia-gateway/secrets/turn_password
```

### 5.4 配置设备

在 `media_gateway.toml` 中配置共享的云端信令和 ICE 参数：

```toml
[webrtc.cloud]
url = "ws://203.0.113.10:18080/device"
device_id = "rk3568-001"
token_file = "/etc/rkmedia-gateway/secrets/webrtc_device.token"

[webrtc.ice]
stun_host = "203.0.113.10"
stun_port = 3478
turn_host = "203.0.113.10"
turn_port = 3478
turn_username = "user"
turn_password_file = "/etc/rkmedia-gateway/secrets/turn_password"
relay_only = false
```

每个启用 WebRTC 的码流还要配置稳定且唯一的 `stream_id`，例如：

```toml
[stream.main.webrtc]
enabled = true
stream_id = "main"
```

说明：

- `relay_only=false`：允许 ICE 在 host、srflx 和 relay 中选择可用的最佳路径；
- `relay_only=true`：强制通过 TURN，适合验证完整的 TURN 媒体中继链路；
- 切换到正式环境后，`url` 必须改成可信的 `wss://.../device`；
- 配置校验要求云端 URL 以 `/device` 结尾，设备 ID 和 Token 文件均不能为空。

### 5.5 启动和检查

先以前台方式启动，便于发现动态库或配置错误：

```bash
cd /opt/rkmedia_gateway
export LD_LIBRARY_PATH=/opt/rkmedia_gateway/lib:${LD_LIBRARY_PATH:-}
./all_services /opt/rkmedia_gateway/media_gateway.toml
```

在另一个终端检查：

```bash
tail -f /tmp/rkmedia_gateway.log
./rkmgw getStatus
./rkmgw getWebRTC
```

同时在云服务器检查：

```bash
sudo journalctl -u rkmgw-signaling -f
sudo journalctl -u coturn -f
```

正常情况下，信令服务日志会出现 `connection opened role=device`，设备日志会显示 WebSocket 已连接并完成云端注册。

### 5.6 可选：设备 systemd 服务

手工联调稳定后，可以创建 `/etc/systemd/system/rkmedia-gateway.service`：

```ini
[Unit]
Description=RKMediaGateway service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/rkmedia_gateway
Environment=LD_LIBRARY_PATH=/opt/rkmedia_gateway/lib
ExecStart=/opt/rkmedia_gateway/all_services /opt/rkmedia_gateway/media_gateway.toml
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

然后执行：

```bash
systemctl daemon-reload
systemctl enable --now rkmedia-gateway
systemctl status rkmedia-gateway --no-pager
journalctl -u rkmedia-gateway -f
```

## 6. 浏览器端到端验证

先在 PC 的仓库目录启动本地静态服务器：

```bash
cd test/webRTC/browser
python3 -m http.server --bind 127.0.0.1 8081
```

浏览器访问 `http://localhost:8081`。通过 `localhost` 打开页面可满足浏览器采集麦克风所需的安全上下文要求。

然后执行：

1. 打开浏览器测试页面；
2. 选择云端 WS 模式；
3. 信令地址填写 `ws://203.0.113.10:18080/browser`；
4. 输入云端配置的浏览器测试 Token；
5. TURN 地址填写 `203.0.113.10:3478`，并填写 TURN 用户名和密码；
6. 打开设备 `rk3568-001` 的 `main` 码流；
7. 确认收到 `authenticated`、`opened`、Answer 和双方 ICE candidate；
8. 检查视频、设备上行音频和浏览器下行语音；
9. 用 `getWebRTC` 检查 session、媒体计数和入站 RTP 统计。

浏览器页面不会从信令服务器自动获得 TURN 密码，测试时必须单独填写完整的 TURN 地址、端口、用户名和密码。

建议依次执行两类网络测试：

- `relay_only=false`：验证 ICE 能在直连、STUN 反射和 TURN 中继之间正常选路；
- `relay_only=true`：确认最终候选对包含 `relay`，并验证媒体确实经过 TURN。

## 7. 更新服务器代码

不要直接用普通用户覆盖 `/opt/rkmgw`。更新单个文件时先上传到 `/tmp`：

```bash
scp cloud/signaling_server/server.py \
  ubuntu@203.0.113.10:/tmp/server.py
```

在云服务器安装、测试并重启：

```bash
sudo install -o rkmgw-signaling -g rkmgw-signaling -m 0644 \
  /tmp/server.py /opt/rkmgw/cloud/signaling_server/server.py

cd /opt/rkmgw
sudo -u rkmgw-signaling .venv/bin/python -m unittest \
  discover -s cloud/signaling_server/tests -v

sudo systemctl restart rkmgw-signaling
sudo systemctl status rkmgw-signaling --no-pager
sudo journalctl -u rkmgw-signaling -n 100 --no-pager
```

涉及多个文件时，应上传完整 `cloud` 目录并按第 3.1～3.2 节重新安装，避免服务器上形成不一致的代码版本。

## 8. 常见问题排查

### 8.1 信令服务无法启动

```bash
sudo systemctl status rkmgw-signaling --no-pager
sudo journalctl -u rkmgw-signaling -n 200 --no-pager
sudo -u rkmgw-signaling test -r /etc/rkmgw/signaling-auth.json
```

重点检查 Python 包目录是否为 `/opt/rkmgw/cloud/signaling_server`、鉴权 JSON 是否有效以及运行用户是否有读取权限。

### 8.2 浏览器或设备连接一分钟后离线

检查 Nginx 是否保留 Upgrade 头，并确认 `proxy_read_timeout` 大于 WebSocket 保活周期：

```bash
sudo nginx -T | grep -n -A15 -B3 'location /device'
sudo journalctl -u rkmgw-signaling -f
sudo tail -f /var/log/nginx/error.log
```

### 8.3 STUN 正常但 TURN 没有 relay 候选

依次检查：

1. coturn 未配置 `stun-only`；
2. 已启用 `lt-cred-mech`；
3. 客户端用户名无多余字符；
4. `turnadmin` 使用的 realm 与 `turnserver.conf` 完全一致；
5. 云安全组已开放 UDP `49152-65535`；
6. `external-ip` 的公网/私网映射正确。

```bash
sudo journalctl -u coturn -f
sudo grep -nE \
  '^[[:space:]]*(stun-only|no-auth|lt-cred-mech|realm|external-ip|min-port|max-port)' \
  /etc/turnserver.conf
```

### 8.4 已完成信令交换但 ICE 失败

信令连通不代表媒体路径一定连通。核对双方是否都收到了 candidate、TURN 参数是否完整、候选类型及云安全组中继端口。必要时设置 `relay_only=true`，先把问题收敛到 TURN 路径。

### 8.5 安全收尾

联调完成后必须：

- 关闭 TCP `18080` 或将来源限制恢复为白名单；
- 作废联调时使用的设备和浏览器 Token；
- 删除 `/tmp` 中的上传副本；
- 确保 Token、TURN 密码和私钥均未提交到 Git；
- 将设备和浏览器切换到可信 WSS；
- 保持 `8765` 仅监听 `127.0.0.1`。

## 9. 相关文档

- [云端 WebRTC 信令协议](../../bussiness/mediaOutput/src/webRTC/doc/信令协议/云端WebRTC信令协议.md)
- [WebRTC 云端信令与媒体测试方案](../../bussiness/mediaOutput/src/webRTC/doc/测试方案和用例/WebRTC云端信令与媒体测试方案.md)
- [Nginx 临时 WS 配置示例](nginx-ws-test.conf.example)
