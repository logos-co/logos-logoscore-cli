# The Qt-free front-end boundary gate.
#
# INVARIANTS:
#   * lp_client_create has exactly one definer in the process: the shared
#     liblogos_protocol_plain runtime.
#   * front-end consumers do not contain private lp_* implementations.
#   * Qt host APIs and Qt/full-protocol dynamic dependencies stay out of the
#     logoscore/logosctl process. The child logos_host_qt process is excluded.
#
# THE IN-PROCESS IMAGE SET — this scoping IS the correctness of the gate:
#   IN   bin/logosctl, bin/logoscore   the front-ends
#   IN   lib/liblogos_core.*           a plain-runtime consumer
#   IN   lib/*.dylib|so                anything else loaded into the front-end
#   OUT  bin/logos_host* and ui-host   separate processes that may load Qt
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
  depsCmd = if isDarwin then "${pkgs.darwin.cctools}/bin/otool -L"
            else "${tp}objdump -p";
in
pkgs.runCommand "logos-logoscore-cli-symbol-gate${pkgs.lib.optionalString negativeControl "-negative"}" {
  nativeBuildInputs = [ pkgs.coreutils pkgs.findutils pkgs.gnugrep pkgs.gnused pkgs.stdenv.cc.bintools ];
} ''
  set -uo pipefail
  export LC_ALL=C   # comm(1) in names() requires a byte-order sort

  # TIER 1 — no consumer may contain a private copy of the lp_* runtime.
  TIER1_RE='^_?lp_'
  # TIER 2 — the Qt host API must stay out of this process entirely.
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
  # An ELF symbol version (lp_client_create@@LOGOS_PROTOCOL_PLAIN) is not part of the name.
  names() { ${definedCmd} "$1" 2>/dev/null | ${tp}c++filt 2>/dev/null | sed -E 's/^[0-9a-fA-F]+ [A-Za-z] //; s/@.*$//'; }
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
    # Plant the actual Qt-free runtime owner. A negative control has to
    # duplicate a DEFINER rather than another consumer.
    _definer=""
    for c in "$ROOT/lib/liblogos_protocol_plain.dylib" "$ROOT/lib/liblogos_protocol_plain.so" "$ROOT/bin/liblogos_protocol_plain.dll"; do
      [ -e "$c" ] && _definer="$c" && break
    done
    [ -n "$_definer" ] || { echo "NEGATIVE CONTROL: no liblogos_protocol_plain to plant"; exit 1; }
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
      logos_host*|ui-host|*.dll) continue ;;                 # separate processes
      .*) continue ;;                                       # reached via resolve_image
    esac
    CONSUMERS+=("$(resolve_image "$e")")
  done
  # The shared plain runtime is the owner, so exclude it from the consumer scan
  # and include it in the exactly-one-definer scan below.
  OWNERS=()
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    case "$(basename "$p")" in
      liblogos_protocol_plain.*) OWNERS+=("$p") ;;
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

  echo
  echo "== the plain C ABI is defined by EXACTLY ONE image =="
  valid "$PROVIDER" || exit 1
  for fam in lp_client_create; do
    _n=0; _owners=""
    for img in "''${ALL[@]}"; do
      [ -e "$img" ] || continue
      c=$(names "$img" | grep -cE "^_?''${fam}$" || true)
      if [ "$c" -gt 0 ]; then _n=$((_n + 1)); _owners="$_owners $(basename "$img")($c)"; fi
    done
    if [ "$_n" -eq 1 ]; then note "$fam" "1 definer:$_owners  OK"
    else bad "$fam" "$_n definers:$_owners  EXPECTED exactly 1"; fi
  done

  echo
  echo "== TIER 1: no in-process consumer defines lp_* (expect 0) =="
  for img in "''${CONSUMERS[@]}"; do
    valid "$img" || continue
    n=$(names "$img" | grep -Ec "$TIER1_RE" || true)
    if [ "$n" -eq 0 ]; then note "$(basename "$img")" "0  OK"
    else bad "$(basename "$img")" "$n  SPLIT-BRAIN"; names "$img" | grep -E "$TIER1_RE" | sed 's/^/      /' | head -6; fi
  done

  echo
  echo "== TIER 2: no Qt host API in the front-end process (expect <=$TIER2_ALLOW) =="
  T2=0
  for img in "''${CONSUMERS[@]}"; do
    n=$(names "$img" | grep -Ec "$TIER2_RE" || true); T2=$((T2 + n)); note "$(basename "$img")" "$n"
  done
  if [ "$T2" -le "$TIER2_ALLOW" ]; then note "tier-2 total" "$T2  OK"
  else bad "tier-2 total" "$T2  > $TIER2_ALLOW  REGRESSION"; fi

  echo
  echo "== no Qt/full protocol dependency in front-end images =="
  for img in "''${CONSUMERS[@]}"; do
    deps=$(${depsCmd} "$img" 2>/dev/null || true)
    if printf '%s\n' "$deps" \
        | grep -Eiq 'Qt[0-9]?(Core|RemoteObjects)|logos_qt_host|liblogos_protocol\.(dylib|so|dll)'; then
      bad "$(basename "$img")" "imports Qt or the full Qt protocol"
    else
      note "$(basename "$img")" "Qt-free  OK"
    fi
  done

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
