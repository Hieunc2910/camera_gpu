/*
 * Copyright (c) 2018-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "deepstream_app.h"
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

#include <cuda_runtime.h>
#include <gst/gst.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <json-glib/json-glib.h>
#include <png.h>
#include <sys/time.h>
#include <curl/curl.h>

#include <glib.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>

static const char* get_color_format_str(NvBufSurfaceColorFormat format) {
    switch (format) {
        case NVBUF_COLOR_FORMAT_RGBA: return "RGBA";
        case NVBUF_COLOR_FORMAT_RGB: return "RGB";
        case NVBUF_COLOR_FORMAT_NV12: return "NV12";
        case NVBUF_COLOR_FORMAT_NV21: return "NV21";
        case NVBUF_COLOR_FORMAT_BGR: return "BGR";
        case NVBUF_COLOR_FORMAT_GRAY8: return "GRAY8";
        default: return "UNKNOWN";
    }
}
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


static void initialize_camera_info(AppCtx* appCtx);
const char* get_student_id_from_name(const char* name);
static char* encode_full_frame_base64(NvBufSurface* surface, NvDsFrameMeta* frame_meta);
/* Cấu trúc để theo dõi người đã xuất hiện */
typedef struct PersonTracker {
    char name[256];
    time_t last_log_time;
    gboolean is_present;
    struct PersonTracker* next;
} PersonTracker;
/* Danh sách linked list để theo dõi các người */
static PersonTracker* person_list = NULL;
static GMutex person_list_mutex;
// Thay đổi cấu trúc StudentInfo
#define MAX_STUDENTS 2000
typedef struct {
    int id;
    char full_name[128];
} StudentInfo;
StudentInfo students[MAX_STUDENTS];
int num_students = 0;

// Thêm cấu trúc để lưu thông tin camera
typedef struct {
    char ip_address[64];
    char mac_address[64];
    guint source_id;
} CameraInfo;

// Thêm array để lưu thông tin camera cho mỗi source
#define MAX_CAMERAS 200
static CameraInfo camera_info[MAX_CAMERAS];
static gboolean camera_info_initialized = FALSE;
// Hàm lấy địa chỉ IP từ URL RTSP
/* Thời gian timeout  */
#define PRESENCE_TIMEOUT 300
/* Thời gian giu log (3 ngày) */
#define LOG_RETENTION_DAYS 3
#define LOG_RETENTION_SECONDS (LOG_RETENTION_DAYS * 24 * 60 * 60)  // 3 days in seconds



/* Hàm tạo file log.json nếu chưa có */
static gboolean create_log_file_if_not_exists(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        /* File chưa tồn tại, tạo file mới với array JSON rỗng */
        file = fopen(filename, "w");
        if (file == NULL) {
            g_print("Error: Cannot create log file %s\n", filename);
            return FALSE;
        }
        fprintf(file, "[]");
        fclose(file);
        g_print("Created new log file: %s\n", filename);
    } else {
        fclose(file);
    }
    return TRUE;
}

/* Hàm tìm person trong danh sách */
static PersonTracker* find_person(const char* name) {
    PersonTracker* current = person_list;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/* Hàm thêm person mới vào danh sách */
static PersonTracker* add_person(const char* name) {
    PersonTracker* new_person = (PersonTracker*)malloc(sizeof(PersonTracker));
    if (new_person == NULL) {
        g_print("Error: Cannot allocate memory for new person\n");
        return NULL;
    }

    strncpy(new_person->name, name, sizeof(new_person->name) - 1);
    new_person->name[sizeof(new_person->name) - 1] = '\0';
    new_person->last_log_time  = time(NULL);
    new_person->is_present = TRUE;
    new_person->next = person_list;
    person_list = new_person;

    return new_person;
}

const char* get_student_id_from_name(const char* person_name) {
    static char id[16];
    if (!person_name) return NULL;
    const char* comma = strchr(person_name, ',');
    if (!comma) return NULL;
    size_t len = comma - person_name;
    if (len >= sizeof(id)) len = sizeof(id) - 1;
    strncpy(id, person_name, len);
    id[len] = '\0';
    return id;
}
/* Hàm ghi log recognition event với thông tin mới */
static void log_recognition_event(const char* person_name, NvBufSurface* surface,
                                  NvDsFrameMeta* frame_meta, NvDsObjectMeta* obj_meta)  {
    JsonParser* parser = NULL;
    JsonNode* root_node = NULL;
    JsonArray* root_array = NULL;
    JsonObject* new_entry = NULL;
    JsonGenerator* generator = NULL;
    char timestamp_str[64];
    time_t current_time;
    struct tm* time_info;
    gchar* json_string = NULL;
    GError* error = NULL;
    gboolean api_success = FALSE;

    g_mutex_lock(&person_list_mutex);

    current_time = time(NULL);
    PersonTracker* person = find_person(person_name);

    if (person == NULL) {
        // Người mới, thêm vào danh sách và ghi log
        person = add_person(person_name);
        if (person == NULL) {
            g_mutex_unlock(&person_list_mutex);
            return;
        }
        // Log lần đầu tiên
    } else if (person->is_present == FALSE) {
        // Người này đã vắng mặt đủ lâu, giờ xuất hiện lại => log
        person->is_present = TRUE;
        person->last_log_time = current_time;
        // Log lần quay lại
    } else {
        // Người này vẫn đang hiện diện, chỉ cập nhật last_log_time, KHÔNG log
        person->last_log_time = current_time;
        g_mutex_unlock(&person_list_mutex);
        return;
    }
    // Đủ điều kiện log (mới hoặc vừa quay lại sau khi vắng mặt)
    g_mutex_unlock(&person_list_mutex);

    /* Format timestamp */
    time_info = localtime(&current_time);
    strftime(timestamp_str, sizeof(timestamp_str), "%d-%m-%Y %H:%M:%S", time_info);

    // Lấy student_id thay vì sử dụng tên
    const char* student_id = get_student_id_from_name(person_name);
    if (!student_id) {
        student_id = "UNKNOWN";
    }

    // Lấy thông tin camera dựa trên source_id
    guint source_id = frame_meta ? frame_meta->pad_index : 0;
    const char* ip_address = "unknown";
    const char* mac_address = "unknown";

    if (source_id < MAX_CAMERAS && camera_info_initialized) {
        ip_address = camera_info[source_id].ip_address;
        mac_address = camera_info[source_id].mac_address;
    }

    char* full_frame_base64 = NULL;
    if (surface && frame_meta) {
        full_frame_base64 = encode_full_frame_base64(surface, frame_meta);
    }

    /* Thử gửi lên server trước */
    CURL *curl;
    CURLcode res;
    long response_code = 0;
    curl = curl_easy_init();
    if (curl) {
        // Tạo JSON body
        JsonBuilder *builder = json_builder_new();
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "student_id");
        json_builder_add_string_value(builder, student_id);
        json_builder_set_member_name(builder, "ip_address");
        json_builder_add_string_value(builder, ip_address);
        json_builder_set_member_name(builder, "mac_address");
        json_builder_add_string_value(builder, mac_address);
        json_builder_set_member_name(builder, "face_image");
        json_builder_add_string_value(builder, full_frame_base64 ? full_frame_base64 : "");
        json_builder_set_member_name(builder, "timestamp");
        json_builder_add_string_value(builder, timestamp_str);
        json_builder_end_object(builder);

        JsonGenerator *gen = json_generator_new();
        JsonNode *root = json_builder_get_root(builder);
        json_generator_set_root(gen, root);
        gchar *json_body = json_generator_to_data(gen, NULL);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, "https://topcam.ai.vn/apis/aiFaceRecognitionLogAPI");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // Timeout 10 giây

        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        if (res == CURLE_OK && response_code == 200) {
            api_success = TRUE;
            g_print("Successfully sent log to API for student_id: %s\n", student_id);

            /* Khi API thành công, thử gửi lại các log local đã tồn tại */
            static time_t last_retry_time = 0;
            time_t now = time(NULL);
            if (now - last_retry_time > 180) { // Chỉ retry mỗi 1 phút để tránh spam
                last_retry_time = now;

                // Kiểm tra xem có file log.json không và có log nào cần retry không
                if (access("log.json", F_OK) == 0) {
                    JsonParser* retry_parser = json_parser_new();
                    GError* retry_error = NULL;

                    if (json_parser_load_from_file(retry_parser, "log.json", &retry_error)) {
                        JsonNode* retry_root = json_parser_get_root(retry_parser);
                        if (JSON_NODE_HOLDS_ARRAY(retry_root)) {
                            JsonArray* retry_array = json_node_get_array(retry_root);
                            gint retry_count = json_array_get_length(retry_array);

                            if (retry_count > 0) {
                                g_print("Found %d pending logs, attempting to retry...\n", retry_count);

                                JsonArray* remaining_logs = json_array_new();
                                gboolean has_successful_retries = FALSE;

                                // Duyệt qua từng log và thử gửi lại
                                for (gint i = 0; i < retry_count; i++) {
                                    JsonNode* log_node = json_array_get_element(retry_array, i);
                                    if (!JSON_NODE_HOLDS_OBJECT(log_node)) continue;

                                    JsonObject* log_obj = json_node_get_object(log_node);
                                    const char* retry_student_id = json_object_get_string_member(log_obj, "student_id");
                                    const char* retry_timestamp = json_object_get_string_member(log_obj, "timestamp");
                                    const char* retry_ip = json_object_get_string_member(log_obj, "ip_address");
                                    const char* retry_mac = json_object_get_string_member(log_obj, "mac_address");
                                    const char* retry_image = json_object_get_string_member(log_obj, "face_image");

                                    if (!retry_student_id || !retry_timestamp || !retry_ip || !retry_mac) {
                                        continue; // Bỏ qua log thiếu thông tin
                                    }

                                    // Thử gửi log này
                                    CURL *retry_curl = curl_easy_init();
                                    if (retry_curl) {
                                        JsonBuilder *retry_builder = json_builder_new();
                                        json_builder_begin_object(retry_builder);
                                        json_builder_set_member_name(retry_builder, "student_id");
                                        json_builder_add_string_value(retry_builder, retry_student_id);
                                        json_builder_set_member_name(retry_builder, "ip_address");
                                        json_builder_add_string_value(retry_builder, retry_ip);
                                        json_builder_set_member_name(retry_builder, "mac_address");
                                        json_builder_add_string_value(retry_builder, retry_mac);
                                        json_builder_set_member_name(retry_builder, "face_image");
                                        json_builder_add_string_value(retry_builder, retry_image ? retry_image : "");
                                        json_builder_set_member_name(retry_builder, "timestamp");
                                        json_builder_add_string_value(retry_builder, retry_timestamp);
                                        json_builder_end_object(retry_builder);

                                        JsonGenerator *retry_gen = json_generator_new();
                                        JsonNode *retry_json_root = json_builder_get_root(retry_builder);
                                        json_generator_set_root(retry_gen, retry_json_root);
                                        gchar *retry_json_body = json_generator_to_data(retry_gen, NULL);

                                        struct curl_slist *retry_headers = NULL;
                                        retry_headers = curl_slist_append(retry_headers, "Content-Type: application/json");

                                        curl_easy_setopt(retry_curl, CURLOPT_URL, "https://topcam.ai.vn/apis/aiFaceRecognitionLogAPI");
                                        curl_easy_setopt(retry_curl, CURLOPT_POST, 1L);
                                        curl_easy_setopt(retry_curl, CURLOPT_HTTPHEADER, retry_headers);
                                        curl_easy_setopt(retry_curl, CURLOPT_POSTFIELDS, retry_json_body);
                                        curl_easy_setopt(retry_curl, CURLOPT_TIMEOUT, 5L); // Timeout ngắn hơn cho retry

                                        CURLcode retry_res = curl_easy_perform(retry_curl);
                                        long retry_response_code = 0;
                                        curl_easy_getinfo(retry_curl, CURLINFO_RESPONSE_CODE, &retry_response_code);

                                        if (retry_res == CURLE_OK && retry_response_code == 200) {
                                            has_successful_retries = TRUE;
                                            g_print("Successfully retried log for student_id: %s\n", retry_student_id);
                                        } else {
                                            // Gửi thất bại, giữ lại log này
                                            json_array_add_element(remaining_logs, json_node_copy(log_node));
                                        }

                                        curl_slist_free_all(retry_headers);
                                        curl_easy_cleanup(retry_curl);
                                        g_free(retry_json_body);
                                        json_node_free(retry_json_root);
                                        g_object_unref(retry_gen);
                                        g_object_unref(retry_builder);
                                    } else {
                                        // Không tạo được curl, giữ lại log
                                        json_array_add_element(remaining_logs, json_node_copy(log_node));
                                    }
                                }

                                // Cập nhật file log.json với những log chưa gửi được
                                if (has_successful_retries) {
                                    JsonNode* remaining_root = json_node_new(JSON_NODE_ARRAY);
                                    json_node_set_array(remaining_root, remaining_logs);

                                    JsonGenerator* retry_generator = json_generator_new();
                                    json_generator_set_root(retry_generator, remaining_root);
                                    json_generator_set_pretty(retry_generator, TRUE);

                                    GError* write_error = NULL;
                                    if (!json_generator_to_file(retry_generator, "log.json", &write_error)) {
                                        g_print("Error updating log.json after retry: %s\n", write_error->message);
                                        g_error_free(write_error);
                                    } else {
                                        gint remaining_count = json_array_get_length(remaining_logs);
                                        g_print("Retry completed: %d logs sent, %d logs remaining\n",
                                               retry_count - remaining_count, remaining_count);
                                    }

                                    json_node_free(remaining_root);
                                    g_object_unref(retry_generator);
                                } else {
                                    json_array_unref(remaining_logs);
                                }
                            }
                        }
                    } else {
                        g_error_free(retry_error);
                    }
                    g_object_unref(retry_parser);
                }
            }
        } else {
            g_print("Failed to send log to API: %s (HTTP: %ld)\n",
                   curl_easy_strerror(res), response_code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        g_free(json_body);
        json_node_free(root);
        g_object_unref(gen);
        g_object_unref(builder);
    }

    /* Nếu gửi API thành công, không cần lưu vào file local */
    if (api_success) {
        if (full_frame_base64) {
            free(full_frame_base64);
        }
        return;
    }

    /* Nếu gửi API thất bại, lưu vào file log.json local */
    g_print("Saving to local log due to API failure for student_id: %s\n", student_id);

    /* Tạo file log.json nếu chưa có */
    if (!create_log_file_if_not_exists("log.json")) {
        if (full_frame_base64) {
            free(full_frame_base64);
        }
        return;
    }

    /* Parse file JSON hiện tại */
    parser = json_parser_new();

    if (!json_parser_load_from_file(parser, "log.json", &error)) {
        g_print("Error parsing log.json: %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        if (full_frame_base64) {
            free(full_frame_base64);
        }
        return;
    }

    root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root_node)) {
        g_print("Error: log.json does not contain an array\n");
        g_object_unref(parser);
        if (full_frame_base64) {
            free(full_frame_base64);
        }
        return;
    }

    root_array = json_node_get_array(root_node);

    /* Tạo entry mới */
    new_entry = json_object_new();

    // Thêm các trường vào JSON
    json_object_set_string_member(new_entry, "student_id", student_id);
    json_object_set_string_member(new_entry, "timestamp", timestamp_str);
    json_object_set_string_member(new_entry, "ip_address", ip_address);
    json_object_set_string_member(new_entry, "mac_address", mac_address);

    if (full_frame_base64) {
        json_object_set_string_member(new_entry, "face_image", full_frame_base64);
        free(full_frame_base64);
    } else {
        json_object_set_string_member(new_entry, "face_image", "");
    }

    /* Thêm vào array */
    JsonNode* entry_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(entry_node, new_entry);
    json_array_add_element(root_array, entry_node);

    /* Ghi lại file */
    generator = json_generator_new();
    json_generator_set_root(generator, root_node);
    json_generator_set_pretty(generator, TRUE);

    if (!json_generator_to_file(generator, "log.json", &error)) {
        g_print("Error writing to log.json: %s\n", error->message);
        g_error_free(error);
    } else {
        g_print("Saved to local log: %s (ID: %s) at %s from IP: %s\n",
                person_name, student_id, timestamp_str, ip_address);
    }

    /* Cleanup */
    g_object_unref(generator);
    g_object_unref(parser);
}

