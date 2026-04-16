/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    char args[4][CHILD_COMMAND_LEN];
    int argc;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    char args[4][CHILD_COMMAND_LEN];
    int argc;
    int nice_value;
    int log_write_fd;
} child_config_t;
typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Passed to the pipe-reader thread for each container */
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

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
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}
/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}
/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];
    int fd;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;
        if (write(fd, item.data, item.length) < 0) { /* best effort */ }
        close(fd);
    }

    return NULL;
}

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *pra = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

        n = read(pra->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break;

        item.length = (size_t)n;
        bounded_buffer_push(pra->log_buffer, &item);
    }

    close(pra->read_fd);
    free(pra);
    return NULL;
}


/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc inside the container's mount namespace, after chroot */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    if (cfg->nice_value != 0)
        if (nice(cfg->nice_value) == -1) { /* best effort */ }

    char *argv[6] = { cfg->command, NULL, NULL, NULL, NULL, NULL };
    int i;
    for (i = 0; i < cfg->argc && i < 4; i++)
        argv[i + 1] = cfg->args[i];
    argv[cfg->argc + 1] = NULL;
    execv(cfg->command, argv);
    perror("execv");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}
/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static volatile sig_atomic_t g_should_stop = 0;
static supervisor_ctx_t *g_ctx = NULL;

