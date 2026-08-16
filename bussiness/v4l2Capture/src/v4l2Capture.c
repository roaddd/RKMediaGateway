#include "v4l2Capture.h"

#include "logger.h"

#include <inttypes.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>

#ifdef ENABLE_RKAIQ
#include "rk_aiq_user_api2_sysctl.h"
#endif

#ifndef V4L2_CAPTURE_ENABLE_OSD
#define V4L2_CAPTURE_ENABLE_OSD 0
#endif

/**
 * @description: 打印 V4L2 接口错误日志。
 * @param {const char *} msg 错误上下文描述。
 * @param {int} ret 错误码。
 * @return {static void}
 */
static void print_v4l2_error(const char *msg, int ret) {
    LOG_ERROR("%s: %s (errno=%d)", msg, strerror(-ret), ret);
}

/**
 * @description: 获取当前单调时钟时间，单位微秒。
 * @return {static uint64_t}
 */
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 按 entity id 查询 media controller 中的 entity 描述。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {uint32_t} entity_id 待查询的 entity id，可带 MEDIA_ENT_ID_FLAG_NEXT。
 * @param {struct media_entity_desc *} entity 输出 entity 描述。
 * @return {int} 0 成功，-1 失败。
 */
static int enum_media_entity(int media_fd, uint32_t entity_id, struct media_entity_desc *entity)
{
    if (!entity)
        return -1;

    memset(entity, 0, sizeof(*entity));
    entity->id = entity_id;
    return ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, entity);
}

/**
 * @description: 根据字符设备 major/minor 反查 /dev 下的设备节点路径。
 * @param {const char *} prefix 设备节点前缀，例如 /dev/v4l-subdev。
 * @param {int} max_index 最大扫描序号，不包含该值。
 * @param {unsigned int} dev_major 目标字符设备 major。
 * @param {unsigned int} dev_minor 目标字符设备 minor。
 * @param {char *} out_path 输出设备节点路径。
 * @param {size_t} out_size out_path 缓冲区大小。
 * @return {int} 0 成功，-1 未找到或参数无效。
 */
static int find_devnode_by_rdev(const char *prefix,
                                int max_index,
                                unsigned int dev_major,
                                unsigned int dev_minor,
                                char *out_path,
                                size_t out_size)
{
    int i;
    char path[PATH_MAX];
    struct stat st;

    if (!prefix || !out_path || out_size == 0)
    {
        LOG_ERROR("Invalid parameters for find_devnode_by_rdev");
        return -1;
    }

    /* /dev/v4l-subdevX 没有固定序号，按 rdev 精确匹配 media entity。 */
    for (i = 0; i < max_index; ++i)
    {
        snprintf(path, sizeof(path), "%s%d", prefix, i);
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
            continue;
        if ((unsigned int)major(st.st_rdev) == dev_major &&
            (unsigned int)minor(st.st_rdev) == dev_minor)
        {
            snprintf(out_path, out_size, "%s", path);
            return 0;
        }
    }

    return -1;
}

/**
 * @description: 根据 video/subdev 字符设备 major/minor 在 media graph 中找到对应 entity。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {unsigned int} dev_major 目标字符设备 major。
 * @param {unsigned int} dev_minor 目标字符设备 minor。
 * @param {struct media_entity_desc *} out_entity 输出匹配到的 entity。
 * @return {int} 0 成功，-1 未找到。
 */
static int media_find_entity_by_dev(int media_fd,
                                    unsigned int dev_major,
                                    unsigned int dev_minor,
                                    struct media_entity_desc *out_entity)
{
    uint32_t id = 0;
    struct media_entity_desc entity;

    while (1)
    {
        /* MEDIA_ENT_ID_FLAG_NEXT 让内核按 id 顺序返回下一个 entity。 */
        memset(&entity, 0, sizeof(entity));
        entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) != 0)
            break;

        if (entity.dev.major == dev_major && entity.dev.minor == dev_minor)
        {
            if (out_entity)
                *out_entity = entity;
            return 0;
        }
        id = entity.id;
    }

    return -1;
}

/**
 * @description: 根据 media entity 名称查找 entity。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {const char *} entity_name 目标 entity 名称。
 * @param {struct media_entity_desc *} out_entity 输出匹配到的 entity。
 * @return {int} 0 成功，-1 未找到。
 */
static int media_find_entity_by_name(int media_fd,
                                     const char *entity_name,
                                     struct media_entity_desc *out_entity)
{
    uint32_t id = 0;
    struct media_entity_desc entity = {0};

    if (!entity_name || entity_name[0] == '\0')
        return -1;

    while (1)
    {
        /* media entity 名称由内核驱动注册，RKAIQ 返回的 sensor 名称与这里一致。 */
        memset(&entity, 0, sizeof(entity));
        entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) != 0)
            break;

        if (strcmp(entity.name, entity_name) == 0)
        {
            if (out_entity)
                *out_entity = entity;
            return 0;
        }
        id = entity.id;
    }

    return -1;
}

#ifdef ENABLE_RKAIQ
/**
 * @description: 使用 RKAIQ 已有接口，根据 video node 查询绑定的 sensor entity。
 * @param {const char *} video_path V4L2 video 节点，例如 /dev/video0。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {struct media_entity_desc *} out_sensor 输出 sensor entity。
 * @return {int} 0 成功，-1 失败。
 */
static int media_find_sensor_by_rkaiq_binding(const char *video_path,
                                              int media_fd,
                                              struct media_entity_desc *out_sensor)
{
    const char *sensor_name = NULL;
    int ret = -1;

    if (!video_path || video_path[0] == '\0' || !out_sensor)
        return -1;

    /*
     * RKAIQ 内部已经适配 Rockchip media 拓扑差异。当前内核里
     * rkisp_mainpath/rkisp_selfpath 的 links=0，直接按 link 反查会失败。
     */
    sensor_name = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(video_path);
    LOG_WARN("resolve sensor subdev: rkaiq bind query video=%s sensor='%s'",
             video_path,
             sensor_name ? sensor_name : "NULL");
    if (!sensor_name || sensor_name[0] == '\0')
        return -1;

    ret = media_find_entity_by_name(media_fd, sensor_name, out_sensor);
    if (ret != 0)
    {
        LOG_WARN("resolve sensor subdev: rkaiq sensor entity not found sensor='%s'",
                 sensor_name);
        return -1;
    }

    LOG_WARN("resolve sensor subdev: rkaiq sensor entity found id=%u name='%s' type=0x%x dev=%u:%u",
             out_sensor->id,
             out_sensor->name,
             out_sensor->type,
             out_sensor->dev.major,
             out_sensor->dev.minor);
    return 0;
}
#endif

/**
 * @description: 从全图扫描与当前 entity 相连的 data link，避免依赖当前 entity.links。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {uint32_t} entity_id 当前 entity id。
 * @param {int} pass 0 只走 enabled link，1 走 disabled link 兜底。
 * @param {uint32_t *} out_next_entity 输出相邻 entity id。
 * @param {unsigned int *} out_flags 输出 link flags。
 * @return {int} 0 找到一条候选 link，-1 未找到。
 */
