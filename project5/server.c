/*
 * server.c
 * ==========================================================================
 * Digital Library Reservation Server
 *
 * CONCURRENCY MODEL
 * -----------------
 * Thread-per-client: each accepted connection is handled by a dedicated
 * pthread.  This keeps the per-client logic simple (straight-line blocking
 * I/O rather than an event-driven state machine) and is appropriate for the
 * 5-client limit.  The main thread only accepts connections and dispatches.
 *
 * SHARED RESOURCE PROTECTION
 * ---------------------------
 * Two separate mutexes prevent false contention:
 *   books_mutex  — protects the books[] array (availability + reserved_by).
 *                  Held only for the duration of the read-check-write cycle
 *                  during reservation, keeping the critical section minimal.
 *   users_mutex  — protects sessions[] (active client list) and active_count.
 *   log_mutex    — serialises writes to stdout and server.log so log lines
 *                  from concurrent threads never interleave.
 *
 * DEADLOCK PREVENTION
 * --------------------
 * No function ever holds two mutexes simultaneously.  Lock order:
 *   - log_mutex  is never held when acquiring books_mutex or users_mutex.
 *   - books_mutex and users_mutex are never held at the same time.
 *
 * RACE CONDITION ANALYSIS (book reservation)
 * -------------------------------------------
 * Without a mutex, two threads could both read a book as "available",
 * both decide to reserve it, and both succeed — a classic TOCTOU race.
 * The books_mutex ensures the entire check-then-act sequence is atomic:
 *
 *   lock(books_mutex)
 *     if (available) { mark reserved; save; }
 *   unlock(books_mutex)
 *
 * SESSION LIFECYCLE
 * -----------------
 *   accept() → spawn thread → AUTH phase → RESERVATION loop → cleanup
 *
 * PERSISTENCE
 * -----------
 * books.dat is loaded at startup (or defaults used if absent) and
 * re-written after every reservation change, always under books_mutex.
 *
 * TIMEOUT
 * -------
 * SO_RCVTIMEO set to CLIENT_TIMEOUT_SEC on each client socket.
 * When recv() returns EAGAIN/EWOULDBLOCK the client is warned and evicted.
 *
 * GRACEFUL SHUTDOWN
 * -----------------
 * SIGINT sets shutdown_flag.  The accept() loop detects EINTR and exits.
 * All open client sockets are shut down so their threads unblock and
 * terminate cleanly before the process exits.
 * ==========================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

/* ── Valid library IDs ────────────────────────────────────────────────────── */
#define NUM_VALID_IDS 10
static const char *VALID_IDS[NUM_VALID_IDS] = {
    "LIB001","LIB002","LIB003","LIB004","LIB005",
    "LIB006","LIB007","LIB008","LIB009","LIB010"
};

/* ── Book record ──────────────────────────────────────────────────────────── */
typedef struct {
    int  index;                        /* 1-based, sent to client             */
    char title[MAX_TITLE_LEN];
    char reserved_by[MAX_LIB_ID_LEN]; /* empty string = available            */
} Book;

/* ── Default book catalogue (used when books.dat does not exist) ─────────── */
static Book books[MAX_BOOKS] = {
    {1, "The Art of Computer Programming",           ""},
    {2, "Operating Systems: Three Easy Pieces",      ""},
    {3, "Computer Networks (Tanenbaum)",              ""},
    {4, "The Linux Programming Interface",            ""},
    {5, "Clean Code (Robert C. Martin)",              ""},
    {6, "Structure and Interpretation of Programs",  ""},
    {7, "Modern Operating Systems",                   ""},
    {8, "Computer Organization and Design",           ""},
};
static pthread_mutex_t books_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Client session record ────────────────────────────────────────────────── */
typedef struct {
    int       fd;                          /* socket file descriptor          */
    int       active;                      /* 1 = slot in use                 */
    int       authenticated;
    char      lib_id[MAX_LIB_ID_LEN];
    char      ip[INET_ADDRSTRLEN];
    time_t    connect_time;
    pthread_t thread;
} ClientSession;

static ClientSession sessions[MAX_CLIENTS];
static int           active_count = 0;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Logging ──────────────────────────────────────────────────────────────── */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE           *log_file  = NULL;

/* ── Shutdown flag (set by SIGINT handler) ────────────────────────────────── */
static volatile sig_atomic_t shutdown_flag = 0;

/* ── Listening socket (global so signal handler can close it) ─────────────── */
static int listen_fd = -1;


