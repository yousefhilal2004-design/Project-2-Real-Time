/*
 * opengl_ui.c  
 * --------------------------------
 * Real-time OpenGL/GLUT visualization of the traffic light system.
 *
 * Layout (top-down view, right-hand traffic):
 *   Each approach has two lanes:
 *     Left lane  (closer to centre line) — left-turn-only cars
 *     Right lane (away from centre line) — straight / right-turn cars
 *
 *   Each approach has two signal heads:
 *     Main head   (3 circles R/Y/G)    — straight + right signal
 *     Arrow head  (3 circles R/Y/G<-)  — protected left-turn arrow
 *
 * Animation:
 *   g_anim[d]      — rolling offset for straight/right lane (driven by light[d])
 *   g_left_anim[d] — rolling offset for left-turn lane     (driven by left_light[d])
 *   Cars advance toward the stop line when GREEN, decelerate when RED/YELLOW.
 *
 * IPC: read-only SHM attach; snapshot taken under sem_lock() every timer tick.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <GL/glut.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

/* ------------------------------------------------------------------ */
/* Window / timing constants                                           */
/* ------------------------------------------------------------------ */
#define WIN_W   940
#define WIN_H   720
#define FPS      20

/* Road geometry — full road half-width (each side carries 2 lanes) */
#define ROAD_W   80.0f

/* Lane offsets from road centre (positive = away from centre line) */
#define LANE_THROUGH  (ROAD_W * 0.62f)  /* straight / right lane centre */
#define LANE_LEFT     (ROAD_W * 0.22f)  /* left-turn lane centre        */

/* ------------------------------------------------------------------ */
/* Car geometry                                                        */
/* ------------------------------------------------------------------ */
#define CAR_LEN      18.0f
#define CAR_WID      11.0f
#define CAR_GAP       5.0f
#define CAR_SPACING  (CAR_LEN + CAR_GAP)
#define CAR_SPEED    38.0f   /* px/s while moving */
#define LEFT_CARS     3      /* fixed left-turn queue depth per direction */

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static SharedData  g_snap;
static SharedData *g_shm  = NULL;
static int         g_tick = 0;

static float g_anim[NUM_DIRECTIONS]      = {0.0f, 0.0f, 0.0f, 0.0f};
static float g_left_anim[NUM_DIRECTIONS] = {0.0f, 0.0f, 0.0f, 0.0f};

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */
static void draw_circle(float cx, float cy, float r) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= 32; i++) {
        float a = (float)i * 2.0f * (float)M_PI / 32.0f;
        glVertex2f(cx + r * cosf(a), cy + r * sinf(a));
    }
    glEnd();
}

static void draw_rect(float x, float y, float hw, float hh) {
    glBegin(GL_QUADS);
    glVertex2f(x-hw, y-hh); glVertex2f(x+hw, y-hh);
    glVertex2f(x+hw, y+hh); glVertex2f(x-hw, y+hh);
    glEnd();
}

static void draw_rect_outline(float x, float y, float hw, float hh) {
    glBegin(GL_LINE_LOOP);
    glVertex2f(x-hw, y-hh); glVertex2f(x+hw, y-hh);
    glVertex2f(x+hw, y+hh); glVertex2f(x-hw, y+hh);
    glEnd();
}

static void draw_text(float x, float y, const char *s) {
    glRasterPos2f(x, y);
    for (; *s; s++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *s);
}
static void draw_text18(float x, float y, const char *s) {
    glRasterPos2f(x, y);
    for (; *s; s++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *s);
}

/* ------------------------------------------------------------------ */
/* Signal head colour helper                                           */
/* ------------------------------------------------------------------ */
static void set_bulb_color(int bulb_state, int active) {
    if (!active) { glColor3f(0.15f, 0.15f, 0.15f); return; }
    switch (bulb_state) {
    case RED:    glColor3f(1.0f, 0.05f, 0.05f); break;
    case YELLOW: glColor3f(1.0f, 0.90f, 0.00f); break;
    case GREEN:  glColor3f(0.0f, 0.90f, 0.20f); break;
    default:     glColor3f(0.3f, 0.30f, 0.30f); break;
    }
}

