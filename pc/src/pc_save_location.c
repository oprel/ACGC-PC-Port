#include "pc_save_location.h"
#include "pc_settings.h"
#include "pc_platform.h"
#include "m_common_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#include <windows.h>
#include <winhttp.h>
#include <process.h>  /* _getpid */
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#define LOCK_FILENAME ".LOCK"
#define PC_SAVE_LOCK_HEARTBEAT_MIN_SECONDS (20)
#define PC_SAVE_LOCK_HEARTBEAT_MAX_SECONDS (40)
#define PC_SAVE_LOCK_STALE_SECONDS (30 * 60)
#define PC_SAVE_ROOT_DEFAULT "save"
#define PC_SAVE_REMOTE_DIR "save_remote"
#define PC_REMOTE_MAX_BYTES (4 * 1024 * 1024)

static char     s_save_root[512] = PC_SAVE_ROOT_DEFAULT;
static char     s_lock_path[512] = {0};
static char     s_remote_url[512] = {0};
static char     s_remote_token[128] = {0};
#ifdef _WIN32
static wchar_t  s_remote_host[256] = {0};
static char     s_remote_path[512] = {0};
static INTERNET_PORT s_remote_port = 0;
static int      s_remote_https = 0;
#endif
static int      s_lock_held = 0;
static int      s_atexit_registered = 0;
static time_t   s_lock_since = 0;
static time_t   s_last_heartbeat = 0;
static int      s_heartbeat_interval = 0;

static int next_heartbeat_interval(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    return PC_SAVE_LOCK_HEARTBEAT_MIN_SECONDS +
           rand() % (PC_SAVE_LOCK_HEARTBEAT_MAX_SECONDS - PC_SAVE_LOCK_HEARTBEAT_MIN_SECONDS + 1);
}

static void get_hostname(char* out, int out_size) {
#ifdef _WIN32
    DWORD len = (DWORD)out_size;
    if (!GetComputerNameA(out, &len)) {
        snprintf(out, out_size, "unknown-host");
    }
#else
    if (gethostname(out, out_size) != 0) {
        snprintf(out, out_size, "unknown-host");
    }
    out[out_size - 1] = '\0';
#endif
}

static long get_pid(void) {
#ifdef _WIN32
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static void get_owner_name(char* out, size_t out_size) {
    char host[128];
    char player[PLAYER_NAME_LEN + 1];
    size_t i;

    if (out_size <= 0 || !out) return;

    get_hostname(host, sizeof(host));
    player[0] = '\0';

    if (Now_Private != NULL && !mPr_NullCheckPlayerName(Now_Private->player_ID.player_name)) {
        for (i = 0; i < PLAYER_NAME_LEN; ++i) {
            unsigned char c = Now_Private->player_ID.player_name[i];
            if (c == '\0' || c == ' ') break;
            if (i + 1 >= sizeof(player)) break;
            player[i] = (char)c;
            player[i + 1] = '\0';
        }
    }

    if (player[0] != '\0') {
        snprintf(out, out_size, "%s (%s)", player, host);
    } else {
        snprintf(out, out_size, "%s", host);
    }
}

int pc_save_is_remote(void) {
    return s_remote_url[0] != '\0';
}

/* --- .LOCK file helper functions --- */

static int read_lock_file(const char* path, char* buf, size_t buf_size) {
    FILE* f = fopen(path, "r");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, buf_size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

static int get_lock_field(const char* body, const char* key, char* out, int out_size) {
    size_t keylen = strlen(key);
    const char* line = body;

    if (out_size <= 0) return 0;

    while (line && *line) {
        const char* nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t)(nl - line) : strlen(line);

        if (linelen > keylen && line[keylen] == '=' && strncmp(line, key, keylen) == 0) {
            size_t vlen = linelen - keylen - 1;
            if (vlen >= (size_t)out_size) vlen = out_size - 1;
            memcpy(out, line + keylen + 1, vlen);
            out[vlen] = '\0';
            return 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

static void fatal_lock_error_and_exit(const char* body) {
    char host[128] = "an unknown computer";
    char val[32];
    char since_str[64];
    char message[512];
    time_t since = 0;

    get_lock_field(body, "host", host, sizeof(host));
    if (get_lock_field(body, "since", val, sizeof(val))) {
        long long v;
        if (sscanf(val, "%lld", &v) == 1) since = (time_t)v;
    }

    if (since <= 0) {
        snprintf(since_str, sizeof(since_str), "an unknown amount of time");
    }else{
        long mins = (long)difftime(time(NULL), since) / 60;
        snprintf(since_str, sizeof(since_str), "%ldh %ldm", mins / 60, mins % 60);
    }
   
    snprintf(message, sizeof(message),
             "Someone else is already playing this town!\n\n"
             "Currently playing: %s\n"
             "Playing for: %s\n\n",
             host, since_str);
    pc_fatal_error_and_exit("Animal Crossing - Load Error", message);
}

static void build_lock_contents(char* buf, size_t buf_size, time_t since) {
    char owner[256];

    get_owner_name(owner, sizeof(owner));
    snprintf(buf, buf_size,
             "owner=%s\n"
             "pid=%ld\n"
             "since=%lld\n"
             "time=%lld\n",
             owner, get_pid(), (long long)since, (long long)time(NULL));
}

static int try_create_lock(const char* path, const char* contents) {
    char err_desc[512];
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_EXISTS) return 0;
        if (err){
            snprintf(err_desc, sizeof(err_desc), "Could not create lock file '%s' (error %lu).", path, (unsigned long)err);
            pc_fatal_error_and_exit("Animal Crossing - Save Lock Error", err_desc);
        }
        
        return -1;
    }
    {
        DWORD written;
        WriteFile(h, contents, (DWORD)strlen(contents), &written, NULL);
        CloseHandle(h);
    }
    return 1;
#else
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return 0;
        if (errno){
            snprintf(err_desc, sizeof(err_desc), "Could not create lock file '%s' (%s).", path, strerror(errno));
            pc_fatal_error_and_exit("Animal Crossing - Save Lock Error", err_desc);
        }
        return -1;
    }
    {
        ssize_t n = write(fd, contents, strlen(contents));
        (void)n;
        close(fd);
    }
    return 1;
#endif
}

static void overwrite_owned_lock(const char* path, const char* contents) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, contents, (DWORD)strlen(contents), &written, NULL);
        CloseHandle(h);
    }
