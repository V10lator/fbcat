/* Copyright © 2009 Piotr Lewandowski
 * Copyright © 2009, 2013, 2014 Jakub Wilk
 * Copyright © 2013 David Lechner
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991.
 */

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

#if !defined(le32toh) || !defined(le16toh)

#if BYTE_ORDER == LITTLE_ENDIAN
#define le32toh(x) (x)
#define le16toh(x) (x)
#else
#include <byteswap.h>
#define le32toh(x) bswap_32(x)
#define le16toh(x) bswap_16(x)
#endif

#endif

static const char default_fbdev[] = "/dev/fb0";

static const char bug_tracker_url[] = "https://bitbucket.org/jwilk/fbcat/issues";

static void posix_error(const char *s, ...)
{
  va_list argv;
  va_start(argv, s);
  fprintf(stderr, "fbcat: ");
  vfprintf(stderr, s, argv);
  fprintf(stderr, ": ");
  perror(NULL);
  va_end(argv);
  exit(2);
}

static void not_supported(const char *s)
{
  fprintf(stderr,
    "fbcat: not yet supported: %s\n"
    "Please file a bug at <%s>.\n",
    s,
    bug_tracker_url
  );
  exit(3);
}

static inline unsigned char get_color(unsigned int pixel, const struct fb_bitfield *bitfield, uint16_t *colormap)
{
  return colormap[(pixel >> bitfield->offset) & ((1 << bitfield->length) - 1)] >> 8;
}

static void dump_video_memory(
  const unsigned char *video_memory,
  const struct fb_var_screeninfo *info,
  const struct fb_cmap *colormap,
  unsigned int line_length,
  FILE *fp
)
{
  unsigned int x, y;
  unsigned char *row = malloc(info->xres * 3);
  if (row == NULL)
    posix_error("malloc failed");
  assert(row != NULL);

  fprintf(fp, "P6 %" PRIu32 " %" PRIu32 " 255\n", info->xres, info->yres);
  for (y = 0; y < info->yres; y++)
  {
    const unsigned char *current;
    current = video_memory + (y + info->yoffset) * line_length + info->xoffset * 4;
    for (x = 0; x < info->xres; x++)
    {
      unsigned int pixel = 0;
      /* The following code assumes little-endian byte ordering in the frame
	   * buffer. */
      pixel = le32toh(*((uint32_t *) current));
      current += 4;
	  
	  /* V10lator: For some reason (only qualcomm might know) the pixels are f*cked up:
	   * Transparent = Red,
	   * Blue = Green,
	   * Green = Blue */
      row[x * 3 + 0] = get_color(pixel, &info->transp, colormap->red);
      row[x * 3 + 1] = get_color(pixel, &info->blue, colormap->green);
      row[x * 3 + 2] = get_color(pixel, &info->green, colormap->blue);
    }
    if (fwrite(row, 1, info->xres * 3, fp) != info->xres * 3)
      posix_error("write error");
  }

  free(row);
}

int main(int argc, const char **argv)
{
  const char *fbdev_name;
  int fd;

  bool show_usage = false;
  if (isatty(STDOUT_FILENO))
  {
    fprintf(stderr, "fbcat: refusing to write binary data to a terminal\n");
    show_usage = true;
  }
  if (argc > 2)
    show_usage = true;
  if (show_usage)
  {
    fprintf(stderr, "Usage: %s [fbdev]\n", argv[0]);
    return 1;
  }

  if (argc == 2)
    fbdev_name = argv[1];
  else
  {
    fbdev_name = getenv("FRAMEBUFFER");
    if (fbdev_name == NULL || fbdev_name[0] == '\0')
      fbdev_name = default_fbdev;
  }

  fd = open(fbdev_name, O_RDONLY);
  if (fd == -1)
    posix_error("could not open %s", fbdev_name);

  struct fb_fix_screeninfo fix_info;
  struct fb_var_screeninfo var_info;
  uint16_t colormap_data[4][1 << 8];
  struct fb_cmap colormap =
  {
    0,
    1 << 8,
    colormap_data[0],
    colormap_data[1],
    colormap_data[2],
    colormap_data[3],
  };

  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix_info))
    posix_error("FBIOGET_FSCREENINFO failed");

  if (fix_info.type != FB_TYPE_PACKED_PIXELS)
    not_supported("framebuffer type is not PACKED_PIXELS");

  if (ioctl(fd, FBIOGET_VSCREENINFO, &var_info))
    posix_error("FBIOGET_VSCREENINFO failed");

  if (var_info.red.length > 8 || var_info.green.length > 8 || var_info.blue.length > 8)
    not_supported("color depth > 8 bits per component");

  unsigned int i;
  for (i = 0; i < (1U << var_info.red.length); i++)
    colormap.red[i] = i * 0xffff / ((1 << var_info.red.length) - 1);
  for (i = 0; i < (1U << var_info.green.length); i++)
    colormap.green[i] = i * 0xffff / ((1 << var_info.green.length) - 1);
  for (i = 0; i < (1U << var_info.blue.length); i++)
    colormap.blue[i] = i * 0xffff / ((1 << var_info.blue.length) - 1);

  if (var_info.bits_per_pixel < 8)
    not_supported("< 8 bpp");
  const size_t mapped_length = fix_info.line_length * (var_info.yres + var_info.yoffset);
  unsigned char *video_memory = mmap(NULL, mapped_length, PROT_READ, MAP_SHARED, fd, 0);
  if (video_memory == MAP_FAILED)
    posix_error("mmap failed");
  dump_video_memory(video_memory, &var_info, &colormap, fix_info.line_length, stdout);

  if (fclose(stdout))
    posix_error("write error");

  /* deliberately ignore errors */
  munmap(video_memory, mapped_length);
  close(fd);
  return 0;
}

/* vim:set ts=2 sts=2 sw=2 et: */
