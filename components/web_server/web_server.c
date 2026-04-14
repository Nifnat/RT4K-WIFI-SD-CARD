#include "web_server.h"
#include "sd_control.h"
#include "network.h"
#include "rt4k_config.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"

static const char *TAG = "webserver";
static httpd_handle_t s_server = NULL;

/* Buffer size for streaming file data */
#define FILE_BUF_SIZE 4096
#define SMALL_POST_BODY_MAX 1024
#define MODELINE_POST_BODY_MAX (12 * 1024)

#define LOG_STREAM_SIZE 2048

static StreamBufferHandle_t s_log_stream = NULL;
static volatile int s_ws_log_fd = -1;   /* -1 = no client */
static vprintf_like_t s_orig_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Fast path: no client, zero overhead */
    if (s_ws_log_fd < 0) {
        return s_orig_vprintf(fmt, args);
    }

    char tmp[256];
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, copy);
    va_end(copy);

    if (len > 0 && s_log_stream) {
        size_t n = (len < (int)sizeof(tmp)) ? (size_t)len : sizeof(tmp) - 1;
        /* Non-blocking: silently drops if buffer full — fine for debug */
        xStreamBufferSend(s_log_stream, tmp, n, 0);
    }

    return s_orig_vprintf(fmt, args);
}
#define MODELINE_CONTENT_MAX   (8 * 1024)

/* SPIFFS mount point for static web assets */
#define SPIFFS_MOUNT "/spiffs"

/* ─── Helpers ─────────────────────────────────────────────────────── */

static const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcasecmp(ext, ".htm") == 0 || strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, ".xml") == 0) return "text/xml";
    if (strcasecmp(ext, ".pdf") == 0) return "application/x-pdf";
    if (strcasecmp(ext, ".zip") == 0) return "application/x-zip";
    if (strcasecmp(ext, ".gz") == 0) return "application/x-gzip";
    return "text/plain";
}

/* Add CORS / Private-Network-Access headers required by Chrome PNA policy */
static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

/* Extract a query parameter value. Returns length written or -1 if not found. */
static int get_query_param(httpd_req_t *req, const char *key, char *val, size_t val_len)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return -1;

    char *query = malloc(qlen + 1);
    if (!query) return -1;

    if (httpd_req_get_url_query_str(req, query, qlen + 1) != ESP_OK) {
        free(query);
        return -1;
    }

    esp_err_t err = httpd_query_key_value(query, key, val, val_len);
    free(query);
    return (err == ESP_OK) ? (int)strlen(val) : -1;
}

/* URL-decode a string in-place */
static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Read full POST body into a buffer. Caller must free. Returns length or -1. */
static int read_post_body(httpd_req_t *req, char **out_buf, int max_len)
{
    int total = req->content_len;
    if (total <= 0 || total > max_len) return -1;

    char *buf = malloc(total + 1);
    if (!buf) return -1;

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            return -1;
        }
        received += ret;
    }
    buf[total] = '\0';
    *out_buf = buf;
    return total;
}

/* Parse a URL-encoded form body for a given key. Returns length or -1. */
static int parse_form_field(const char *body, const char *key, char *val, size_t val_len)
{
    /* body is url-encoded: key1=val1&key2=val2 */
    const char *p = body;
    size_t key_len = strlen(key);

    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *start = p + key_len + 1;
            const char *end = strchr(start, '&');
            size_t len = end ? (size_t)(end - start) : strlen(start);
            if (len >= val_len) len = val_len - 1;
            memcpy(val, start, len);
            val[len] = '\0';
            url_decode(val);
            return (int)len;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

/* ─── /relinquish ─────────────────────────────────────────────────── */

static esp_err_t handle_relinquish(httpd_req_t *req)
{
    set_cors_headers(req);
    sd_control_relinquish();
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ─── /list ───────────────────────────────────────────────────────── */

static esp_err_t handle_list(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "LIST:SDBUSY");
        return ESP_OK;
    }

    char dir_path[256];
    if (get_query_param(req, "dir", dir_path, sizeof(dir_path)) < 0 &&
        get_query_param(req, "path", dir_path, sizeof(dir_path)) < 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "LIST:BADARGS");
        return ESP_OK;
    }
    url_decode(dir_path);

    /* Build full VFS path */
    char full_path[300];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, dir_path);

    sd_control_take();

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "LIST:BADPATH");
        sd_control_relinquish();
        return ESP_OK;
    }

    if (!S_ISDIR(st.st_mode)) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "LIST:NOTDIR");
        sd_control_relinquish();
        return ESP_OK;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "LIST:NOTDIR");
        sd_control_relinquish();
        return ESP_OK;
    }

    /* Stream JSON via chunked response to avoid building a huge String */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);

    struct dirent *entry;
    int cnt = 0;
    char chunk[256];

    while ((entry = readdir(dir)) != NULL) {
        /* Build entry path to stat for size and type */
        char entry_path[600];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);

        struct stat entry_st;
        bool is_dir = (entry->d_type == DT_DIR);
        long file_size = 0;

        if (stat(entry_path, &entry_st) == 0) {
            is_dir = S_ISDIR(entry_st.st_mode);
            file_size = is_dir ? 0 : (long)entry_st.st_size;
        }

        int len = snprintf(chunk, sizeof(chunk),
            "%s{\"type\":\"%s\",\"name\":\"%s\",\"size\":\"%ld\"}",
            cnt > 0 ? "," : "",
            is_dir ? "dir" : "file",
            entry->d_name,
            file_size);

        httpd_resp_send_chunk(req, chunk, len);
        cnt++;
    }

    closedir(dir);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0); /* Finalize chunked response */

    sd_control_relinquish();
    return ESP_OK;
}