static int media_find_connected_entity_by_scan(int media_fd,
                                               uint32_t entity_id,
                                               int pass,
                                               uint32_t *out_next_entity,
                                               unsigned int *out_flags)
{
    uint32_t id = 0;
    uint32_t next_entity_id = 0;
    struct media_entity_desc owner = {0};
    struct media_links_enum links = {0};
    struct media_pad_desc *pads = NULL;
    struct media_link_desc *link_descs = NULL;
    struct media_link_desc *link = NULL;
    int i = 0;
    int is_data_link = 0;
    int is_enabled = 0;
    int ret = -1;

    if (!out_next_entity || !out_flags)
    {
        LOG_ERROR("media_find_connected_entity_by_scan failed: invalid args entity_id=%u pass=%d out_next=%p out_flags=%p",
                  entity_id,
                  pass,
                  (void *)out_next_entity,
                  (void *)out_flags);
        return -1;
    }

    *out_next_entity = 0;
    *out_flags = 0;
    LOG_WARN("media_find_connected_entity_by_scan begin: target_id=%u pass=%d policy=%s",
             entity_id,
             pass,
             pass == 0 ? "enabled-data-link" : "disabled-data-link-fallback");

    while (1)
    {
        /* 逐个枚举 owner entity，再从 owner 的 links 中反查是否连接到目标 entity。 */
        memset(&owner, 0, sizeof(owner));
        owner.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &owner) != 0)
        {
            LOG_WARN("media_find_connected_entity_by_scan enum entities end: target_id=%u pass=%d last_id=%u errno=%d(%s)",
                     entity_id,
                     pass,
                     id,
                     errno,
                     strerror(errno));
            break;
        }

        LOG_WARN("media_find_connected_entity_by_scan owner: target_id=%u owner_id=%u owner='%s' pads=%u links=%u type=0x%x",
                 entity_id,
                 owner.id,
                 owner.name,
                 owner.pads,
                 owner.links,
                 owner.type);

        /* 每轮循环复用临时数组，先释放上一个 owner 的 pads/links 缓冲区。 */
        free(pads);
        free(link_descs);
        pads = NULL;
        link_descs = NULL;

        /* MEDIA_IOC_ENUM_LINKS 需要调用方按 owner.pads/owner.links 提供数组空间。 */
        if (owner.pads > 0)
            pads = (struct media_pad_desc *)calloc(owner.pads, sizeof(*pads));
        if (owner.links > 0)
            link_descs = (struct media_link_desc *)calloc(owner.links, sizeof(*link_descs));
        if ((owner.pads > 0 && !pads) || (owner.links > 0 && !link_descs))
        {
            LOG_WARN("media_find_connected_entity_by_scan alloc failed: owner='%s' id=%u pads=%u links=%u",
                     owner.name,
                     owner.id,
                     owner.pads,
                     owner.links);
            goto cleanup;
        }

        if (owner.links == 0)
        {
            LOG_WARN("media_find_connected_entity_by_scan skip owner: reason=no-links target_id=%u owner_id=%u owner='%s'",
                     entity_id,
                     owner.id,
                     owner.name);
            id = owner.id;
            continue;
        }

        /*
         * media-ctl 展示的是整理后的完整拓扑视图，可能包含其它 entity 反向补进来的
         * backlink；raw MEDIA_IOC_ENUM_LINKS 只返回当前 owner 直接枚举到的 links。
         * 因此不能只依赖目标 entity 自己的 links，必须全图扫描每个 owner 的 links。
         */
        memset(&links, 0, sizeof(links));
        links.entity = owner.id;
        links.pads = pads;
        links.links = link_descs;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links) != 0)
        {
            LOG_WARN("media_find_connected_entity_by_scan enum links failed: owner='%s' id=%u pads=%u links=%u errno=%d(%s)",
                     owner.name,
                     owner.id,
                     owner.pads,
                     owner.links,
                     errno,
                     strerror(errno));
            id = owner.id;
            continue;
        }

        for (i = 0; i < owner.links; ++i)
        {
            link = &link_descs[i];
            is_data_link = ((link->flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_DATA_LINK);
            is_enabled = ((link->flags & MEDIA_LNK_FL_ENABLED) != 0);
            next_entity_id = 0;

            if (!is_data_link)
            {
                LOG_WARN("media_find_connected_entity_by_scan skip link: reason=not-data-link owner='%s' owner_id=%u index=%d source=%u:%u sink=%u:%u flags=0x%x",
                         owner.name,
                         owner.id,
                         i,
                         link->source.entity,
                         link->source.index,
                         link->sink.entity,
                         link->sink.index,
                         link->flags);
                continue;
            }
            if (pass == 0 && !is_enabled)
            {
                LOG_WARN("media_find_connected_entity_by_scan skip link: reason=disabled-in-first-pass owner='%s' owner_id=%u index=%d source=%u:%u sink=%u:%u flags=0x%x",
                         owner.name,
                         owner.id,
                         i,
                         link->source.entity,
                         link->source.index,
                         link->sink.entity,
                         link->sink.index,
                         link->flags);
                continue;
            }
            if (pass == 1 && is_enabled)
            {
                LOG_WARN("media_find_connected_entity_by_scan skip link: reason=enabled-in-fallback-pass owner='%s' owner_id=%u index=%d source=%u:%u sink=%u:%u flags=0x%x",
                         owner.name,
                         owner.id,
                         i,
                         link->source.entity,
                         link->source.index,
                         link->sink.entity,
                         link->sink.index,
                         link->flags);
                continue;
            }

            /*
             * 当前目标是找上游 sensor，只接受 target 位于 sink 端的链路；
             * source 端才是上游节点，避免从 ISP source pad 走回输出 video node。
             */
            if (link->sink.entity == entity_id)
                next_entity_id = link->source.entity;
            else
            {
                LOG_WARN("media_find_connected_entity_by_scan skip link: reason=not-upstream target_id=%u owner='%s' owner_id=%u index=%d source=%u:%u sink=%u:%u flags=0x%x",
                         entity_id,
                         owner.name,
                         owner.id,
                         i,
                         link->source.entity,
                         link->source.index,
                         link->sink.entity,
                         link->sink.index,
                         link->flags);
                continue;
            }

            LOG_WARN("media_find_connected_entity_by_scan hit: owner='%s' owner_id=%u target_id=%u next_id=%u source=%u:%u sink=%u:%u flags=0x%x pass=%d",
                     owner.name,
                     owner.id,
                     entity_id,
                     next_entity_id,
                     link->source.entity,
                     link->source.index,
                     link->sink.entity,
                     link->sink.index,
                     link->flags,
                     pass);
            *out_next_entity = next_entity_id;
            *out_flags = link->flags;
            ret = 0;
            goto cleanup;
        }

        id = owner.id;
    }

cleanup:
    if (ret != 0)
    {
        LOG_WARN("media_find_connected_entity_by_scan failed: target_id=%u pass=%d ret=%d",
                 entity_id,
                 pass,
                 ret);
    }
    free(pads);
    free(link_descs);
    return ret;
}

/**
 * @description: 从指定 entity 沿 media data link 递归查找 sensor entity。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {uint32_t} entity_id 起始 entity id，通常是 rkisp_mainpath 对应 video entity。
 * @param {int} depth 当前递归深度，用于防止异常拓扑死循环。
 * @param {uint32_t *} visited 已访问 entity id 数组。
 * @param {int} visited_count 已访问 entity 数量。
 * @param {struct media_entity_desc *} out_sensor 输出 sensor entity。
 * @return {int} 0 成功，-1 未找到。
 */
static int media_find_upstream_sensor_recursive(int media_fd,
                                                uint32_t entity_id,
                                                int depth,
                                                uint32_t *visited,
                                                int visited_count,
                                                struct media_entity_desc *out_sensor)
{
    struct media_entity_desc entity;
    struct media_links_enum links;
    struct media_pad_desc *pads = NULL;
    struct media_link_desc *link_descs = NULL;
    const struct media_link_desc *link = NULL;
    uint32_t next_entity_id = 0;
    uint32_t scan_next_entity_id = 0;
    unsigned int scan_flags = 0;
    int i;
    int pass;
    int is_data_link = 0;
    int is_enabled = 0;
    int is_connected = 0;
    int ret = -1;

