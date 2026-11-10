/* stub_server.c - see stub_server.h. POSIX-only (fork + sockets); test
 * harness code, never linked into the door itself, so it is fine for this
 * file to need POSIX feature macros/headers that the production modules
 * deliberately avoid. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <signal.h>
#include <poll.h>
#include "stub_server.h"

/* How long stub_server_start_sequence()'s child waits for EACH connection
 * before giving up - see that function's own comment. Generous for a
 * loopback test (every real connection here arrives in well under a
 * second) without making a genuinely hung test suite wait long. */
#define SEQUENCE_ACCEPT_TIMEOUT_SECS 5

#define CAPTURE_BUF_SIZE 4096

static int make_listener(int *out_port)
{
    int fd;
    struct sockaddr_in addr;
    socklen_t addrlen;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral: the kernel picks an unused port */

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    addrlen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *) &addr, &addrlen) != 0) {
        close(fd);
        return -1;
    }
    *out_port = (int) ntohs(addr.sin_port);

    return fd;
}

int stub_server_start(const unsigned char *response, unsigned long response_len, int *out_pid)
{
    int listen_fd;
    int port;
    pid_t pid;

    listen_fd = make_listener(&port);
    if (listen_fd < 0) {
        return -1;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(listen_fd);
        return -1;
    }

    if (pid == 0) {
        /* Child: serve exactly one connection, then exit. */
        int conn_fd;
        unsigned long sent;
        char reqbuf[2048];
        long got;
        int have_terminator = 0;

        conn_fd = accept(listen_fd, (struct sockaddr *) 0, (socklen_t *) 0);
        close(listen_fd);
        if (conn_fd >= 0) {
            /* Drain (and discard) the client's request before writing the
             * scripted response. This matters: if this socket still has
             * unread inbound data sitting in its receive buffer when
             * close() is called below, BSD sockets send a TCP RST instead
             * of a clean FIN, which the client's read() then reports as
             * ECONNRESET instead of a graceful EOF - exactly the
             * EOF-framed-body and truncated-body test cases below need to
             * NOT happen. Reading up to the blank line that ends the
             * request headers (or filling reqbuf, or hitting EOF/error)
             * is enough - this stub never needs the request body. */
            got = 0;
            while (got < (long) sizeof(reqbuf) - 1) {
                ssize_t n = read(conn_fd, reqbuf + got, sizeof(reqbuf) - 1 - (size_t) got);
                if (n <= 0) {
                    break;
                }
                got += n;
                reqbuf[got] = '\0';
                if (strstr(reqbuf, "\r\n\r\n") != (char *) 0) {
                    have_terminator = 1;
                    break;
                }
            }
            (void) have_terminator;

            sent = 0;
            while (sent < response_len) {
                ssize_t n = write(conn_fd, response + sent, (size_t) (response_len - sent));
                if (n <= 0) {
                    break;
                }
                sent += (unsigned long) n;
            }
            close(conn_fd);
        }
        _exit(0);
    }

    /* Parent: the child owns the connection now. */
    close(listen_fd);
    *out_pid = (int) pid;
    return port;
}

void stub_server_reap(int pid)
{
    int status;
    waitpid((pid_t) pid, &status, 0);
}

int stub_closed_port(void)
{
    int fd;
    int port;

    fd = make_listener(&port);
    if (fd < 0) {
        return -1;
    }
    close(fd); /* never listen()s: nothing accepts connections on this port now */
    return port;
}