/* ─── /download ───────────────────────────────────────────────────── */

static esp_err_t handle_download(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DOWNLOAD:SDBUSY");
        return ESP_OK;
    }

    char file_path[256];
    if (get_query_param(req, "dir", file_path, sizeof(file_path)) < 0 &&
        get_query_param(req, "path", file_path, sizeof(file_path)) < 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DOWNLOAD:BADARGS");
        return ESP_OK;
    }
    url_decode(file_path);

    char full_path[300];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, file_path);

    sd_control_take();

    struct stat file_stat;
    if (stat(full_path, &file_stat) != 0) {
        httpd_resp_set_status(req, "404");
        httpd_resp_sendstr(req, "DOWNLOAD:FileNotFound");
        sd_control_relinquish();
        return ESP_OK;
    }

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_set_status(req, "404");
        httpd_resp_sendstr(req, "DOWNLOAD:FileNotFound");
        sd_control_relinquish();
        return ESP_OK;
    }

    httpd_resp_set_type(req, get_content_type(file_path));
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%ld", (long)file_stat.st_size);
    httpd_resp_set_hdr(req, "Content-Length", len_str);

    /* Extract filename for Content-Disposition */
    const char *fname = strrchr(file_path, '/');
    fname = fname ? fname + 1 : file_path;
    char safe_fname[256];
    snprintf(safe_fname, sizeof(safe_fname), "%s", fname);
    /* Sanitize: strip characters that break the header */
    for (char *p = safe_fname; *p; p++) {
        if (*p == '"' || *p == '\r' || *p == '\n') *p = '_';
    }
    char disp[300];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", safe_fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* Stream file in chunks */
    char *buf = malloc(FILE_BUF_SIZE);
    if (!buf) {
        fclose(f);
        sd_control_relinquish();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, FILE_BUF_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            ESP_LOGW(TAG, "Download: send chunk failed");
            break;
        }
    }

    httpd_resp_send_chunk(req, NULL, 0); /* Finalize */
    free(buf);
    fclose(f);
    sd_control_relinquish();
    return ESP_OK;
}

/* ─── /delete ─────────────────────────────────────────────────────── */

static esp_err_t handle_delete(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DELETE:SDBUSY");
        return ESP_OK;
    }

    char path_param[256];
    if (get_query_param(req, "path", path_param, sizeof(path_param)) < 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DELETE:BADARGS");
        return ESP_OK;
    }
    url_decode(path_param);

    char full_path[300];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_MOUNT_POINT, path_param);

    sd_control_take();

    struct stat st;
    if (stat(full_path, &st) != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DELETE:BADPATH");
        sd_control_relinquish();
        return ESP_OK;
    }

    if (!S_ISDIR(st.st_mode)) {
        unlink(full_path);
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "DELETE:ISDIR");
    }

    sd_control_relinquish();
    return ESP_OK;
}

/* ─── /rename ─────────────────────────────────────────────────────── */

static esp_err_t handle_rename(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:SDBUSY");
        return ESP_OK;
    }

    char *body = NULL;
    if (read_post_body(req, &body, SMALL_POST_BODY_MAX) < 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:BADARGS");
        return ESP_OK;
    }

    char old_path[256] = "";
    char new_name[256] = "";
    parse_form_field(body, "oldPath", old_path, sizeof(old_path));
    parse_form_field(body, "newName", new_name, sizeof(new_name));
    free(body);

    if (old_path[0] == '\0' || new_name[0] == '\0') {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:BADARGS");
        return ESP_OK;
    }

    /* Ensure leading slash */
    char norm_old[300];
    if (old_path[0] != '/') {
        snprintf(norm_old, sizeof(norm_old), "%s/%s", SD_MOUNT_POINT, old_path);
    } else {
        snprintf(norm_old, sizeof(norm_old), "%s%s", SD_MOUNT_POINT, old_path);
    }

    /* Build new path from old path's directory + new name */
    char norm_new[300];
    const char *last_slash = strrchr(norm_old, '/');
    if (last_slash) {
        size_t dir_len = last_slash - norm_old + 1;
        memcpy(norm_new, norm_old, dir_len);
        strncpy(norm_new + dir_len, new_name, sizeof(norm_new) - dir_len - 1);
        norm_new[sizeof(norm_new) - 1] = '\0';
    } else {
        snprintf(norm_new, sizeof(norm_new), "%s/%s", SD_MOUNT_POINT, new_name);
    }

    ESP_LOGI(TAG, "Rename: %s -> %s", norm_old, norm_new);

    sd_control_take();

    struct stat st;
    if (stat(norm_old, &st) != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:SOURCEMISSING");
        sd_control_relinquish();
        return ESP_OK;
    }

    if (stat(norm_new, &st) == 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:DESTEXISTS");
        sd_control_relinquish();
        return ESP_OK;
    }

    if (rename(norm_old, norm_new) == 0) {
        httpd_resp_sendstr(req, "ok");
    } else {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "RENAME:FAILED");
    }

    sd_control_relinquish();
    return ESP_OK;
}

