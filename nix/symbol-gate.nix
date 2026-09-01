# The one-runtime symbol gate.
#
# INVARIANT: across the images that share ONE process, each runtime type is
# DEFINED by exactly ONE image.
#
# EXACTLY-ONE, deliberately, rather than "liblogos_core is the provider". It was
# the latter while liblogos_core absorbed both static archives and re-exported
# them. It is not any more: liblogos_protocol owns TokenManager/LogosAPIClient
# and liblogos_qt_host owns LogosAPI, and liblogos_core imports both like every
# other consumer. Naming an owner here would need editing every time ownership
# moves -- and the first time it moved, that check failed while every real
# assertion still passed. A second
# definition is a second TokenManager, and the split-brain that follows is
# invisible to the build. Ownership is declared in
# logos-protocol/cpp/logos_shared_api.h. (This used to also point at
# cmake/LogosSharedFromDll.cmake -- the single-provider shim, deleted once the
# runtime became real shared libraries. There is nothing to follow there.)
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
# because neither repo depends on the other. The image sets differ (no plugins/ here), so this is not
# a copy that could simply be included.
#
# negativeControl = true runs the identical script against a tree with a REAL
# duplicate planted where a consumer goes, and asserts the gate REJECTS it.
# Without that, an absence assertion is indistinguishable from a broken one.
{ pkgs, appPkg, negativeControl ? false }:

let
  isDarwin  = pkgs.stdenv.isDarwin;
  isWindows = pkgs.stdenv.hostPlatform.isWindows;
  # "" natively, "x86_64-w64-mingw32-" for the Windows cross. The cross bintools
  # installs ONLY the prefixed names, so a bare `nm` / `c++filt` is not on PATH
  # in that derivation -- every measurement produced nothing and valid() below
  # refused to assert over it. Fail-closed, but the gate could never run.
  tp = pkgs.stdenv.cc.targetPrefix;
  # Mach-O: -gU is defined externals. ELF: -D --defined-only. A PE has no ELF
  # dynamic symbol table, so -D reads NOTHING from a .dll -- measured: 0 lines
  # against a real mingw PE where plain nm reads 687.
  definedCmd = if isDarwin then "${tp}nm -gU"
               else if isWindows then "${tp}nm --defined-only"
               else "${tp}nm -D --defined-only";
  totalCmd   = if isDarwin then "${tp}nm -a"
               else if isWindows then "${tp}nm"
               else "${tp}nm -D";
