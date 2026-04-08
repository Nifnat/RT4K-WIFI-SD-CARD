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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "webserver";
static httpd_handle_t s_server = NULL;

/* Buffer size for streaming file data */
#define FILE_BUF_SIZE 4096
#define SMALL_POST_BODY_MAX 1024
#define MODELINE_POST_BODY_MAX (12 * 1024)
#define MODELINE_CONTENT_MAX   (8 * 1024)

/* Upload async writer configuration */
#define UPLOAD_RECV_BUF_SIZE   4096      /* Network recv chunk */
#define UPLOAD_RING_SLOTS      4         /* Number of ring buffer slots */
#define UPLOAD_RING_SLOT_SIZE  (16384)   /* 16KB per slot */
#define UPLOAD_WRITER_STACK    4096      /* Writer task stack */

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

/* ─── /upload ─────────────────────────────────────────────────────── */

/*
 * Async upload handler with producer-consumer architecture.
 *
 * The HTTP handler task receives data from the network into a ring buffer.
 * A separate writer task drains the ring buffer to the SD card in large
 * (UPLOAD_RING_SLOT_SIZE) writes, keeping the TCP window open while
 * the SD card is busy.
 *
 * Supports two modes:
 *   1. Raw body:  POST /upload?path=/dir&filename=foo.bin
 *      Content-Type: application/octet-stream — skips multipart overhead.
 *   2. Multipart: POST /upload?path=/dir  with multipart/form-data body.
 *      Headers are parsed synchronously; file data streams through the ring.
 */

/* ── Ring buffer shared between recv (producer) and writer (consumer) ── */

typedef struct {
    char   *slots;                         /* UPLOAD_RING_SLOTS × UPLOAD_RING_SLOT_SIZE */
    size_t  slot_len[UPLOAD_RING_SLOTS];   /* bytes valid in each slot */
    int     wr;                            /* next slot producer fills */
    int     rd;                            /* next slot consumer writes to SD */
    SemaphoreHandle_t full;                /* counts slots ready to write */
    SemaphoreHandle_t empty;               /* counts free slots */
    SemaphoreHandle_t writer_done;         /* signaled by writer before exit */
    bool    done;                          /* producer sets when finished */
    bool    error;                         /* set by either side on failure */
    const char *error_msg;
    char    file_path[512];                /* full VFS path for writer to open */
    int64_t write_us;                      /* total SD write time (µs) */
} upload_ring_t;