/* ==========================================================================
 * Logging
 * --------------------------------------------------------------------------
 * log_event() writes a timestamped line to both stdout and server.log.
 * The log_mutex ensures lines from concurrent threads never interleave.
 * ========================================================================== */
static void log_event(const char *fmt, ...)
{
    char      timebuf[32];
    time_t    now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&log_mutex);

    /* Write to stdout */
    printf("[%s] ", timebuf);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);

    /* Mirror to log file */
    if (log_file) {
        fprintf(log_file, "[%s] ", timebuf);
        va_end(ap);
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
    va_end(ap);
}


/* ==========================================================================
 * Persistent storage — books.dat
 * --------------------------------------------------------------------------
 * Format (plain text, one book per line):
 *   <title>|<reserved_by>\n
 * "reserved_by" is empty string for available books.
 *
 * Both functions must be called while books_mutex is held by the caller.
 * ========================================================================== */
static void save_books(void)
{
    /* Called under books_mutex — safe to write without extra locking */
    FILE *f = fopen(BOOKS_FILENAME, "w");
    if (!f) {
        log_event("WARN  Could not write %s: %s", BOOKS_FILENAME, strerror(errno));
        return;
    }
    for (int i = 0; i < MAX_BOOKS; i++) {
        fprintf(f, "%s|%s\n", books[i].title, books[i].reserved_by);
    }
    fclose(f);
}

static void load_books(void)
{
    FILE *f = fopen(BOOKS_FILENAME, "r");
    if (!f) {
        log_event("INFO  No %s found — using default catalogue.", BOOKS_FILENAME);
        return;
    }
    char line[MAX_TITLE_LEN + MAX_LIB_ID_LEN + 4];
    int  i = 0;
    while (i < MAX_BOOKS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';   /* strip line ending            */
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        strncpy(books[i].title,       line,   MAX_TITLE_LEN  - 1);
        strncpy(books[i].reserved_by, sep+1,  MAX_LIB_ID_LEN - 1);
        books[i].index = i + 1;
        i++;
    }
    fclose(f);
    log_event("INFO  Loaded book state from %s.", BOOKS_FILENAME);
}


/* ==========================================================================
 * print_server_status
 * --------------------------------------------------------------------------
 * Displays active users and book inventory to the server console.
 * Called after every authentication event and every reservation change.
 * Acquires both users_mutex and books_mutex internally.
 * ========================================================================== */
static void print_server_status(void)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&log_mutex);

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║           SERVER STATUS SNAPSHOT             ║\n");
    printf("╠══════════════════════════════════════════════╣\n");

    /* Active users — read sessions under users_mutex */
    pthread_mutex_lock(&users_mutex);
    printf("║  Active users (%d/%d):%-26s║\n",
           active_count, MAX_CLIENTS, "");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active && sessions[i].authenticated) {
            long secs = (long)(now - sessions[i].connect_time);
            printf("║    [%d] %-10s (IP: %-15s) %3lds ║\n",
                   i+1, sessions[i].lib_id, sessions[i].ip, secs);
        }
    }
    pthread_mutex_unlock(&users_mutex);

    printf("╠══════════════════════════════════════════════╣\n");

    /* Book inventory — read books under books_mutex */
    pthread_mutex_lock(&books_mutex);
    printf("║  Book inventory:%-29s║\n", "");
    for (int i = 0; i < MAX_BOOKS; i++) {
        const char *status = books[i].reserved_by[0]
            ? books[i].reserved_by : "AVAILABLE";
        printf("║  [%d] %-32s %9s ║\n",
               books[i].index, books[i].title, status);
    }
    pthread_mutex_unlock(&books_mutex);

    printf("╚══════════════════════════════════════════════╝\n\n");
    fflush(stdout);

    pthread_mutex_unlock(&log_mutex);
}


/* ==========================================================================
 * build_book_list_payload
 * --------------------------------------------------------------------------
 * Serialises the current books[] array into the wire payload format.
 * Must be called while books_mutex is held.
 * ========================================================================== */
static void build_book_list_payload(char *buf, size_t buflen)
{
    int offset = 0;
    offset += snprintf(buf + offset, buflen - offset, "%d\n", MAX_BOOKS);
    for (int i = 0; i < MAX_BOOKS; i++) {
        const char *status = books[i].reserved_by[0]
            ? books[i].reserved_by : "available";
        offset += snprintf(buf + offset, buflen - offset,
                           "%d|%s|%s\n",
                           books[i].index, books[i].title, status);
    }
}


