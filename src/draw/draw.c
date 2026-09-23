#include "display/disp_manager.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <jpeglib.h>
#include <setjmp.h>

typedef struct
{
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
} my_error_mgr;

static DispBuff g_DispBuff;

void DrawInit(void)
{
    GetDispBuffer(&g_DispBuff);
}

/* ---------- 把 RGB 数据写到 LCD，支持 16bpp / 32bpp ---------- */
/* ---------- 把 RGB 数据等比缩放到 LCD 居中显示 ---------- */
void lcd_show_rgb(const unsigned char *rgb, int src_w, int src_h)
{
    int x, y;
    int dst_w = g_DispBuff.iXres;
    int dst_h = g_DispBuff.iYres;
    int bpp = g_DispBuff.iBpp;
    int line_length = g_DispBuff.iXres * g_DispBuff.iBpp / 8;
    unsigned char *fb_mem = g_DispBuff.buff;

    /* 计算等比缩放后的显示尺寸 */
    int disp_w = dst_w;
    int disp_h = (int)((long)dst_w * src_h / src_w);
    if (disp_h > dst_h)
    {
        disp_h = dst_h;
        disp_w = (int)((long)dst_h * src_w / src_h);
    }
    int off_x = (dst_w - disp_w) / 2;
    int off_y = (dst_h - disp_h) / 2;

    /* 黑边只清一次（首帧清屏即可，后续无需重复清） */
    static int cleared = 0;
    if (!cleared)
    {
        CleanScreen(0x0);
        cleared = 1;
    }

    for (y = 0; y < disp_h; y++)
    {
        int sy = y * src_h / disp_h; /* 最近邻采样 */
        const unsigned char *src_row = rgb + sy * src_w * 3;
        unsigned char *dst_row = fb_mem + (y + off_y) * line_length;

        for (x = 0; x < disp_w; x++)
        {
            int sx = x * src_w / disp_w;
            unsigned char r = src_row[sx * 3 + 0];
            unsigned char g = src_row[sx * 3 + 1];
            unsigned char b = src_row[sx * 3 + 2];

            int dx = x + off_x;
            if (bpp == 32)
            {
                unsigned int *p = (unsigned int *)(dst_row + dx * 4);
                *p = (0xffu << 24) | (r << 16) | (g << 8) | b;
            }
            else
            {
                unsigned short *p = (unsigned short *)(dst_row + dx * 2);
                *p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
}

static void my_error_exit(j_common_ptr cinfo)
{
    my_error_mgr *err = (my_error_mgr *)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

static void my_emit_message(j_common_ptr cinfo, int msg_level)
{
    (void)cinfo;
    (void)msg_level; /* 吞掉所有警告 */
}

static size_t find_jpeg_eoi(const unsigned char *data, size_t len)
{
    if (len < 2)
        return len;
    size_t start = (len > 64) ? len - 64 : 0;
    for (size_t i = len - 2;; i--)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD9)
            return i + 2;
        if (i == start)
            break;
    }
    return len;
}

/* ---------- MJPEG 解码为 RGB ---------- */
int jpeg_to_rgb(const unsigned char *jpeg_buf, size_t jpeg_len,
                unsigned char **rgb_out, int *w_out, int *h_out)
{
    struct jpeg_decompress_struct cinfo;
    my_error_mgr jerr;

    /* 修正长度：截到真正的 EOI */
    jpeg_len = find_jpeg_eoi(jpeg_buf, jpeg_len);

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    jerr.pub.emit_message = my_emit_message;

    if (setjmp(jerr.setjmp_buffer))
    {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_buf, jpeg_len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width;
    int h = cinfo.output_height;
    int comp = cinfo.output_components;

    unsigned char *rgb = malloc((size_t)w * h * comp);
    if (!rgb)
    {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    int row_stride = w * comp;
    unsigned char *row_ptr = rgb;
    while (cinfo.output_scanline < cinfo.output_height)
    {
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
        row_ptr += row_stride;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *rgb_out = rgb;
    *w_out = w;
    *h_out = h;
    return 0;
}