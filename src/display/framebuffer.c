#include "disp_manager.h"
#include "common/logger.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int FrameBufferDispInit(void);
static void FrameBufferDispExit(void);
static void FrameBufferGetDispBuff(PDispBuff ptDispBuff);
static void FrameBufferCleanScreen(unsigned int Color);

DispOpr FrameBufferDispOpr = {
    .name = "fb",
    .DispInit = FrameBufferDispInit,
    .DispExit = FrameBufferDispExit,
    .GetDispBuff = FrameBufferGetDispBuff,
    .CleanScreen = FrameBufferCleanScreen,
};

static int g_fb_fd;
static int g_screen_size;
static unsigned char *g_fbmem;
static struct fb_var_screeninfo g_tvar;

static int FrameBufferDispInit(void)
{
    g_fb_fd = open(FB_DEVICES_NAME, O_RDWR);
    if (g_fb_fd < 0)
    {
        ERR_PRINTF("FrameBufferDispInit: open %s failed\n", FB_DEVICES_NAME);
        return -1;
    }

    /* 将LCD属性赋值到g_tvar中 */
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_tvar))
    {
        ERR_PRINTF("FrameBufferDispInit: can't get g_tvar\n");
        close(g_fb_fd);
        g_fb_fd = -1;
        return -1;
    }

    g_screen_size = g_tvar.xres * g_tvar.yres * g_tvar.bits_per_pixel / 8;
    g_fbmem = mmap(NULL, g_screen_size, PROT_WRITE | PROT_READ, MAP_SHARED, g_fb_fd, 0);
    if (g_fbmem == MAP_FAILED)
    {
        ERR_PRINTF("FrameBufferDispInit: mmap failed\n");
        close(g_fb_fd);
        g_fb_fd = -1;
        return -1;
    }
    return 0;
}

static void FrameBufferDispExit(void)
{
    if (g_fbmem && g_fbmem != MAP_FAILED)
    {
        munmap(g_fbmem, g_screen_size);
    }

    close(g_fb_fd);
    g_fb_fd = -1;
}

static void FrameBufferGetDispBuff(PDispBuff ptDispBuff)
{
    ptDispBuff->buff = g_fbmem;
    ptDispBuff->iBpp = g_tvar.bits_per_pixel;
    ptDispBuff->iXres = g_tvar.xres;
    ptDispBuff->iYres = g_tvar.yres;
}

static void FrameBufferCleanScreen(unsigned int Color)
{
#if 0
    // 遍历每一个像素，并为其赋值-----该方法效率极其低下，每一个像素点都涉及乘法和加法以及switch判断，执行的运算会很多
    int xres = g_tvar.xres;
    int yres = g_tvar.yres;
    int x, y;
    int i;
    for (y = 0; y < g_tvar.yres; y++)
    {
        x = y * g_tvar.xres;
        for (i = 0; i < g_tvar.xres; i++, x++)
        {
            PutPixel(x, y, Color);
        }
    }
#endif

    // 该方法虽然也要执行xres*yres次，但是进行的运算会少很多
    int i;

    unsigned char *pen_8 = g_fbmem;
    unsigned short *pen_16 = (unsigned short *)pen_8;
    unsigned int *pen_32 = (unsigned int *)pen_8;

    unsigned int red, green, blue;

    /* 根据设备的bpp选择颜色格式 */
    switch (g_tvar.bits_per_pixel)
    {
    case 8:
    {
        /* 8位颜色为1个字节，因此将每个字节颜色设置为dwColor */
        memset(g_fbmem, Color, g_screen_size);
        break;
    }
    case 16:
    {
        /* 565 */
        red = (Color >> 16) & 0xff;
        green = (Color >> 8) & 0xff;
        blue = (Color >> 0) & 0xff;
        Color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);

        /* 16位颜色为2个字节，因此将每2个字节颜色设置为dwColor */
        for (i = 0; i < g_screen_size; i += 2, pen_16++)
        {
            *pen_16 = Color;
        }
        break;
    }
    case 32:
    {
        /* 32位颜色为4个字节，因此将每4个字节颜色设置为dwColor */
        for (i = 0; i < g_screen_size; i += 4, pen_32++)
        {
            *pen_32 = Color;
        }
        break;
    }
    default:
    {
        printf("can't surport %dbpp\n", g_tvar.bits_per_pixel);
        break;
    }
    }
}

void FrameBufferRegister(void)
{
    RegisterDispOpr(&FrameBufferDispOpr);
}