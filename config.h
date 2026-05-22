/*
 * config.h  —  Anwar Atawna
 * -------------------------
 * System-wide constants, IPC keys, message types, and timing parameters.
 * Every process in the system includes this header.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ------------------------------------------------------------------ */
/* Light states                                                         */
/* ------------------------------------------------------------------ */
#define RED    0
#define YELLOW 1
#define GREEN  2

/* ------------------------------------------------------------------ */
/* Directions                                                           */
/* ------------------------------------------------------------------ */
#define NORTH          0
#define SOUTH          1
#define EAST           2
#define WEST           3
#define NUM_DIRECTIONS 4

/* ------------------------------------------------------------------ */
/* IPC keys  (must be unique on the host)                              */
/* ------------------------------------------------------------------ */
#define SHM_KEY  0x1234   /* shared memory segment                    */
#define SEM_KEY  0x5678   /* semaphore set                            */
#define MSG_KEY  0x9ABC   /* POSIX System-V message queue             */

/* ------------------------------------------------------------------ */
/* Semaphore indices inside the set                                    */
/* ------------------------------------------------------------------ */
#define NUM_SEMS  2
#define SEM_MAIN  0   /* guards all SHM read-modify-writes            */
#define SEM_LOG   1   /* guards log-file writes                       */

/* ------------------------------------------------------------------ */
/* Traffic phases                                                       */
/*                                                                      */
/* Full cycle (left-turn protected first):                             */
/*   NS_LEFT_GREEN → NS_LEFT_YELLOW → ALL_RED_3 →                     */
/*   NS_GREEN      → NS_YELLOW      → ALL_RED_1 →                     */
/*   EW_LEFT_GREEN → EW_LEFT_YELLOW → ALL_RED_4 →                     */
/*   EW_GREEN      → EW_YELLOW      → ALL_RED_2 → (repeat)            */
/* Special: PEDESTRIAN, EMERGENCY (interrupt any green phase)          */
/* ------------------------------------------------------------------ */
#define PHASE_NS_GREEN        0
#define PHASE_NS_YELLOW       1
#define PHASE_ALL_RED_1       2   /* after NS straight, before EW left  */
#define PHASE_EW_GREEN        3
#define PHASE_EW_YELLOW       4
#define PHASE_ALL_RED_2       5   /* after EW straight, before NS left  */
#define PHASE_PEDESTRIAN      6
#define PHASE_EMERGENCY       7
#define PHASE_NS_LEFT_GREEN   8   /* N+S left-turn arrows green         */
#define PHASE_NS_LEFT_YELLOW  9
#define PHASE_EW_LEFT_GREEN  10   /* E+W left-turn arrows green         */
#define PHASE_EW_LEFT_YELLOW 11
#define PHASE_ALL_RED_3      12   /* after NS left, before NS straight  */
#define PHASE_ALL_RED_4      13   /* after EW left, before EW straight  */
#define NUM_NORMAL_PHASES     6

/* ------------------------------------------------------------------ */
/* Timing (seconds)                                                     */
/* ------------------------------------------------------------------ */
#define GREEN_DURATION        10   /* default straight green length    */
#define LEFT_GREEN_DURATION    8   /* fixed left-turn arrow hold time  */
#define YELLOW_DURATION        3   /* mandatory yellow before red      */
#define ALL_RED_DURATION       2   /* all-red safety gap               */
#define PEDESTRIAN_DURATION   15   /* walk signal duration             */
#define EMERGENCY_DURATION    20   /* emergency green hold             */
#define MIN_GREEN_DURATION     5   /* adaptive timing lower bound      */
#define MAX_GREEN_DURATION    20   /* adaptive timing upper bound      */

/* ------------------------------------------------------------------ */
/* Message types for the System-V message queue                        */
/*                                                                      */
/* Per-direction command types (controller → traffic_light):           */
/*   MSG_CMD_FOR(NORTH)=11, SOUTH=12, EAST=13, WEST=14                */
/* ------------------------------------------------------------------ */
#define MSG_CMD_NORTH   11
#define MSG_CMD_SOUTH   12
#define MSG_CMD_EAST    13
#define MSG_CMD_WEST    14
#define MSG_CMD_FOR(d)  (MSG_CMD_NORTH + (d))   /* inline macro       */

#define MSG_VEHICLE     2   /* vehicle arrival / departure             */
#define MSG_PEDESTRIAN  3   /* pedestrian crossing request             */
#define MSG_EMERGENCY   4   /* emergency vehicle detected              */
#define MSG_LOG         5   /* log entry destined for logger process   */
#define MSG_CONFIRM     6   /* traffic_light acknowledges command      */
#define MSG_SHUTDOWN    7   /* system-wide shutdown notification       */

/* ------------------------------------------------------------------ */
/* Source identifiers (field Message.source)                           */
/* ------------------------------------------------------------------ */
#define SRC_CONTROLLER    10
#define SRC_VEHICLE_DET   11
#define SRC_PEDESTRIAN    12
#define SRC_EMERGENCY     13
#define SRC_LOGGER        14
#define SRC_SAFETY        15
#define SRC_TRAFFIC_LIGHT 20   /* +direction offset: 20-23            */

/* ------------------------------------------------------------------ */
/* Adaptive green-time parameters                                       */
/* ------------------------------------------------------------------ */
#define GREEN_TIME_PER_VEHICLE  1   /* extra seconds added per queued vehicle */
#define MAX_VEHICLES_PER_DIR   10   /* simulation cap                         */

/* ------------------------------------------------------------------ */
/* Miscellaneous                                                        */
/* ------------------------------------------------------------------ */
#define LOG_FILE "log.txt"

#endif /* CONFIG_H */
