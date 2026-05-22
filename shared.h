/*
 * shared.h 
 * -------------------------
 * Defines SharedData — the single struct that lives in shared memory.
 * Every process attaches to the same SHM segment and reads/writes
 * this struct, always under sem_lock() / sem_unlock().
 *
 * Layout rationale:
 *   - light[]          : authoritative light states (controller writes)
 *   - vehicle_count[]  : updated by vehicle_detector, read by controller
 *   - pedestrian_*     : set by pedestrian process, cleared by controller
 *   - emergency_*      : set by emergency process, cleared by controller
 *   - safety_*         : written by safety_monitor (read-only for others)
 *   - running/shutdown : lifecycle flags managed by main process
 */

#ifndef SHARED_H
#define SHARED_H

#include "config.h"
#include <time.h>

typedef struct {

    /* --- Traffic light states (RED / YELLOW / GREEN per direction) --- */
    int light[NUM_DIRECTIONS];       /* straight + right signal          */
    int left_light[NUM_DIRECTIONS];  /* left-turn arrow signal           */

    /* --- Current traffic phase (PHASE_* constant) --- */
    int current_phase;

    /* --- Seconds remaining in the current phase (informational) --- */
    int phase_time_remaining;

    /* --- Vehicle queues per direction --- */
    int vehicle_count[NUM_DIRECTIONS];

    /* --- Pedestrian crossing state --- */
    int pedestrian_request;    /* 1 = request pending, cleared by controller  */
    int pedestrian_active;     /* 1 = walk signal currently active            */
    int pedestrian_direction;  /* which direction crossing was requested (0-3)*/

    /* --- Emergency vehicle state --- */
    int emergency_mode;       /* 1 = system in emergency mode                */
    int emergency_direction;  /* which direction has emergency priority       */

    /* --- Safety monitor output --- */
    int  safety_violation;    /* 1 = violation currently active              */
    char safety_msg[128];     /* human-readable violation description         */

    /* --- Timestamps --- */
    time_t phase_start_time;  /* when the current phase began                */
    time_t last_update;       /* last time SHM was modified                  */

    /* --- Lifecycle --- */
    int controller_pid;       /* PID of controller process (for signalling)  */
    int running;              /* 1 = system is operational                   */
    int shutdown;             /* 1 = all processes must exit                 */

} SharedData;

#endif /* SHARED_H */