/* ─── Upload: /upload_begin … /upload_end ──────── */
/*
 * Two-phase upload protocol optimised to separate WiFi receive
 * from SD write:
 *
 *   Phase 1 – receive entire chunk body into a 32 KiB RAM buffer
 *             (WiFi active, SD idle).
 *   Phase 2 – write buffer to SD in a single fwrite()
 *             (SD active, WiFi idle / sleeping).
 *
 * The HTTP response is not sent until Phase 2 completes, so the
 * client naturally paces itself — no artificial inter-chunk delays
 * needed.
 */

#define UPLOAD_BUF_SIZE     (32 * 1024)           /* 32 KiB accumulation buffer */
#define UPLOAD_TIMEOUT_US   (30LL * 1000000)      /* 30 s stale-session guard   */

static FILE    *s_ul_fp        = NULL;
static char     s_ul_path[512] = "";
static int64_t  s_ul_bytes     = 0;
static int64_t  s_ul_write_us  = 0;
static int64_t  s_ul_recv_us   = 0;
static int64_t  s_ul_last_us   = 0;
static char    *s_ul_buf       = NULL;   /* 32 KiB buffer, allocated on begin */

static void upload_cleanup(void)
{
    if (s_ul_fp) { fclose(s_ul_fp); s_ul_fp = NULL; }
    if (s_ul_path[0]) {
        unlink(s_ul_path);
        sd_control_unhold();
        sd_control_relinquish();
        s_ul_path[0] = '\0';
    }
    if (s_ul_buf) { free(s_ul_buf); s_ul_buf = NULL; }
    s_ul_bytes = s_ul_write_us = s_ul_recv_us = s_ul_last_us = 0;
}

static void upload_check_timeout(void)
{
    if (s_ul_fp && s_ul_last_us > 0 &&
        (esp_timer_get_time() - s_ul_last_us) > UPLOAD_TIMEOUT_US) {
        ESP_LOGW(TAG, "Upload session timed out — cleaning up");
        upload_cleanup();
    }
}

/* POST /upload_begin?path=/dir&filename=foo.bin */
static esp_err_t handle_upload_begin(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    upload_check_timeout();
    if (s_ul_fp) {
        ESP_LOGW(TAG, "Aborting previous upload session");
        upload_cleanup();
    }

    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "503");
        httpd_resp_sendstr(req, "{\"error\":\"SDBUSY\"}");
        return ESP_OK;
    }

    /* Allocate 32 KiB accumulation buffer */
    s_ul_buf = heap_caps_malloc(UPLOAD_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!s_ul_buf) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"NOMEM\"}");
        return ESP_OK;
    }

    char upload_dir[256] = "/";
    get_query_param(req, "path", upload_dir, sizeof(upload_dir));
    url_decode(upload_dir);
    if (upload_dir[0] != '/') {
        memmove(upload_dir + 1, upload_dir, strlen(upload_dir) + 1);
        upload_dir[0] = '/';
    }

    char filename[128] = "";
    if (get_query_param(req, "filename", filename, sizeof(filename)) < 0) {
        free(s_ul_buf); s_ul_buf = NULL;
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "{\"error\":\"NOFILENAME\"}");
        return ESP_OK;
    }
    url_decode(filename);

    sd_control_take();
    sd_control_hold();

    if (!sd_control_we_have_control()) {
        ESP_LOGE(TAG, "Upload: SD mount failed");
        sd_control_unhold();
        free(s_ul_buf); s_ul_buf = NULL;
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"MOUNT\"}");
        return ESP_OK;
    }

    /* Ensure upload directory exists */
    if (strcmp(upload_dir, "/") != 0) {
        char dir_full[300];
        snprintf(dir_full, sizeof(dir_full), "%s%s", SD_MOUNT_POINT, upload_dir);
        size_t dlen = strlen(dir_full);
        if (dlen > 1 && dir_full[dlen - 1] == '/') dir_full[dlen - 1] = '\0';
        mkdir(dir_full, 0775);
    }

    /* Build full path */
    if (upload_dir[strlen(upload_dir) - 1] == '/')
        snprintf(s_ul_path, sizeof(s_ul_path), "%s%s%s",
                 SD_MOUNT_POINT, upload_dir, filename);
    else
        snprintf(s_ul_path, sizeof(s_ul_path), "%s%s/%s",
                 SD_MOUNT_POINT, upload_dir, filename);

    unlink(s_ul_path);
    s_ul_fp = fopen(s_ul_path, "wb");
    if (!s_ul_fp) {
        ESP_LOGE(TAG, "Upload: failed to open %s", s_ul_path);
        sd_control_unhold();
        sd_control_relinquish();
        s_ul_path[0] = '\0';
        free(s_ul_buf); s_ul_buf = NULL;
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"OPEN\"}");
        return ESP_OK;
    }

    /* Bypass stdio buffering — our 32 KiB writes go straight to VFS */
    setvbuf(s_ul_fp, NULL, _IONBF, 0);

    s_ul_bytes = s_ul_write_us = s_ul_recv_us = 0;
    s_ul_last_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Upload started: %s (32 KiB buffered writes)", s_ul_path);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /upload_chunk  (body = raw bytes, max 32 KiB) */
