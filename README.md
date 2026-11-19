# AI Smart Airport - Projet STR (INSAT)
Gestion intelligente des correspondances.

## Arborescence
- common/    : types.h, protocol.h, config (M1)        - NE PAS MODIFIER sans prevenir
- node_a/    : vols + portes (M3, M4)
- node_b/    : IA / inference (M2)
- node_c/    : decision + ordonnanceur (M5)
- node_p/    : passagers, files, bus (M4)
- scheduler/ : simulateur RM/DM/EDF (M5)
- ai/ data/  : entrainement, jeu de donnees (M2)
- scenarios/ : scenarios de demo et de charge (M1)
- tests/     : tests unitaires et de course critique
- docs/      : rapport, schemas

## Regles Git
- branche `main` protegee : on y merge seulement du code qui compile (`make`).
- une branche par membre : `m1-integration`, `m2-ia`, `m3-threads`, `m4-sync`, `m5-sched`, `m6-network`.
- messages de commit courts : `m4: ajout gate_reserve avec semaphore`.
- integration de M1 : jeudi 26 et vendredi 27 novembre.

## Build
    make        # compile et lance les tests
