#include "bsp_fs.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "bsp_fs";

bool bsp_fs_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool bsp_fs_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

esp_err_t bsp_fs_file_size(const char *path, size_t *out_size)
{
    struct stat st;
    if (!path || !out_size) return ESP_ERR_INVALID_ARG;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    *out_size = st.st_size;
    return ESP_OK;
}

static esp_err_t write_file(const char *path, const void *data, size_t len, const char *mode)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, mode);
    if (!f) {
        ESP_LOGE(TAG, "open %s failed: errno %d", path, errno);
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    if (len && fwrite(data, 1, len, f) != len) ret = ESP_FAIL;
    if (fclose(f) != 0) ret = ESP_FAIL;
    return ret;
}

esp_err_t bsp_fs_create_file(const char *path, const void *data, size_t len)
{
    return write_file(path, data, len, "wb");
}

esp_err_t bsp_fs_append_file(const char *path, const void *data, size_t len)
{
    return write_file(path, data, len, "ab");
}

esp_err_t bsp_fs_delete_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (unlink(path) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_fs_create_dir(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (mkdir(path, 0775) == 0) return ESP_OK;
    if (errno == EEXIST && bsp_fs_is_dir(path)) return ESP_OK;
    ESP_LOGE(TAG, "mkdir %s failed: errno %d", path, errno);
    return ESP_FAIL;
}

esp_err_t bsp_fs_delete_dir(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (rmdir(path) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_fs_delete_recursive(const char *path)
{
    if (!strcmp(path, "/") || !strcmp(path, "/sdcard") || !strcmp(path, "/sdcard/")) {
    ESP_LOGE(TAG, "refuse to delete mount root");
    return ESP_ERR_INVALID_ARG;
}
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!bsp_fs_is_dir(path)) return bsp_fs_delete_file(path);

    DIR *d = opendir(path);
    if (!d) return ESP_FAIL;

    // Cấp phát path trên heap, tránh tràn stack khi đệ quy sâu
    size_t plen = strlen(path);
    char *child = malloc(plen + 1 + 256 + 1);
    if (!child) { closedir(d); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, plen + 1 + 256 + 1, "%s/%s", path, e->d_name);
        ret = bsp_fs_delete_recursive(child);
        if (ret != ESP_OK) break;
    }
    free(child);
    closedir(d);

    return ret == ESP_OK ? bsp_fs_delete_dir(path) : ret;
}

esp_err_t bsp_fs_list_dir(const char *path, bsp_fs_list_cb_t cb, void *ctx)
{
    if (!path || !cb) return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(path);
    if (!d) return ESP_ERR_NOT_FOUND;

    size_t plen = strlen(path);
    char *full = malloc(plen + 1 + 256 + 1);
    if (!full) { closedir(d); return ESP_ERR_NO_MEM; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, plen + 1 + 256 + 1, "%s/%s", path, e->d_name);
        struct stat st;
        bool is_dir = false;
        size_t size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = st.st_size;
        }
        if (!cb(e->d_name, is_dir, size, ctx)) break;
    }
    free(full);
    closedir(d);
    return ESP_OK;
}

esp_err_t bsp_fs_create_dir_recursive(const char *path)
{
    if (!path || path[0] != '/') return ESP_ERR_INVALID_ARG;

    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, path, len + 1);

    // Bỏ dấu '/' thừa ở cuối
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    esp_err_t ret = ESP_OK;
    for (size_t i = 1; i <= len && ret == ESP_OK; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0') continue;
        if (tmp[i - 1] == '/') continue;              // bỏ qua "//"

        char saved = tmp[i];
        tmp[i] = '\0';
        if (!bsp_fs_is_dir(tmp)) {                    // đã có thì thôi (kể cả "/sdcard")
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed: errno %d", tmp, errno);
                ret = ESP_FAIL;
            }
        }
        tmp[i] = saved;
    }

    free(tmp);
    return ret;
}

esp_err_t bsp_fs_create_file_p(const char *path, const void *data, size_t len)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        size_t dlen = slash - path;
        char *dir = malloc(dlen + 1);
        if (!dir) return ESP_ERR_NO_MEM;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        esp_err_t ret = bsp_fs_create_dir_recursive(dir);
        free(dir);
        if (ret != ESP_OK) return ret;
    }
    return bsp_fs_create_file(path, data, len);
}

FILE *bsp_fs_fopen_buffered(const char *path, const char *mode, void *vbuf, size_t vbuf_size)
{
    FILE *f = fopen(path, mode);
    if (f && vbuf && vbuf_size) {
        setvbuf(f, (char *)vbuf, _IOFBF, vbuf_size);
    }
    return f;
}

esp_err_t bsp_fs_read_file_psram(const char *path, uint8_t **out, size_t *out_len)
{

    if (!path || !out || !out_len) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
     static uint8_t stream_buf[16 * 1024];
    setvbuf(f, (char *)stream_buf, _IOFBF, sizeof(stream_buf));

    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }

    // Large reads amortize FATFS and SDMMC call overhead; keep the staging buffer DMA-capable.
    const size_t CH = 64 * 1024;
    uint8_t *tmp = heap_caps_malloc(CH, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *dst = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!tmp || !dst) {
        heap_caps_free(tmp); heap_caps_free(dst); fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t done = 0;
    while (done < (size_t)sz) {
        size_t want = (size_t)sz - done;
        if (want > CH) want = CH;
        size_t n = fread(tmp, 1, want, f);
        if (n != want) break;
        memcpy(dst + done, tmp, want);
        done += want;
    }
    heap_caps_free(tmp);
    bool read_error = ferror(f);
    fclose(f);

    if (done != (size_t)sz || read_error) { heap_caps_free(dst); return ESP_FAIL; }
    *out = dst;
    *out_len = done;
    return ESP_OK;
}
static bool has_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + nlen - elen, ext) == 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

typedef struct {
    char (*names)[256];
    int max, count;
    const char *ext;
} list_ctx_t;

static bool collect_cb(const char *name, bool is_dir, size_t size, void *ctx)
{
    (void)size;
    list_ctx_t *c = (list_ctx_t *)ctx;
    if (is_dir) return true;
    if (!has_ext(name, c->ext)) return true;
    if (c->count >= c->max) return false;   // đủ rồi thì dừng duyệt
    strncpy(c->names[c->count], name, 255);
    c->names[c->count][255] = '\0';
    c->count++;
    return true;
}

int bsp_fs_list_by_ext(const char *dir, const char *ext, char names[][256], int max_count)
{
    if (!dir || !ext || !names || max_count <= 0) return 0;

    list_ctx_t ctx = { .names = names, .max = max_count, .count = 0, .ext = ext };
    bsp_fs_list_dir(dir, collect_cb, &ctx);

    qsort(names, ctx.count, 256, cmp_str);
    return ctx.count;
}