    /* visited 记录本次递归路径已经访问过的 entity，避免 media graph 环路导致死递归。 */
    for (i = 0; i < visited_count; ++i)
    {
        if (visited[i] == entity_id)
        {
            LOG_WARN("media_find_upstream_sensor skip visited entity: depth=%d entity_id=%u visited_index=%d",
                     depth,
                     entity_id,
                     i);
            return -1;
        }
    }
    if (visited_count >= 32)
    {
        LOG_WARN("media_find_upstream_sensor stop: visited full depth=%d entity_id=%u visited_count=%d",
                 depth,
                 entity_id,
                 visited_count);
        return -1;
    }
    /* 当前 entity 入栈，递归到下一跳时继续携带这份访问路径。 */
    visited[visited_count] = entity_id;
    ++visited_count;

    /*
     * 获取 entity_id 对应的 entity 信息。media-ctl 的 links 数量是整理后的拓扑视图，
     * 而这里拿到的是 raw MEDIA_IOC_ENUM_ENTITIES 返回值；部分 Rockchip entity
     * 自身 links 可能少于 media-ctl 展示数量，后面会通过全图扫描 owner links 兜底。
     */
    if (depth > 16 || enum_media_entity(media_fd, entity_id, &entity) != 0)
    {
        LOG_ERROR("media_find_upstream_sensor failed: depth=%d entity_id=%u", depth, entity_id);
        return -1;
    }

    LOG_WARN("media_find_upstream_sensor enter: depth=%d id=%u name='%s' type=0x%x pads=%u links=%u dev=%u:%u",
             depth,
             entity.id,
             entity.name,
             entity.type,
             entity.pads,
             entity.links,
             entity.dev.major,
             entity.dev.minor);

    /* Rockchip 4.19 头文件里 sensor 类型可能使用旧 type 宏。 */
    if (entity.type == MEDIA_ENT_T_V4L2_SUBDEV_SENSOR ||
        entity.type == MEDIA_ENT_F_CAM_SENSOR)
    {
        LOG_INFO("media_find_upstream_sensor found sensor: id=%u name='%s' type=0x%x dev=%u:%u",
                 entity.id,
                 entity.name,
                 entity.type,
                 entity.dev.major,
                 entity.dev.minor);
        if (out_sensor)
            *out_sensor = entity;
        return 0;
    }

    /* 走到这里还没找到，于是获取某个media entity的具体links信息 */
    if (entity.pads > 0)
        pads = (struct media_pad_desc *)calloc(entity.pads, sizeof(*pads));
    if (entity.links > 0)
        link_descs = (struct media_link_desc *)calloc(entity.links, sizeof(*link_descs));
    if ((entity.pads > 0 && !pads) || (entity.links > 0 && !link_descs))
    {
        LOG_WARN("media_find_upstream_sensor alloc failed: id=%u name='%s' pads=%u links=%u pads_ptr=%p links_ptr=%p",
                 entity.id,
                 entity.name,
                 entity.pads,
                 entity.links,
                 (void *)pads,
                 (void *)link_descs);
        goto cleanup;
    }

    memset(&links, 0, sizeof(links));
    links.entity = entity.id;
    links.pads = pads;
    links.links = link_descs;
    if (ioctl(media_fd, MEDIA_IOC_ENUM_LINKS, &links) != 0)
    {
        LOG_WARN("media_find_upstream_sensor enum links failed: entity_id=%u name='%s' pads=%u links=%u errno=%d(%s)",
                 entity.id,
                 entity.name,
                 entity.pads,
                 entity.links,
                 errno,
                 strerror(errno));
        goto cleanup;
    }

    /*
     * 优先沿 enabled data link 搜索；如果旧内核/驱动没有正确标记 enabled，
     * 第二轮再沿同一 media graph 内的 data link 兜底搜索。
     */
    for (pass = 0; pass < 2 && ret != 0; ++pass)
    {
        LOG_WARN("media_find_upstream_sensor pass begin: depth=%d entity='%s' id=%u pass=%d policy=%s",
                 depth,
                 entity.name,
                 entity.id,
                 pass,
                 pass == 0 ? "enabled-data-link" : "disabled-data-link-fallback");

        for (i = 0; i < entity.links; ++i)
        {
            link = &link_descs[i];
            is_data_link = ((link->flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_DATA_LINK);
            is_enabled = ((link->flags & MEDIA_LNK_FL_ENABLED) != 0);
            is_connected = (link->sink.entity == entity.id || link->source.entity == entity.id);
            next_entity_id = 0;

            LOG_WARN("media_find_upstream_sensor link: depth=%d entity='%s' id=%u pass=%d index=%d source=%u:%u sink=%u:%u flags=0x%x data=%d enabled=%d connected=%d",
                     depth,
                     entity.name,
                     entity.id,
                     pass,
                     i,
                     link->source.entity,
                     link->source.index,
                     link->sink.entity,
                     link->sink.index,
                     link->flags,
                     is_data_link,
                     is_enabled,
                     is_connected);

            if (!is_data_link)
            {
                LOG_WARN("media_find_upstream_sensor skip link: reason=not-data-link depth=%d entity='%s' index=%d flags=0x%x",
                         depth,
                         entity.name,
                         i,
                         link->flags);
                continue;
            }
            if (pass == 0 && !is_enabled)
            {
                LOG_WARN("media_find_upstream_sensor skip link: reason=disabled-in-first-pass depth=%d entity='%s' index=%d flags=0x%x",
                         depth,
                         entity.name,
                         i,
                         link->flags);
                continue;
            }
            if (pass == 1 && is_enabled)
            {
                LOG_WARN("media_find_upstream_sensor skip link: reason=enabled-in-fallback-pass depth=%d entity='%s' index=%d flags=0x%x",
                         depth,
                         entity.name,
                         i,
                         link->flags);
                continue;
            }

            /*
             * 不强依赖 source/sink 方向：从 video node 反查 sensor 时，
             * 不同内核返回的 link 方向和 entity 类型可能不完全一致。
             */
            if (link->sink.entity == entity.id)
                next_entity_id = link->source.entity;
            else
            {
                LOG_WARN("media_find_upstream_sensor skip link: reason=not-upstream depth=%d entity='%s' index=%d source=%u sink=%u",
                         depth,
                         entity.name,
                         i,
                         link->source.entity,
                         link->sink.entity);
                continue;
            }

            if (pass == 1)
            {
                LOG_WARN("media_find_upstream_sensor fallback disabled link: entity='%s' id=%u next_id=%u flags=0x%x",
                         entity.name,
                         entity.id,
                         next_entity_id,
                         link->flags);
            }

            LOG_WARN("media_find_upstream_sensor recurse: depth=%d from='%s' id=%u next_id=%u pass=%d link_index=%d",
                     depth,
                     entity.name,
                     entity.id,
                     next_entity_id,
                     pass,
                     i);

            if (media_find_upstream_sensor_recursive(media_fd,
                                                     next_entity_id,
                                                     depth + 1,
                                                     visited,
                                                     visited_count,
                                                     out_sensor) == 0)
            {
                ret = 0;
                break;
            }

            LOG_WARN("media_find_upstream_sensor backtrack: depth=%d from='%s' id=%u next_id=%u pass=%d link_index=%d",
                     depth,
                     entity.name,
                     entity.id,
                     next_entity_id,
                     pass,
                     i);
        }
    }

    /*
     * 有些 Rockchip 4.19 驱动里 video node 自身返回 links=0，但连接关系会
     * 出现在上游 subdev 的 links 中。当前 entity 本地遍历失败后，全图扫描兜底。
     */
    for (pass = 0; pass < 2 && ret != 0; ++pass)
    {
        scan_next_entity_id = 0;
        scan_flags = 0;
        if (media_find_connected_entity_by_scan(media_fd,
                                                entity.id,
                                                pass,
                                                &scan_next_entity_id,
                                                &scan_flags) != 0)
        {
            LOG_WARN("media_find_upstream_sensor scan miss: depth=%d entity='%s' id=%u pass=%d",
                     depth,
                     entity.name,
                     entity.id,
                     pass);
            continue;
        }

        LOG_WARN("media_find_upstream_sensor scan recurse: depth=%d from='%s' id=%u next_id=%u pass=%d flags=0x%x",
                 depth,
                 entity.name,
                 entity.id,
                 scan_next_entity_id,
                 pass,
                 scan_flags);
        if (media_find_upstream_sensor_recursive(media_fd,
                                                 scan_next_entity_id,
                                                 depth + 1,
                                                 visited,
                                                 visited_count,
                                                 out_sensor) == 0)
        {
            ret = 0;
            break;
        }

        LOG_WARN("media_find_upstream_sensor scan backtrack: depth=%d from='%s' id=%u next_id=%u pass=%d flags=0x%x",
                 depth,
                 entity.name,
                 entity.id,
                 scan_next_entity_id,
                 pass,
                 scan_flags);
    }

cleanup:
    LOG_WARN("media_find_upstream_sensor leave: depth=%d id=%u name='%s' ret=%d",
             depth,
             entity.id,
             entity.name,
             ret);
    free(pads);
    free(link_descs);
    return ret;
}

/**
 * @description: 从指定 entity 在 media graph 中查找绑定的 sensor entity。
 * @param {int} media_fd /dev/mediaX 文件描述符。
 * @param {uint32_t} entity_id 起始 entity id。
 * @param {struct media_entity_desc *} out_sensor 输出 sensor entity。
 * @return {int} 0 成功，-1 未找到。
 */
static int media_find_upstream_sensor(int media_fd,
                                      uint32_t entity_id,
                                      struct media_entity_desc *out_sensor)
{
    /*
     * raw MEDIA_IOC_ENUM_ENTITIES/MEDIA_IOC_ENUM_LINKS 返回的是当前 owner entity
     * 直接枚举到的 links；media-ctl -p 展示的是整理后的完整拓扑视图，可能包含
     * 其它 entity 反向补进来的 backlink。因此这里不能只信起始 video entity 的
     * links 数量，需要递归失败后配合全图 owner link 扫描兜底。
     */
    /* visited 是一次 sensor 解析过程的访问路径缓存，用于递归环路检测。 */
    uint32_t visited[32] = {0};

