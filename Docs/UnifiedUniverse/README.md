# Unified Universe — kickoff package

Everything needed to start the Unified Universe prototype in an unattended Claude Code session while you are away.

## Start the session (5 minutes before you leave)

1. Copy this folder into the vxl project, for example to `Docs/UnifiedUniverse/`.
2. Open Claude Code at the project root.
3. Paste the contents of `KICKOFF_PROMPT.md` (everything below its line).
4. Grant whatever permissions you're comfortable leaving on for the day. The prompt keeps all changes inside the new plugin on branch `uu/prototype-day1`, but tool approvals that need a click will stall it until you return.

## Tonight

Open `Plugins/UnifiedUniverse/Docs/REVIEW_TONIGHT.md` first. It links the verification report, lists what needs testing in Unreal with exact steps, and lists every decision taken while you were away.

## What's inside

| Path | What it is |
| --- | --- |
| `KICKOFF_PROMPT.md` | The prompt for the unattended session: situation, plan P0–P6 with gates, decision rules, handover format |
| `docs/UnifiedUniverse_Design.md` | The full design doc, exported (diagrams converted to Mermaid and a table) |
| `docs/DECISIONS.md` | Provisional answers to every open question (D-001–D-019), plus where new decisions get logged |
| `docs/VERIFICATION_TOOLING.md` | The self-verification tooling the session builds, and the checks that define "done" |
| `plugin_seed/CLAUDE.md` | Standing working agreement, copied to `Plugins/UnifiedUniverse/CLAUDE.md` |
| `templates/` | Starting files for the environment record, progress log and tonight's review |
| `reference/ULS_REFERENCE_INDEX.md` | Which UltraLargeScale files to consult for which part of the work |

## Decisions worth a look before you leave

Three provisional decisions affect level assignment and seeds, so they're expensive to change once content exists: **D-002** (coordinate width), **D-010** (window size N = 2) and **D-011** (m_ref = 6.5, θ_min = 8e-4 rad). They're fine for a prototype; just confirm them before any content is authored on top. **D-008** (Local + Cruise motion modes) also answers the open comment on the design doc.