/* ==========================================================================
 * authenticate
 * --------------------------------------------------------------------------
 * Validates a library ID string against the VALID_IDS table.
 * Pure function — no shared state, no locking needed.
 *
 * Returns 1 if valid, 0 otherwise.
 * ========================================================================== */
static int authenticate(const char *lib_id)
{
    /* Strip trailing whitespace / newline the client may have included */
    char clean[MAX_LIB_ID_LEN];
    strncpy(clean, lib_id, MAX_LIB_ID_LEN - 1);
    clean[MAX_LIB_ID_LEN - 1] = '\0';
    clean[strcspn(clean, " \t\r\n")] = '\0';

    for (int i = 0; i < NUM_VALID_IDS; i++) {
        if (strcmp(clean, VALID_IDS[i]) == 0) return 1;
    }
    return 0;
}


/* ==========================================================================
 * handle_client  (thread entry point)
 * --------------------------------------------------------------------------
 * Manages the complete lifecycle of a single client session:
 *
 *   Phase 1 — Authentication
 *     Wait for MSG_AUTH_REQ.  Validate the library ID.
 *     Send MSG_AUTH_OK or MSG_AUTH_FAIL.
 *
 *   Phase 2 — Reservation loop  (only if authenticated)
 *     Send book list (MSG_BOOK_LIST).
 *     Wait for MSG_RESERVE_REQ or MSG_DISCONNECT.
 *     For each reservation:
 *       Lock books_mutex → check availability → update → unlock.
 *       Send MSG_RESERVE_OK or MSG_RESERVE_FAIL.
 *     A client may make multiple reservations in one session.
 *
 *   Cleanup
 *     Remove from sessions[].  Log disconnection.  Close socket.
 * ========================================================================== */
