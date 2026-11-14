#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
} jpeg_image_t;

// Góc xoay output. Giá trị khớp với `jpeg_rotate_t` của esp_new_jpeg nhưng khai
// báo độc lập ở đây để header này không lộ chi tiết thư viện bên trong
// managed_components ra ngoài.
// Thư viện chỉ hỗ trợ xoay khi CẢ width lẫn height của JPEG đều chia hết cho 8
// (320 và 240 đều thoả). Xoay được thực hiện bên trong lúc decode nên KHÔNG
// cần thêm buffer và KHÔNG đụng tới vùng vẽ của panel.
typedef enum {
    JPEG_DEC_ROTATE_0   = 0,
    JPEG_DEC_ROTATE_90  = 1,   // xoay 90° theo chiều kim đồng hồ
    JPEG_DEC_ROTATE_180 = 2,   // xoay 180° theo chiều kim đồng hồ
    JPEG_DEC_ROTATE_270 = 3,   // xoay 270° theo chiều kim đồng hồ
} jpeg_dec_rotate_t;

esp_err_t jpeg_dec_get_info(const uint8_t *data, size_t len, uint16_t *w, uint16_t *h);

// Cấp/thu hồi buffer pixel cho đúng 1 frame: width * height * 2 byte, căn 16 byte
// theo yêu cầu của thư viện giải mã. Trả NULL nếu hết RAM.
// Phải giải phóng bằng jpeg_dec_pixbuf_free().
uint8_t *jpeg_dec_pixbuf_alloc(uint16_t width, uint16_t height);
void jpeg_dec_pixbuf_free(uint8_t *buf);

// out_size phải >= width * height * 2 của ảnh JPEG thật (lấy bằng jpeg_dec_get_info).
// rotate khác 0 thì ảnh được xoay trong lúc decode; vẫn dùng đúng out_size như trên
// (xoay không làm thay đổi tổng số byte).
esp_err_t jpeg_dec_buffer(const uint8_t *data, size_t len, bool swap_bytes, jpeg_dec_rotate_t rotate,
                          uint8_t *out, size_t out_size, uint16_t *w, uint16_t *h);

esp_err_t jpeg_dec_file(const char *path, bool swap_bytes, jpeg_image_t *out);

void jpeg_image_free(jpeg_image_t *img);

#ifdef __cplusplus
}
#endif
