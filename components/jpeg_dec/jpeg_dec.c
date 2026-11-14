#include "jpeg_dec.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"

static const char *TAG = "jpeg_dec";

esp_err_t jpeg_dec_get_info(const uint8_t *data, size_t len, uint16_t *w, uint16_t *h)
{
    if (!data || !len || !w || !h) return ESP_ERR_INVALID_ARG;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t handle = NULL;
    if (jpeg_dec_open(&config, &handle) != JPEG_ERR_OK) return ESP_FAIL;

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)data, .inbuf_len = (int)len };
    jpeg_dec_header_info_t info = {0};

    jpeg_error_t ret = jpeg_dec_parse_header(handle, &io, &info);
    jpeg_dec_close(handle);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "parse header failed: %d (progressive JPEG?)", ret);
        return ESP_FAIL;
    }

    *w = info.width;
    *h = info.height;
    return ESP_OK;
}

uint8_t *jpeg_dec_pixbuf_alloc(uint16_t width, uint16_t height)
{
    if (!width || !height) return NULL;
    // Thư viện yêu cầu buffer output căn 16 byte -> phải dùng jpeg_calloc_align,
    // không được dùng heap_caps_malloc (địa chỉ trả về chỉ căn 4/8 byte).
    return (uint8_t *)jpeg_calloc_align((size_t)width * height * 2, 16);
}

void jpeg_dec_pixbuf_free(uint8_t *buf)
{
    if (buf) jpeg_free_align(buf);   // phải đi kèm jpeg_calloc_align
}

// Decode với handle đã mở, dùng nội bộ bởi jpeg_dec_buffer và jpeg_dec_file
static esp_err_t decode_with_handle(jpeg_dec_handle_t handle, const uint8_t *data, size_t len,
                                    uint8_t *out, size_t out_size, uint16_t *w, uint16_t *h)
{
    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)data,
        .inbuf_len = (int)len,
        .outbuf = out,
    };
    jpeg_dec_header_info_t info = {0};

    jpeg_error_t ret = jpeg_dec_parse_header(handle, &io, &info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "parse header failed: %d", ret);
        return ESP_FAIL;
    }

    int need = 0;
    ret = jpeg_dec_get_outbuf_len(handle, &need);
    if (ret != JPEG_ERR_OK || (size_t)need > out_size) {
        ESP_LOGE(TAG, "outbuf too small: need=%d have=%u", need, (unsigned)out_size);
        return ESP_ERR_NO_MEM;
    }

    ret = jpeg_dec_process(handle, &io);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "decode failed: %d", ret);
        return ESP_FAIL;
    }

    if (w) *w = info.width;
    if (h) *h = info.height;
    return ESP_OK;
}

esp_err_t jpeg_dec_buffer(const uint8_t *data, size_t len, bool swap_bytes, jpeg_dec_rotate_t rotate,
                          uint8_t *out, size_t out_size, uint16_t *w, uint16_t *h)
{
    if (!data || !len || !out) return ESP_ERR_INVALID_ARG;
    if (rotate < JPEG_DEC_ROTATE_0 || rotate > JPEG_DEC_ROTATE_270) return ESP_ERR_INVALID_ARG;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = swap_bytes ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
    // Xoay nằm trong đường decode: image -> decoding -> rotation. Không tốn thêm
    // buffer, không đổi tổng số byte output, và KHÔNG liên quan tới vùng vẽ của panel.
    // Thư viện chỉ hỗ trợ khi width và height của JPEG đều là bội số của 8, nếu
    // không jpeg_dec_process() sẽ trả lỗi (decode_with_handle log ra ngay).
    config.rotate = (jpeg_rotate_t)rotate;

    jpeg_dec_handle_t handle = NULL;
    if (jpeg_dec_open(&config, &handle) != JPEG_ERR_OK) return ESP_FAIL;

    esp_err_t ret = decode_with_handle(handle, data, len, out, out_size, w, h);
    jpeg_dec_close(handle);
    return ret;
}

esp_err_t jpeg_dec_file(const char *path, bool swap_bytes, jpeg_image_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return ESP_FAIL; }
    static uint8_t stream_buf[16 * 1024];
    setvbuf(f, (char *)stream_buf, _IOFBF, sizeof(stream_buf));

    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }

    uint8_t *in = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!in) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(in, 1, sz, f);
    fclose(f);
    if (n != (size_t)sz) { heap_caps_free(in); return ESP_FAIL; }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = swap_bytes ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_dec_handle_t handle = NULL;
    if (jpeg_dec_open(&config, &handle) != JPEG_ERR_OK) {
        heap_caps_free(in);
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = { .inbuf = in, .inbuf_len = (int)sz };
    jpeg_dec_header_info_t info = {0};

    jpeg_error_t ret = jpeg_dec_parse_header(handle, &io, &info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "parse header failed: %d (progressive JPEG?)", ret);
        jpeg_dec_close(handle);
        heap_caps_free(in);
        return ESP_FAIL;
    }

    int outbuf_len = 0;
    jpeg_dec_get_outbuf_len(handle, &outbuf_len);

    // Yêu cầu của thư viện: buffer output phải căn 16 byte, cấp bằng jpeg_calloc_align
    uint8_t *pix = jpeg_calloc_align(outbuf_len, 16);
    if (!pix) {
        jpeg_dec_close(handle);
        heap_caps_free(in);
        return ESP_ERR_NO_MEM;
    }

    io.outbuf = pix;
    ret = jpeg_dec_process(handle, &io);
    jpeg_dec_close(handle);
    heap_caps_free(in);

    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "decode failed: %d", ret);
        jpeg_free_align(pix);
        return ESP_FAIL;
    }

    out->pixels = (uint16_t *)pix;
    out->width = info.width;
    out->height = info.height;
    return ESP_OK;
}

