/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implements all six tasks:
 *   Task 1 - Multi-container supervisor with clone() + namespaces
 *   Task 2 - CLI over UNIX domain socket (Path B IPC)
 *   Task 3 - Bounded-buffer logging pipeline with producer/consumer threads
 *   Task 4 - Kernel module integration via ioctl
 *   Task 5 - Scheduling experiment support (nice values)
 *   Task 6 - Clean teardown of all resources
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <linux/limits.h>

#include "monitor_ioctl.h"

/* Suppress warn_unused_result for socket writes to disconnected clients */
#define SEND(fd, buf, len) do { if(write((fd),(buf),(len)) < 0) {} } while(0)

/* ================================================================
 * Constants
 * ================================================================ */
#define CONTAINER_ID_LEN      64
#define LOG_CHUNK_SIZE         4096
#define LOG_BUFFER_CAPACITY    16
#define CHILD_COMMAND_LEN      256
#define CONTROL_MESSAGE_LEN    1024
#define STACK_SIZE             (1024 * 1024)   /* 1 MiB clone stack */
#define CONTROL_PATH           "/tmp/mini_runtime.sock"
#define MONITOR_DEVICE         "/dev/container_monitor"
#define LOG_DIR                "./logs"
#define DEFAULT_SOFT_LIMIT     (40UL * 1024 * 1024)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT     (64UL * 1024 * 1024)   /* 64 MiB */

/* ================================================================
 * Enums
 * ================================================================ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,          /* cleanly stopped via engine stop   */
    CONTAINER_KILLED,           /* killed by a non-SIGKILL signal    */
    CONTAINER_EXITED,           /* exited normally on its own        */
    CONTAINER_HARD_LIMIT_KILLED /* SIGKILL from kernel memory module */
} container_state_t;

/* ================================================================
 * Data structures
 * ================================================================ */

/*
 * container_record_t
 * One node per container in a linked list owned by supervisor_ctx_t.
 * All fields except those written once at creation time are protected
 * by supervisor_ctx_t.metadata_lock.
 *
 * stop_requested:
 *   Set to 1 by the supervisor BEFORE sending SIGTERM on a "stop"
 *   command.  The SIGCHLD handler reads this flag to decide whether
 *   to mark the container as STOPPED (intentional) or
 *   HARD_LIMIT_KILLED (SIGKILL from the kernel module).
 */
typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;   /* 1 = we sent the stop signal */
    char              log_path[PATH_MAX];
    int               pipe_read_fd;     /* supervisor reads container output */
    struct container_record *next;
} container_record_t;

/*
 * log_item_t
 * One chunk of output captured from a container.
 * Stored inside the bounded buffer until consumed.
 */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

/*
 * bounded_buffer_t
 * Ring buffer shared between producer threads (one per container)
 * and the single consumer (logging) thread.
 *
 * Synchronisation:
 *   mutex    - protects head, tail, count, shutting_down
 *   not_full - producer waits here when count == capacity
 *   not_empty- consumer waits here when count == 0
 */
typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/*
 * control_request_t / control_response_t
 * Fixed-size structs exchanged over the UNIX domain socket.
 * Using fixed structs (not text) avoids partial-read issues.
 */
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;                        /* 0 = ok, -1 = error            */
    char message[CONTROL_MESSAGE_LEN];  /* human-readable response text   */
} control_response_t;

/*
 * child_config_t
 * Passed to child_fn() through the clone() arg pointer.
 * Must stay valid until child_fn() has finished reading it.
 */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;   /* write end of the pipe to the supervisor */
} child_config_t;

/*
 * supervisor_ctx_t
 * The single global state object for the supervisor daemon.
 * Passed to threads and signal handlers via the global g_ctx pointer.
 */
typedef struct {
    int               server_fd;       /* listening UNIX socket fd      */
    int               monitor_fd;      /* /dev/container_monitor fd     */
    volatile int      should_stop;     /* set by SIGTERM/SIGINT handler */
    pthread_t         logger_thread;   /* single consumer thread        */
    bounded_buffer_t  log_buffer;
    pthread_mutex_t   metadata_lock;   /* protects the containers list  */
    container_record_t *containers;    /* head of linked list           */
} supervisor_ctx_t;

