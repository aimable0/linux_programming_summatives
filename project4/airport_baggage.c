/*
 * =============================================================================
 * Airport Baggage Handling System — Multithreaded Simulation (POSIX Threads)
 * =============================================================================
 *
 * OVERVIEW
 * --------
 * This program simulates a simplified airport baggage handling system using
 * three concurrent POSIX threads:
 *
 *   1. Conveyor Belt Loader  (Producer)  — loads luggage every 2 s
 *   2. Aircraft Loader       (Consumer)  — dispatches luggage every 4 s
 *   3. Monitor               (Reporter)  — prints statistics every 5 s
 *
 * SHARED MEMORY & SYNCHRONISATION
 * --------------------------------
 * All shared state lives in a single ConveyorBelt struct protected by ONE
 * mutex (belt_mutex).  Two condition variables gate the two blocking cases:
 *
 *   • cond_not_full  — Producer waits here when the belt is at capacity (5).
 *                      Consumer signals it after removing an item.
 *   • cond_not_empty — Consumer waits here when the belt is empty.
 *                      Producer signals it after adding an item.
 *
 * The Monitor thread acquires the SAME mutex before reading any counters,
 * so it never sees a partially-updated state and never causes deadlocks
 * (it holds the lock only for the duration of a memcpy / read, then releases).
 *
 * CIRCULAR BUFFER
 * ---------------
 * The belt is implemented as a fixed-size circular (ring) buffer of capacity
 * BELT_CAPACITY.  Two indices (head, tail) advance modulo BELT_CAPACITY:
 *   • tail — where the next item is written  (Producer advances tail)
 *   • head — where the next item is read     (Consumer advances head)
 * This avoids any shifting of elements and gives O(1) enqueue / dequeue.
 *
 * GRACEFUL TERMINATION
 * --------------------
 * The program stops after TOTAL_LUGGAGE (10) items have been fully dispatched
 * to the aircraft.  A global atomic-style flag (done) is set once this limit
 * is reached; all threads check it and exit cleanly.
 * SIGINT (Ctrl-C) also sets the flag so the user can interrupt at any time.
 *
 * COMPILE
 * -------
 *   gcc -Wall -Wextra -pthread -o airport_baggage airport_baggage.c
 *
 * RUN
 * ---
 *   ./airport_baggage
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>

/* ── Configuration constants ─────────────────────────────────────────────── */
#define BELT_CAPACITY   5   /* Maximum items on the conveyor belt at once     */
#define TOTAL_LUGGAGE  10   /* Producer loads exactly this many; consumer dispatches all of them */
#define LOAD_TIME       2   /* Seconds for producer to add one item           */
#define DISPATCH_TIME   4   /* Seconds for consumer to remove one item        */
#define MONITOR_PERIOD  5   /* Seconds between monitor reports                */

/* ── Luggage item ────────────────────────────────────────────────────────── */
typedef struct {
    int id;   /* Unique, incremental luggage identifier */
} Luggage;

/* ── Shared conveyor belt (circular buffer + counters) ───────────────────── */
typedef struct {
    Luggage  buffer[BELT_CAPACITY]; /* Ring buffer holding luggage items      */
    int      head;                  /* Index of next item to consume          */
    int      tail;                  /* Index where next item will be placed   */
    int      count;                 /* Current number of items on the belt    */

    int      total_loaded;          /* Cumulative items placed by producer    */
    int      total_dispatched;      /* Cumulative items removed by consumer   */

    pthread_mutex_t belt_mutex;     /* Guards ALL fields above                */
    pthread_cond_t  cond_not_full;  /* Producer waits here; consumer signals  */
    pthread_cond_t  cond_not_empty; /* Consumer waits here; producer signals  */
} ConveyorBelt;

/* ── Global state ────────────────────────────────────────────────────────── */
static ConveyorBelt belt;   /* The one shared belt structure                 */
static volatile int done = 0; /* Set to 1 to request all threads to exit     */

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper: thread-safe timestamped log line
 * ═══════════════════════════════════════════════════════════════════════════ */
