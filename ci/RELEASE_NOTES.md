This release ships **`logosctl`**: `logoscore`, `lgpd` and `lgpm` merged into one binary, with package management built in. Usage is in [docs/logosctl.md](https://github.com/logos-co/logos-logoscore-cli/blob/@COMMIT@/docs/logosctl.md).

`logoscore` binaries are no longer published; earlier releases still carry them. The two share no state: `logosctl` keeps its session in `~/.logosctl` (`%LOCALAPPDATA%\.logosctl` on Windows) and never reads `~/.logoscore`.

## Install

### Linux

The artifact is an [AppImage](https://appimage.org/) — a single self-contained executable. Download it, extract, and drop it on your `PATH` (swap `x86_64` for `aarch64` on ARM):

```bash
wget https://github.com/logos-co/logos-logoscore-cli/releases/download/@TAG@/logosctl-x86_64-linux.tar.gz
tar -xvf logosctl-x86_64-linux.tar.gz
mkdir -p ~/.local/bin
install -m755 logosctl-x86_64.AppImage ~/.local/bin/logosctl
# make sure ~/.local/bin is on your PATH (add this to ~/.bashrc or ~/.zshrc):
export PATH="$HOME/.local/bin:$PATH"
```

Now `logosctl` runs from anywhere. (If you hit a FUSE error, run it as `APPIMAGE_EXTRACT_AND_RUN=1 logosctl`.)

### macOS (Apple Silicon)

The binaries are signed and notarized by Apple.

```bash
curl -fLO https://github.com/logos-co/logos-logoscore-cli/releases/download/@TAG@/logosctl-aarch64-macos.tar.gz
tar -xvf logosctl-aarch64-macos.tar.gz
# move the whole folder somewhere permanent (keep its contents together —
# the binary finds its libraries via ../lib) and put its bin/ on your PATH:
mkdir -p ~/.local
mv logosctl-aarch64-macos ~/.local/logosctl
echo 'export PATH="$HOME/.local/logosctl/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

Now `logosctl` runs from anywhere.

### Windows (x86_64)

> **Download from PowerShell with `curl.exe` — _not_ a web browser.** A browser marks the zip as downloaded from the internet, Explorer's *Extract All* copies that mark onto every file, and Windows may then block these unsigned binaries. If you already used a browser, run `Unblock-File logosctl-x86_64-windows.zip` before extracting.

```powershell
curl.exe -fLO https://github.com/logos-co/logos-logoscore-cli/releases/download/@TAG@/logosctl-x86_64-windows.zip
Expand-Archive logosctl-x86_64-windows.zip -DestinationPath "$env:LOCALAPPDATA\Programs"
# keep the folder together (logosctl.exe finds its DLLs and modules
# relative to itself) and put its bin\ on your user PATH:
$bin = "$env:LOCALAPPDATA\Programs\logosctl-x86_64-windows\bin"
[Environment]::SetEnvironmentVariable('Path', "$bin;" + [Environment]::GetEnvironmentVariable('Path', 'User'), 'User')
$env:Path = "$bin;$env:Path"
```

Now `logosctl` runs from anywhere, in this window and in new ones.