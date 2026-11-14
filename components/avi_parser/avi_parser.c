#include "avi_parser.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"

static const char *TAG = "avi_parser";
#define MAX_STREAMS 4

typedef enum { STREAM_UNKNOWN, STREAM_VIDEO, STREAM_AUDIO } stream_kind_t;

struct avi_parser_s {
    FILE *f;
    uint8_t *vbuf;              // stdio stream buffer (setvbuf)
    long movi_data_start;       // vị trí để rewind
    stream_kind_t kind[MAX_STREAMS];
    int stream_count;
};

static bool read_exact(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n;
}

static bool read_fourcc(FILE *f, char out[4])
{
    return read_exact(f, out, 4);
}

static bool read_u32(FILE *f, uint32_t *out)
{
    return read_exact(f, out, 4);   // ESP32 little-endian, khớp luôn với AVI, không cần đổi byte
}

static bool skip_bytes(FILE *f, long n)
{
    if (n <= 0) return true;
    return fseek(f, n, SEEK_CUR) == 0;
}

// Đọc header một stream (strl: strh + strf [+ các chunk khác bỏ qua]).
// list_size đã trừ sẵn 4 byte của fourcc "strl".
static esp_err_t parse_strl(struct avi_parser_s *p, uint32_t list_size, avi_info_t *info)
{
    long consumed = 0;
    stream_kind_t kind = STREAM_UNKNOWN;

    while (consumed < (long)list_size) {
        char id[4];
        uint32_t size;
        if (!read_fourcc(p->f, id) || !read_u32(p->f, &size)) return ESP_FAIL;
        consumed += 8;

        if (!memcmp(id, "strh", 4)) {
            char type[4];
            if (!read_fourcc(p->f, type)) return ESP_FAIL;
            if (!memcmp(type, "vids", 4)) kind = STREAM_VIDEO;
            else if (!memcmp(type, "auds", 4)) kind = STREAM_AUDIO;
            // Đã đọc 4 byte type, còn size-4 byte phía sau của strh, bỏ qua (không cần fcc_handler, scale/rate...)
            if (!skip_bytes(p->f, (long)size - 4)) return ESP_FAIL;
        } else if (!memcmp(id, "strf", 4)) {
            if (kind == STREAM_AUDIO && size >= 16) {
                uint16_t fmt_tag, channels, block_align, bits;
                uint32_t sample_rate, avg_bytes;
                if (!read_exact(p->f, &fmt_tag, 2) || !read_exact(p->f, &channels, 2) ||
                    !read_exact(p->f, &sample_rate, 4) || !read_exact(p->f, &avg_bytes, 4) ||
                    !read_exact(p->f, &block_align, 2) || !read_exact(p->f, &bits, 2)) {
                    return ESP_FAIL;
                }
                info->audio_format_tag = fmt_tag;
                info->audio_channels = channels;
                info->audio_sample_rate = sample_rate;
                info->audio_bits = bits;
                info->has_audio = true;
                if (!skip_bytes(p->f, (long)size - 16)) return ESP_FAIL;
            } else {
                // strf của video (BITMAPINFOHEADER) không cần, width/height đã có từ avih
                if (!skip_bytes(p->f, (long)size)) return ESP_FAIL;
            }
        } else {
            if (!skip_bytes(p->f, (long)size)) return ESP_FAIL;
        }
        if (size & 1) { fgetc(p->f); consumed += 1; }  // byte đệm căn chẵn
        consumed += size;
    }

    if (p->stream_count < MAX_STREAMS) {
        p->kind[p->stream_count++] = kind;
    }
    return ESP_OK;
}

