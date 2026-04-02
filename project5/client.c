/*
 * client.c
 * ==========================================================================
 * Digital Library Reservation Client
 *
 * SESSION FLOW
 * ------------
 *   1. Connect to server via TCP.
 *   2. Prompt user for library ID  → send MSG_AUTH_REQ.
 *   3. Handle MSG_AUTH_OK or MSG_AUTH_FAIL.
 *   4. If authenticated:
 *        a. Receive and display book list (MSG_BOOK_LIST).
 *        b. Prompt for book selection.
 *        c. Send MSG_RESERVE_REQ.
 *        d. Display reservation result.
 *        e. Receive updated book list — loop back to (b), or type 0 to exit.
 *   5. Send MSG_DISCONNECT.  Print farewell.
 *
 * USAGE
 * -----
 *   ./client [server_ip]        (default: 127.0.0.1)
 * ==========================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "protocol.h"

/* ── ANSI colour helpers ──────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_GREEN   "\033[92m"
#define C_RED     "\033[91m"
#define C_YELLOW  "\033[93m"
#define C_CYAN    "\033[96m"
#define C_BLUE    "\033[94m"

static int server_fd = -1;   /* global so signal handler can close it        */


/* ==========================================================================
 * display_book_list
 * --------------------------------------------------------------------------
 * Parses the serialised book-list payload received from the server:
 *
 *   Line 1: "<count>\n"
 *   Lines 2+: "<index>|<title>|<status>\n"
 *
 * Prints a formatted table to stdout.
 * ========================================================================== */
static void display_book_list(const char *payload)
{
    printf("\n" C_BOLD C_BLUE "  ╔══════════════════════════════════════════════════════════╗\n");
    printf(                   "  ║             AVAILABLE LIBRARY CATALOGUE                  ║\n");
    printf(                   "  ╠══════════════════════════════════════════════════════════╣\n" C_RESET);

    char buf[MAX_PAYLOAD_LEN];
    strncpy(buf, payload, MAX_PAYLOAD_LEN - 1);
    buf[MAX_PAYLOAD_LEN - 1] = '\0';

    char *line = strtok(buf, "\n");
    if (!line) return;

    int count = atoi(line);    /* first line is the book count — skip display */
    (void)count;

    while ((line = strtok(NULL, "\n")) != NULL) {
        /* Parse "index|title|status" */
        char *idx_str = strtok(line, "|");
        char *title   = strtok(NULL, "|");
        char *status  = strtok(NULL, "|");
        if (!idx_str || !title || !status) continue;

        int is_avail = (strcmp(status, "available") == 0);

        /* Colour status indicator */
        const char *status_str = is_avail
            ? C_GREEN "AVAILABLE" C_RESET
            : C_RED   "RESERVED " C_RESET;

        if (is_avail) {
            printf(C_BOLD "  ║  [%s] %-42s " C_RESET "%s  " C_BLUE "║\n" C_RESET,
                   idx_str, title, status_str);
        } else {
            /* Show who holds the reservation */
            printf("  ║  [%s] %-42s %s  " C_BLUE "║\n" C_RESET,
                   idx_str, title, status_str);
        }
    }

    printf(C_BLUE "  ╚══════════════════════════════════════════════════════════╝\n" C_RESET);
    printf("\n");
}


/* ==========================================================================
 * signal handler — SIGINT (Ctrl+C)
 * --------------------------------------------------------------------------
 * Send a clean disconnect message before exiting so the server can update
 * its active-users list rather than waiting for a timeout.
 * ========================================================================== */
static char g_lib_id[MAX_LIB_ID_LEN] = "";

static void sigint_handler(int sig)
{
    (void)sig;
    printf("\n" C_YELLOW "Interrupted. Sending disconnect..." C_RESET "\n");
    if (server_fd >= 0) {
        send_msg(server_fd, MSG_DISCONNECT, "");
        close(server_fd);
    }
    printf(C_CYAN "Session closed. Goodbye, %s\n" C_RESET,
           g_lib_id[0] ? g_lib_id : "(unauthenticated)");
    exit(0);
}


/* ==========================================================================
 * main
 * ========================================================================== */
