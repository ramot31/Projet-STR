"""Règle experte de simulation de correspondance manquée."""

import numpy as np
import pandas as pd
from scipy.special import ndtr

# Hypothèses de simulation : durées en minutes, distances en mètres.
DISEMBARK_MIN = {"small": 5, "medium": 10, "large": 18}
WALKING_SPEED_M_PER_MIN = 80
WEATHER_FACTOR = {"normal": 1.00, "rain": 1.10, "strong_wind": 1.20}
PEAK_FACTOR = 1.10
GROUP_MIN_PER_PASSENGER = 0.08
BUS_CAPACITY = 40
EXTRA_ROTATION_MIN = 5
NO_BUS_WAIT_MIN = 8
BOARDING_CLOSURE_MIN = 10
NOISE_SIGMA_MIN = 6


def needed_time(df: pd.DataFrame) -> pd.Series:
    """Temps nécessaire pour rejoindre le vol suivant, en minutes."""
    numeric = df[["gate_distance_m", "connecting_pax", "buses_available",
                  "security_wait_min", "peak_hour"]]
    if not np.isfinite(numeric.to_numpy()).all():
        raise ValueError("Les variables numériques doivent être finies et non manquantes.")
    if not df["peak_hour"].isin([0, 1]).all():
        raise ValueError("peak_hour doit valoir 0 ou 1.")
    disembark = df["aircraft_type"].map(DISEMBARK_MIN)
    weather = df["weather"].map(WEATHER_FACTOR)
    if disembark.isna().any() or weather.isna().any():
        raise ValueError("Type d'avion ou météo inconnu ou manquant.")

    peak = np.where(df["peak_hour"] == 1, PEAK_FACTOR, 1.0)
    rotations = np.ceil(
        df["connecting_pax"] / (BUS_CAPACITY * df["buses_available"].clip(lower=1))
    )
    bus_wait = EXTRA_ROTATION_MIN * (rotations - 1) + NO_BUS_WAIT_MIN * (
        df["buses_available"] == 0
    )
    return (
        (disembark + df["gate_distance_m"] / WALKING_SPEED_M_PER_MIN) * weather * peak
        + GROUP_MIN_PER_PASSENGER * df["connecting_pax"]
        + bus_wait
        + df["security_wait_min"]
    )


def available_time(df: pd.DataFrame) -> pd.Series:
    """Temps disponible avant la clôture de l'embarquement, en minutes."""
    if not np.isfinite(df[["scheduled_buffer_min", "delay_min"]].to_numpy()).all():
        raise ValueError("Les variables numériques doivent être finies et non manquantes.")
    return df["scheduled_buffer_min"] - df["delay_min"] - BOARDING_CLOSURE_MIN


def true_risk(df: pd.DataFrame) -> pd.Series:
    """Probabilité théorique de manquer la correspondance."""
    probability = ndtr((needed_time(df) - available_time(df)) / NOISE_SIGMA_MIN)
    return pd.Series(probability, index=df.index, name="true_risk")


def compute_label(df: pd.DataFrame, seed: int = 42) -> pd.Series:
    """Classe simulée avec un bruit gaussien reproductible."""
    noise = np.random.default_rng(seed).normal(0, NOISE_SIGMA_MIN, len(df))
    return (needed_time(df) + noise > available_time(df)).astype(int).rename("connection_missed")
