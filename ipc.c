/*
 * ipc.c  —  Anwar Atawna
 * ----------------------
 * Implements all IPC primitives:
 *   - Shared memory (SHM) creation and lifecycle
 *   - Binary semaphore set (P/V operations with SEM_UNDO crash safety)
 *   - System-V message queue (non-blocking send, typed receive)
 *   - String-conversion helpers used by every process
 *
 * Key design decisions:
 *
 *   SEM_UNDO flag on all semop() calls:
 *     If a process crashes while holding the lock, the kernel
 *     automatically undoes the operation. Without this, every other
 *     process would block forever on sem_lock() — a classic deadlock.
 *
 *   EINTR retry on semop():
 *     A signal (e.g. SIGCHLD) can interrupt semop() with EINTR.
 *     We retry in that case so the lock/unlock completes correctly.
 *
 *   IPC_NOWAIT on msgsnd():
 *     We never block a sender. If the queue is full the message is
 *     dropped. For a simulation this is acceptable; in production you
 *     would use a bounded retry or a larger queue (msgctl MSGMNB).
 *
 *   stale IPC cleanup in ipc_init():
 *     ipc_init() removes any leftover segments from a previous run
 *     before creating fresh ones. This prevents "resource busy" errors
 *     during development when the previous run was killed hard.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <time.h>

#include "config.h"
#include "ipc.h"

/* ------------------------------------------------------------------ */
/* Module-level IPC handles (one copy per process after attach/init)   */
/* ------------------------------------------------------------------ */
static int         g_shmid = -1;
static int         g_semid = -1;
static int         g_msgid = -1;
static SharedData *g_shm   = NULL;