static void *handle_client(void *arg)
{
    ClientSession *sess = (ClientSession *)arg;
    Message        msg;
    char           payload[MAX_PAYLOAD_LEN];
    int            slot = (int)(sess - sessions);  /* index into sessions[]  */

    /* ── Set per-socket inactivity timeout ───────────────────────────────── */
    struct timeval tv = { CLIENT_TIMEOUT_SEC, 0 };
    setsockopt(sess->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    log_event("CONN  New connection from %s (slot %d)", sess->ip, slot);

    /* ════════════════════════════════════════════════════════════════════════
     * PHASE 1: AUTHENTICATION
     * ════════════════════════════════════════════════════════════════════════ */
    int r = recv_msg(sess->fd, &msg);
    if (r <= 0) {
        if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            log_event("TIMEOUT  Slot %d timed out during auth phase", slot);
        else
            log_event("DISCONNECT  Slot %d disconnected before auth", slot);
        goto cleanup;
    }

    if (msg.type != MSG_AUTH_REQ) {
        /* Protocol violation — not sending an auth request first */
        send_msg(sess->fd, MSG_AUTH_FAIL, "Protocol error: expected authentication.");
        log_event("PROTO  Slot %d sent unexpected message type 0x%02X", slot, msg.type);
        goto cleanup;
    }

    /* Strip newline from library ID payload */
    msg.payload[strcspn(msg.payload, "\r\n")] = '\0';

    if (!authenticate(msg.payload)) {
        snprintf(payload, sizeof(payload),
                 "Authentication failed: library ID '%.*s' is not recognised.",
                 MAX_LIB_ID_LEN - 1, msg.payload);
        send_msg(sess->fd, MSG_AUTH_FAIL, payload);
        log_event("AUTH  FAIL  '%s' from %s — invalid ID", msg.payload, sess->ip);
        goto cleanup;
    }

    /* Authentication succeeded */
    strncpy(sess->lib_id, msg.payload, MAX_LIB_ID_LEN - 1);
    sess->authenticated = 1;

    snprintf(payload, sizeof(payload),
             "Welcome, %s! You are now authenticated.", sess->lib_id);
    if (send_msg(sess->fd, MSG_AUTH_OK, payload) < 0) goto cleanup;

    log_event("AUTH  OK   '%s' from %s", sess->lib_id, sess->ip);
    print_server_status();

    /* ════════════════════════════════════════════════════════════════════════
     * PHASE 2: BOOK LIST + RESERVATION LOOP
     * ════════════════════════════════════════════════════════════════════════ */

    /* Build and send the current book list.
     * Lock books_mutex while serialising to guarantee a consistent snapshot
     * — another thread must not modify a book between our first and last read.
     */
    pthread_mutex_lock(&books_mutex);
    build_book_list_payload(payload, sizeof(payload));
    pthread_mutex_unlock(&books_mutex);

    if (send_msg(sess->fd, MSG_BOOK_LIST, payload) < 0) goto cleanup;
    log_event("LIST  Sent book catalogue to %s", sess->lib_id);

    /* Reservation loop: client may reserve multiple books per session */
    while (!shutdown_flag) {
        r = recv_msg(sess->fd, &msg);

        if (r == 0) {
            log_event("DISCONNECT  %s closed the connection", sess->lib_id);
            break;
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_msg(sess->fd, MSG_SERVER_INFO,
                         "Session timed out due to inactivity. Goodbye.");
                log_event("TIMEOUT  %s evicted after %ds inactivity",
                          sess->lib_id, CLIENT_TIMEOUT_SEC);
            } else {
                log_event("ERROR  recv() failed for %s: %s",
                          sess->lib_id, strerror(errno));
            }
            break;
        }

        if (msg.type == MSG_DISCONNECT) {
            log_event("QUIT  %s sent disconnect", sess->lib_id);
            break;
        }

        if (msg.type != MSG_RESERVE_REQ) {
            send_msg(sess->fd, MSG_SERVER_INFO, "Unexpected message type.");
            continue;
        }

        /* ── Parse book index ─────────────────────────────────────────────── */
        int book_idx = atoi(msg.payload);  /* 1-based                         */

        if (book_idx < 1 || book_idx > MAX_BOOKS) {
            snprintf(payload, sizeof(payload),
                     "Invalid selection: please choose a number between 1 and %d.",
                     MAX_BOOKS);
            send_msg(sess->fd, MSG_RESERVE_FAIL, payload);
            log_event("RESERVE  INVALID  %s requested index %d",
                      sess->lib_id, book_idx);
            continue;
        }

        int bidx = book_idx - 1;   /* convert to 0-based array index         */

        /* ── CRITICAL SECTION: check-then-act reservation ────────────────────
         * The entire read + conditional update is performed atomically under
         * books_mutex.  Any other thread attempting a reservation for the
         * same book will block here until we release the lock, guaranteeing
         * that at most one reservation succeeds.
         * ─────────────────────────────────────────────────────────────────── */
        pthread_mutex_lock(&books_mutex);

        if (books[bidx].reserved_by[0] != '\0') {
            /* Book already reserved — send rejection */
            snprintf(payload, sizeof(payload),
                     "Reservation failed: \"%s\" is already reserved by %s.",
                     books[bidx].title, books[bidx].reserved_by);
            pthread_mutex_unlock(&books_mutex);

            send_msg(sess->fd, MSG_RESERVE_FAIL, payload);
            log_event("RESERVE  DENIED  %s requested \"%s\" (held by %s)",
                      sess->lib_id, books[bidx].title, books[bidx].reserved_by);

        } else {
            /* Book available — mark as reserved and persist */
            strncpy(books[bidx].reserved_by, sess->lib_id, MAX_LIB_ID_LEN - 1);
            save_books();   /* called under books_mutex — file write is safe  */
            pthread_mutex_unlock(&books_mutex);

            snprintf(payload, sizeof(payload),
                     "Reservation confirmed: \"%s\" is now reserved for %s.",
                     books[bidx].title, sess->lib_id);
            send_msg(sess->fd, MSG_RESERVE_OK, payload);
            log_event("RESERVE  OK     %s reserved \"%s\"",
                      sess->lib_id, books[bidx].title);
            print_server_status();
        }

        /* Send updated book list so client sees current availability */
        pthread_mutex_lock(&books_mutex);
        build_book_list_payload(payload, sizeof(payload));
        pthread_mutex_unlock(&books_mutex);
        send_msg(sess->fd, MSG_BOOK_LIST, payload);
    }

    /* ════════════════════════════════════════════════════════════════════════
     * CLEANUP: remove session from active list, close socket
     * ════════════════════════════════════════════════════════════════════════ */
cleanup:
    close(sess->fd);

    pthread_mutex_lock(&users_mutex);
    sess->active        = 0;
    sess->authenticated = 0;
    sess->lib_id[0]     = '\0';
    active_count--;
    pthread_mutex_unlock(&users_mutex);

    log_event("CLEAN  Slot %d released. Active sessions: %d",
              slot, active_count);
    print_server_status();

    return NULL;
}


/* ==========================================================================
 * Signal handler — SIGINT
 * --------------------------------------------------------------------------
 * Async-signal-safe: only sets a flag and closes the listen socket.
 * The main thread detects the flag and handles the orderly shutdown.
 * ========================================================================== */