static void log_event(const char *role, const char *msg)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    /* Format:  [HH:MM:SS]  [ROLE]  message */
    printf("[%02d:%02d:%02d] %-22s %s\n",
           t->tm_hour, t->tm_min, t->tm_sec, role, msg);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal handler — catches SIGINT (Ctrl-C) for graceful shutdown
 * ═══════════════════════════════════════════════════════════════════════════ */
static void handle_sigint(int sig)
{
    (void)sig;
    /*
     * Setting 'done' here is safe in a signal handler because it is only
     * written once (to 1) and all threads treat it as a one-way latch.
     * We do NOT call mutex-lock here — that would be async-signal-unsafe.
     */
    done = 1;
    write(STDOUT_FILENO,
          "\n[SIGNAL] SIGINT received. Shutting down…\n", 43);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PRODUCER THREAD — Conveyor Belt Loader
 *
 * Logic:
 *   loop:
 *     sleep(LOAD_TIME)          ← simulates physical loading time
 *     lock mutex
 *     while belt is full → wait on cond_not_full
 *     enqueue item at buffer[tail], advance tail (mod BELT_CAPACITY)
 *     increment count & total_loaded
 *     signal cond_not_empty     ← wake consumer if it was sleeping
 *     unlock mutex
 * ═══════════════════════════════════════════════════════════════════════════ */
static void *producer_thread(void *arg)
{
    (void)arg;
    int next_id = 1; /* Luggage IDs start at 1 */

    /*
     * PRODUCER LIMIT
     * The loop runs at most TOTAL_LUGGAGE times.  Once all items have been
     * placed on the belt the producer exits, letting the consumer drain
     * whatever remains before the whole program terminates.
     */
    while (!done && next_id <= TOTAL_LUGGAGE) {
        /* Simulate the time taken to prepare/load one luggage item */
        sleep(LOAD_TIME);

        if (done) break;

        /* ── Enter critical section ─────────────────────────────────────── */
        pthread_mutex_lock(&belt.belt_mutex);

        /*
         * FULL BELT HANDLING
         * If the belt is at maximum capacity we must wait.
         * pthread_cond_wait atomically:
         *   1. Releases belt_mutex (so the consumer can acquire it and make room)
         *   2. Puts this thread to sleep on cond_not_full
         * When signalled by the consumer, it re-acquires belt_mutex before
         * returning, so we are back inside the critical section.
         * The while loop (not if) guards against spurious wake-ups.
         */
        while (belt.count == BELT_CAPACITY && !done) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "Belt FULL (%d/%d) — waiting for space…", belt.count, BELT_CAPACITY);
            log_event("[PRODUCER]", msg);
            pthread_cond_wait(&belt.cond_not_full, &belt.belt_mutex);
        }

        if (done) {
            pthread_mutex_unlock(&belt.belt_mutex);
            break;
        }

        /* ── Place item in circular buffer ──────────────────────────────── */
        Luggage item;
        item.id = next_id++;
        belt.buffer[belt.tail] = item;
        belt.tail = (belt.tail + 1) % BELT_CAPACITY; /* Advance tail (wraps) */
        belt.count++;
        belt.total_loaded++;

        int remaining = TOTAL_LUGGAGE - belt.total_loaded;
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "Loaded  luggage #%02d  │ Belt: %d/%d  (remaining to load: %d)",
                 item.id, belt.count, BELT_CAPACITY, remaining < 0 ? 0 : remaining);
        log_event("[PRODUCER]", msg);

        /*
         * SIGNAL cond_not_empty
         * The consumer may be sleeping because the belt was empty.
         * Now that we have added an item, wake it up.
         */
        pthread_cond_signal(&belt.cond_not_empty);
        pthread_mutex_unlock(&belt.belt_mutex);
        /* ── Exit critical section ──────────────────────────────────────── */
    }

    /*
     * Producer is done loading.  Broadcast cond_not_empty one final time so
     * that a waiting consumer is definitely unblocked and can drain the belt.
     */
    log_event("[PRODUCER]",
              "All items loaded onto belt. Producer done — waiting for consumer to drain.");
    pthread_mutex_lock(&belt.belt_mutex);
    pthread_cond_broadcast(&belt.cond_not_empty);
    pthread_mutex_unlock(&belt.belt_mutex);

    log_event("[PRODUCER]", "Exiting.");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CONSUMER THREAD — Aircraft Loader
 *
 * Logic:
 *   loop:
 *     lock mutex
 *     while belt is empty → wait on cond_not_empty
 *     dequeue item from buffer[head], advance head (mod BELT_CAPACITY)
 *     decrement count, increment total_dispatched
 *     signal cond_not_full      ← wake producer if it was sleeping
 *     unlock mutex
 *     sleep(DISPATCH_TIME)      ← simulates physical loading into aircraft
 *     if total_dispatched >= TOTAL_LUGGAGE → set done = 1, break
 * ═══════════════════════════════════════════════════════════════════════════ */
