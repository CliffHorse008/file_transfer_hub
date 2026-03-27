#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BACKLOG 16
#define READ_BUFFER_SIZE 8192
#define MAX_HEADER_SIZE (64 * 1024)
#define MAX_BODY_SIZE (100 * 1024 * 1024)
#define LOG_FILE_PREFIX "file_manager"

static char g_log_dir[PATH_MAX] = ".";

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

typedef struct {
    char *name;
    bool is_directory;
    off_t size;
    time_t mtime;
} FileEntry;

typedef struct {
    char method[8];
    char path[2048];
    char query[2048];
    char content_type[256];
    char user_agent[512];
    size_t content_length;
} HttpRequest;

typedef struct {
    int client_fd;
    const char *base_dir;
    const char *frontend_dir;
    char client_ip[INET_ADDRSTRLEN];
} ClientContext;

static volatile sig_atomic_t keep_running = 1;

static bool find_query_param(const char *query, const char *key, char *value, size_t value_size);
static bool list_directory(const char *dir_path, FileEntry **entries_out, size_t *count_out);

static void sanitize_log_text(const char *input, char *output, size_t output_size) {
    if (output_size == 0) {
        return;
    }

    size_t oi = 0;
    for (size_t i = 0; input[i] != '\0' && oi + 1 < output_size; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            output[oi++] = ' ';
        } else if (c < 32) {
            output[oi++] = '?';
        } else {
            output[oi++] = (char)c;
        }
    }
    output[oi] = '\0';
}

static void log_event(const char *client_ip, const char *event, const char *detail) {
    time_t now = time(NULL);
    struct tm tm_value;
    char time_buffer[32];
    if (localtime_r(&now, &tm_value) == NULL ||
        strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &tm_value) == 0) {
        snprintf(time_buffer, sizeof(time_buffer), "unknown-time");
    }

    char log_filename[64];
    if (localtime_r(&now, &tm_value) == NULL) {
        snprintf(log_filename, sizeof(log_filename), LOG_FILE_PREFIX ".log");
    } else {
        snprintf(log_filename,
                 sizeof(log_filename),
                 LOG_FILE_PREFIX "-%04d-%02d-%02d.log",
                 tm_value.tm_year + 1900,
                 tm_value.tm_mon + 1,
                 tm_value.tm_mday);
    }

    char log_path[PATH_MAX];
    int path_written = snprintf(log_path, sizeof(log_path), "%s/%s", g_log_dir, log_filename);
    if (path_written < 0 || (size_t)path_written >= sizeof(log_path)) {
        return;
    }

    FILE *log_file = fopen(log_path, "a");
    if (log_file == NULL) {
        return;
    }

    char safe_detail[1024];
    sanitize_log_text(detail, safe_detail, sizeof(safe_detail));
    fprintf(log_file,
            "[%s] ip=%s event=%s detail=%s\n",
            time_buffer,
            client_ip != NULL ? client_ip : "unknown",
            event,
            safe_detail);
    fclose(log_file);
}

static void handle_signal(int signum) {
    (void)signum;
    keep_running = 0;
}

static void sb_init(StringBuilder *sb) {
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static void sb_free(StringBuilder *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

static bool sb_ensure(StringBuilder *sb, size_t extra) {
    size_t needed = sb->length + extra + 1;
    if (needed <= sb->capacity) {
        return true;
    }

    size_t new_capacity = sb->capacity == 0 ? 1024 : sb->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    char *new_data = realloc(sb->data, new_capacity);
    if (new_data == NULL) {
        return false;
    }

    sb->data = new_data;
    sb->capacity = new_capacity;
    return true;
}

static bool sb_append_n(StringBuilder *sb, const char *text, size_t length) {
    if (!sb_ensure(sb, length)) {
        return false;
    }

    memcpy(sb->data + sb->length, text, length);
    sb->length += length;
    sb->data[sb->length] = '\0';
    return true;
}

static bool sb_append(StringBuilder *sb, const char *text) {
    return sb_append_n(sb, text, strlen(text));
}

static bool sb_append_format(StringBuilder *sb, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }

    if (!sb_ensure(sb, (size_t)needed)) {
        va_end(args);
        return false;
    }

    vsnprintf(sb->data + sb->length, sb->capacity - sb->length, format, args);
    va_end(args);
    sb->length += (size_t)needed;
    return true;
}

static bool send_all(int client_fd, const void *buffer, size_t length) {
    const char *data = buffer;
    size_t sent = 0;
    while (sent < length) {
        ssize_t written = send(client_fd, data + sent, length - sent, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

static bool send_response(int client_fd,
                          int status_code,
                          const char *status_text,
                          const char *content_type,
                          const void *body,
                          size_t body_length,
                          const char *extra_headers) {
    StringBuilder headers;
    sb_init(&headers);
    bool ok = sb_append_format(&headers,
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n",
                               status_code,
                               status_text,
                               content_type,
                               body_length);

    if (ok && extra_headers != NULL) {
        ok = sb_append(&headers, extra_headers);
    }
    if (ok) {
        ok = sb_append(&headers, "\r\n");
    }

    if (!ok) {
        sb_free(&headers);
        return false;
    }

    bool sent = send_all(client_fd, headers.data, headers.length);
    if (sent && body_length > 0) {
        sent = send_all(client_fd, body, body_length);
    }

    sb_free(&headers);
    return sent;
}

static bool send_text_response(int client_fd, int status_code, const char *status_text, const char *message) {
    return send_response(client_fd,
                         status_code,
                         status_text,
                         "text/plain; charset=utf-8",
                         message,
                         strlen(message),
                         NULL);
}

static char from_hex(char c) {
    if (c >= '0' && c <= '9') {
        return (char)(c - '0');
    }
    c = (char)tolower((unsigned char)c);
    return (char)(10 + c - 'a');
}

static bool url_decode(const char *input, char *output, size_t output_size) {
    size_t oi = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (oi + 1 >= output_size) {
            return false;
        }

        if (input[i] == '%') {
            if (!isxdigit((unsigned char)input[i + 1]) || !isxdigit((unsigned char)input[i + 2])) {
                return false;
            }
            output[oi++] = (char)((from_hex(input[i + 1]) << 4) | from_hex(input[i + 2]));
            i += 2;
        } else if (input[i] == '+') {
            output[oi++] = ' ';
        } else {
            output[oi++] = input[i];
        }
    }
    output[oi] = '\0';
    return true;
}

static bool url_encode_component(const char *input, StringBuilder *sb) {
    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (!sb_append_n(sb, (const char *)&c, 1)) {
                return false;
            }
        } else {
            char encoded[3];
            encoded[0] = '%';
            encoded[1] = hex[c >> 4];
            encoded[2] = hex[c & 0x0F];
            if (!sb_append_n(sb, encoded, sizeof(encoded))) {
                return false;
            }
        }
    }
    return true;
}

static const char *format_size(off_t size, char *buffer, size_t buffer_size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)size;
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit_index;
    }
    snprintf(buffer, buffer_size, "%.2f %s", value, units[unit_index]);
    return buffer;
}