/* Hàm xóa log cũ hơn 3 ngày */
static void cleanup_old_logs() {
    JsonParser* parser = NULL;
    JsonNode* root_node = NULL;
    JsonArray* root_array = NULL;
    JsonArray* new_array = NULL;
    JsonGenerator* generator = NULL;
    GError* error = NULL;
    time_t current_time = time(NULL);
    time_t cutoff_time = current_time - LOG_RETENTION_SECONDS;
    gint array_length, i;
    gboolean has_changes = FALSE;

    /* Kiểm tra xem file log.json có tồn tại không */
    if (access("log.json", F_OK) != 0) {
        return; /* File không tồn tại, không cần cleanup */
    }

    /* Parse file JSON hiện tại */
    parser = json_parser_new();

    if (!json_parser_load_from_file(parser, "log.json", &error)) {
        g_print("Error parsing log.json for cleanup: %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root_node)) {
        g_print("Error: log.json does not contain an array\n");
        g_object_unref(parser);
        return;
    }

    root_array = json_node_get_array(root_node);
    array_length = json_array_get_length(root_array);

    /* Tạo array mới để chứa các entry còn hợp lệ */
    new_array = json_array_new();

    /* Duyệt qua các entry và chỉ giữ lại những entry trong 3 ngày */
    for (i = 0; i < array_length; i++) {
        JsonNode* element_node = json_array_get_element(root_array, i);
        if (JSON_NODE_HOLDS_OBJECT(element_node)) {
            JsonObject* entry_obj = json_node_get_object(element_node);
            const gchar* timestamp_str = json_object_get_string_member(entry_obj, "timestamp");

            if (timestamp_str) {
                struct tm time_struct = {0};
                time_t entry_time;

                /* Parse timestamp string (format: "YYYY-MM-DD HH:MM:SS") */
                if (strptime(timestamp_str, "%Y-%m-%d %H:%M:%S", &time_struct)) {
                    entry_time = mktime(&time_struct);

                    /* Nếu entry còn trong thời hạn 3 ngày, giữ lại */
                    if (entry_time >= cutoff_time) {
                        json_array_add_element(new_array, json_node_copy(element_node));
                    } else {
                        has_changes = TRUE;
                        g_print("Removed old log entry: %s at %s\n",
                               json_object_get_string_member(entry_obj, "name"),
                               timestamp_str);
                    }
                } else {
                    /* Nếu không parse được timestamp, giữ lại để an toàn */
                    json_array_add_element(new_array, json_node_copy(element_node));
                }
            } else {
                /* Nếu không có timestamp, giữ lại để an toàn */
                json_array_add_element(new_array, json_node_copy(element_node));
            }
        }
    }

    /* Nếu có thay đổi, ghi lại file */
    if (has_changes) {
        JsonNode* new_root = json_node_new(JSON_NODE_ARRAY);
        json_node_set_array(new_root, new_array);

        generator = json_generator_new();
        json_generator_set_root(generator, new_root);
        json_generator_set_pretty(generator, TRUE);

        if (!json_generator_to_file(generator, "log.json", &error)) {
            g_print("Error writing cleaned log.json: %s\n", error->message);
            g_error_free(error);
        } else {
            g_print("Cleaned up old log entries (older than %d days)\n", LOG_RETENTION_DAYS);
        }

        g_object_unref(generator);
        json_node_free(new_root);
    } else {
        json_array_unref(new_array);
    }

    g_object_unref(parser);
}

/* Hàm cleanup để đánh dấu người không còn xuất hiện */
static gboolean cleanup_absent_persons(gpointer user_data) {
    time_t current_time = time(NULL);
    PersonTracker* current;

    g_mutex_lock(&person_list_mutex);
    current = person_list;

    while (current != NULL) {
        if (current->is_present && (current_time - current->last_log_time ) >= PRESENCE_TIMEOUT) {
            current->is_present = FALSE;
            g_print("Person %s marked as absent\n", current->name);
        }
        current = current->next;
    }

    g_mutex_unlock(&person_list_mutex);
    return TRUE; /* Tiếp tục timer */
}

/* Hàm cleanup định kỳ cho log và ảnh cũ */
static gboolean cleanup_old_data(gpointer user_data) {
    cleanup_old_logs();
    return TRUE; /* Tiếp tục timer */
}


void initialize_logging_system(AppCtx* appCtx) {
    if (!create_log_file_if_not_exists("log.json")) {
        return;
    }
    g_mutex_init(&person_list_mutex);

    // Khởi tạo thông tin camera
    initialize_camera_info(appCtx);


    /* Tạo timer để cleanup các person không còn xuất hiện (chạy mỗi 30 giây) */
    g_timeout_add_seconds(30, cleanup_absent_persons, NULL);

    /* Tạo timer để cleanup log và ảnh cũ (chạy mỗi 6 giờ) */
    g_timeout_add_seconds(6 * 60 * 60, cleanup_old_data, NULL);

    /* Chạy cleanup ngay lần đầu để xóa data cũ */
    cleanup_old_logs();

    g_print("Face recognition logging system initialized with %d-day retention\n", LOG_RETENTION_DAYS);
}


void cleanup_logging_system() {
    PersonTracker* current = person_list;
    PersonTracker* next;



    g_mutex_lock(&person_list_mutex);
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
    person_list = NULL;
    g_mutex_unlock(&person_list_mutex);

    g_mutex_clear(&person_list_mutex);
    g_print("Face recognition logging system cleaned up\n");
}


static gboolean extract_ip_from_rtsp_url(const char* rtsp_url, char* ip_buffer, size_t buffer_size) {
    if (!rtsp_url || !ip_buffer || buffer_size == 0) {
        return FALSE;
    }

    // Tìm "://" trong URL
    const char* start = strstr(rtsp_url, "://");
    if (!start) {
        return FALSE;
    }
    start += 3; // Bỏ qua "://"

    // Tìm '@' nếu có (user:pass@ip:port)
    const char* at_pos = strchr(start, '@');
    if (at_pos) {
        start = at_pos + 1;
    }

    // Tìm ':' hoặc '/' để kết thúc IP
    const char* end = strchr(start, ':');
    const char* slash = strchr(start, '/');

    if (!end || (slash && slash < end)) {
        end = slash;
    }

    if (!end) {
        end = start + strlen(start);
    }

    size_t ip_len = end - start;
    if (ip_len >= buffer_size) {
        return FALSE;
    }

    strncpy(ip_buffer, start, ip_len);
    ip_buffer[ip_len] = '\0';

    return TRUE;
}

// Hàm lấy MAC address từ IP (cần có quyền truy cập ARP table)
static gboolean get_mac_from_ip(const char* ip_address, char* mac_buffer, size_t buffer_size) {
    if (!ip_address || !mac_buffer || buffer_size < 18) {
        return FALSE;
    }

    char command[256];
    snprintf(command, sizeof(command), "arp -n %s 2>/dev/null | awk 'NR==2{print $3}'", ip_address);

    FILE* fp = popen(command, "r");
    if (!fp) {
        // Nếu không lấy được, tạo MAC giả từ IP
        snprintf(mac_buffer, buffer_size, "00:00:00:%02x:%02x:%02x",
                 (unsigned char)(rand() % 256),
                 (unsigned char)(rand() % 256),
                 (unsigned char)(rand() % 256));
        return FALSE;
    }

    if (fgets(mac_buffer, buffer_size, fp) != NULL) {
        // Xóa newline
        char* newline = strchr(mac_buffer, '\n');
        if (newline) *newline = '\0';

        // Kiểm tra định dạng MAC
        if (strlen(mac_buffer) >= 17) {
            pclose(fp);
            return TRUE;
        }
    }

    pclose(fp);

    // Nếu không lấy được MAC thực, tạo MAC giả
    snprintf(mac_buffer, buffer_size, "00:00:00:%02x:%02x:%02x",
             (unsigned char)(rand() % 256),
             (unsigned char)(rand() % 256),
             (unsigned char)(rand() % 256));
    return FALSE;
}

// Hàm khởi tạo thông tin camera
static void initialize_camera_info(AppCtx* appCtx) {
    if (camera_info_initialized) {
        return;
    }

    NvDsConfig* config = &appCtx->config;

    for (guint i = 0; i < config->num_source_sub_bins && i < MAX_CAMERAS; i++) {
        camera_info[i].source_id = i;

        // Lấy IP từ URI
        if (config->multi_source_config[i].uri) {
            if (!extract_ip_from_rtsp_url(config->multi_source_config[i].uri,
                                         camera_info[i].ip_address,
                                         sizeof(camera_info[i].ip_address))) {
                snprintf(camera_info[i].ip_address, sizeof(camera_info[i].ip_address),
                        "192.168.1.%d", (int)(100 + i));
            }
        } else {
            snprintf(camera_info[i].ip_address, sizeof(camera_info[i].ip_address),
                    "192.168.1.%d", (int)(100 + i));
        }

        // Lấy MAC address
        get_mac_from_ip(camera_info[i].ip_address,
                       camera_info[i].mac_address,
                       sizeof(camera_info[i].mac_address));

        g_print("Camera %d: IP=%s, MAC=%s\n", i,
                camera_info[i].ip_address,
                camera_info[i].mac_address);
    }

    camera_info_initialized = TRUE;
}

void load_labels(const char* filename) {
    if (!filename) {
        g_print("Error: Invalid filename provided to load_labels\n");
        return;
    }

    FILE* f = fopen(filename, "r");
    if (!f) {
        g_print("Error: Cannot open label file: %s\n", filename);
        return;
    }

    char line[256];
    num_students = 0;

    while (fgets(line, sizeof(line), f) && num_students < MAX_STUDENTS) {
        int id;
        char name[128], code[64];

        // Thêm kiểm tra định dạng dòng
        if (strlen(line) < 3) continue; // Bỏ qua dòng trống hoặc quá ngắn

        if (sscanf(line, "%d,%127[^,],%63s", &id, name, code) == 3) {
            students[num_students].id = id;
            strncpy(students[num_students].full_name, name, sizeof(students[num_students].full_name) - 1);
            students[num_students].full_name[sizeof(students[num_students].full_name) - 1] = '\0';
            num_students++;
        } else {
            g_print("Warning: Invalid line format in label file: %s\n", line);
        }
    }

    fclose(f);
    g_print("Loaded %d students from label file\n", num_students);

    if (num_students >= MAX_STUDENTS) {
        g_print("Warning: Reached maximum number of students (%d). Some entries may be ignored.\n", MAX_STUDENTS);
    }
}

#define MAX_DISPLAY_LEN 64
static guint batch_num = 0;
static guint demux_batch_num = 0;

GST_DEBUG_CATEGORY_EXTERN(NVDS_APP);

GQuark _dsmeta_quark;

#define CEIL(a, b) ((a + b - 1) / b)

/**
 * @brief  Add the (nvmsgconv->nvmsgbroker) sink-bin to the
 *         overall DS pipeline (if any configured) and link the same to
 *         common_elements.tee (This tee connects
 *         the common analytics path to Tiler/display-sink and
 *         to configured broker sink if any)
 *         NOTE: This API shall return TRUE if there are no
 *         broker sinks to add to pipeline
 *
 * @param  appCtx [IN]
 * @return TRUE if succussful; FALSE otherwise
 */
static gboolean add_and_link_broker_sink(AppCtx *appCtx);

/**
 * @brief  Checks if there are any [sink] groups
 *         configured for source_id=provided source_id
 *         NOTE: source_id key and this API is valid only when we
 *         disable [tiler] and thus use demuxer for individual
 *         stream out
 * @param  config [IN] The DS Pipeline configuration struct
 * @param  source_id [IN] Source ID for which a specific [sink]
 *         group is searched for
 */
static gboolean is_sink_available_for_source_id(NvDsConfig *config, guint source_id);

/**
 * callback function to receive messages from components
 * in the pipeline.
 */
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
    AppCtx *appCtx = (AppCtx *)data;
    GST_CAT_DEBUG(NVDS_APP, "Received message on bus: source %s, msg_type %s",
                  GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message));
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_INFO: {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        gst_message_parse_info(message, &error, &debuginfo);
        g_printerr("INFO from %s: %s\n", GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo) {
            g_printerr("Debug info: %s\n", debuginfo);
        }
        g_error_free(error);
        g_free(debuginfo);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        gst_message_parse_warning(message, &error, &debuginfo);
        g_printerr("WARNING from %s: %s\n", GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo) {
            g_printerr("Debug info: %s\n", debuginfo);
        }
        g_error_free(error);
        g_free(debuginfo);
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        const gchar *attempts_error =
            "Reconnection attempts exceeded for all sources or EOS received.";
        guint i = 0;
        gst_message_parse_error(message, &error, &debuginfo);

        if (strstr(error->message, attempts_error)) {
            g_print(
                "Reconnection attempt  exceeded or EOS received for all sources."
                " Exiting.\n");
            g_error_free(error);
            g_free(debuginfo);
            appCtx->return_value = 0;
            appCtx->quit = TRUE;
            return TRUE;
        }

        g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo) {
            g_printerr("Debug info: %s\n", debuginfo);
        }

        NvDsSrcParentBin *bin = &appCtx->pipeline.multi_src_bin;
        GstElement *msg_src_elem = (GstElement *)GST_MESSAGE_SRC(message);
        gboolean bin_found = FALSE;
        /* Find the source bin which generated the error. */
        while (msg_src_elem && !bin_found) {
            for (i = 0; i < bin->num_bins && !bin_found; i++) {
                if (bin->sub_bins[i].src_elem == msg_src_elem ||
                    bin->sub_bins[i].bin == msg_src_elem) {
                    bin_found = TRUE;
                    break;
                }
            }
            msg_src_elem = GST_ELEMENT_PARENT(msg_src_elem);
        }

        if ((i != bin->num_bins) &&
            (appCtx->config.multi_source_config[0].type == NV_DS_SOURCE_RTSP)) {
            // Error from one of RTSP source.
            NvDsSrcBin *subBin = &bin->sub_bins[i];

            if (!subBin->reconfiguring || g_strrstr(debuginfo, "500 (Internal Server Error)")) {
                subBin->reconfiguring = TRUE;
                g_timeout_add(0, reset_source_pipeline, subBin);
            }
            g_error_free(error);
            g_free(debuginfo);
            return TRUE;
        }

        if (appCtx->config.multi_source_config[0].type == NV_DS_SOURCE_CAMERA_V4L2) {
            if (g_strrstr(debuginfo, "reason not-negotiated (-4)")) {
                NVGSTDS_INFO_MSG_V(
                    "incorrect camera parameters provided, please "
                    "provide supported resolution and "
                    "frame rate\n");
            }

            if (g_strrstr(debuginfo, "Buffer pool activation failed")) {
                NVGSTDS_INFO_MSG_V("usb bandwidth might be saturated\n");
            }
        }

        g_error_free(error);
        g_free(debuginfo);
        appCtx->return_value = -1;
        appCtx->quit = TRUE;
        break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
        GstState oldstate, newstate;
        gst_message_parse_state_changed(message, &oldstate, &newstate, NULL);
        if (GST_ELEMENT(GST_MESSAGE_SRC(message)) == appCtx->pipeline.pipeline) {
            switch (newstate) {
            case GST_STATE_PLAYING:
                NVGSTDS_INFO_MSG_V("Pipeline running\n");
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline),
                                                  GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-playing");
                break;
            case GST_STATE_PAUSED:
                if (oldstate == GST_STATE_PLAYING) {
                    NVGSTDS_INFO_MSG_V("Pipeline paused\n");
                }
                break;
            case GST_STATE_READY:
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline),
                                                  GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-ready");
                if (oldstate == GST_STATE_NULL) {
                    NVGSTDS_INFO_MSG_V("Pipeline ready\n");
                } else {
                    NVGSTDS_INFO_MSG_V("Pipeline stopped\n");
                }
                break;
            case GST_STATE_NULL:
                g_mutex_lock(&appCtx->app_lock);
                g_cond_broadcast(&appCtx->app_cond);
                g_mutex_unlock(&appCtx->app_lock);
                break;
            default:
                break;
            }
        }
        break;
    }
    case GST_MESSAGE_EOS: {
        /*
         * In normal scenario, this would use g_main_loop_quit() to exit the
         * loop and release the resources. Since this application might be
         * running multiple pipelines through configuration files, it should wait
         * till all pipelines are done.
         */
        NVGSTDS_INFO_MSG_V("Received EOS. Exiting ...\n");
        appCtx->quit = TRUE;
        return FALSE;
        break;
    }
    default:
        break;
    }
    return TRUE;
}

