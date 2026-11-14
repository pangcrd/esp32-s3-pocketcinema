#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback duyệt thư mục. Trả về false để dừng sớm.
typedef bool (*bsp_fs_list_cb_t)(const char *name, bool is_dir, size_t size, void *ctx);

bool      bsp_fs_exists(const char *path);
bool      bsp_fs_is_dir(const char *path);
esp_err_t bsp_fs_file_size(const char *path, size_t *out_size);

esp_err_t bsp_fs_create_file(const char *path, const void *data, size_t len); 
esp_err_t bsp_fs_append_file(const char *path, const void *data, size_t len);
esp_err_t bsp_fs_delete_file(const char *path);

esp_err_t bsp_fs_create_dir(const char *path);  
esp_err_t bsp_fs_delete_dir(const char *path);   
esp_err_t bsp_fs_delete_recursive(const char *path);

esp_err_t bsp_fs_create_dir_recursive(const char *path);          
esp_err_t bsp_fs_create_file_p(const char *path, const void *data, size_t len); 

esp_err_t bsp_fs_list_dir(const char *path, bsp_fs_list_cb_t cb, void *ctx);

esp_err_t bsp_fs_read_file_psram(const char *path, uint8_t **out, size_t *out_len);
FILE *bsp_fs_fopen_buffered(const char *path, const char *mode, void *vbuf, size_t vbuf_size);

int bsp_fs_list_by_ext(const char *dir, const char *ext, char names[][256], int max_count);

#ifdef __cplusplus
}
#endif