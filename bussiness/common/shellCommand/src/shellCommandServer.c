/**
 * @file shellCommandServer.c
 * @brief 基于 Unix Domain Socket 的进程内 shell 命令服务实现。
 *
 * 本模块负责维护命令注册表、监听本地 Unix Domain Socket、接收客户端命令、
 * 查找对应处理函数并把文本结果返回给客户端。业务模块只需要调用 regShellCmd
 * 注册命令，不需要关心 socket 收发细节。
 */

#include "shellCommandServer.h"

#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SHELL_COMMAND_MAX_COMMANDS 32
#define SHELL_COMMAND_MAX_LINE 512
#define SHELL_COMMAND_LISTEN_BACKLOG 4

typedef struct {
    char name[64];                 /* 命令名称。 */
    ShellCommandHandler handler;   /* 命令处理函数。 */
    void *user_data;               /* 传给命令处理函数的业务上下文。 */
} ShellCommandEntry;

typedef struct {
    pthread_t thread;                                 /* 服务线程句柄。 */
    pthread_mutex_t lock;                             /* 保护命令注册表和运行状态。 */
    ShellCommandEntry commands[SHELL_COMMAND_MAX_COMMANDS]; /* 命令注册表。 */
    int command_count;                                /* 已注册命令数量。 */
    int listen_fd;                                    /* 监听 socket fd。 */
    int running;                                      /* 服务线程是否需要继续运行。 */
    int started;                                      /* 服务线程是否已经启动。 */
    int initialized;                                  /* 服务是否已经初始化。 */
    char socket_path[108];                            /* Unix Domain Socket 路径。 */
} ShellCommandServer;

static ShellCommandServer g_shell_command_server = {0};

/**
 * @brief ? shell ???????????????
 *
 * output ?????? SHELL_COMMAND_MAX_REPLY_SIZE?????????
 * offset ?????????????
 */
