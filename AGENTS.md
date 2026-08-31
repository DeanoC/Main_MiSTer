# Main_MiSTer working policy

## Start here

Read `README.md` before changing this fork. Identify the branch and trace the
existing Main command path before proposing a different runtime.

## Clarity is part of correctness

1. A fresh clone must state what this fork contains, how FogCast uses it, and
   whether the checked-out branch is conventional Main or an experiment.
2. Documentation describes present branch code in present tense. Proposed or
   experimental work is labelled as such.
3. A commit changing the FogCast-facing command or runtime path updates the
   README in the same commit.
4. Git history is the archive. Do not keep superseded implementations beside
   the current implementation for reference.
5. Preserve upstream Main behavior unless a concrete FogCast requirement
   demonstrates the need for a change.
6. Extend the proven path with the smallest useful change.
7. If documentation, code, the built artifact, and hardware observation
   disagree, resolve the discrepancy before unrelated development continues.
8. Keep experimental native-runtime work on explicitly named branches. Never
   describe branch code as deployed without checking the deployed binary.
9. Do not add coordinators, supervisors, ownership databases, attestation,
   rollback systems, security frameworks, failover, or recovery machinery
   without a demonstrated current need and explicit user approval.
10. Tests must be proportional to the change. FPGA and core-loading behavior
    is verified on the disposable development hardware when relevant.
11. Commits are coherent and describe the working result, not a chain of
    status or evidence documents.

## Disposable development hardware

The designated MiSTer Pi is disposable local hobby-development hardware.
SSH, `root`/`1`, changing host keys, rebooting, reflashing, and rebuilding are
normal. Keep private target addresses and credentials out of tracked files.

## Working practices

- Preserve unrelated user changes and upstream licensing.
- Use a branch or worktree for substantial changes.
- Prefer `rg` for source discovery.
- Run the narrowest relevant build or test and `git diff --check`.
- Do not commit, push, or open a pull request unless the user authorizes it.
