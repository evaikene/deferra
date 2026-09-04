# JobU Planning Documents

This directory contains approved technical and code-level plans for JobU.

These documents describe intended behavior and staged implementation boundaries.
The phase status records project progress, while the repository root README and
current source tree remain authoritative for exact implemented behavior.

## Documents

| Document | Phase status | Purpose |
| --- | --- | --- |
| [JobU v1 technical plan](jobu-v1-technical-plan.md) | Active | Defines v1 product scope, architecture, invariants, phased delivery, acceptance criteria, and explicitly deferred work. |
| [JobU Phase 0 code-level design](jobu-phase0-code-design.md) | Completed | Defines the initial module boundaries and reusable error, buffer, UUID, time, and attribute contracts. |
| [JobU Phase 1 code-level design](jobu-phase1-code-design.md) | Completed | Defines the backend-independent database API and isolated SQLite driver. |
| [JobU Phase 2 code-level design](jobu-phase2-code-design.md) | Completed | Defines bounded local IPC, JSON and JSON-RPC infrastructure, and the first `system.info` round trip. |
| [JobU Phase 3 code-level design](jobu-phase3-code-design.md) | Completed | Defines the durable application model, persistence repositories, management service, RPC methods, and command-line control plane. |
| [JobU Phase 4 code-level design](jobu-phase4-code-design.md) | Completed | Defines cron evaluation and the centralized, event-driven scheduler with deterministic attempt execution. |
| [JobU Phase 5 code-level design](jobu-phase5-code-design.md) | Completed | Defines the asynchronous HTTP implementation, stage boundaries, verification requirements, and Phase 6/7 entry boundaries. |
| [JobU Phase 5 Stage 5.4a code-level design](jobu-phase5-stage5.4a-code-design.md) | Completed | Defines explicit EventLoop readiness trigger modes and the level-triggered libcurl integration correction. |
| [JobU Phase 5 closure code-level design](jobu-phase5-closure-code-design.md) | Completed | Defines the Object private-data and signal/callback architecture, closure stages, and Phase 6 entry boundary. |
| [JobU Phase 6 code-level design](jobu-phase6-code-design.md) | Implementation-ready | Defines asynchronous local command execution, process supervision, CLI policy, executor integration, and platform stage boundaries. |

## Authority

Use the documents in this order:

1. The current source tree is authoritative for already implemented behavior.
2. The v1 technical plan defines product-level intent and cross-phase invariants.
3. A phase code-level design specializes the technical plan for that phase.
4. The pull request's declared stage defines the scope of an individual change.

If implementation evidence materially conflicts with an approved design, stop
and revise the design rather than silently changing the architecture.

## Staged implementation

Each code-level design stage is a separate implementation and review boundary.

For the stage named by a pull request:

- the stage's `Implement`, `Verification`, and `Exit` sections are in scope;
- design sections referenced by that stage apply;
- explicit exclusions and deferred entry boundaries remain out of scope;
- requirements assigned to later stages are planned work, not omissions in the
  current stage.

Deferral does not excuse a correctness, security, lifetime, data-integrity,
public-contract, or compatibility defect introduced by the current change.

Pull request descriptions should identify the applicable plan and stage:

```text
Plan: docs/planning/jobu-phase5-code-design.md
Stage: 5.3
```

## Maintenance

- Approve and merge plan changes before the corresponding implementation when
  practical.
- Mark a stage complete only after its implementation has merged and its
  required validation has passed.
- Keep this file as an index; do not duplicate stage requirements here.
- Preserve superseded plans as design history and clearly link to their
  replacements.
