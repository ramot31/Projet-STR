# Member 5 integration guide

The standalone scheduler is intentionally isolated under `scheduler/` and
does not modify `common/types.h`, `common/protocol.h`, Node A/B/C/P, or the
existing simulator. The module's local `Task` model maps naturally from an
integration adapter as follows: `task_id`, `execution=C`, `period=T`,
`deadline=D`, release time, remaining execution, and risk.

Use `python -m scheduler.run_experiments` to regenerate Member 5 artifacts.
Other members should consume the CSV results or import `Task` and `simulate`;
the scheduler does not open sockets and does not call the AI service directly.
An adapter may translate Node B's normalized risk to `Task.risk` or to an
`AIPolicy` `RiskRecord`.

Integration rules:

1. Keep real-time-critical work marked `hard_real_time=True`; AI boosts are
   never allowed to reorder it.
2. Treat unavailable/failed AI as risk 0.0 and continue with fixed scheduling.
3. Do not interpret an AI priority as a deadline guarantee.
4. Preserve the existing shared protocol and types. If a future integration
   needs a shared field, propose it to the team rather than editing the frozen
   headers silently.
5. The simulator is discrete-time and intended for analysis/Gantt charts. It
   is not Member 3's live POSIX-thread scheduler.

The default risk boundaries are exact: low `[0.0, 0.35)`, medium
`[0.35, 0.85)`, and critical `[0.85, 1.0]`. A missing/failed AI result uses
risk `0.0`. The boost is capped at 3.0. `hard_real_time=True` bypasses the AI
boost and preserves the selected RM/DM/EDF classical key.

The current repository has no shared scheduler interface to modify. A future
adapter should be added outside this owned module and should pass explicit
`C`, `T`, `D`, release, and criticality values rather than relying on hidden
defaults.
