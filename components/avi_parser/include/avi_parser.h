#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    double   fps;
    uint32_t total_frames;     // 0 nếu header không ghi

    bool     has_audio;
    uint16_t audio_format_tag; // 1 = PCM
    uint16_t audio_channels;
    uint32_t audio_sample_rate;
    uint16_t audio_bits;
} avi_info_t;

typedef enum {
    AVI_CHUNK_VIDEO,
    AVI_CHUNK_AUDIO,
    AVI_CHUNK_EOF,
} avi_chunk_type_t;

typedef struct avi_parser_s *avi_parser_handle_t;

// Mở file, parse header. Sau khi thành công, con trỏ đọc nằm ngay đầu khối movi,
// sẵn sàng cho avi_parser_read_chunk().
esp_err_t avi_parser_open(const char *path, avi_parser_handle_t *out_handle, avi_info_t *info);

// Đọc chunk video/audio kế tiếp vào buf (do caller cấp). Tự động bỏ qua các
// chunk khác (index, text...) nên *type chỉ trả về VIDEO/AUDIO/EOF.
// Nếu chunk lớn hơn buf_size: trả ESP_ERR_NO_MEM, *type vẫn đúng loại,
// dữ liệu chunk đó bị bỏ qua (mất 1 frame) — nên cấp buf đủ lớn (32-64KB).
esp_err_t avi_parser_read_chunk(avi_parser_handle_t h, avi_chunk_type_t *type,
                                uint8_t *buf, size_t buf_size, size_t *out_len);

// Quay lại đầu khối movi để phát lại từ đầu (loop video).
esp_err_t avi_parser_rewind(avi_parser_handle_t h);

void avi_parser_close(avi_parser_handle_t h);

#ifdef __cplusplus
}
#endif