static esp_err_t handle_upload_chunk(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!s_ul_fp || !s_ul_buf) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "{\"error\":\"NOSESSION\"}");
        return ESP_OK;
    }

    s_ul_last_us = esp_timer_get_time();
    int64_t chunk_start_us = s_ul_last_us;
    int64_t deadline = s_ul_last_us + UPLOAD_TIMEOUT_US;

    int remaining = req->content_len;
    if (remaining > UPLOAD_BUF_SIZE) {
        httpd_resp_set_status(req, "413");
        httpd_resp_sendstr(req, "{\"error\":\"TOOLARGE\"}");
        return ESP_OK;
    }

    /* ── Phase 1: recv entire body into RAM (WiFi active, SD idle) ── */
    int64_t recv_start = esp_timer_get_time();
    int buf_offset = 0;

    while (remaining > 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "Upload: recv timeout");
            upload_cleanup();
            httpd_resp_set_status(req, "500");
            httpd_resp_sendstr(req, "{\"error\":\"TIMEOUT\"}");
            return ESP_OK;
        }

        int received = httpd_req_recv(req, s_ul_buf + buf_offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Upload: recv error");
            upload_cleanup();
            httpd_resp_set_status(req, "500");
            httpd_resp_sendstr(req, "{\"error\":\"RECV\"}");
            return ESP_OK;
        }
        buf_offset += received;
        remaining  -= received;
    }

    int64_t recv_us = esp_timer_get_time() - recv_start;
    s_ul_recv_us += recv_us;

    /* ── Phase 2: write buffer to SD in one shot (SD active, WiFi idle) ── */
    int64_t write_start = esp_timer_get_time();
    size_t wr = fwrite(s_ul_buf, 1, buf_offset, s_ul_fp);
    int64_t write_us = esp_timer_get_time() - write_start;
    s_ul_write_us += write_us;

    if ((int)wr != buf_offset) {
        ESP_LOGE(TAG, "Upload: short write (%u/%d)", (unsigned)wr, buf_offset);
        upload_cleanup();
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"WRITE\"}");
        return ESP_OK;
    }
    s_ul_bytes += wr;

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"written\":%lld,\"recv_ms\":%lld,\"write_ms\":%lld,\"elapsed_ms\":%lld}",
             s_ul_bytes, recv_us / 1000, write_us / 1000,
             (esp_timer_get_time() - chunk_start_us) / 1000);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /upload_end */
static esp_err_t handle_upload_end(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!s_ul_fp) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "{\"error\":\"NOSESSION\"}");
        return ESP_OK;
    }

    bool close_ok = (fclose(s_ul_fp) == 0);
    s_ul_fp = NULL;

    sd_control_unhold();
    sd_control_relinquish();

    free(s_ul_buf);
    s_ul_buf = NULL;

    if (!close_ok) {
        ESP_LOGE(TAG, "Upload: fclose failed");
        unlink(s_ul_path);
        s_ul_path[0] = '\0';
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"CLOSE\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Upload complete: %lld bytes, recv=%lldms write=%lldms (%.0f KiB/s write)",
             s_ul_bytes, s_ul_recv_us / 1000, s_ul_write_us / 1000,
             s_ul_write_us > 0
               ? (double)s_ul_bytes / 1024.0 * 1000000.0 / (double)s_ul_write_us
               : 0.0);

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"total\":%lld,\"recv_ms\":%lld,\"write_ms\":%lld}",
             s_ul_bytes, s_ul_recv_us / 1000, s_ul_write_us / 1000);
    httpd_resp_sendstr(req, resp);

    s_ul_path[0] = '\0';
    s_ul_bytes = s_ul_write_us = s_ul_recv_us = s_ul_last_us = 0;
    return ESP_OK;
}