/**
 * Function to dump bounding box data in kitti format. For this to work,
 * property "gie-kitti-output-dir" must be set in configuration file.
 * Data of different sources and frames is dumped in separate file.
 */
static void write_kitti_output(AppCtx *appCtx, NvDsBatchMeta *batch_meta)
{
    gchar bbox_file[1024] = {0};
    FILE *bbox_params_dump_file = NULL;

    if (!appCtx->config.bbox_dir_path)
        return;

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = l_frame->data;
        guint stream_id = frame_meta->pad_index;
        g_snprintf(bbox_file, sizeof(bbox_file) - 1, "%s/%02u_%03u_%06lu.txt",
                   appCtx->config.bbox_dir_path, appCtx->index, stream_id,
                   (gulong)frame_meta->frame_num);
        bbox_params_dump_file = fopen(bbox_file, "w");
        if (!bbox_params_dump_file)
            continue;

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;
            float left = obj->rect_params.left;
            float top = obj->rect_params.top;
            float right = left + obj->rect_params.width;
            float bottom = top + obj->rect_params.height;
            // Here confidence stores detection confidence, since dump gie output
            // is before tracker plugin
            float confidence = obj->confidence;
            fprintf(bbox_params_dump_file,
                    "%s 0.0 0 0.0 %f %f %f %f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %f\n", obj->obj_label,
                    left, top, right, bottom, confidence);
        }
        fclose(bbox_params_dump_file);
    }
}

/**
 * Function to dump past frame objs in kitti format.
 */