#else
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t n = write(fd, contents, strlen(contents));
        (void)n;
        close(fd);
    }
#endif
}


/* --- Remote .LOCK functionality --- */
#ifdef _WIN32

static void pc_save_remote_parse_url(const char* url) {
    wchar_t wide[1024], path[512];
    URL_COMPONENTS c;

    MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])));

    memset(&c, 0, sizeof(c));
    c.dwStructSize = sizeof(c);
    c.lpszHostName = s_remote_host;
    c.dwHostNameLength = sizeof(s_remote_host) / sizeof(s_remote_host[0]);
    c.lpszUrlPath = path;
    c.dwUrlPathLength = sizeof(path) / sizeof(path[0]);

    if (!WinHttpCrackUrl(wide, 0, 0, &c)) {
        fprintf(stderr, "[PC] Could not parse save_location URL.\n");
        return;
    }
    s_remote_port = c.nPort;
    s_remote_https = (c.nScheme == INTERNET_SCHEME_HTTPS);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s_remote_path, sizeof(s_remote_path), NULL, NULL);
}


static int pc_save_http_request(const char* method, const char* query,
                                 const void* body, size_t body_len,
                                 char* out, size_t out_cap, size_t* out_len) {
    char object[1024];
    wchar_t wobject[1024], wmethod[8];
    HINTERNET session, connect, request = NULL;
    DWORD status = 0, status_size = sizeof(status);
    char* buf = NULL;
    size_t len = 0, cap = 0;
    int ok = 0;
    char err_desc[512];

    if (out && out_cap) out[0] = '\0';
    if (out_len) *out_len = 0;

    snprintf(object, sizeof(object), "%s?%s", s_remote_path, query);
    MultiByteToWideChar(CP_UTF8, 0, object, -1, wobject, (int)(sizeof(wobject) / sizeof(wobject[0])));
    MultiByteToWideChar(CP_UTF8, 0, method, -1, wmethod, (int)(sizeof(wmethod) / sizeof(wmethod[0])));

    session = WinHttpOpen(L"ACGC-PC-Port/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Could not start the HTTP client.");
        return 0;
    }

    connect = WinHttpConnect(session, s_remote_host, s_remote_port, 0);
    if (connect) {
        request = WinHttpOpenRequest(connect, wmethod, wobject, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, s_remote_https ? WINHTTP_FLAG_SECURE : 0);
    }

    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)body, (DWORD)body_len, (DWORD)body_len, 0) &&
        WinHttpReceiveResponse(request, NULL)) {

        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

        for (;;) {
            DWORD avail = 0, read = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            if (len + avail > cap) {
                cap = len + avail + 4096;
                buf = (char*)realloc(buf, cap);
                if (!buf) break;
            }
            if (!WinHttpReadData(request, buf + len, avail, &read) || read == 0) break;
            len += read;
        }
        if (out && out_cap && len) {
            size_t n = len < out_cap - 1 ? len : out_cap - 1;
            memcpy(out, buf, n);
            out[n] = '\0';
        }
        if (out_len) *out_len = len;
        if (status >= 200 && status < 300) {
            ok = 1;
        } else if (status == 409) {
            fatal_lock_error_and_exit(out);
        } else {
            snprintf(err_desc, sizeof(err_desc), "UNKNOWN ERROR [%ld]: %s", status, out);
        }
    } else {
        snprintf(err_desc, sizeof(err_desc), "Could not reach the save server. Check your internet connection.");
    }

    free(buf);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok) pc_fatal_error_and_exit("Animal Crossing - Save Error", err_desc);

    return ok;
}

