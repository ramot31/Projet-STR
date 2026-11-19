/* types.h - Structures partagees par tous les noeuds (fige par M1 - ne pas modifier sans prevenir l'equipe) */
#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>

#define MAX_FLIGHTS 16
#define NB_GATES    3     /* G12, G15, G18 */
#define NB_BUSES    5

typedef enum { GATE_FREE = 0, GATE_OCCUPIED, GATE_UNAVAILABLE } gate_state_t;
typedef enum { PRIO_NORMAL = 0, PRIO_HIGH, PRIO_CRITICAL } priority_t;
typedef enum { RISK_LOW = 0, RISK_MEDIUM, RISK_CRITICAL } risk_level_t;  /* seuils fixes par M5 */
typedef enum { AC_SMALL = 0, AC_MEDIUM, AC_WIDEBODY } aircraft_t;
typedef enum { WX_NORMAL = 0, WX_RAIN, WX_WIND } weather_t;

typedef struct {
    char     id[8];              /* "TU701" */
    int      sched_arrival_min;  /* minutes depuis minuit : 10h20 = 620 */
    int      est_arrival_min;    /* heure estimee (apres retard) */
    int      delay_min;
    int      nb_passengers;      /* 240 */
    int      nb_connecting;      /* 35 */
    int      next_flight_min;    /* 11h15 = 675 */
    char     gate[4];            /* "G12" */
    double   risk;               /* sortie IA, 0.0 .. 1.0 */
    priority_t priority;
} flight_t;

typedef struct {
    char         name[4];        /* "G12" */
    gate_state_t state;
    char         flight[8];      /* vol affecte ou "" */
    int          distance_m;     /* distance vers la zone de correspondance */
} gate_t;

/* Variables d'entree du modele IA (ordre fige : identique au CSV d'entrainement) */
typedef struct {
    double delay_min;
    double nb_connecting;
    double time_left_min;        /* next_flight_min - est_arrival_min */
    double gate_distance_m;
    double security_wait_min;
    /* variables etendues (ajout 1) */
    double aircraft_type;        /* aircraft_t */
    double rush_hour;            /* 0 ou 1 */
    double weather;              /* weather_t */
    double buses_available;      /* 0 .. 5 */
} ai_features_t;

#endif