/* producer thread arguments */
typedef struct {
    supervisor_ctx_t *ctx;
    char              container_id[CONTAINER_ID_LEN];
    int               pipe_read_fd;
} producer_args_t;

/* ================================================================
 * Global supervisor pointer (needed by signal handlers)
 * ================================================================ */
static supervisor_ctx_t *g_ctx = NULL;

/* ================================================================
 * Forward declarations
 * ================================================================ */
static int send_control_request(const control_request_t *req);

/* ================================================================
 * Usage
 * ================================================================ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ================================================================
 * Argument parsers
 * ================================================================ */
static int parse_mib_flag(const char *flag,
                           const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nv;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value (must be -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

/* ================================================================
 * Bounded buffer — init / destroy / shutdown
 * ================================================================ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    /* Wake ALL blocked producers and the consumer so they can exit */
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* ================================================================
 * Task 3 — bounded_buffer_push  (producer side)
 *
 * WHY:
 *   A producer thread reads bytes from a container's pipe and must
 *   hand them off to the consumer without dropping data.  It blocks
 *   on not_full when the ring is full, which prevents it from
 *   overwriting unconsumed entries.  It returns -1 on shutdown so
 *   the producer loop exits cleanly.
 *
 * RACE WITHOUT MUTEX:
 *   Two producers could simultaneously read count, see "room", and
 *   both write to the same tail slot — corrupting one entry.
 * ================================================================ */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    /* Block while full, but exit immediately on shutdown */
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;                           /* copy into ring  */
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);                  /* wake consumer   */
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ================================================================
 * Task 3 — bounded_buffer_pop  (consumer side)
 *
 * Returns 0  → valid item written into *item
 * Returns 1  → shutdown + buffer empty, consumer should exit
 *
 * WHY WHILE LOOP:
 *   POSIX allows spurious wakeups from pthread_cond_wait.  We must
 *   re-check the condition after every wake to be correct.
 * ================================================================ */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    /* Block while empty and not shutting down */
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    /* If still empty after waking, we're done */
    if (b->count == 0) {
        pthread_mutex_unlock(&b->mutex);
        return 1;   /* signal consumer to exit */
    }

    *item = b->items[b->head];                           /* copy out        */
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);                   /* wake a producer */
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ================================================================
 * Task 3 — logging_thread  (the single consumer)
 *
 * WHY ONE CONSUMER:
 *   A single consumer serialises all writes to log files, so we
 *   never need to lock the file itself.  The bounded buffer provides
 *   the thread-safe hand-off from multiple producers.
 *
 * SHUTDOWN CONTRACT:
 *   bounded_buffer_begin_shutdown() broadcasts on not_empty.
 *   pop() returns 1 when the buffer is empty AND shutting_down=1.
 *   We break out of the loop only then, guaranteeing every entry
 *   that was pushed before shutdown is written to disk first.
 * ================================================================ */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Build per-container log path */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        /* Append chunk to log file; open/close each time ensures durability */
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            /* Write all bytes; retry on short write */
            size_t written = 0;
            while (written < item.length) {
                ssize_t n = write(fd, item.data + written, item.length - written);
                if (n <= 0) break;
                written += (size_t)n;
            }
            close(fd);
        }
    }
    /* pop() returned 1 → buffer empty and shutdown signalled */
    return NULL;
}

/* ================================================================
 * Task 3 — producer_thread  (one per container)
 *
 * Reads raw bytes from the pipe attached to one container's
 * stdout/stderr and pushes them into the bounded buffer.
 * Exits when read() returns 0 (EOF = container exited and the
 * write end of the pipe is closed) or push fails (shutdown).
 * ================================================================ */