void shell_command_reply_append(char *output,
                                size_t *offset,
                                const char *fmt,
                                ...)
{
    va_list ap;
    int written = 0;
    size_t reply_size = SHELL_COMMAND_MAX_REPLY_SIZE;

    if (!output || !offset || !fmt || *offset >= reply_size)
    {
        LOG_ERROR("shell_command_reply_append failed: invalid argument output=%p offset=%p fmt=%p offset_value=%zu reply_size=%zu",
                  (void *)output,
                  (void *)offset,
                  (const void *)fmt,
                  offset ? *offset : 0,
                  reply_size);
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(output + *offset, reply_size - *offset, fmt, ap);
    va_end(ap);
    if (written < 0)
    {
        LOG_ERROR("shell_command_reply_append failed: vsnprintf ret=%d", written);
        return;
    }
    if ((size_t)written >= reply_size - *offset)
    {
        LOG_ERROR("shell_command_reply_append failed: reply buffer truncated written=%d remain=%zu",
                  written,
                  reply_size - *offset);
        *offset = reply_size - 1;
        return;
    }
    *offset += (size_t)written;
}

/**
 * @brief 生成 help 命令输出。
 *
 * help 命令由 shell 命令服务内置实现，用于列出当前已经注册的命令名称。
 */
static int shell_command_handle_help(char *reply, size_t reply_size)
{
    ShellCommandServer *server = NULL;
    size_t offset = 0;
    int i = 0;

    if (!reply || reply_size == 0)
    {
        LOG_ERROR("shell_command_handle_help failed: invalid argument reply=%p reply_size=%zu",
                  (void *)reply,
                  reply_size);
        return -1;
    }

    server = &g_shell_command_server;
    reply[0] = '\0';
    shell_command_reply_append(reply, &offset, "commands:\n");

    pthread_mutex_lock(&server->lock);
    for (i = 0; i < server->command_count; ++i)
    {
        shell_command_reply_append(reply,
                                   &offset,
                                   "  %s\n",
                                   server->commands[i].name);
    }
    pthread_mutex_unlock(&server->lock);
    return 0;
}

/**
 * @brief 去掉命令行尾部的换行和空白字符。
 *
 * 客户端发送命令时通常会带有换行，本函数在解析命令前统一裁剪尾部
 * '\n'、'\r'、空格和 Tab。
 */
static void trim_line_tail(char *line)
{
    size_t len = 0;

    if (!line)
    {
        LOG_ERROR("trim_line_tail failed: line is NULL");
        return;
    }

    len = strlen(line);
    while (len > 0 &&
           (line[len - 1] == '\n' ||
            line[len - 1] == '\r' ||
            line[len - 1] == ' ' ||
            line[len - 1] == '\t'))
    {
        line[len - 1] = '\0';
        len--;
    }
}

/**
 * @brief 将命令行拆成命令名和原始输入参数。
 *
 * 只查找第一个空格或 Tab：前半段作为命令名，后半段跳过连续空白后
 * 原样传给命令处理函数，避免“先拆成 argv 再重新拼接”导致参数格式丢失。
 */
static void split_command_line(char *line, char **command_name, char **input)
{
    char *cursor = NULL;

    if (command_name)
        *command_name = NULL;
    if (input)
        *input = "";
    if (!line || !command_name || !input)
    {
        LOG_ERROR("split_command_line failed: invalid argument line=%p command_name=%p input=%p",
                  (void *)line,
                  (void *)command_name,
                  (void *)input);
        return;
    }

    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0')
        return;

    *command_name = line;
    cursor = line;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
        cursor++;
    if (*cursor == '\0')
        return;

    *cursor = '\0';
    cursor++;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    *input = cursor;
}

/**
 * @brief 查找命令处理函数并执行。
 *
 * 如果命令为空或命令名为 help，则直接生成 help 输出；如果命令不存在，
 * 则返回 unknown_command 错误文本。
 */
static int dispatch_command(char *line, char *reply, size_t reply_size)
{
    ShellCommandServer *server = NULL;
    ShellCommandEntry entry = {0};
    char *command_name = NULL;
    char *input = "";
    int i = 0;
    int found = 0;
    int ret = 0;

    if (!reply || reply_size == 0)
    {
        LOG_ERROR("dispatch_command failed: invalid argument line=%p reply=%p reply_size=%zu",
                  (void *)line,
                  (void *)reply,
                  reply_size);
        return -1;
    }

    reply[0] = '\0';
    split_command_line(line, &command_name, &input);
    if (!command_name)
        return shell_command_handle_help(reply, reply_size);

    if (strcmp(command_name, "help") == 0)
        return shell_command_handle_help(reply, reply_size);

    server = &g_shell_command_server;
    pthread_mutex_lock(&server->lock);
    for (i = 0; i < server->command_count; ++i)
    {
        if (strcmp(server->commands[i].name, command_name) == 0)
        {
            entry = server->commands[i];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&server->lock);

    if (!found || !entry.handler)
    {
        LOG_ERROR("dispatch_command failed: unknown command=%s", command_name);
        snprintf(reply,
                 reply_size,
                 "error=unknown_command\ncommand=%s\ntry=help\n",
                 command_name);
        return -1;
    }

    ret = entry.handler(entry.user_data, input, reply);
    if (ret != 0)
    {
        LOG_ERROR("dispatch_command failed: handler command=%s ret=%d", command_name, ret);
    }
    return ret;
}

/**
 * @brief 将缓冲区完整写入 fd。
 *
 * Unix stream socket 可能出现短写，本函数循环调用 write，直到全部数据
 * 写完或出现不可恢复错误。
 */
static int write_all(int fd, const char *data, size_t data_len)
{
    ssize_t write_len = 0;
    size_t offset = 0;

    if (fd < 0 || (!data && data_len > 0))
    {
        LOG_ERROR("write_all failed: invalid argument fd=%d data=%p data_len=%zu",
                  fd,
                  (const void *)data,
                  data_len);
        return -1;
    }

    while (offset < data_len)
    {
        write_len = write(fd, data + offset, data_len - offset);
        if (write_len < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("write_all failed: fd=%d offset=%zu data_len=%zu errno=%d(%s)",
                      fd,
                      offset,
                      data_len,
                      errno,
                      strerror(errno));
            return -1;
        }
        if (write_len == 0)
        {
            LOG_ERROR("write_all failed: write returned 0 fd=%d offset=%zu data_len=%zu",
                      fd,
                      offset,
                      data_len);
            return -1;
        }
        offset += (size_t)write_len;
    }

    return 0;
}

/**
 * @brief 处理一个客户端连接。
 *
 * 该函数读取客户端的一行命令，分发执行后把文本回复写回客户端。
 * 当前协议是短连接模式：一个连接处理一条命令。
 */
static void handle_client(int client_fd)
{
    char line[SHELL_COMMAND_MAX_LINE] = {0};
    char reply[SHELL_COMMAND_MAX_REPLY_SIZE] = {0};
    ssize_t read_len = 0;
    size_t reply_len = 0;
    int dispatch_ret = 0;

    if (client_fd < 0)
    {
        LOG_ERROR("handle_client failed: invalid client_fd=%d", client_fd);
        return;
    }

    read_len = read(client_fd, line, sizeof(line) - 1);
    if (read_len <= 0)
    {
        LOG_ERROR("shell command read failed fd=%d ret=%zd errno=%d(%s)",
                  client_fd,
                  read_len,
                  errno,
                  strerror(errno));
        return;
    }
    line[read_len] = '\0';
    trim_line_tail(line);
    dispatch_ret = dispatch_command(line, reply, sizeof(reply));
    if (dispatch_ret != 0)
    {
        LOG_ERROR("handle_client failed: dispatch command ret=%d line=%s",
                  dispatch_ret,
                  line);
    }

    reply_len = strlen(reply);
    if (reply_len == 0)
        snprintf(reply, sizeof(reply), "ok\n");
    reply_len = strlen(reply);
    if (write_all(client_fd, reply, reply_len) != 0)
    {
        LOG_ERROR("shell command write failed fd=%d errno=%d(%s)",
                  client_fd,
                  errno,
                  strerror(errno));
    }
}

/**
 * @brief shell 命令服务线程主函数。
 *
 * 服务线程使用 select 周期性等待监听 socket；收到连接后 accept，
 * 调用 handle_client 处理一条命令，然后关闭客户端连接。
 */
static void *shell_command_thread(void *arg)
{
    ShellCommandServer *server = NULL;
    fd_set readfds;
    struct timeval timeout = {0};
    int select_ret = 0;
    int client_fd = -1;

    (void)arg;
    server = &g_shell_command_server;

    while (server->running)
    {
        FD_ZERO(&readfds);
        FD_SET(server->listen_fd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        select_ret = select(server->listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (select_ret < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("shell command select failed errno=%d(%s)",
                      errno,
                      strerror(errno));
            break;
        }
        if (select_ret == 0)
            continue;
        if (!FD_ISSET(server->listen_fd, &readfds))
            continue;

        client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("shell command accept failed errno=%d(%s)",
                      errno,
                      strerror(errno));
            continue;
        }
        handle_client(client_fd);
        close(client_fd);
        client_fd = -1;
    }

    return NULL;
}

/**
 * @brief 初始化 shell 命令服务。
 *
 * 初始化过程会创建 Unix Domain Socket、绑定监听路径并进入 listen 状态。
 * 若 socket_path 为空，则使用 SHELL_COMMAND_DEFAULT_SOCKET_PATH。
 */
int shell_command_server_init(const char *socket_path)
{
    ShellCommandServer *server = NULL;
    struct sockaddr_un addr = {0};
    int mutex_ret = 0;
    int bind_ret = 0;
    int listen_ret = 0;
    int cleanup_ret = 0;

    server = &g_shell_command_server;
    if (server->initialized)
        return 0;

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    snprintf(server->socket_path,
             sizeof(server->socket_path),
             "%s",
             (socket_path && socket_path[0] != '\0') ?
                 socket_path :
                 SHELL_COMMAND_DEFAULT_SOCKET_PATH);

    mutex_ret = pthread_mutex_init(&server->lock, NULL);
    if (mutex_ret != 0)
    {
        LOG_ERROR("shell_command_server_init failed: pthread_mutex_init ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
        return -1;
    }

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
    {
        LOG_ERROR("shell_command_server_init failed: socket errno=%d(%s)",
                  errno,
                  strerror(errno));
        cleanup_ret = pthread_mutex_destroy(&server->lock);
        if (cleanup_ret != 0)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: pthread_mutex_destroy ret=%d(%s)",
                      cleanup_ret,
                      strerror(cleanup_ret));
        }
        return -1;
    }

    unlink(server->socket_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server->socket_path);

    bind_ret = bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (bind_ret != 0)
    {
        LOG_ERROR("shell_command_server_init failed: bind path=%s errno=%d(%s)",
                  server->socket_path,
                  errno,
                  strerror(errno));
        cleanup_ret = close(server->listen_fd);
        if (cleanup_ret != 0)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: close fd=%d errno=%d(%s)",
                      server->listen_fd,
                      errno,
                      strerror(errno));
        }
        cleanup_ret = pthread_mutex_destroy(&server->lock);
        if (cleanup_ret != 0)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: pthread_mutex_destroy ret=%d(%s)",
                      cleanup_ret,
                      strerror(cleanup_ret));
        }
        server->listen_fd = -1;
        return -1;
    }

    listen_ret = listen(server->listen_fd, SHELL_COMMAND_LISTEN_BACKLOG);
    if (listen_ret != 0)
    {
        LOG_ERROR("shell_command_server_init failed: listen errno=%d(%s)",
                  errno,
                  strerror(errno));
        cleanup_ret = close(server->listen_fd);
        if (cleanup_ret != 0)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: close fd=%d errno=%d(%s)",
                      server->listen_fd,
                      errno,
                      strerror(errno));
        }
        cleanup_ret = unlink(server->socket_path);
        if (cleanup_ret != 0 && errno != ENOENT)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: unlink path=%s errno=%d(%s)",
                      server->socket_path,
                      errno,
                      strerror(errno));
        }
        cleanup_ret = pthread_mutex_destroy(&server->lock);
        if (cleanup_ret != 0)
        {
            LOG_ERROR("shell_command_server_init cleanup failed: pthread_mutex_destroy ret=%d(%s)",
                      cleanup_ret,
                      strerror(cleanup_ret));
        }
        server->listen_fd = -1;
        return -1;
    }

    server->initialized = 1;
    return 0;
}