static void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    (void)sig;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx)
            continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = c->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->stop_requested)
                        c->state = CONTAINER_STOPPED;
                    else if (c->exit_signal == SIGKILL)
                        c->state = CONTAINER_KILLED;
                    else
                        c->state = CONTAINER_EXITED;
                }
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
    g_should_stop = 1;
    if (g_ctx && g_ctx->server_fd >= 0)
        shutdown(g_ctx->server_fd, SHUT_RDWR);
}

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open /dev/container_monitor (kernel module not loaded?)");

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    listen(ctx.server_fd, 8);

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    pthread_t log_thread;
    pthread_create(&log_thread, NULL, logging_thread, &ctx);

    mkdir(LOG_DIR, 0755);

    while (!g_should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF || errno == EINVAL)
                break;
            continue;
        }

        control_request_t req = {0};
        control_response_t resp = {0};

        if (read(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        if (req.kind == CMD_START) {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "pipe failed");
                if (write(client_fd, &resp, sizeof(resp)) < 0) {}
                close(client_fd);
                continue;
            }

            char *stack = malloc(STACK_SIZE);
            if (!stack) {
                close(pipefd[0]); close(pipefd[1]);
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "malloc failed");
                if (write(client_fd, &resp, sizeof(resp)) < 0) {}
                close(client_fd);
                continue;
            }

            child_config_t cfg = {0};
            strncpy(cfg.id, req.container_id, sizeof(cfg.id) - 1);
            strncpy(cfg.rootfs, req.rootfs, sizeof(cfg.rootfs) - 1);
            strncpy(cfg.command, req.command, sizeof(cfg.command) - 1);
            cfg.nice_value   = req.nice_value;
            cfg.log_write_fd = pipefd[1];
            cfg.argc = 0;
            cfg.argc = req.argc;
            int i;
            for (i = 0; i < req.argc && i < 4; i++)
                strncpy(cfg.args[i], req.args[i], CHILD_COMMAND_LEN - 1);
            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              &cfg);
            close(pipefd[1]);

            if (pid < 0) {
                perror("clone");
                free(stack);
                close(pipefd[0]);
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "failed to start container");
            } else {
                container_record_t *rec = calloc(1, sizeof(*rec));
                strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
                rec->host_pid         = pid;
                rec->started_at       = time(NULL);
                rec->state            = CONTAINER_RUNNING;
                rec->soft_limit_bytes = req.soft_limit_bytes;
                rec->hard_limit_bytes = req.hard_limit_bytes;
                snprintf(rec->log_path, sizeof(rec->log_path),
                         "%s/%s.log", LOG_DIR, req.container_id);

                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next      = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);

                if (ctx.monitor_fd >= 0)
                    register_with_monitor(ctx.monitor_fd, req.container_id,
                                          pid, req.soft_limit_bytes,
                                          req.hard_limit_bytes);

                pipe_reader_args_t *pra = malloc(sizeof(*pra));
                if (pra) {
                    pra->read_fd    = pipefd[0];
                    pra->log_buffer = &ctx.log_buffer;
                    strncpy(pra->container_id, req.container_id, CONTAINER_ID_LEN - 1);
                    pthread_t reader;
                    pthread_create(&reader, NULL, pipe_reader_thread, pra);
                    pthread_detach(reader);
                } else {
                    close(pipefd[0]);
                }

                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "started %s pid=%d", req.container_id, pid);
            }

        } else if (req.kind == CMD_RUN) {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "pipe failed");
                if (write(client_fd, &resp, sizeof(resp)) < 0) {}
                close(client_fd);
                continue;
            }

            char *stack = malloc(STACK_SIZE);
            if (!stack) {
                close(pipefd[0]); close(pipefd[1]);
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "malloc failed");
                if (write(client_fd, &resp, sizeof(resp)) < 0) {}
                close(client_fd);
                continue;
            }

            child_config_t cfg = {0};
            strncpy(cfg.id, req.container_id, sizeof(cfg.id) - 1);
            strncpy(cfg.rootfs, req.rootfs, sizeof(cfg.rootfs) - 1);
            strncpy(cfg.command, req.command, sizeof(cfg.command) - 1);
            cfg.nice_value   = req.nice_value;
            cfg.log_write_fd = pipefd[1];
            cfg.argc = 0;
            cfg.argc = req.argc;
            int i;
            for (i = 0; i < req.argc && i < 4; i++)
                strncpy(cfg.args[i], req.args[i], CHILD_COMMAND_LEN - 1);
            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              &cfg);
            close(pipefd[1]);

            if (pid < 0) {
                perror("clone");
                free(stack);
                close(pipefd[0]);
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "failed to start container");
                if (write(client_fd, &resp, sizeof(resp)) < 0) {}
                close(client_fd);
                continue;
            }

            container_record_t *rec = calloc(1, sizeof(*rec));
            strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
            rec->host_pid         = pid;
            rec->started_at       = time(NULL);
            rec->state            = CONTAINER_RUNNING;
            rec->soft_limit_bytes = req.soft_limit_bytes;
            rec->hard_limit_bytes = req.hard_limit_bytes;
            snprintf(rec->log_path, sizeof(rec->log_path),
                     "%s/%s.log", LOG_DIR, req.container_id);

            pthread_mutex_lock(&ctx.metadata_lock);
            rec->next      = ctx.containers;
            ctx.containers = rec;
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (ctx.monitor_fd >= 0)
                register_with_monitor(ctx.monitor_fd, req.container_id,
                                      pid, req.soft_limit_bytes,
                                      req.hard_limit_bytes);

            pipe_reader_args_t *pra = malloc(sizeof(*pra));
            if (pra) {
                pra->read_fd    = pipefd[0];
                pra->log_buffer = &ctx.log_buffer;
                strncpy(pra->container_id, req.container_id, CONTAINER_ID_LEN - 1);
                pthread_t reader;
                pthread_create(&reader, NULL, pipe_reader_thread, pra);
                pthread_detach(reader);
            } else {
                close(pipefd[0]);
            }

            int wstatus;
            waitpid(pid, &wstatus, 0);

            if (WIFEXITED(wstatus)) {
                resp.status = WEXITSTATUS(wstatus);
                snprintf(resp.message, sizeof(resp.message),
                         "exited with code %d", resp.status);
            } else if (WIFSIGNALED(wstatus)) {
                resp.status = 128 + WTERMSIG(wstatus);
                snprintf(resp.message, sizeof(resp.message),
                         "killed by signal %d", WTERMSIG(wstatus));
            }

        } else if (req.kind == CMD_PS) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            int offset = 0;
            while (c && offset < (int)sizeof(resp.message) - 1) {
                offset += snprintf(resp.message + offset,
                                   sizeof(resp.message) - offset,
                                   "%-16s %-10s pid=%-6d started=%ld\n",
                                   c->id,
                                   state_to_string(c->state),
                                   c->host_pid,
                                   (long)c->started_at);
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            if (offset == 0)
                snprintf(resp.message, sizeof(resp.message), "no containers running");
            resp.status = 0;

        } else if (req.kind == CMD_LOGS) {
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log",
                     LOG_DIR, req.container_id);

            int lfd = open(log_path, O_RDONLY);
            if (lfd < 0) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message),
                         "no logs for %s", req.container_id);
            } else {
                ssize_t n = read(lfd, resp.message, sizeof(resp.message) - 1);
                close(lfd);
                if (n < 0) n = 0;
                resp.message[n] = '\0';
                resp.status = 0;
            }

        } else if (req.kind == CMD_STOP) {
            int found = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0) {
                    c->stop_requested = 1;
                    kill(c->host_pid, SIGTERM);
                    if (ctx.monitor_fd >= 0)
                        unregister_from_monitor(ctx.monitor_fd, c->id, c->host_pid);
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message),
                             "stopped %s", req.container_id);
                    found = 1;
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            if (!found) {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message),
                         "container not found: %s", req.container_id);
            }

        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "ok");
        }

        if (write(client_fd, &resp, sizeof(resp)) < 0) {}
        close(client_fd);
    }
    close(ctx.server_fd);
    ctx.server_fd = -1;
    unlink(CONTROL_PATH);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(log_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}
/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    control_response_t resp = {0};
    if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp))
        fprintf(stdout, "%s\n", resp.message);

    close(fd);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    int i;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [args...] [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.argc = 0;

    int flag_start = 5;
    for (i = 5; i < argc && argv[i][0] != '-'; i++) {
        if (req.argc < 4) {
            strncpy(req.args[req.argc], argv[i], CHILD_COMMAND_LEN - 1);
            req.argc++;
        }
        flag_start = i + 1;
    }

    if (parse_optional_flags(&req, argc, argv, flag_start) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    int i;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [args...] [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.argc = 0;

    int flag_start = 5;
    for (i = 5; i < argc && argv[i][0] != '-'; i++) {
        if (req.argc < 4) {
            strncpy(req.args[req.argc], argv[i], CHILD_COMMAND_LEN - 1);
            req.argc++;
        }
        flag_start = i + 1;
    }

    if (parse_optional_flags(&req, argc, argv, flag_start) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{

    control_request_t req;
    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

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