#else /* !_WIN32 */

static void pc_save_remote_parse_url(const char* url) { (void)url; }

static int pc_save_http_request(const char* method, const char* query,
                                 const void* body, size_t body_len,
                                 char* out, size_t out_cap, size_t* out_len) {
    (void)method; (void)query; (void)body; (void)body_len; (void)out; (void)out_cap; (void)out_len;
    pc_fatal_error_and_exit("Animal Crossing - Save Error", "Remote save locations only implemented on Windows.");
    return 0;
}

#endif /* _WIN32 */

static int pc_save_remote_call(const char* method, const char* action, const char* extra,
                                const void* body, size_t body_len,
                                char* out, size_t out_cap, size_t* out_len) {
    char query[256];
    snprintf(query, sizeof(query), "action=%s%s", action, extra ? extra : "");
    return pc_save_http_request(method, query, body, body_len, out, out_cap, out_len);
}

int pc_save_http_load_to_file(const char* path) {
    char* buf;
    size_t len = 0;
    FILE* fp;

    if (!pc_save_is_remote()) {
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Not a remote save location.");
        return 0;
    }

    buf = (char*)malloc(PC_REMOTE_MAX_BYTES);
    if (!buf) {
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Out of memory.");
        return 0;
    }

    if (!pc_save_remote_call("GET", "load", NULL, NULL, 0, buf, PC_REMOTE_MAX_BYTES, &len)) {
        free(buf);
        return 0;
    }
    if (len == 0) {
        free(buf);
        remove(path); /* brand-new town - no save on the server yet */
        return 1;
    }

    if (len < 64 || memcmp(buf, "GAF", 3) != 0) {
        free(buf);
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Save server returned an invalid save file.");
        return 0;
    }

    fp = fopen(path, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len) {
        if (fp) fclose(fp);
        free(buf);
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Could not write the remote save file.");
        return 0;
    }
    fclose(fp);
    free(buf);
    return 1;
}

int pc_save_http_save_from_file(const char* path) {
    char extra[256];
    char* data = NULL;
    long size = 0;
    FILE* fp;
    int ok;

    if (!pc_save_is_remote()) {
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Not a remote save location.");
        return 0;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        pc_fatal_error_and_exit("Animal Crossing - Save Error", "Could not open the remote save file.");
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);

    if (size > 0) {
        data = (char*)malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, fp) != (size_t)size) {
            fclose(fp);
            free(data);
            pc_fatal_error_and_exit("Animal Crossing - Save Error", "Could not read the remote save file.");
            return 0;
        }
    }
    fclose(fp);

    snprintf(extra, sizeof(extra), "&token=%s", s_remote_token);
    ok = pc_save_remote_call("POST", "save", extra, data, (size_t)(size > 0 ? size : 0),
                              NULL, 0, NULL);
    free(data);
    return ok;
}

/* --- Public .LOCK file functions --- */

int pc_save_lock_acquire(const char* dir) {
    char path[512];
    char contents[256];
    time_t now;
    int attempt;

    if (s_lock_held) return 1;

    if (pc_save_is_remote()) {
        char resp[512];
        char owner[256];
        char extra[256];

        get_owner_name(owner, sizeof(owner));
        snprintf(extra, sizeof(extra), "&owner=%s&pid=%ld", owner, get_pid());

        if (!pc_save_remote_call("POST", "acquire", extra, NULL, 0,
                                  resp, sizeof(resp), NULL)) {
            return 0;
        }
        if (!get_lock_field(resp, "token", s_remote_token, sizeof(s_remote_token))) {
            pc_fatal_error_and_exit("Animal Crossing - Save Error", "Save server did not return a lock token.");
            return 0;
        }
        s_lock_held = 1;
        s_last_heartbeat = time(NULL);
        if (!s_atexit_registered) {
            atexit(pc_save_lock_release);
            s_atexit_registered = 1;
        }
        return 1;
    }

    now = time(NULL);
    snprintf(path, sizeof(path), "%s/%s", dir, LOCK_FILENAME);
    build_lock_contents(contents, sizeof(contents), now);

    for (attempt = 0; attempt < 2; attempt++) {
        int created = try_create_lock(path, contents);

        if (created == 1) {
            snprintf(s_lock_path, sizeof(s_lock_path), "%s", path);
            s_lock_held = 1;
            s_lock_since = now;
            s_last_heartbeat = now;

            /* Still try to clean up if not using clean shutdown path. */
            if (!s_atexit_registered) {
                atexit(pc_save_lock_release);
                s_atexit_registered = 1;
            }
            return 1;
        }

        if (created < 0) {
            return 0;
        }

        if (attempt == 0) { /* check if .LOCK file is stale */
            char buf[256] = "";
            char val[32];
            long long parsed_time;

            if (!read_lock_file(path, buf, sizeof(buf))) return 0;
            if (!get_lock_field(buf, "time", val, sizeof(val))) return 0;
            if (sscanf(val, "%lld", &parsed_time) != 1) return 0;

            if(difftime(time(NULL), (time_t)parsed_time) > PC_SAVE_LOCK_STALE_SECONDS){
                remove(path);
                continue;
            }
        }

        break;
    }

    {
        char buf[256] = "";
        read_lock_file(path, buf, sizeof(buf));
        fatal_lock_error_and_exit(buf);
    }
    return 0;
}