in
pkgs.runCommand "logos-logoscore-cli-symbol-gate${pkgs.lib.optionalString negativeControl "-negative"}" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.findutils pkgs.gnugrep pkgs.gnused pkgs.stdenv.cc.bintools ];
} ''
  set -uo pipefail
  export LC_ALL=C   # comm(1) in names() requires a byte-order sort

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
  ${if isWindows then ''
  # PE reports an import THUNK as a defined text symbol: for every imported
  # function ld synthesizes a .text stub AND an __imp_<mangled> slot in the
  # import address table, and `nm --defined-only` shows the stub as `T`. Counting
  # that alone reports images as DEFINERS of types they merely import. The paired
  # __imp_ entry is the discriminator, and it is the right one -- a genuine
  # second copy statically linked in has no __imp_ slot and still counts. (The
  # PE export table would also hide the phantom, but it hides a real private copy
  # too, trading a false positive for a false NEGATIVE.)
  names() {
    local t; t=$(mktemp -d)
    ${definedCmd} "$1" 2>/dev/null | awk '{print $3}' | grep -v '^$' | sort -u > "$t/all"
    grep '^__imp_' "$t/all" | sed 's/^__imp_//' | sort -u > "$t/imp"
    grep -v '^__imp_' "$t/all" | sort -u > "$t/def"
    comm -23 "$t/def" "$t/imp" | ${tp}c++filt 2>/dev/null
    rm -rf "$t"
  }
  '' else ''
  names() { ${definedCmd} "$1" 2>/dev/null | ${tp}c++filt 2>/dev/null | sed -E 's/^[0-9a-fA-F]+ [A-Za-z] //'; }
  ''}
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
    # Plants liblogos_protocol, NOT liblogos_core. liblogos_core was a genuine
    # second copy of the runtime while it absorbed both archives; since it began
    # IMPORTING instead it defines zero runtime symbols, so planting it plants
    # NOTHING and this control silently stops testing anything. A negative
    # control has to duplicate a DEFINER.
    _definer=""
    for c in "$ROOT/lib/liblogos_protocol.dylib" "$ROOT/lib/liblogos_protocol.so" "$ROOT/bin/liblogos_protocol.dll"; do
      [ -e "$c" ] && _definer="$c" && break
    done
    [ -n "$_definer" ] || { echo "NEGATIVE CONTROL: no liblogos_protocol to plant"; exit 1; }
    cp "$_definer" "$ROOT/lib/libnegative_control.''${_definer##*.}"
    echo "NEGATIVE CONTROL: planted a duplicate definer; the gate MUST reject this tree."
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
  # The runtime OWNERS are not consumers. liblogos_protocol owns TokenManager
  # and LogosAPIClient; liblogos_qt_host owns LogosAPI. They must DEFINE those,
  # so scanning them under "no consumer defines the runtime" reports the correct
  # answer to the wrong question -- measured here as a 35-symbol SPLIT-BRAIN
  # against liblogos_protocol, which is simply the library doing its job.
  #
  # They are counted in ALL below, where exactly-one-definer is asserted, and
  # excluded here, where the assertion is that nothing ELSE defines it.
  OWNERS=()
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    case "$(basename "$p")" in
      liblogos_protocol.*|liblogos_qt_host.*) OWNERS+=("$p") ;;
      *) CONSUMERS+=("$p") ;;
    esac
  # bin/ is searched too, and .dll matched: on Windows every liblogos_* shared
  # image is staged into bin/, NOT lib/, because the PE loader searches the
  # executable's directory. Globbing lib/*.{dylib,so} alone found ZERO owners
  # there, so the exactly-one assertion could not pass on a correct tree.
  done < <(find -L "$ROOT/lib" "$ROOT/bin" -maxdepth 1 -type f \
    \( -name '*.dylib' -o -name '*.so' -o -name 'liblogos_*.dll' \) 2>/dev/null \
    | grep -v 'liblogos_core' || true)

  echo "provider  = ''${PROVIDER#$ROOT/}"
  printf 'consumers = '; for c in "''${CONSUMERS[@]}"; do printf '%s ' "''${c#$ROOT/}"; done; echo

  # Positive control AND demangler validity check in one: if c++filt were absent
  # or broken this reports 0 and the gate FAILS rather than passing vacuously.
  # Every image sharing the process, providers included: ownership is what is
  # being asserted, so nothing may be exempt from the count.
  ALL=("$PROVIDER" "''${CONSUMERS[@]}" "''${OWNERS[@]}")

  # StoreRegistry is deliberately absent below. token_manager.cpp defines
  # `static StoreRegistry r;` inside registry(), so it has a LOCAL symbol and no
  # external one, and is reachable only through TokenManager's accessors.
  # Requiring exactly one DEFINER of something never exported would fail
  # forever. It stays in the TIER 1 scan, where "no consumer defines it" is
  # meaningful precisely because it should never become external.
  #
  # This also doubles as the demangler validity control: a broken c++filt makes
  # every family report ZERO definers and fail, rather than every consumer
  # reporting a reassuring zero.
  echo
  echo "== each runtime type is defined by EXACTLY ONE image =="
  valid "$PROVIDER" || exit 1
  for fam in TokenManager LogosAPI LogosAPIClient; do
    _n=0; _owners=""
    for img in "''${ALL[@]}"; do
      [ -e "$img" ] || continue
      c=$(names "$img" | grep -cE "^''${fam}::|^(vtable|typeinfo|typeinfo name|guard variable) for ''${fam}\b" || true)
      if [ "$c" -gt 0 ]; then _n=$((_n + 1)); _owners="$_owners $(basename "$img")($c)"; fi
    done
    if [ "$_n" -eq 1 ]; then note "$fam" "1 definer:$_owners  OK"
    else bad "$fam" "$_n definers:$_owners  EXPECTED exactly 1"; fi
  done

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
