/* resources.h - Ressources partagees protegees (M4) : portes d'embarquement et bus.
 *
 * Modele (meme mecanisme pour les deux ressources) :
 *   - un SEMAPHORE COMPTEUR = nombre d'unites libres et en etat de marche
 *     (un thread qui n'obtient pas de permis attend, avec timeout : c'est la file d'attente) ;
 *   - un MUTEX par ressource protege la table d'etat des unites (qui est libre, qui l'occupe).
 *   Ordre d'acquisition fixe : semaphore PUIS mutex (jamais l'inverse) -> pas d'interblocage.
 *   Le mutex est PTHREAD_PRIO_INHERIT quand la plateforme le permet (evite l'inversion de priorite).
 *
 * Toutes les fonctions sont thread-safe. timeout_ms : 0 = essai sans attente, < 0 = attente sans limite.
 * A lier par le noeud qui possede la ressource : portes -> noeud A (M6), bus -> noeud passagers P (M4). */
#ifndef RESOURCES_H
#define RESOURCES_H
#include "types.h"

#define GATE_NAME_LEN 4   /* "G12" + '\0' (identique a gate_t.name) */

typedef enum {
    RES_OK       =  0,
    RES_TIMEOUT  = -1,    /* aucune unite disponible dans le delai */
    RES_INVALID  = -2     /* argument inconnu, double liberation, unite deja hors service */
} res_status_t;

/* (Re)initialise portes (G12, G15, G18 libres) et bus (NB_BUSES libres). A appeler hors concurrence. */
void resources_init(void);

/* --- Portes ---------------------------------------------------------------------------------- */
/* Reserve la porte libre la plus proche de la zone de correspondance (distance_m minimale).
 * Ecrit son nom dans gate_out (GATE_NAME_LEN octets). */
int gate_reserve(const char *flight, int timeout_ms, char gate_out[GATE_NAME_LEN]);
int gate_release(const char *gate);
/* Evenement GATE_UNAVAILABLE : la porte ne sera plus jamais attribuee. Si un vol l'occupe, il la
 * garde jusqu'a sa liberation, puis elle sort du service (la capacite baisse sans casser le semaphore). */
int gate_set_unavailable(const char *gate);
int gates_free_count(void);

/* --- Bus ------------------------------------------------------------------------------------- */
/* Reserve un bus ; bus_id vaut 1..NB_BUSES. */
int bus_reserve(int timeout_ms, int *bus_id);
int bus_release(int bus_id);
/* Evenement BUS_BREAKDOWN : un bus tombe en panne (de preference un bus libre). Retourne son id,
 * ou RES_INVALID s'il n'en reste plus. */
int bus_breakdown(void);
/* Evenement BUS_BREAKDOWN buses=N : abaisse la capacite a N bus (jamais de hausse : pas de reparation
 * modelisee). Retourne la capacite obtenue. */
int bus_set_capacity(int target);
int buses_free_count(void);
int buses_capacity(void);   /* bus en etat de marche, libres ou occupes */

#endif