static void *producer_thread(void *arg)
{
    producer_args_t *pa = (producer_args_t *)arg;
    supervisor_ctx_t *ctx = pa->ctx;
    int fd = pa->pipe_read_fd;
    char cid[CONTAINER_ID_LEN];
    strncpy(cid, pa->container_id, CONTAINER_ID_LEN - 1);
    cid[CONTAINER_ID_LEN - 1] = '\0';
    free(pa);

    log_item_t item;
    ssize_t n;

    while ((n = read(fd, item.data, LOG_CHUNK_SIZE - 1)) > 0) {
        item.data[n] = '\0';
        item.length  = (size_t)n;
        strncpy(item.container_id, cid, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;   /* shutdown in progress */
    }

    close(fd);
    return NULL;
}

/* ================================================================
 * Task 1 — child_fn  (runs inside the container after clone())
 *
 * At entry we are inside the new namespaces but have not yet done
 * any isolation setup.  Steps:
 *
 *  1. Redirect stdout+stderr into the pipe write-end so all output
 *     flows to the supervisor's logging pipeline.
 *  2. Set a unique hostname using our private UTS namespace.
 *  3. Mount procfs at <rootfs>/proc while we still know the host
 *     path.  Because we have CLONE_NEWNS, this mount is invisible
 *     to the host.
 *  4. chroot() into the container's rootfs directory.  After this,
 *     "/" refers to the container's private filesystem.
 *  5. chdir("/") — mandatory after chroot; otherwise cwd is
 *     outside the new root and path resolution breaks.
 *  6. Apply the requested nice value.
 *  7. execv() the target command — replaces this process image.
 * ================================================================ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Step 1: Redirect stdout and stderr into the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Step 2: Set container hostname (private UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");   /* non-fatal */

    /* Step 3: Mount /proc inside the container rootfs
     *         Must happen before chroot while we can still name the path */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);   /* ignore error: may already exist */
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        perror("mount proc");   /* non-fatal */

    /* Step 4: chroot into the container's private rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    /* Step 5: Change working directory inside the new root */
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Step 6: Apply scheduling nice value */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");
    }

    /* Step 7: Execute the container command */
    char *exec_argv[] = { cfg->command, NULL };
    execv(cfg->command, exec_argv);

    /* Only reached if execv fails */
    perror("execv");
    return 1;
}

/* ================================================================
 * ioctl helpers
 * ================================================================ */
int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid, unsigned long soft, unsigned long hard)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0 ? -1 : 0;
}

/* ================================================================
 * Metadata helpers
 * ================================================================ */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static void prepend_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next       = ctx->containers;
    ctx->containers = rec;
}

/* ================================================================
 * Task 2 — Signal handlers
 *
 * SIGCHLD:
 *   Delivered by the kernel whenever a child process changes state.
 *   We use waitpid(-1, …, WNOHANG) in a loop to reap ALL exited
 *   children in one handler invocation (signals can be merged).
 *   SA_RESTART ensures interrupted syscalls (accept) auto-retry.
 *   SA_NOCLDSTOP means we only get SIGCHLD on exit, not on stop.
 *
 * Termination classification:
 *   WIFEXITED    → normal exit          → CONTAINER_EXITED
 *   stop_requested=1 + signal           → CONTAINER_STOPPED
 *   SIGKILL without stop_requested      → CONTAINER_HARD_LIMIT_KILLED
 *   any other signal                    → CONTAINER_KILLED
 *
 * SIGTERM/SIGINT:
 *   Set should_stop so the event loop breaks and shutdown begins.
 * ================================================================ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    /* Drain all exited children — signals are not queued */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);

        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->stop_requested) {
                        c->state = CONTAINER_STOPPED;
                    } else if (WTERMSIG(status) == SIGKILL) {
                        c->state = CONTAINER_HARD_LIMIT_KILLED;
                    } else {
                        c->state = CONTAINER_KILLED;
                    }
                }
                /* Unregister from kernel module */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, c->host_pid);
                break;
            }
            c = c->next;
        }

        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
    /* Interrupt accept() in the event loop */
}

/* ================================================================
 * Task 1 + 3 + 4 — launch_container
 *
 * Called by the supervisor when it receives a CMD_START or CMD_RUN.
 * Sequence:
 *   1. Reject duplicate container IDs.
 *   2. Create a pipe: child writes, supervisor reads.
 *   3. Build child_config_t on the heap.
 *   4. Allocate a 1 MiB stack for clone().
 *   5. clone() with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS.
 *   6. Close the write end in the supervisor.
 *   7. Allocate and populate a container_record_t.
 *   8. Register the host PID with the kernel module.
 *   9. Start a producer thread to read from the pipe.
 * ================================================================ */
