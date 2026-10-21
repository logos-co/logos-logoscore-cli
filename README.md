# logos-logoscore-cli

The headless CLI runtime for the [Logos](https://github.com/logos-co) modular
application platform. It loads Logos modules — Qt plugins and Qt-free
`universal` / `cdylib` ones alike — and lets you call their methods from the
command line, no GUI needed.

This repo is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## logosctl

The binary this repo builds is **`logosctl`**: the module runtime, its client,
and package management (`lgpd` + `lgpm`) in one tool, with its session in
`~/.logosctl`. Commands, configuration, authentication and transports are in
**[docs/logosctl.md](docs/logosctl.md)**.

It replaces `logoscore`, the CLI this repo used to ship alongside it. Earlier
releases still carry `logoscore` binaries; this tree no longer builds them.

## How to Build

### Using Nix (Recommended)

| Output | |
|---|---|
| `.#ctl` *(default)* | dev build |
| `.#ctl-bundle-dir` | portable, self-contained directory |
| `.#ctl-appimage` | portable, single-file AppImage (Linux) |

It comes in two flavors: a **dev** build for local iteration and a
**portable** build for distribution. The dev build links against dev
`logos-liblogos` and works with **dev** modules; the portable build is
self-contained and works with **portable** modules — which is what the public
catalog ships, so installing *from the catalog* needs the portable bundle.
Installing a locally built `.lgx` is the mirror image: a nix module build
produces a `-dev` variant, so the dev build is the one that takes it. See
[installing a local .lgx](doctests/logosctl-local-install.test.yaml).

#### Dev Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it. It works with **dev** modules: those produced by a local module `nix build`, or installed by the [package manager](https://github.com/logos-co/logos-package-manager)'s dev build.

```bash
nix build                        # logosctl (dev) — same as '.#ctl'
./result/bin/logosctl --help
```

#### Portable Builds

Portable builds are **fully self-contained** — no `/nix/store` references at runtime. They work with **portable** modules: releases from [logos-modules](https://github.com/logos-co/logos-modules), or modules installed by the package manager's portable build.

| Output | Platform | Format |
|---|---|---|
| `ctl-bundle-dir` | Linux, macOS | Self-contained flat directory with `bin/`, `lib/`, and `modules/` |
| `ctl-appimage` | Linux | Single-file `.AppImage` executable |

##### Self-contained directory bundle (all platforms)
```bash
nix build '.#ctl-bundle-dir'
./result/bin/logosctl --help
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

**Note:** In zsh, quote targets containing `#` to prevent glob expansion (e.g., `'.#ctl'`).

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

### Running Tests

`.#checks.<system>.tests` runs the unit and CLI suites; it is what CI runs.
The daemon-backed integration suite runs in
[logos-test-modules](https://github.com/logos-co/logos-test-modules), which
owns the plugins it loads.

```bash
nix build '.#checks.aarch64-darwin.tests'

# The suites and their binaries, for local iteration
nix build '.#tests'
./result/bin/cli_tests
./result/bin/cli_tests --gtest_filter=CLITest.*

# Or via nix checks
nix flake check
```

## Dependency Resolution

When loading modules, `logosctl` automatically resolves and load transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
