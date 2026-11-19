#include <assert.h>
#include <stdio.h>
#include "../common/fallback.h"
int main(void) {
    double r = fallback_risk(20, 35);
    printf("TU701 repli : risque=%.2f niveau=%d\n", r, risk_to_level(r));
    assert(fallback_risk(25, 10) > fallback_risk(5, 50));   /* plus de retard, moins de temps => plus de risque */
    assert(risk_to_level(0.87) == RISK_CRITICAL && risk_to_level(0.20) == RISK_LOW);
    printf("test_fallback OK\n");
    return 0;
}