/* POST /upload_abort */
static esp_err_t handle_upload_abort(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (s_ul_fp) {
        ESP_LOGW(TAG, "Upload aborted by client");
        upload_cleanup();
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ─── /modeline ───────────────────────────────────────────────────── */

static esp_err_t handle_modeline(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "503");
        httpd_resp_sendstr(req, "SD card busy");
        return ESP_OK;
    }

    char *body = NULL;
    if (read_post_body(req, &body, MODELINE_POST_BODY_MAX) < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Missing body");
        return ESP_OK;
    }

    char number[8] = "";
    char *content = malloc(MODELINE_CONTENT_MAX);
    if (!content) {
        free(body);
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }
    content[0] = '\0';
    parse_form_field(body, "number", number, sizeof(number));
    parse_form_field(body, "content", content, MODELINE_CONTENT_MAX);
    free(body);

    if (number[0] == '\0' || content[0] == '\0') {
        free(content);
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Missing required parameters");
        return ESP_OK;
    }

    sd_control_take();

    /* Ensure modelines directory exists */
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/modelines", SD_MOUNT_POINT);
    mkdir(dir_path, 0775);

    char file_path[128];
    snprintf(file_path, sizeof(file_path), "%s/modelines/custom%s.txt", SD_MOUNT_POINT, number);

    /* Remove existing */
    unlink(file_path);

    FILE *f = fopen(file_path, "w");
    if (!f) {
        free(content);
        sd_control_relinquish();
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Failed to create file");
        return ESP_OK;
    }

    int written = fprintf(f, "%s\n", content);
    fclose(f);
    free(content);
    sd_control_relinquish();

    if (written > 0) {
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Failed to write file");
    }

    return ESP_OK;
}

/* ─── WiFi handlers ───────────────────────────────────────────────── */

static esp_err_t handle_wifi_ap(httpd_req_t *req)
{
    set_cors_headers(req);
    if (network_is_sta_mode()) {
        httpd_resp_sendstr(req, "WIFI:StartAPmode");
        network_start_softap();
    } else {
        httpd_resp_sendstr(req, "WIFI:AlreadyAPmode");
    }
    return ESP_OK;
}

