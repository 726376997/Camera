#ifndef DRAW_H_
#define DRAW_H_

#include <stddef.h>

void DrawInit(void);
void lcd_show_rgb(const unsigned char *rgb, int src_w, int src_h);
int jpeg_to_rgb(const unsigned char *jpeg_buf, size_t jpeg_len,
                unsigned char **rgb_out, int *w_out, int *h_out);
#endif /* DRAW_H_ */