esp_err_t avi_parser_open(const char *path, avi_parser_handle_t *out_handle, avi_info_t *info)
{
    if (!path || !out_handle || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    struct avi_parser_s *p = calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;

    p->f = fopen(path, "rb");
    if (!p->f) {
        /* Log tại đây: trước kia nhánh này trả ESP_FAIL im lặng, khiến lỗi
         * "không mở được file" trông giống hệt lỗi parse AVI hỏng. */
        ESP_LOGE(TAG, "cannot open '%s' (errno %d)", path, errno);
        free(p);
        return ESP_FAIL;
    }

    p->vbuf = malloc(16 * 1024);
    if (p->vbuf) setvbuf(p->f, (char *)p->vbuf, _IOFBF, 16 * 1024);

    char id[4];
    uint32_t riff_size;
    char form[4];
    if (!read_fourcc(p->f, id) || memcmp(id, "RIFF", 4) ||
        !read_u32(p->f, &riff_size) ||
        !read_fourcc(p->f, form) || memcmp(form, "AVI ", 4)) {
        ESP_LOGE(TAG, "not a valid AVI file");
        goto fail;
    }

    while (1) {
        char cid[4];
        uint32_t csize;
        if (!read_fourcc(p->f, cid) || !read_u32(p->f, &csize)) {
            ESP_LOGE(TAG, "unexpected EOF before movi list");
            goto fail;
        }

        if (!memcmp(cid, "LIST", 4)) {
            char ltype[4];
            if (!read_fourcc(p->f, ltype)) {
                ESP_LOGE(TAG, "truncated file: no LIST type after LIST header");
                goto fail;
            }

            if (!memcmp(ltype, "movi", 4)) {
                p->movi_data_start = ftell(p->f);
                break;   // xong phần header, con trỏ đang ở đầu movi
            } else if (!memcmp(ltype, "hdrl", 4)) {
                long remain = (long)csize - 4;
                while (remain > 0) {
                    char hid[4];
                    uint32_t hsize;
                    if (!read_fourcc(p->f, hid) || !read_u32(p->f, &hsize)) goto fail;
                    remain -= 8;

                    if (!memcmp(hid, "avih", 4)) {
                        uint32_t microsec_per_frame, max_bps, pad_gran, flags;
                        uint32_t total_frames, initial_frames, streams, sugg_buf;
                        uint32_t width, height;
                        if (!read_exact(p->f, &microsec_per_frame, 4) ||
                            !read_exact(p->f, &max_bps, 4) ||
                            !read_exact(p->f, &pad_gran, 4) ||
                            !read_exact(p->f, &flags, 4) ||
                            !read_exact(p->f, &total_frames, 4) ||
                            !read_exact(p->f, &initial_frames, 4) ||
                            !read_exact(p->f, &streams, 4) ||
                            !read_exact(p->f, &sugg_buf, 4) ||
                            !read_exact(p->f, &width, 4) ||
                            !read_exact(p->f, &height, 4)) goto fail;
                        info->width = (uint16_t)width;
                        info->height = (uint16_t)height;
                        info->fps = microsec_per_frame ? 1e6 / microsec_per_frame : 0;
                        info->total_frames = total_frames;
                        if (!skip_bytes(p->f, (long)hsize - 40)) goto fail;  // 10 field đã đọc x4 = 40
                    } else if (!memcmp(hid, "LIST", 4)) {
                        char sltype[4];
                        if (!read_fourcc(p->f, sltype)) goto fail;
                        if (!memcmp(sltype, "strl", 4)) {
                            if (parse_strl(p, hsize - 4, info) != ESP_OK) goto fail;
                        } else {
                            if (!skip_bytes(p->f, (long)hsize - 4)) goto fail;
                        }
                    } else {
                        if (!skip_bytes(p->f, (long)hsize)) goto fail;
                    }
                    if (hsize & 1) { fgetc(p->f); remain -= 1; }
                    remain -= hsize;
                }
            } else {
                if (!skip_bytes(p->f, (long)csize - 4)) goto fail;
            }
        } else {
            // chunk lạ ở top-level trước movi (hiếm), bỏ qua
            if (!skip_bytes(p->f, (long)csize)) goto fail;
            if (csize & 1) fgetc(p->f);
        }
    }

    if (info->width == 0 || info->height == 0) {
        ESP_LOGE(TAG, "missing avih header, width/height unknown");
        goto fail;
    }

    ESP_LOGI(TAG, "AVI: %ux%u @ %.2f fps, audio=%d (%u Hz, %u ch, %u bit)",
             info->width, info->height, info->fps, info->has_audio,
             info->audio_sample_rate, info->audio_channels, info->audio_bits);

    *out_handle = p;
    return ESP_OK;

fail:
    if (p->f) fclose(p->f);
    free(p->vbuf);
    free(p);
    return ESP_FAIL;
}

esp_err_t avi_parser_read_chunk(avi_parser_handle_t h, avi_chunk_type_t *type,
                                uint8_t *buf, size_t buf_size, size_t *out_len)
{
    if (!h || !type || !buf || !out_len) return ESP_ERR_INVALID_ARG;

    while (1) {
        char id[4];
        uint32_t size;
        if (!read_fourcc(h->f, id) || !read_u32(h->f, &size)) {
            *type = AVI_CHUNK_EOF;
            return ESP_OK;
        }

        if (!memcmp(id, "idx1", 4)) {
            *type = AVI_CHUNK_EOF;   // hết phần dữ liệu movi
            return ESP_OK;
        }

        if (!memcmp(id, "LIST", 4)) {
            // Chunk kiểu "rec " bọc nhóm frame (nếu có) — bỏ qua header LIST,
            // không skip phần thân, để các chunk con bên trong được đọc tuần tự bình thường.
            char ltype[4];
            if (!read_fourcc(h->f, ltype)) { *type = AVI_CHUNK_EOF; return ESP_OK; }
            continue;
        }

        if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') {
            // chunk không đúng định dạng "NNxx", bỏ qua an toàn
            if (!skip_bytes(h->f, (long)size)) { *type = AVI_CHUNK_EOF; return ESP_OK; }
            if (size & 1) fgetc(h->f);
            continue;
        }

        int stream_idx = (id[0] - '0') * 10 + (id[1] - '0');
        stream_kind_t kind = (stream_idx < MAX_STREAMS) ? h->kind[stream_idx] : STREAM_UNKNOWN;

        bool is_video = (!memcmp(id + 2, "dc", 2) || !memcmp(id + 2, "db", 2)) && kind == STREAM_VIDEO;
        bool is_audio = !memcmp(id + 2, "wb", 2) && kind == STREAM_AUDIO;

        if (!is_video && !is_audio) {
            if (!skip_bytes(h->f, (long)size)) { *type = AVI_CHUNK_EOF; return ESP_OK; }
            if (size & 1) fgetc(h->f);
            continue;
        }

        if (size > buf_size) {
            ESP_LOGW(TAG, "chunk %.4s size %u exceeds buffer %u, dropped", id, size, (unsigned)buf_size);
            skip_bytes(h->f, (long)size);
            if (size & 1) fgetc(h->f);
            *type = is_video ? AVI_CHUNK_VIDEO : AVI_CHUNK_AUDIO;
            *out_len = 0;
            return ESP_ERR_NO_MEM;
        }

        if (!read_exact(h->f, buf, size)) { *type = AVI_CHUNK_EOF; return ESP_OK; }
        if (size & 1) fgetc(h->f);

        *type = is_video ? AVI_CHUNK_VIDEO : AVI_CHUNK_AUDIO;
        *out_len = size;
        return ESP_OK;
    }
}

esp_err_t avi_parser_rewind(avi_parser_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    return fseek(h->f, h->movi_data_start, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
}

void avi_parser_close(avi_parser_handle_t h)
{
    if (!h) return;
    if (h->f) fclose(h->f);
    free(h->vbuf);
    free(h);
}