int stub_server_start_capturing(const unsigned char *response, unsigned long response_len,
                                 int *out_pid, int *out_capture_fd)
{
    int listen_fd;
    int port;
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) != 0) {
        return -1;
    }

    listen_fd = make_listener(&port);
    if (listen_fd < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(listen_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: capture the request, hand it to the parent over the
         * pipe, THEN serve the scripted response, then exit - see
         * stub_server.h's doc comment for why this ordering matters. */
        int conn_fd;
        unsigned long sent;
        static unsigned char capbuf[CAPTURE_BUF_SIZE];
        unsigned long caplen = 0;
        struct timeval tv;

        close(pipefd[0]);

        conn_fd = accept(listen_fd, (struct sockaddr *) 0, (socklen_t *) 0);
        close(listen_fd);
        if (conn_fd >= 0) {
            /* A short receive timeout is how this loop knows the client
             * is done sending: the client (ae_http_get()/ae_ae_http_request())
             * never closes its write side before reading the response,
             * so waiting for EOF here would deadlock. Once no more bytes
             * arrive within the timeout, everything the client sent has
             * necessarily already been delivered on a loopback
             * connection. */
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            while (caplen < sizeof(capbuf)) {
                ssize_t n = read(conn_fd, capbuf + caplen, sizeof(capbuf) - caplen);
                if (n <= 0) {
                    break;
                }
                caplen += (unsigned long) n;
            }

            {
                unsigned long off = 0;
                while (off < caplen) {
                    ssize_t n = write(pipefd[1], capbuf + off, (size_t) (caplen - off));
                    if (n <= 0) {
                        break;
                    }
                    off += (unsigned long) n;
                }
            }
            close(pipefd[1]);

            sent = 0;
            while (sent < response_len) {
                ssize_t n = write(conn_fd, response + sent, (size_t) (response_len - sent));
                if (n <= 0) {
                    break;
                }
                sent += (unsigned long) n;
            }
            close(conn_fd);
        } else {
            close(pipefd[1]);
        }
        _exit(0);
    }

    /* Parent: the child owns the connection and the pipe's write end now. */
    close(pipefd[1]);
    close(listen_fd);
    *out_pid = (int) pid;
    *out_capture_fd = pipefd[0];
    return port;
}

int stub_server_start_sequence(const unsigned char * const *responses,
                                const unsigned long *response_lens,
                                int count, int *out_pid)
{
    int listen_fd;
    int port;
    pid_t pid;

    if (count <= 0) {
        return -1;
    }

    listen_fd = make_listener(&port);
    if (listen_fd < 0) {
        return -1;
    }
    if (listen(listen_fd, count) != 0) {
        close(listen_fd);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(listen_fd);
        return -1;
    }

    if (pid == 0) {
        /* Child: serve exactly `count` connections, in order, then exit.
         * If the code under test makes FEWER connections than scripted
         * (e.g. a broken retry that gives up after one attempt instead of
         * two), accept() below would otherwise block forever and the
         * parent's stub_server_reap() would hang the whole test suite
         * with it - a connection-count shortfall must fail fast instead.
         * select() with a timeout ahead of each accept() bounds the wait;
         * a timeout (or a select() error) exits the child non-zero, which
         * closes listen_fd and every future connect() to this port then
         * fails cleanly (ECONNREFUSED) instead of hanging. */
        int idx;

        for (idx = 0; idx < count; idx++) {
            int conn_fd;
            unsigned long sent;
            char reqbuf[2048];
            long got;
            fd_set readfds;
            struct timeval tv;
            int sel_rc;

            FD_ZERO(&readfds);
            FD_SET(listen_fd, &readfds);
            tv.tv_sec = SEQUENCE_ACCEPT_TIMEOUT_SECS;
            tv.tv_usec = 0;
            sel_rc = select(listen_fd + 1, &readfds, (fd_set *) 0, (fd_set *) 0, &tv);
            if (sel_rc <= 0) {
                /* Timed out (0) or an error (<0) waiting for connection
                 * idx - the caller scripted `count` responses but the
                 * code under test only made `idx` connections. Fail fast. */
                close(listen_fd);
                _exit(1);
            }

            conn_fd = accept(listen_fd, (struct sockaddr *) 0, (socklen_t *) 0);
            if (conn_fd < 0) {
                break;
            }

            got = 0;
            while (got < (long) sizeof(reqbuf) - 1) {
                ssize_t n = read(conn_fd, reqbuf + got, sizeof(reqbuf) - 1 - (size_t) got);
                if (n <= 0) {
                    break;
                }
                got += n;
                reqbuf[got] = '\0';
                if (strstr(reqbuf, "\r\n\r\n") != (char *) 0) {
                    break;
                }
            }

            sent = 0;
            while (sent < response_lens[idx]) {
                ssize_t n = write(conn_fd, responses[idx] + sent, (size_t) (response_lens[idx] - sent));
                if (n <= 0) {
                    break;
                }
                sent += (unsigned long) n;
            }
            close(conn_fd);
        }
        close(listen_fd);
        _exit(0);
    }

    /* Parent: the child owns the connections now. */
    close(listen_fd);
    *out_pid = (int) pid;
    return port;
}

