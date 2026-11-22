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

## Entraînement IA
Exécuter `ai/train_risk_model.ipynb` depuis le dossier `ai` ou la racine du
projet. La règle de `ai/label_rule.py` ajoute `connection_missed` au CSV comme
cible binaire avec un bruit reproductible (seed 42). Seules les variables brutes
alimentent le modèle. Le score de risque est calculé à la demande et n'est pas
enregistré dans le CSV ; le risque théorique sert uniquement à l'évaluation.

### Consulter les variables d’entrée du modèle

Le modèle sauvegardé contient la liste des colonnes attendues dans `risk_raw_columns_`.
Depuis le dossier `ai/`, exécuter :

```bash
python -c "import joblib; model = joblib.load('model/risk_model.joblib'); print(model.risk_raw_columns_)"
```

La commande affiche les 9 colonnes attendues. Pour consulter les valeurs de remplacement
utilisées lorsque des champs sont absents, afficher également `model.risk_defaults_`.

### Installer les dépendances

Pour lancer le nœud B avec le modèle déjà entraîné, installer les dépendances avec :

```bash
python -m pip install -r requirements.txt
```

Les versions sont fixées, car `risk_model.joblib` doit être rechargé avec la même
version de scikit-learn que celle utilisée lors de l’entraînement.

### Tester la connexion entre les nœuds B et C

Ce test est facultatif. Sous Windows, exécuter les commandes suivantes dans un
terminal Ubuntu sous WSL ; elles fonctionnent aussi sous Ubuntu. Vérifier que le
modèle exporté `ai/model/risk_model.joblib` est présent sur le PC personnel et
que les ports 5002 et 5003 sont libres.

Installer les outils nécessaires :

```bash
sudo apt update
sudo apt install build-essential python3 python3-venv
```

Depuis la racine du projet, compiler le nœud C et installer les dépendances Python
dans un environnement virtuel :

```bash
make dirs
make bin/node_c
python3 -m venv ai/.venv
source ai/.venv/bin/activate
python -m pip install -r ai/requirements.txt
```

Lancer le test depuis la racine du projet :

```bash
python ai/test_node_b_node_c.py
```

Le script démarre B et C temporairement et vérifie que C reçoit un résultat de
risque de B. En cas de succès, il affiche un message commençant par `OK`.
