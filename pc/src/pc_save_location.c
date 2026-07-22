#include "pc_save_location.h"
#include "pc_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#include <windows.h>
#include <process.h>  /* _getpid */
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#define LOCK_FILENAME ".LOCK"
#define PC_SAVE_LOCK_HEARTBEAT_SECONDS (60)
#define PC_SAVE_LOCK_STALE_SECONDS (30 * 60)
#define PC_SAVE_ROOT_DEFAULT "save"

static char     s_save_root[512] = PC_SAVE_ROOT_DEFAULT;
static char     s_lock_path[512] = {0};
static int      s_lock_held = 0;
static int      s_atexit_registered = 0;
static time_t   s_last_heartbeat = 0;

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

static void build_lock_contents(char* buf, size_t buf_size) {
    char host[128];

    get_hostname(host, sizeof(host));

    snprintf(buf, buf_size,
             "host=%s\n"
             "pid=%ld\n"
             "time=%lld\n",
             host, get_pid(), (long long)time(NULL));
}

/* Best-effort read of an existing lock file's host/time for the busy
 * message. A foreign/malformed lock file still blocks access either way -
 * it just won't have nice details to show. */
static void describe_existing_lock(const char* path, char* err, int err_size) {
    FILE* f;
    char host[128] = "an unknown computer";
    char when[64] = "an unknown time";
    char line[256];

    if (err == NULL || err_size <= 0) return;

    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "host=", 5) == 0) {
                snprintf(host, sizeof(host), "%s", line + 5);
            } else if (strncmp(line, "time=", 5) == 0) {
                snprintf(when, sizeof(when), "%s", line + 5);
            }
        }
        fclose(f);
    }

    snprintf(err, err_size,
             "Someone else is already playing this town!\n\n"
             "Currently playing: %s\n"
             "Since timestamp: %s\n\n",
             host, when);
}

/* True if path's recorded time is older than PC_SAVE_LOCK_STALE_SECONDS. */
static int lock_file_is_stale(const char* path) {
    FILE* f;
    char line[256];
    time_t lock_time = 0;
    long long parsed_time = 0;
    int found = 0;

    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "time=", 5) == 0) {
            if (sscanf(line + 5, "%lld", &parsed_time) == 1) {
                lock_time = (time_t)parsed_time;
                found = 1;
            }
            break;
        }
    }
    fclose(f);

    if (!found) return 0;
    return difftime(time(NULL), lock_time) > PC_SAVE_LOCK_STALE_SECONDS;
}

static int try_create_lock(const char* path, const char* contents, char* err, int err_size) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD last_err = GetLastError();
        if (last_err == ERROR_FILE_EXISTS) return 0;
        if (last_err && err_size > 0) {
            snprintf(err, err_size,
                     "Could not create lock file '%s' (error %lu).",
                     path, (unsigned long)err);
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
        if (err && err_size > 0) {
            snprintf(err, err_size,
                     "Could not create lock file '%s' (%s).",
                     path, strerror(errno));
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

static void release_atexit(void) {
    pc_save_lock_release();
}

int pc_save_lock_acquire(const char* dir, char* err, int err_size) {
    char path[512];
    char contents[256];
    int attempt;

    if (s_lock_held) return 1;

    snprintf(path, sizeof(path), "%s/%s", dir, LOCK_FILENAME);
    build_lock_contents(contents, sizeof(contents));

    for (attempt = 0; attempt < 2; attempt++) {
        int created = try_create_lock(path, contents, err, err_size);

        if (created == 1) {
            snprintf(s_lock_path, sizeof(s_lock_path), "%s", path);
            s_lock_held = 1;
            s_last_heartbeat = time(NULL);

            /* Still try to clean up if not using clean shutdown path. */
            if (!s_atexit_registered) {
                atexit(release_atexit);
                s_atexit_registered = 1;
            }
            return 1;
        }

        if (created < 0) {
            return 0;
        }

        if (attempt == 0 && lock_file_is_stale(path)) {
            remove(path);
            continue;
        }

        break;
    }

    describe_existing_lock(path, err, err_size);
    return 0;
}

void pc_save_lock_release(void) {
    if (!s_lock_held) return;
    remove(s_lock_path);
    s_lock_held = 0;
    s_lock_path[0] = '\0';
    s_last_heartbeat = 0;
}

void pc_save_lock_heartbeat(void) {
    time_t now;
    char contents[256];

    if (!s_lock_held) return;

    now = time(NULL);
    if (difftime(now, s_last_heartbeat) < PC_SAVE_LOCK_HEARTBEAT_SECONDS) {
        return;
    }

    build_lock_contents(contents, sizeof(contents));
    overwrite_owned_lock(s_lock_path, contents);
    s_last_heartbeat = now;
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