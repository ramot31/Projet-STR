"""
predict.py - Inférence du risque de correspondance manquée (nœud B, M2).

Charge model/risk_model.joblib (exporté par le notebook) et expose :

    predict(features: dict) -> {"risk": float}      # probabilité entre 0 et 1

Les noms d'entrée sont ceux des variables du modèle ; les noms inconnus sont refusés.
Les codes aircraft_type 0/1/2 et weather 0/1/2 sont convertis en libellés.

Règles de sécurité :
- delay_min et scheduled_buffer_min sont OBLIGATOIRES : sans eux, la
  fonction lève ValueError. Le nœud B ne répond alors pas, et C passe en mode dégradé
  (règle de repli) au lieu de recevoir un risque faux.
- Les autres champs manquants sont remplacés par des valeurs par défaut, avec un
  avertissement dans le log.
"""
import logging
from pathlib import Path

import joblib
import numpy as np
import pandas as pd

MODEL_PATH = Path(__file__).resolve().parent / "model" / "risk_model.joblib"
logger = logging.getLogger("airport.risk")

AIRCRAFT_CODES = {0: "small", 1: "medium", 2: "large"}        # aircraft_t (types.h)
WEATHER_CODES = {0: "normal", 1: "rain", 2: "strong_wind"}    # weather_t (types.h)
CATEGORICAL = {"aircraft_type": AIRCRAFT_CODES, "weather": WEATHER_CODES}
REQUIRED = ("delay_min", "scheduled_buffer_min")

_MODEL = None


def load_model():
    """Charge le modèle une seule fois."""
    global _MODEL
    if _MODEL is None:
        _MODEL = joblib.load(MODEL_PATH)
    return _MODEL


def _is_missing(value):
    return value is None or (not isinstance(value, str) and pd.isna(value))


def _to_model_inputs(features):
    converted = features.copy()
    # codes numériques -> libellés
    for col, codes in CATEGORICAL.items():
        value = converted.get(col)
        if isinstance(value, str) and value.strip().lstrip("-").isdigit():
            value = int(value)
        if isinstance(value, (int, float, np.number)) and not isinstance(value, bool):
            converted[col] = codes.get(int(value), f"code_inconnu_{int(value)}")
    return converted


def predict(features: dict) -> dict:
    model = load_model()
    unknown = set(features) - set(model.risk_raw_columns_)
    if unknown:
        raise ValueError(f"Variables inconnues pour le modèle : {', '.join(sorted(unknown))}")
    given = _to_model_inputs(features)

    absent = [c for c in REQUIRED if _is_missing(given.get(c))]
    if absent:
        raise ValueError(f"Champs obligatoires manquants : {', '.join(absent)}")

    values, replaced = {}, []
    for col in model.risk_raw_columns_:
        value = given.get(col)
        if _is_missing(value):
            value = model.risk_defaults_[col]
            replaced.append(col)
        values[col] = value
    if replaced:
        logger.warning("Champs manquants remplacés par défaut : %s", ", ".join(replaced))

    for col, value in values.items():
        if col in CATEGORICAL:
            values[col] = str(value)
            if values[col] not in CATEGORICAL[col].values():
                logger.warning("Catégorie inconnue pour %s : %r (ignorée par le modèle)", col, value)
        else:
            try:
                values[col] = float(value)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"Valeur numérique invalide pour {col} : {value!r}") from exc
            if not np.isfinite(values[col]):
                raise ValueError(f"Valeur numérique non finie pour {col}")

    row = pd.DataFrame([values])
    risk = float(model.predict_proba(row[model.risk_feature_columns_])[0, 1])
    return {"risk": risk}