static void *consumer_thread(void *arg)
{
    (void)arg;

    while (!done) {
        /* ── Enter critical section ─────────────────────────────────────── */
        pthread_mutex_lock(&belt.belt_mutex);

        /*
         * EMPTY BELT HANDLING
         * If there are no items to dispatch, we wait.
         * pthread_cond_wait releases the mutex so the producer can add items,
         * then re-acquires it when signalled.  The while loop handles spurious
         * wake-ups and the done flag.
         */
        while (belt.count == 0 && !done) {
            log_event("[CONSUMER]", "Belt EMPTY — waiting for luggage…");
            pthread_cond_wait(&belt.cond_not_empty, &belt.belt_mutex);
        }

        if (done && belt.count == 0) {
            pthread_mutex_unlock(&belt.belt_mutex);
            break;
        }

        /* ── Remove item from circular buffer ───────────────────────────── */
        Luggage item = belt.buffer[belt.head];
        belt.head = (belt.head + 1) % BELT_CAPACITY; /* Advance head (wraps) */
        belt.count--;
        belt.total_dispatched++;
        int dispatched_so_far = belt.total_dispatched;

        char msg[80];
        snprintf(msg, sizeof(msg),
                 "Dispatched luggage #%02d │ Belt: %d/%d  (total dispatched: %d)",
                 item.id, belt.count, BELT_CAPACITY, dispatched_so_far);
        log_event("[CONSUMER]", msg);

        /*
         * SIGNAL cond_not_full
         * The producer may be sleeping because the belt was full.
         * Now that we have freed a slot, wake it up.
         */
        pthread_cond_signal(&belt.cond_not_full);
        pthread_mutex_unlock(&belt.belt_mutex);
        /* ── Exit critical section ──────────────────────────────────────── */

        /* Simulate time to physically load the item onto the aircraft */
        sleep(DISPATCH_TIME);

        /* ── Check termination condition ────────────────────────────────── */
        if (dispatched_so_far >= TOTAL_LUGGAGE) {
            char fin[64];
            snprintf(fin, sizeof(fin),
                     "All %d items dispatched. Setting shutdown flag.", TOTAL_LUGGAGE);
            log_event("[CONSUMER]", fin);
            done = 1;
            /*
             * Broadcast to BOTH condition variables so that any threads
             * waiting inside pthread_cond_wait are unblocked and can check
             * 'done' and exit cleanly.  Without this, the producer (if
             * sleeping on cond_not_full) would block forever.
             */
            pthread_mutex_lock(&belt.belt_mutex);
            pthread_cond_broadcast(&belt.cond_not_full);
            pthread_cond_broadcast(&belt.cond_not_empty);
            pthread_mutex_unlock(&belt.belt_mutex);
            break;
        }
    }

    log_event("[CONSUMER]", "Exiting.");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MONITOR THREAD — Statistics Reporter
 *
 * Runs every MONITOR_PERIOD seconds.  Takes a snapshot of the shared counters
 * while holding belt_mutex, then releases the lock before printing.  This
 * approach (lock → copy → unlock → print) minimises the time the mutex is
 * held and prevents the monitor from stalling the producer or consumer.
 *
 * MONITOR THREAD SAFETY
 * ─────────────────────
 * • Acquires the same belt_mutex as the other threads → no torn reads.
 * • Copies counters into local variables → no pointer aliasing issues.
 * • Does NOT call pthread_cond_wait → can never deadlock.
 * • Does NOT signal any condition → cannot accidentally wake threads.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void *monitor_thread(void *arg)
{
    (void)arg;
    int report_no = 0;

    while (!done) {
        sleep(MONITOR_PERIOD);
        if (done) break;

        /* ── Snapshot shared state under lock ───────────────────────────── */
        pthread_mutex_lock(&belt.belt_mutex);
        int snap_loaded     = belt.total_loaded;
        int snap_dispatched = belt.total_dispatched;
        int snap_count      = belt.count;
        pthread_mutex_unlock(&belt.belt_mutex);
        /* ── Lock released — safe to do I/O ────────────────────────────── */

        report_no++;
        printf("\n");
        printf("╔══════════════════════════════════════════╗\n");
        printf("║          MONITOR REPORT #%-2d               ║\n", report_no);
        printf("╠══════════════════════════════════════════╣\n");
        printf("║  Total loaded onto belt  : %-14d║\n", snap_loaded);
        printf("║  Total dispatched        : %-14d║\n", snap_dispatched);
        printf("║  Current belt occupancy  : %d / %-10d║\n",
               snap_count, BELT_CAPACITY);
        printf("╚══════════════════════════════════════════╝\n\n");
        fflush(stdout);
    }

    log_event("[MONITOR]", "Exiting.");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * INITIALISATION & CLEANUP
 * ═══════════════════════════════════════════════════════════════════════════ */
static void belt_init(void)
{
    memset(&belt, 0, sizeof(belt)); /* Zero all fields (head=0, tail=0, count=0) */

    /*
     * pthread_mutex_init with NULL attr → default (fast) mutex.
     * pthread_cond_init  with NULL attr → default condition variable.
     */
    if (pthread_mutex_init(&belt.belt_mutex, NULL) != 0) {
        perror("pthread_mutex_init"); exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&belt.cond_not_full, NULL) != 0) {
        perror("pthread_cond_init cond_not_full"); exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&belt.cond_not_empty, NULL) != 0) {
        perror("pthread_cond_init cond_not_empty"); exit(EXIT_FAILURE);
    }
}