static bool format_time_string(time_t raw_time, char *buffer, size_t buffer_size) {
    struct tm tm_value;
    if (localtime_r(&raw_time, &tm_value) == NULL) {
        return false;
    }
    return strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &tm_value) > 0;
}

static int compare_entries(const void *left, const void *right) {
    const FileEntry *a = left;
    const FileEntry *b = right;

    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    return strcasecmp(a->name, b->name);
}

static void free_entries(FileEntry *entries, size_t count) {
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
    }
    free(entries);
}

static bool is_safe_filename(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return false;
    }
    return strstr(name, "..") == NULL;
}

static void sanitize_uploaded_filename(char *name) {
    char *last_slash = strrchr(name, '/');
    char *last_backslash = strrchr(name, '\\');
    char *base = name;

    if (last_slash != NULL && last_slash + 1 > base) {
        base = last_slash + 1;
    }
    if (last_backslash != NULL && last_backslash + 1 > base) {
        base = last_backslash + 1;
    }
    if (base != name) {
        memmove(name, base, strlen(base) + 1);
    }
}

static bool join_path(char *output, size_t output_size, const char *dir_path, const char *name) {
    int written = snprintf(output, output_size, "%s/%s", dir_path, name);
    return written >= 0 && (size_t)written < output_size;
}

static bool starts_with_path(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) {
        return false;
    }
    return path[prefix_len] == '\0' || path[prefix_len] == '/';
}

static bool normalize_relative_path(const char *input, char *output, size_t output_size) {
    if (input == NULL || input[0] == '\0') {
        if (output_size == 0) {
            return false;
        }
        output[0] = '\0';
        return true;
    }

    size_t out_len = 0;
    const char *cursor = input;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        const char *segment_end = cursor;
        while (*segment_end != '\0' && *segment_end != '/') {
            ++segment_end;
        }

        size_t segment_len = (size_t)(segment_end - cursor);
        if (segment_len == 0) {
            cursor = segment_end;
            continue;
        }
        if ((segment_len == 1 && cursor[0] == '.') ||
            (segment_len == 2 && cursor[0] == '.' && cursor[1] == '.')) {
            return false;
        }
        if (segment_len >= NAME_MAX) {
            return false;
        }
        for (size_t i = 0; i < segment_len; ++i) {
            unsigned char c = (unsigned char)cursor[i];
            if (c < 32 || c == '\\') {
                return false;
            }
        }

        size_t needed = out_len + (out_len > 0 ? 1 : 0) + segment_len + 1;
        if (needed > output_size) {
            return false;
        }
        if (out_len > 0) {
            output[out_len++] = '/';
        }
        memcpy(output + out_len, cursor, segment_len);
        out_len += segment_len;
        output[out_len] = '\0';

        cursor = segment_end;
    }

    return true;
}

