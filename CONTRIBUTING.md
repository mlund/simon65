# Contributing

Contributions of code, tests, documentation, and bug reports are all welcome.

## Ground rules

- Be respectful and constructive. Assume good faith.
- Keep changes focused: one logical change per pull request.
- Discuss large or invasive changes in an issue before writing the code.

## Licensing and origin

By contributing, you agree that your contributions are licensed under project
licenses.

If you port or adapt code from another project, say so in the file and preserve
the original attribution and license notice. Do not paste code whose license is
unknown or incompatible.

## Development workflow

Before opening a pull request, build, run the `tidy` target, and play the change
on a MEGA65 (or xemu, for what does not depend on timing).

- Format C++ with `clang-format` and Python with `ruff format`; `ruff check` and
  `tidy` must be clean.
- Comments explain *why*, not *what*.
- A format claim cites its source: `engines/agos/<file>:<line>` in ScummVM for the
  game, `iomap.txt` or the core's VHDL for MEGA65 hardware.
- Low byte count is a goal: quote the size change of a full rebuild.
- Never commit game data.

## Using AI coding assistants

Contributions written with the help of AI coding agents (Claude Code, Codex, and
similar) are welcome, **provided a human is in the loop**. If you use one:

- You, the human contributor, are responsible for the change. Read, understand,
  and stand behind every line you submit — the same bar as code you wrote by
  hand.
- Verify it: run the build, `clang-format`, `ruff` and the `tidy` target
  locally, and run the game. "The model said so" is not a substitute for evidence.
- Ensure the agent did not import code, comments, or data of unknown or
  incompatible provenance (see *Licensing and origin* above).

Unreviewed, bulk machine-generated pull requests will be closed.

## Submitting changes

1. Fork and create a topic branch.
2. Make your change with a brief, clear commit message.
3. Open a pull request describing *what* changed and *why*.
