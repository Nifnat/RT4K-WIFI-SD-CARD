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

static const char *TAG = "webserver";
static httpd_handle_t s_server = NULL;

/* Buffer size for streaming file data */
#define FILE_BUF_SIZE 4096

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
static int read_post_body(httpd_req_t *req, char **out_buf)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) return -1;

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

    /* Extract filename for Content-Disposition */
    const char *fname = strrchr(file_path, '/');
    fname = fname ? fname + 1 : file_path;
    char disp[300];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
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
    if (read_post_body(req, &body) < 0) {
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
 * Upload handler: receives multipart/form-data.
 * The ESP-IDF HTTP server does NOT have built-in multipart parsing,
 * so we parse the multipart boundary manually to extract the file data.
 */

/* Find a substring in a buffer (not null-terminated) */
static const char *memmem_find(const char *haystack, size_t hlen,
                               const char *needle, size_t nlen)
{
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static esp_err_t handle_upload(httpd_req_t *req)
{
    set_cors_headers(req);
    if (sd_control_can_take() != 0) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "UPLOAD:SDBUSY");
        return ESP_OK;
    }

    /* Get content type to extract boundary */
    char content_type[128] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

    const char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "UPLOAD:NOBOUNDARY");
        return ESP_OK;
    }
    boundary_start += 9; /* skip "boundary=" */

    char boundary[128];
    snprintf(boundary, sizeof(boundary), "--%s", boundary_start);
    size_t boundary_len = strlen(boundary);

    /* Get upload path from query string */
    char upload_dir[256] = "/";
    get_query_param(req, "path", upload_dir, sizeof(upload_dir));
    url_decode(upload_dir);
    if (upload_dir[0] != '/') {
        memmove(upload_dir + 1, upload_dir, strlen(upload_dir) + 1);
        upload_dir[0] = '/';
    }

    sd_control_take();

    /* Ensure upload directory exists */
    if (strcmp(upload_dir, "/") != 0) {
        char dir_full[300];
        snprintf(dir_full, sizeof(dir_full), "%s%s", SD_MOUNT_POINT, upload_dir);
        /* Remove trailing slash for mkdir */
        size_t dlen = strlen(dir_full);
        if (dlen > 1 && dir_full[dlen - 1] == '/') dir_full[dlen - 1] = '\0';
        mkdir(dir_full, 0775); /* ignore error - may already exist */
    }

    /* Receive data and parse multipart */
    char *buf = malloc(FILE_BUF_SIZE);
    if (!buf) {
        sd_control_relinquish();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    FILE *upload_file = NULL;
    bool in_file_data = false;
    bool header_parsed __attribute__((unused)) = false;
    char filename[128] = "";

    /* Accumulator for boundary detection at chunk edges */
    char *accum = malloc(FILE_BUF_SIZE * 2);
    if (!accum) {
        free(buf);
        sd_control_relinquish();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    size_t accum_len = 0;

    while (remaining > 0) {
        int to_recv = remaining < FILE_BUF_SIZE ? remaining : FILE_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            ESP_LOGW(TAG, "Upload: recv error");
            break;
        }
        remaining -= received;

        /* Append to accumulator */
        if (accum_len + received > FILE_BUF_SIZE * 2) {
            /* Flush the safe portion that can't contain a split boundary */
            if (in_file_data && upload_file) {
                size_t safe = accum_len - boundary_len;
                if (safe > 0) {
                    fwrite(accum, 1, safe, upload_file);
                    memmove(accum, accum + safe, accum_len - safe);
                    accum_len -= safe;
                }
            }
        }
        memcpy(accum + accum_len, buf, received);
        accum_len += received;

        /* Process accumulated data */
        while (accum_len > 0) {
            if (!in_file_data) {
                /* Look for boundary */
                const char *bnd = memmem_find(accum, accum_len, boundary, boundary_len);
                if (!bnd) break;

                /* Move past boundary + CRLF */
                size_t skip = (bnd - accum) + boundary_len;
                if (skip + 2 <= accum_len && accum[skip] == '\r' && accum[skip + 1] == '\n') {
                    skip += 2;
                }

                /* Check for closing boundary (--boundary--) */
                if (skip <= accum_len && bnd[boundary_len] == '-' && bnd[boundary_len + 1] == '-') {
                    /* End of multipart */
                    accum_len = 0;
                    break;
                }

                memmove(accum, accum + skip, accum_len - skip);
                accum_len -= skip;

                /* Parse headers until empty line */
                const char *header_end = memmem_find(accum, accum_len, "\r\n\r\n", 4);
                if (!header_end) break; /* Need more data */

                /* Extract filename from Content-Disposition header */
                size_t hdr_len = header_end - accum;
                const char *fname_start = memmem_find(accum, hdr_len, "filename=\"", 10);
                if (fname_start) {
                    fname_start += 10;
                    const char *fname_end = memchr(fname_start, '"', hdr_len - (fname_start - accum));
                    if (fname_end) {
                        size_t flen = fname_end - fname_start;
                        if (flen >= sizeof(filename)) flen = sizeof(filename) - 1;
                        memcpy(filename, fname_start, flen);
                        filename[flen] = '\0';
                    }
                }

                /* Skip past headers */
                skip = (header_end - accum) + 4;
                memmove(accum, accum + skip, accum_len - skip);
                accum_len -= skip;

                if (filename[0] != '\0') {
                    /* Open file for writing */
                    char file_full[512];
                    if (upload_dir[strlen(upload_dir) - 1] == '/') {
                        snprintf(file_full, sizeof(file_full), "%s%s%s",
                                 SD_MOUNT_POINT, upload_dir, filename);
                    } else {
                        snprintf(file_full, sizeof(file_full), "%s%s/%s",
                                 SD_MOUNT_POINT, upload_dir, filename);
                    }

                    /* Remove existing file */
                    unlink(file_full);

                    upload_file = fopen(file_full, "w");
                    if (!upload_file) {
                        ESP_LOGE(TAG, "Upload: failed to open %s", file_full);
                    } else {
                        ESP_LOGI(TAG, "Upload: writing to %s", file_full);
                        in_file_data = true;
                        header_parsed = true;
                    }
                }
            } else {
                /* We're inside file data — look for next boundary */
                const char *bnd = memmem_find(accum, accum_len, boundary, boundary_len);
                if (bnd) {
                    /* Write data up to boundary (minus preceding \r\n) */
                    size_t data_len = bnd - accum;
                    if (data_len >= 2) data_len -= 2; /* Strip trailing \r\n before boundary */
                    if (upload_file && data_len > 0) {
                        fwrite(accum, 1, data_len, upload_file);
                    }
                    if (upload_file) {
                        fclose(upload_file);
                        upload_file = NULL;
                    }
                    in_file_data = false;
                    filename[0] = '\0';

                    /* Don't consume boundary — let outer loop handle it */
                    size_t consumed = bnd - accum;
                    memmove(accum, accum + consumed, accum_len - consumed);
                    accum_len -= consumed;
                } else {
                    /* No boundary found — write safe data, keep tail for boundary straddling */
                    if (accum_len > boundary_len + 4) {
                        size_t safe = accum_len - boundary_len - 4;
                        if (upload_file) {
                            fwrite(accum, 1, safe, upload_file);
                        }
                        memmove(accum, accum + safe, accum_len - safe);
                        accum_len -= safe;
                    }
                    break; /* Need more data */
                }
            }
        }
    }

    /* Clean up */
    if (upload_file) {
        fclose(upload_file);
    }

    free(buf);
    free(accum);
    sd_control_relinquish();

    httpd_resp_sendstr(req, "ok");
    ESP_LOGI(TAG, "Upload complete");
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
    if (read_post_body(req, &body) < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_sendstr(req, "Missing body");
        return ESP_OK;
    }

    char number[8] = "";
    char content[1024] = "";
    parse_form_field(body, "number", number, sizeof(number));
    parse_form_field(body, "content", content, sizeof(content));
    free(body);

    if (number[0] == '\0' || content[0] == '\0') {
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
        sd_control_relinquish();
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "Failed to create file");
        return ESP_OK;
    }

    int written = fprintf(f, "%s\n", content);
    fclose(f);
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
    if (read_post_body(req, &body) < 0) {
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

/* Check if a string looks like a raw IP address (starts with digit) */
static bool host_is_ip(const char *host, size_t len)
{
    return len > 0 && host[0] >= '0' && host[0] <= '9';
}

static esp_err_t handle_static(httpd_req_t *req)
{
    set_cors_headers(req);
    const char *uri = req->uri;

    /* Redirect IP-based access to mDNS hostname to avoid Chrome PNA blocks.
     * Chrome blocks fetch() from non-secure (HTTP) origins to private IPs,
     * but allows it when the origin is a .local mDNS hostname. */
    char host_buf[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host_buf, sizeof(host_buf)) == ESP_OK) {
        if (host_is_ip(host_buf, strlen(host_buf))) {
            char location[600];
            snprintf(location, sizeof(location), "http://rt4ksdcard.local%s", uri);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", location);
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    /* Default to index.htm */
    char path[600];
    if (strcmp(uri, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.htm", SPIFFS_MOUNT);
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

    /* Determine content type from original URI (not gz path) */
    httpd_resp_set_type(req, get_content_type(uri));
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
    if (read_post_body(req, &body) < 0) {
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

/* ─── /ota ─────────────────────────────────────────────────────────── */

static esp_err_t handle_ota(httpd_req_t *req)
{
    set_cors_headers(req);

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

/* ─── OPTIONS preflight handler (Chrome Private Network Access) ──── */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ─── Server startup ──────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 22;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;  /* 30s for large uploads */
    config.send_wait_timeout = 30;

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