static bool resolve_directory_path(const char *base_dir,
                                   const char *relative_path,
                                   char *resolved_dir,
                                   size_t resolved_size) {
    char normalized[PATH_MAX];
    if (!normalize_relative_path(relative_path, normalized, sizeof(normalized))) {
        return false;
    }

    if (normalized[0] == '\0') {
        return snprintf(resolved_dir, resolved_size, "%s", base_dir) > 0 &&
               strlen(base_dir) < resolved_size;
    }

    char candidate[PATH_MAX];
    if (!join_path(candidate, sizeof(candidate), base_dir, normalized)) {
        return false;
    }

    if (realpath(candidate, resolved_dir) == NULL) {
        return false;
    }
    if (!starts_with_path(resolved_dir, base_dir)) {
        return false;
    }

    struct stat st;
    if (stat(resolved_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    return true;
}

static bool build_relative_child_path(const char *current_path,
                                      const char *name,
                                      char *child_path,
                                      size_t child_size) {
    if (!is_safe_filename(name)) {
        return false;
    }
    if (current_path == NULL || current_path[0] == '\0') {
        return snprintf(child_path, child_size, "%s", name) > 0 &&
               strlen(name) < child_size;
    }
    return join_path(child_path, child_size, current_path, name);
}

static bool build_parent_path(const char *current_path, char *parent_path, size_t parent_size) {
    if (current_path == NULL || current_path[0] == '\0') {
        if (parent_size == 0) {
            return false;
        }
        parent_path[0] = '\0';
        return true;
    }

    const char *slash = strrchr(current_path, '/');
    if (slash == NULL) {
        if (parent_size == 0) {
            return false;
        }
        parent_path[0] = '\0';
        return true;
    }

    size_t length = (size_t)(slash - current_path);
    if (length + 1 > parent_size) {
        return false;
    }
    memcpy(parent_path, current_path, length);
    parent_path[length] = '\0';
    return true;
}

static bool json_escape_append(StringBuilder *sb, const char *text) {
    for (size_t i = 0; text[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '\\':
                if (!sb_append(sb, "\\\\")) {
                    return false;
                }
                break;
            case '"':
                if (!sb_append(sb, "\\\"")) {
                    return false;
                }
                break;
            case '\b':
                if (!sb_append(sb, "\\b")) {
                    return false;
                }
                break;
            case '\f':
                if (!sb_append(sb, "\\f")) {
                    return false;
                }
                break;
            case '\n':
                if (!sb_append(sb, "\\n")) {
                    return false;
                }
                break;
            case '\r':
                if (!sb_append(sb, "\\r")) {
                    return false;
                }
                break;
            case '\t':
                if (!sb_append(sb, "\\t")) {
                    return false;
                }
                break;
            case '<':
                if (!sb_append(sb, "\\u003c")) {
                    return false;
                }
                break;
            case '>':
                if (!sb_append(sb, "\\u003e")) {
                    return false;
                }
                break;
            case '&':
                if (!sb_append(sb, "\\u0026")) {
                    return false;
                }
                break;
            default:
                if (c < 32) {
                    if (!sb_append_format(sb, "\\u%04x", c)) {
                        return false;
                    }
                } else if (!sb_append_n(sb, (const char *)&text[i], 1)) {
                    return false;
                }
                break;
        }
    }
    return true;
}

static const char *find_bytes(const char *haystack,
                              size_t haystack_len,
                              const char *needle,
                              size_t needle_len) {
    if (needle_len == 0) {
        return haystack;
    }
    if (haystack_len < needle_len) {
        return NULL;
    }

    size_t limit = haystack_len - needle_len;
    for (size_t i = 0; i <= limit; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool send_json_message(int client_fd, int status_code, const char *status_text, bool ok_value, const char *message) {
    StringBuilder body;
    sb_init(&body);
    bool ok = sb_append(&body, "{\"ok\":");
    if (ok) {
        ok = sb_append(&body, ok_value ? "true" : "false");
    }
    if (ok) {
        ok = sb_append(&body, ",\"message\":\"");
    }
    if (ok) {
        ok = json_escape_append(&body, message);
    }
    if (ok) {
        ok = sb_append(&body, "\"}");
    }

    if (!ok) {
        sb_free(&body);
        return false;
    }

    bool sent = send_response(client_fd,
                              status_code,
                              status_text,
                              "application/json; charset=utf-8",
                              body.data,
                              body.length,
                              NULL);
    sb_free(&body);
    return sent;
}

static bool resolve_listing_target(const char *base_dir,
                                   const char *query,
                                   char *relative_path,
                                   size_t relative_path_size,
                                   char *target_dir,
                                   size_t target_dir_size) {
    char decoded_path[PATH_MAX];
    if (find_query_param(query, "path", decoded_path, sizeof(decoded_path))) {
        if (!normalize_relative_path(decoded_path, relative_path, relative_path_size)) {
            return false;
        }
    } else {
        relative_path[0] = '\0';
    }

    return resolve_directory_path(base_dir, relative_path, target_dir, target_dir_size);
}

static bool build_directory_listing_body(const char *relative_path,
                                         const char *target_dir,
                                         StringBuilder *body) {
    FileEntry *entries = NULL;
    size_t entry_count = 0;
    if (!list_directory(target_dir, &entries, &entry_count)) {
        return false;
    }

    char parent_path[PATH_MAX];
    if (!build_parent_path(relative_path, parent_path, sizeof(parent_path))) {
        free_entries(entries, entry_count);
        return false;
    }

    bool ok = sb_append(body, "{\"ok\":true,\"path\":\"");
    if (ok) {
        ok = json_escape_append(body, relative_path);
    }
    if (ok) {
        ok = sb_append(body, "\",\"parent_path\":\"");
    }
    if (ok) {
        ok = json_escape_append(body, parent_path);
    }
    if (ok) {
        ok = sb_append(body, "\",\"items\":[");
    }

    for (size_t i = 0; ok && i < entry_count; ++i) {
        char time_buffer[64];
        char size_buffer[64];
        char child_path[PATH_MAX];
        StringBuilder encoded_dir;
        StringBuilder encoded_name;

        sb_init(&encoded_dir);
        sb_init(&encoded_name);

        ok = format_time_string(entries[i].mtime, time_buffer, sizeof(time_buffer));
        if (ok) {
            ok = url_encode_component(relative_path, &encoded_dir);
        }
        if (ok) {
            ok = url_encode_component(entries[i].name, &encoded_name);
        }
        if (ok && entries[i].is_directory) {
            ok = build_relative_child_path(relative_path, entries[i].name, child_path, sizeof(child_path));
        }

        if (ok && i > 0) {
            ok = sb_append(body, ",");
        }
        if (ok) {
            ok = sb_append(body, "{\"name\":\"");
        }
        if (ok) {
            ok = json_escape_append(body, entries[i].name);
        }
        if (ok) {
            ok = sb_append(body, "\",\"is_directory\":");
        }
        if (ok) {
            ok = sb_append(body, entries[i].is_directory ? "true" : "false");
        }
        if (ok) {
            ok = sb_append_format(body, ",\"size_bytes\":%lld,", (long long)entries[i].size);
        }
        if (ok) {
            ok = sb_append(body, "\"size_text\":\"");
        }
        if (ok) {
            ok = json_escape_append(body, entries[i].is_directory ? "-" : format_size(entries[i].size, size_buffer, sizeof(size_buffer)));
        }
        if (ok) {
            ok = sb_append(body, "\",\"modified_at\":\"");
        }
        if (ok) {
            ok = json_escape_append(body, time_buffer);
        }
        if (ok) {
            ok = sb_append(body, "\"");
        }
        if (ok && entries[i].is_directory) {
            ok = sb_append(body, ",\"path\":\"");
        }
        if (ok && entries[i].is_directory) {
            ok = json_escape_append(body, child_path);
        }
        if (ok && entries[i].is_directory) {
            ok = sb_append(body, "\"");
        }
        if (ok && !entries[i].is_directory) {
            ok = sb_append(body, ",\"download_url\":\"/api/download?path=");
        }
        if (ok && !entries[i].is_directory) {
            ok = sb_append_n(body, encoded_dir.data, encoded_dir.length);
        }
        if (ok && !entries[i].is_directory) {
            ok = sb_append(body, "&file=");
        }
        if (ok && !entries[i].is_directory) {
            ok = sb_append_n(body, encoded_name.data, encoded_name.length);
        }
        if (ok && !entries[i].is_directory) {
            ok = sb_append(body, "\"");
        }
        if (ok) {
            ok = sb_append(body, "}");
        }

        sb_free(&encoded_dir);
        sb_free(&encoded_name);
    }

    if (ok) {
        ok = sb_append(body, "]}");
    }

    free_entries(entries, entry_count);
    return ok;
}

static void make_download_filename(const char *filename, char *output, size_t output_size) {
    size_t oi = 0;
    for (size_t i = 0; filename[i] != '\0' && oi + 1 < output_size; ++i) {
        char c = filename[i];
        if (c == '"' || c == '\\' || (unsigned char)c < 32) {
            output[oi++] = '_';
        } else {
            output[oi++] = c;
        }
    }
    output[oi] = '\0';
}

static bool list_directory(const char *dir_path, FileEntry **entries_out, size_t *count_out) {
    DIR *directory = opendir(dir_path);
    if (directory == NULL) {
        return false;
    }

    FileEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        if (!join_path(full_path, sizeof(full_path), dir_path, entry->d_name)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            FileEntry *new_entries = realloc(entries, new_capacity * sizeof(FileEntry));
            if (new_entries == NULL) {
                free_entries(entries, count);
                closedir(directory);
                return false;
            }
            entries = new_entries;
            capacity = new_capacity;
        }

        entries[count].name = strdup(entry->d_name);
        if (entries[count].name == NULL) {
            free_entries(entries, count);
            closedir(directory);
            return false;
        }
        entries[count].is_directory = S_ISDIR(st.st_mode);
        entries[count].size = st.st_size;
        entries[count].mtime = st.st_mtime;
        ++count;
    }

    closedir(directory);
    qsort(entries, count, sizeof(FileEntry), compare_entries);

    *entries_out = entries;
    *count_out = count;
    return true;
}

static bool send_directory_listing_json(int client_fd, const char *base_dir, const char *query) {
    char relative_path[PATH_MAX];
    char target_dir[PATH_MAX];
    if (!resolve_listing_target(base_dir,
                                query,
                                relative_path,
                                sizeof(relative_path),
                                target_dir,
                                sizeof(target_dir))) {
        char decoded_path[PATH_MAX];
        if (find_query_param(query, "path", decoded_path, sizeof(decoded_path)) &&
            !normalize_relative_path(decoded_path, relative_path, sizeof(relative_path))) {
            return send_json_message(client_fd, 400, "Bad Request", false, "目录参数无效。");
        }
        return send_json_message(client_fd, 404, "Not Found", false, "目录不存在。");
    }

    StringBuilder body;
    sb_init(&body);
    if (!build_directory_listing_body(relative_path, target_dir, &body)) {
        sb_free(&body);
        return send_json_message(client_fd, 500, "Internal Server Error", false, "目录响应生成失败。");
    }

    bool sent = send_response(client_fd,
                              200,
                              "OK",
                              "application/json; charset=utf-8",
                              body.data,
                              body.length,
                              NULL);
    sb_free(&body);
    return sent;
}

static bool send_streamed_file(int client_fd,
                               const char *file_path,
                               const char *content_type,
                               const char *extra_headers) {
    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    StringBuilder headers;
    sb_init(&headers);
    bool ok = sb_append_format(&headers,
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %lld\r\n"
                               "Connection: close\r\n",
                               content_type,
                               (long long)st.st_size);
    if (ok && extra_headers != NULL) {
        ok = sb_append(&headers, extra_headers);
    }
    if (ok) {
        ok = sb_append(&headers, "\r\n");
    }
    if (!ok) {
        sb_free(&headers);
        close(fd);
        return false;
    }

    bool sent = send_all(client_fd, headers.data, headers.length);
    sb_free(&headers);

    char buffer[READ_BUFFER_SIZE];
    while (sent) {
        ssize_t read_bytes = read(fd, buffer, sizeof(buffer));
        if (read_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            sent = false;
            break;
        }
        if (read_bytes == 0) {
            break;
        }
        sent = send_all(client_fd, buffer, (size_t)read_bytes);
    }

    close(fd);
    return sent;
}

static bool resolve_frontend_file(const char *frontend_dir,
                                  const char *request_path,
                                  char *file_path,
                                  size_t file_size) {
    const char *name = NULL;

    if (strcmp(request_path, "/") == 0 || strcmp(request_path, "/index.html") == 0) {
        name = "index.html";
    } else if (strcmp(request_path, "/app.js") == 0) {
        name = "app.js";
    } else if (strcmp(request_path, "/app.css") == 0) {
        name = "app.css";
    } else if (strcmp(request_path, "/favicon.svg") == 0) {
        name = "favicon.svg";
    } else {
        return false;
    }

    return join_path(file_path, file_size, frontend_dir, name);
}

static const char *static_cache_headers(const char *request_path) {
    if (strcmp(request_path, "/app.js") == 0 ||
        strcmp(request_path, "/app.css") == 0 ||
        strcmp(request_path, "/favicon.svg") == 0) {
        return "Cache-Control: public, max-age=86400\r\n";
    }
    if (strcmp(request_path, "/") == 0 || strcmp(request_path, "/index.html") == 0) {
        return "Cache-Control: no-cache\r\n";
    }
    return NULL;
}

static bool load_file_into_builder(const char *file_path, StringBuilder *body) {
    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    bool ok = sb_ensure(body, (size_t)st.st_size);
    while (ok && body->length < (size_t)st.st_size) {
        ssize_t read_bytes = read(fd,
                                  body->data + body->length,
                                  (size_t)st.st_size - body->length);
        if (read_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (read_bytes == 0) {
            break;
        }
        body->length += (size_t)read_bytes;
    }

    if (ok && body->data != NULL) {
        body->data[body->length] = '\0';
    }

    close(fd);
    return ok;
}

static bool send_index_page(int client_fd,
                            const char *base_dir,
                            const char *frontend_dir,
                            const char *query) {
    char index_file[PATH_MAX];
    if (!join_path(index_file, sizeof(index_file), frontend_dir, "index.html")) {
        return send_text_response(client_fd, 500, "Internal Server Error", "Failed to load index template.\n");
    }

    StringBuilder html;
    sb_init(&html);
    if (!load_file_into_builder(index_file, &html)) {
        sb_free(&html);
        return send_text_response(client_fd, 500, "Internal Server Error", "Failed to load index template.\n");
    }

    const char *marker = "<script id=\"initial-directory-data\" type=\"application/json\"></script>";
    const char *marker_pos = strstr(html.data, marker);
    if (marker_pos != NULL) {
        char relative_path[PATH_MAX];
        char target_dir[PATH_MAX];
        if (resolve_listing_target(base_dir,
                                   query,
                                   relative_path,
                                   sizeof(relative_path),
                                   target_dir,
                                   sizeof(target_dir))) {
            StringBuilder initial_data;
            sb_init(&initial_data);
            if (build_directory_listing_body(relative_path, target_dir, &initial_data)) {
                StringBuilder page;
                sb_init(&page);
                size_t prefix_length = (size_t)(marker_pos - html.data);
                bool ok = sb_append_n(&page, html.data, prefix_length);
                if (ok) {
                    ok = sb_append(&page, "<script id=\"initial-directory-data\" type=\"application/json\">");
                }
                if (ok) {
                    ok = sb_append_n(&page, initial_data.data, initial_data.length);
                }
                if (ok) {
                    ok = sb_append(&page, "</script>");
                }
                if (ok) {
                    ok = sb_append(&page, marker_pos + strlen(marker));
                }
                if (ok) {
                    sb_free(&html);
                    html = page;
                } else {
                    sb_free(&page);
                }
            }
            sb_free(&initial_data);
        }
    }

    bool sent = send_response(client_fd,
                              200,
                              "OK",
                              "text/html; charset=utf-8",
                              html.data,
                              html.length,
                              static_cache_headers("/"));
    sb_free(&html);
    return sent;
}

static const char *guess_content_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    ++ext;

    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 || strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(ext, "css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(ext, "js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(ext, "json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(ext, "png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, "gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, "svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(ext, "pdf") == 0) {
        return "application/pdf";
    }
    if (strcasecmp(ext, "zip") == 0) {
        return "application/zip";
    }
    return "application/octet-stream";
}

static bool write_file_bytes(const char *path, const char *data, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    size_t written_total = 0;
    while (written_total < length) {
        ssize_t written = write(fd, data + written_total, length - written_total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return false;
        }
        written_total += (size_t)written;
    }

    close(fd);
    return true;
}

static bool save_uploaded_file(const char *target_dir,
                               const char *content_type,
                               const char *body,
                               size_t body_length,
                               char *saved_filename,
                               size_t filename_size,
                               char *error_message,
                               size_t error_size) {
    const char *boundary_key = "boundary=";
    const char *boundary_pos = strstr(content_type, boundary_key);
    if (boundary_pos == NULL) {
        snprintf(error_message, error_size, "缺少 multipart boundary。");
        return false;
    }

    boundary_pos += strlen(boundary_key);
    char boundary[256];
    size_t boundary_len = strcspn(boundary_pos, ";\r\n");
    if (boundary_len == 0 || boundary_len >= sizeof(boundary)) {
        snprintf(error_message, error_size, "无效的 multipart boundary。");
        return false;
    }
    memcpy(boundary, boundary_pos, boundary_len);
    boundary[boundary_len] = '\0';

    char delimiter[300];
    int delimiter_written = snprintf(delimiter, sizeof(delimiter), "--%s", boundary);
    if (delimiter_written < 0 || (size_t)delimiter_written >= sizeof(delimiter)) {
        snprintf(error_message, error_size, "boundary 过长。");
        return false;
    }
    size_t delimiter_len = (size_t)delimiter_written;

    const char *cursor = body;
    const char *body_end = body + body_length;
    while (cursor < body_end) {
        const char *part_start = find_bytes(cursor,
                                            (size_t)(body_end - cursor),
                                            delimiter,
                                            delimiter_len);
        if (part_start == NULL || part_start >= body_end) {
            break;
        }

        part_start += delimiter_len;
        if (part_start + 2 <= body_end && memcmp(part_start, "--", 2) == 0) {
            break;
        }
        if (part_start + 2 > body_end || memcmp(part_start, "\r\n", 2) != 0) {
            snprintf(error_message, error_size, "multipart 数据格式错误。");
            return false;
        }
        part_start += 2;

        const char *headers_end = find_bytes(part_start,
                                             (size_t)(body_end - part_start),
                                             "\r\n\r\n",
                                             4);
        if (headers_end == NULL || headers_end >= body_end) {
            snprintf(error_message, error_size, "未找到上传头信息。");
            return false;
        }

        size_t headers_length = (size_t)(headers_end - part_start);
        char *headers = malloc(headers_length + 1);
        if (headers == NULL) {
            snprintf(error_message, error_size, "内存不足。");
            return false;
        }
        memcpy(headers, part_start, headers_length);
        headers[headers_length] = '\0';

        bool is_target_field = strstr(headers, "name=\"file\"") != NULL;
        char *filename_pos = strstr(headers, "filename=\"");
        const char *data_start = headers_end + 4;
        const char *next_boundary = find_bytes(data_start,
                                               (size_t)(body_end - data_start),
                                               delimiter,
                                               delimiter_len);

        if (is_target_field && filename_pos != NULL && next_boundary != NULL && next_boundary <= body_end) {
            filename_pos += strlen("filename=\"");
            char *filename_end = strchr(filename_pos, '"');
            if (filename_end == NULL) {
                free(headers);
                snprintf(error_message, error_size, "上传文件名解析失败。");
                return false;
            }

            size_t filename_length = (size_t)(filename_end - filename_pos);
            if (filename_length == 0 || filename_length >= NAME_MAX) {
                free(headers);
                snprintf(error_message, error_size, "上传文件名无效。");
                return false;
            }

            char filename[NAME_MAX];
            memcpy(filename, filename_pos, filename_length);
            filename[filename_length] = '\0';
            sanitize_uploaded_filename(filename);

            if (!is_safe_filename(filename)) {
                free(headers);
                snprintf(error_message, error_size, "上传文件名不安全。");
                return false;
            }

            const char *data_end = next_boundary;
            if (data_end - 2 >= data_start && memcmp(data_end - 2, "\r\n", 2) == 0) {
                data_end -= 2;
            }

            char full_path[PATH_MAX];
            if (!join_path(full_path, sizeof(full_path), target_dir, filename)) {
                free(headers);
                snprintf(error_message, error_size, "目标路径过长。");
                return false;
            }

            bool saved = write_file_bytes(full_path, data_start, (size_t)(data_end - data_start));
            free(headers);
            if (!saved) {
                snprintf(error_message, error_size, "文件保存失败: %s", strerror(errno));
                return false;
            }
            snprintf(saved_filename, filename_size, "%s", filename);
            return true;
        }

        free(headers);
        cursor = data_start;
    }

    snprintf(error_message, error_size, "未找到 name=file 的上传内容。");
    return false;
}

static bool find_query_param(const char *query, const char *key, char *value, size_t value_size) {
    if (query == NULL || query[0] == '\0') {
        return false;
    }

    size_t key_len = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *amp = strchr(cursor, '&');
        size_t pair_len = amp == NULL ? strlen(cursor) : (size_t)(amp - cursor);

        if (pair_len > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            char encoded[2048];
            if (pair_len - key_len - 1 >= sizeof(encoded)) {
                return false;
            }
            memcpy(encoded, cursor + key_len + 1, pair_len - key_len - 1);
            encoded[pair_len - key_len - 1] = '\0';
            return url_decode(encoded, value, value_size);
        }

        if (amp == NULL) {
            break;
        }
        cursor = amp + 1;
    }

    return false;
}

static bool read_http_request(int client_fd, HttpRequest *request, char **body_out, size_t *body_length_out) {
    memset(request, 0, sizeof(*request));
    *body_out = NULL;
    *body_length_out = 0;

    size_t buffer_size = READ_BUFFER_SIZE;
    char *buffer = malloc(buffer_size);
    if (buffer == NULL) {
        return false;
    }

    size_t total_read = 0;
    size_t header_end_offset = 0;

    while (1) {
        if (total_read == buffer_size) {
            if (buffer_size >= MAX_HEADER_SIZE) {
                free(buffer);
                return false;
            }
            size_t new_size = buffer_size * 2;
            char *new_buffer = realloc(buffer, new_size);
            if (new_buffer == NULL) {
                free(buffer);
                return false;
            }
            buffer = new_buffer;
            buffer_size = new_size;
        }

        ssize_t received = recv(client_fd, buffer + total_read, buffer_size - total_read, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return false;
        }
        if (received == 0) {
            free(buffer);
            return false;
        }

        total_read += (size_t)received;
        if (total_read >= 4) {
            for (size_t i = 0; i + 3 < total_read; ++i) {
                if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                    header_end_offset = i + 4;
                    goto headers_complete;
                }
            }
        }

        if (total_read >= MAX_HEADER_SIZE) {
            free(buffer);
            return false;
        }
    }

headers_complete:
    buffer[header_end_offset - 1] = '\0';

    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL) {
        free(buffer);
        return false;
    }
    *line_end = '\0';

    char full_target[2048];
    if (sscanf(buffer, "%7s %2047s", request->method, full_target) != 2) {
        free(buffer);
        return false;
    }

    char *query_sep = strchr(full_target, '?');
    if (query_sep != NULL) {
        *query_sep = '\0';
        snprintf(request->query, sizeof(request->query), "%s", query_sep + 1);
    }
    snprintf(request->path, sizeof(request->path), "%s", full_target);

    char *header_line = line_end + 2;
    while (header_line < buffer + header_end_offset - 2) {
        char *next_line = strstr(header_line, "\r\n");
        if (next_line == NULL) {
            break;
        }
        *next_line = '\0';

        if (strncasecmp(header_line, "Content-Length:", 15) == 0) {
            const char *value = header_line + 15;
            while (*value == ' ' || *value == '\t') {
                ++value;
            }
            request->content_length = (size_t)strtoull(value, NULL, 10);
        } else if (strncasecmp(header_line, "Content-Type:", 13) == 0) {
            const char *value = header_line + 13;
            while (*value == ' ' || *value == '\t') {
                ++value;
            }
            snprintf(request->content_type, sizeof(request->content_type), "%s", value);
        } else if (strncasecmp(header_line, "User-Agent:", 11) == 0) {
            const char *value = header_line + 11;
            while (*value == ' ' || *value == '\t') {
                ++value;
            }
            snprintf(request->user_agent, sizeof(request->user_agent), "%s", value);
        }

        header_line = next_line + 2;
    }

    if (request->content_length > MAX_BODY_SIZE) {
        free(buffer);
        return false;
    }

    if (request->content_length > 0) {
        char *body = malloc(request->content_length);
        if (body == NULL) {
            free(buffer);
            return false;
        }

        size_t buffered_body = total_read - header_end_offset;
        if (buffered_body > request->content_length) {
            buffered_body = request->content_length;
        }
        memcpy(body, buffer + header_end_offset, buffered_body);

        size_t body_read = buffered_body;
        while (body_read < request->content_length) {
            ssize_t received = recv(client_fd, body + body_read, request->content_length - body_read, 0);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(body);
                free(buffer);
                return false;
            }
            if (received == 0) {
                free(body);
                free(buffer);
                return false;
            }
            body_read += (size_t)received;
        }

        *body_out = body;
        *body_length_out = request->content_length;
    }

    free(buffer);
    return true;
}

