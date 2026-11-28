# Membre 3 : threads POSIX et ordonnancement intelligent (implémentation)

Module `rt/`, exécutable `bin/rt_engine`. Il transforme le scénario en application concurrente temps réel :
de vrais threads POSIX, des tâches périodiques sans dérive, des priorités `SCHED_FIFO`, et la politique IA
de M5 appliquée aux threads de chaque vol.

## Compiler et lancer (Linux ou WSL)

```bash
make bin/rt_engine && make test_rt          # build + tests unitaires (politique, mesures, dérive)
sudo ./bin/rt_engine --scenario scenarios/demo_tu701.txt --set TU701.delay=25 --set queue=150
sudo make rt-compare                        # priorités fixes contre priorités IA (TU701, TU703, TU702)
sudo ./bin/rt_engine --scenario scenarios/surcharge.txt --load 5   # surcharge : 20 vols
```

`sudo` donne le droit d'utiliser `SCHED_FIFO`. Sans lui, le programme se replie sur `SCHED_OTHER` + `nice`
et l'affiche sur la 2e ligne de sortie. Dans WSL : `wsl -u root`.

Options : `--policy ai|fixed`, `--set [VOL.]cle=val`, `--load K`, `--risk VOL=0.83`, `--ai-cost-ms 4`,
`--window-ms 20`, `--cpu N` (`-1` = multiprocesseur), `--no-fifo`, `--no-background`, `--csv-prefix p`.

## Tâches

| Tâche | Rôle | Type | T (ms) | D (ms) | Prio FIFO | Criticité |
|---|---|---|---|---|---|---|
| T0 | acquisition, route les événements vers les files | sporadique | – | 10 | 95 | haute |
| ALR | alerte porte indisponible | sporadique | – | **50** | 90 | **critique** |
| T1 | surveillance des vols, déclenche T3 | périodique | 100 | 100 | 82 | haute |
| T4 | décision et ressources, lance les vols | sporadique | – | 100 | 80 | haute |
| T3 | prédiction IA (coût mesuré par M2 : 4 ms) | sporadique | – | 200 | 70 | haute |
| T2 | surveillance des files, sécurité, bus | périodique | 500 | 500 | 60 | moyenne |
| Vols | porte (sémaphore M4), chemin, bus et notification | sporadique | – | – | **10 à 19** | conseil IA |
| BG | analyse historique (charge de fond) | apériodique | – | – | nice 19 | basse |

Les priorités suivent Deadline Monotonic : deadline plus courte, priorité plus haute. Le boost IA reste
dans la bande 10 à 19, toujours sous T2 (60). Ce garde-fou est vérifié à la compilation (`_Static_assert`)
et dans `tests/test_rt.c`.

Hypothèse monoprocesseur, comme l'étude RM/DM/EDF de M5 : tous les threads sont placés sur le CPU 0.

## Résultats de référence (`rt/results/`, SCHED_FIFO, WSL2)

| Vol (risque) | Fixe : réponse | IA : réponse | Fixe : porte | IA : porte |
|---|---|---|---|---|
| TU701 (0,87) | 146,2 ms | **65,6 ms** | G18 | **G12** |
| TU703 (0,65) | 106,0 ms | 105,9 ms | G15 | G15 |
| TU702 (0,20) | 65,8 ms | 146,0 ms | G12 | G18 |

Les tâches T0, ALR, T1, T2, T3 et T4 ont 0 dépassement dans tous les scénarios, y compris en surcharge ×5.
