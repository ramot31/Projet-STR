/* ai_policy.c - Voir ai_policy.h. */
#include "ai_policy.h"

#define PRIO_STEPS_PER_BOOST 3.0   /* 1 point de boost = 3 niveaux de priorite OS */

static int round_pos(double x) { return (int)(x + 0.5); }

double m5_clamp_risk(double risk) {
    if (!(risk >= 0.0)) return 0.0;   /* attrape aussi NaN */
    return risk > 1.0 ? 1.0 : risk;
}

risk_level_t m5_level(double risk) {
    double r = m5_clamp_risk(risk);
    if (r >= M5_CRITICAL_MIN) return RISK_CRITICAL;
    if (r >= M5_LOW_MAX) return RISK_MEDIUM;
    return RISK_LOW;
}

const char *m5_level_name(risk_level_t level) {
    return level == RISK_CRITICAL ? "CRITIQUE" : level == RISK_MEDIUM ? "MOYEN" : "FAIBLE";
}

double m5_boost(double risk, policy_t policy) {
    if (policy != POLICY_AI) return 0.0;
    double b = m5_clamp_risk(risk) * M5_BOOST_FACTOR;
    return b > M5_MAX_BOOST ? M5_MAX_BOOST : b;
}

int job_fifo_prio(policy_t policy, double boost, int arrival_rank) {
    if (policy == POLICY_AI) {
        int p = JOB_FIFO_MIN + round_pos(boost * PRIO_STEPS_PER_BOOST);
        return p > JOB_FIFO_MAX ? JOB_FIFO_MAX : p;
    }
    int p = JOB_FIFO_MAX - (arrival_rank < 0 ? 0 : arrival_rank);
    return p < JOB_FIFO_MIN ? JOB_FIFO_MIN : p;
}

int job_nice(policy_t policy, double boost, int arrival_rank) {
    if (policy == POLICY_AI) {
        int n = JOB_NICE_BEST + round_pos((M5_MAX_BOOST - boost) * PRIO_STEPS_PER_BOOST);
        return n > JOB_NICE_WORST ? JOB_NICE_WORST : (n < JOB_NICE_BEST ? JOB_NICE_BEST : n);
    }
    int n = JOB_NICE_BEST + (arrival_rank < 0 ? 0 : arrival_rank);
    return n > JOB_NICE_WORST ? JOB_NICE_WORST : n;
}