int main(int argc, char *argv[])
{
    const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";

    /* Install Ctrl+C handler for clean disconnect */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* ── Connect to server ────────────────────────────────────────────────── */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port   = htons(SERVER_PORT);

    if (inet_pton(AF_INET, server_ip, &srv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        return 1;
    }
    if (connect(server_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("connect");
        fprintf(stderr, "Could not connect to server at %s:%d\n",
                server_ip, SERVER_PORT);
        return 1;
    }

    printf(C_BOLD C_CYAN
           "\n  ╔══════════════════════════════════════════╗\n"
           "  ║   Digital Library Reservation System    ║\n"
           "  ║   Connected to %s:%-5d             ║\n"
           "  ╚══════════════════════════════════════════╝\n"
           C_RESET "\n", server_ip, SERVER_PORT);

    Message msg;

    /* ── Check for server-full message ───────────────────────────────────── */
    /* The server may immediately send MSG_SERVER_FULL before we even prompt  */
    /* We handle this by peeking at the first incoming message.               */

    /* ── STEP 1: Authentication ───────────────────────────────────────────── */
    char lib_id_input[MAX_LIB_ID_LEN];
    printf(C_BOLD "  Enter your Library ID: " C_RESET);
    fflush(stdout);

    if (!fgets(lib_id_input, sizeof(lib_id_input), stdin)) {
        close(server_fd); return 1;
    }
    lib_id_input[strcspn(lib_id_input, "\r\n")] = '\0';   /* strip newline   */

    /* Store for signal handler */
    strncpy(g_lib_id, lib_id_input, MAX_LIB_ID_LEN - 1);

    /* Send authentication request */
    if (send_msg(server_fd, MSG_AUTH_REQ, lib_id_input) < 0) {
        fprintf(stderr, "Failed to send auth request.\n");
        close(server_fd); return 1;
    }

    /* Receive authentication response */
    int r = recv_msg(server_fd, &msg);
    if (r <= 0) {
        fprintf(stderr, "Server disconnected unexpectedly.\n");
        close(server_fd); return 1;
    }

    /* Handle server-full scenario */
    if (msg.type == MSG_SERVER_FULL) {
        printf("\n" C_RED "  ✗ %s\n" C_RESET "\n", msg.payload);
        close(server_fd); return 0;
    }

    if (msg.type == MSG_AUTH_FAIL) {
        printf("\n" C_RED "  ✗ %s\n" C_RESET "\n", msg.payload);
        close(server_fd); return 0;
    }

    if (msg.type != MSG_AUTH_OK) {
        fprintf(stderr, "Unexpected message type 0x%02X\n", msg.type);
        close(server_fd); return 1;
    }

    printf("\n" C_GREEN "  ✓ %s\n" C_RESET, msg.payload);

    /* ── STEP 2: Receive and display book list ────────────────────────────── */
    r = recv_msg(server_fd, &msg);
    if (r <= 0 || msg.type != MSG_BOOK_LIST) {
        fprintf(stderr, "Failed to receive book list.\n");
        close(server_fd); return 1;
    }

    display_book_list(msg.payload);

    /* ── STEP 3: Reservation loop ─────────────────────────────────────────── */
    while (1) {
        printf(C_BOLD "  Enter book number to reserve (0 to exit): " C_RESET);
        fflush(stdout);

        char input_buf[16];
        if (!fgets(input_buf, sizeof(input_buf), stdin)) break;
        input_buf[strcspn(input_buf, "\r\n")] = '\0';

        int choice = atoi(input_buf);

        if (choice == 0) {
            /* User wants to exit */
            send_msg(server_fd, MSG_DISCONNECT, "");
            break;
        }

        if (choice < 1 || choice > MAX_BOOKS) {
            printf(C_YELLOW "  Please enter a number between 1 and %d, or 0 to exit.\n"
                   C_RESET, MAX_BOOKS);
            continue;
        }

        /* Send reservation request */
        snprintf(input_buf, sizeof(input_buf), "%d", choice);
        if (send_msg(server_fd, MSG_RESERVE_REQ, input_buf) < 0) {
            fprintf(stderr, "  Failed to send reservation request.\n");
            break;
        }

        /* Receive reservation result */
        r = recv_msg(server_fd, &msg);
        if (r <= 0) {
            fprintf(stderr, "  Server disconnected.\n");
            break;
        }

        if (msg.type == MSG_RESERVE_OK) {
            printf("\n" C_GREEN "  ✓ %s\n" C_RESET "\n", msg.payload);
        } else if (msg.type == MSG_RESERVE_FAIL) {
            printf("\n" C_RED "  ✗ %s\n" C_RESET "\n", msg.payload);
        } else if (msg.type == MSG_SERVER_INFO) {
            printf("\n" C_YELLOW "  ℹ %s\n" C_RESET "\n", msg.payload);
            break;   /* e.g. timeout message — server will close connection    */
        }

        /* Receive updated book list after every reservation attempt */
        r = recv_msg(server_fd, &msg);
        if (r <= 0 || msg.type != MSG_BOOK_LIST) break;
        display_book_list(msg.payload);
    }

    /* ── Farewell ─────────────────────────────────────────────────────────── */
    close(server_fd);
    server_fd = -1;
    printf(C_CYAN "\n  Session closed. Goodbye, %s\n\n" C_RESET, lib_id_input);
    return 0;
}