/* ------------------------------------------------------------------ */
/* draw_signal_head: vertical 3-bulb housing                          */
/* ------------------------------------------------------------------ */
static void draw_signal_head(float cx, float cy,
                              float hw, float hh, float sp, float r,
                              int st) {
    glColor3f(0.12f, 0.12f, 0.12f);
    draw_rect(cx, cy, hw, hh);
    glColor3f(0.25f, 0.25f, 0.25f);
    draw_rect(cx, cy - hh - 16.0f, 3.0f, 16.0f);   /* pole */

    set_bulb_color(RED,    st == RED);    draw_circle(cx, cy + sp, r);
    set_bulb_color(YELLOW, st == YELLOW); draw_circle(cx, cy,      r);
    set_bulb_color(GREEN,  st == GREEN);  draw_circle(cx, cy - sp, r);
}

/* ------------------------------------------------------------------ */
/* draw_traffic_light: main (larger) straight-signal head              */
/* ------------------------------------------------------------------ */
static void draw_traffic_light(float cx, float cy, int dir) {
    int st = g_snap.light[dir];
    draw_signal_head(cx, cy, 20.0f, 62.0f, 38.0f, 13.0f, st);

    glColor3f(1.0f, 1.0f, 1.0f);
    char buf[16];
    snprintf(buf, sizeof(buf), "%s", dir_str(dir));
    draw_text(cx - 12.0f, cy - 62.0f - 13.0f, buf);
    snprintf(buf, sizeof(buf), "Q:%d", g_snap.vehicle_count[dir]);
    glColor3f(1.0f, 1.0f, 0.3f);
    draw_text(cx - 10.0f, cy - 62.0f - 25.0f, buf);
}

/* ------------------------------------------------------------------ */
/* draw_left_signal: smaller left-turn arrow head                     */
/* ------------------------------------------------------------------ */
static void draw_left_signal(float cx, float cy, int dir) {
    int lst = g_snap.left_light[dir];
    draw_signal_head(cx, cy, 14.0f, 44.0f, 26.0f, 9.0f, lst);

    glColor3f(0.9f, 0.9f, 0.9f);
    draw_text(cx - 5.0f, cy - 44.0f - 10.0f, "<-");
}

/* ------------------------------------------------------------------ */
/* Car drawing (top-down)                                              */
/* ------------------------------------------------------------------ */
static void draw_car(float cx, float cy, int vert, int idx) {
    float bw = vert ? CAR_WID : CAR_LEN;
    float bh = vert ? CAR_LEN : CAR_WID;

    static const float palette[8][3] = {
        {0.85f,0.15f,0.15f}, {0.15f,0.40f,0.85f},
        {0.85f,0.70f,0.10f}, {0.85f,0.45f,0.10f},
        {0.30f,0.75f,0.35f}, {0.65f,0.20f,0.80f},
        {0.90f,0.90f,0.90f}, {0.15f,0.65f,0.75f},
    };
    int ci = idx % 8;
    glColor3f(palette[ci][0], palette[ci][1], palette[ci][2]);
    draw_rect(cx, cy, bw/2, bh/2);

    glColor3f(0.7f, 0.85f, 1.0f);
    draw_rect(cx, cy, bw/2 * 0.55f, bh/2 * 0.35f);

    glColor3f(0.0f, 0.0f, 0.0f);
    glLineWidth(1.0f);
    draw_rect_outline(cx, cy, bw/2, bh/2);
}