void pc_save_lock_release(void) {
    char extra[256];

    if (!s_lock_held) return;

    if (pc_save_is_remote()) {
        snprintf(extra, sizeof(extra), "&token=%s", s_remote_token);
        pc_save_remote_call("POST", "release", extra, NULL, 0, NULL, 0, NULL);
        s_remote_token[0] = '\0';
    } else {
        remove(s_lock_path);
    }
    s_lock_held = 0;
    s_lock_path[0] = '\0';
    s_lock_since = 0;
    s_last_heartbeat = 0;
    s_heartbeat_interval = 0;
}

void pc_save_lock_heartbeat(void) {
    time_t now;
    char contents[256];

    if (!s_lock_held) return;

    now = time(NULL);
    if (s_heartbeat_interval == 0) s_heartbeat_interval = next_heartbeat_interval();
    if (difftime(now, s_last_heartbeat) < s_heartbeat_interval) {
        return;
    }

    if (pc_save_is_remote()) {
        char extra[256];
        char owner[256];
        get_owner_name(owner, sizeof(owner));
        snprintf(extra, sizeof(extra), "&token=%s&owner=%s", s_remote_token, owner);
        pc_save_remote_call("POST", "renew", extra, NULL, 0, NULL, 0, NULL);
    } else {
        build_lock_contents(contents, sizeof(contents), s_lock_since);
        overwrite_owned_lock(s_lock_path, contents);
    }
    s_last_heartbeat = now;
    s_heartbeat_interval = next_heartbeat_interval();
}


/* --- Save location --- */

static void trim_trailing_slashes(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\')) {
        s[--len] = '\0';
    }
}


static void mkdir_recursive(const char* path) {
    char tmp[512];
    size_t i, len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char sep = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
#ifdef _WIN32
                _mkdir(tmp);
#else
                mkdir(tmp, 0755);
#endif
            }
            tmp[i] = sep;
        }
    }

#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

void pc_save_location_init(void) {
    const char* custom = g_pc_settings.save_location;
    struct stat st;

    if (custom[0] != '\0' &&
        (strncmp(custom, "http://", 7) == 0 || strncmp(custom, "https://", 8) == 0)) {
        snprintf(s_remote_url, sizeof(s_remote_url), "%s", custom);
        pc_save_remote_parse_url(custom);
        snprintf(s_save_root, sizeof(s_save_root), "%s", PC_SAVE_REMOTE_DIR);
        mkdir_recursive(s_save_root);
        printf("[PC] Save location (remote): %s\n", s_save_root);
        return;
    }

    if (custom[0] != '\0') {
        snprintf(s_save_root, sizeof(s_save_root), "%s", custom);
        trim_trailing_slashes(s_save_root);
        if (s_save_root[0] == '\0') {
            snprintf(s_save_root, sizeof(s_save_root), "%s", PC_SAVE_ROOT_DEFAULT);
        }
    } else {
        snprintf(s_save_root, sizeof(s_save_root), "%s", PC_SAVE_ROOT_DEFAULT);
    }

    mkdir_recursive(s_save_root);

    if (stat(s_save_root, &st) != 0) {
        fprintf(stderr,
                "[PC] WARNING: save_location '%s' could not be created or reached "
                "(is the network path available?). Falling back to the local "
                "'%s' folder.\n",
                s_save_root, PC_SAVE_ROOT_DEFAULT);
        snprintf(s_save_root, sizeof(s_save_root), "%s", PC_SAVE_ROOT_DEFAULT);
        mkdir_recursive(s_save_root);
    }

    printf("[PC] Save location: %s\n", s_save_root);
}

const char* pc_save_root(void) {
    return s_save_root;
}