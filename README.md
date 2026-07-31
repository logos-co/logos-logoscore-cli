# logos-logoscore-cli

The headless CLI runtime for the [Logos](https://github.com/logos-co) modular
application platform. It loads Logos modules (Qt plugins) and lets you call
their methods from the command line — no GUI needed.

This repo is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## Two binaries

| | | Documentation |
|---|---|---|
| **`logoscore`** | The tool that exists today. Commands, flags, config format and `~/.logoscore` are **unchanged**. Keep using it. | **[docs/logoscore.md](docs/logoscore.md)** |
| **`logosctl`** | `logoscore` + `lgpd` + `lgpm` merged into one, with package management built in. New surface, own `~/.logosctl` session directory. **Being validated; not yet the default.** | **[docs/logosctl.md](docs/logosctl.md)** |

They share the runtime but no state, so a `logosctl` session cannot disturb a
`logoscore` deployment. They are built from separate flake outputs and released
as separate artifacts. `logoscore` will be removed only once `logosctl` has been
properly validated in real use.

Everything below applies to both. For what each tool actually *does* — commands,
configuration, authentication, transports — follow the links above.

## How to Build

### Using Nix (Recommended)

**The two binaries have separate flake outputs.** `cli` is `logoscore`, `ctl`
is `logosctl`, and each ships only its own binary — so `nix build` with no
target, and anything already pointing at `.#cli`, still gets `logoscore`
exactly as before.

| Output | Binary | |
|---|---|---|
| `.#cli` *(default)* | `logoscore` | dev build |
| `.#cli-bundle-dir` | `logoscore` | portable, self-contained directory |
| `.#cli-appimage` | `logoscore` | portable, single-file AppImage (Linux) |
| `.#ctl` | `logosctl` | dev build |
| `.#ctl-bundle-dir` | `logosctl` | portable, self-contained directory |
| `.#ctl-appimage` | `logosctl` | portable, single-file AppImage (Linux) |

Both come in two flavors: a **dev** build for local iteration and a
**portable** build for distribution. The dev build links against dev
`logos-liblogos` and works with **dev** modules; the portable build is
self-contained and works with **portable** modules — which is what the public
catalog ships, so `logosctl`'s package commands need the portable bundle.

#### Dev Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it. It works with **dev** modules: those produced by a local module `nix build`, or installed by the [package manager](https://github.com/logos-co/logos-package-manager)'s dev build.

```bash
nix build                        # logoscore (dev) — same as '.#cli'
nix build '.#ctl'                # logosctl (dev)
./result/bin/logoscore --help
```

#### Portable Builds

Portable builds are **fully self-contained** — no `/nix/store` references at runtime. They work with **portable** modules: releases from [logos-modules](https://github.com/logos-co/logos-modules), or modules installed by the package manager's portable build.

| Output | Platform | Format |
|---|---|---|
| `cli-bundle-dir` / `ctl-bundle-dir` | Linux, macOS | Self-contained flat directory with `bin/`, `lib/`, and `modules/` |
| `cli-appimage` / `ctl-appimage` | Linux | Single-file `.AppImage` executable |

##### Self-contained directory bundle (all platforms)
```bash
nix build '.#cli-bundle-dir'
./result/bin/logoscore --help
```

##### Linux AppImage (Linux only)
```bash
nix build '.#ctl-appimage'
./result/logosctl.AppImage --help
```

#### Development Shell

```bash
nix develop
```

**Note:** In zsh, quote targets containing `#` to prevent glob expansion (e.g., `'.#cli'`).

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

### Running Tests

Each binary has its own suite, in its own derivation, so nix builds them
concurrently. `tests` runs both.

| Check | Covers |
|---|---|
| `.#checks.<system>.tests` | both — this is what CI runs |
| `.#checks.<system>.tests-logosctl` | unit + CLI + integration for `logosctl` |
| `.#checks.<system>.tests-logoscore` | CLI + integration for `logoscore` |

The `logoscore` suite is a deliberate duplicate of the `logosctl` one, frozen
against `logoscore`'s surface. It exists so that changes to the shared runtime
cannot regress the tool people are actually using, and it gets deleted along
with the binary.

```bash
# Both suites
nix build '.#checks.aarch64-darwin.tests'

# One suite, and its binaries, for local iteration
nix build '.#tests'
./result/bin/cli_tests
./result/bin/cli_tests --gtest_filter=CLITest.*

# Or via nix checks
nix flake check
```

## Dependency Resolution

When loading modules, both binaries automatically resolve and load transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
