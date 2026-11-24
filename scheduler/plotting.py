"""Matplotlib output for Member 5 experiments."""

from pathlib import Path


def gantt(result, path: str | Path, title: str | None = None):
    import matplotlib.pyplot as plt
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(12, 3.5))
    colors = {"IDLE": "#dddddd"}
    labels = sorted({label for _, _, label in result.timeline if label != "IDLE"})
    cmap = plt.get_cmap("tab10")
    colors.update({label: cmap(i % 10) for i, label in enumerate(labels)})
    y = {label: i for i, label in enumerate(labels)}
    y["IDLE"] = -1
    for start, end, label in result.intervals():
        ax.barh(y[label], end - start, left=start, height=0.65,
                color=colors[label], edgecolor="black", linewidth=0.4)
        if label != "IDLE":
            ax.text((start + end) / 2, y[label], label, ha="center", va="center", fontsize=8)
    for job in result.jobs:
        ax.axvline(job.absolute_deadline, color="red" if job.missed else "#888888",
                    alpha=0.25 if not job.missed else 0.8, linewidth=0.8,
                    linestyle="--")
    ax.set_yticks([y[label] for label in labels])
    ax.set_yticklabels(labels)
    ax.set_xlabel("time (ms)")
    ax.set_title(title or f"{result.algorithm} schedule")
    ax.grid(axis="x", alpha=0.25)
    fig.tight_layout()
    # Passing an open handle avoids a Windows/Pillow failure when an existing
    # generated PNG is being replaced while an IDE still has it indexed.
    with path.open("wb") as handle:
        fig.savefig(handle, format="png", dpi=160)
    plt.close(fig)


def miss_plot(rows, path: str | Path):
    import matplotlib.pyplot as plt
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for algorithm in ("RM", "DM", "EDF"):
        points = [(row["utilization"], row[algorithm + "_misses"]) for row in rows]
        ax.plot(*zip(*points), marker="o", label=algorithm)
    ax.axvline(1.0, color="black", linestyle="--", linewidth=0.8, label="U=1")
    ax.set(xlabel="CPU utilization U", ylabel="deadline misses", title="Load experiment")
    ax.grid(alpha=0.25)
    ax.legend()
    fig.tight_layout()
    with path.open("wb") as handle:
        fig.savefig(handle, format="png", dpi=160)
    plt.close(fig)