static void belt_destroy(void)
{
    pthread_mutex_destroy(&belt.belt_mutex);
    pthread_cond_destroy(&belt.cond_not_full);
    pthread_cond_destroy(&belt.cond_not_empty);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   Airport Baggage Handling System — Starting     ║\n");
    printf("║   Belt capacity : %-5d   Target items : %-5d    ║\n",
           BELT_CAPACITY, TOTAL_LUGGAGE);
    printf("║   Producer      : %-3ds    Consumer : %-3ds       ║\n",
           LOAD_TIME, DISPATCH_TIME);
    printf("║   Monitor       : every %-3ds                     ║\n",
           MONITOR_PERIOD);
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* Register SIGINT handler for graceful Ctrl-C shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* Initialise shared belt */
    belt_init();

    /* Create threads */
    pthread_t prod_tid, cons_tid, mon_tid;

    if (pthread_create(&prod_tid, NULL, producer_thread, NULL) != 0) {
        perror("pthread_create producer"); exit(EXIT_FAILURE);
    }
    if (pthread_create(&cons_tid, NULL, consumer_thread, NULL) != 0) {
        perror("pthread_create consumer"); exit(EXIT_FAILURE);
    }
    if (pthread_create(&mon_tid, NULL, monitor_thread, NULL) != 0) {
        perror("pthread_create monitor"); exit(EXIT_FAILURE);
    }

    /* Wait for all threads to finish */
    pthread_join(prod_tid, NULL);
    pthread_join(cons_tid, NULL);
    pthread_join(mon_tid,  NULL);

    /* ── Final statistics summary ────────────────────────────────────────── */
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║           FINAL STATISTICS               ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  Total items loaded onto belt : %-8d║\n", belt.total_loaded);
    printf("║  Total items dispatched       : %-8d║\n", belt.total_dispatched);
    printf("║  Items remaining on belt      : %-8d║\n", belt.count);
    printf("╚══════════════════════════════════════════╝\n");

    /* Cleanup synchronisation primitives */
    belt_destroy();

    printf("\n[MAIN] All threads joined. Program terminated cleanly.\n");
    return EXIT_SUCCESS;
}

