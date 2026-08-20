# The one-runtime symbol gate.
#
# INVARIANT: across the images that share ONE process, the logos C++ runtime is
# DEFINED exactly once — in liblogos_core, the single provider. A second
# definition is a second TokenManager, and the split-brain that follows is
# invisible to the build. See logos-protocol/cpp/logos_shared_api.h and
# cmake/LogosSharedFromDll.cmake.
#
# This repo learned that the loud way and then the quiet way. On Windows a
# duplicate fails the LINK outright ("multiple definition of
# `TokenManager::instance()'"), and that loudness is exactly what hid the other
# platforms: off Windows the duplicate links cleanly, the image binds its own
# copy, and it surfaces at runtime as refused calls. Nothing measured it until
# this check existed.
#
# THE IN-PROCESS IMAGE SET — this scoping IS the correctness of the gate:
#   IN   bin/logosctl, bin/logoscore   the front-ends
#   IN   lib/liblogos_core.*           the single provider
#   IN   lib/*.dylib|so                anything else loaded into the front-end
#   OUT  bin/logos_host, bin/ui-host   SEPARATE PROCESSES; they correctly keep
#                                      their own statics
#   OUT  modules/**                    loaded by logos_host, out-of-process, so
#                                      a module's own copy is the CORRECT
#                                      per-process singleton
#
# Kept as a sibling of logos-basecamp/nix/symbol-gate.nix rather than shared,
# for the same reason cmake/LogosSharedFromDll.cmake is duplicated: neither repo
# depends on the other. The image sets differ (no plugins/ here), so this is not
# a copy that could simply be included.
#
# negativeControl = true runs the identical script against a tree with a REAL
# duplicate planted where a consumer goes, and asserts the gate REJECTS it.
# Without that, an absence assertion is indistinguishable from a broken one.
{ pkgs, appPkg, negativeControl ? false }:

let
  isDarwin = pkgs.stdenv.isDarwin;
  definedCmd = if isDarwin then "nm -gU" else "nm -D --defined-only";
  totalCmd   = if isDarwin then "nm -a" else "nm -D";