/* Writer task: runs on its own stack, writes ring slots to SD */
static void upload_writer_task(void *arg)
{
    upload_ring_t *ring = (upload_ring_t *)arg;
    FILE *f = NULL;

    /* Open file for writing */
    unlink(ring->file_path);
    f = fopen(ring->file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Writer: failed to open %s", ring->file_path);
        ring->error = true;
        ring->error_msg = "UPLOAD:OPEN";
        /* Drain all pending signals so producer doesn't block forever */
        while (!ring->done || xSemaphoreTake(ring->full, 0) == pdTRUE) {
            xSemaphoreGive(ring->empty);
            if (!ring->done) xSemaphoreTake(ring->full, pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(ring->writer_done);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Writer: writing to %s", ring->file_path);

    while (true) {
        /* Wait for a full slot (with timeout so we can detect completion) */
        if (xSemaphoreTake(ring->full, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (ring->done || ring->error) break;
            continue;
        }

        if (ring->error) {
            xSemaphoreGive(ring->empty);
            break;
        }

        size_t len = ring->slot_len[ring->rd];
        if (len > 0) {
            int64_t t0 = esp_timer_get_time();
            size_t written = fwrite(ring->slots + (ring->rd * UPLOAD_RING_SLOT_SIZE), 1, len, f);
            ring->write_us += esp_timer_get_time() - t0;

            if (written != len) {
                ESP_LOGE(TAG, "Writer: short write (%u/%u)", written, len);
                ring->error = true;
                ring->error_msg = "UPLOAD:WRITE";
                xSemaphoreGive(ring->empty);
                break;
            }
        }

        ring->rd = (ring->rd + 1) % UPLOAD_RING_SLOTS;
        xSemaphoreGive(ring->empty);
    }

    if (f) {
        if (fclose(f) != 0 && !ring->error) {
            ESP_LOGE(TAG, "Writer: fclose failed");
            ring->error = true;
            ring->error_msg = "UPLOAD:CLOSE";
        }
    }

    /* Drain remaining signals so producer unblocks */
    while (xSemaphoreTake(ring->full, 0) == pdTRUE) {
        xSemaphoreGive(ring->empty);
    }

    xSemaphoreGive(ring->writer_done);
    vTaskDelete(NULL);
}

/* Push data into the ring buffer, splitting across slot boundaries */
static bool ring_push(upload_ring_t *ring, const char *data, size_t len)
{
    while (len > 0 && !ring->error) {
        size_t slot_used = ring->slot_len[ring->wr];
        size_t space = UPLOAD_RING_SLOT_SIZE - slot_used;

        if (space == 0) {
            /* Current slot full — hand to writer */
            xSemaphoreGive(ring->full);
            /* Wait for a free slot */
            while (xSemaphoreTake(ring->empty, pdMS_TO_TICKS(200)) != pdTRUE) {
                if (ring->error) return false;
            }
            ring->wr = (ring->wr + 1) % UPLOAD_RING_SLOTS;
            ring->slot_len[ring->wr] = 0;
            slot_used = 0;
            space = UPLOAD_RING_SLOT_SIZE;
        }

        size_t chunk = len < space ? len : space;
        memcpy(ring->slots + (ring->wr * UPLOAD_RING_SLOT_SIZE) + slot_used, data, chunk);
        ring->slot_len[ring->wr] += chunk;
        data += chunk;
        len -= chunk;
    }
    return !ring->error;
}

/* Flush the current partially-filled slot to the writer */
static void ring_flush(upload_ring_t *ring)
{
    if (ring->slot_len[ring->wr] > 0) {
        xSemaphoreGive(ring->full);
        /* Wait for writer to consume it */
        while (xSemaphoreTake(ring->empty, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (ring->error) return;
        }
        ring->wr = (ring->wr + 1) % UPLOAD_RING_SLOTS;
        ring->slot_len[ring->wr] = 0;
    }
}

/* Find a substring in a buffer (not null-terminated) */
static const char *memmem_find(const char *haystack, size_t hlen,
                               const char *needle, size_t nlen)
{
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return haystack + i;
    }
    return NULL;
}

/*
 * Drain multipart preamble + headers from the request, returning the
 * extracted filename.  Returns bytes of leftover data already read
 * past the header that belong to the file body, stored in `leftover`.
 * Returns -1 on error.
 */
static int multipart_drain_headers(httpd_req_t *req, const char *boundary,
                                   size_t boundary_len, int *remaining,
                                   char *filename, size_t fname_size,
                                   char *leftover, size_t leftover_cap)
{
    /* Small buffer to accumulate preamble+headers (typically < 512 bytes) */
    char hdr_buf[1024];
    size_t hdr_len = 0;

    while (*remaining > 0 && hdr_len < sizeof(hdr_buf) - 1) {
        int to_recv = *remaining < (int)(sizeof(hdr_buf) - hdr_len)
                      ? *remaining : (int)(sizeof(hdr_buf) - hdr_len);
        int received = httpd_req_recv(req, hdr_buf + hdr_len, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        *remaining -= received;
        hdr_len += received;

        /* Look for boundary followed by headers ending with \r\n\r\n */
        const char *bnd = memmem_find(hdr_buf, hdr_len, boundary, boundary_len);
        if (!bnd) continue;

        /* Find end-of-headers after boundary */
        size_t after_bnd = (bnd - hdr_buf) + boundary_len;
        /* Skip CRLF after boundary */
        if (after_bnd + 2 <= hdr_len && hdr_buf[after_bnd] == '\r' && hdr_buf[after_bnd + 1] == '\n')
            after_bnd += 2;

        const char *hdr_end = memmem_find(hdr_buf + after_bnd, hdr_len - after_bnd, "\r\n\r\n", 4);
        if (!hdr_end) continue;

        /* Extract filename from Content-Disposition */
        size_t headers_region = hdr_end - (hdr_buf + after_bnd);
        const char *fn_start = memmem_find(hdr_buf + after_bnd, headers_region, "filename=\"", 10);
        if (fn_start) {
            fn_start += 10;
            const char *fn_end = memchr(fn_start, '"', headers_region - (fn_start - (hdr_buf + after_bnd)));
            if (fn_end) {
                size_t flen = fn_end - fn_start;
                if (flen >= fname_size) flen = fname_size - 1;
                memcpy(filename, fn_start, flen);
                filename[flen] = '\0';
            }
        }

        /* Everything after \r\n\r\n is file data */
        size_t body_start = (hdr_end - hdr_buf) + 4;
        size_t leftover_len = hdr_len - body_start;
        if (leftover_len > leftover_cap) leftover_len = leftover_cap;
        memcpy(leftover, hdr_buf + body_start, leftover_len);
        return (int)leftover_len;
    }

    return -1; /* headers too large or recv failed */
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "UPLOAD:SDBUSY");
        return ESP_OK;
    }

    /* ── Determine upload mode: raw vs multipart ── */
    char content_type[128] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    bool is_raw = (strstr(content_type, "multipart") == NULL);

    /* Get upload path from query string */
    char upload_dir[256] = "/";
    get_query_param(req, "path", upload_dir, sizeof(upload_dir));
    url_decode(upload_dir);
    if (upload_dir[0] != '/') {
        memmove(upload_dir + 1, upload_dir, strlen(upload_dir) + 1);
        upload_dir[0] = '/';
    }

    /* For raw mode, filename comes from query param */
    char filename[128] = "";
    if (is_raw) {
        if (get_query_param(req, "filename", filename, sizeof(filename)) < 0) {
            httpd_resp_set_status(req, "400");
            httpd_resp_sendstr(req, "UPLOAD:NOFILENAME");
            return ESP_OK;
        }
        url_decode(filename);
    }

    /* Multipart: extract boundary */
    char boundary[128] = "";
    size_t boundary_len = 0;
    if (!is_raw) {
        const char *bs = strstr(content_type, "boundary=");
        if (!bs) {
            httpd_resp_set_status(req, "400");
            httpd_resp_sendstr(req, "UPLOAD:NOBOUNDARY");
            return ESP_OK;
        }
        bs += 9;
        snprintf(boundary, sizeof(boundary), "--%s", bs);
        boundary_len = strlen(boundary);
    }

    sd_control_take();

    /* Ensure upload directory exists */
    if (strcmp(upload_dir, "/") != 0) {
        char dir_full[300];
        snprintf(dir_full, sizeof(dir_full), "%s%s", SD_MOUNT_POINT, upload_dir);
        size_t dlen = strlen(dir_full);
        if (dlen > 1 && dir_full[dlen - 1] == '/') dir_full[dlen - 1] = '\0';
        mkdir(dir_full, 0775);
    }

    /* ── Allocate ring buffer ── */
    upload_ring_t ring = {0};
    ring.slots = heap_caps_malloc(UPLOAD_RING_SLOTS * UPLOAD_RING_SLOT_SIZE, MALLOC_CAP_8BIT);
    if (!ring.slots) {
        sd_control_relinquish();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    ring.full  = xSemaphoreCreateCounting(UPLOAD_RING_SLOTS, 0);
    ring.empty = xSemaphoreCreateCounting(UPLOAD_RING_SLOTS, UPLOAD_RING_SLOTS);
    ring.writer_done = xSemaphoreCreateBinary();
    ring.error_msg = "UPLOAD:FAILED";

    /* Build output file path */
    int remaining = req->content_len;
    char file_full[512] = "";
    const char *error_msg = "UPLOAD:FAILED";
    bool upload_failed = false;

    /* For multipart, parse headers to get filename */
    char *leftover_buf = NULL;
    int leftover_len = 0;

    if (!is_raw) {
        leftover_buf = malloc(UPLOAD_RECV_BUF_SIZE);
        if (!leftover_buf) {
            free(ring.slots);
            vSemaphoreDelete(ring.full);
            vSemaphoreDelete(ring.empty);
            vSemaphoreDelete(ring.writer_done);
            sd_control_relinquish();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_OK;
        }

        leftover_len = multipart_drain_headers(req, boundary, boundary_len,
                                                &remaining, filename, sizeof(filename),
                                                leftover_buf, UPLOAD_RECV_BUF_SIZE);
        if (leftover_len < 0 || filename[0] == '\0') {
            upload_failed = true;
            error_msg = "UPLOAD:HEADERS";
        }
    }

    if (!upload_failed && filename[0] == '\0') {
        upload_failed = true;
        error_msg = "UPLOAD:NOFILE";
    }

    if (!upload_failed) {
        if (upload_dir[strlen(upload_dir) - 1] == '/') {
            snprintf(file_full, sizeof(file_full), "%s%s%s",
                     SD_MOUNT_POINT, upload_dir, filename);
        } else {
            snprintf(file_full, sizeof(file_full), "%s%s/%s",
                     SD_MOUNT_POINT, upload_dir, filename);
        }
        snprintf(ring.file_path, sizeof(ring.file_path), "%s", file_full);

        /* Take an empty slot for initial filling */
        xSemaphoreTake(ring.empty, portMAX_DELAY);
        ring.slot_len[0] = 0;

        /* Start writer task */
        TaskHandle_t writer = NULL;
        if (xTaskCreate(upload_writer_task, "upld_wr", UPLOAD_WRITER_STACK,
                        &ring, tskIDLE_PRIORITY + 2, &writer) != pdPASS) {
            upload_failed = true;
            error_msg = "UPLOAD:TASK";
            xSemaphoreGive(ring.empty);
        }
    }

    bool writer_started = !upload_failed;
    int64_t recv_us = 0;
    int64_t total_bytes = 0;

    /* ── Push leftover header data into ring ── */
    if (!upload_failed && !is_raw && leftover_len > 0) {
        /* For multipart, we need to watch for the closing boundary in the tail.
         * The closing boundary "\r\n--boundary--" is at the very end of the body.
         * We know 'remaining' bytes are left, and the tail contains the boundary.
         * We'll trim the boundary from the data at the end. */
        if (!ring_push(&ring, leftover_buf, leftover_len)) {
            upload_failed = true;
            error_msg = ring.error_msg;
        }
        total_bytes += leftover_len;
    }

    free(leftover_buf);
    leftover_buf = NULL;

    /* ── Main recv loop: read from network → push into ring ── */
    if (!upload_failed) {
        char *recv_buf = malloc(UPLOAD_RECV_BUF_SIZE);
        if (!recv_buf) {
            upload_failed = true;
            error_msg = "UPLOAD:NOMEM";
        } else {
            while (remaining > 0 && !upload_failed && !ring.error) {
                int to_recv = remaining < (int)UPLOAD_RECV_BUF_SIZE ? remaining : (int)UPLOAD_RECV_BUF_SIZE;
                int64_t t0 = esp_timer_get_time();
                int received = httpd_req_recv(req, recv_buf, to_recv);
                recv_us += esp_timer_get_time() - t0;

                if (received <= 0) {
                    if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
                    upload_failed = true;
                    error_msg = "UPLOAD:RECV";
                    break;
                }
                remaining -= received;

                if (!ring_push(&ring, recv_buf, received)) {
                    upload_failed = true;
                    error_msg = ring.error_msg;
                    break;
                }
                total_bytes += received;
            }
            free(recv_buf);
        }
    }

    /* ── Signal writer we're done ── */
    if (writer_started) {
        ring_flush(&ring);
        ring.done = true;
        /* Give one more signal to wake writer if it's waiting */
        xSemaphoreGive(ring.full);

        /* Wait for writer task to finish */
        xSemaphoreTake(ring.writer_done, pdMS_TO_TICKS(10000));
    }

    if (ring.error && !upload_failed) {
        upload_failed = true;
        error_msg = ring.error_msg;
    }

    /* ── For multipart, the data we pushed includes the closing boundary.
     *    The file on disk has the trailing "\r\n--boundary--\r\n" appended.
     *    Truncate it off. ── */
    if (!upload_failed && !is_raw && boundary_len > 0) {
        struct stat st;
        if (stat(file_full, &st) == 0 && st.st_size > 0) {
            /* Closing boundary = "\r\n" + boundary + "--" + optional "\r\n"
             * We need to find exactly where it starts and truncate. */
            size_t tail_max = boundary_len + 8; /* \r\n + boundary + -- + \r\n */
            if ((size_t)st.st_size > tail_max) {
                FILE *f = fopen(file_full, "r+b");
                if (f) {
                    char tail[256];
                    size_t to_read = tail_max < sizeof(tail) ? tail_max : sizeof(tail);
                    long seek_pos = st.st_size - (long)to_read;
                    fseek(f, seek_pos, SEEK_SET);
                    size_t got = fread(tail, 1, to_read, f);
                    /* Find "\r\n--boundary" in the tail */
                    const char *bnd = memmem_find(tail, got, boundary, boundary_len);
                    if (bnd) {
                        long trim_pos = seek_pos + (bnd - tail);
                        /* Also trim preceding \r\n */
                        if (trim_pos >= 2) trim_pos -= 2;
                        fclose(f);
                        truncate(file_full, trim_pos);
                    } else {
                        fclose(f);
                    }
                }
            }
        }
    }

    /* ── Clean up ── */
    free(ring.slots);
    vSemaphoreDelete(ring.full);
    vSemaphoreDelete(ring.empty);
    vSemaphoreDelete(ring.writer_done);
    sd_control_relinquish();

    if (upload_failed) {
        if (file_full[0] != '\0') unlink(file_full);
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, error_msg);
        ESP_LOGW(TAG, "Upload failed: %s", error_msg);
        return ESP_OK;
    }

    /* Log timing stats */
    int64_t total_us = recv_us + ring.write_us;
    ESP_LOGI(TAG, "Upload complete: %lld bytes, recv=%lldms write=%lldms total=%lldms (%.0f KiB/s)",
             total_bytes, recv_us / 1000, ring.write_us / 1000, total_us / 1000,
             total_us > 0 ? (double)total_bytes / 1024.0 * 1000000.0 / (double)total_us : 0.0);

    httpd_resp_sendstr(req, "ok");
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
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"enabled\":%s}",
             sd_control_is_access_enabled() ? "true" : "false");
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

/* ─── Server startup ──────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 25;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 60;  /* 60s for large uploads over weak WiFi */
    config.send_wait_timeout = 60;
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
        { .uri = "/list",       .method = HTTP_GET,  .handler = handle_list },
        { .uri = "/download",   .method = HTTP_GET,  .handler = handle_download },
        { .uri = "/delete",     .method = HTTP_GET,  .handler = handle_delete },
        { .uri = "/upload",     .method = HTTP_POST, .handler = handle_upload },
        { .uri = "/rename",     .method = HTTP_POST, .handler = handle_rename },
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
        { .uri = "/*",          .method = HTTP_GET,     .handler = handle_static },
        { .uri = "/*",          .method = HTTP_OPTIONS, .handler = handle_options },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

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