/* ------------------------------------------------------------------ */
/* semun: required argument union for semctl() on Linux.               */
/* POSIX leaves this user-defined; <sys/sem.h> does not define it.    */
/* ------------------------------------------------------------------ */
union semun {
    int            val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* ------------------------------------------------------------------ */
/* Internal helper: execute one semaphore operation with EINTR retry   */
/* ------------------------------------------------------------------ */
static int do_semop(int semnum, int op) {
    struct sembuf sb;
    sb.sem_num = (unsigned short)semnum;
    sb.sem_op  = (short)op;
    sb.sem_flg = SEM_UNDO;   /* auto-release on crash */

    while (semop(g_semid, &sb, 1) == -1) {
        if (errno == EINTR) continue;   /* signal interrupted — retry */
        perror("[IPC] semop");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/* Shared memory + semaphore lifecycle                                  */
/* ================================================================== */

SharedData *ipc_init(void) {
    /* --- Remove stale objects from a previous run --- */
    int old;
    if ((old = shmget(SHM_KEY, 0, 0666)) != -1) shmctl(old, IPC_RMID, NULL);
    if ((old = semget(SEM_KEY, 0, 0666)) != -1) semctl(old, 0, IPC_RMID);
    if ((old = msgget(MSG_KEY, 0666))    != -1) msgctl(old, IPC_RMID, NULL);

    /* --- Create shared memory --- */
    g_shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    if (g_shmid == -1) { perror("[IPC] shmget"); return NULL; }

    g_shm = (SharedData *)shmat(g_shmid, NULL, 0);
    if (g_shm == (void *)-1) {
        perror("[IPC] shmat");
        shmctl(g_shmid, IPC_RMID, NULL);
        return NULL;
    }
    memset(g_shm, 0, sizeof(SharedData));

    /* --- Create semaphore set with NUM_SEMS semaphores --- */
    g_semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | 0666);
    if (g_semid == -1) {
        perror("[IPC] semget");
        shmdt(g_shm); shmctl(g_shmid, IPC_RMID, NULL);
        return NULL;
    }

    /* Initialize all semaphores to 1 (unlocked) */
    union semun arg;
    arg.val = 1;
    for (int i = 0; i < NUM_SEMS; i++) {
        if (semctl(g_semid, i, SETVAL, arg) == -1) {
            perror("[IPC] semctl SETVAL");
            shmdt(g_shm); shmctl(g_shmid, IPC_RMID, NULL);
            semctl(g_semid, 0, IPC_RMID);
            return NULL;
        }
    }

    /* --- Create message queue --- */
    g_msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (g_msgid == -1) {
        perror("[IPC] msgget");
        shmdt(g_shm); shmctl(g_shmid, IPC_RMID, NULL);
        semctl(g_semid, 0, IPC_RMID);
        return NULL;
    }

    printf("[IPC] initialized  shmid=%-4d  semid=%-4d  msgid=%d\n",
           g_shmid, g_semid, g_msgid);
    return g_shm;
}

SharedData *ipc_attach(void) {
    /* Attach to EXISTING SHM (created by ipc_init in the master process) */
    g_shmid = shmget(SHM_KEY, sizeof(SharedData), 0666);
    if (g_shmid == -1) { perror("[IPC] shmget (attach)"); return NULL; }

    g_shm = (SharedData *)shmat(g_shmid, NULL, 0);
    if (g_shm == (void *)-1) { perror("[IPC] shmat (attach)"); return NULL; }

    g_semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (g_semid == -1) {
        perror("[IPC] semget (attach)");
        shmdt(g_shm); return NULL;
    }

    /* Message queue is optional for read-only observer processes */
    g_msgid = msgget(MSG_KEY, 0666);
    if (g_msgid == -1) {
        fprintf(stderr, "[IPC] warning: message queue not found"
                        " (read-only mode)\n");
    }

    return g_shm;
}

void ipc_detach(SharedData *shm) {
    if (shm) shmdt(shm);
    g_shm = NULL;
}

void ipc_destroy(void) {
    if (g_shm)          { shmdt(g_shm);                   g_shm  = NULL; }
    if (g_shmid != -1)  { shmctl(g_shmid, IPC_RMID, NULL); g_shmid = -1; }
    if (g_semid != -1)  { semctl(g_semid, 0, IPC_RMID);    g_semid = -1; }
    if (g_msgid != -1)  { msgctl(g_msgid, IPC_RMID, NULL);  g_msgid = -1; }
    printf("[IPC] all IPC objects destroyed.\n");
}

/* ================================================================== */
/* Semaphore operations                                                 */
/* ================================================================== */

/* P operation on SEM_MAIN: decrement (block if 0) */
void sem_lock(void)   { do_semop(SEM_MAIN, -1); }

/* V operation on SEM_MAIN: increment (wake waiters) */
void sem_unlock(void) { do_semop(SEM_MAIN, +1); }

/* ================================================================== */
/* Message queue operations                                             */
/* ================================================================== */

int msg_send(const Message *msg) {
    if (g_msgid == -1) {
        g_msgid = msgget(MSG_KEY, 0666);
        if (g_msgid == -1) return -1;
    }
    size_t sz = sizeof(Message) - sizeof(long);
    if (msgsnd(g_msgid, msg, sz, IPC_NOWAIT) == -1) {
        /* EAGAIN = queue full; drop silently (simulation tolerance) */
        if (errno != EAGAIN) perror("[IPC] msgsnd");
        return -1;
    }
    return 0;
}

int msg_recv(Message *msg, long mtype, int block) {
    if (g_msgid == -1) {
        g_msgid = msgget(MSG_KEY, 0666);
        if (g_msgid == -1) return -1;
    }
    int flags = block ? 0 : IPC_NOWAIT;
    ssize_t n = msgrcv(g_msgid, msg, sizeof(Message) - sizeof(long),
                       mtype, flags);
    if (n == -1) return -1;
    return (int)n;
}

void msg_destroy(void) {
    if (g_msgid != -1) { msgctl(g_msgid, IPC_RMID, NULL); g_msgid = -1; }
}

/* ================================================================== */
/* Utility helpers                                                      */
/* ================================================================== */

const char *light_str(int state) {
    switch (state) {
    case RED:    return "RED";
    case YELLOW: return "YELLOW";
    case GREEN:  return "GREEN";
    default:     return "UNKNOWN";
    }
}

const char *dir_str(int dir) {
    switch (dir) {
    case NORTH: return "NORTH";
    case SOUTH: return "SOUTH";
    case EAST:  return "EAST";
    case WEST:  return "WEST";
    default:    return "UNKNOWN";
    }
}

const char *phase_str(int phase) {
    switch (phase) {
    case PHASE_NS_GREEN:       return "N-S GREEN";
    case PHASE_NS_YELLOW:      return "N-S YELLOW";
    case PHASE_ALL_RED_1:      return "ALL-RED (1)";
    case PHASE_EW_GREEN:       return "E-W GREEN";
    case PHASE_EW_YELLOW:      return "E-W YELLOW";
    case PHASE_ALL_RED_2:      return "ALL-RED (2)";
    case PHASE_PEDESTRIAN:     return "PEDESTRIAN";
    case PHASE_EMERGENCY:      return "EMERGENCY";
    case PHASE_NS_LEFT_GREEN:  return "N-S LEFT-GREEN";
    case PHASE_NS_LEFT_YELLOW: return "N-S LEFT-YLW";
    case PHASE_EW_LEFT_GREEN:  return "E-W LEFT-GREEN";
    case PHASE_EW_LEFT_YELLOW: return "E-W LEFT-YLW";
    case PHASE_ALL_RED_3:      return "ALL-RED (3)";
    case PHASE_ALL_RED_4:      return "ALL-RED (4)";
    default:                   return "UNKNOWN";
    }
}

void build_cmd_message(Message *m, int direction, int new_state, int left_state) {
    memset(m, 0, sizeof(Message));
    m->mtype      = MSG_CMD_FOR(direction);
    m->source     = SRC_CONTROLLER;
    m->direction  = direction;
    m->value      = new_state;
    m->left_value = left_state;
    m->timestamp  = time(NULL);
    snprintf(m->message, sizeof(m->message), "CMD: %s straight=%s left=%s",
             dir_str(direction), light_str(new_state), light_str(left_state));
}