/**
 * @brief 注册或更新一条 shell 命令。
 *
 * 如果同名命令已经存在，则更新其处理函数和 user_data；否则新增一条注册项。
 */
int regShellCmd(const char *name,
                ShellCommandHandler handler,
                void *user_data)
{
    ShellCommandServer *server = NULL;
    int i = 0;

    if (!name || name[0] == '\0' || !handler)
    {
        LOG_ERROR("regShellCmd failed: name=%p handler_valid=%d",
                  (const void *)name,
                  handler ? 1 : 0);
        return -1;
    }

    server = &g_shell_command_server;
    if (!server->initialized)
    {
        LOG_ERROR("regShellCmd failed: server not initialized name=%s", name);
        return -1;
    }

    pthread_mutex_lock(&server->lock);
    for (i = 0; i < server->command_count; ++i)
    {
        if (strcmp(server->commands[i].name, name) == 0)
        {
            server->commands[i].handler = handler;
            server->commands[i].user_data = user_data;
            pthread_mutex_unlock(&server->lock);
            return 0;
        }
    }

    if (server->command_count >= SHELL_COMMAND_MAX_COMMANDS)
    {
        pthread_mutex_unlock(&server->lock);
        LOG_ERROR("regShellCmd failed: command table full name=%s", name);
        return -1;
    }

    snprintf(server->commands[server->command_count].name,
             sizeof(server->commands[server->command_count].name),
             "%s",
             name);
    server->commands[server->command_count].handler = handler;
    server->commands[server->command_count].user_data = user_data;
    server->command_count++;
    pthread_mutex_unlock(&server->lock);
    return 0;
}

