/*
 * Web UI server — single entry point (no terminal role switching).
 *   ./web_server [port]
 * Browser login picks Administrator vs User. Cookie session.
 * Default port: 8765 → http://127.0.0.1:8765/
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "shared.h"
#include "doc_control.h"
#include "registry.h"
#include "user_auth.h"

#define DEFAULT_PORT 8765
#define REQ_CAP (256 * 1024)
#define DOC_CAP (256 * 1024)
#define MAX_SESS 64

typedef struct {
    char sid[80];
    User u;
    bool in_use;
} WebSession;

static WebSession g_sessions[MAX_SESS];
static User g_actor;
static bool g_guest = true;
static int g_port = DEFAULT_PORT;

#define ACTOR_OWNER (!g_guest && (g_actor.priority == PRIORITY_OWNER))

static void gen_session_id(char *out, size_t cap) {
    static unsigned seq;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out, cap, "s%lu_%lu_%u_%d", (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec, seq++, (int)getpid());
}

static void actor_reset_guest(void) {
    memset(&g_actor, 0, sizeof(g_actor));
    g_guest = true;
}

static void actor_bind_user(const User *u) {
    g_actor = *u;
    g_actor.pid = getpid();
    g_guest = false;
}

static User *session_by_sid(const char *sid) {
    if (!sid || !sid[0]) {
        return NULL;
    }
    for (int i = 0; i < MAX_SESS; i++) {
        if (g_sessions[i].in_use && strcmp(g_sessions[i].sid, sid) == 0) {
            return &g_sessions[i].u;
        }
    }
    return NULL;
}

static const char *session_create(const User *u) {
    for (int i = 0; i < MAX_SESS; i++) {
        if (!g_sessions[i].in_use) {
            gen_session_id(g_sessions[i].sid, sizeof(g_sessions[i].sid));
            g_sessions[i].u = *u;
            g_sessions[i].u.pid = getpid();
            g_sessions[i].in_use = true;
            return g_sessions[i].sid;
        }
    }
    return NULL;
}

static void session_destroy(const char *sid) {
    if (!sid || !sid[0]) {
        return;
    }
    for (int i = 0; i < MAX_SESS; i++) {
        if (g_sessions[i].in_use && strcmp(g_sessions[i].sid, sid) == 0) {
            g_sessions[i].in_use = false;
            g_sessions[i].sid[0] = '\0';
            return;
        }
    }
}

static void parse_cookie_sid(const char *headers, char *sid_out, size_t cap) {
    sid_out[0] = '\0';
    const char *p = strstr(headers, "Cookie:");
    if (!p) {
        p = strstr(headers, "cookie:");
    }
    if (!p) {
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *needle = "doc_sid=";
    const char *s = strstr(p, needle);
    if (!s) {
        return;
    }
    s += strlen(needle);
    size_t i = 0;
    while (*s && *s != ';' && *s != ' ' && *s != '\r' && *s != '\n' && i + 1 < cap) {
        sid_out[i++] = *s++;
    }
    sid_out[i] = '\0';
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

static void http_headers(int fd, int status, const char *status_text, const char *ctype, size_t body_len) {
    char h[512];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     status, status_text, ctype, body_len);
    if (n > 0 && (size_t)n < sizeof(h)) {
        (void)send_all(fd, h, (size_t)n);
    }
}

static void http_text(int fd, int status, const char *ctype, const char *body, size_t body_len) {
    const char *st = status == 200 ? "OK"
                     : status == 400 ? "Bad Request"
                     : status == 401 ? "Unauthorized"
                     : status == 403 ? "Forbidden"
                     : status == 404 ? "Not Found"
                     : status == 409 ? "Conflict"
                                     : "Error";
    http_headers(fd, status, st, ctype, body_len);
    if (body && body_len) {
        (void)send_all(fd, body, body_len);
    }
}

static void http_json(int fd, int status, const char *json) {
    http_text(fd, status, "application/json; charset=utf-8", json, strlen(json));
}

static void http_json_set_cookie(int fd, int status, const char *json, const char *set_cookie_line_crlf) {
    const char *st = status == 200 ? "OK" : "Bad Request";
    char h[1024];
    size_t jlen = strlen(json);
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     status, st, set_cookie_line_crlf, jlen);
    if (n > 0 && (size_t)n < sizeof(h)) {
        (void)send_all(fd, h, (size_t)n);
    }
    (void)send_all(fd, json, jlen);
}

static bool can_read_doc(void) {
    if (g_guest) {
        return false;
    }
    if (g_actor.priority == PRIORITY_OWNER) {
        return true;
    }
    return g_actor.access_type == ACCESS_READ_ONLY || g_actor.access_type == ACCESS_BOTH;
}

static bool can_write_doc(void) {
    if (g_guest) {
        return false;
    }
    if (g_actor.priority == PRIORITY_OWNER) {
        return true;
    }
    return g_actor.access_type == ACCESS_WRITE_ONLY || g_actor.access_type == ACCESS_BOTH;
}

static int read_until_headers(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, buf + total, 1, 0);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return (int)total;
        }
    }
    return -1;
}

static void method_path(const char *req, char *method, size_t mlen, char *path, size_t plen) {
    method[0] = path[0] = '\0';
    const char *sp = strpbrk(req, " \t");
    if (!sp) {
        return;
    }
    size_t ml = (size_t)(sp - req);
    if (ml >= mlen) {
        ml = mlen - 1;
    }
    memcpy(method, req, ml);
    method[ml] = '\0';

    while (*sp == ' ' || *sp == '\t') {
        sp++;
    }
    const char *sp2 = strpbrk(sp, " \t\r\n");
    if (!sp2) {
        return;
    }
    size_t pl = (size_t)(sp2 - sp);
    if (pl >= plen) {
        pl = plen - 1;
    }
    memcpy(path, sp, pl);
    path[pl] = '\0';

    char *q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }
}

static long header_content_length(const char *headers) {
    const char *p = strstr(headers, "Content-Length:");
    if (!p) {
        p = strstr(headers, "content-length:");
    }
    if (!p) {
        return -1;
    }
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return strtol(p, NULL, 10);
}

static int recv_body(int fd, char *buf, size_t cap, long want) {
    if (want < 0 || (size_t)want >= cap) {
        return -1;
    }
    size_t got = 0;
    while ((long)got < want) {
        ssize_t n = recv(fd, buf + got, cap - 1 - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}

static bool path_safe(const char *p) {
    if (strstr(p, "..") != NULL) {
        return false;
    }
    return true;
}

static void send_static(int fd, const char *rel) {
    if (!path_safe(rel)) {
        http_json(fd, 404, "{\"error\":\"bad path\"}");
        return;
    }
    char fpath[512];
    snprintf(fpath, sizeof(fpath), "web/%s", rel);
    FILE *fp = fopen(fpath, "rb");
    if (!fp) {
        http_json(fd, 404, "{\"error\":\"not found\"}");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0 || sz > (long)DOC_CAP) {
        fclose(fp);
        http_json(fd, 400, "{\"error\":\"file too large\"}");
        return;
    }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        http_json(fd, 500, "{\"error\":\"oom\"}");
        return;
    }
    size_t r = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[r] = '\0';

    const char *ctype = "application/octet-stream";
    if (strstr(rel, ".html")) {
        ctype = "text/html; charset=utf-8";
    } else if (strstr(rel, ".css")) {
        ctype = "text/css; charset=utf-8";
    } else if (strstr(rel, ".js")) {
        ctype = "application/javascript; charset=utf-8";
    }

    http_headers(fd, 200, "OK", ctype, r);
    (void)send_all(fd, buf, r);
    free(buf);
}

static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            continue;
        } else if (c < 32) {
            continue;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void handle_bootstrap(int fd) {
    if (g_guest) {
        http_json(fd, 200, "{\"mode\":\"guest\",\"user\":\"\",\"canRead\":false,\"canWrite\":false,\"priority\":0}");
        return;
    }
    char esc[160];
    json_escape(g_actor.name, esc, sizeof(esc));
    char json[512];
    const char *mode = ACTOR_OWNER ? "owner" : "user";
    int pr = g_actor.priority;
    const char *cr = can_read_doc() ? "true" : "false";
    const char *cw = can_write_doc() ? "true" : "false";
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"user\":\"%s\",\"canRead\":%s,\"canWrite\":%s,\"priority\":%d}", mode, esc, cr, cw, pr);
    http_json(fd, 200, json);
}

static void owner_prepare_read(void) {
    lock_info->owner_waiting = true;
}

static void owner_signal_if_writer(int doc_fd) {
    (void)doc_fd;
    if (lock_info->lock_type == 2 && lock_info->holding_pid > 0) {
        if (process_exists(lock_info->holding_pid)) {
            kill(lock_info->holding_pid, PRIORITY_SIGNAL);
        }
        sleep(1);
    }
}

static void handle_get_document(int fd) {
    if (g_guest) {
        http_json(fd, 401, "{\"error\":\"login required\"}");
        return;
    }
    if (!can_read_doc()) {
        http_json(fd, 403, "{\"error\":\"no read permission\"}");
        return;
    }

    int o_fd = open(SHARED_DOC, O_RDWR);
    if (o_fd < 0) {
        http_json(fd, 500, "{\"error\":\"open failed\"}");
        return;
    }

    if (ACTOR_OWNER) {
        owner_prepare_read();
        owner_signal_if_writer(o_fd);
    }

    if (!acquire_read_lock(o_fd, &g_actor)) {
        close(o_fd);
        if (ACTOR_OWNER) {
            lock_info->owner_waiting = false;
        }
        http_json(fd, 409, "{\"error\":\"could not acquire read lock\"}");
        return;
    }

    if (ACTOR_OWNER) {
        lock_info->owner_waiting = false;
    }

    char *buf = malloc(DOC_CAP + 1);
    if (!buf) {
        release_read_lock(o_fd, &g_actor);
        close(o_fd);
        http_json(fd, 500, "{\"error\":\"oom\"}");
        return;
    }

    ssize_t total = 0;
    ssize_t n;
    while ((n = read(o_fd, buf + total, DOC_CAP - (size_t)total)) > 0) {
        total += n;
        if (total >= DOC_CAP) {
            break;
        }
    }
    buf[total] = '\0';

    release_read_lock(o_fd, &g_actor);
    close(o_fd);

    http_text(fd, 200, "text/plain; charset=utf-8", buf, (size_t)total);
    free(buf);
}

static void handle_put_document(int fd, const char *body, size_t body_len) {
    if (g_guest) {
        http_json(fd, 401, "{\"error\":\"login required\"}");
        return;
    }
    if (!can_write_doc()) {
        http_json(fd, 403, "{\"error\":\"no write permission\"}");
        return;
    }

    int o_fd = open(SHARED_DOC, O_RDWR);
    if (o_fd < 0) {
        http_json(fd, 500, "{\"error\":\"open failed\"}");
        return;
    }

    if (!acquire_write_lock(o_fd, &g_actor)) {
        close(o_fd);
        http_json(fd, 409, "{\"error\":\"could not acquire write lock\"}");
        return;
    }

    if (ftruncate(o_fd, 0) != 0) {
        release_write_lock(o_fd, &g_actor);
        close(o_fd);
        http_json(fd, 500, "{\"error\":\"truncate failed\"}");
        return;
    }
    if (lseek(o_fd, 0, SEEK_SET) < 0) {
        release_write_lock(o_fd, &g_actor);
        close(o_fd);
        http_json(fd, 500, "{\"error\":\"seek failed\"}");
        return;
    }

    const char *p = body;
    size_t left = body_len;
    while (left > 0) {
        ssize_t w = write(o_fd, p, left);
        if (w <= 0) {
            release_write_lock(o_fd, &g_actor);
            close(o_fd);
            http_json(fd, 500, "{\"error\":\"write failed\"}");
            return;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }

    release_write_lock(o_fd, &g_actor);
    close(o_fd);

    http_json(fd, 200, "{\"ok\":true}");
}

static void handle_get_users(int fd) {
    if (g_guest || !ACTOR_OWNER) {
        http_json(fd, 403, "{\"error\":\"owner only\"}");
        return;
    }
    User users[MAX_USERS];
    int n = read_control_file(users, MAX_USERS);
    char *out = malloc(DOC_CAP);
    if (!out) {
        http_json(fd, 500, "{\"error\":\"oom\"}");
        return;
    }
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, DOC_CAP - pos, "[");
    for (int i = 0; i < n; i++) {
        const char *pr = users[i].priority == PRIORITY_OWNER ? "owner"
                         : users[i].priority == PRIORITY_HIGH   ? "high"
                                                                : "low";
        const char *ac = users[i].access_type == ACCESS_READ_ONLY   ? "read-only"
                         : users[i].access_type == ACCESS_WRITE_ONLY ? "write-only"
                                                                     : "read-write";
        int active = (users[i].pid > 0 && process_exists(users[i].pid)) ? 1 : 0;
        char esc[128];
        json_escape(users[i].name, esc, sizeof(esc));
        pos += (size_t)snprintf(out + pos, DOC_CAP - pos, "%s{\"name\":\"%s\",\"priority\":\"%s\",\"access\":\"%s\","
                                                         "\"pid\":%d,\"active\":%s}",
                                (i == 0) ? "" : ",", esc, pr, ac, (int)users[i].pid, active ? "true" : "false");
        if (pos >= DOC_CAP - 4) {
            break;
        }
    }
    pos += (size_t)snprintf(out + pos, DOC_CAP - pos, "]");
    http_text(fd, 200, "application/json; charset=utf-8", out, pos);
    free(out);
}

static int json_int(const char *body, const char *key, int *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *out = (int)strtol(p, NULL, 10);
    return 0;
}

static int json_str(const char *body, const char *key, char *buf, size_t bufsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < bufsz) {
        if (*p == '\\' && p[1] == 'n') {
            buf[i++] = '\n';
            p += 2;
            continue;
        }
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return 0;
}

static void handle_auth_login(int fd, const char *body) {
    char role[24];
    if (json_str(body, "role", role, sizeof(role)) != 0) {
        http_json(fd, 400, "{\"error\":\"missing role\"}");
        return;
    }
    if (strcmp(role, "owner") == 0) {
        User users[MAX_USERS];
        int n = read_control_file(users, MAX_USERS);
        if (n < 1) {
            http_json(fd, 500, "{\"error\":\"no admin in registry\"}");
            return;
        }
        const char *sid = session_create(&users[0]);
        if (!sid) {
            http_json(fd, 503, "{\"error\":\"too many sessions\"}");
            return;
        }
        char hdr[384];
        snprintf(hdr, sizeof(hdr), "Set-Cookie: doc_sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400\r\n", sid);
        http_json_set_cookie(fd, 200, "{\"ok\":true}", hdr);
        return;
    }
    if (strcmp(role, "user") == 0) {
        char uname[64];
        if (json_str(body, "username", uname, sizeof(uname)) != 0) {
            http_json(fd, 400, "{\"error\":\"missing username\"}");
            return;
        }
        User u;
        if (!find_user(uname, &u)) {
            http_json(fd, 404, "{\"error\":\"unknown user\"}");
            return;
        }
        const char *sid = session_create(&u);
        if (!sid) {
            http_json(fd, 503, "{\"error\":\"too many sessions\"}");
            return;
        }
        char hdr[384];
        snprintf(hdr, sizeof(hdr), "Set-Cookie: doc_sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400\r\n", sid);
        http_json_set_cookie(fd, 200, "{\"ok\":true}", hdr);
        return;
    }
    http_json(fd, 400, "{\"error\":\"invalid role\"}");
}

static void handle_auth_logout(int fd, const char *sid) {
    session_destroy(sid);
    http_json_set_cookie(fd, 200, "{\"ok\":true}",
                        "Set-Cookie: doc_sid=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n");
}

static void handle_post_users_add(int fd, const char *body) {
    if (g_guest || !ACTOR_OWNER) {
        http_json(fd, 403, "{\"error\":\"owner only\"}");
        return;
    }
    char name[64];
    int pri = PRIORITY_LOW;
    int acc = ACCESS_READ_ONLY;
    if (json_str(body, "name", name, sizeof(name)) != 0) {
        http_json(fd, 400, "{\"error\":\"missing name\"}");
        return;
    }
    (void)json_int(body, "priority", &pri);
    (void)json_int(body, "access", &acc);
    if (pri != PRIORITY_HIGH && pri != PRIORITY_LOW) {
        pri = PRIORITY_LOW;
    }
    if (acc < ACCESS_READ_ONLY || acc > ACCESS_BOTH) {
        acc = ACCESS_READ_ONLY;
    }
    int rc = registry_add_user(name, pri, acc);
    if (rc == REG_OK) {
        http_json(fd, 200, "{\"ok\":true}");
    } else if (rc == REG_ERR_EXISTS) {
        http_json(fd, 409, "{\"error\":\"exists\"}");
    } else if (rc == REG_ERR_FULL) {
        http_json(fd, 400, "{\"error\":\"full\"}");
    } else {
        http_json(fd, 400, "{\"error\":\"add failed\"}");
    }
}

static void handle_post_users_remove(int fd, const char *body) {
    if (g_guest || !ACTOR_OWNER) {
        http_json(fd, 403, "{\"error\":\"owner only\"}");
        return;
    }
    char name[64];
    if (json_str(body, "name", name, sizeof(name)) != 0) {
        http_json(fd, 400, "{\"error\":\"missing name\"}");
        return;
    }
    int rc = registry_remove_user(name);
    if (rc == REG_OK) {
        http_json(fd, 200, "{\"ok\":true}");
    } else if (rc == REG_ERR_ADMIN) {
        http_json(fd, 403, "{\"error\":\"cannot remove admin\"}");
    } else if (rc == REG_ERR_NOTFOUND) {
        http_json(fd, 404, "{\"error\":\"not found\"}");
    } else {
        http_json(fd, 400, "{\"error\":\"remove failed\"}");
    }
}

static void handle_post_users_update(int fd, const char *body) {
    if (g_guest || !ACTOR_OWNER) {
        http_json(fd, 403, "{\"error\":\"owner only\"}");
        return;
    }
    char name[64];
    int pri = PRIORITY_LOW;
    int acc = ACCESS_READ_ONLY;
    if (json_str(body, "name", name, sizeof(name)) != 0) {
        http_json(fd, 400, "{\"error\":\"missing name\"}");
        return;
    }
    (void)json_int(body, "priority", &pri);
    (void)json_int(body, "access", &acc);
    int rc = registry_update_user(name, pri, acc);
    if (rc == REG_OK) {
        http_json(fd, 200, "{\"ok\":true}");
    } else if (rc == REG_ERR_ADMIN) {
        http_json(fd, 403, "{\"error\":\"cannot update admin\"}");
    } else if (rc == REG_ERR_NOTFOUND) {
        http_json(fd, 404, "{\"error\":\"not found\"}");
    } else {
        http_json(fd, 400, "{\"error\":\"update failed\"}");
    }
}

static void handle_get_history(int fd) {
    if (g_guest || !ACTOR_OWNER) {
        http_json(fd, 403, "{\"error\":\"owner only\"}");
        return;
    }
    FILE *fp = fopen("history.txt", "rb");
    if (!fp) {
        http_text(fd, 200, "text/plain; charset=utf-8", "", 0);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0 || sz > (long)DOC_CAP) {
        fclose(fp);
        http_json(fd, 400, "{\"error\":\"history too large\"}");
        return;
    }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        http_json(fd, 500, "{\"error\":\"oom\"}");
        return;
    }
    size_t r = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[r] = '\0';
    http_text(fd, 200, "text/plain; charset=utf-8", buf, r);
    free(buf);
}

static void handle_client(int cfd) {
    char *req = malloc(REQ_CAP);
    if (!req) {
        close(cfd);
        return;
    }
    int rh = read_until_headers(cfd, req, REQ_CAP);
    if (rh < 0) {
        free(req);
        close(cfd);
        return;
    }

    char *split = strstr(req, "\r\n\r\n");
    if (!split) {
        free(req);
        close(cfd);
        return;
    }
    *split = '\0';
    const char *headers = req;
    char sid_buf[96];
    parse_cookie_sid(headers, sid_buf, sizeof(sid_buf));
    User *sess_u = session_by_sid(sid_buf);
    if (sess_u) {
        actor_bind_user(sess_u);
    } else {
        actor_reset_guest();
    }

    char *body_start = split + 4;
    int body_prefix = rh - (int)(body_start - req);
    if (body_prefix < 0) {
        body_prefix = 0;
    }

    char method[16];
    char path[256];
    method_path(req, method, sizeof(method), path, sizeof(path));

    long cl = header_content_length(headers);
    char body_buf[REQ_CAP];
    size_t body_len = 0;
    const char *body_ptr = body_buf;

    if (cl > 0) {
        if ((size_t)cl >= sizeof(body_buf)) {
            free(req);
            close(cfd);
            return;
        }
        size_t first = (size_t)body_prefix;
        if (first > (size_t)cl) {
            first = (size_t)cl;
        }
        if (first > 0) {
            memcpy(body_buf, body_start, first);
        }
        long need = cl - (long)first;
        if (need > 0) {
            if (recv_body(cfd, body_buf + first, sizeof(body_buf) - first, need) < 0) {
                free(req);
                close(cfd);
                return;
            }
        }
        body_len = (size_t)cl;
        body_buf[body_len] = '\0';
    } else {
        body_buf[0] = '\0';
    }

    free(req);

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            send_static(cfd, "index.html");
        } else if (strcmp(path, "/css/style.css") == 0) {
            send_static(cfd, "css/style.css");
        } else if (strcmp(path, "/js/app.js") == 0) {
            send_static(cfd, "js/app.js");
        } else if (strcmp(path, "/api/bootstrap") == 0) {
            handle_bootstrap(cfd);
        } else if (strcmp(path, "/api/document") == 0) {
            handle_get_document(cfd);
        } else if (strcmp(path, "/api/users") == 0) {
            handle_get_users(cfd);
        } else if (strcmp(path, "/api/history") == 0) {
            handle_get_history(cfd);
        } else {
            http_json(cfd, 404, "{\"error\":\"not found\"}");
        }
    } else if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/document") == 0) {
        handle_put_document(cfd, body_ptr, body_len);
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/auth/login") == 0) {
            handle_auth_login(cfd, body_ptr);
        } else if (strcmp(path, "/api/auth/logout") == 0) {
            handle_auth_logout(cfd, sid_buf);
        } else if (strcmp(path, "/api/users/add") == 0) {
            handle_post_users_add(cfd, body_ptr);
        } else if (strcmp(path, "/api/users/remove") == 0) {
            handle_post_users_remove(cfd, body_ptr);
        } else if (strcmp(path, "/api/users/update") == 0) {
            handle_post_users_update(cfd, body_ptr);
        } else if (strcmp(path, "/api/history/push") == 0) {
            if (g_guest || !ACTOR_OWNER) {
                http_json(cfd, 403, "{\"error\":\"owner only\"}");
            } else {
                append_to_history();
                http_json(cfd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/history/pop") == 0) {
            if (g_guest || !ACTOR_OWNER) {
                http_json(cfd, 403, "{\"error\":\"owner only\"}");
            } else {
                pop_last_snapshot();
                http_json(cfd, 200, "{\"ok\":true}");
            }
        } else {
            http_json(cfd, 404, "{\"error\":\"not found\"}");
        }
    } else {
        http_json(cfd, 405, "{\"error\":\"method not allowed\"}");
    }

    close(cfd);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2) {
        char *end = NULL;
        long p = strtol(argv[1], &end, 10);
        if (end != argv[1] && *end == '\0' && p > 0 && p < 65536) {
            g_port = (int)p;
        }
    }

    if (g_port <= 0 || g_port > 65535) {
        g_port = DEFAULT_PORT;
    }

    create_shared_doc_if_not_exists();
    initialize_control_file();
    initialize_synchronization(true);

    User users[MAX_USERS];
    int uc = read_control_file(users, MAX_USERS);
    users[0].pid = getpid();
    write_control_file(users, uc);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        goto fail_cleanup;
    }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(srv);
        goto fail_cleanup;
    }
    if (listen(srv, 8) != 0) {
        perror("listen");
        close(srv);
        goto fail_cleanup;
    }

    printf("Web UI: http://127.0.0.1:%d/\n", g_port);
    printf("Press Ctrl+C to stop.\n");

    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(cfd);
    }

    close(srv);
fail_cleanup:
    cleanup_synchronization(true);
    return 0;
}