static bool send_file_download(int client_fd, const char *base_dir, const char *query, const char *client_ip) {
    char relative_path[PATH_MAX];
    char decoded_path[PATH_MAX];
    if (find_query_param(query, "path", decoded_path, sizeof(decoded_path))) {
        if (!normalize_relative_path(decoded_path, relative_path, sizeof(relative_path))) {
            log_event(client_ip, "download_failed", "invalid path parameter");
            return send_text_response(client_fd, 400, "Bad Request", "Invalid path parameter.\n");
        }
    } else {
        relative_path[0] = '\0';
    }

    char target_dir[PATH_MAX];
    if (!resolve_directory_path(base_dir, relative_path, target_dir, sizeof(target_dir))) {
        log_event(client_ip, "download_failed", "directory not found");
        return send_text_response(client_fd, 404, "Not Found", "Directory not found.\n");
    }

    char filename[NAME_MAX];
    if (!find_query_param(query, "file", filename, sizeof(filename)) || !is_safe_filename(filename)) {
        log_event(client_ip, "download_failed", "invalid file parameter");
        return send_text_response(client_fd, 400, "Bad Request", "Invalid file parameter.\n");
    }

    char full_path[PATH_MAX];
    if (!join_path(full_path, sizeof(full_path), target_dir, filename)) {
        log_event(client_ip, "download_failed", "path too long");
        return send_text_response(client_fd, 400, "Bad Request", "Path too long.\n");
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "file=%.200s path=%.200s reason=not_found", filename, relative_path);
        log_event(client_ip, "download_failed", detail);
        return send_text_response(client_fd, 404, "Not Found", "File not found.\n");
    }

    if (access(full_path, R_OK) != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail), "file=%.200s path=%.200s reason=access_denied", filename, relative_path);
        log_event(client_ip, "download_failed", detail);
        return send_text_response(client_fd, 500, "Internal Server Error", "Unable to open file.\n");
    }

    char download_name[NAME_MAX];
    make_download_filename(filename, download_name, sizeof(download_name));
    char extra_headers[NAME_MAX + 64];
    int written = snprintf(extra_headers,
                           sizeof(extra_headers),
                           "Content-Disposition: attachment; filename=\"%s\"\r\n",
                           download_name);
    if (written < 0 || (size_t)written >= sizeof(extra_headers)) {
        log_event(client_ip, "download_failed", "header build failed");
        return send_text_response(client_fd, 500, "Internal Server Error", "Header build failed.\n");
    }

    if (!send_streamed_file(client_fd, full_path, guess_content_type(filename), extra_headers)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "file=%.200s path=%.200s reason=stream_failed", filename, relative_path);
        log_event(client_ip, "download_failed", detail);
        return send_text_response(client_fd, 500, "Internal Server Error", "Unable to stream file.\n");
    }
    char detail[512];
    snprintf(detail, sizeof(detail), "file=%.200s path=%.200s size=%lld", filename, relative_path, (long long)st.st_size);
    log_event(client_ip, "download_success", detail);
    return true;
}