static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp)
{
    /* 1. Reject duplicate IDs */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: container '%s' already exists", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* 2. Create pipe */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: pipe() failed: %s", strerror(errno));
        return -1;
    }

    /* 3. Build child config (heap-allocated; child reads before exec) */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: out of memory");
        return -1;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* 4. Allocate clone stack (stacks grow downward → pass top) */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: out of memory");
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    /* 5. clone() — creates the container process
     *   CLONE_NEWPID  → child is PID 1 in its own PID namespace
     *   CLONE_NEWUTS  → child has its own hostname
     *   CLONE_NEWNS   → child has its own mount namespace
     *   SIGCHLD       → send SIGCHLD to supervisor on child exit
     */
    pid_t pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (pid < 0) {
        free(stack); free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "ERROR: clone() failed: %s", strerror(errno));
        return -1;
    }

    /* 6. Supervisor closes write end
     *    If we keep it open the pipe never reaches EOF and the
     *    producer thread will block forever after the container exits. */
    close(pipefd[1]);

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* 7. Build and register container metadata */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        /* Container is running but we can't track it — kill it */
        kill(pid, SIGKILL);
        free(stack); free(cfg);
        close(pipefd[0]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "ERROR: out of memory");
        return -1;
    }
    strncpy(rec->id,      req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code        = 0;
    rec->exit_signal      = 0;
    rec->stop_requested   = 0;
    rec->pipe_read_fd     = pipefd[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    prepend_container(ctx, rec);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* 8. Register with kernel memory monitor */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                                   req->soft_limit_bytes, req->hard_limit_bytes) < 0)
            fprintf(stderr, "[supervisor] Warning: ioctl REGISTER failed for '%s'\n",
                    req->container_id);
        else
            fprintf(stderr, "[supervisor] Registered '%s' (pid=%d) with monitor\n",
                    req->container_id, pid);
    }

    /* 9. Start producer thread for this container's log output */
    producer_args_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->ctx         = ctx;
        pa->pipe_read_fd = pipefd[0];
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pa->container_id[CONTAINER_ID_LEN - 1] = '\0';

        pthread_t tid;
        if (pthread_create(&tid, NULL, producer_thread, pa) == 0)
            pthread_detach(tid);   /* producer joins itself when pipe closes */
        else {
            free(pa);
            close(pipefd[0]);
        }
    }

    free(stack);   /* stack is copied by clone(); safe to free now */
    free(cfg);     /* child has already exec'd or read config */

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "started '%s' pid=%d", req->container_id, pid);
    return 0;
}

/* ================================================================
 * ps output builder
 * ================================================================ */
static void build_ps_output(supervisor_ctx_t *ctx, char *buf, size_t sz)
{
    size_t off = 0;

    pthread_mutex_lock(&ctx->metadata_lock);

#define APPEND(...) do { \
    int _n = snprintf(buf + off, sz - off, __VA_ARGS__); \
    if (_n > 0) off += (size_t)_n; \
} while(0)

    APPEND("%-16s %-8s %-20s %-20s %-12s %-12s\n",
           "ID", "PID", "STARTED", "STATE", "SOFT(MiB)", "HARD(MiB)");
    APPEND("%-16s %-8s %-20s %-20s %-12s %-12s\n",
           "----------------", "--------",
           "--------------------", "--------------------",
           "------------", "------------");

    container_record_t *c = ctx->containers;
    int count = 0;
    while (c && off < sz - 1) {
        char tbuf[32];
        struct tm *tm_info = localtime(&c->started_at);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);

        APPEND("%-16s %-8d %-20s %-20s %-12lu %-12lu",
               c->id, c->host_pid, tbuf, state_to_string(c->state),
               c->soft_limit_bytes >> 20,
               c->hard_limit_bytes >> 20);

        /* Show exit info when available */
        if (c->state == CONTAINER_EXITED)
            APPEND("  [exit=%d]", c->exit_code);
        else if (c->state == CONTAINER_STOPPED ||
                 c->state == CONTAINER_KILLED   ||
                 c->state == CONTAINER_HARD_LIMIT_KILLED)
            APPEND("  [signal=%d]", c->exit_signal);

        APPEND("\n");
        c = c->next;
        count++;
    }
    if (count == 0)
        APPEND("(no containers)\n");

    pthread_mutex_unlock(&ctx->metadata_lock);
}

/* ================================================================
 * handle_client — processes one CLI connection in the supervisor
 * ================================================================ */