    return media_find_upstream_sensor_recursive(media_fd, entity_id, 0, visited, 0, out_sensor);
}

/**
 * @description: 解析当前 video node 绑定的 sensor subdev，并缓存到 ctx->sensor_subdev_path。
 * @param {V4L2CaptureCtx *} ctx V4L2 采集上下文，必须已填充 device_path。
 * @return {int} 0 成功，-1 失败。
 */
static int v4l2_capture_resolve_sensor_subdev(V4L2CaptureCtx *ctx)
{
    int media_index = 0;
    int media_fd = -1;
    int find_video_ret = -1;
    int find_sensor_ret = -1;
    int find_subdev_ret = -1;
    char media_path[PATH_MAX] = {0};
    struct stat video_st = {0};
    struct media_entity_desc video_entity = {0};
    struct media_entity_desc sensor_entity = {0};

    if (!ctx || ctx->device_path[0] == '\0')
    {
        LOG_ERROR("v4l2_capture_resolve_sensor_subdev failed: invalid args ctx=%p device_path=%s",
                  (void *)ctx,
                  ctx ? ctx->device_path : "(null)");
        return -1;
    }

    if (stat(ctx->device_path, &video_st) != 0 || !S_ISCHR(video_st.st_mode))
    {
        LOG_WARN("resolve sensor subdev failed: stat video=%s errno=%d(%s)",
                 ctx->device_path,
                 errno,
                 strerror(errno));
        return -1;
    }

    for (media_index = 0; media_index < 16; ++media_index)
    {
        snprintf(media_path, sizeof(media_path), "/dev/media%d", media_index);
        media_fd = open(media_path, O_RDWR | O_CLOEXEC);
        if (media_fd < 0)
            continue;

        /*
         * 先用 /dev/videoX 的 rdev 找到 media entity，再沿拓扑追到
         * sensor entity，最后用 sensor entity 的 rdev 反查 /dev/v4l-subdevX。
         */
        memset(&video_entity, 0, sizeof(video_entity));
        memset(&sensor_entity, 0, sizeof(sensor_entity));
        find_video_ret = media_find_entity_by_dev(media_fd,
                                                  (unsigned int)major(video_st.st_rdev),
                                                  (unsigned int)minor(video_st.st_rdev),
                                                  &video_entity);
        if (find_video_ret != 0)
        {
            LOG_WARN("resolve sensor subdev: video entity not found media=%s video=%s rdev=%u:%u",
                     media_path,
                     ctx->device_path,
                     (unsigned int)major(video_st.st_rdev),
                     (unsigned int)minor(video_st.st_rdev));
            close(media_fd);
            continue;
        }

        LOG_INFO("resolve sensor subdev: video entity media=%s video=%s id=%u name='%s' type=0x%x",
                 media_path,
                 ctx->device_path,
                 video_entity.id,
                 video_entity.name,
                 video_entity.type);

#ifdef ENABLE_RKAIQ
        /*
         * RKAIQ 已经维护了 Rockchip video node 到 sensor entity 的绑定关系。
         * 优先使用该绑定直接定位 sensor，失败后再走 media graph 上游扫描兜底。
         */
        find_sensor_ret = media_find_sensor_by_rkaiq_binding(ctx->device_path,
                                                             media_fd,
                                                             &sensor_entity);
        if (find_sensor_ret != 0)
        {
            LOG_WARN("resolve sensor subdev: rkaiq binding failed, fallback to media graph scan video=%s",
                     ctx->device_path);
        }
#else
        find_sensor_ret = -1;
#endif
        if (find_sensor_ret != 0)
            find_sensor_ret = media_find_upstream_sensor(media_fd, video_entity.id, &sensor_entity);
        if (find_sensor_ret != 0)
        {
            LOG_WARN("resolve sensor subdev: upstream sensor not found media=%s video_entity='%s' id=%u",
                     media_path,
                     video_entity.name,
                     video_entity.id);
            close(media_fd);
            continue;
        }

        find_subdev_ret = find_devnode_by_rdev("/dev/v4l-subdev",
                                               64,
                                               sensor_entity.dev.major,
                                               sensor_entity.dev.minor,
                                               ctx->sensor_subdev_path,
                                               sizeof(ctx->sensor_subdev_path));
        if (find_subdev_ret == 0)
        {
            LOG_INFO("resolve sensor subdev success: video=%s media=%s sensor='%s' subdev=%s",
                     ctx->device_path,
                     media_path,
                     sensor_entity.name,
                     ctx->sensor_subdev_path);
            close(media_fd);
            return 0;
        }

        LOG_WARN("resolve sensor subdev: sensor subdev node not found media=%s sensor='%s' dev=%u:%u",
                 media_path,
                 sensor_entity.name,
                 sensor_entity.dev.major,
                 sensor_entity.dev.minor);

        close(media_fd);
    }

    LOG_ERROR("resolve sensor subdev failed: video=%s", ctx->device_path);
    return -1;
}

/**
 * @description: 将已经释放完上层引用的采集 buffer 重新放回 V4L2 驱动队列。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {int} index 待归还的驱动 buffer 下标。
 * @return {int} 0 成功，-1 失败。
 */
static int v4l2_capture_requeue_buffer(V4L2CaptureCtx *ctx, int index)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];
    if (!ctx || ctx->fd < 0 || index < 0 || index >= ctx->buf_count)
    {
        LOG_ERROR("v4l2_capture_requeue_buffer failed: invalid args ctx=%p fd=%d index=%d",
                  (void *)ctx,
                  ctx ? ctx->fd : -1,
                  index);
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (uint32_t)index;
    buf.length = V4L2_CAPTURE_MAX_PLANES;
    buf.m.planes = planes;
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0)
    {
        LOG_ERROR("v4l2 capture qbuf failed index=%d errno=%d(%s)", index, errno, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * @description: 增加一份采集 buffer 引用。
 * @param {V4L2CaptureBufferRef *} buffer_ref 待增加引用的采集 buffer。
 * @return {void}
 */
void v4l2_capture_buffer_ref(V4L2CaptureBufferRef *buffer_ref)
{
    V4L2CaptureCtx *ctx = NULL;
    if (!buffer_ref || !buffer_ref->capture)
    {
        LOG_ERROR("v4l2_capture_buffer_ref failed: invalid args buffer_ref=%p", (void *)buffer_ref);
        return;
    }
    ctx = buffer_ref->capture;
    pthread_mutex_lock(&ctx->buffer_lock);
    buffer_ref->refs++;
    pthread_mutex_unlock(&ctx->buffer_lock);
}

/**
 * @description: 释放一份采集 buffer 引用，引用归零时将 buffer 归还给驱动。
 * @param {V4L2CaptureBufferRef *} buffer_ref 待释放引用的采集 buffer。
 * @return {void}
 */
void v4l2_capture_buffer_unref(V4L2CaptureBufferRef *buffer_ref) {
    V4L2CaptureCtx *ctx = NULL;
    int requeue = 0;
    int index = -1;
    if (!buffer_ref || !buffer_ref->capture)
    {
        LOG_ERROR("v4l2_capture_buffer_unref failed: invalid args buffer_ref=%p", (void *)buffer_ref);
        return;
    }
    ctx = buffer_ref->capture;
    pthread_mutex_lock(&ctx->buffer_lock);
    if (buffer_ref->refs > 0)
    {
        buffer_ref->refs--;
        requeue = (buffer_ref->refs == 0);
    }
    index = buffer_ref->index;
    pthread_mutex_unlock(&ctx->buffer_lock);
    if (requeue) v4l2_capture_requeue_buffer(ctx, index);
}

/**
 * @description: 获取采集 buffer 对应的 DMA-BUF fd。
 * @param {const V4L2CaptureBufferRef *} buffer_ref 采集 buffer 引用。
 * @return {int} 有效 DMA-BUF fd；-1 表示引用无效或导出失败。
 */
int v4l2_capture_buffer_dmabuf_fd(const V4L2CaptureBufferRef *buffer_ref) {
    V4L2CaptureCtx *ctx = NULL;
    if (!buffer_ref || !buffer_ref->capture)
    {
        LOG_ERROR("v4l2_capture_buffer_dmabuf_fd failed: invalid args buffer_ref=%p", (void *)buffer_ref);
        return -1;
    }
    ctx = buffer_ref->capture;
    if (buffer_ref->index < 0 || buffer_ref->index >= ctx->buf_count)
    {
        LOG_ERROR("v4l2_capture_buffer_dmabuf_fd failed: invalid buffer index=%d", buffer_ref->index);
        return -1;
    }
    return ctx->buf_dmabuf_fd[buffer_ref->index];
}

/**
 * @description: 获取采集 buffer 的完整容量。
 * @param {const V4L2CaptureBufferRef *} buffer_ref 采集 buffer 引用。
 * @return {size_t} buffer 容量；引用无效时返回 0。
 */
size_t v4l2_capture_buffer_size(const V4L2CaptureBufferRef *buffer_ref) {
    V4L2CaptureCtx *ctx = NULL;
    if (!buffer_ref || !buffer_ref->capture)
    {
        LOG_ERROR("v4l2_capture_buffer_size failed: invalid args buffer_ref=%p", (void *)buffer_ref);
        return 0;
    }
    ctx = buffer_ref->capture;
    if (buffer_ref->index < 0 || buffer_ref->index >= ctx->buf_count)
    {
        LOG_ERROR("v4l2_capture_buffer_size failed: invalid buffer index=%d", buffer_ref->index);
        return 0;
    }
    return (size_t)ctx->buf_len[buffer_ref->index];
}

#if V4L2_CAPTURE_ENABLE_OSD
/**
 * @description: 获取当前实时时钟时间，单位微秒。
 * @return {static uint64_t}
 */
static uint64_t get_realtime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 获取字符对应的 5x7 点阵数据。
 * @param {char} c 输入字符。
 * @param {uint8_t} rows 输出点阵行数据。
 * @return {static int}
 */
static int glyph5x7(char c, uint8_t rows[7]) {
    switch (c) {
        case '0': { uint8_t r[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '1': { uint8_t r[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows, r, 7); return 1; }
        case '2': { uint8_t r[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows, r, 7); return 1; }
        case '3': { uint8_t r[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(rows, r, 7); return 1; }
        case '4': { uint8_t r[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows, r, 7); return 1; }
        case '5': { uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(rows, r, 7); return 1; }
        case '6': { uint8_t r[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '7': { uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows, r, 7); return 1; }
        case '8': { uint8_t r[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows, r, 7); return 1; }
        case '9': { uint8_t r[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(rows, r, 7); return 1; }
        case 't': { uint8_t r[7] = {0x04,0x04,0x1F,0x04,0x04,0x04,0x03}; memcpy(rows, r, 7); return 1; }
        case 'f': { uint8_t r[7] = {0x06,0x08,0x08,0x1E,0x08,0x08,0x08}; memcpy(rows, r, 7); return 1; }
        case '=': { uint8_t r[7] = {0x00,0x1F,0x00,0x00,0x1F,0x00,0x00}; memcpy(rows, r, 7); return 1; }
        case ' ': { uint8_t r[7] = {0,0,0,0,0,0,0}; memcpy(rows, r, 7); return 1; }
        default: return 0;
    }
}

/**
 * @description: 在 NV12 图像的 Y 亮度平面上绘制文本。
 * @param {uint8_t *} nv12 NV12 图像数据。
 * @param {int} width 图像宽度。
 * @param {int} height 图像高度。
 * @param {int} x 文本起始 x 坐标。
 * @param {int} y 文本起始 y 坐标。
 * @param {const char *} text 待绘制文本。
 * @param {int} scale 点阵缩放倍数。
 * @return {static void}
 */
static void draw_text_nv12_y(uint8_t *nv12, int width, int height, int x, int y, const char *text, int scale) {
    int cursor_x = x;
    uint8_t glyph[7];
    uint8_t *y_plane = nv12;
    int glyph_w = 5 * scale;
    int glyph_h = 7 * scale;
    int gap = scale;
    int total_h = glyph_h + 2 * scale;

    if (!nv12 || !text || width <= 0 || height <= 0 || scale <= 0) {
        return;
    }

    for (const char *p = text; *p; ++p) {
        if (!glyph5x7(*p, glyph)) {
            cursor_x += glyph_w + gap;
            continue;
        }

        // black background for readability
        for (int yy = y - scale; yy < y + total_h - scale; ++yy) {
            if (yy < 0 || yy >= height) continue;
            for (int xx = cursor_x - scale; xx < cursor_x + glyph_w + scale; ++xx) {
                if (xx < 0 || xx >= width) continue;
                y_plane[yy * width + xx] = 16;
            }
        }

        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] >> (4 - col)) & 0x01) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = cursor_x + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (px < 0 || py < 0 || px >= width || py >= height) {
                                continue;
                            }
                            y_plane[py * width + px] = 235;
                        }
                    }
                }
            }
        }
        cursor_x += glyph_w + gap;
    }
}
#endif