/* ------------------------------------------------------------------ */
/* draw_through_cars: straight/right-turn lane                        */
/* ------------------------------------------------------------------ */
static void draw_through_cars(int dir) {
    int count = g_snap.vehicle_count[dir];
    if (count <= 0) return;

    float cx  = WIN_W / 2.0f;
    float cy  = WIN_H / 2.0f;
    float off = g_anim[dir];

    for (int i = 0; i < count && i < MAX_VEHICLES_PER_DIR; i++) {
        float dist = (float)(i + 1) * CAR_SPACING - off;
        if (dist <= CAR_LEN / 2.0f) continue;

        float x, y;
        switch (dir) {
        case NORTH: x = cx - LANE_THROUGH; y = cy + ROAD_W + dist; break;
        case SOUTH: x = cx + LANE_THROUGH; y = cy - ROAD_W - dist; break;
        case EAST:  x = cx + ROAD_W + dist; y = cy + LANE_THROUGH; break;
        case WEST:  x = cx - ROAD_W - dist; y = cy - LANE_THROUGH; break;
        default: continue;
        }
        draw_car(x, y, (dir == NORTH || dir == SOUTH), i);
    }
}

/* ------------------------------------------------------------------ */
/* draw_left_turn_cars: left-turn lane                                 */
/* ------------------------------------------------------------------ */
static void draw_left_turn_cars(int dir) {
    float cx  = WIN_W / 2.0f;
    float cy  = WIN_H / 2.0f;
    float off = g_left_anim[dir];

    for (int i = 0; i < LEFT_CARS; i++) {
        float dist = (float)(i + 1) * CAR_SPACING - off;
        if (dist <= CAR_LEN / 2.0f) continue;

        float x, y;
        switch (dir) {
        case NORTH: x = cx - LANE_LEFT; y = cy + ROAD_W + dist; break;
        case SOUTH: x = cx + LANE_LEFT; y = cy - ROAD_W - dist; break;
        case EAST:  x = cx + ROAD_W + dist; y = cy + LANE_LEFT; break;
        case WEST:  x = cx - ROAD_W - dist; y = cy - LANE_LEFT; break;
        default: continue;
        }
        draw_car(x, y, (dir == NORTH || dir == SOUTH), i + 4);
    }
}