static void handle_client(supervisor_ctx_t *ctx, int cfd)
{
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    /* Read the fixed-size request */
    ssize_t n = read(cfd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "ERROR: bad request");
        SEND(cfd, &resp, sizeof(resp));
        close(cfd);
        return;
    }

    switch (req.kind) {

    /* ---- start ---- */
    case CMD_START:
        launch_container(ctx, &req, &resp);
        SEND(cfd, &resp, sizeof(resp));
        break;

    /* ---- run ---- */
    case CMD_RUN:
        launch_container(ctx, &req, &resp);
        SEND(cfd, &resp, sizeof(resp));   /* send "started" ack first */

        if (resp.status == 0) {
            /* Poll until the container exits, then send final status */
            while (1) {
                usleep(100000);   /* 100 ms poll interval */
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *c = find_container(ctx, req.container_id);
                int done = c &&
                    (c->state == CONTAINER_EXITED            ||
                     c->state == CONTAINER_STOPPED           ||
                     c->state == CONTAINER_KILLED            ||
                     c->state == CONTAINER_HARD_LIMIT_KILLED);
                if (done) {
                    resp.status = c->exit_code;
                    snprintf(resp.message, sizeof(resp.message),
                             "container '%s' finished state=%s exit_code=%d",
                             c->id, state_to_string(c->state), c->exit_code);
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                if (done) break;
            }
            SEND(cfd, &resp, sizeof(resp));
        }
        break;

    /* ---- stop ---- */
    case CMD_STOP: {
        pid_t stop_pid = -1;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found", req.container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            SEND(cfd, &resp, sizeof(resp));
            break;
        } else if (c->state != CONTAINER_RUNNING &&
                   c->state != CONTAINER_STARTING) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' is already in state '%s'",
                     req.container_id, state_to_string(c->state));
            pthread_mutex_unlock(&ctx->metadata_lock);
            SEND(cfd, &resp, sizeof(resp));
            break;
        } else {
            /*
             * Set stop_requested BEFORE sending any signal.
             * The SIGCHLD handler checks this flag to classify the
             * termination as CONTAINER_STOPPED (not HARD_LIMIT_KILLED).
             */
            c->stop_requested = 1;
            stop_pid = c->host_pid;
            kill(stop_pid, SIGTERM);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "stopping '%s' (pid=%d) — SIGTERM sent, SIGKILL in 2s if needed",
                     req.container_id, stop_pid);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        SEND(cfd, &resp, sizeof(resp));

        /*
         * Wait up to 2 seconds for the container to exit gracefully.
         * If it has not exited (e.g. /bin/sh ignores SIGTERM), send SIGKILL.
         * We check the state every 100ms to avoid sleeping the full 2s
         * when the process exits quickly.
         */
        if (stop_pid > 0) {
            int waited_ms = 0;
            while (waited_ms < 2000) {
                usleep(100000);   /* 100ms */
                waited_ms += 100;
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *c2 = find_container(ctx, req.container_id);
                int still_running = c2 &&
                    (c2->state == CONTAINER_RUNNING ||
                     c2->state == CONTAINER_STARTING);
                pthread_mutex_unlock(&ctx->metadata_lock);
                if (!still_running)
                    break;   /* exited cleanly, no need for SIGKILL */
            }

            /* If still running after 2s, force kill */
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c3 = find_container(ctx, req.container_id);
            if (c3 && (c3->state == CONTAINER_RUNNING ||
                       c3->state == CONTAINER_STARTING)) {
                kill(c3->host_pid, SIGKILL);
                fprintf(stderr,
                        "[supervisor] '%s' did not exit after SIGTERM, sent SIGKILL\n",
                        req.container_id);
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
        }
        break;
    }

    /* ---- ps ---- */
    case CMD_PS: {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "=== container list ===");
        SEND(cfd, &resp, sizeof(resp));
        /* Stream the ps table as raw bytes after the fixed-size header */
        char psbuf[8192];
        build_ps_output(ctx, psbuf, sizeof(psbuf));
        SEND(cfd, psbuf, strlen(psbuf));
        break;
    }

    /* ---- logs ---- */
    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        char log_path[PATH_MAX] = {0};
        if (c) strncpy(log_path, c->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!c) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found", req.container_id);
            SEND(cfd, &resp, sizeof(resp));
            break;
        }

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "=== logs for '%s' ===", req.container_id);
        SEND(cfd, &resp, sizeof(resp));

        /* Stream the actual log file to the client */
        int lfd = open(log_path, O_RDONLY);
        if (lfd >= 0) {
            char lbuf[4096];
            ssize_t lr;
            while ((lr = read(lfd, lbuf, sizeof(lbuf))) > 0)
                SEND(cfd, lbuf, (size_t)lr);
            close(lfd);
        } else {
            const char *empty = "(log file empty or not yet created)\n";
            SEND(cfd, empty, strlen(empty));
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "ERROR: unknown command");
        SEND(cfd, &resp, sizeof(resp));
        break;
    }

    close(cfd);
}