/**
 * @description: 使用默认参数初始化 V4L2 采集模块。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @return {int}
 */
int v4l2_capture_init(V4L2CaptureCtx *ctx) {
    V4L2CaptureConfig config;
    memset(&config, 0, sizeof(config));
    config.device_path = CAM_DEV_PATH;
    config.width = CAPTURE_WIDTH;
    config.height = CAPTURE_HEIGHT;
    config.pixelformat = CAPTURE_FORMAT;
    config.buffer_count = V4L2_CAPTURE_BUFFER_COUNT;
    return v4l2_capture_init_with_config(ctx, &config);
}

/**
 * @description: 按指定设备、分辨率和格式初始化 V4L2 采集模块。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {V4L2CaptureConfig *} config 采集配置。
 * @return {int}
 */
int v4l2_capture_init_with_config(V4L2CaptureCtx *ctx, const V4L2CaptureConfig *config) {
    const char *device_path;
    int capture_width;
    int capture_height;
    uint32_t pixelformat;
    int buffer_count;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type;
    int ret;
    int sensor_subdev_ret = -1;
    int i;

    if (!ctx) {
        LOG_ERROR("v4l2_capture_init_with_config failed: ctx is NULL");
        return -1;
    }
    device_path = (config && config->device_path && config->device_path[0] != '\0') ? config->device_path : CAM_DEV_PATH;
    capture_width = (config && config->width > 0) ? config->width : CAPTURE_WIDTH;
    capture_height = (config && config->height > 0) ? config->height : CAPTURE_HEIGHT;
    pixelformat = (config && config->pixelformat != 0) ? config->pixelformat : CAPTURE_FORMAT;
    buffer_count = (config && config->buffer_count > 0) ? config->buffer_count : V4L2_CAPTURE_BUFFER_COUNT;
    if (buffer_count > V4L2_CAPTURE_BUFFER_COUNT) buffer_count = V4L2_CAPTURE_BUFFER_COUNT;

    memset(ctx, 0, sizeof(V4L2CaptureCtx));
    ctx->fd = -1;
    snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", device_path);
    for (i = 0; i < V4L2_CAPTURE_BUFFER_COUNT; ++i)
    {
        ctx->buf_dmabuf_fd[i] = -1;
    }

    ctx->fd = open(device_path, O_RDWR, 0);
    if (ctx->fd < 0) {
        LOG_ERROR("v4l2_capture_init_with_config failed: open device=%s errno=%d(%s)",
                  device_path,
                  errno,
                  strerror(errno));
        return -1;
    }
    printf("[INFO] open camera %s success\n", device_path);
    if (pthread_mutex_init(&ctx->buffer_lock, NULL) != 0) {
        LOG_ERROR("v4l2_capture_init_with_config failed: pthread_mutex_init");
        v4l2_capture_deinit(ctx);
        return -1;
    }
    ctx->buffer_lock_ready = 1;

    ret = ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_QUERYCAP failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        LOG_ERROR("v4l2_capture_init_with_config failed: device not support video capture");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR("v4l2_capture_init_with_config failed: device not support streaming capture");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    printf("[INFO] camera support video capture and streaming\n");
    /* 解析当前 video node 绑定的 sensor subdev */
    sensor_subdev_ret = v4l2_capture_resolve_sensor_subdev(ctx);
    if (sensor_subdev_ret != 0)
    {
        LOG_ERROR("v4l2_capture_init_with_config warning: resolve sensor subdev failed device=%s ret=%d, "
                  "dynamic fps will fallback to video node S_PARM",
                  ctx->device_path,
                  sensor_subdev_ret);
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = capture_width;
    fmt.fmt.pix_mp.height = capture_height;
    fmt.fmt.pix_mp.pixelformat = pixelformat;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, &fmt);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_S_FMT failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    ctx->format.field = fmt.fmt.pix_mp.field;
    printf("[INFO] v4l2 negotiated format: %dx%d pixelformat=0x%x num_planes=%u field=%u\n",
           fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat,
           fmt.fmt.pix_mp.num_planes,
           (unsigned)fmt.fmt.pix_mp.field);
    /*
     * Rockchip ISP 驱动在部分传感器/工作模式下可能将请求的 V4L2_FIELD_NONE
     * 改写为 V4L2_FIELD_INTERLACED。交错场帧包含两个不同时间点的场，
     * 编码后会表现为运动物体上的横向条纹（"撕裂"）。
     *
     * 尝试多轮重试强制协商为 V4L2_FIELD_NONE：
     * 部分驱动第一轮 S_FMT 不会采纳 field，第二次才会生效。
     */
    if (fmt.fmt.pix_mp.field != V4L2_FIELD_NONE) {
        int field_retry;
        for (field_retry = 0; field_retry < 3; ++field_retry) {
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == 0 &&
                fmt.fmt.pix_mp.field == V4L2_FIELD_NONE) {
                LOG_WARN("v4l2 field corrected to V4L2_FIELD_NONE after retry=%d", field_retry + 1);
                ctx->format.field = fmt.fmt.pix_mp.field;
                break;
            }
        }
        if (fmt.fmt.pix_mp.field != V4L2_FIELD_NONE) {
            LOG_WARN("v4l2 driver field=%u after %d retries (req=%u V4L2_FIELD_NONE); "
                     "interlaced frames WILL cause tearing artifacts. "
                     "Try: v4l2-ctl -d %s --set-fmt-video=field=none",
                     (unsigned)fmt.fmt.pix_mp.field,
                     field_retry,
                     (unsigned)V4L2_FIELD_NONE,
                     device_path);
            ctx->format.field = fmt.fmt.pix_mp.field;
        }
    }
    ctx->format.width = (int)fmt.fmt.pix_mp.width;
    ctx->format.height = (int)fmt.fmt.pix_mp.height;
    ctx->format.pixelformat = fmt.fmt.pix_mp.pixelformat;
    ctx->format.num_planes = fmt.fmt.pix_mp.num_planes;
    if (ctx->format.num_planes > VIDEO_MAX_PLANES) {
        ctx->format.num_planes = VIDEO_MAX_PLANES;
    }
    for (i = 0; i < (int)ctx->format.num_planes; ++i) {
        ctx->format.planes[i].bytesperline = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        ctx->format.planes[i].sizeimage = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
        printf("[INFO] v4l2 negotiated plane[%d]: bytesperline=%u sizeimage=%u\n",
               i,
               ctx->format.planes[i].bytesperline,
               ctx->format.planes[i].sizeimage);
    }

    memset(&req, 0, sizeof(req));
    // V4L2 缓冲深度会影响采集稳定性。
    // demo 和 v4l2-ctl 都使用 4 个 mmap buffer；2 个 buffer 在有用户态拷贝和编码调度时容易让 ISP 队列变薄，
    // 从而把 VIDIOC_DQBUF 等待时间拉长到 40ms 以上，所以这里与测试命令保持一致。
    req.count = (uint32_t)buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_REQBUFS failed", ret);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    ctx->buf_count = req.count;
    if (ctx->buf_count > V4L2_CAPTURE_BUFFER_COUNT) {
        LOG_ERROR("v4l2_capture_init_with_config failed: driver returned too many buffers count=%d max=%d",
                  ctx->buf_count,
                  V4L2_CAPTURE_BUFFER_COUNT);
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    printf("[INFO] request %d buffers success\n", ctx->buf_count);

    for (i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = V4L2_CAPTURE_MAX_PLANES;
        buf.m.planes = planes;

        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("v4l2_capture_init_with_config failed: query buffer=%d errno=%d(%s)",
                      i,
                      errno,
                      strerror(errno));
            v4l2_capture_deinit(ctx);
            return -1;
        }

        ctx->buf[i] = mmap(NULL,
                           planes[0].length,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           ctx->fd,
                           planes[0].m.mem_offset);
        if (ctx->buf[i] == MAP_FAILED) {
            LOG_ERROR("v4l2_capture_init_with_config failed: mmap buffer=%d errno=%d(%s)",
                      i,
                      errno,
                      strerror(errno));
            ctx->buf[i] = NULL;
            v4l2_capture_deinit(ctx);
            return -1;
        }
        ctx->buf_len[i] = (int)planes[0].length;
        ctx->buffer_refs[i].capture = ctx;
        ctx->buffer_refs[i].index = i;
        {
            /**
             * 尝试导出 DMA-BUF fd，失败不影响正常采集和用户态拷贝链路，但会导致零拷贝链路无法使用。
             */
            struct v4l2_exportbuffer expbuf;
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            expbuf.index = (uint32_t)i;
            expbuf.plane = 0;
            expbuf.flags = O_CLOEXEC;
            if (ioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) == 0) {
                ctx->buf_dmabuf_fd[i] = expbuf.fd;
            } else {
                LOG_WARN("v4l2 capture export dmabuf failed buffer=%d errno=%d(%s)",
                         i,
                         errno,
                         strerror(errno));
            }
        }
        printf("[INFO] buffer %d mapped: addr=%p, len=%d\n", i, ctx->buf[i], ctx->buf_len[i]);

        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("v4l2_capture_init_with_config failed: qbuf buffer=%d errno=%d(%s)",
                      i,
                      errno,
                      strerror(errno));
            v4l2_capture_deinit(ctx);
            return -1;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        print_v4l2_error("VIDIOC_STREAMON failed", ret);
        v4l2_capture_deinit(ctx);
        return -1;
    }
    printf("[INFO] start streaming capture success\n");

    // 预分配一块用户态缓存。
    // 后续每次取帧都会先把 DQBUF 得到的数据拷贝到这里，再把原始缓冲 QBUF 回驱动。
    // 这样上层拿到的 frame_data 在本次函数返回后仍然有效，不会因为驱动复用底层缓冲而被覆盖。
    ctx->frame_cache_len = ctx->format.width * ctx->format.height * 3 / 2;
    ctx->frame_cache = (uint8_t *)malloc((size_t)ctx->frame_cache_len);
    if (!ctx->frame_cache) {
        LOG_ERROR("v4l2_capture_init_with_config failed: malloc frame cache size=%d", ctx->frame_cache_len);
        v4l2_capture_deinit(ctx);
        return -1;
    }

    return 0;
}