/* ------------------------------------------------------------------ */
/* update_anim: advance animation offsets each timer tick              */
/* ------------------------------------------------------------------ */
static void update_anim(void) {
    float dt = 1.0f / (float)FPS;
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        if (g_snap.light[d] == GREEN) {
            g_anim[d] += CAR_SPEED * dt;
            if (g_anim[d] >= CAR_SPACING) g_anim[d] -= CAR_SPACING;
        } else {
            if (g_anim[d] > 0.0f) {
                g_anim[d] -= CAR_SPEED * dt * 1.5f;
                if (g_anim[d] < 0.0f) g_anim[d] = 0.0f;
            }
        }

        if (g_snap.left_light[d] == GREEN) {
            g_left_anim[d] += CAR_SPEED * dt;
            if (g_left_anim[d] >= CAR_SPACING) g_left_anim[d] -= CAR_SPACING;
        } else {
            if (g_left_anim[d] > 0.0f) {
                g_left_anim[d] -= CAR_SPEED * dt * 1.5f;
                if (g_left_anim[d] < 0.0f) g_left_anim[d] = 0.0f;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* draw_intersection: roads, lane dividers, stop lines, zebra          */
/* ------------------------------------------------------------------ */
static void draw_intersection(void) {
    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;

    /* Road surface */
    glColor3f(0.30f, 0.30f, 0.30f);
    draw_rect(cx,          WIN_H/2.0f, ROAD_W, WIN_H/2.0f);
    draw_rect(WIN_W/2.0f,  cy,         WIN_W/2.0f, ROAD_W);

    /* Intersection box slightly lighter */
    glColor3f(0.34f, 0.34f, 0.34f);
    draw_rect(cx, cy, ROAD_W, ROAD_W);

    /* ---- Lane dividers (dashed white) ---- */
    glColor3f(0.9f, 0.9f, 0.9f);
    glLineWidth(1.5f);
    glBegin(GL_LINES);
    {
        float lx_n = cx - ROAD_W * 0.42f;
        float lx_s = cx + ROAD_W * 0.42f;
        float dash = 14.0f, gap = 10.0f;
        for (float y = cy + ROAD_W; y < WIN_H; y += dash + gap) {
            glVertex2f(lx_n, y); glVertex2f(lx_n, y + dash);
        }
        for (float y = 0; y < cy - ROAD_W; y += dash + gap) {
            glVertex2f(lx_s, y); glVertex2f(lx_s, y + dash);
        }
        float ly_e = cy + ROAD_W * 0.42f;
        float ly_w = cy - ROAD_W * 0.42f;
        for (float x = cx + ROAD_W; x < WIN_W; x += dash + gap) {
            glVertex2f(x, ly_e); glVertex2f(x + dash, ly_e);
        }
        for (float x = 0; x < cx - ROAD_W; x += dash + gap) {
            glVertex2f(x, ly_w); glVertex2f(x + dash, ly_w);
        }
    }
    glEnd();

    /* Stop lines */
    glColor3f(1.0f, 1.0f, 1.0f);
    glLineWidth(2.5f);
    glBegin(GL_LINES);
    glVertex2f(cx - ROAD_W, cy + ROAD_W); glVertex2f(cx, cy + ROAD_W);
    glVertex2f(cx, cy - ROAD_W); glVertex2f(cx + ROAD_W, cy - ROAD_W);
    glVertex2f(cx + ROAD_W, cy); glVertex2f(cx + ROAD_W, cy + ROAD_W);
    glVertex2f(cx - ROAD_W, cy - ROAD_W); glVertex2f(cx - ROAD_W, cy);
    glEnd();

    /* Yellow dashed centre lines */
    glColor3f(1.0f, 0.9f, 0.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    for (float y = 0; y < cy - ROAD_W; y += 28.0f) {
        glVertex2f(cx, y); glVertex2f(cx, y + 14.0f);
    }
    for (float y = cy + ROAD_W; y < WIN_H; y += 28.0f) {
        glVertex2f(cx, y); glVertex2f(cx, y + 14.0f);
    }
    for (float x = 0; x < cx - ROAD_W; x += 28.0f) {
        glVertex2f(x, cy); glVertex2f(x + 14.0f, cy);
    }
    for (float x = cx + ROAD_W; x < WIN_W; x += 28.0f) {
        glVertex2f(x, cy); glVertex2f(x + 14.0f, cy);
    }
    glEnd();
    glLineWidth(1.0f);

    /* Pedestrian crossing zebra stripes — direction-specific */
    if (g_snap.pedestrian_active) {
        int   blink = (g_tick / (FPS / 2)) % 2;
        float alpha = blink ? 0.90f : 0.50f;
        float sw    = 9.0f, sg = 13.0f;
        int   ns    = 4;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 1.0f, 1.0f, alpha);

        switch (g_snap.pedestrian_direction) {
        case NORTH:
            for (int i = 0; i < ns; i++) {
                float sy = cy + ROAD_W + 5.0f + (float)i * (sw + sg);
                glBegin(GL_QUADS);
                glVertex2f(cx - ROAD_W, sy);      glVertex2f(cx + ROAD_W, sy);
                glVertex2f(cx + ROAD_W, sy + sw); glVertex2f(cx - ROAD_W, sy + sw);
                glEnd();
            }
            break;
        case SOUTH:
            for (int i = 0; i < ns; i++) {
                float sy = cy - ROAD_W - 5.0f - (float)i * (sw + sg) - sw;
                glBegin(GL_QUADS);
                glVertex2f(cx - ROAD_W, sy);      glVertex2f(cx + ROAD_W, sy);
                glVertex2f(cx + ROAD_W, sy + sw); glVertex2f(cx - ROAD_W, sy + sw);
                glEnd();
            }
            break;
        case EAST:
            for (int i = 0; i < ns; i++) {
                float sx = cx + ROAD_W + 5.0f + (float)i * (sw + sg);
                glBegin(GL_QUADS);
                glVertex2f(sx,      cy - ROAD_W); glVertex2f(sx + sw, cy - ROAD_W);
                glVertex2f(sx + sw, cy + ROAD_W); glVertex2f(sx,      cy + ROAD_W);
                glEnd();
            }
            break;
        case WEST:
            for (int i = 0; i < ns; i++) {
                float sx = cx - ROAD_W - 5.0f - (float)i * (sw + sg) - sw;
                glBegin(GL_QUADS);
                glVertex2f(sx,      cy - ROAD_W); glVertex2f(sx + sw, cy - ROAD_W);
                glVertex2f(sx + sw, cy + ROAD_W); glVertex2f(sx,      cy + ROAD_W);
                glEnd();
            }
            break;
        default: break;
        }
        glDisable(GL_BLEND);
    }
}

/* ------------------------------------------------------------------ */
/* Main display callback                                               */
/* ------------------------------------------------------------------ */
static void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);

    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;

    glColor3f(0.13f, 0.24f, 0.13f);
    draw_rect(cx, cy, WIN_W/2.0f, WIN_H/2.0f);

    draw_intersection();

    /* ---- Vehicle queues ---- */
    draw_through_cars(NORTH); draw_left_turn_cars(NORTH);
    draw_through_cars(SOUTH); draw_left_turn_cars(SOUTH);
    draw_through_cars(EAST);  draw_left_turn_cars(EAST);
    draw_through_cars(WEST);  draw_left_turn_cars(WEST);

    /* ---- Main straight signals ---- */
    draw_traffic_light(cx,          cy + 215.0f, NORTH);
    draw_traffic_light(cx,          cy - 215.0f, SOUTH);
    draw_traffic_light(cx + 250.0f, cy,          EAST);
    draw_traffic_light(cx - 250.0f, cy,          WEST);

    /* ---- Left-turn arrow signals ---- */
    draw_left_signal(cx + 55.0f,  cy + 215.0f, NORTH);
    draw_left_signal(cx - 55.0f,  cy - 215.0f, SOUTH);
    draw_left_signal(cx + 250.0f, cy - 90.0f,  EAST);
    draw_left_signal(cx - 250.0f, cy + 90.0f,  WEST);

    /* ---- Status bar ---- */
    glColor3f(0.05f, 0.05f, 0.05f);
    glBegin(GL_QUADS);
    glVertex2f(0,WIN_H-50); glVertex2f(WIN_W,WIN_H-50);
    glVertex2f(WIN_W,WIN_H); glVertex2f(0,WIN_H);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    char status[256];
    snprintf(status, sizeof(status),
             "Phase: %-14s  Time: %ds  |  N=%d  S=%d  E=%d  W=%d vehicles",
             phase_str(g_snap.current_phase),
             g_snap.phase_time_remaining,
             g_snap.vehicle_count[NORTH], g_snap.vehicle_count[SOUTH],
             g_snap.vehicle_count[EAST],  g_snap.vehicle_count[WEST]);
    draw_text18(10.0f, WIN_H - 30.0f, status);

    /* ---- Pedestrian WALK label with direction ---- */
    if (g_snap.pedestrian_active) {
        int blink = (g_tick / (FPS / 2)) % 2;
        if (blink) {
            glColor3f(1.0f, 1.0f, 0.0f);
            char wbuf[32];
            snprintf(wbuf, sizeof(wbuf), "WALK [%s]",
                     dir_str(g_snap.pedestrian_direction));
            draw_text18(cx - 42.0f, cy + 8.0f, wbuf);
        }
    }

    if (g_snap.pedestrian_request && !g_snap.pedestrian_active) {
        glColor3f(1.0f, 0.55f, 0.0f);
        char preqbuf[48];
        snprintf(preqbuf, sizeof(preqbuf), "PED REQUEST [%s]",
                 dir_str(g_snap.pedestrian_direction));
        draw_text(cx - 48.0f, cy + 95.0f, preqbuf);
    }

    /* ---- Emergency overlay ---- */
    if (g_snap.emergency_mode) {
        /* Derive direction from whichever light is GREEN — guaranteed correct
           even across binary reloads, since light[] is always at offset 0. */
        int emdir = g_snap.emergency_direction;
        for (int d = 0; d < NUM_DIRECTIONS; d++) {
            if (g_snap.light[d] == GREEN) { emdir = d; break; }
        }

        int blink = (g_tick / (FPS / 5)) % 2;
        if (blink) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1.0f, 0.0f, 0.0f, 0.22f);
            draw_rect(cx, cy, WIN_W/2.0f, WIN_H/2.0f);
            glDisable(GL_BLEND);
        }
        glColor3f(1.0f, 0.25f, 0.25f);
        char emsg[64];
        snprintf(emsg, sizeof(emsg), "*** EMERGENCY: %s ***", dir_str(emdir));
        draw_text18(cx - 90.0f, WIN_H - 68.0f, emsg);
    }

    /* ---- Safety violation border ---- */
    if (g_snap.safety_violation) {
        glColor3f(1.0f, 0.0f, 0.0f);
        glLineWidth(5.0f);
        draw_rect_outline(cx, (WIN_H-50.0f)/2.0f,
                          WIN_W/2.0f - 3.0f, (WIN_H-50.0f)/2.0f - 3.0f);
        glLineWidth(1.0f);
        glColor3f(1.0f, 0.3f, 0.3f);
        draw_text18(10.0f, WIN_H - 70.0f, g_snap.safety_msg);
    }

    /* ---- Legend ---- */
    glColor3f(0.75f, 0.75f, 0.75f);
    draw_text(5.0f, 8.0f,
              "Traffic Light IPC System — Anwar Atawna"
              "   [Main signal = straight/right]   [Small signal = left-turn arrow]"
              "   [Q/ESC close]");

    glutSwapBuffers();
}