static void handle_client(int client_fd, const char *base_dir, const char *frontend_dir, const char *client_ip) {
    HttpRequest request;
    char *body = NULL;
    size_t body_length = 0;

    if (!read_http_request(client_fd, &request, &body, &body_length)) {
        log_event(client_ip, "request_failed", "malformed http request");
        send_text_response(client_fd, 400, "Bad Request", "Malformed HTTP request.\n");
        free(body);
        return;
    }

    char access_detail[1024];
    snprintf(access_detail,
             sizeof(access_detail),
             "method=%.16s path=%.300s query=%.300s ua=%.300s",
             request.method,
             request.path,
             request.query[0] != '\0' ? request.query : "-",
             request.user_agent[0] != '\0' ? request.user_agent : "-");
    log_event(client_ip, "request", access_detail);

    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/api/list") == 0) {
        send_directory_listing_json(client_fd, base_dir, request.query);
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/api/download") == 0) {
        send_file_download(client_fd, base_dir, request.query, client_ip);
    } else if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/upload") == 0) {
        char relative_path[PATH_MAX];
        char decoded_path[PATH_MAX];
        if (find_query_param(request.query, "path", decoded_path, sizeof(decoded_path))) {
            if (!normalize_relative_path(decoded_path, relative_path, sizeof(relative_path))) {
                log_event(client_ip, "upload_failed", "invalid path parameter");
                send_json_message(client_fd, 400, "Bad Request", false, "目录参数无效。");
                free(body);
                return;
            }
        } else {
            relative_path[0] = '\0';
        }

        char target_dir[PATH_MAX];
        if (!resolve_directory_path(base_dir, relative_path, target_dir, sizeof(target_dir))) {
            log_event(client_ip, "upload_failed", "target directory not found");
            send_json_message(client_fd, 404, "Not Found", false, "目标目录不存在。");
            free(body);
            return;
        }

        char error_message[256];
        char saved_filename[NAME_MAX];
        if (request.content_length == 0 || body == NULL) {
            log_event(client_ip, "upload_failed", "empty upload body");
            send_json_message(client_fd, 400, "Bad Request", false, "上传内容为空。");
        } else if (strstr(request.content_type, "multipart/form-data") == NULL) {
            log_event(client_ip, "upload_failed", "invalid content type");
            send_json_message(client_fd, 400, "Bad Request", false, "上传必须使用 multipart/form-data。");
        } else if (!save_uploaded_file(target_dir,
                                       request.content_type,
                                       body,
                                       body_length,
                                       saved_filename,
                                       sizeof(saved_filename),
                                       error_message,
                                       sizeof(error_message))) {
            char detail[512];
            snprintf(detail, sizeof(detail), "path=%.200s reason=%.200s", relative_path, error_message);
            log_event(client_ip, "upload_failed", detail);
            send_json_message(client_fd, 400, "Bad Request", false, error_message);
        } else {
            char success_message[256];
            snprintf(success_message, sizeof(success_message), "上传成功: %.200s", saved_filename);
            char detail[512];
            snprintf(detail, sizeof(detail), "file=%.200s path=%.200s request_bytes=%zu", saved_filename, relative_path, body_length);
            log_event(client_ip, "upload_success", detail);
            send_json_message(client_fd, 200, "OK", true, success_message);
        }
    } else if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/favicon.ico") == 0) {
        send_response(client_fd,
                      204,
                      "No Content",
                      "image/x-icon",
                      NULL,
                      0,
                      "Cache-Control: public, max-age=86400\r\n");
    } else if (strcmp(request.method, "GET") == 0 &&
               (strcmp(request.path, "/") == 0 || strcmp(request.path, "/index.html") == 0)) {
        send_index_page(client_fd, base_dir, frontend_dir, request.query);
    } else if (strcmp(request.method, "GET") == 0) {
        char static_file[PATH_MAX];
        if (!resolve_frontend_file(frontend_dir, request.path, static_file, sizeof(static_file))) {
            send_text_response(client_fd, 404, "Not Found", "Route not found.\n");
        } else if (!send_streamed_file(client_fd,
                                       static_file,
                                       guess_content_type(static_file),
                                       static_cache_headers(request.path))) {
            send_text_response(client_fd, 500, "Internal Server Error", "Failed to load static file.\n");
        }
    } else {
        send_text_response(client_fd, 404, "Not Found", "Route not found.\n");
    }

    free(body);
}