static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    set_cors_headers(req);
    char resp[64];
    switch (network_get_status()) {
    case NET_STATUS_DISCONNECTED:
        strcpy(resp, "WIFI:Failed");
        break;
    case NET_STATUS_CONNECTING:
        strcpy(resp, "WIFI:Connecting");
        break;
    case NET_STATUS_CONNECTED: {
        char ip[16];
        network_get_ip_str(ip, sizeof(ip));
        snprintf(resp, sizeof(resp), "WIFI:Connected:%s", ip);
        break;
    }
    }
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    set_cors_headers(req);
    char *body = NULL;
    if (read_post_body(req, &body, SMALL_POST_BODY_MAX) < 0) {
        httpd_resp_sendstr(req, "WIFI:BadRequest");
        return ESP_OK;
    }

    char ssid[32] = "";
    char password[64] = "";
    parse_form_field(body, "ssid", ssid, sizeof(ssid));
    parse_form_field(body, "password", password, sizeof(password));
    free(body);

    if (ssid[0] == '\0' || password[0] == '\0') {
        httpd_resp_sendstr(req, "WIFI:WrongPara");
        return ESP_OK;
    }

    if (network_connect(ssid, password)) {
        httpd_resp_sendstr(req, "WIFI:Starting");
    } else {
        char resp[64];
        char ip[16];
        network_get_ip_str(ip, sizeof(ip));
        snprintf(resp, sizeof(resp), "WIFI:AlreadyCon:%s", ip);
        httpd_resp_sendstr(req, resp);
    }

    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    set_cors_headers(req);
    network_start_scan();
    httpd_resp_set_type(req, "text/json");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t handle_wifi_list(httpd_req_t *req)
{
    set_cors_headers(req);
    char buf[2048];
    network_get_scan_results_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ─── Static file serving from SPIFFS ─────────────────────────────── */

static esp_err_t handle_static(httpd_req_t *req)
{
    set_cors_headers(req);
    const char *uri = req->uri;

    /* Default to index.htm */
    char path[600];
    const char *content_type_path = uri;
    if (strcmp(uri, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.htm", SPIFFS_MOUNT);
        content_type_path = "/index.htm";
    } else {
        snprintf(path, sizeof(path), "%s%s", SPIFFS_MOUNT, uri);
    }

    /* Check for .gz version first */
    char gz_path[610];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", path);

    bool is_gz = false;
    struct stat st;
    if (stat(gz_path, &st) == 0) {
        strcpy(path, gz_path);
        is_gz = true;
    } else if (stat(path, &st) != 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    /* Determine content type from resolved path (not gz path) */
    httpd_resp_set_type(req, get_content_type(content_type_path));
    if (is_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    /* Stream file */
    char *buf = malloc(FILE_BUF_SIZE);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, FILE_BUF_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    fclose(f);
    return ESP_OK;
}

/* ─── /sd_access ──────────────────────────────────────────────────── */

static esp_err_t handle_sd_access_get(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"enabled\":%s,\"esp_exclusive\":%s}",
             sd_control_is_access_enabled() ? "true" : "false",
             sd_control_is_esp_exclusive() ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_sd_access_post(httpd_req_t *req)
{
    set_cors_headers(req);

    char *body = NULL;
    if (read_post_body(req, &body, SMALL_POST_BODY_MAX) < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Missing body");
        return ESP_OK;
    }

    char enable_str[8] = "";
    parse_form_field(body, "enable", enable_str, sizeof(enable_str));
    free(body);

    bool enable = (strcmp(enable_str, "1") == 0 || strcmp(enable_str, "true") == 0);
    esp_err_t err = sd_control_set_access(enable);

    if (err == ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"enabled\":%s}",
                 sd_control_is_access_enabled() ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409");
        httpd_resp_sendstr(req, "{\"error\":\"RT4K may be using SD card\"}");
    } else {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "{\"error\":\"Failed to mount SD card\"}");
    }

    return ESP_OK;
}


/* ─── /sd_esp_exclusive ────────────────────────────────────────────── */

static esp_err_t handle_sd_esp_exclusive_get(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"esp_exclusive\":%s}",
             sd_control_is_esp_exclusive() ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handle_sd_esp_exclusive_post(httpd_req_t *req)
{
    set_cors_headers(req);

    char *body = NULL;
    if (read_post_body(req, &body, SMALL_POST_BODY_MAX) < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Missing body");
        return ESP_OK;
    }

    char val[8] = "";
    parse_form_field(body, "enable", val, sizeof(val));
    free(body);

    bool enable = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
    sd_control_set_esp_exclusive(enable);

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"esp_exclusive\":%s}",
             sd_control_is_esp_exclusive() ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ─── OTA auth helper ──────────────────────────────────────────────── */

static bool check_ota_auth(httpd_req_t *req)
{
    char stored_pass[OTA_PASS_HASH_LEN];
    if (!rt4k_config_get_ota_password(stored_pass, sizeof(stored_pass))) {
        return true; /* No password set — allow */
    }

    char hdr_val[128] = "";
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", hdr_val, sizeof(hdr_val)) != ESP_OK
        || !rt4k_config_check_ota_password(hdr_val)) {
        httpd_resp_set_status(req, "401");
        httpd_resp_sendstr(req, "Unauthorized - incorrect OTA password");
        return false;
    }
    return true;
}

/* ─── /ota ─────────────────────────────────────────────────────────── */

static esp_err_t handle_ota(httpd_req_t *req)
{
    set_cors_headers(req);
    if (!check_ota_auth(req)) return ESP_OK;

    int content_len = req->content_len;
    if (content_len <= 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "No firmware data");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update starting, firmware size: %d bytes", content_len);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "No OTA partition available");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%"PRIx32,
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "OTA begin failed");
        return ESP_OK;
    }

    char *buf = malloc(FILE_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }

    int remaining = content_len;
    int received_total = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_recv = remaining < FILE_BUF_SIZE ? remaining : FILE_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; /* Retry on timeout */
            }
            ESP_LOGE(TAG, "OTA: recv error");
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }

        received_total += received;
        remaining -= received;

        /* Log progress every ~100KB */
        if (received_total % (100 * 1024) < FILE_BUF_SIZE) {
            ESP_LOGI(TAG, "OTA: %d / %d bytes (%d%%)",
                     received_total, content_len,
                     (int)((int64_t)received_total * 100 / content_len));
        }
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "OTA receive/write failed");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500");
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            httpd_resp_sendstr(req, "Firmware validation failed - bad image");
        } else {
            httpd_resp_sendstr(req, "OTA finalize failed");
        }
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Failed to set boot partition");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 2 seconds...");
    httpd_resp_sendstr(req, "OK");

    /* Delay then reboot so the response gets sent */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK; /* Never reached */
}

/* ─── /ota_spiffs ─────────────────────────────────────────────────── */

static esp_err_t handle_ota_spiffs(httpd_req_t *req)
{
    set_cors_headers(req);
    if (!check_ota_auth(req)) return ESP_OK;

    int content_len = req->content_len;
    if (content_len <= 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "No SPIFFS data");
        return ESP_OK;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!part) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "SPIFFS partition not found");
        return ESP_OK;
    }

    if ((size_t)content_len > part->size) {
        httpd_resp_set_status(req, "400");
        char msg[80];
        snprintf(msg, sizeof(msg), "Image too large (%d > %"PRIu32")", content_len, part->size);
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SPIFFS update: %d bytes -> partition '%s'", content_len, part->label);

    /* Unmount SPIFFS so we can write to the partition */
    esp_vfs_spiffs_unregister("storage");
    ESP_LOGI(TAG, "SPIFFS unmounted");

    /* Erase the partition */
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS erase failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Partition erase failed");
        esp_restart();
        return ESP_OK;
    }

    char *buf = malloc(FILE_BUF_SIZE);
    if (!buf) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Out of memory");
        esp_restart();
        return ESP_OK;
    }

    int remaining = content_len;
    size_t offset = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_recv = remaining < FILE_BUF_SIZE ? remaining : FILE_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "SPIFFS OTA: recv error");
            failed = true;
            break;
        }

        err = esp_partition_write(part, offset, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS write failed at offset %u: %s", offset, esp_err_to_name(err));
            failed = true;
            break;
        }

        offset += received;
        remaining -= received;

        if (offset % (100 * 1024) < FILE_BUF_SIZE) {
            ESP_LOGI(TAG, "SPIFFS OTA: %u / %d bytes (%d%%)",
                     offset, content_len,
                     (int)((int64_t)offset * 100 / content_len));
        }
    }

    free(buf);

    if (failed) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "SPIFFS write failed");
        ESP_LOGW(TAG, "SPIFFS write failed, rebooting to remount...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SPIFFS update complete! Rebooting...");
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/* ─── /ota_auth_check ──────────────────────────────────────────────── */