static void write_kitti_past_track_output(AppCtx *appCtx, NvDsBatchMeta *batch_meta)
{
    if (!appCtx->config.kitti_track_dir_path)
        return;

    // dump past frame tracked objects appending current frame objects
    gchar bbox_file[1024] = {0};
    FILE *bbox_params_dump_file = NULL;

    NvDsPastFrameObjBatch *pPastFrameObjBatch = NULL;
    NvDsUserMetaList *bmeta_list = NULL;
    NvDsUserMeta *user_meta = NULL;
    for (bmeta_list = batch_meta->batch_user_meta_list; bmeta_list != NULL;
         bmeta_list = bmeta_list->next) {
        user_meta = (NvDsUserMeta *)bmeta_list->data;
        if (user_meta && user_meta->base_meta.meta_type == NVDS_TRACKER_PAST_FRAME_META) {
            pPastFrameObjBatch = (NvDsPastFrameObjBatch *)(user_meta->user_meta_data);
            for (uint si = 0; si < pPastFrameObjBatch->numFilled; si++) {
                NvDsPastFrameObjStream *objStream = (pPastFrameObjBatch->list) + si;
                guint stream_id = (guint)(objStream->streamID);
                for (uint li = 0; li < objStream->numFilled; li++) {
                    NvDsPastFrameObjList *objList = (objStream->list) + li;
                    for (uint oi = 0; oi < objList->numObj; oi++) {
                        NvDsPastFrameObj *obj = (objList->list) + oi;
                        g_snprintf(bbox_file, sizeof(bbox_file) - 1, "%s/%02u_%03u_%06lu.txt",
                                   appCtx->config.kitti_track_dir_path, appCtx->index, stream_id,
                                   (gulong)obj->frameNum);

                        float left = obj->tBbox.left;
                        float right = left + obj->tBbox.width;
                        float top = obj->tBbox.top;
                        float bottom = top + obj->tBbox.height;
                        // Past frame object confidence given by tracker
                        float confidence = obj->confidence;
                        bbox_params_dump_file = fopen(bbox_file, "a");
                        if (!bbox_params_dump_file) {
                            continue;
                        }
                        fprintf(bbox_params_dump_file,
                                "%s %lu 0.0 0 0.0 %f %f %f %f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %f\n",
                                objList->objLabel, objList->uniqueId, left, top, right, bottom,
                                confidence);
                        fclose(bbox_params_dump_file);
                    }
                }
            }
        }
    }
}

/**
 * Function to dump bounding box data in kitti format with tracking ID added.
 * For this to work, property "kitti-track-output-dir" must be set in
 * configuration file. Data of different sources and frames is dumped in
 * separate file.
 */
static void write_kitti_track_output(AppCtx *appCtx, NvDsBatchMeta *batch_meta)
{
    gchar bbox_file[1024] = {0};
    FILE *bbox_params_dump_file = NULL;

    if (!appCtx->config.kitti_track_dir_path)
        return;

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = l_frame->data;
        guint stream_id = frame_meta->pad_index;
        g_snprintf(bbox_file, sizeof(bbox_file) - 1, "%s/%02u_%03u_%06lu.txt",
                   appCtx->config.kitti_track_dir_path, appCtx->index, stream_id,
                   (gulong)frame_meta->frame_num);
        bbox_params_dump_file = fopen(bbox_file, "w");
        if (!bbox_params_dump_file)
            continue;

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;
            float left = obj->tracker_bbox_info.org_bbox_coords.left;
            float top = obj->tracker_bbox_info.org_bbox_coords.top;
            float right = left + obj->tracker_bbox_info.org_bbox_coords.width;
            float bottom = top + obj->tracker_bbox_info.org_bbox_coords.height;
            // Here confidence stores tracker confidence value for tracker output
            float confidence = obj->tracker_confidence;
            guint64 id = obj->object_id;
            fprintf(bbox_params_dump_file,
                    "%s %lu 0.0 0 0.0 %f %f %f %f 0.0 0.0 0.0 0.0 0.0 0.0 0.0 %f\n", obj->obj_label,
                    id, left, top, right, bottom, confidence);
        }
        fclose(bbox_params_dump_file);
    }
}

static gint component_id_compare_func(gconstpointer a, gconstpointer b)
{
    NvDsClassifierMeta *cmetaa = (NvDsClassifierMeta *)a;
    NvDsClassifierMeta *cmetab = (NvDsClassifierMeta *)b;

    if (cmetaa->unique_component_id < cmetab->unique_component_id)
        return -1;
    if (cmetaa->unique_component_id > cmetab->unique_component_id)
        return 1;
    return 0;
}

/**
 * Function to process the attached metadata. This is just for demonstration
 * and can be removed if not required.
 * Here it demonstrates to use bounding boxes of different color and size for
 * different type / class of objects.
 * It also demonstrates how to join the different labels(PGIE + SGIEs)
 * of an object to form a single string.
 */
static void process_meta(AppCtx *appCtx, NvDsBatchMeta *batch_meta, GstBuffer *frame_buffer)
{
    // For single source always display text either with demuxer or with tiler
    if (!appCtx->config.tiled_display_config.enable || appCtx->config.num_source_sub_bins == 1) {
        appCtx->show_bbox_text = 1;
    }

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = l_frame->data;
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;

            gint class_index = obj->class_id;
            NvDsGieConfig *gie_config = NULL;
            gchar *str_ins_pos = NULL;

            if (obj->unique_component_id == (gint)appCtx->config.primary_gie_config.unique_id) {
                gie_config = &appCtx->config.primary_gie_config;
            } else {
                for (gint i = 0; i < (gint)appCtx->config.num_secondary_gie_sub_bins; i++) {
                    gie_config = &appCtx->config.secondary_gie_sub_bin_config[i];
                    if (obj->unique_component_id == (gint)gie_config->unique_id) {
                        break;
                    }
                    gie_config = NULL;
                }
            }
            g_free(obj->text_params.display_text);
            obj->text_params.display_text = NULL;

            if (gie_config != NULL) {
                if (g_hash_table_contains(gie_config->bbox_border_color_table,
                                          class_index + (gchar *)NULL)) {
                    obj->rect_params.border_color = *((NvOSD_ColorParams *)g_hash_table_lookup(
                        gie_config->bbox_border_color_table, class_index + (gchar *)NULL));
                } else {
                    obj->rect_params.border_color = gie_config->bbox_border_color;
                }
                obj->rect_params.border_width = appCtx->config.osd_config.border_width;

                if (g_hash_table_contains(gie_config->bbox_bg_color_table,
                                          class_index + (gchar *)NULL)) {
                    obj->rect_params.has_bg_color = 1;
                    obj->rect_params.bg_color = *((NvOSD_ColorParams *)g_hash_table_lookup(
                        gie_config->bbox_bg_color_table, class_index + (gchar *)NULL));
                } else {
                    obj->rect_params.has_bg_color = 0;
                }
            }

            if (!appCtx->show_bbox_text)
                continue;

            obj->text_params.x_offset = obj->rect_params.left;
            obj->text_params.y_offset = obj->rect_params.top - 30;
            obj->text_params.font_params.font_color = appCtx->config.osd_config.text_color;
            obj->text_params.font_params.font_size = appCtx->config.osd_config.text_size;
            obj->text_params.font_params.font_name = appCtx->config.osd_config.font;
            if (appCtx->config.osd_config.text_has_bg) {
                obj->text_params.set_bg_clr = 1;
                obj->text_params.text_bg_clr = appCtx->config.osd_config.text_bg_color;
            }

            obj->text_params.display_text = g_malloc(128);
            obj->text_params.display_text[0] = '\0';
            str_ins_pos = obj->text_params.display_text;

            if (obj->obj_label[0] != '\0')
                sprintf(str_ins_pos, "%s", obj->obj_label);
            str_ins_pos += strlen(str_ins_pos);

            if (obj->object_id != UNTRACKED_OBJECT_ID) {
                /** object_id is a 64-bit sequential value;
                 * but considering the display aesthetic,
                 * trimming to lower 32-bits */
                if (appCtx->config.tracker_config.display_tracking_id) {
                    guint64 const LOW_32_MASK = 0x00000000FFFFFFFF;
                    sprintf(str_ins_pos, " %lu", (obj->object_id & LOW_32_MASK));
                    str_ins_pos += strlen(str_ins_pos);
                }
            }

            obj->classifier_meta_list =
                g_list_sort(obj->classifier_meta_list, component_id_compare_func);

            for (NvDsMetaList *l_class = obj->classifier_meta_list; l_class != NULL;
                 l_class = l_class->next) {
                NvDsClassifierMeta *cmeta = (NvDsClassifierMeta *)l_class->data;
                for (NvDsMetaList *l_label = cmeta->label_info_list; l_label != NULL;
                     l_label = l_label->next) {
                    NvDsLabelInfo *label = (NvDsLabelInfo *)l_label->data;
                    const char *person_name = NULL;
                    if (label->pResult_label) {
                        sprintf(str_ins_pos, " %s", label->pResult_label);
                        person_name = label->pResult_label;
                    } else if (label->result_label[0] != '\0') {
                        sprintf(str_ins_pos, " %s", label->result_label);
                        person_name = label->result_label;
                    }

                    str_ins_pos += strlen(str_ins_pos);

                    }
                }
            }
        }
    }


/**
 * Function which processes the inferred buffer and its metadata.
 * It also gives opportunity to attach application specific
 * metadata (e.g. clock, analytics output etc.).
 */
static void process_buffer(GstBuffer *buf, AppCtx *appCtx, guint index)
{
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta) {
        NVGSTDS_WARN_MSG_V("Batch meta not found for buffer %p", buf);
        return;
    }
    process_meta(appCtx, batch_meta, buf);
    // NvDsInstanceData *data = &appCtx->instance_data[index];
    // guint i;

    //  data->frame_num++;

    /* Opportunity to modify the processed metadata or do analytics based on
     * type of object e.g. maintaining count of particular type of car.
     */
    if (appCtx->all_bbox_generated_cb) {
        appCtx->all_bbox_generated_cb(appCtx, buf, batch_meta, index);
    }
    // data->bbox_list_size = 0;

    /*
     * callback to attach application specific additional metadata.
     */
    if (appCtx->overlay_graphics_cb) {
        appCtx->overlay_graphics_cb(appCtx, buf, batch_meta, index);
    }
}

/**
 * Buffer probe function to get the results of primary infer.
 * Here it demonstrates the use by dumping bounding box coordinates in
 * kitti format.
 */
static GstPadProbeReturn gie_primary_processing_done_buf_prob(GstPad *pad,
                                                              GstPadProbeInfo *info,
                                                              gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *)info->data;
    AppCtx *appCtx = (AppCtx *)u_data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta) {
        NVGSTDS_WARN_MSG_V("Batch meta not found for buffer %p", buf);
        return GST_PAD_PROBE_OK;
    }

    write_kitti_output(appCtx, batch_meta);

    return GST_PAD_PROBE_OK;
}

/**
 * Probe function to get results after all inferences(Primary + Secondary)
 * are done. This will be just before OSD or sink (in case OSD is disabled).
 */
