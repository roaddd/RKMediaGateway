// SPDX-License-Identifier: GPL-2.0
/*
 * 枚举 OV13850 子设备支持的帧间隔。
 *
 * 这个测试会同时打印请求的 width/height 和驱动返回的 width/height。
 * 正确的 enum_frame_interval() 实现应该只返回当前请求分辨率支持的帧率。
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef MEDIA_BUS_FMT_SBGGR10_1X10
#define MEDIA_BUS_FMT_SBGGR10_1X10 0x3007
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct frame_size {
	unsigned int width;
	unsigned int height;
};

static double interval_to_fps(const struct v4l2_fract *interval)
{
	if (!interval->numerator)
		return 0.0;

	return (double)interval->denominator / interval->numerator;
}

static void enum_intervals(int fd, const char *dev, unsigned int pad,
			   unsigned int code, const struct frame_size *size)
{
	unsigned int index;
	unsigned int count = 0;

	printf("\nrequest: dev=%s pad=%u code=0x%04x width=%u height=%u\n",
	       dev, pad, code, size->width, size->height);

	for (index = 0; ; index++) {
		struct v4l2_subdev_frame_interval_enum fie;

		memset(&fie, 0, sizeof(fie));
		fie.index = index;
		fie.pad = pad;
		fie.code = code;
		fie.width = size->width;
		fie.height = size->height;
		fie.which = V4L2_SUBDEV_FORMAT_ACTIVE;

		if (ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, &fie) < 0) {
			if (errno == EINVAL) {
				printf("  stop: index=%u returned EINVAL\n", index);
				break;
			}

			printf("  error: index=%u ioctl failed: %s\n",
			       index, strerror(errno));
			break;
		}

		printf("  index=%u returned: width=%u height=%u interval=%u/%u fps=%.3f\n",
		       index, fie.width, fie.height,
		       fie.interval.numerator, fie.interval.denominator,
		       interval_to_fps(&fie.interval));
		count++;
	}

	printf("  total returned intervals: %u\n", count);
}

static unsigned int parse_u32(const char *s)
{
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(s, &end, 0);
	if (errno || !end || *end != '\0' || value > 0xffffffffUL) {
		fprintf(stderr, "invalid number: %s\n", s);
		exit(EXIT_FAILURE);
	}

	return (unsigned int)value;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-d /dev/v4l-subdevX] [-p pad] [-c mbus_code]\n"
		"\n"
		"Default: dev=/dev/v4l-subdev3 pad=0 code=0x3007\n"
		"\n"
		"Example:\n"
		"  %s -d /dev/v4l-subdev3 -p 0 -c 0x3007\n",
		prog, prog);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/v4l-subdev3";
	unsigned int pad = 0;
	unsigned int code = MEDIA_BUS_FMT_SBGGR10_1X10;
	const struct frame_size sizes[] = {
		{ 2112, 1568 },
		{ 4224, 3136 },
	};
	int fd;
	int opt;
	unsigned int i;

	while ((opt = getopt(argc, argv, "d:p:c:h")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 'p':
			pad = parse_u32(optarg);
			break;
		case 'c':
			code = parse_u32(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
		return EXIT_FAILURE;
	}

	for (i = 0; i < ARRAY_SIZE(sizes); i++)
		enum_intervals(fd, dev, pad, code, &sizes[i]);

	close(fd);
	return EXIT_SUCCESS;
}
