/* simulator.c - Simulateur d'evenements parametrable (M1).
 * Usage : bin/simulator --scenario f.txt [--speed 1.0] [--set cle=val | --set VOL.cle=val] [--load K]
 *   ligne de scenario :  t_s ; noeud (A|P|C) ; TYPE ; cle=val,cle=val
 *   --set TU701.delay=25 --set queue=150   : remplace une valeur sans recompiler
 *   --load K : chaque FLIGHT_DELAY est envoye K fois (TU701, TU701x2, ...) pour les tests de surcharge */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "net.h"

#define MAXEV 512
typedef struct { double t; char node; int type; char payload[160]; } ev_t;
typedef struct { char flight[16], key[24], val[24]; } ovr_t;
static ev_t evs[MAXEV]; static int nev;
static ovr_t ovr[32]; static int novr;

static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0; return s; }

static int load_scenario(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return -1; }
    char line[300];
    while (fgets(line, sizeof line, f) && nev < MAXEV) {
        char *s = trim(line); if (!*s || *s == '#') continue;
        char *f1 = strtok(s, ";"), *f2 = strtok(NULL, ";"), *f3 = strtok(NULL, ";"), *f4 = strtok(NULL, "");
        if (!f1 || !f2 || !f3 || !f4) { fprintf(stderr, "ligne ignoree : %s\n", s); continue; }
        int ty = event_from_name(trim(f3)); if (ty < 0) { fprintf(stderr, "type inconnu : %s\n", f3); continue; }
        ev_t *e = &evs[nev++]; e->t = atof(trim(f1)); e->node = trim(f2)[0]; e->type = ty;
        snprintf(e->payload, sizeof e->payload, "%s", trim(f4));
    }
    fclose(f); return 0;
}

/* applique les --set au payload : remplace la valeur d'une cle deja presente */
static void apply_overrides(char *payload, size_t n) {
    char keys[10][24], vals[10][48], flight[16] = ""; int k = 0;
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s", payload);
    for (char *tok = strtok(tmp, ","); tok && k < 10; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
        snprintf(keys[k], 24, "%s", tok); snprintf(vals[k], 48, "%s", eq + 1);
        if (!strcmp(keys[k], "flight")) snprintf(flight, 16, "%s", vals[k]); k++;
    }
    for (int o = 0; o < novr; o++) {
        if (ovr[o].flight[0] && strcmp(ovr[o].flight, flight)) continue;
        for (int i = 0; i < k; i++) if (!strcmp(keys[i], ovr[o].key)) snprintf(vals[i], 48, "%s", ovr[o].val);
    }
    payload[0] = 0;
    for (int i = 0; i < k; i++) { size_t l = strlen(payload); snprintf(payload + l, n - l, "%s%s=%s", i ? "," : "", keys[i], vals[i]); }
}

static void send_event(const ev_t *e, const char *payload, double t) {
    message_t m; msg_init(&m, e->type, "SIM"); snprintf(m.payload, sizeof m.payload, "%s", payload);
    int port = e->node == 'A' ? PORT_NODE_A : e->node == 'P' ? PORT_NODE_P : PORT_NODE_C;
    int r = net_send(port, &m);
    printf("[t=%5.1fs] -> noeud %c  %-18s %s %s\n", t, e->node, event_name(e->type), payload, r ? "(ECHEC envoi)" : "");
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *scen = "scenarios/demo_tu701.txt"; double speed = 1.0; int load = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scen = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--set") && i + 1 < argc && novr < 32) {
            char *a = argv[++i], *eq = strchr(a, '='); if (!eq) continue; *eq = 0;
            char *dot = strchr(a, '.'); ovr_t *o = &ovr[novr++]; memset(o, 0, sizeof *o);
            if (dot) { *dot = 0; snprintf(o->flight, 16, "%s", a); snprintf(o->key, 24, "%s", dot + 1); }
            else snprintf(o->key, 24, "%s", a);
            snprintf(o->val, 24, "%s", eq + 1);
        } else { fprintf(stderr, "usage: %s --scenario f [--speed x] [--set [VOL.]cle=val] [--load K]\n", argv[0]); return 1; }
    }
    if (load_scenario(scen)) return 1;
    printf("Simulateur : %d evenements, vitesse x%.1f, charge x%d\n", nev, speed, load);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); int sent = 0;
    for (int i = 0; i < nev; i++) {
        struct timespec w = t0; double tw = evs[i].t / speed;
        w.tv_sec += (time_t)tw; w.tv_nsec += (long)((tw - (long)tw) * 1e9);
        if (w.tv_nsec >= 1000000000L) { w.tv_sec++; w.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &w, NULL);
        char pl[160]; snprintf(pl, sizeof pl, "%s", evs[i].payload); apply_overrides(pl, sizeof pl);
        int copies = (evs[i].type == EV_FLIGHT_DELAY) ? load : 1;
        for (int c = 0; c < copies; c++) {
            char p2[160]; snprintf(p2, sizeof p2, "%s", pl);
            if (c > 0) { char fl[16]; if (!kv_get(pl, "flight", fl, sizeof fl)) {
                char *pos = strstr(p2, "flight="); if (pos) { char rest[160]; char *comma = strchr(pos, ',');
                    snprintf(rest, sizeof rest, "%s", comma ? comma : "");
                    snprintf(pos, sizeof p2 - (pos - p2), "flight=%sx%d%s", fl, c + 1, rest); } } }
            send_event(&evs[i], p2, evs[i].t / speed); sent++;
        }
    }
    printf("Scenario termine : %d messages envoyes.\n", sent);
    return 0;
}