/* ------------------------------------------------------------------ */
/* Timer callback                                                      */
/* ------------------------------------------------------------------ */
static void timer_cb(int val) {
    (void)val;
    g_tick++;

    if (g_shm) {
        sem_lock();
        g_snap = *g_shm;
        sem_unlock();

        if (g_snap.shutdown) {
            printf("[GLUI] system shutdown — exiting\n");
            ipc_detach(g_shm);
            exit(0);
        }
    }

    update_anim();
    glutPostRedisplay();
    glutTimerFunc(1000 / FPS, timer_cb, 0);
}

static void keyboard_cb(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 27 || key == 'q' || key == 'Q') {
        if (g_shm) ipc_detach(g_shm);
        exit(0);
    }
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int main(int argc, char *argv[]) {
    g_shm = ipc_attach();
    if (!g_shm) {
        fprintf(stderr, "[GLUI] Cannot attach to SHM — run ./main first\n");
        return 1;
    }
    sem_lock(); g_snap = *g_shm; sem_unlock();

    printf("[GLUI] OpenGL UI started — Q or ESC to close\n");
    fflush(stdout);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(WIN_W, WIN_H);
    glutInitWindowPosition(80, 40);
    glutCreateWindow("Traffic Light IPC System");

    glClearColor(0.13f, 0.24f, 0.13f, 1.0f);
    glEnable(GL_LINE_SMOOTH);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WIN_W, 0, WIN_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glutDisplayFunc(display);
    glutKeyboardFunc(keyboard_cb);
    glutTimerFunc(1000 / FPS, timer_cb, 0);
    glutMainLoop();
    return 0;
}