static esp_err_t handle_ota_auth_check(httpd_req_t *req)
{
    set_cors_headers(req);
    if (!check_ota_auth(req)) return ESP_OK; /* sends 401 */
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ─── /ota_password ────────────────────────────────────────────────── */

static esp_err_t handle_ota_password_get(httpd_req_t *req)
{
    set_cors_headers(req);
    char pass[OTA_PASS_HASH_LEN];
    bool has = rt4k_config_get_ota_password(pass, sizeof(pass));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, has ? "{\"hasPassword\":true}" : "{\"hasPassword\":false}");
    return ESP_OK;
}

static esp_err_t handle_ota_password_post(httpd_req_t *req)
{
    set_cors_headers(req);

    char *body = NULL;
    int len = read_post_body(req, &body, SMALL_POST_BODY_MAX);
    if (len < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Bad request");
        return ESP_OK;
    }

    char current[64] = "";
    char newpass[64] = "";
    parse_form_field(body, "current", current, sizeof(current));
    parse_form_field(body, "password", newpass, sizeof(newpass));
    free(body);

    /* If a password is already set, require current password to change it */
    char stored[OTA_PASS_HASH_LEN];
    if (rt4k_config_get_ota_password(stored, sizeof(stored))) {
        if (!rt4k_config_check_ota_password(current)) {
            httpd_resp_set_status(req, "401");
            httpd_resp_sendstr(req, "Current password incorrect");
            return ESP_OK;
        }
    }

    if (rt4k_config_set_ota_password(newpass)) {
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Failed to save password");
    }
    return ESP_OK;
}

/* ─── OPTIONS preflight handler (Chrome Private Network Access) ──── */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-OTA-Password");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ─── WebSocket log streaming ─────────────────────────────────────── */

static esp_err_t handle_ws_logs(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Upgrade handshake — activate log capture */
        int fd = httpd_req_to_sockfd(req);

        /* Create stream buffer on first connect (or if previous was freed) */
        if (!s_log_stream) {
            s_log_stream = xStreamBufferCreate(LOG_STREAM_SIZE, 1);
            if (!s_log_stream) {
                ESP_LOGE(TAG, "Failed to create log stream buffer");
                return ESP_FAIL;
            }
        } else {
            /* New client replaces old — flush stale data */
            xStreamBufferReset(s_log_stream);
        }
        s_ws_log_fd = fd;
        ESP_LOGI(TAG, "WS log client connected: fd=%d", fd);
        return ESP_OK;
    }

    /* Receive incoming frame (poll request) */
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        s_ws_log_fd = -1;
        return ret;
    }

    /* Consume payload if any */
    if (frame.len > 0 && frame.len < 64) {
        uint8_t discard[64];
        frame.payload = discard;
        httpd_ws_recv_frame(req, &frame, sizeof(discard));
    }

    /* Read available log data and send */
    if (s_log_stream) {
        char buf[1024];
        size_t n = xStreamBufferReceive(s_log_stream, buf, sizeof(buf), 0);

        if (n > 0) {
            httpd_ws_frame_t resp = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)buf,
                .len = n,
            };
            ret = httpd_ws_send_frame(req, &resp);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed fd=%d, disconnecting", s_ws_log_fd);
                s_ws_log_fd = -1;
            }
        }
    }
    return ESP_OK;
}

/* ─── SD reprobe ──────────────────────────────────────────────────── */

static esp_err_t handle_sd_reprobe(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = sd_control_reprobe();

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"sd_bus\":\"%s\",\"probe_log\":\"%s\"}",
             err == ESP_OK ? "true" : "false",
             sd_control_get_bus_mode(),
             sd_control_get_probe_log());
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ─── Device info ─────────────────────────────────────────────────── */