static void sigint_handler(int sig)
{
    (void)sig;
    shutdown_flag = 1;
    if (listen_fd >= 0) close(listen_fd);
}


/* ==========================================================================
 * main
 * ========================================================================== */
int main(void)
{
    /* ── Open log file ────────────────────────────────────────────────────── */
    log_file = fopen(LOG_FILENAME, "a");
    if (!log_file)
        fprintf(stderr, "WARN: could not open %s for logging\n", LOG_FILENAME);

    log_event("STARTUP  Digital Library Reservation Server starting...");
    log_event("INFO     Listening on port %d | Max clients: %d | Timeout: %ds",
              SERVER_PORT, MAX_CLIENTS, CLIENT_TIMEOUT_SEC);

    /* ── Load persistent book state ──────────────────────────────────────── */
    pthread_mutex_lock(&books_mutex);
    load_books();
    pthread_mutex_unlock(&books_mutex);

    /* ── Install signal handler ───────────────────────────────────────────── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGPIPE — writing to a closed socket returns EPIPE instead of
     * delivering a signal that would terminate the process silently.         */
    signal(SIGPIPE, SIG_IGN);

    /* ── Create listening socket ──────────────────────────────────────────── */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    /* SO_REUSEADDR allows re-binding immediately after server restart
     * without waiting for TIME_WAIT sockets to expire.                       */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, SERVER_BACKLOG) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    log_event("READY  Server accepting connections on port %d", SERVER_PORT);
    print_server_status();

    /* ── Accept loop ─────────────────────────────────────────────────────── */
    while (!shutdown_flag) {
        struct sockaddr_in client_addr;
        socklen_t          addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (shutdown_flag) break;           /* SIGINT closed the socket   */
            if (errno == EINTR) continue;       /* other signal, retry        */
            perror("accept");
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        /* ── Find a free session slot ─────────────────────────────────────── */
        pthread_mutex_lock(&users_mutex);

        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!sessions[i].active) { slot = i; break; }
        }

        if (slot == -1) {
            /* Server at capacity */
            pthread_mutex_unlock(&users_mutex);
            log_event("FULL   Rejected %s — server at capacity (%d/%d)",
                      client_ip, active_count, MAX_CLIENTS);
            send_msg(client_fd, MSG_SERVER_FULL,
                     "Server is at capacity. Please try again later.");
            close(client_fd);
            continue;
        }

        /* Initialise session slot */
        sessions[slot].fd             = client_fd;
        sessions[slot].active         = 1;
        sessions[slot].authenticated  = 0;
        sessions[slot].lib_id[0]      = '\0';
        sessions[slot].connect_time   = time(NULL);
        strncpy(sessions[slot].ip, client_ip, INET_ADDRSTRLEN - 1);
        active_count++;

        pthread_mutex_unlock(&users_mutex);

        /* Spawn client thread.
         * pthread_detach() frees the thread's resources automatically when
         * it exits, since we will not call pthread_join() on it.
         * We track active state via sessions[].active instead.              */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&sessions[slot].thread, &attr,
                           handle_client, &sessions[slot]) != 0) {
            log_event("ERROR  pthread_create failed for slot %d: %s",
                      slot, strerror(errno));
            pthread_mutex_lock(&users_mutex);
            sessions[slot].active = 0;
            active_count--;
            pthread_mutex_unlock(&users_mutex);
            close(client_fd);
        }
        pthread_attr_destroy(&attr);
    }

    /* ── Graceful shutdown ────────────────────────────────────────────────── */
    log_event("SHUTDOWN  SIGINT received — shutting down...");

    /* Wake up all blocked client threads by shutting down their sockets */
    pthread_mutex_lock(&users_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active) {
            send_msg(sessions[i].fd, MSG_SERVER_INFO,
                     "Server is shutting down. Goodbye.");
            shutdown(sessions[i].fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&users_mutex);

    /* Brief pause to allow threads to finish their cleanup paths */
    sleep(1);

    /* Save final book state */
    pthread_mutex_lock(&books_mutex);
    save_books();
    pthread_mutex_unlock(&books_mutex);

    log_event("SHUTDOWN  Final book state saved. Server exiting cleanly.");

    if (log_file) fclose(log_file);

    pthread_mutex_destroy(&books_mutex);
    pthread_mutex_destroy(&users_mutex);
    pthread_mutex_destroy(&log_mutex);

    return 0;
}