unsigned long stub_server_read_capture(int capture_fd, unsigned char *out, unsigned long outsize)
{
    unsigned char drain[512];
    unsigned long total = 0;

    for (;;) {
        ssize_t n = read(capture_fd, drain, sizeof(drain));
        if (n <= 0) {
            break;
        }
        {
            unsigned long avail = (unsigned long) n;
            unsigned long room = (total < outsize) ? outsize - total : 0;
            unsigned long copy = (avail < room) ? avail : room;
            if (copy > 0) {
                memcpy(out + total, drain, copy);
            }
            total += copy;
        }
    }
    close(capture_fd);
    return total;
}


/* ---- /chat/v1 ------------------------------------------------------- */

static int chat_read_request(int fd, char *req, int cap, char **body)
{
    int n = 0, have_head = 0, want = 0;
    char *end = 0;
    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        int got = (int)recv(fd, req + n, (size_t)(cap - 1 - n), 0);
        if (got <= 0) break;
        n += got;
        req[n] = '\0';
        if (!have_head && (end = strstr(req, "\r\n\r\n")) != 0) {
            const char *cl = strstr(req, "Content-Length:");
            have_head = 1;
            want = (cl != 0 && cl < end) ? atoi(cl + 15) : 0;
        }
        if (have_head && n >= (int)(end - req) + 4 + want) break;
    }
    req[n] = '\0';
    *body = end != 0 ? end + 4 : req + n;
    return n;
}

static void chat_reply(int fd, int status, const char *type, const char *body)
{
    char head[256];
    int len = (int)strlen(body);
    int hl = sprintf(head, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                     status, type, len);
    (void)!write(fd, head, (size_t)hl);
    (void)!write(fd, body, (size_t)len);
}

static void chat_copy_json_field(char *dst, int cap, const char *body, const char *key)
{
    char pat[48];
    const char *p;
    int n = 0;
    sprintf(pat, "\"%s\":\"", key);
    dst[0] = '\0';
    p = strstr(body, pat);
    if (p == 0) return;
    p += strlen(pat);
    while (*p != '\0' && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1] != '\0') p++;
        dst[n++] = *p++;
    }
    dst[n] = '\0';
}

/* Remembers a write call so a test can check what the door sent. */
static void chat_note_write(stub_chat_stats *st, const char *path, const char *body)
{
    strncpy(st->last_write_path, path, sizeof st->last_write_path - 1);
    st->last_write_path[sizeof st->last_write_path - 1] = '\0';
    strncpy(st->last_write_body, body ? body : "", sizeof st->last_write_body - 1);
    st->last_write_body[sizeof st->last_write_body - 1] = '\0';
}

/* The event type the doorserver emits for POST /rooms/:id/<action>, 0 when it is no moderation route. */
static const char *chat_mod_event(const char *path)
{
    static const char *const map[][2] = {
        { "/kick", "kicked" }, { "/ban", "banned" }, { "/unban", "unbanned" }, { "/mute", "muted" },
        { "/unmute", "unmuted" }, { "/op", "op" }, { "/deop", "deop" }
    };
    const char *tail = strrchr(path, '/');
    int i;
    if (tail == 0) return 0;
    for (i = 0; i < 7; i++) if (strcmp(tail, map[i][0]) == 0) return map[i][1];
    return 0;
}