static esp_err_t handle_device_info(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    /* Uptime */
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);
    uint32_t days  = uptime_s / 86400;
    uint32_t hours = (uptime_s % 86400) / 3600;
    uint32_t mins  = (uptime_s % 3600) / 60;
    uint32_t secs  = uptime_s % 60;

    /* Heap */
    uint32_t free_heap  = esp_get_free_heap_size();
    uint32_t min_heap   = esp_get_minimum_free_heap_size();
    uint32_t largest_blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    /* WiFi */
    const char *wifi_str;
    char ip_buf[16] = "";
    int8_t rssi = 0;
    switch (network_get_status()) {
    case NET_STATUS_CONNECTED:
        wifi_str = "connected";
        network_get_ip_str(ip_buf, sizeof(ip_buf));
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
            }
        }
        break;
    case NET_STATUS_CONNECTING:
        wifi_str = "connecting";
        break;
    default:
        wifi_str = "disconnected";
        break;
    }

    /* SD bus mode */
    const char *sd_mode = sd_control_get_bus_mode();
    const char *probe_log = sd_control_get_probe_log();

    /* Task count */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    /* Firmware version from PROJECT_VER in CMakeLists.txt */
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"%s\","
        "\"uptime\":\"%"PRIu32"d %"PRIu32"h %"PRIu32"m %"PRIu32"s\","
        "\"uptime_s\":%"PRIu32","
        "\"free_heap\":%"PRIu32","
        "\"min_heap\":%"PRIu32","
        "\"largest_block\":%"PRIu32","
        "\"wifi_status\":\"%s\","
        "\"ip\":\"%s\","
        "\"rssi\":%d,"
        "\"sd_bus\":\"%s\","
        "\"sd_probe_log\":\"%s\","
        "\"tasks\":%u"
        "}",
        app->version,
        days, hours, mins, secs,
        uptime_s,
        free_heap,
        min_heap,
        largest_blk,
        wifi_str,
        ip_buf,
        (int)rssi,
        sd_mode,
        probe_log,
        (unsigned)task_count);
    (void)n;

    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ─── Server startup ──────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;  /* 60s for large uploads over weak WiFi */
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    /* Register URI handlers — order matters for wildcard matching */
    const httpd_uri_t uris[] = {
        { .uri = "/relinquish", .method = HTTP_GET,  .handler = handle_relinquish },
        { .uri = "/sd_access",  .method = HTTP_GET,  .handler = handle_sd_access_get },
        { .uri = "/sd_access",  .method = HTTP_POST, .handler = handle_sd_access_post },
        { .uri = "/sd_esp_exclusive", .method = HTTP_GET,  .handler = handle_sd_esp_exclusive_get },
        { .uri = "/sd_esp_exclusive", .method = HTTP_POST, .handler = handle_sd_esp_exclusive_post },
        { .uri = "/list",       .method = HTTP_GET,  .handler = handle_list },
        { .uri = "/download",   .method = HTTP_GET,  .handler = handle_download },
        { .uri = "/delete",     .method = HTTP_GET,  .handler = handle_delete },
        { .uri = "/upload_begin",  .method = HTTP_POST, .handler = handle_upload_begin },
        { .uri = "/upload_chunk",  .method = HTTP_POST, .handler = handle_upload_chunk },
        { .uri = "/upload_end",    .method = HTTP_POST, .handler = handle_upload_end },
        { .uri = "/upload_abort",  .method = HTTP_POST, .handler = handle_upload_abort },
        { .uri = "/rename",         .method = HTTP_POST, .handler = handle_rename },
        { .uri = "/modeline",   .method = HTTP_POST, .handler = handle_modeline },
        { .uri = "/wifiap",     .method = HTTP_POST, .handler = handle_wifi_ap },
        { .uri = "/wificonnect",.method = HTTP_POST, .handler = handle_wifi_connect },
        { .uri = "/wifistatus", .method = HTTP_GET,  .handler = handle_wifi_status },
        { .uri = "/wifiscan",   .method = HTTP_GET,  .handler = handle_wifi_scan },
        { .uri = "/wifilist",   .method = HTTP_GET,  .handler = handle_wifi_list },
        { .uri = "/ota",        .method = HTTP_POST, .handler = handle_ota },
        { .uri = "/ota_spiffs", .method = HTTP_POST, .handler = handle_ota_spiffs },
        { .uri = "/ota_auth_check", .method = HTTP_GET, .handler = handle_ota_auth_check },
        { .uri = "/ota_password", .method = HTTP_GET,  .handler = handle_ota_password_get },
        { .uri = "/ota_password", .method = HTTP_POST, .handler = handle_ota_password_post },
        { .uri = "/device_info", .method = HTTP_GET,  .handler = handle_device_info },
        { .uri = "/sd_reprobe", .method = HTTP_POST, .handler = handle_sd_reprobe },
        { .uri = "/ws/logs",    .method = HTTP_GET,  .handler = handle_ws_logs,
                                .is_websocket = true },
        { .uri = "/*",          .method = HTTP_GET,     .handler = handle_static },
        { .uri = "/*",          .method = HTTP_OPTIONS, .handler = handle_options },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    /* Install log hook to capture output into stream buffer */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
