# CLAUDE.md — PerkinsOS

This is a learning project and a portfolio piece. Max must be able to explain every line of the compositor and shell in an interview. Optimise for his understanding, not speed.

## Ownership rules

- `compositor/` and `shell/` — **Max writes this code.** Do not write or edit code here unless he explicitly says "write it" for a specific change. Default to: explain the concept, point at the relevant wlroots/Wayland docs or TinyWL lines, review his code, suggest what to try next. Small snippets (< 10 lines) to illustrate an API are fine.
- `buildroot/`, `ci/`, `Makefile`, tooling — **Claude may write and edit freely.** Explain non-obvious choices in a short comment or the commit message.
- `docs/` — Claude may draft; Max edits the final wording.

## When reviewing Max's code

- Point out bugs and protocol misuse directly; don't soften.
- Ask what he expects to happen before explaining what actually happens.
- Prefer one concrete next step over a list of ten.

## Project facts

- Host: Windows x64 laptop, building inside WSL2 Ubuntu.
- Target: x86_64, run in QEMU with virtio-gpu.
- Stack: Buildroot image, Linux kernel, wlroots (C), compositor forked from TinyWL.
- Pin one wlroots version; only use TinyWL from that same version.
- Scope is fixed by `docs/PLAN.md`. New feature ideas go in the README's "Later" list, not into code.

## Commits

- Small, focused commits. Max writes his own messages for `compositor/` and `shell/` changes.
- Never commit build output (`output/`, images, downloads).