/**
 * @brief 启动 shell 命令服务线程。
 */
int shell_command_server_start(void)
{
    ShellCommandServer *server = NULL;
    int thread_ret = 0;

    server = &g_shell_command_server;
    if (!server->initialized)
    {
        LOG_ERROR("shell_command_server_start failed: server not initialized");
        return -1;
    }
    if (server->started)
        return 0;

    server->running = 1;
    thread_ret = pthread_create(&server->thread, NULL, shell_command_thread, NULL);
    if (thread_ret != 0)
    {
        server->running = 0;
        LOG_ERROR("shell_command_server_start failed: pthread_create ret=%d(%s)",
                  thread_ret,
                  strerror(thread_ret));
        return -1;
    }

    server->started = 1;
    LOG_INFO("shell command server started path=%s", server->socket_path);
    return 0;
}

/**
 * @brief 停止 shell 命令服务线程。
 *
 * 该函数会请求线程退出并等待 pthread_join 完成。
 */
void shell_command_server_stop(void)
{
    ShellCommandServer *server = NULL;
    int join_ret = 0;

    server = &g_shell_command_server;
    if (!server->initialized)
        return;

    server->running = 0;
    if (server->started)
    {
        join_ret = pthread_join(server->thread, NULL);
        if (join_ret != 0)
        {
            LOG_ERROR("shell_command_server_stop failed: pthread_join ret=%d(%s)",
                      join_ret,
                      strerror(join_ret));
            return;
        }
        server->started = 0;
    }
}

/**
 * @brief 释放 shell 命令服务资源。
 *
 * 释放顺序为：停止服务线程、关闭监听 fd、删除 socket 文件、销毁互斥锁、
 * 清空全局服务状态。
 */
void shell_command_server_deinit(void)
{
    ShellCommandServer *server = NULL;
    int mutex_ret = 0;
    int close_ret = 0;
    int unlink_ret = 0;

    server = &g_shell_command_server;
    if (!server->initialized)
        return;

    shell_command_server_stop();
    if (server->listen_fd >= 0)
    {
        close_ret = close(server->listen_fd);
        if (close_ret != 0)
        {
            LOG_ERROR("shell_command_server_deinit failed: close fd=%d errno=%d(%s)",
                      server->listen_fd,
                      errno,
                      strerror(errno));
        }
        server->listen_fd = -1;
    }
    unlink_ret = unlink(server->socket_path);
    if (unlink_ret != 0 && errno != ENOENT)
    {
        LOG_ERROR("shell_command_server_deinit failed: unlink path=%s errno=%d(%s)",
                  server->socket_path,
                  errno,
                  strerror(errno));
    }
    mutex_ret = pthread_mutex_destroy(&server->lock);
    if (mutex_ret != 0)
    {
        LOG_ERROR("shell_command_server_deinit failed: pthread_mutex_destroy ret=%d(%s)",
                  mutex_ret,
                  strerror(mutex_ret));
    }
    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
}