static GstPadProbeReturn gie_processing_done_buf_prob(GstPad *pad,
                                                      GstPadProbeInfo *info,
                                                      gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsInstanceBin *bin = (NvDsInstanceBin *)u_data;
    guint index = bin->index;
    AppCtx *appCtx = bin->appCtx;

    if (gst_buffer_is_writable(buf))
        process_buffer(buf, appCtx, index);

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta) return GST_PAD_PROBE_OK;

    GstMapInfo in_map_info;
    NvBufSurface *surface = NULL;
    gboolean buffer_mapped = FALSE;

    if (gst_buffer_map(buf, &in_map_info, GST_MAP_READ)) {
        surface = (NvBufSurface *)in_map_info.data;
        buffer_mapped = TRUE;

    } else {
        g_print("[ERROR] Failed to map buffer\n");
    }

    // ✅ PROCESS TẤT CẢ NGƯỜI TRONG BATCH - NHƯNG TRÁNH TRÙNG LẶP
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)l_frame->data;

        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;

            for (NvDsMetaList *l_class = obj->classifier_meta_list; l_class != NULL; l_class = l_class->next) {
                NvDsClassifierMeta *cmeta = (NvDsClassifierMeta *)l_class->data;

                for (NvDsMetaList *l_label = cmeta->label_info_list; l_label != NULL; l_label = l_label->next) {
                    NvDsLabelInfo *label = (NvDsLabelInfo *)l_label->data;

                    const char *person_name = NULL;
                    if (label->pResult_label) {
                        person_name = label->pResult_label;
                    } else if (label->result_label[0] != '\0') {
                        person_name = label->result_label;
                    }

                    // ✅ CHỈ LOG KHI CÓ TÊN NGƯỜI VÀ SURFACE HỢP LỆ
                    if (person_name && strlen(person_name) > 0 && surface) {
                        log_recognition_event(person_name, surface, frame_meta, obj);
                    }
                }
            }
        }
    }

    // ✅ UNMAP BUFFER
    if (buffer_mapped) {
        gst_buffer_unmap(buf, &in_map_info);
    }

    return GST_PAD_PROBE_OK;
}
/**
 * Buffer probe function after tracker.
 */
static GstPadProbeReturn analytics_done_buf_prob(GstPad *pad,
                                                 GstPadProbeInfo *info,
                                                 gpointer u_data)
{
    NvDsInstanceBin *bin = (NvDsInstanceBin *)u_data;
    guint index = bin->index;
    AppCtx *appCtx = bin->appCtx;
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    if (!batch_meta) {
        return GST_PAD_PROBE_OK;
    }

    write_kitti_track_output(appCtx, batch_meta);
    if (appCtx->config.tracker_config.enable_past_frame) {
        write_kitti_past_track_output(appCtx, batch_meta);
    }
    if (appCtx->bbox_generated_post_analytics_cb) {
        appCtx->bbox_generated_post_analytics_cb(appCtx, buf, batch_meta, index);
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn latency_measurement_buf_prob(GstPad *pad,
                                                      GstPadProbeInfo *info,
                                                      gpointer u_data)
{
    AppCtx *appCtx = (AppCtx *)u_data;
    guint i = 0, num_sources_in_batch = 0;
    if (nvds_enable_latency_measurement) {
        GstBuffer *buf = (GstBuffer *)info->data;
        NvDsFrameLatencyInfo *latency_info = NULL;
        g_mutex_lock(&appCtx->latency_lock);
        latency_info = appCtx->latency_info;
        g_print("\n************BATCH-NUM = %d**************\n", batch_num);
        num_sources_in_batch = nvds_measure_buffer_latency(buf, latency_info);

        for (i = 0; i < num_sources_in_batch; i++) {
            g_print("Source id = %d Frame_num = %d Frame latency = %lf (ms) \n",
                    latency_info[i].source_id, latency_info[i].frame_num, latency_info[i].latency);
        }
        g_mutex_unlock(&appCtx->latency_lock);
        batch_num++;
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn demux_latency_measurement_buf_prob(GstPad *pad,
                                                            GstPadProbeInfo *info,
                                                            gpointer u_data)
{
    AppCtx *appCtx = (AppCtx *)u_data;
    guint i = 0, num_sources_in_batch = 0;
    if (nvds_enable_latency_measurement) {
        GstBuffer *buf = (GstBuffer *)info->data;
        NvDsFrameLatencyInfo *latency_info = NULL;
        g_mutex_lock(&appCtx->latency_lock);
        latency_info = appCtx->latency_info;
        g_print("\n************DEMUX BATCH-NUM = %d**************\n", demux_batch_num);
        num_sources_in_batch = nvds_measure_buffer_latency(buf, latency_info);

        for (i = 0; i < num_sources_in_batch; i++) {
            g_print("Source id = %d Frame_num = %d Frame latency = %lf (ms) \n",
                    latency_info[i].source_id, latency_info[i].frame_num, latency_info[i].latency);
        }
        g_mutex_unlock(&appCtx->latency_lock);
        demux_batch_num++;
    }

    return GST_PAD_PROBE_OK;
}

static gboolean add_and_link_broker_sink(AppCtx *appCtx)
{
    NvDsConfig *config = &appCtx->config;
    /** Only first instance_bin broker sink
     * employed as there's only one analytics path for N sources
     * NOTE: There shall be only one [sink] group
     * with type=6 (NV_DS_SINK_MSG_CONV_BROKER)
     * a) Multiple of them does not make sense as we have only
     * one analytics pipe generating the data for broker sink
     * b) If Multiple broker sinks are configured by the user
     * in config file, only the first in the order of
     * appearance will be considered
     * and others shall be ignored
     * c) Ideally it should be documented (or obvious) that:
     * multiple [sink] groups with type=6 (NV_DS_SINK_MSG_CONV_BROKER)
     * is invalid
     */
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.instance_bins[0];
    NvDsPipeline *pipeline = &appCtx->pipeline;

    for (guint i = 0; i < config->num_sink_sub_bins; i++) {
        if (config->sink_bin_sub_bin_config[i].type == NV_DS_SINK_MSG_CONV_BROKER) {
            /////////////////
            /* Start Custom */
            /////////////////
            if (!config->sink_bin_sub_bin_config[i].msg_conv_broker_on_demux) {
                if (!pipeline->dxexample_tee) {
                    NVGSTDS_ERR_MSG_V(
                        "%s failed; broker added without analytics; check config file\n", __func__);
                    return FALSE;
                }
                /** add the broker sink bin to pipeline */
                if (!gst_bin_add(GST_BIN(pipeline->pipeline),
                                 instance_bin->sink_bin.sub_bins[i].bin)) {
                    return FALSE;
                }
                /** link the broker sink bin to the common_elements tee
                 * (The tee after nvinfer -> tracker (optional) -> sgies (optional)
                 * block) -> dsexample */
                if (!link_element_to_tee_src_pad(pipeline->dxexample_tee,
                                                 instance_bin->sink_bin.sub_bins[i].bin)) {
                    return FALSE;
                }
            }
            ////////////////
            /* End Custom */
            ////////////////
        }
    }
    return TRUE;
}

static gboolean create_demux_pipeline(AppCtx *appCtx, guint index)
{
    gboolean ret = FALSE;
    NvDsConfig *config = &appCtx->config;
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.demux_instance_bins[index];
    GstElement *last_elem;
    gchar elem_name[32];

    instance_bin->index = index;
    instance_bin->appCtx = appCtx;

    g_snprintf(elem_name, 32, "processing_demux_bin_%d", index);
    instance_bin->bin = gst_bin_new(elem_name);

    if (!create_demux_sink_bin(config->num_sink_sub_bins, config->sink_bin_sub_bin_config,
                               &instance_bin->demux_sink_bin,
                               config->sink_bin_sub_bin_config[index].source_id)) {
        goto done;
    }

    gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->demux_sink_bin.bin);
    last_elem = instance_bin->demux_sink_bin.bin;

    /////////////////
    /* Start Custom */
    /////////////////
    if (config->segvisual_config.enable) {
        if (!create_segvisual_bin(&config->segvisual_config, &instance_bin->segvisual_bin)) {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->segvisual_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->segvisual_bin.bin, last_elem);

        last_elem = instance_bin->segvisual_bin.bin;
    }
    ////////////////
    /* End Custom */
    ////////////////

    if (config->osd_config.enable) {
        if (!create_osd_bin(&config->osd_config, &instance_bin->osd_bin)) {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->osd_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->osd_bin.bin, last_elem);

        last_elem = instance_bin->osd_bin.bin;
    }

    NVGSTDS_BIN_ADD_GHOST_PAD(instance_bin->bin, last_elem, "sink");
    if (config->osd_config.enable) {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id, instance_bin->osd_bin.nvosd,
                               "src", gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               instance_bin);
    } else {
        NVGSTDS_ELEM_ADD_PROBE(
            instance_bin->all_bbox_buffer_probe_id, instance_bin->demux_sink_bin.bin, "sink",
            gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, instance_bin);
    }

    ret = TRUE;
done:
    if (!ret) {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to add components to pipeline which are dependent on number
 * of streams. These components work on single buffer. If tiling is being
 * used then single instance will be created otherwise < N > such instances
 * will be created for < N > streams
 */
static gboolean create_processing_instance(AppCtx *appCtx, guint index)
{
    gboolean ret = FALSE;
    NvDsConfig *config = &appCtx->config;
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.instance_bins[index];
    GstElement *last_elem;
    gchar elem_name[32];

    instance_bin->index = index;
    instance_bin->appCtx = appCtx;

    g_snprintf(elem_name, 32, "processing_bin_%d", index);
    instance_bin->bin = gst_bin_new(elem_name);

    if (!create_sink_bin(config->num_sink_sub_bins, config->sink_bin_sub_bin_config,
                         &instance_bin->sink_bin, index)) {
        goto done;
    }

    gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->sink_bin.bin);
    last_elem = instance_bin->sink_bin.bin;

    /////////////////
    /* Start Custom */
    /////////////////
    if (config->segvisual_config.enable) {
        if (!create_segvisual_bin(&config->segvisual_config, &instance_bin->segvisual_bin)) {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->segvisual_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->segvisual_bin.bin, last_elem);

        last_elem = instance_bin->segvisual_bin.bin;
    }
    ////////////////
    /* End Custom */
    ////////////////

    if (config->osd_config.enable) {
        if (!create_osd_bin(&config->osd_config, &instance_bin->osd_bin)) {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->osd_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->osd_bin.bin, last_elem);

        last_elem = instance_bin->osd_bin.bin;
    }

    /////////////////
    /* Start Custom */
    /////////////////
    instance_bin->msg_conv_broker_tee =
        gst_element_factory_make(NVDS_ELEM_TEE, "msg_conv_broker_tee");
    if (!instance_bin->msg_conv_broker_tee) {
        NVGSTDS_ERR_MSG_V("Failed to create element 'msg_conv_broker_tee'");
        goto done;
    }
    gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->msg_conv_broker_tee);

    if (!link_element_to_tee_src_pad(instance_bin->msg_conv_broker_tee, last_elem)) {
        goto done;
    }

    for (guint i = 0; i < config->num_sink_sub_bins; i++) {
        if (!config->sink_bin_sub_bin_config[i].enable) {
            continue;
        }
        if (config->sink_bin_sub_bin_config[i].source_id != index) {
            continue;
        }
        if (config->sink_bin_sub_bin_config[i].link_to_demux) {
            continue;
        }

        if (config->sink_bin_sub_bin_config[i].type == NV_DS_SINK_MSG_CONV_BROKER) {
            if (config->sink_bin_sub_bin_config[i].msg_conv_broker_on_demux) {
                /** add the broker sink bin to tee */
                if (!gst_bin_add(GST_BIN(instance_bin->bin),
                                 instance_bin->sink_bin.sub_bins[i].bin)) {
                    goto done;
                }

                /** link the broker sink bin to the common_elements tee
                 * (The tee after nvinfer -> tracker (optional) -> sgies (optional)
                 * block) */
                if (!link_element_to_tee_src_pad(instance_bin->msg_conv_broker_tee,
                                                 instance_bin->sink_bin.sub_bins[i].bin)) {
                    goto done;
                }
            }
        }
    }

    last_elem = instance_bin->msg_conv_broker_tee;
    ////////////////
    /* End Custom */
    ////////////////

    NVGSTDS_BIN_ADD_GHOST_PAD(instance_bin->bin, last_elem, "sink");
    if (config->osd_config.enable) {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id, instance_bin->osd_bin.nvosd,
                               "src", gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               instance_bin);
    } else {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id, instance_bin->sink_bin.bin,
                               "sink", gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               instance_bin);
    }

    ret = TRUE;
done:
    if (!ret) {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to create common elements(Primary infer, tracker, secondary infer)
 * of the pipeline. These components operate on muxed data from all the
 * streams. So they are independent of number of streams in the pipeline.
 */
static gboolean create_common_elements(NvDsConfig *config,
                                       NvDsPipeline *pipeline,
                                       GstElement **sink_elem,
                                       GstElement **src_elem,
                                       bbox_generated_callback bbox_generated_post_analytics_cb)
{
    gboolean ret = FALSE;
    *sink_elem = *src_elem = NULL;

    if (config->primary_gie_config.enable) {
        if (config->num_secondary_gie_sub_bins > 0) {
            if (!create_secondary_gie_bin(config->num_secondary_gie_sub_bins,
                                          config->primary_gie_config.unique_id,
                                          config->secondary_gie_sub_bin_config,
                                          &pipeline->common_elements.secondary_gie_bin)) {
                goto done;
            }
            gst_bin_add(GST_BIN(pipeline->pipeline),
                        pipeline->common_elements.secondary_gie_bin.bin);
            if (!*src_elem) {
                *src_elem = pipeline->common_elements.secondary_gie_bin.bin;
            }
            if (*sink_elem) {
                NVGSTDS_LINK_ELEMENT(pipeline->common_elements.secondary_gie_bin.bin, *sink_elem);
            }
            *sink_elem = pipeline->common_elements.secondary_gie_bin.bin;
        }
    }

    if (config->dsanalytics_config.enable) {
        if (!create_dsanalytics_bin(&config->dsanalytics_config,
                                    &pipeline->common_elements.dsanalytics_bin)) {
            g_print("creating dsanalytics bin failed\n");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.dsanalytics_bin.bin);

        if (!*src_elem) {
            *src_elem = pipeline->common_elements.dsanalytics_bin.bin;
        }
        if (*sink_elem) {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.dsanalytics_bin.bin, *sink_elem);
        }
        *sink_elem = pipeline->common_elements.dsanalytics_bin.bin;
    }

    if (config->tracker_config.enable) {
        if (!create_tracking_bin(&config->tracker_config, &pipeline->common_elements.tracker_bin)) {
            g_print("creating tracker bin failed\n");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.tracker_bin.bin);
        if (!*src_elem) {
            *src_elem = pipeline->common_elements.tracker_bin.bin;
        }
        if (*sink_elem) {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.tracker_bin.bin, *sink_elem);
        }
        *sink_elem = pipeline->common_elements.tracker_bin.bin;
    }

    if (config->primary_gie_config.enable) {
        if (!create_primary_gie_bin(&config->primary_gie_config,
                                    &pipeline->common_elements.primary_gie_bin)) {
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.primary_gie_bin.bin);
        if (*sink_elem) {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.primary_gie_bin.bin, *sink_elem);
        }
        *sink_elem = pipeline->common_elements.primary_gie_bin.bin;
        if (!*src_elem) {
            *src_elem = pipeline->common_elements.primary_gie_bin.bin;
        }
        NVGSTDS_ELEM_ADD_PROBE(pipeline->common_elements.primary_bbox_buffer_probe_id,
                               pipeline->common_elements.primary_gie_bin.bin, "src",
                               gie_primary_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               pipeline->common_elements.appCtx);
    }

    if (config->preprocess_config.enable) {
        if (!create_preprocess_bin(&config->preprocess_config,
                                   &pipeline->common_elements.preprocess_bin)) {
            g_print("creating preprocess bin failed\n");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.preprocess_bin.bin);

        if (!*src_elem) {
            *src_elem = pipeline->common_elements.preprocess_bin.bin;
        }
        if (*sink_elem) {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.preprocess_bin.bin, *sink_elem);
        }

        *sink_elem = pipeline->common_elements.preprocess_bin.bin;
    }

    if (*src_elem) {
        NVGSTDS_ELEM_ADD_PROBE(pipeline->common_elements.primary_bbox_buffer_probe_id, *src_elem,
                               "src", analytics_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               &pipeline->common_elements);

        /* Add common message converter */
        if (config->msg_conv_config.enable) {
            NvDsSinkMsgConvBrokerConfig *convConfig = &config->msg_conv_config;
            pipeline->common_elements.msg_conv =
                gst_element_factory_make(NVDS_ELEM_MSG_CONV, "common_msg_conv");
            if (!pipeline->common_elements.msg_conv) {
                NVGSTDS_ERR_MSG_V("Failed to create element 'common_msg_conv'");
                goto done;
            }

            g_object_set(G_OBJECT(pipeline->common_elements.msg_conv), "config",
                         convConfig->config_file_path, "msg2p-lib",
                         (convConfig->conv_msg2p_lib ? convConfig->conv_msg2p_lib : "null"),
                         "payload-type", convConfig->conv_payload_type, "comp-id",
                         convConfig->conv_comp_id, "debug-payload-dir",
                         convConfig->debug_payload_dir, "multiple-payloads",
                         convConfig->multiple_payloads, NULL);

            gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.msg_conv);

            NVGSTDS_LINK_ELEMENT(*src_elem, pipeline->common_elements.msg_conv);
            *src_elem = pipeline->common_elements.msg_conv;
        }
        pipeline->common_elements.tee =
            gst_element_factory_make(NVDS_ELEM_TEE, "common_analytics_tee");
        if (!pipeline->common_elements.tee) {
            NVGSTDS_ERR_MSG_V("Failed to create element 'common_analytics_tee'");
            goto done;
        }

        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->common_elements.tee);

        NVGSTDS_LINK_ELEMENT(*src_elem, pipeline->common_elements.tee);
        *src_elem = pipeline->common_elements.tee;
    }

    ret = TRUE;