/**
 * @description: 通过绑定的 sensor subdev 设置帧间隔。
 * @param {V4L2CaptureCtx *} ctx V4L2 采集上下文，需已解析 sensor_subdev_path。
 * @param {int} fps 目标帧率。
 * @return {int} 0 成功，-1 失败。
 */
static int v4l2_capture_set_sensor_subdev_fps(V4L2CaptureCtx *ctx, int fps)
{
    int subdev_fd = -1;
    struct v4l2_subdev_frame_interval interval = {0};

    if (!ctx || ctx->sensor_subdev_path[0] == '\0' || fps <= 0)
    {
        LOG_WARN("v4l2_capture_set_sensor_subdev_fps failed: invalid args ctx=%p subdev=%s fps=%d",
                 (void *)ctx,
                 ctx ? ctx->sensor_subdev_path : "(null)",
                 fps);
        return -1;
    }

    subdev_fd = open(ctx->sensor_subdev_path, O_RDWR | O_CLOEXEC);
    if (subdev_fd < 0)
    {
        LOG_WARN("v4l2 sensor subdev fps failed: open subdev=%s fps=%d errno=%d(%s)",
                 ctx->sensor_subdev_path,
                 fps,
                 errno,
                 strerror(errno));
        return -1;
    }

    /* Rockchip sensor 驱动在 pad0 暴露 source pad，帧率切换走 subdev API。 */
    interval.pad = 0;
    interval.interval.numerator = 1;
    interval.interval.denominator = (uint32_t)fps;
    if (ioctl(subdev_fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &interval) < 0)
    {
        LOG_WARN("v4l2 sensor subdev fps failed: subdev=%s fps=%d errno=%d(%s)",
                 ctx->sensor_subdev_path,
                 fps,
                 errno,
                 strerror(errno));
        close(subdev_fd);
        return -1;
    }

    close(subdev_fd);
    LOG_WARN("v4l2 sensor subdev fps updated: video=%s subdev=%s request=%d actual=%u/%u",
             ctx->device_path,
             ctx->sensor_subdev_path,
             fps,
             interval.interval.denominator,
             interval.interval.numerator);
    return 0;
}