in
pkgs.runCommand "logos-logoscore-cli-symbol-gate${pkgs.lib.optionalString negativeControl "-negative"}" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.findutils pkgs.gnugrep pkgs.gnused pkgs.stdenv.cc.bintools ];
} ''
  set -uo pipefail

  # TIER 1 — the split-brain itself. No allowance, ever.
  TIER1_RE='^(TokenManager|StoreRegistry)::|^(vtable|typeinfo|typeinfo name|guard variable) for (TokenManager|StoreRegistry)\b'
  # TIER 2 — LogosAPI/LogosAPIClient. Zero since the single-provider shim was
  # extended to every platform. Never raise this.
  TIER2_RE='^(LogosAPI|LogosAPIClient)::|^(vtable|typeinfo|typeinfo name|guard variable) for (LogosAPI|LogosAPIClient)\b'
  TIER2_ALLOW=0

  FAIL=0
  note() { printf '  %-56s %s\n' "$1" "$2"; }
  bad()  { FAIL=1; printf '  %-56s %s\n' "$1" "$2"; }

  # nix wraps binaries TWO ways and both defeat a naive measurement:
  #   * a shell wrapper that execs bin/.<name>       -- nm reads ZERO symbols
  #   * makeBinaryWrapper's COMPILED stub, real image at bin/.<name>-wrapped
  #     -- nm reads ~10 symbols, so the validity guard below does NOT catch it.
  # bin/logoscore in this repo is the SECOND kind: the stub reports 0 runtime
  # symbols for a binary that actually imports 12. So the rule is deterministic:
  # if a hidden sibling exists, it IS the image.
  resolve_image() {
    local f="$1" d b real
    d=$(dirname "$f"); b=$(basename "$f")
    for cand in "$d/.$b-wrapped" "$d/.$b"; do
      [ -e "$cand" ] && { printf '%s\n' "$cand"; return; }
    done
    if head -c2 "$f" 2>/dev/null | grep -q '#!'; then
      real=$(sed -nE 's/^exec "\$BINDIR\/([^"]+)".*/\1/p' "$f" | tail -1)
      [ -n "$real" ] && [ -e "$d/$real" ] && { printf '%s\n' "$d/$real"; return; }
    fi
    printf '%s\n' "$f"
  }
  names() { ${definedCmd} "$1" 2>/dev/null | c++filt 2>/dev/null | sed -E 's/^[0-9a-fA-F]+ [A-Za-z] //'; }
  valid() {
    local t; t=$(${totalCmd} "$1" 2>/dev/null | wc -l | tr -d ' ')
    [ "''${t:-0}" -gt 0 ] || { bad "$(basename "$1")" "ERROR: nm read 0 symbols — vacuous"; return 1; }
  }

  ROOT=$TMPDIR/bundle
  mkdir -p "$ROOT"
  cp -R ${appPkg}/. "$ROOT"/ 2>/dev/null || true
  chmod -R u+w "$ROOT"

  PROVIDER=""
  for c in "$ROOT/lib/liblogos_core.dylib" "$ROOT/lib/liblogos_core.so" "$ROOT/bin/liblogos_core.dll"; do
    [ -e "$c" ] && PROVIDER="$c" && break
  done
  [ -n "$PROVIDER" ] || { echo "FATAL: no liblogos_core under the bundle"; exit 1; }

  ${pkgs.lib.optionalString negativeControl ''
    mkdir -p "$ROOT/lib"
    cp "$PROVIDER" "$ROOT/lib/libnegative_control''${PROVIDER##*liblogos_core}"
    echo "NEGATIVE CONTROL: planted a duplicate runtime; the gate MUST reject this tree."
  ''}

  # -L on every find: a nix output stages its libs as SYMLINKS into the store,
  # and `find -type f` does NOT match a symlink. Measured here: `find` returned
  # 0 entries for a lib/ holding two real libraries, so libpackage_manager_lib
  # was silently skipped as a consumer. `[ -f ]` below is fine -- test(1)
  # follows symlinks -- but find does not, and the difference is another
  # vacuous-absence trap.
  CONSUMERS=()
  for e in "$ROOT"/bin/*; do
    [ -f "$e" ] || continue
    case "$(basename "$e")" in
      logos_host|ui-host|logos_host_qt|*.dll) continue ;;   # separate processes
      .*) continue ;;                                       # reached via resolve_image
    esac
    CONSUMERS+=("$(resolve_image "$e")")
  done
  while IFS= read -r p; do
    [ -n "$p" ] && [ "$p" != "$PROVIDER" ] && CONSUMERS+=("$p")
  done < <(find -L "$ROOT/lib" -maxdepth 1 -type f \( -name '*.dylib' -o -name '*.so' \) 2>/dev/null | grep -v 'liblogos_core' || true)

  echo "provider  = ''${PROVIDER#$ROOT/}"
  printf 'consumers = '; for c in "''${CONSUMERS[@]}"; do printf '%s ' "''${c#$ROOT/}"; done; echo

  # Positive control AND demangler validity check in one: if c++filt were absent
  # or broken this reports 0 and the gate FAILS rather than passing vacuously.
  echo
  echo "== the provider DEFINES the runtime (expect >=1) =="
  valid "$PROVIDER" || exit 1
  n=$(names "$PROVIDER" | grep -Ec 'TokenManager::instance' || true)
  if [ "$n" -ge 1 ]; then note "liblogos_core  TokenManager::instance" "$n  OK"
  else bad "liblogos_core  TokenManager::instance" "$n  EXPECTED >=1 (or demangling is broken)"; fi

  echo
  echo "== TIER 1: no in-process consumer defines TokenManager/StoreRegistry (expect 0) =="
  for img in "''${CONSUMERS[@]}"; do
    valid "$img" || continue
    n=$(names "$img" | grep -Ec "$TIER1_RE" || true)
    if [ "$n" -eq 0 ]; then note "$(basename "$img")" "0  OK"
    else bad "$(basename "$img")" "$n  SPLIT-BRAIN"; names "$img" | grep -E "$TIER1_RE" | sed 's/^/      /' | head -6; fi
  done

  echo
  echo "== TIER 2: LogosAPI/LogosAPIClient duplication (expect <=$TIER2_ALLOW) =="
  T2=0
  for img in "''${CONSUMERS[@]}"; do
    n=$(names "$img" | grep -Ec "$TIER2_RE" || true); T2=$((T2 + n)); note "$(basename "$img")" "$n"
  done
  if [ "$T2" -le "$TIER2_ALLOW" ]; then note "tier-2 total" "$T2  OK"
  else bad "tier-2 total" "$T2  > $TIER2_ALLOW  REGRESSION"; fi

  echo
  ${if negativeControl then ''
    if [ "$FAIL" -ne 0 ]; then
      echo "NEGATIVE CONTROL: PASS — the gate correctly rejected a planted duplicate."
      mkdir -p $out; echo ok > $out/result; exit 0
    else
      echo "NEGATIVE CONTROL: FAIL — the gate ACCEPTED a planted duplicate. It is vacuous."
      exit 1
    fi
  '' else ''
    if [ "$FAIL" -eq 0 ]; then
      echo "SYMBOL GATE: PASS"; mkdir -p $out; echo ok > $out/result; exit 0
    else
      echo "SYMBOL GATE: FAIL — see logos-protocol/cpp/logos_shared_api.h"; exit 1
    fi
  ''}
''