static void chat_serve(int fd, stub_chat_stats *st, int *state)
{
    char req[6144], method[8], path[256], text[1400];
    char *body;
    int is_post, is_get;
    if (chat_read_request(fd, req, sizeof req, &body) <= 0) return;
    if (sscanf(req, "%7s %255s", method, path) != 2) return;
    is_post = strcmp(method, "POST") == 0;
    is_get = strcmp(method, "GET") == 0;
    if (is_post && strcmp(path, "/achievements/v1/boards/register") == 0) {   /* unauthenticated, A2 */
        st->registers++;
        strncpy(st->last_register_body, body, sizeof st->last_register_body - 1);
        if (strstr(req, "Accept: text/plain") == 0) st->bad_auth++;
        if (strstr(body, "\"name\":\"bad\"") != 0)
            chat_reply(fd, 400, "application/json", "{\"error\":\"bad_request\",\"detail\":\"name must be printable ASCII\"}");
        else
            chat_reply(fd, 201, "text/plain", "OK regd.sekrit\n");
        return;
    }
    if (strstr(req, "Authorization: Board ") == 0) st->bad_auth++;
    if (!(is_post && strcmp(path, "/chat/v1/sessions") == 0) && strstr(req, "X-Chat-Session: ") == 0) st->bad_session++;

    if (is_post && strcmp(path, "/chat/v1/sessions") == 0) {
        st->sessions++;
        char reply[160];
        sprintf(reply, "{\"sid\":\"sid0123456789\",\"user\":\"%.48s\",\"expiresIn\":90}",
                st->session_user[0] != '\0' ? st->session_user : "Tester");
        chat_reply(fd, 200, "application/json", reply);
    } else if (st->expire_next && !(is_post && strncmp(path, "/chat/v1/sessions", 17) == 0)) {
        st->expire_next = 0;
        chat_reply(fd, 401, "application/json", "{\"error\":\"session_expired\",\"detail\":\"gone\"}");
    } else if (strncmp(path, "/chat/v1/rooms/", 15) == 0 && strstr(path, "/pins") != 0) {      /* Phase A */
        chat_note_write(st, path, body);
        if (is_get) chat_reply(fd, 200, "application/json",
            "[{\"messageId\":\"m1\",\"user\":\"alice\",\"content\":\"hello there\",\"pinnedBy\":\"Tester\",\"at\":1700000000,\"messageAt\":1699990000}]");
        else { st->pin_calls++; chat_reply(fd, is_post ? 201 : 200, "application/json", "{\"ok\":true}"); }
    } else if (is_get && strncmp(path, "/chat/v1/search", 15) == 0) {
        chat_note_write(st, path, body);
        chat_reply(fd, 200, "application/json",
            "[{\"id\":\"m1\",\"room\":\"Lobby\",\"user\":\"alice\",\"at\":1700000000,\"content\":\"hello there\"}]");
    } else if (strncmp(path, "/chat/v1/emoji", 14) == 0) {
        chat_note_write(st, path, body);
        if (is_get) chat_reply(fd, 200, "text/plain", "EMOJI|1|3|2\r\nparty|PARTY|\\o/\r\npic||\r\n");
        else {
            st->emoji_calls++;
            if (strcmp(method, "PUT") == 0 && body != 0) {
                strncpy(st->emoji_body, body, sizeof st->emoji_body - 1);
                st->emoji_body[sizeof st->emoji_body - 1] = '\0';
            }
            chat_reply(fd, 200, "application/json", "{\"ok\":true}");
        }
    } else if (strcmp(path, "/chat/v1/discord") == 0) {
        chat_note_write(st, path, body);
        if (!is_get) st->discord_on = body != 0 && strstr(body, "\"on\":true") != 0;
        chat_reply(fd, 200, "application/json", st->discord_on ? "{\"on\":true}" : "{\"on\":false}");
    } else if (strcmp(path, "/chat/v1/motd") == 0) {
        chat_note_write(st, path, body);
        if (is_get) chat_reply(fd, 200, "application/json", st->motd_set ? "{\"text\":\"Be nice\"}" : "{\"text\":\"\"}");
        else { st->motd_set = body != 0 && strstr(body, "\"text\":\"\"") == 0; chat_reply(fd, 200, "application/json", "{\"ok\":true}"); }
    } else if (strncmp(path, "/chat/v1/rooms/", 15) == 0
               && ((strcmp(method, "DELETE") == 0 && strchr(path + 15, '/') == 0) || (is_post && strstr(path, "/clear") != 0))) {
        chat_note_write(st, path, body);
        st->room_admin_calls++;
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (is_get && strncmp(path, "/chat/v1/threads/", 17) == 0) {
        st->thread_views++;
        chat_reply(fd, 200, "text/plain",
                   "HISTORY|1|6|2\r\n1|m1|alice|1700000000||m1|9|hello there\r\n2|m2|bob|1700000060|m1|m1|0|second line\r\n");
    } else if (strncmp(path, "/chat/v1/messages/", 18) == 0 && strchr(path + 18, '/') == 0
               && (strcmp(method, "PUT") == 0 || strcmp(method, "DELETE") == 0)) {
        /* PUT /messages/:id edits, DELETE /messages/:id deletes; the event comes on the next poll. */
        char content[200];
        chat_note_write(st, path, body);
        chat_copy_json_field(content, sizeof content, body, "content");
        if (method[0] == 'P') {
            st->edits++;
            sprintf(st->pending_row, "edit|lobby|Tester|%s|1700000500|0|%s", path + 18, content);
        } else {
            st->deletes++;
            sprintf(st->pending_row, "delete|lobby|Tester|%s|1700000500|0|", path + 18);
        }
        chat_reply(fd, 200, "application/json", method[0] == 'P' ? "{\"id\":\"m1\"}" : "{\"ok\":true}");
    } else if (strcmp(method, "PUT") == 0 && strncmp(path, "/chat/v1/rooms/", 15) == 0 && strstr(path, "/motd") != 0) {
        /* PUT /rooms/:id/motd: the hub's room_motd event comes on the next poll */
        chat_note_write(st, path, body);
        chat_copy_json_field(st->room_motd, sizeof st->room_motd, body, "motd");
        st->room_motd_puts++;
        sprintf(st->pending_row, "room_motd|lobby|Tester||1700000500|0|%s", st->room_motd);
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (is_post && strncmp(path, "/chat/v1/rooms/", 15) == 0 && strstr(path, "/typing") != 0) {
        /* POST /rooms/:id/typing (D3): logged, not noted, so a typed command keeps last_write_path */
        char text[220];
        chat_copy_json_field(text, sizeof text, body, "text");
        st->typing_calls++;
        if (strlen(st->typing_log) + strlen(text) + 2 < sizeof st->typing_log) { strcat(st->typing_log, text); strcat(st->typing_log, "|"); }
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (is_post && strncmp(path, "/chat/v1/rooms/", 15) == 0 && strstr(path, "/mode") != 0) {
        /* POST /rooms/:id/mode: "+z" is refused (a non-moderator); an 'x' flag is unknown; the rest apply */
        char modes[72], params[160], applied[260], reply[400];
        chat_note_write(st, path, body);
        chat_copy_json_field(modes, sizeof modes, body, "modes");
        chat_copy_json_field(params, sizeof params, body, "params");
        st->mode_calls++;
        if (strcmp(modes, "+z") == 0) {
            chat_reply(fd, 403, "application/json", "{\"error\":\"forbidden\",\"detail\":\"Only moderators can change modes\"}");
            return;
        }
        if (strchr(modes, 'x') != 0) {
            chat_reply(fd, 200, "application/json", "{\"applied\":\"+m\",\"errors\":\"Unknown mode flag: x|User nobody@UPROUGH not found\"}");
            return;
        }
        sprintf(applied, "%s%s%s", modes, params[0] ? " " : "", params);
        sprintf(st->pending_row, "mode|lobby|Tester||1700000500|0|%s", applied);
        sprintf(reply, "{\"applied\":\"%s\",\"errors\":\"\"}", applied);
        chat_reply(fd, 200, "application/json", reply);
    } else if (strcmp(method, "PUT") == 0 && strstr(path, "/topic") != 0) {
        char topic[200];
        chat_note_write(st, path, body);
        chat_copy_json_field(topic, sizeof topic, body, "topic");
        st->topics++;
        sprintf(st->pending_row, "topic|lobby|Tester||1700000500|0|%s", topic);
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (strstr(path, "/invites") != 0 && strncmp(path, "/chat/v1/rooms/", 15) == 0) {
        chat_note_write(st, path, body);
        if (is_post) st->invites++; else st->uninvites++;
        chat_reply(fd, is_post ? 201 : 200, "application/json", "{\"ok\":true}");
    } else if (is_post && strncmp(path, "/chat/v1/rooms/", 15) == 0 && chat_mod_event(path) != 0) {
        char user[64], reason[128];
        chat_note_write(st, path, body);
        chat_copy_json_field(user, sizeof user, body, "user");
        chat_copy_json_field(reason, sizeof reason, body, "reason");
        if (strchr(user, '@') != 0) *strchr(user, '@') = '\0';
        st->moderations++;
        if (strcmp(chat_mod_event(path), "unbanned") != 0)    /* the doorserver emits no unban event */
            sprintf(st->pending_row, "%s|lobby|%s|Tester|1700000500|0|%s", chat_mod_event(path), user, reason);
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/chat/v1/messages/", 18) == 0) {
        int n = 0;
        while (path[18 + n] && path[18 + n] != '/' && n < (int)sizeof st->last_react_msg - 1) { st->last_react_msg[n] = path[18 + n]; n++; }
        st->last_react_msg[n] = '\0';
        st->unreacts++;
        chat_reply(fd, 200, "application/json", "{\"count\":0}");
    } else if (strcmp(path, "/chat/v1/presence") == 0 && (strcmp(method, "PUT") == 0 || strcmp(method, "DELETE") == 0)) {
        /* PUT /presence {status,text} sets, DELETE /presence clears; the presence event comes on the next poll. */
        chat_note_write(st, path, body);
        if (strcmp(method, "PUT") == 0) {
            char status[16], note[80];
            chat_copy_json_field(status, sizeof status, body, "status");
            chat_copy_json_field(note, sizeof note, body, "text");
            st->presences++;
            if (strcmp(st->presence_user, "alice") == 0) {      /* who.txt follows, as the real server's does */
                strcpy(st->alice_status, status);
                strcpy(st->alice_text, note);
            }
            sprintf(st->pending_row, "presence|lobby|%s||1700000500|0|%s!%s", st->presence_user[0] ? st->presence_user : "Tester", status, note);
        } else {
            st->presence_clears++;
            strcpy(st->pending_row, "presence|lobby|Tester||1700000500|0|online!");
        }
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (is_get && strncmp(path, "/chat/v1/users/", 15) == 0 && strstr(path, "/profile.txt") != 0) {
        st->profiles++;
        chat_note_write(st, path, "");
        if (strncmp(path + 15, "alice", 5) == 0)
            chat_reply(fd, 200, "text/plain", "PROFILE|1|9|1\r\nalice@boardA|alice|boardA|away|lunch|1700000000|1700000300|12|1|lobby,secret\r\n");
        else
            chat_reply(fd, 404, "application/json", "{\"error\":\"not_found\",\"detail\":\"no such user\"}");
    } else if (is_get && strcmp(path, "/chat/v1/ignores.txt") == 0) {
        char rows[300];
        char *p2, list[sizeof st->ignore_list];
        st->ignore_lists++;
        strcpy(rows, "IGNORES|1|9|0\r\n");
        strcpy(list, st->ignore_list);
        for (p2 = strtok(list, ","); p2 != 0; p2 = strtok(0, ","))
            sprintf(rows + strlen(rows), "%s@boardA|1700000100\r\n", p2);
        chat_reply(fd, 200, "text/plain", rows);
    } else if (is_post && strcmp(path, "/chat/v1/ignores") == 0) {
        char who[40];
        chat_note_write(st, path, body);
        chat_copy_json_field(who, sizeof who, body, "user");
        if (strchr(who, '@') != 0) *strchr(who, '@') = '\0';
        if (st->ignore_list[0] != '\0') strcat(st->ignore_list, ",");
        strcat(st->ignore_list, who);
        st->ignore_adds++;
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/chat/v1/ignores/", 17) == 0) {
        chat_note_write(st, path, "");
        st->ignore_list[0] = '\0';
        st->ignore_removes++;
        chat_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (strcmp(method, "DELETE") == 0) {
        st->session_closes++;
        chat_reply(fd, 204, "text/plain", "");
    } else if (is_get && strcmp(path, "/chat/v1/rooms.txt") == 0) {
        st->rooms_listed++;
        if (st->rooms_kinds)                                   /* the hub since room types: kind is the last field */
            chat_reply(fd, 200, "text/plain", "ROOMS|1|4|4\nlobby|Lobby|Hello all|public|3|boardA|text\n"
                       "paint|Paint||public|0|boardA|art\ntalk|Talk||public|0|boardA|voice\nsecret|Secret||private|1|boardB\n");
        else chat_reply(fd, 200, "text/plain", "ROOMS|1|4|2\nlobby|Lobby|Hello all|public|3|boardA\nsecret|Secret||private|1|boardB\n");
    } else if (is_post && strcmp(path, "/chat/v1/rooms") == 0) {
        st->rooms_created++;
        chat_copy_json_field(st->last_created, sizeof st->last_created, body, "name");
        chat_copy_json_field(st->last_created_kind, sizeof st->last_created_kind, body, "kind");
        chat_copy_json_field(st->last_created_topic, sizeof st->last_created_topic, body, "topic");
        chat_copy_json_field(st->last_created_vis, sizeof st->last_created_vis, body, "visibility");
        chat_reply(fd, 200, "application/json", "{\"id\":\"newroom\",\"name\":\"newroom\",\"visibility\":\"public\"}");
    } else if (is_post && strstr(path, "/join") != 0) {
        st->joins++;
        {
            const char *from = path + 14;                      /* past "/chat/v1/rooms/" minus one */
            const char *to = strstr(path, "/join");
            int n = 0;
            if (strncmp(path, "/chat/v1/rooms/", 15) == 0) from = path + 15;
            while (from + n < to && n < (int)sizeof st->last_join - 1) { st->last_join[n] = from[n]; n++; }
            st->last_join[n] = '\0';
        }
        if (strstr(path, "/rooms/nosuch/") != 0) { chat_reply(fd, 404, "application/json", "{\"error\":\"not_found\"}"); return; }
        {
            char reply[320];
            sprintf(reply, "{\"id\":\"lobby\",\"name\":\"Lobby\",\"topic\":\"Hello all\",\"motd\":\"%.128s\",\"members\":3,\"ops\":1,\"since\":5}", st->room_motd);
            chat_reply(fd, 200, "application/json", reply);
        }
    } else if (is_post && strstr(path, "/leave") != 0) {
        st->leaves++;
        chat_reply(fd, 204, "text/plain", "");
    } else if (is_get && strstr(path, "/rooms/") != 0 && strstr(path, "/history.txt") != 0) {
        st->histories++;
        chat_reply(fd, 200, "text/plain",
                   "HISTORY|1|6|2\r\n1|m1|alice|1700000000||m1|9|hello there!bar\r\n2|m2|bob|1700000060|m1|m1|0|second line|bob\r\n");
    } else if (is_post && strstr(path, "/rooms/") != 0 && strstr(path, "/messages") != 0) {
        st->messages++;
        chat_note_write(st, path, body);
        chat_copy_json_field(st->last_message, sizeof st->last_message, body, "content");
        chat_copy_json_field(st->last_reply_to, sizeof st->last_reply_to, body, "replyTo");
        *state = 1;
        chat_reply(fd, 200, "application/json", "{\"id\":\"m3\",\"seq\":6,\"at\":1700000100}");
    } else if (is_post && strstr(path, "/react") != 0) {
        st->reacts++;
        chat_copy_json_field(st->last_react, sizeof st->last_react, body, "emoji");
        if (strncmp(path, "/chat/v1/messages/", 18) == 0) {
            int n = 0;
            while (path[18 + n] && path[18 + n] != '/' && n < (int)sizeof st->last_react_msg - 1) { st->last_react_msg[n] = path[18 + n]; n++; }
            st->last_react_msg[n] = '\0';
        }
        if (st->react_known) chat_reply(fd, 200, "application/json", "{\"count\":1}");
        else {
            sprintf(st->pending_row, "react|lobby|Tester|%s|1700000400|0|%s", st->last_react_msg, st->last_react);
            chat_reply(fd, 200, "application/json", "{\"count\":2}");
        }
    } else if (is_post && strstr(path, "/thread") != 0) {
        st->threads++;
        chat_reply(fd, 200, "application/json", "{\"id\":\"t1\",\"seq\":9}");
    } else if (is_post && st->dm_unknown[0] != '\0'
               && (strcmp(path, "/chat/v1/dm-group") == 0 || strncmp(path, "/chat/v1/dm/", 12) == 0)) {
        char reply[160];
        sprintf(reply, "{\"error\":\"not_found\",\"detail\":\"unknown user: %.100s\"}", st->dm_unknown);
        chat_reply(fd, 404, "application/json", reply);
    } else if (is_post && strcmp(path, "/chat/v1/dm-group") == 0) {
        st->group_dms++;
        strncpy(st->last_group_body, body, sizeof st->last_group_body - 1);
        if (st->group_dm_reject[0] != '\0') {
            char reply[220];
            sprintf(reply, "{\"error\":\"bad_request\",\"detail\":\"%.160s\"}", st->group_dm_reject);
            chat_reply(fd, 400, "application/json", reply);
        } else chat_reply(fd, 201, "application/json", "{\"id\":\"g1\",\"seq\":4,\"threadId\":\"dgabc123def456\"}");
    } else if (is_post && strncmp(path, "/chat/v1/dm/", 12) == 0) {
        st->dms++;
        strncpy(st->last_dm_peer, path + 12, sizeof st->last_dm_peer - 1);
        chat_copy_json_field(st->last_dm_text, sizeof st->last_dm_text, body, "content");
        chat_reply(fd, 200, "application/json", "{\"id\":\"d1\",\"seq\":3,\"threadId\":\"th1\"}");
    } else if (is_get && strstr(path, "/chat/v1/dm.txt") != 0) {
        st->dm_lists++;
        chat_reply(fd, 200, "text/plain", "DM|1|3|1\r\nth1|alice|1700000000|2\r\n");
    } else if (is_get && strstr(path, "/chat/v1/dm/") != 0) {
        chat_reply(fd, 200, "text/plain", "HISTORY|1|3|1\r\n1|d0|alice|1700000000|||5|private hello\r\n");
    } else if (is_get && strstr(path, "/chat/v1/who.txt") != 0) {
        st->whos++;
        {
            char rows[200];
            sprintf(rows, "WHO|1|9|2\r\nalice|lobby|0|9|12|%s|%s\r\nbob|lobby|1|0|3|online|\r\n",
                    st->alice_status[0] ? st->alice_status : "away", st->alice_status[0] ? st->alice_text : "lunch");
            chat_reply(fd, 200, "text/plain", rows);
        }
    } else if (is_get && strncmp(path, "/chat/v1/messages/", 18) == 0 && strstr(path, "/reactions.txt") != 0) {
        st->reaction_lists++;
        {                                             /* the caller's last emoji, +1 before any react */
            char rows[128];
            sprintf(rows, "REACTIONS|1|9|1\r\n%s|1|1\r\n", st->last_react[0] ? st->last_react : "+1");
            chat_reply(fd, 200, "text/plain", rows);
        }
    } else if (is_get && strstr(path, "/chat/v1/events.txt") != 0) {
        st->events++;
        if (st->events > st->events_ok_first && st->events_fail > 0) {
            st->events_fail--;
            chat_reply(fd, 500, "text/plain", "down");
            return;
        }
        {
            int rows = 0;
            char body_rows[1200];
            body_rows[0] = '\0';
            if (st->events == 1) {
                strcat(body_rows, "7|message|lobby|carol|m9|1700000200|12|from idle poll\n");
                rows++;
            }
            if (*state == 1) {
                *state = 2;
                sprintf(body_rows + strlen(body_rows), "8|message|lobby|Tester|m3|1700000300|0|%s|%s\n", st->last_message, st->last_reply_to);
                rows++;
            }
            {
            int last = rows > 0 ? 8 : 7;
            if (st->pending_row[0] != '\0') {
                sprintf(body_rows + strlen(body_rows), "9|%s\n", st->pending_row);
                st->pending_row[0] = '\0';
                rows++;
                last = 9;
            }
            sprintf(text, "EVENTS|1|%d|%d\n%s", last, rows, body_rows);
            }
        }
        chat_reply(fd, 200, "text/plain", text);
    } else {
        chat_reply(fd, 404, "application/json", "{\"error\":\"not_found\"}");
    }
}

int stub_chat_start(stub_chat_stats **stats, int *out_pid)
{
    int listen_fd, port;
    pid_t pid, parent;
    stub_chat_stats *st = mmap(0, sizeof *st, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED) return -1;
    memset(st, 0, sizeof *st);
    listen_fd = make_listener(&port);
    if (listen_fd < 0) return -1;
    if (listen(listen_fd, 8) != 0) { close(listen_fd); return -1; }
    parent = getpid();
    pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) {
        int state = 0;
        for (;;) {
            int fd;
            struct pollfd pf;
            pf.fd = listen_fd; pf.events = POLLIN; pf.revents = 0;
            /* The test process can die without calling stub_chat_stop (a failed assert aborts it):
               a stub that outlived it was reparented to init and ran forever. Check every 200 ms. */
            if (poll(&pf, 1, 200) <= 0) { if (getppid() != parent) _exit(0); continue; }
            fd = accept(listen_fd, 0, 0);
            if (fd < 0) continue;
            chat_serve(fd, st, &state);
            close(fd);
        }
    }
    close(listen_fd);
    *stats = st;
    *out_pid = (int)pid;
    return port;
}

void stub_chat_stop(int pid)
{
    if (pid > 0) { kill((pid_t)pid, SIGTERM); waitpid((pid_t)pid, 0, 0); }
}