/* ================================================================
 * Task 2 + 1 + 3 + 6 — run_supervisor
 *
 * The long-running daemon.  Steps:
 *   1. Install signal handlers.
 *   2. Open /dev/container_monitor (Task 4).
 *   3. Create and bind the UNIX domain socket (Task 2 Path B IPC).
 *   4. Start the logging consumer thread (Task 3).
 *   5. Event loop: accept() → handle_client().
 *   6. Orderly shutdown: stop containers, drain log buffer,
 *      join logger thread, free resources (Task 6).
 * ================================================================ */
static int run_supervisor(const char *rootfs)
{
    (void)rootfs;   /* per-container rootfs is supplied at start time */

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* -- Init mutex and bounded buffer -- */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Step 1: Signal handlers */
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = 0;   /* do NOT restart accept() so the loop can exit */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Step 2: Open kernel monitor device */
    ctx.monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s (%s) — "
                        "memory limits disabled\n",
                MONITOR_DEVICE, strerror(errno));
    else
        fprintf(stderr, "[supervisor] Kernel monitor device opened.\n");

    /* Step 3: UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);   /* remove stale socket from previous run */
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }
    fprintf(stderr, "[supervisor] Control socket: %s\n", CONTROL_PATH);

    /* Step 4: Start logging consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        close(ctx.server_fd);
        return 1;
    }

    mkdir(LOG_DIR, 0755);
    fprintf(stderr, "[supervisor] Ready. Log dir: %s\n", LOG_DIR);

    /* Step 5: Event loop */
    while (!ctx.should_stop) {
        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;   /* interrupted by signal — retry */
            if (!ctx.should_stop)
                perror("accept");
            break;
        }
        handle_client(&ctx, cfd);
    }

    /* Step 6: Orderly shutdown */
    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Send SIGTERM to every still-running container */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers 2 seconds to exit gracefully */
    sleep(2);

    /* Force-kill any that are still running */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING)
            kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Signal logging pipeline to shut down, then wait for consumer to flush */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    fprintf(stderr, "[supervisor] Logger thread joined (all log data flushed).\n");

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Release kernel + socket resources */
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean exit. No zombies.\n");
    return 0;
}

/* ================================================================
 * Task 2 — send_control_request  (CLI client side)
 *
 * Connects to the supervisor's UNIX socket, sends the binary
 * control_request_t, reads back the control_response_t, and
 * prints the message.
 *
 * For CMD_LOGS: also streams the raw log content that follows.
 * For CMD_RUN:  reads a second response after the "started" ack.
 * ================================================================ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                        "Is 'engine supervisor' running?\n", CONTROL_PATH);
        close(fd);
        return 1;
    }

    /* Send request */
    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        fprintf(stderr, "write request failed\n");
        close(fd);
        return 1;
    }

    /* Read first response */
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    if (read(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "read response failed\n");
        close(fd);
        return 1;
    }
    printf("%s\n", resp.message);

    if (req->kind == CMD_LOGS || req->kind == CMD_PS) {
        /* Stream raw bytes that follow the fixed-size response header */
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)n, stdout);

    } else if (req->kind == CMD_RUN && resp.status == 0) {
        /* Block until the container finishes (second response) */
        memset(&resp, 0, sizeof(resp));
        if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp))
            printf("%s\n", resp.message);
    }

    close(fd);
    return resp.status == 0 ? 0 : 1;
}

/* ================================================================
 * CLI command dispatchers
 * ================================================================ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