done:
    return ret;
}

static gboolean is_sink_available_for_source_id(NvDsConfig *config, guint source_id)
{
    for (guint j = 0; j < config->num_sink_sub_bins; j++) {
        if (config->sink_bin_sub_bin_config[j].enable &&
            config->sink_bin_sub_bin_config[j].source_id == source_id &&
            config->sink_bin_sub_bin_config[j].link_to_demux == FALSE) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Main function to create the pipeline.
 */
gboolean create_pipeline(AppCtx *appCtx,
                         bbox_generated_callback bbox_generated_post_analytics_cb,
                         bbox_generated_callback all_bbox_generated_cb,
                         perf_callback perf_cb,
                         overlay_graphics_callback overlay_graphics_cb)
{
    gboolean ret = FALSE;
    NvDsPipeline *pipeline = &appCtx->pipeline;
    NvDsConfig *config = &appCtx->config;
    GstBus *bus;
    GstElement *last_elem;
    GstElement *tmp_elem1;
    GstElement *tmp_elem2;
    guint i;
    GstPad *fps_pad = NULL;
    gulong latency_probe_id;

    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);

    appCtx->all_bbox_generated_cb = all_bbox_generated_cb;
    appCtx->bbox_generated_post_analytics_cb = bbox_generated_post_analytics_cb;
    appCtx->overlay_graphics_cb = overlay_graphics_cb;

    if (config->osd_config.num_out_buffers < 8) {
        config->osd_config.num_out_buffers = 8;
    }

    pipeline->pipeline = gst_pipeline_new("pipeline");
    if (!pipeline->pipeline) {
        NVGSTDS_ERR_MSG_V("Failed to create pipeline");
        goto done;
    }
    // Khởi tạo hệ thống logging nhận diện khuôn mặt
    initialize_logging_system(appCtx);
    g_print("Face recognition logging system initialized\n");

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline->pipeline));
    pipeline->bus_id = gst_bus_add_watch(bus, bus_callback, appCtx);
    gst_object_unref(bus);

    if (config->file_loop) {
        /* Let each source bin know it needs to loop. */
        guint i;
        for (i = 0; i < config->num_source_sub_bins; i++)
            config->multi_source_config[i].loop = TRUE;
    }

    for (guint i = 0; i < config->num_sink_sub_bins; i++) {
        NvDsSinkSubBinConfig *sink_config = &config->sink_bin_sub_bin_config[i];
        switch (sink_config->type) {
        case NV_DS_SINK_FAKE:
        case NV_DS_SINK_RENDER_EGL:
        case NV_DS_SINK_RENDER_OVERLAY:
            /* Set the "qos" property of sink, if not explicitly specified in the
       config. */
            if (!sink_config->render_config.qos_value_specified) {
                sink_config->render_config.qos = FALSE;
            }
        default:
            break;
        }
    }
    /*
     * Add muxer and < N > source components to the pipeline based
     * on the settings in configuration file.
     */
    if (!create_multi_source_bin(config->num_source_sub_bins, config->multi_source_config,
                                 &pipeline->multi_src_bin))
        goto done;
    gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->multi_src_bin.bin);

    if (config->streammux_config.is_parsed) {
        if (!set_streammux_properties(&config->streammux_config,
                                      pipeline->multi_src_bin.streammux)) {
            NVGSTDS_WARN_MSG_V("Failed to set streammux properties");
        }
    }

    if (appCtx->latency_info == NULL) {
        appCtx->latency_info = (NvDsFrameLatencyInfo *)calloc(
            1, config->streammux_config.batch_size * sizeof(NvDsFrameLatencyInfo));
    }

    /////////////////
    /* Start Custom */
    /////////////////
    if (config->tiled_display_config.enable != NV_DS_TILED_DISPLAY_DISABLE) {
        /** a tee after the tiler which shall be connected to sink(s) */
        pipeline->tiler_tee = gst_element_factory_make(NVDS_ELEM_TEE, "tiler_tee");
        if (!pipeline->tiler_tee) {
            NVGSTDS_ERR_MSG_V("Failed to create element 'tiler_tee'");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->tiler_tee);
    }
    ////////////////
    /* End Custom */
    ////////////////

    /** Tiler + Demux in Parallel Use-Case */
    if (config->tiled_display_config.enable == NV_DS_TILED_DISPLAY_ENABLE_WITH_PARALLEL_DEMUX) {
        pipeline->demuxer = gst_element_factory_make(NVDS_ELEM_STREAM_DEMUX, "demuxer");
        if (!pipeline->demuxer) {
            NVGSTDS_ERR_MSG_V("Failed to create element 'demuxer'");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->demuxer);

        /** NOTE:
         * demux output is supported for only one source
         * If multiple [sink] groups are configured with
         * link_to_demux=1, only the first [sink]
         * shall be constructed for all occurences of
         * [sink] groups with link_to_demux=1
         */
        {
            gchar pad_name[16];
            GstPad *demux_src_pad;

            i = 0;
            if (!create_demux_pipeline(appCtx, i)) {
                goto done;
            }

            for (i = 0; i < config->num_sink_sub_bins; i++) {
                if (config->sink_bin_sub_bin_config[i].link_to_demux == TRUE) {
                    g_snprintf(pad_name, 16, "src_%02d",
                               config->sink_bin_sub_bin_config[i].source_id);
                    break;
                }
            }

            if (i >= config->num_sink_sub_bins) {
                g_print(
                    "\n\nError : sink for demux (use link-to-demux-only property) "
                    "is not provided "
                    "in the config file\n\n");
                goto done;
            }

            i = 0;

            gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->demux_instance_bins[i].bin);

            demux_src_pad = gst_element_get_request_pad(pipeline->demuxer, pad_name);
            NVGSTDS_LINK_ELEMENT_FULL(pipeline->demuxer, pad_name,
                                      pipeline->demux_instance_bins[i].bin, "src");
            gst_object_unref(demux_src_pad);

            NVGSTDS_ELEM_ADD_PROBE(
                latency_probe_id, appCtx->pipeline.demux_instance_bins[i].demux_sink_bin.bin,
                "sink", demux_latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, appCtx);
            latency_probe_id = latency_probe_id;
        }

        last_elem = pipeline->demuxer;
        link_element_to_tee_src_pad(pipeline->tiler_tee, last_elem);
        last_elem = pipeline->tiler_tee;
    }

    if (config->tiled_display_config.enable) {
        /* Tiler will generate a single composited buffer for all sources. So need
         * to create only one processing instance. */
        if (!create_processing_instance(appCtx, 0)) {
            goto done;
        }
        // create and add tiling component to pipeline.
        if (config->tiled_display_config.columns * config->tiled_display_config.rows <
            config->num_source_sub_bins) {
            if (config->tiled_display_config.columns == 0) {
                config->tiled_display_config.columns =
                    (guint)(sqrt(config->num_source_sub_bins) + 0.5);
            }
            config->tiled_display_config.rows = (guint)ceil(1.0 * config->num_source_sub_bins /
                                                            config->tiled_display_config.columns);
            NVGSTDS_WARN_MSG_V(
                "Num of Tiles less than number of sources, readjusting to "
                "%u rows, %u columns",
                config->tiled_display_config.rows, config->tiled_display_config.columns);
        }

        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->instance_bins[0].bin);
        last_elem = pipeline->instance_bins[0].bin;

        if (!create_tiled_display_bin(&config->tiled_display_config,
                                      &pipeline->tiled_display_bin)) {
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->tiled_display_bin.bin);
        NVGSTDS_LINK_ELEMENT(pipeline->tiled_display_bin.bin, last_elem);
        last_elem = pipeline->tiled_display_bin.bin;

        link_element_to_tee_src_pad(pipeline->tiler_tee, pipeline->tiled_display_bin.bin);
        last_elem = pipeline->tiler_tee;

        NVGSTDS_ELEM_ADD_PROBE(latency_probe_id, pipeline->instance_bins->sink_bin.sub_bins[0].sink,
                               "sink", latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               appCtx);
        latency_probe_id = latency_probe_id;
    } else {
        /*
         * Create demuxer only if tiled display is disabled.
         */
        pipeline->demuxer = gst_element_factory_make(NVDS_ELEM_STREAM_DEMUX, "demuxer");
        if (!pipeline->demuxer) {
            NVGSTDS_ERR_MSG_V("Failed to create element 'demuxer'");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->demuxer);

        for (i = 0; i < config->num_source_sub_bins; i++) {
            gchar pad_name[16];
            GstPad *demux_src_pad;

            /* Check if any sink has been configured to render/encode output for
             * source index `i`. The processing instance for that source will be
             * created only if atleast one sink has been configured as such.
             */
            if (!is_sink_available_for_source_id(config, i))
                continue;

            if (!create_processing_instance(appCtx, i)) {
                goto done;
            }
            gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->instance_bins[i].bin);

            g_snprintf(pad_name, 16, "src_%02d", i);
            demux_src_pad = gst_element_get_request_pad(pipeline->demuxer, pad_name);
            NVGSTDS_LINK_ELEMENT_FULL(pipeline->demuxer, pad_name, pipeline->instance_bins[i].bin,
                                      "sink");
            gst_object_unref(demux_src_pad);

            for (int k = 0; k < MAX_SINK_BINS; k++) {
                if (strncmp(GST_ELEMENT_NAME(pipeline->instance_bins[i].sink_bin.sub_bins[k].sink),
                            "sink_sub_bin_hlssink", 20) == 0) {
                    NVGSTDS_ELEM_ADD_PROBE(
                        latency_probe_id, pipeline->instance_bins[i].sink_bin.sub_bins[k].sink,
                        "video", latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, appCtx);
                    break;
                } else if (pipeline->instance_bins[i].sink_bin.sub_bins[k].sink) {
                    NVGSTDS_ELEM_ADD_PROBE(
                        latency_probe_id, pipeline->instance_bins[i].sink_bin.sub_bins[k].sink,
                        "sink", latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, appCtx);
                    break;
                }
            }

            latency_probe_id = latency_probe_id;
        }
        last_elem = pipeline->demuxer;
    }

    if (config->tiled_display_config.enable == NV_DS_TILED_DISPLAY_DISABLE) {
        fps_pad = gst_element_get_static_pad(pipeline->demuxer, "sink");
    } else {
        fps_pad = gst_element_get_static_pad(pipeline->tiled_display_bin.bin, "sink");
    }

    pipeline->common_elements.appCtx = appCtx;
    // Decide where in the pipeline the element should be added and add only if
    // enabled

    /////////////////
    /* Start Custom */
    /////////////////
    // TODO: Create dxexample_tee
    pipeline->dxexample_tee = gst_element_factory_make(NVDS_ELEM_TEE, "dxexample_tee");
    if (!pipeline->dxexample_tee) {
        NVGSTDS_ERR_MSG_V("Failed to create element 'dxexample_tee'");
        goto done;
    }
    gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->dxexample_tee);
    NVGSTDS_LINK_ELEMENT(pipeline->dxexample_tee, last_elem);
    last_elem = pipeline->dxexample_tee;
    ////////////////
    /* End Custom */
    ////////////////

    if (config->dsexample_config.enable) {
        // Create dsexample element bin and set properties
        if (!create_dsexample_bin(&config->dsexample_config, &pipeline->dsexample_bin)) {
            goto done;
        }
        // Add dsexample bin to instance bin
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->dsexample_bin.bin);

        // Link this bin to the last element in the bin
        NVGSTDS_LINK_ELEMENT(pipeline->dsexample_bin.bin, last_elem);

        // Set this bin as the last element
        last_elem = pipeline->dsexample_bin.bin;
    }

    /////////////////
    /* Start Custom */
    /////////////////
    if (config->dspostprocessing_config.enable) {
        // Create dspostprocessing element bin and set properties
        if (!create_dspostprocessing_bin(&config->dspostprocessing_config,
                                         &pipeline->dspostprocessing_bin)) {
            goto done;
        }
        // Add dspostprocessing bin to instance bin
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->dspostprocessing_bin.bin);

        // Link this bin to the last element in the bin
        NVGSTDS_LINK_ELEMENT(pipeline->dspostprocessing_bin.bin, last_elem);

        // Set this bin as the last element
        last_elem = pipeline->dspostprocessing_bin.bin;
    }
    ////////////////
    /* End Custom */
    ////////////////

    // create and add common components to pipeline.
    if (!create_common_elements(config, pipeline, &tmp_elem1, &tmp_elem2,
                                bbox_generated_post_analytics_cb)) {
        goto done;
    }

    if (!add_and_link_broker_sink(appCtx)) {
        goto done;
    }

    if (tmp_elem2) {
        NVGSTDS_LINK_ELEMENT(tmp_elem2, last_elem);
        last_elem = tmp_elem1;
    }

    NVGSTDS_LINK_ELEMENT(pipeline->multi_src_bin.bin, last_elem);

    // enable performance measurement and add call back function to receive
    // performance data.
    if (config->enable_perf_measurement) {
        appCtx->perf_struct.context = appCtx;
        enable_perf_measurement(
            &appCtx->perf_struct, fps_pad, pipeline->multi_src_bin.num_bins,
            config->perf_measurement_interval_sec,
            config->multi_source_config[0].dewarper_config.num_surfaces_per_frame, perf_cb);
    }

    latency_probe_id = latency_probe_id;

    if (config->num_message_consumers) {
        for (i = 0; i < config->num_message_consumers; i++) {
            appCtx->c2d_ctx[i] = start_cloud_to_device_messaging(
                &config->message_consumer_config[i], NULL, &appCtx->pipeline.multi_src_bin);
            if (appCtx->c2d_ctx[i] == NULL) {
                NVGSTDS_ERR_MSG_V("Failed to create message consumer");
                goto done;
            }
        }
    }

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL,
                                      "ds-app-null");

    g_mutex_init(&appCtx->app_lock);
    g_cond_init(&appCtx->app_cond);
    g_mutex_init(&appCtx->latency_lock);


    ret = TRUE;
