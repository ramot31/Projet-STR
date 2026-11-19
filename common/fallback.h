/* fallback.h - Regle de repli sans IA (M1). Utilisee par le noeud C si le noeud B ne repond plus.
 * Depend seulement du retard et du temps restant avant le vol suivant. Volontairement simple. */
#ifndef FALLBACK_H
#define FALLBACK_H
#include "types.h"

static inline double fallback_risk(double delay_min, double time_left_min) {
    double r = 0.5 * (delay_min / 30.0) + 0.5 * (1.0 - time_left_min / 60.0);
    if (time_left_min <= 15.0) r = 0.95;
    return r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r);
}

/* Seuils PROVISOIRES : M5 les fixe definitivement avant le 22 nov. */
#define RISK_T_MEDIUM 0.50
#define RISK_T_CRITICAL 0.80
static inline risk_level_t risk_to_level(double risk) {
    return risk >= RISK_T_CRITICAL ? RISK_CRITICAL : (risk >= RISK_T_MEDIUM ? RISK_MEDIUM : RISK_LOW);
}
#endif
