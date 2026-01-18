/*
 * modwebfiles.c - Static file server module for MicroPython using ESP-IDF HTTP server
 *
 * Provides high-performance static file serving with:
 * - Direct file streaming from MicroPython VFS
 * - Automatic gzip compression support
 * - MIME type detection
 * - Cache control headers
 *
 * Copyright (c) 2025 Jonathan Peace
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// ESP-IDF includes
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// MicroPython includes
#include "py/runtime.h"
#include "py/stream.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "extmod/vfs.h"

// External functions from httpserver module
extern httpd_handle_t httpserver_get_handle(void);
extern bool httpserver_ensure_mp_thread_state(void);

// Tag for logging
static const char *TAG = "WEBFILES";

// Default base path for files (gets overwritten by webfiles.serve())
static char base_path[64] = "/";

// URI prefix for the file server
static char uri_prefix[32] = "/";

// ESP-IDF direct file serving globals (for www partition)
static bool www_partition_mounted = false;
static bool www_partition_readonly = true;
static const char *www_mount_point = "/www";

// Maximum file path length
#define FILE_PATH_MAX 128

// Scratch buffer size for file transfers
#define SCRATCH_BUFSIZE 4096

// MIME Type Mapping
static const struct {
    const char *extension;
    const char *mimetype;
} mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".js", "application/javascript"},
    {".mjs", "application/javascript"}, // ES modules
    {".css", "text/css"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".svg", "image/svg+xml"},
    {".json", "application/json"},
    {".txt", "text/plain"},
    {".md", "text/markdown"},
    {".wasm", "application/wasm"}, // WebAssembly
    {".map", "application/json"}, // Source maps
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".bin", "application/octet-stream"},
    {NULL, NULL}
};

// Get MIME type based on file extension
// Exported for use by modhttpserver.c
const char* webfiles_get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    
    for (int i = 0; mime_types[i].extension; i++) {
        if (strcasecmp(mime_types[i].extension, ext) == 0) {
            return mime_types[i].mimetype;
        }
    }
    return "text/plain";
}


// Resolve gzip version of file if available
// This function uses mp_vfs_stat() so it MUST be called with GIL held
void webfiles_resolve_gzip(char *filepath, size_t filepath_size, const char *accept_encoding, bool *use_compression) {
    *use_compression = false;
    
    // Only check for common text formats that benefit from compression
    const char *ext = strrchr(filepath, '.');
    if (!ext || !(strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".css") == 0 ||
                  strcasecmp(ext, ".js") == 0 || strcasecmp(ext, ".svg") == 0)) {
        return;  // Not a compressible file type
    }
    
    // Check if client supports gzip
    if (!accept_encoding || !strstr(accept_encoding, "gzip")) {
        return;  // Client doesn't support gzip
    }
    
    // Try .gz version using MicroPython VFS stat
    char gz_filepath[FILE_PATH_MAX + 4];  // +4 for ".gz" + null
    snprintf(gz_filepath, sizeof(gz_filepath), "%s.gz", filepath);
    
    // Check if .gz file exists using MicroPython VFS
    mp_obj_t gz_path_obj = mp_obj_new_str(gz_filepath, strlen(gz_filepath));
    
    // Use exception handler to check existence
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_stat(gz_path_obj);  // Will raise if doesn't exist
        nlr_pop();
        
        // Success! .gz version exists
        if (strlen(gz_filepath) < filepath_size) {
            strcpy(filepath, gz_filepath);
            *use_compression = true;
            ESP_LOGI(TAG, "Using gzip version: %s", filepath);
        } else {
            ESP_LOGW(TAG, "Gzip path too long, using original");
        }
    } else {
        // .gz doesn't exist, use original (no logging needed)
        nlr_pop();
    }
}

// Build full path including base path
static const char* get_full_path(char *dest, const char *uri, size_t destsize) {
    size_t base_len = strlen(base_path);
    size_t prefix_len = strlen(uri_prefix);
    size_t uri_len = strlen(uri);
    
    // Handle query parameters and fragments in URI
    const char *query = strchr(uri, '?');
    if (query) {
        uri_len = query - uri;
    }
    const char *fragment = strchr(uri, '#');
    if (fragment && (!query || fragment < query)) {
        uri_len = fragment - uri;
    }
    
    // Skip the URI prefix to get the relative path
    const char *relative_path = uri;
    if (prefix_len > 1 && strncmp(uri, uri_prefix, prefix_len) == 0) {
        // Skip the prefix, keeping the trailing slash if present
        // Example: uri="/diag/test.txt", prefix="/diag" -> relative_path="/test.txt"
        relative_path = uri + prefix_len;
        uri_len -= prefix_len;
    }
    
    // Check if path will fit in destination buffer
    if (base_len + uri_len + 1 > destsize) {
        ESP_LOGE(TAG, "Path too long");
        return NULL;
    }
    
    // Construct full path
    strcpy(dest, base_path);
    if (base_len > 0 && base_path[base_len-1] == '/' && relative_path[0] == '/') {
        // Avoid double slash
        strlcpy(dest + base_len, relative_path + 1, uri_len);
    } else {
        strlcpy(dest + base_len, relative_path, uri_len + 1);
    }
    
    return dest;
}

// Set content type based on file extension
// Exported for use by modhttpserver.c
void webfiles_set_content_type_from_file(httpd_req_t *req, const char *filepath) {
    const char* mime_type = webfiles_get_mime_type(filepath);
    httpd_resp_set_type(req, mime_type);
    
    // Set Cache-Control header for static assets
    // Don't cache HTML, but cache other static assets
    if (strstr(mime_type, "text/html") == NULL) {
        // Cache for 1 hour (3600 seconds)
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    } else {
        // Don't cache HTML content
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    }
    
    // Add CORS headers for development convenience
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Mount the www partition for direct ESP-IDF file access (read-only)
static esp_err_t mount_www_partition_readonly(void) {
    if (www_partition_mounted) {
        if (!www_partition_readonly) {
            ESP_LOGW(TAG, "www partition already mounted read-write, unmount first for read-only access");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting www partition for direct file access (read-only)");

    // Mount LittleFS filesystem read-only
    esp_vfs_littlefs_conf_t conf = {
        .base_path = www_mount_point,
        .partition_label = "www",
        .format_if_mount_failed = false,  // Don't format - partition should be pre-populated
        .dont_mount = false,
        .read_only = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS filesystem on www partition: %s", esp_err_to_name(err));
        return err;
    }

    www_partition_mounted = true;
    www_partition_readonly = true;
    ESP_LOGI(TAG, "www partition mounted successfully at %s (read-only)", www_mount_point);
    return ESP_OK;
}

// Mount the www partition for read-write access (for populating files)
static esp_err_t mount_www_partition_readwrite(void) {
    if (www_partition_mounted) {
        if (www_partition_readonly) {
            ESP_LOGW(TAG, "www partition already mounted read-only, unmount first for read-write access");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting www partition for file population (read-write)");

    // Mount LittleFS filesystem read-write
    esp_vfs_littlefs_conf_t conf = {
        .base_path = www_mount_point,
        .partition_label = "www",
        .format_if_mount_failed = true,  // Format if mount fails (for first use)
        .dont_mount = false,
        .read_only = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS filesystem on www partition: %s", esp_err_to_name(err));
        return err;
    }

    www_partition_mounted = true;
    www_partition_readonly = false;
    ESP_LOGI(TAG, "www partition mounted successfully at %s (read-write)", www_mount_point);
    return ESP_OK;
}

// Unmount the www partition
static esp_err_t unmount_www_partition(void) {
    if (!www_partition_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting www partition");

    esp_err_t err = esp_vfs_littlefs_unregister("www");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount www partition: %s", esp_err_to_name(err));
        return err;
    }

    www_partition_mounted = false;
    www_partition_readonly = true;  // Reset to default
    ESP_LOGI(TAG, "www partition unmounted successfully");
    return ESP_OK;
}

// Copy file from MicroPython filesystem to www partition using MicroPython VFS for source, ESP-IDF VFS for dest
static bool copy_file_to_www(const char *src_path, const char *dst_filename) {
    if (!www_partition_mounted || www_partition_readonly) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: www partition not mounted read-write\n");
        return false;
    }

    // Build destination path
    char dst_path[FILE_PATH_MAX];
    snprintf(dst_path, sizeof(dst_path), "%s/%s", www_mount_point, dst_filename);

    mp_printf(&mp_plat_print, "[WEBFILES] Copying %s to %s\n", src_path, dst_path);

    // Open source file using MicroPython VFS
    mp_obj_t args[2] = {
        mp_obj_new_str(src_path, strlen(src_path)),
        MP_OBJ_NEW_QSTR(MP_QSTR_rb)
    };

    nlr_buf_t nlr;
    mp_obj_t src_file_obj = MP_OBJ_NULL;

    if (nlr_push(&nlr) == 0) {
        src_file_obj = mp_vfs_open(MP_ARRAY_SIZE(args), args, (mp_map_t *)&mp_const_empty_map);
        nlr_pop();
    } else {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to open source file: %s\n", src_path);
        return false;
    }

    if (src_file_obj == MP_OBJ_NULL) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to open source file: %s\n", src_path);
        return false;
    }

    mp_printf(&mp_plat_print, "[WEBFILES] Source file opened successfully\n");

    // Open destination file (in www partition) using ESP-IDF VFS
    FILE *dst_file = fopen(dst_path, "wb");
    if (!dst_file) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to open destination file: %s (errno: %d)\n", dst_path, errno);
        // Close MicroPython file
        mp_stream_close(src_file_obj);
        return false;
    }
    mp_printf(&mp_plat_print, "[WEBFILES] Destination file opened successfully\n");

    // Copy file contents
    char buffer[1024];
    size_t total_copied = 0;

    // Use MicroPython stream interface to read from source
    const mp_stream_p_t *src_stream = mp_get_stream(src_file_obj);
    if (!src_stream) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Source file is not a stream\n");
        fclose(dst_file);
        mp_stream_close(src_file_obj);
        return false;
    }

    while (true) {
        int errcode;
        mp_uint_t bytes_read = src_stream->read(src_file_obj, buffer, sizeof(buffer), &errcode);
        if (bytes_read == MP_STREAM_ERROR) {
            mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to read from source file (errcode: %d)\n", errcode);
            fclose(dst_file);
            mp_stream_close(src_file_obj);
            return false;
        }

        if (bytes_read == 0) {
            // End of file
            break;
        }

        if (fwrite(buffer, 1, bytes_read, dst_file) != bytes_read) {
            mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to write to destination file\n");
            fclose(dst_file);
            mp_stream_close(src_file_obj);
            return false;
        }
        total_copied += bytes_read;
    }

    fclose(dst_file);
    mp_stream_close(src_file_obj);

    mp_printf(&mp_plat_print, "[WEBFILES] Successfully copied %d bytes to %s\n", (int)total_copied, dst_path);
    return total_copied > 0;
}


// Direct file serving handler for www partition (bypasses MicroPython)
static esp_err_t webfiles_direct_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== DIRECT WEBFILES HANDLER CALLED for URI: %s ===", req->uri);

    if (!www_partition_mounted) {
        ESP_LOGE(TAG, "www partition not mounted");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "www partition not mounted");
        return ESP_FAIL;
    }

    // Process any URL query parameters if needed
    char *query = strchr(req->uri, '?');
    if (query) {
        ESP_LOGI(TAG, "Request has query params: %s", query);
        *query = '\0'; // Temporarily terminate URI at the query string for path resolution
    }

    // Build path within www partition
    char filepath[FILE_PATH_MAX];
    size_t uri_len = strlen(req->uri);
    if (uri_len + strlen(www_mount_point) >= sizeof(filepath)) {
        ESP_LOGE(TAG, "Path too long");
        if (query) *query = '?';
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Construct full path: /www + uri (skip leading / if uri starts with /)
    strcpy(filepath, www_mount_point);
    if (req->uri[0] == '/') {
        strlcat(filepath, req->uri, sizeof(filepath));
    } else {
        strlcat(filepath, "/", sizeof(filepath));
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    // Restore query string if we modified it
    if (query) *query = '?';

    ESP_LOGI(TAG, "Direct serving file: %s", filepath);

    // Open file directly using ESP-IDF VFS
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    ESP_LOGI(TAG, "File size: %d bytes", (int)file_size);

    // Set content type
    webfiles_set_content_type_from_file(req, filepath);

    // Allocate buffer on heap to avoid stack overflow with large files
    char *buffer = malloc(SCRATCH_BUFSIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for file transfer");
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Read and send file in chunks
    size_t total_sent = 0;
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, SCRATCH_BUFSIZE, file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk");
            free(buffer);
            fclose(file);
            return ESP_FAIL;
        }
        total_sent += bytes_read;
    }

    free(buffer);
    fclose(file);

    // Finish response
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "Direct file served successfully: %d bytes", (int)total_sent);

    return ESP_OK;
}

// Queues file requests to be processed by the MicroPython task
// Direct file handler using GIL acquisition (optimized for multi-threaded HTTP server)
// CONTEXT: ESP-IDF HTTP Server Task (one of multiple worker threads)
// EXPERIMENTAL: Initializes MP thread state on-demand for HTTP worker threads
static esp_err_t webfiles_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Serving file for URI: %s (method: %s)", req->uri, 
             req->method == HTTP_GET ? "GET" : "HEAD");
    
    // Check if this is a HEAD request
    bool is_head_request = (req->method == HTTP_HEAD);
    
    // Process any URL query parameters if needed
    char *query = strchr(req->uri, '?');
    if (query) {
        *query = '\0'; // Temporarily terminate URI at the query string for path resolution
    }

    // Get the full file path from the URI
    char filepath[FILE_PATH_MAX];
    if (get_full_path(filepath, req->uri, sizeof(filepath)) == NULL) {
        ESP_LOGE(TAG, "Failed to get file path for URI: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Restore query string if we modified it
    if (query) *query = '?';
    
    // Get Accept-Encoding header for gzip detection
    char accept_encoding[64] = {0};
    httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_encoding, sizeof(accept_encoding));

    // For HEAD requests, we need to use MicroPython VFS but can skip file reading
    if (is_head_request) {
        // Ensure HTTP worker thread has MP state
        if (!httpserver_ensure_mp_thread_state()) {
            ESP_LOGE(TAG, "Failed to initialize MP thread state");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Thread init failed");
            return ESP_FAIL;
        }

        // Acquire MicroPython GIL for VFS access
        MP_THREAD_GIL_ENTER();
        
        // Check for gzip version
        bool use_compression = false;
        webfiles_resolve_gzip(filepath, sizeof(filepath), accept_encoding, &use_compression);
        
        // Use mp_vfs_stat to check file existence and get size
        mp_obj_t path_obj = mp_obj_new_str(filepath, strlen(filepath));
        nlr_buf_t nlr;
        size_t file_size = 0;
        
        if (nlr_push(&nlr) == 0) {
            // mp_vfs_stat returns a tuple with 10 elements, st_size is at index 6
            mp_obj_t stat_result = mp_vfs_stat(path_obj);
            
            // Verify it's actually a tuple
            if (!mp_obj_is_type(stat_result, &mp_type_tuple)) {
                ESP_LOGE(TAG, "HEAD: mp_vfs_stat did not return a tuple!");
                nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("stat() returned non-tuple")));
            }
            
            // Extract size from stat tuple (items[6] = st_size)
            mp_obj_t *items;
            size_t tuple_len;
            mp_obj_get_array(stat_result, &tuple_len, &items);
            
            if (tuple_len < 7) {
                ESP_LOGE(TAG, "HEAD: stat tuple too short (%d < 7)", (int)tuple_len);
                nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("stat() tuple too short")));
            }
            
            mp_obj_t size_obj = items[6];
            
            // Extract file size - try direct extraction first for small ints
            if (mp_obj_is_small_int(size_obj)) {
                file_size = (size_t)MP_OBJ_SMALL_INT_VALUE(size_obj);
            } else {
                // For larger integers or other int types, use mp_obj_get_int
                mp_int_t size_int = mp_obj_get_int(size_obj);
                file_size = (size_t)size_int;
            }
            
            nlr_pop();
        } else {
            // File doesn't exist
            ESP_LOGE(TAG, "HEAD: File not found: %s", filepath);
            nlr_pop();
            MP_THREAD_GIL_EXIT();
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
            return ESP_FAIL;
        }
        
        // Set headers for HEAD request
        const char *mime_type = webfiles_get_mime_type(filepath);
        httpd_resp_set_type(req, mime_type);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        
        if (use_compression) {
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        }
        
        // Set Content-Length for HEAD request (no chunked encoding for HEAD)
        char content_length_str[32];
        snprintf(content_length_str, sizeof(content_length_str), "%d", (int)file_size);
        esp_err_t hdr_ret = httpd_resp_set_hdr(req, "Content-Length", content_length_str);
        if (hdr_ret != ESP_OK) {
            ESP_LOGE(TAG, "HEAD: Failed to set Content-Length header: %d", hdr_ret);
        }
        
        // Release GIL BEFORE finalizing response (ESP-IDF operations should not be under GIL)
        MP_THREAD_GIL_EXIT();

        // Finalize HEAD response (no body) - use httpd_resp_send() for non-chunked
        esp_err_t send_ret = httpd_resp_send(req, NULL, 0);
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "HEAD: httpd_resp_send() failed: %d", send_ret);
        }
        
        ESP_LOGI(TAG, "HEAD: %s (%d bytes)%s", filepath, (int)file_size, 
                 use_compression ? " [gzip]" : "");
        return send_ret == ESP_OK ? ESP_OK : ESP_FAIL;
    }

    // For GET requests, do full file processing with GIL
    // Ensure HTTP worker thread has MP state
    if (!httpserver_ensure_mp_thread_state()) {
        ESP_LOGE(TAG, "Failed to initialize MP thread state");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Thread init failed");
        return ESP_FAIL;
    }

    // Acquire MicroPython GIL for VFS access
    MP_THREAD_GIL_ENTER();
    
    // Check for gzip version
    bool use_compression = false;
    webfiles_resolve_gzip(filepath, sizeof(filepath), accept_encoding, &use_compression);
    
    ESP_LOGI(TAG, "Serving file: %s%s", filepath, use_compression ? " (gzipped)" : "");
    
    // Wrap entire file serving in exception handler
    nlr_buf_t nlr_top;
    mp_obj_t file_obj = MP_OBJ_NULL;
    char *buffer = NULL;
    esp_err_t result = ESP_OK;
    
    if (nlr_push(&nlr_top) == 0) {
        // Open file using MicroPython VFS
        mp_obj_t path_obj = mp_obj_new_str(filepath, strlen(filepath));
        mp_obj_t args[2] = { path_obj, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
        file_obj = mp_builtin_open(2, args, (mp_map_t *)&mp_const_empty_map);

        // Get file size by seeking to end
        mp_obj_t seek_method = mp_load_attr(file_obj, MP_QSTR_seek);
        mp_obj_t tell_method = mp_load_attr(file_obj, MP_QSTR_tell);
        
        // Seek to end (2 = SEEK_END)
        mp_obj_t seek_args[2] = { MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(2) };
        mp_call_function_n_kw(seek_method, 2, 0, seek_args);
        
        // Get position (file size)
        mp_obj_t size_obj = mp_call_function_0(tell_method);
        size_t file_size = mp_obj_get_int(size_obj);
        
        // Seek back to start (0 = SEEK_SET)
        seek_args[1] = MP_OBJ_NEW_SMALL_INT(0);
        mp_call_function_n_kw(seek_method, 2, 0, seek_args);
        
        ESP_LOGD(TAG, "File size: %d bytes", (int)file_size);

        // Set content type (use original filename for MIME type if compressed)
        char original_filepath[FILE_PATH_MAX];
        if (use_compression) {
            // Strip .gz extension for MIME type detection
            strcpy(original_filepath, filepath);
            char *dot_pos = strrchr(original_filepath, '.');
            if (dot_pos) {
                *dot_pos = '\0';  // Remove .gz extension
            }
            const char *mime_type = webfiles_get_mime_type(original_filepath);
            httpd_resp_set_type(req, mime_type);
            
            // Set compression headers
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        } else {
            const char *mime_type = webfiles_get_mime_type(filepath);
            httpd_resp_set_type(req, mime_type);
        }
        
        // Enable CORS
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

        // Note: We use chunked transfer encoding (via httpd_resp_send_chunk)
        // Do NOT set Content-Length - it's incompatible with Transfer-Encoding: chunked

        // Read and send file content (GET requests only)
        size_t total_sent = 0;
        
        // Allocate buffer for file transfer
        #define FILE_BUFFER_SIZE 4096
        buffer = malloc(FILE_BUFFER_SIZE);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate file buffer");
            nlr_raise(mp_obj_new_exception_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Out of memory")));
        }

        // Read and send file in chunks (keep GIL held throughout)
        mp_obj_t read_method = mp_load_attr(file_obj, MP_QSTR_read);
        esp_err_t send_err = ESP_OK;

        while (total_sent < file_size && send_err == ESP_OK) {
            // Read chunk from file
            mp_obj_t chunk_size_obj = MP_OBJ_NEW_SMALL_INT(FILE_BUFFER_SIZE);
            mp_obj_t chunk_obj = mp_call_function_1(read_method, chunk_size_obj);
            
            // Get buffer and length from bytes object
            mp_buffer_info_t bufinfo;
            mp_get_buffer_raise(chunk_obj, &bufinfo, MP_BUFFER_READ);
            
            if (bufinfo.len == 0) {
                break; // EOF
            }
            
            // Send chunk via HTTP (GIL still held)
            send_err = httpd_resp_send_chunk(req, bufinfo.buf, bufinfo.len);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk: %d", send_err);
                result = send_err;
                break;
            }
            
            total_sent += bufinfo.len;
        }

        ESP_LOGI(TAG, "File served: %d bytes (%s)", (int)total_sent, use_compression ? "gzip" : "uncompressed");

        // NOTE: Skipping final empty chunk - httpd_resp_send_chunk(req, NULL, 0) causes crashes
        // The HTTP server handles chunked transfer termination automatically

        // NOTE: Skipping explicit file close - it causes crashes
        // File was opened read-only, MicroPython GC will clean it up

        // Protected code ends here
        nlr_pop();
        
    } else {
        // Exception occurred during file serving
        ESP_LOGE(TAG, "Exception during file serving");
        
        // Try to close file if it was opened
        if (file_obj != MP_OBJ_NULL) {
            nlr_buf_t close_nlr;
            if (nlr_push(&close_nlr) == 0) {
                mp_obj_t close_method = mp_load_attr(file_obj, MP_QSTR_close);
                mp_call_function_0(close_method);
                nlr_pop();
            } else {
                nlr_pop();
                ESP_LOGW(TAG, "Failed to close file after exception");
            }
        }
        
        result = ESP_FAIL;
        
        // Release GIL before sending error response
        MP_THREAD_GIL_EXIT();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error reading file");
        MP_THREAD_GIL_ENTER();
    }
    
    // Clean up buffer if allocated
    if (buffer) {
        free(buffer);
    }
    
    // Release MicroPython GIL
    MP_THREAD_GIL_EXIT();

    // Send final empty chunk to terminate chunked transfer
    // This MUST be done for httpd_resp_send_chunk() to work correctly
    // Done after GIL release since it's pure ESP-IDF operation
    if (result == ESP_OK) {
        ESP_LOGD(TAG, "Sending final empty chunk to terminate response");
        esp_err_t chunk_err = httpd_resp_send_chunk(req, NULL, 0);
        if (chunk_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send final chunk: %d", chunk_err);
            return chunk_err;
        }
        ESP_LOGD(TAG, "Final chunk sent successfully");
    }

    return result;
}

// ------------------------------------------------------------------------
// MicroPython module interface functions
// ------------------------------------------------------------------------

// webfiles.serve(base_path, uri_prefix) -> bool
// Serve files from base_path at uri_prefix
static mp_obj_t webfiles_serve(mp_obj_t base_path_obj, mp_obj_t uri_prefix_obj) {
    if (!mp_obj_is_str(base_path_obj) || !mp_obj_is_str(uri_prefix_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Both arguments must be strings"));
    }

    const char* path = mp_obj_str_get_str(base_path_obj);
    const char* prefix = mp_obj_str_get_str(uri_prefix_obj);
    
    ESP_LOGI(TAG, "Setting up file server with base path: %s, uri prefix: %s", path, prefix);
    mp_printf(&mp_plat_print, "[WEBFILES] Base path: %s, URI prefix: %s\n", path, prefix);
    
    // Check if path is a valid directory using MicroPython VFS (skip check for current directory)
    if (strcmp(path, ".") != 0 && strcmp(path, "") != 0) {
        // Use MicroPython's VFS to check if path exists and is a directory
        mp_obj_t path_obj = mp_obj_new_str(path, strlen(path));
        mp_obj_t stat_result = MP_OBJ_NULL;
        
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            // Try to stat the path using MicroPython VFS
            stat_result = mp_vfs_stat(path_obj);
            nlr_pop();
        } else {
            // Exception means path doesn't exist
            ESP_LOGE(TAG, "Path not found in MicroPython VFS: %s", path);
            mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Path not found: %s\n", path);
            return mp_obj_new_bool(false);
        }
        
        // Check if it's a directory (st_mode is first element of stat tuple)
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(stat_result, 10, &items);
        mp_int_t st_mode = mp_obj_get_int(items[0]);
        
        if (!(st_mode & MP_S_IFDIR)) {
            ESP_LOGE(TAG, "Path is not a directory: %s", path);
            mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Not a directory: %s\n", path);
            return mp_obj_new_bool(false);
        }
        
        ESP_LOGI(TAG, "Path verified in MicroPython VFS: %s", path);
        mp_printf(&mp_plat_print, "[WEBFILES] ✓ Path exists in MicroPython VFS: %s\n", path);
    } else {
        ESP_LOGI(TAG, "Using current directory");
        mp_printf(&mp_plat_print, "[WEBFILES] Using current directory\n");
    }

    // Get HTTP server handle
    httpd_handle_t server = httpserver_get_handle();
    if (!server) {
        ESP_LOGE(TAG, "HTTP server not running");
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: HTTP server not running\n");
        return mp_obj_new_bool(false);
    }

    // Save base path and URI prefix
    // Convert "." to "/" for absolute path (ESP-IDF task has different CWD)
    if (strcmp(path, ".") == 0) {
        strlcpy(base_path, "/", sizeof(base_path));
        mp_printf(&mp_plat_print, "[WEBFILES] Converted '.' to '/'\n");
    } else {
        strlcpy(base_path, path, sizeof(base_path));
    }
    strlcpy(uri_prefix, prefix, sizeof(uri_prefix));
    
    // Use a static buffer for the URI pattern
    static char registered_uri_pattern[64];
    registered_uri_pattern[sizeof(registered_uri_pattern) - 1] = '\0';
    
    // If prefix already ends with *, use as-is, otherwise append *
    if (prefix[strlen(prefix) - 1] == '*') {
        snprintf(registered_uri_pattern, sizeof(registered_uri_pattern), "%s", prefix);
    } else {
        snprintf(registered_uri_pattern, sizeof(registered_uri_pattern), "%s*", prefix);
    }

    ESP_LOGI(TAG, "Registering URI handler with pattern: %s", registered_uri_pattern);
    mp_printf(&mp_plat_print, "[WEBFILES] Registering pattern: %s\n", registered_uri_pattern);

    // Register the handler with ESP-IDF HTTP server
    // Register GET handler
    httpd_uri_t get_handler = {
        .uri      = registered_uri_pattern,
        .method   = HTTP_GET,
        .handler  = webfiles_handler,
        .user_ctx = NULL
    };

    esp_err_t ret = httpd_register_uri_handler(server, &get_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET handler: %d", ret);
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to register GET handler (error %d)\n", ret);
        return mp_obj_new_bool(false);
    }

    // Register HEAD handler (same handler, but ESP-IDF will handle not sending body)
    httpd_uri_t head_handler = {
        .uri      = registered_uri_pattern,
        .method   = HTTP_HEAD,
        .handler  = webfiles_handler,
        .user_ctx = NULL
    };

    ret = httpd_register_uri_handler(server, &head_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HEAD handler: %d", ret);
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to register HEAD handler (error %d)\n", ret);
        return mp_obj_new_bool(false);
    }
    
    mp_printf(&mp_plat_print, "[WEBFILES] File server started successfully\n");
    return mp_obj_new_bool(true);
}
static MP_DEFINE_CONST_FUN_OBJ_2(webfiles_serve_obj, webfiles_serve);

// webfiles.serve_file(file_path, uri) -> bool
// Serve a specific file at a specific URI (placeholder for future implementation)
static mp_obj_t webfiles_serve_file(mp_obj_t file_path_obj, mp_obj_t uri_obj) {
    if (!mp_obj_is_str(file_path_obj) || !mp_obj_is_str(uri_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Both arguments must be strings"));
    }

    const char* file_path = mp_obj_str_get_str(file_path_obj);
    // const char* uri = mp_obj_str_get_str(uri_obj);  // TODO: Will be used in future implementation
    (void)uri_obj;  // Suppress unused parameter warning
    
    // Check if file exists
    struct stat file_stat;
    if (stat(file_path, &file_stat) == -1) {
        ESP_LOGE(TAG, "File not found: %s", file_path);
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: File not found: %s\n", file_path);
        return mp_obj_new_bool(false);
    }
    
    // TODO: Implement custom handler for specific files
    // This would require keeping track of file mappings
    
    ESP_LOGW(TAG, "serve_file not yet implemented");
    mp_printf(&mp_plat_print, "[WEBFILES] WARNING: serve_file not yet implemented\n");
    return mp_obj_new_bool(false);
}
static MP_DEFINE_CONST_FUN_OBJ_2(webfiles_serve_file_obj, webfiles_serve_file);

// Forward declarations for www partition functions
static mp_obj_t webfiles_mount_www(void);
static mp_obj_t webfiles_mount_www_rw(void);
static mp_obj_t webfiles_unmount_www(void);
static mp_obj_t webfiles_copy_to_www(mp_obj_t src_path_obj, mp_obj_t dst_filename_obj);
static mp_obj_t webfiles_serve_www(mp_obj_t uri_prefix_obj);

static MP_DEFINE_CONST_FUN_OBJ_0(webfiles_mount_www_obj, webfiles_mount_www);
static MP_DEFINE_CONST_FUN_OBJ_0(webfiles_mount_www_rw_obj, webfiles_mount_www_rw);
static MP_DEFINE_CONST_FUN_OBJ_0(webfiles_unmount_www_obj, webfiles_unmount_www);
static MP_DEFINE_CONST_FUN_OBJ_2(webfiles_copy_to_www_obj, webfiles_copy_to_www);
static MP_DEFINE_CONST_FUN_OBJ_1(webfiles_serve_www_obj, webfiles_serve_www);

// webfiles.mount_www() -> bool
// Mount the www partition using ESP-IDF FAT (read-only)
static mp_obj_t webfiles_mount_www(void) {
    if (www_partition_mounted) {
        if (www_partition_readonly) {
            mp_printf(&mp_plat_print, "[WEBFILES] www partition already mounted read-only\n");
            return mp_obj_new_bool(true);
        } else {
            // Unmount read-write first
            unmount_www_partition();
        }
    }

    esp_err_t err = mount_www_partition_readonly();
    if (err != ESP_OK) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to mount www partition read-only (%d)\n", err);
        return mp_obj_new_bool(false);
    }
    mp_printf(&mp_plat_print, "[WEBFILES] www partition mounted successfully (read-only)\n");
    return mp_obj_new_bool(true);
}

// webfiles.mount_www_rw() -> bool
// Mount the www partition using ESP-IDF FAT (read-write)
static mp_obj_t webfiles_mount_www_rw(void) {
    if (www_partition_mounted) {
        if (!www_partition_readonly) {
            mp_printf(&mp_plat_print, "[WEBFILES] www partition already mounted read-write\n");
            return mp_obj_new_bool(true);
        } else {
            // Unmount read-only first
            unmount_www_partition();
        }
    }

    esp_err_t err = mount_www_partition_readwrite();
    if (err != ESP_OK) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to mount www partition read-write (%d)\n", err);
        return mp_obj_new_bool(false);
    }
    mp_printf(&mp_plat_print, "[WEBFILES] www partition mounted successfully (read-write)\n");
    return mp_obj_new_bool(true);
}

// webfiles.copy_to_www(src_path, dst_filename) -> bool
// Copy file from MicroPython filesystem to www partition
static mp_obj_t webfiles_copy_to_www(mp_obj_t src_path_obj, mp_obj_t dst_filename_obj) {
    if (!mp_obj_is_str(src_path_obj) || !mp_obj_is_str(dst_filename_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("Both arguments must be strings"));
    }

    if (!www_partition_mounted || www_partition_readonly) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: www partition not mounted read-write, call mount_www_rw() first\n");
        return mp_obj_new_bool(false);
    }

    const char* src_path = mp_obj_str_get_str(src_path_obj);
    const char* dst_filename = mp_obj_str_get_str(dst_filename_obj);

    bool success = copy_file_to_www(src_path, dst_filename);
    if (success) {
        mp_printf(&mp_plat_print, "[WEBFILES] Successfully copied %s to /www/%s\n", src_path, dst_filename);
    } else {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to copy %s to /www/%s\n", src_path, dst_filename);
    }

    return mp_obj_new_bool(success);
}

// webfiles.unmount_www() -> bool
// Unmount the www partition
static mp_obj_t webfiles_unmount_www(void) {
    esp_err_t err = unmount_www_partition();
    if (err != ESP_OK) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to unmount www partition (%d)\n", err);
        return mp_obj_new_bool(false);
    }
    mp_printf(&mp_plat_print, "[WEBFILES] www partition unmounted successfully\n");
    return mp_obj_new_bool(true);
}


// webfiles.serve_www(uri_prefix) -> bool
// Serve files directly from www partition using ESP-IDF (no MicroPython involvement)
static mp_obj_t webfiles_serve_www(mp_obj_t uri_prefix_obj) {
    if (!mp_obj_is_str(uri_prefix_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("URI prefix must be a string"));
    }

    if (!www_partition_mounted) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: www partition not mounted, call mount_www() first\n");
        return mp_obj_new_bool(false);
    }

    const char* prefix = mp_obj_str_get_str(uri_prefix_obj);

    // Get HTTP server handle
    httpd_handle_t server = httpserver_get_handle();
    if (!server) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: HTTP server not running\n");
        return mp_obj_new_bool(false);
    }

    // Register the direct handler
    httpd_uri_t uri_handler = {
        .uri      = prefix,
        .method   = HTTP_GET,
        .handler  = webfiles_direct_handler,
        .user_ctx = NULL
    };

    esp_err_t ret = httpd_register_uri_handler(server, &uri_handler);
    if (ret != ESP_OK) {
        mp_printf(&mp_plat_print, "[WEBFILES] ERROR: Failed to register direct handler (error %d)\n", ret);
        return mp_obj_new_bool(false);
    }

    mp_printf(&mp_plat_print, "[WEBFILES] Direct file server started at: %s\n", prefix);
    return mp_obj_new_bool(true);
}

// webfiles.check_vfs(path) -> None
// Diagnostic function to check ESP-IDF VFS access
static mp_obj_t webfiles_check_vfs(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    
    mp_printf(&mp_plat_print, "\n[ESP-IDF VFS CHECK]\n");
    mp_printf(&mp_plat_print, "===================\n");
    mp_printf(&mp_plat_print, "Checking path: '%s'\n\n", path);
    
    // 1. Try stat()
    struct stat st;
    if (stat(path, &st) == 0) {
        mp_printf(&mp_plat_print, "✅ stat() SUCCESS\n");
        mp_printf(&mp_plat_print, "   Size: %ld bytes\n", (long)st.st_size);
        mp_printf(&mp_plat_print, "   Type: %s\n", S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "directory" : "other");
    } else {
        mp_printf(&mp_plat_print, "❌ stat() FAILED (errno: %d)\n", errno);
    }
    
    // 2. Try fopen()
    FILE *f = fopen(path, "rb");
    if (f) {
        mp_printf(&mp_plat_print, "✅ fopen() SUCCESS\n");
        fclose(f);
    } else {
        mp_printf(&mp_plat_print, "❌ fopen() FAILED (errno: %d)\n", errno);
    }
    
    // 3. If it's a directory, list contents
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        mp_printf(&mp_plat_print, "\nDirectory contents:\n");
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry;
            int count = 0;
            while ((entry = readdir(dir)) != NULL) {
                count++;
                mp_printf(&mp_plat_print, "  %d. %s\n", count, entry->d_name);
            }
            closedir(dir);
            mp_printf(&mp_plat_print, "Total: %d entries\n", count);
        } else {
            mp_printf(&mp_plat_print, "❌ opendir() FAILED\n");
        }
    }
    
    mp_printf(&mp_plat_print, "===================\n\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(webfiles_check_vfs_obj, webfiles_check_vfs);

// ------------------------------------------------------------------------
// Module definition
// ------------------------------------------------------------------------

// Module globals table
static const mp_rom_map_elem_t webfiles_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_webfiles) },
    { MP_ROM_QSTR(MP_QSTR_serve), MP_ROM_PTR(&webfiles_serve_obj) },
    { MP_ROM_QSTR(MP_QSTR_serve_file), MP_ROM_PTR(&webfiles_serve_file_obj) },
    { MP_ROM_QSTR(MP_QSTR_mount_www), MP_ROM_PTR(&webfiles_mount_www_obj) },
    { MP_ROM_QSTR(MP_QSTR_mount_www_rw), MP_ROM_PTR(&webfiles_mount_www_rw_obj) },
    { MP_ROM_QSTR(MP_QSTR_unmount_www), MP_ROM_PTR(&webfiles_unmount_www_obj) },
    { MP_ROM_QSTR(MP_QSTR_copy_to_www), MP_ROM_PTR(&webfiles_copy_to_www_obj) },
    { MP_ROM_QSTR(MP_QSTR_serve_www), MP_ROM_PTR(&webfiles_serve_www_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_vfs), MP_ROM_PTR(&webfiles_check_vfs_obj) },

    // MIME type constants
    { MP_ROM_QSTR(MP_QSTR_MIME_HTML), MP_ROM_QSTR(MP_QSTR_text_html) },
    { MP_ROM_QSTR(MP_QSTR_MIME_JS), MP_ROM_QSTR(MP_QSTR_application_javascript) },
    { MP_ROM_QSTR(MP_QSTR_MIME_CSS), MP_ROM_QSTR(MP_QSTR_text_css) },
    { MP_ROM_QSTR(MP_QSTR_MIME_JSON), MP_ROM_QSTR(MP_QSTR_application_json) },
    { MP_ROM_QSTR(MP_QSTR_TEXT), MP_ROM_QSTR(MP_QSTR_text_plain) },
    { MP_ROM_QSTR(MP_QSTR_MIME_BINARY), MP_ROM_QSTR(MP_QSTR_application_octet_stream) },
};

// Module dict
static MP_DEFINE_CONST_DICT(webfiles_module_globals, webfiles_module_globals_table);

// Register the module
const mp_obj_module_t webfiles_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&webfiles_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_webfiles, webfiles_module);