done:
    if (fps_pad)
        gst_object_unref(fps_pad);

    if (!ret) {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to destroy pipeline and release the resources, probes etc.
 */
void destroy_pipeline(AppCtx *appCtx)
{
    gint64 end_time;
    NvDsConfig *config = &appCtx->config;
    guint i;
    GstBus *bus = NULL;

    end_time = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

    if (!appCtx)
        return;

    if (appCtx->pipeline.demuxer) {
        gst_pad_send_event(gst_element_get_static_pad(appCtx->pipeline.demuxer, "sink"),
                           gst_event_new_eos());
    } else if (appCtx->pipeline.instance_bins[0].sink_bin.bin) {
        gst_pad_send_event(
            gst_element_get_static_pad(appCtx->pipeline.instance_bins[0].sink_bin.bin, "sink"),
            gst_event_new_eos());
    }

    g_usleep(100000);

    g_mutex_lock(&appCtx->app_lock);
    if (appCtx->pipeline.pipeline) {
        destroy_smart_record_bin(&appCtx->pipeline.multi_src_bin);
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));

        while (TRUE) {
            GstMessage *message = gst_bus_pop(bus);
            if (message == NULL)
                break;
            else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
                bus_callback(bus, message, appCtx);
            else
                gst_message_unref(message);
        }
        gst_object_unref(bus);
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_NULL);
    }
    g_cond_wait_until(&appCtx->app_cond, &appCtx->app_lock, end_time);
    g_mutex_unlock(&appCtx->app_lock);

    for (i = 0; i < appCtx->config.num_source_sub_bins; i++) {
        NvDsInstanceBin *bin = &appCtx->pipeline.instance_bins[i];
        if (config->osd_config.enable) {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id, bin->osd_bin.nvosd, "src");
        } else {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id, bin->sink_bin.bin, "sink");
        }

        if (config->primary_gie_config.enable) {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->primary_bbox_buffer_probe_id, bin->primary_gie_bin.bin,
                                      "src");
        }
    }
    if (appCtx->latency_info == NULL) {
        free(appCtx->latency_info);
        appCtx->latency_info = NULL;
    }

    destroy_sink_bin();
    g_mutex_clear(&appCtx->latency_lock);

    if (appCtx->pipeline.pipeline) {
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        gst_object_unref(appCtx->pipeline.pipeline);
        appCtx->pipeline.pipeline = NULL;
        pause_perf_measurement(&appCtx->perf_struct);
    }

    if (config->num_message_consumers) {
        for (i = 0; i < config->num_message_consumers; i++) {
            if (appCtx->c2d_ctx[i])
                stop_cloud_to_device_messaging(appCtx->c2d_ctx[i]);
        }
    }

    // Cleanup logging khi giải phóng pipeline
    cleanup_logging_system();
}