static void *handle_client_thread(void *arg) {
    ClientContext *context = arg;
    if (context == NULL) {
        return NULL;
    }

    handle_client(context->client_fd, context->base_dir, context->frontend_dir, context->client_ip);
    close(context->client_fd);
    free(context);
    return NULL;
}

static int create_server_socket(uint16_t port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, BACKLOG) != 0) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static bool ensure_directory_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool create_directory_if_missing(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (errno != ENOENT) {
        return false;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return false;
    }

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_directory_tree(const char *path) {
    char buffer[PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }

    memcpy(buffer, path, length + 1);
    while (length > 1 && buffer[length - 1] == '/') {
        buffer[--length] = '\0';
    }

    if (strcmp(buffer, "/") == 0) {
        return true;
    }

    for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (!create_directory_if_missing(buffer)) {
            return false;
        }
        *cursor = '/';
    }

    return create_directory_if_missing(buffer);
}

static bool trim_last_path_component(char *path) {
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }
    *slash = '\0';
    return true;
}

static bool resolve_frontend_dir(const char *argv0, char *frontend_dir, size_t dir_size) {
    char exe_path[PATH_MAX];
    if (realpath(argv0, exe_path) != NULL) {
        if (trim_last_path_component(exe_path)) {
            char candidate[PATH_MAX];
            int written = snprintf(candidate, sizeof(candidate), "%s/../frontend", exe_path);
            if (written >= 0 && (size_t)written < sizeof(candidate) &&
                realpath(candidate, frontend_dir) != NULL &&
                ensure_directory_exists(frontend_dir)) {
                return true;
            }
        }
    }

    if (realpath("./frontend", frontend_dir) != NULL && ensure_directory_exists(frontend_dir)) {
        return true;
    }

    if (dir_size > 0) {
        frontend_dir[0] = '\0';
    }
    return false;
}

