"""
Générateur de features synthétiques - Nœud B (M2), AI Smart Airport.

Produit UNIQUEMENT les features. Le notebook ajoute ensuite la colonne connection_missed
avec la règle experte et un bruit reproductible, avant l'entraînement.
Les deux fichiers CSV sont écrits dans le dossier du générateur.

Usage : python generatorCSV.py [n_lignes] [seed]
"""
from pathlib import Path
import sys
import numpy as np
import pandas as pd

FEATURES = [
    "scheduled_buffer_min", "delay_min", "connecting_pax", "gate_distance_m",
    "security_wait_min", "aircraft_type", "peak_hour", "weather", "buses_available",
]


def generate(n=8000, seed=42):
    rng = np.random.default_rng(seed)
    m = int(n * 1.5)  # on sur-génère puis on filtre les cas incohérents

    aircraft = rng.choice(["small", "medium", "large"], m, p=[0.25, 0.55, 0.20])
    peak = rng.binomial(1, 0.35, m)
    weather = rng.choice(["normal", "rain", "strong_wind"], m, p=[0.70, 0.20, 0.10])

    # Passagers : la capacité dépend du type d'avion, ~17 % en correspondance
    pax_total = np.where(aircraft == "small", rng.integers(70, 121, m),
                 np.where(aircraft == "medium", rng.integers(150, 221, m),
                          rng.integers(280, 401, m)))
    connecting = np.clip(rng.binomial(pax_total, rng.beta(4, 20, m)), 1, None)

    # Retard annoncé : plus long avec mauvaise météo et heure de pointe
    delay = rng.lognormal(2.7, 0.65, m)
    delay *= np.where(weather == "rain", 1.25, np.where(weather == "strong_wind", 1.5, 1.0))
    delay *= np.where(peak == 1, 1.15, 1.0)
    delay = np.clip(np.round(delay), 1, 90).astype(int)

    # Temps restant avant le vol suivant = marge prévue - retard (comme TU701 : 55 - 20 = 35)
    scheduled_buffer = np.clip(rng.normal(75, 25, m), 45, 150).round().astype(int)
    remaining = scheduled_buffer - delay

    # Distance entre portes (m)
    distance = np.clip(rng.gamma(3.0, 130.0, m), 50, 800).round().astype(int)

    # Attente sécurité = personnes x 0,1 min x facteur de ralentissement (x2)
    queue = np.clip(rng.lognormal(4.0, 0.6, m) * np.where(peak == 1, 1.8, 1.0), 0, 250)
    slowdown = np.where(rng.binomial(1, 0.10, m) == 1, 2.0, 1.0)
    security_wait = (queue * 0.1 * slowdown).round(1)

    # Bus : 5 au total, certains déjà en service (plus en pointe), panne possible (5 -> 4)
    in_use = rng.binomial(4, 0.25 + 0.30 * peak)
    breakdown = rng.binomial(1, 0.08, m)
    buses = np.clip(5 - in_use - breakdown, 0, 5)

    df = pd.DataFrame({
        "delay_min": delay,
        "connecting_pax": connecting,
        "scheduled_buffer_min": scheduled_buffer,
        "time_remaining_min": remaining,  # sert uniquement au filtre, retirée en sortie
        "gate_distance_m": distance,
        "security_wait_min": security_wait,
        "aircraft_type": aircraft,
        "peak_hour": peak,
        "weather": weather,
        "buses_available": buses,
    })

    # Cohérence : le vol retardé doit encore arriver avant le départ du vol suivant (reste >= 5 min)
    df = df[df["time_remaining_min"] >= 5].head(n).reset_index(drop=True)
    return df[FEATURES]


def demo_cases():
    """Cas de démo synthétiques correspondant aux risques de référence."""
    return pd.DataFrame([
        dict(flight="TU701", scheduled_buffer_min=55, delay_min=20, connecting_pax=35,
             gate_distance_m=450, security_wait_min=12.0, aircraft_type="medium",
             peak_hour=1, weather="normal", buses_available=4),
        dict(flight="TU702", scheduled_buffer_min=85, delay_min=5, connecting_pax=12,
             gate_distance_m=150, security_wait_min=3.0, aircraft_type="small",
             peak_hour=0, weather="normal", buses_available=5),
        dict(flight="TU703", scheduled_buffer_min=55, delay_min=15, connecting_pax=28,
             gate_distance_m=300, security_wait_min=8.0, aircraft_type="medium",
             peak_hour=1, weather="rain", buses_available=3),
    ])


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 42
    output_dir = Path(__file__).resolve().parent
    dataset_path = output_dir / "dataset_features.csv"
    demo_path = output_dir / "demo_cases.csv"
    generate(n, seed).to_csv(dataset_path, index=False)
    demo_cases().to_csv(demo_path, index=False)
    print(f"{dataset_path} ({n} lignes) et {demo_path} écrits")