int v4l2_capture_set_fps(V4L2CaptureCtx *ctx, int fps)
{
    struct v4l2_streamparm parm = {0};

    if (!ctx || ctx->fd < 0 || fps <= 0)
    {
        LOG_ERROR("v4l2_capture_set_fps failed: invalid args ctx=%p fd=%d fps=%d",
                  (void *)ctx,
                  ctx ? ctx->fd : -1,
                  fps);
        return -1;
    }

    /*
     * rkisp mainpath/selfpath video node 通常不支持 VIDIOC_S_PARM。
     * 动态帧率优先对绑定的 sensor subdev 下发 VIDIOC_SUBDEV_S_FRAME_INTERVAL。
     */
    if (v4l2_capture_set_sensor_subdev_fps(ctx, fps) == 0)
        return 0;

    /* 保留 video node S_PARM 作为非 Rockchip 或其他驱动的兜底路径。 */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(ctx->fd, VIDIOC_G_PARM, &parm) < 0)
    {
        LOG_WARN("v4l2_capture_set_fps: VIDIOC_G_PARM failed fps=%d errno=%d(%s), try S_PARM",
                 fps,
                 errno,
                 strerror(errno));
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    }

    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = (uint32_t)fps;
    if (ioctl(ctx->fd, VIDIOC_S_PARM, &parm) < 0)
    {
        LOG_WARN("v4l2_capture_set_fps failed: fps=%d errno=%d(%s)",
                 fps,
                 errno,
                 strerror(errno));
        return -1;
    }

    LOG_WARN("v4l2 capture fps updated: request=%d actual=%u/%u",
             fps,
             parm.parm.capture.timeperframe.denominator,
             parm.parm.capture.timeperframe.numerator);
    return 0;
}