static bool resolve_log_dir(const char *directory, char *log_dir, size_t log_dir_size) {
    if (directory == NULL || directory[0] == '\0' || log_dir_size == 0) {
        return false;
    }

    if (realpath(directory, log_dir) != NULL) {
        return ensure_directory_exists(log_dir);
    }

    if (errno != ENOENT) {
        return false;
    }

    char candidate[PATH_MAX];
    if (directory[0] == '/') {
        int written = snprintf(candidate, sizeof(candidate), "%s", directory);
        if (written < 0 || (size_t)written >= sizeof(candidate)) {
            return false;
        }
    } else {
        char current_dir[PATH_MAX];
        if (realpath(".", current_dir) == NULL) {
            return false;
        }
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", current_dir, directory);
        if (written < 0 || (size_t)written >= sizeof(candidate)) {
            return false;
        }
    }

    if (!ensure_directory_tree(candidate)) {
        return false;
    }

    if (realpath(candidate, log_dir) == NULL) {
        return false;
    }

    return ensure_directory_exists(log_dir);
}

int main(int argc, char *argv[]) {
    uint16_t port = 8080;
    const char *directory = ".";
    const char *log_directory = ".";

    if (argc > 4) {
        fprintf(stderr, "Usage: %s [directory] [port] [log_directory]\n", argv[0]);
        return 1;
    }

    if (argc >= 2) {
        directory = argv[1];
    }
    if (argc >= 3) {
        long parsed = strtol(argv[2], NULL, 10);
        if (parsed <= 0 || parsed > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
        port = (uint16_t)parsed;
    }
    if (argc >= 4) {
        log_directory = argv[3];
    }

    char base_dir[PATH_MAX];
    if (realpath(directory, base_dir) == NULL) {
        perror("realpath");
        return 1;
    }
    if (!ensure_directory_exists(base_dir)) {
        fprintf(stderr, "Directory does not exist: %s\n", base_dir);
        return 1;
    }

    char frontend_dir[PATH_MAX];
    if (!resolve_frontend_dir(argv[0], frontend_dir, sizeof(frontend_dir))) {
        fprintf(stderr, "Frontend directory not found.\n");
        return 1;
    }

    if (!resolve_log_dir(log_directory, g_log_dir, sizeof(g_log_dir))) {
        fprintf(stderr, "Log directory is unavailable: %s\n", log_directory);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    int server_fd = create_server_socket(port);
    if (server_fd < 0) {
        perror("create_server_socket");
        return 1;
    }

    printf("Open http://127.0.0.1:%u\n", port);
    printf("Files: %s\n", base_dir);
    printf("Logs: %s\n", g_log_dir);

    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            snprintf(client_ip, sizeof(client_ip), "unknown");
        }

        ClientContext *context = malloc(sizeof(*context));
        if (context == NULL) {
            send_text_response(client_fd, 500, "Internal Server Error", "Server is busy.\n");
            close(client_fd);
            continue;
        }

        context->client_fd = client_fd;
        context->base_dir = base_dir;
        context->frontend_dir = frontend_dir;
        snprintf(context->client_ip, sizeof(context->client_ip), "%s", client_ip);

        pthread_t thread;
        int create_result = pthread_create(&thread, NULL, handle_client_thread, context);
        if (create_result != 0) {
            free(context);
            send_text_response(client_fd, 500, "Internal Server Error", "Server is busy.\n");
            close(client_fd);
            continue;
        }

        pthread_detach(thread);
    }

    close(server_fd);
    return 0;
}
