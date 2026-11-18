# Contributing

Thanks for helping. This project recompiles Backbreaker (Xbox 360) for native Linux; the plan and
current status are in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Ground rules

- **Never commit game files or anything derived from them**: ISO images, `default.xex`,
  `default.pe`, the extracted disc (`game/`), recompiled output (`ppc/`), frame captures
  (`captures/`). They are gitignored; keep it that way.
- **Never say where to get the game.** To run anything you need an ISO image made from your own
  retail disc. Do not link to, name or hint at download sources in code, docs, issues or PRs.
- Many tasks need no game files at all (look for the `no game files needed` label).
- **Contributions come in under the project's licence,** the GNU GPL, version 3 or later
  ([`LICENSE`](LICENSE)): opening a pull request means you agree to license your change that way.
  Code from elsewhere needs a licence the GPL-3.0 accepts (BSD, MIT, Apache-2.0, LGPL), and keeps
  its notice, as the Xenia-derived code does.

## Setting up

- Build tools: clang (18+), cmake, ninja, python3, git, openssl. Rendering work also needs Vulkan
  headers/loader, SDL3 and glslang/shaderc development packages.
- Without game files: you can build XenonRecomp with our patch and run the tests (see README,
  "XenonRecomp setup" and "Tests").
- With your own disc image: follow README ("Reproduce extraction", "Recompile") and
  [`docs/runtime.md`](docs/runtime.md) to build and run `build/backbreaker`.

## Workflow

1. Pick an open issue and leave a comment so others know you are on it.
2. Create a branch: `issue-<number>-<short-name>`, e.g. `issue-7-save-system`.
3. Keep commits focused; explain *why* in the message.
4. Open a pull request that says `Fixes #<number>`, and describe how you tested it (paste the
   relevant runtime log lines, test output, or a frame capture summary).
5. If your change moves a phase forward, update `docs/ROADMAP.md` (status table, the phase's
   section, "Current state") in the same PR.
6. **Every change goes through a reviewed pull request - no exceptions, including one-line
   documentation or `.gitignore` changes.** Do not push to `main`, and do not merge a pull
   request yourself: the maintainer (or their review agent) approves and merges. GitHub can't
   enforce this on this private repo, so it's on all of us.

## Working with Claude Code (or another agent)

Issues are written so an agent can work from them:

```
gh issue view <number> --comments
```

Give the agent the issue plus `docs/ROADMAP.md` and `docs/runtime.md`, and remind it of the ground
rules above. Review its diff yourself before opening the PR, and make sure it ran the tests or
the game as the issue's "Done when" list asks.

Rules for agents:
- Never push to `main` and never merge a pull request (`gh pr merge`), even your own small ones.
  Open the PR and stop; the maintainer reviews and merges.
- For every factual claim in a comment, test or PR description ("matches Xenia", "the ISA
  defines...", "verified over N cases"), include the command or source that shows it, so a
  reviewer can re-run it. State what you did not verify.

## Code conventions

- C++20, compiled with clang. Match the style of the surrounding code.
- Kernel/XAM imports: put the guest signature in a comment above each `GUEST_IMPORT(...)`, and
  cite where layouts or behavior come from (usually a Xenia source file). Do not guess structure
  layouts: verify them against a reference.
- Guest memory is big-endian: use `mem::read32` / `mem::write32` etc. (see `runtime/memory.h`).
- Python tools run with `python3 -I` and take paths as arguments.
- Code derived from Xenia (BSD 3-clause) keeps its attribution; the license text is in
  `docs/licenses/xenia-LICENSE.txt`.

## Testing

- Instruction semantics: `tests/instr_semantics.cpp` (see README, "Tests").
- Runtime: `./build/backbreaker --timeout 60` prints a status line every second and, on a crash or
  timeout, each thread's recent kernel calls and profiler scopes.
- Scripted input: `--press "a@20,a@26"`. GPU frame capture: `--capture-frame 1200`.
  Both are described in `docs/runtime.md`.
