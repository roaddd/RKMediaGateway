/**
 * @file rkmgw.c
 * @brief RKMediaGateway 本地调试命令客户端。
 *
 * 该程序模拟普通 Linux 命令的使用方式，例如：
 *   rkmgw help
 *   rkmgw getStatus
 *   rkmgw getFps
 *   rkmgw getIsp
 *
 * 客户端只负责把命令行参数拼成一条文本命令，通过 Unix Domain Socket
 * 发送给正在运行的 RKMediaGateway 进程，再把服务端返回的文本结果打印到终端。
 */

#include "debugCommandServer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RKMGW_COMMAND_LINE_SIZE 512
#define RKMGW_REPLY_CHUNK_SIZE 1024

/**
 * @brief 打印客户端使用说明。
 */
static void print_usage(const char *program)
{
    const char *name = NULL;

    name = program ? program : "rkmgw";
    fprintf(stderr, "usage: %s <command> [args...]\n", name);
    fprintf(stderr, "example: %s help\n", name);
    fprintf(stderr, "example: %s getStatus\n", name);
}

/**
 * @brief 将进程 argv 拼接成服务端可解析的单行命令。
 *
 * 当前服务端按空白字符切分参数，因此这里不做引号转义。调试命令应保持
 * 简短、无空格参数，复杂二进制数据不走该调试通道。
 */
static int build_command_line(int argc, char **argv, char *line, size_t line_size)
{
    size_t offset = 0;
    size_t need = 0;
    int i = 0;

    if (!line || line_size == 0)
    {
        fprintf(stderr, "build command failed: invalid buffer\n");
        return -1;
    }

    line[0] = '\0';
    if (argc <= 1)
    {
        snprintf(line, line_size, "help\n");
        return 0;
    }

    for (i = 1; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        need = strlen(argv[i]) + 2;
        if (offset + need >= line_size)
        {
            fprintf(stderr, "build command failed: command too long\n");
            return -1;
        }
        if (offset > 0)
        {
            line[offset] = ' ';
            offset++;
        }
        snprintf(line + offset, line_size - offset, "%s", argv[i]);
        offset += strlen(argv[i]);
    }

    if (offset + 1 >= line_size)
    {
        fprintf(stderr, "build command failed: command too long\n");
        return -1;
    }
    line[offset] = '\n';
    offset++;
    line[offset] = '\0';
    return 0;
}

/**
 * @brief 连接 RKMediaGateway 调试命令服务端。
 */
static int connect_shell_server(void)
{
    struct sockaddr_un addr = {0};
    int fd = -1;
    int ret = 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        fprintf(stderr, "connect failed: socket errno=%d(%s)\n", errno, strerror(errno));
        return -1;
    }

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", debug_command_DEFAULT_SOCKET_PATH);
    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret != 0)
    {
        fprintf(stderr,
                "connect failed: path=%s errno=%d(%s)\n",
                debug_command_DEFAULT_SOCKET_PATH,
                errno,
                strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief 将命令完整写入 socket，处理 stream socket 短写。
 */
static int write_all(int fd, const char *data, size_t data_len)
{
    ssize_t write_len = 0;
    size_t offset = 0;

    if (fd < 0 || (!data && data_len > 0))
    {
        fprintf(stderr, "write failed: invalid argument\n");
        return -1;
    }

    while (offset < data_len)
    {
        write_len = write(fd, data + offset, data_len - offset);
        if (write_len < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "write failed: errno=%d(%s)\n", errno, strerror(errno));
            return -1;
        }
        if (write_len == 0)
        {
            fprintf(stderr, "write failed: connection closed\n");
            return -1;
        }
        offset += (size_t)write_len;
    }

    return 0;
}

/**
 * @brief 发送命令并持续读取服务端返回结果。
 */
static int send_command_and_print_reply(int fd, const char *line)
{
    char reply[RKMGW_REPLY_CHUNK_SIZE] = {0};
    size_t line_len = 0;
    ssize_t io_ret = 0;
    int result = 0;

    if (fd < 0 || !line)
    {
        fprintf(stderr, "send command failed: invalid argument\n");
        return -1;
    }

    line_len = strlen(line);
    if (write_all(fd, line, line_len) != 0)
        return -1;

    while (1)
    {
        io_ret = read(fd, reply, sizeof(reply));
        if (io_ret < 0)
        {
            fprintf(stderr, "read reply failed: errno=%d(%s)\n", errno, strerror(errno));
            result = -1;
            break;
        }
        if (io_ret == 0)
            break;
        fwrite(reply, 1, (size_t)io_ret, stdout);
    }

    return result;
}

/**
 * @brief rkmgw 客户端入口。
 */
int main(int argc, char **argv)
{
    char command_line[RKMGW_COMMAND_LINE_SIZE] = {0};
    int fd = -1;
    int ret = 0;

    if (build_command_line(argc, argv, command_line, sizeof(command_line)) != 0)
    {
        print_usage(argv ? argv[0] : "rkmgw");
        return EXIT_FAILURE;
    }

    fd = connect_shell_server();
    if (fd < 0)
        return EXIT_FAILURE;

    ret = send_command_and_print_reply(fd, command_line);
    close(fd);
    if (ret != 0)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