gboolean pause_pipeline(AppCtx *appCtx)
{
    GstState cur;
    GstState pending;
    GstStateChangeReturn ret;
    GstClockTime timeout = 5 * GST_SECOND / 1000;

    ret = gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, timeout);

    if (ret == GST_STATE_CHANGE_ASYNC) {
        return FALSE;
    }

    if (cur == GST_STATE_PAUSED) {
        return TRUE;
    } else if (cur == GST_STATE_PLAYING) {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PAUSED);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, GST_CLOCK_TIME_NONE);
        pause_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    } else {
        return FALSE;
    }
}

gboolean resume_pipeline(AppCtx *appCtx)
{
    GstState cur;
    GstState pending;
    GstStateChangeReturn ret;
    GstClockTime timeout = 5 * GST_SECOND / 1000;

    ret = gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, timeout);

    if (ret == GST_STATE_CHANGE_ASYNC) {
        return FALSE;
    }

    if (cur == GST_STATE_PLAYING) {
        return TRUE;
    } else if (cur == GST_STATE_PAUSED) {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PLAYING);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, GST_CLOCK_TIME_NONE);
        resume_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    } else {
        return FALSE;
    }
}

static char* base64_encode(const unsigned char* data, size_t input_length) {
    if (!data || input_length == 0) {
        return NULL;
    }

    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = (char*)malloc(output_length + 1);
    if (!encoded_data) {
        return NULL;
    }

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }

    // Padding
    int padding = input_length % 3;
    if (padding) {
        for (int i = 0; i < 3 - padding; i++) {
            encoded_data[output_length - 1 - i] = '=';
        }
    }

    encoded_data[output_length] = '\0';
    return encoded_data;
}

static NvBufSurfaceMemType get_actual_memory_type(NvBufSurface* surface) {
    if (surface->memType != NVBUF_MEM_DEFAULT) {
        return surface->memType;
    }

    // NVBUF_MEM_DEFAULT sẽ map thành:
    // - NVBUF_MEM_CUDA_DEVICE cho dGPU (desktop GPU)
    // - NVBUF_MEM_SURFACE_ARRAY cho Jetson

    // Kiểm tra CUDA device để xác định platform
    int device_count = 0;
    cudaError_t cuda_status = cudaGetDeviceCount(&device_count);

    if (cuda_status == cudaSuccess && device_count > 0) {
        struct cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);

        // Jetson devices có integrated memory
        if (prop.integrated) {
            return NVBUF_MEM_SURFACE_ARRAY;
        } else {
            return NVBUF_MEM_CUDA_DEVICE;
        }
    }

    // Fallback: assume desktop GPU
    return NVBUF_MEM_CUDA_DEVICE;
}

static char* copy_gpu_data_to_host(NvBufSurface* surface, guint batch_id) {
    if (!surface || batch_id >= surface->numFilled) {
        return NULL;
    }

    NvBufSurfaceParams* params = &surface->surfaceList[batch_id];

    if (params->colorFormat != NVBUF_COLOR_FORMAT_RGBA) {
        return NULL;
    }

    size_t frame_size = params->width * params->height * 4;
    char* host_buffer = (char*)malloc(frame_size);
    if (!host_buffer) {
        g_print("[ERROR] Failed to allocate host buffer\n");
        return NULL;
    }

    // Use cudaMemcpy for device memory
    cudaError_t err = cudaMemcpy(host_buffer, params->dataPtr, frame_size, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        g_print("[ERROR] cudaMemcpy failed: %s\n", cudaGetErrorString(err));
        free(host_buffer);
        return NULL;
    }

    return host_buffer;
}
// Callback function cho PNG writer
static void png_write_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    struct {
        unsigned char** buffer;
        size_t* size;
        size_t* capacity;
    }* write_data = (void*)png_get_io_ptr(png_ptr);

    size_t new_size = *(write_data->size) + length;
    if (new_size > *(write_data->capacity)) {
        *(write_data->capacity) = new_size * 2;
        *(write_data->buffer) = (unsigned char*)realloc(*(write_data->buffer), *(write_data->capacity));
    }

    memcpy(*(write_data->buffer) + *(write_data->size), data, length);
    *(write_data->size) = new_size;
}

// Hàm resize RGBA data với bilinear interpolation
static unsigned char* resize_rgba_bilinear(unsigned char* src_data, int src_width, int src_height,
                                          int dst_width, int dst_height) {
    if (!src_data) return NULL;

    unsigned char* dst_data = (unsigned char*)malloc(dst_width * dst_height * 4);
    if (!dst_data) return NULL;

    float x_ratio = (float)src_width / dst_width;
    float y_ratio = (float)src_height / dst_height;

    for (int y = 0; y < dst_height; y++) {
        for (int x = 0; x < dst_width; x++) {
            float src_x = x * x_ratio;
            float src_y = y * y_ratio;

            int x1 = (int)src_x;
            int y1 = (int)src_y;
            int x2 = (x1 + 1 < src_width) ? x1 + 1 : x1;
            int y2 = (y1 + 1 < src_height) ? y1 + 1 : y1;

            float dx = src_x - x1;
            float dy = src_y - y1;

            for (int c = 0; c < 4; c++) { // RGBA channels
                float val = (1 - dx) * (1 - dy) * src_data[(y1 * src_width + x1) * 4 + c] +
                           dx * (1 - dy) * src_data[(y1 * src_width + x2) * 4 + c] +
                           (1 - dx) * dy * src_data[(y2 * src_width + x1) * 4 + c] +
                           dx * dy * src_data[(y2 * src_width + x2) * 4 + c];

                dst_data[(y * dst_width + x) * 4 + c] = (unsigned char)val;
            }
        }
    }

    return dst_data;
}

// Hàm chuyển RGBA thành PNG
static char* rgba_to_png_base64(unsigned char* rgba_data, int width, int height) {
    if (!rgba_data) return NULL;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return NULL;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return NULL;
    }

    unsigned char* png_buffer = NULL;
    size_t png_size = 0;
    size_t png_capacity = 0;

    struct {
        unsigned char** buffer;
        size_t* size;
        size_t* capacity;
    } write_data = { &png_buffer, &png_size, &png_capacity };

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        if (png_buffer) free(png_buffer);
        return NULL;
    }

    // Setup PNG write callback
    png_set_write_fn(png_ptr, &write_data, png_write_callback, NULL);

    // Set PNG header
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    // Set compression level (0-9, 6 is default)
    png_set_compression_level(png_ptr, 6);

    png_write_info(png_ptr, info_ptr);

    // Create row pointers
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = rgba_data + y * width * 4;
    }

    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);

    // Cleanup
    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    // Encode PNG to base64
    char* base64_result = base64_encode(png_buffer, png_size);
    free(png_buffer);

    return base64_result;
}

static char* encode_full_frame_base64(NvBufSurface* surface, NvDsFrameMeta* frame_meta) {
    if (!surface || !frame_meta) {
        g_print("[DEBUG] Invalid surface or frame_meta\n");
        return NULL;
    }

    guint batch_id = frame_meta->batch_id;
    if (batch_id >= surface->numFilled) {
        batch_id = 0;
    }

    NvBufSurfaceParams* params = &surface->surfaceList[batch_id];

    if (params->colorFormat != NVBUF_COLOR_FORMAT_RGBA) {
        g_print("[DEBUG] Skipping non-RGBA format: %s\n", get_color_format_str(params->colorFormat));
        return NULL;
    }

    const int TARGET_WIDTH = 1280;
    const int TARGET_HEIGHT = 720;

    char* result = NULL;
    char* host_data = NULL;
    unsigned char* resized_data = NULL;
    char* png_base64 = NULL;

    switch (surface->memType) {
        case NVBUF_MEM_DEFAULT: {
            NvBufSurfaceMemType actual_type = get_actual_memory_type(surface);

            if (actual_type == NVBUF_MEM_CUDA_DEVICE) {
                host_data = copy_gpu_data_to_host(surface, batch_id);
                if (host_data) {
                    // Resize ảnh từ original size xuống 720p
                    resized_data = resize_rgba_bilinear((unsigned char*)host_data,
                                                       params->width, params->height,
                                                       TARGET_WIDTH, TARGET_HEIGHT);
                    free(host_data);

                    if (resized_data) {
                        png_base64 = rgba_to_png_base64(resized_data, TARGET_WIDTH, TARGET_HEIGHT);
                        free(resized_data);
                    }
                }
            } else {
                // Jetson: NVBUF_MEM_SURFACE_ARRAY - có thể map trực tiếp
                if (NvBufSurfaceMap(surface, batch_id, 0, NVBUF_MAP_READ) == 0) {
                    if (params->dataPtr) {
                        resized_data = resize_rgba_bilinear((unsigned char*)params->dataPtr,
                                                           params->width, params->height,
                                                           TARGET_WIDTH, TARGET_HEIGHT);
                        if (resized_data) {
                            png_base64 = rgba_to_png_base64(resized_data, TARGET_WIDTH, TARGET_HEIGHT);
                            free(resized_data);
                        }
                    }
                    NvBufSurfaceUnMap(surface, batch_id, 0);
                }
            }
            break;
        }

        case NVBUF_MEM_CUDA_PINNED:
        case NVBUF_MEM_CUDA_UNIFIED: {
            if (NvBufSurfaceMap(surface, batch_id, 0, NVBUF_MAP_READ) == 0) {
                NvBufSurfaceSyncForCpu(surface, batch_id, 0);

                if (params->dataPtr) {
                    resized_data = resize_rgba_bilinear((unsigned char*)params->dataPtr,
                                                       params->width, params->height,
                                                       TARGET_WIDTH, TARGET_HEIGHT);
                    if (resized_data) {
                        png_base64 = rgba_to_png_base64(resized_data, TARGET_WIDTH, TARGET_HEIGHT);
                        free(resized_data);

                    }
                }

                NvBufSurfaceUnMap(surface, batch_id, 0);
            }
            break;
        }

        case NVBUF_MEM_CUDA_DEVICE: {
            host_data = copy_gpu_data_to_host(surface, batch_id);
            if (host_data) {
                resized_data = resize_rgba_bilinear((unsigned char*)host_data,
                                                   params->width, params->height,
                                                   TARGET_WIDTH, TARGET_HEIGHT);
                free(host_data);

                if (resized_data) {
                    png_base64 = rgba_to_png_base64(resized_data, TARGET_WIDTH, TARGET_HEIGHT);
                    free(resized_data);
                }
            }
            break;
        }

        case NVBUF_MEM_SURFACE_ARRAY: {
            if (NvBufSurfaceMap(surface, batch_id, 0, NVBUF_MAP_READ) == 0) {
                if (params->dataPtr) {
                    resized_data = resize_rgba_bilinear((unsigned char*)params->dataPtr,
                                                       params->width, params->height,
                                                       TARGET_WIDTH, TARGET_HEIGHT);
                    if (resized_data) {
                        png_base64 = rgba_to_png_base64(resized_data, TARGET_WIDTH, TARGET_HEIGHT);
                        free(resized_data);

                    }
                }
                NvBufSurfaceUnMap(surface, batch_id, 0);
            }
            break;
        }

        default: {
            g_print("[ERROR] Unsupported memory type: %d\n", surface->memType);
            break;
        }
    }

    if (!png_base64) {
        g_print("[ERROR] Failed to encode PNG frame - all methods failed\n");
        return NULL;
    }

    // ✅ Tạo string theo định dạng web chuẩn
    const char* mime_type = "image/png";
    size_t header_len = snprintf(NULL, 0, "data:%s;base64,", mime_type);
    size_t total_len = header_len + strlen(png_base64) + 1;

    result = (char*)malloc(total_len);
    if (result) {
        snprintf(result, total_len, "data:%s;base64,%s", mime_type, png_base64);

    }

    free(png_base64);
    return result;
}
