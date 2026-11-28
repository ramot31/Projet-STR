/* ai_policy.h - Ordonnancement intelligent, cote implementation (M3, M6).
 * Traduit en C la politique concue par M5 (scheduler/ai_policy.py) et l'applique aux threads :
 *   niveau : faible [0 ; 0,35)  moyen [0,35 ; 0,85)  critique [0,85 ; 1]
 *   priorite d'un travail de vol = base + min(3, risque x 3)   (risque absent ou invalide = 0)
 * Garde-fou : les travaux de vol vivent dans une BANDE de priorites basse (10..19 en SCHED_FIFO),
 * toujours en dessous des taches critiques (60..95). Le boost IA change l'ordre ENTRE les vols,
 * jamais l'ordre entre un vol et une tache temps reel critique. */
#ifndef AI_POLICY_H
#define AI_POLICY_H
#include "types.h"

#define M5_LOW_MAX       0.35   /* risque < 0,35 : faible */
#define M5_CRITICAL_MIN  0.85   /* risque >= 0,85 : critique */
#define M5_BOOST_FACTOR  3.0
#define M5_MAX_BOOST     3.0

/* Priorites SCHED_FIFO des taches temps reel (1..99, plus grand = plus prioritaire).
 * Attribuees par Deadline Monotonic : deadline relative plus courte => priorite plus haute. */
#define PRIO_T0     95   /* acquisition, D = 10 ms  */
#define PRIO_ALERT  90   /* alerte porte HS, D = 50 ms : la tache critique */
#define PRIO_T1     82   /* surveillance vols, D = 100 ms */
#define PRIO_T4     80   /* decision et ressources, D = 100 ms */
#define PRIO_T3     70   /* prediction IA, D = 200 ms */
#define PRIO_T2     60   /* surveillance files, D = 500 ms */

/* Bande des travaux de vol (recherche de porte, chemin passager, notification). */
#define JOB_FIFO_MIN   10
#define JOB_FIFO_MAX   19
/* Repli sans droits temps reel : nice (0 = normal, 19 = le plus faible ; un utilisateur ne peut que monter). */
#define JOB_NICE_BEST   2
#define JOB_NICE_WORST 11
#define NICE_BACKGROUND 19

_Static_assert(JOB_FIFO_MAX < PRIO_T2, "le boost IA ne doit jamais depasser une tache critique");
_Static_assert(JOB_FIFO_MIN + (int)(M5_MAX_BOOST * 3.0) <= JOB_FIFO_MAX, "bande trop etroite pour le boost");

typedef enum { POLICY_FIXED = 0, POLICY_AI } policy_t;

double       m5_clamp_risk(double risk);          /* borne dans [0, 1] ; NaN -> 0 */
risk_level_t m5_level(double risk);
const char  *m5_level_name(risk_level_t level);
double       m5_boost(double risk, policy_t policy); /* 0 en politique fixe */

/* Priorite OS d'un travail de vol. arrival_rank = rang d'arrivee dans le lot (0 = premier).
 *   fixe : ordre d'arrivee  -> JOB_FIFO_MAX - rang
 *   IA   : JOB_FIFO_MIN + boost x 3 (arrondi) */
int job_fifo_prio(policy_t policy, double boost, int arrival_rank);
int job_nice(policy_t policy, double boost, int arrival_rank);
#endif
