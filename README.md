# logos-logoscore-cli

`logoscore` is the headless CLI runtime for the [Logos](https://github.com/logos-co) modular application platform. It loads Logos modules (Qt plugins) and lets you call their methods from the command line — no GUI needed.

`logos-logoscore-cli` is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## How to Build

### Using Nix (Recommended)

```bash
# Build logoscore binary
nix build

# Build and run tests
nix build '.#tests'

# Build portable directory bundle
nix build '.#cli-bundle-dir'
```

The result binary is at `result/bin/logoscore`.

### Running Tests

```bash
# Build and run via nix
nix build '.#tests'
./result/bin/cli_tests

# Run specific tests
./result/bin/cli_tests --gtest_filter=CLITest.*

# Or via nix checks
nix flake check
```

**Note:** In zsh, quote targets with `#` to prevent glob expansion:
```bash
nix build '.#logos-logoscore-cli'
```

## Usage

### Command Line Options

```
logoscore [options]

Options:
  -m, --modules-dir <path>    Directory to scan for modules (repeatable)
  -l, --load-modules <modules> Comma-separated list of modules to load
  -c, --call <call>           Call a module method: module.method(arg1, arg2)
                              Use @file to read a parameter from a file.
                              Can be repeated for sequential calls.
      --quit-on-finish        Exit after all -c calls complete
  -h, --help                  Show help
      --version               Show version
```

### Method Call Syntax

```
module_name.method_name(arg1, arg2, ...)
```

- **Type auto-detection:** `true`/`false` → bool, `42` → int, `3.14` → double, else → string
- **File parameters:** `@filename` loads the file content as the argument
- **30-second timeout** per call; exit code 1 on failure

### Examples

```bash
# Run with default modules directory
logoscore

# Specify a custom modules directory
logoscore --modules-dir /path/to/modules
logoscore -m /path/to/modules

# Load specific modules (deps resolved automatically)
logoscore --load-modules module1,module2,module3
logoscore -l module1,module2,module3

# Load modules and call a method
logoscore -m ./modules -l my_module -c "my_module.start()"

# Call with parameters
logoscore -l storage -c "storage.init('config', 42, true)"

# Read a parameter from a file
logoscore -l storage -c "storage.loadConfig(@config.json)"

# Multiple sequential calls (abort on first error)
logoscore -l storage \
  -c "storage.init(@config.json)" \
  -c "storage.start()"

# Exit after calls finish (useful in scripts)
logoscore -m ./modules -l my_module \
  -c "my_module.doWork(hello)" \
  --quit-on-finish

# Multiple modules directory sources
logoscore -m ./core-modules -m ./extra-modules -l my_module
```

### Dependency Resolution

When using `--load-modules`, `logoscore` automatically resolves and loads transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