/**
 * @description: 采集一帧并返回 V4L2 驱动 buffer 引用，供 DMA-BUF 零拷贝链路继续传递。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {uint8_t **} frame_data 输出当前 mmap buffer 地址，仅在 buffer_ref 有效期内可用。
 * @param {int *} frame_len 输出帧有效长度。
 * @param {uint64_t *} frame_id 输出递增采集帧号。
 * @param {uint64_t *} dqbuf_ts_us VIDIOC_DQBUF 返回后的单调时钟时间。
 * @param {uint64_t *} camera_buffer_wait_us 驱动时间戳到 DQBUF 返回的等待时间。
 * @param {uint64_t *} dqbuf_ioctl_duration_us VIDIOC_DQBUF ioctl 调用耗时。
 * @param {V4L2CaptureBufferRef **} buffer_ref 输出采集 buffer 引用，调用方最终必须 unref。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_acquire_frame(V4L2CaptureCtx *ctx,
                               uint8_t **frame_data,
                               int *frame_len,
                               uint64_t *frame_id,
                               uint64_t *dqbuf_ts_us,
                               uint64_t *camera_buffer_wait_us,
                               uint64_t *dqbuf_ioctl_duration_us,
                               V4L2CaptureBufferRef **buffer_ref) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];
    uint64_t driver_ts_us;
    uint64_t dqbuf_ioctl_start_us;

    if (!ctx || ctx->fd < 0 || !frame_data || !frame_len || !frame_id ||
        !dqbuf_ts_us || !camera_buffer_wait_us || !dqbuf_ioctl_duration_us ||
        !buffer_ref) {
        LOG_ERROR("v4l2_capture_acquire_frame failed: invalid arguments");
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = V4L2_CAPTURE_MAX_PLANES;
    buf.m.planes = planes;

    dqbuf_ioctl_start_us = get_now_us();
    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        LOG_ERROR("v4l2_capture_acquire_frame failed: dqbuf errno=%d(%s)", errno, strerror(errno));
        return -1;
    }
    *dqbuf_ts_us = get_now_us();
    *dqbuf_ioctl_duration_us = *dqbuf_ts_us - dqbuf_ioctl_start_us;
    driver_ts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + (uint64_t)buf.timestamp.tv_usec;
    *camera_buffer_wait_us = (*dqbuf_ts_us >= driver_ts_us) ? (*dqbuf_ts_us - driver_ts_us) : 0;
    if (buf.index >= (uint32_t)ctx->buf_count) {
        LOG_ERROR("v4l2_capture_acquire_frame failed: invalid buffer index=%u count=%d", buf.index, ctx->buf_count);
        return -1;
    }

    ctx->frame_id++;
    *frame_id = ctx->frame_id;
    *frame_data = (uint8_t *)ctx->buf[buf.index];
    *frame_len = (int)planes[0].bytesused;
    pthread_mutex_lock(&ctx->buffer_lock);
    ctx->buffer_refs[buf.index].refs = 1;
    pthread_mutex_unlock(&ctx->buffer_lock);
    *buffer_ref = &ctx->buffer_refs[buf.index];
    return 0;
}

/**
 * @description: 采集一帧并拷贝到内部 frame_cache，返回稳定的用户态帧指针。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @param {uint8_t **} frame_data 输出帧数据，指向内部 frame_cache。
 * @param {int *} frame_len 输出帧有效长度。
 * @param {uint64_t *} frame_id 输出递增采集帧号。
 * @param {uint64_t *} dqbuf_ts_us VIDIOC_DQBUF 返回后的单调时钟时间。
 * @param {uint64_t *} camera_buffer_wait_us 驱动时间戳到 DQBUF 返回的等待时间。
 * @param {uint64_t *} dqbuf_ioctl_duration_us VIDIOC_DQBUF ioctl 调用耗时。
 * @param {uint64_t *} mmap_to_frame_cache_copy_us mmap buffer 拷贝到 frame_cache 的耗时。
 * @return {int} 0 成功，-1 失败。
 */
int v4l2_capture_frame(V4L2CaptureCtx *ctx,
                       uint8_t **frame_data,
                       int *frame_len,
                       uint64_t *frame_id,
                       uint64_t *dqbuf_ts_us,
                       uint64_t *camera_buffer_wait_us,
                       uint64_t *dqbuf_ioctl_duration_us,
                       uint64_t *mmap_to_frame_cache_copy_us) {
    if (!ctx || ctx->fd < 0 || !frame_data || !frame_len || !frame_id || !dqbuf_ts_us || !camera_buffer_wait_us || !dqbuf_ioctl_duration_us || !mmap_to_frame_cache_copy_us) {
        LOG_ERROR("v4l2_capture_frame failed: invalid args ctx=%p fd=%d frame_data=%p frame_len=%p frame_id=%p",
                  (void *)ctx,
                  ctx ? ctx->fd : -1,
                  (void *)frame_data,
                  (void *)frame_len,
                  (void *)frame_id);
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[V4L2_CAPTURE_MAX_PLANES];

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = V4L2_CAPTURE_MAX_PLANES;
    buf.m.planes = planes;

    uint64_t dqbuf_ioctl_start_us = get_now_us();
    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        LOG_ERROR("v4l2_capture_frame failed: dqbuf errno=%d(%s)", errno, strerror(errno));
        return -1;
    }
    *dqbuf_ts_us = get_now_us();
    *dqbuf_ioctl_duration_us = *dqbuf_ts_us - dqbuf_ioctl_start_us;
#if 1
    {
        // 驱动时间戳表示该帧在内核侧的时间点；与 dqbuf_ts_us 的差值可反映帧在驱动队列中的滞留时间。
        uint64_t driver_ts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + (uint64_t)buf.timestamp.tv_usec;
        // 计算从驱动开始采集到v4l2_capture_frame返回的时间差，反映了驱动处理这一帧的总耗时
        // 包括这一帧积压在驱动缓冲区内的时间？
        *camera_buffer_wait_us = (*dqbuf_ts_us >= driver_ts_us) ? (*dqbuf_ts_us - driver_ts_us) : 0;
        // printf("[TRACE] step=driver_timestamp driver_ts_us=%" PRIu64
        //        " camera_buffer_wait_us=%" PRIu64
        //        " ts_flags=0x%x\n",
        //        driver_ts_us,
        //        *camera_buffer_wait_us,
        //        (unsigned)(buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK));
    }
#endif
    ctx->frame_id++;
    *frame_id = ctx->frame_id;
    // printf("[TRACE] frame=%" PRIu64 " step=after_vidioc_dqbuf ts_us=%" PRIu64 "\n",
    //        *frame_id, *dqbuf_ts_us);

    // 某些驱动的 bytesused 可能大于初始预估值，这里按需扩容，避免越界。
    /**
     * todo:2026-08-16, frame_cache有扩容，那么映射到应用空间的mmap的buf_len会变吗，如果不会变，那这里扩容是不是就没必要了。
     */
    if ((int)planes[0].bytesused > ctx->frame_cache_len) {
        uint8_t *new_cache = (uint8_t *)realloc(ctx->frame_cache, planes[0].bytesused);
        if (!new_cache) {
            LOG_ERROR("v4l2_capture_frame failed: realloc frame cache size=%u", planes[0].bytesused);
            if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
                LOG_ERROR("v4l2_capture_frame failed: re-qbuf after realloc errno=%d(%s)", errno, strerror(errno));
            }
            return -1;
        }
        ctx->frame_cache = new_cache;
        ctx->frame_cache_len = (int)planes[0].bytesused;
    }

    // 关键修复点：
    // 旧实现直接把 mmap 缓冲地址返回给上层，然后马上执行 QBUF。
    // 这样一旦驱动重新使用这块缓冲，调用方手里的指针就可能在编码前被新帧覆盖。
    // 现在先拷贝到 frame_cache，再 QBUF，保证上层在下一次取帧前看到的是稳定数据。
    // 这一帧的拷贝耗时4-5ms，拷贝的size？
    {
        uint64_t copy_start_us = get_now_us();
        memcpy(ctx->frame_cache, ctx->buf[buf.index], planes[0].bytesused);
        *mmap_to_frame_cache_copy_us = get_now_us() - copy_start_us;
    }
    *frame_data = ctx->frame_cache;
    *frame_len = (int)planes[0].bytesused;
    // 低延迟思路：
    // 避免每帧打印日志，串口/控制台 IO 会显著拖慢实时链路。

    // 原始驱动缓冲在数据复制完成后即可立即回队，继续参与下一轮采集。
    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("v4l2_capture_frame failed: re-qbuf errno=%d(%s)", errno, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @description: 释放 V4L2 采集模块资源。
 * @param {V4L2CaptureCtx *} ctx 采集上下文。
 * @return {void}
 */
void v4l2_capture_deinit(V4L2CaptureCtx *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }

    for (int i = 0; i < ctx->buf_count; i++) {
        if (ctx->buf_dmabuf_fd[i] >= 0) {
            close(ctx->buf_dmabuf_fd[i]);
            ctx->buf_dmabuf_fd[i] = -1;
        }
        if (ctx->buf[i]) {
            munmap(ctx->buf[i], (size_t)ctx->buf_len[i]);
            ctx->buf[i] = NULL;
        }
    }

    if (ctx->frame_cache) {
        free(ctx->frame_cache);
        ctx->frame_cache = NULL;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    if (ctx->buffer_lock_ready) {
        pthread_mutex_destroy(&ctx->buffer_lock);
    }

    memset(ctx, 0, sizeof(V4L2CaptureCtx));
    ctx->fd = -1;
    printf("[INFO] v4l2 capture deinit success\n");
}
