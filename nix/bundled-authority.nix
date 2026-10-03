# The shipped logosctl, laid out as users get it: its bundle is a symlink farm,
# and the bundled capability_module must still be the runtime's token authority.
# The daemon is the shell of a runtime in a process of its own: the token store
# maps only there, each package module into a host of its own, and none of them
# outlives the daemon.
{ pkgs, ctlPkg, modulesDir }:

pkgs.runCommand "logos-logoscore-cli-bundled-authority" {
  # nixpkgs' ps: macOS's /bin/ps is setuid, which a build user may not exec.
  nativeBuildInputs = [ pkgs.jq pkgs.ps ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.lsof ];
} ''
  export QT_QPA_PLATFORM=offscreen
  ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
  ''}
  export HOME=$TMPDIR/home LOGOSCTL_CONFIG_DIR=$TMPDIR/config
  mkdir -p $HOME $LOGOSCTL_CONFIG_DIR/daemon $out
  printf 'version: 2\nmodules_dirs:\n  - "%s"\n' "${modulesDir}" \
    > $LOGOSCTL_CONFIG_DIR/daemon/config.yaml
  ctl=${ctlPkg}/bin/logosctl
  fail() {
    echo "FAIL: $*" >&2
    cat $out/daemon.log >&2
    $ctl daemon stop > /dev/null 2>&1
    exit 1
  }

  # What a process maps, and its children by executable name (Linux's comm
  # stops at 15 characters).
  maps() { if [ -r /proc/$1/maps ]; then cat /proc/$1/maps; else lsof -p $1 -Fn 2>/dev/null; fi; }
  # Read whole, then searched: `maps | grep -q` fails under pipefail when grep
  # stops early and the writer gets EPIPE.
  has() { maps $1 > $TMPDIR/maps || true; grep -q "$2" $TMPDIR/maps; }
  exe() { if [ -e /proc/$1/exe ]; then basename "$(readlink /proc/$1/exe)"; else basename "$(ps -o comm= -p $1)"; fi; }
  children() { ps -A -o pid=,ppid= | awk -v p=$1 '$2 == p { print $1 }'; }
  child() { for c in $(children $1); do [ "$(exe $c)" = "$2" ] && echo $c; done; }
  alive() { s=$(ps -o stat= -p $1 2>/dev/null) && [ -n "$s" ] && [ "''${s#Z}" = "$s" ]; }
  gone() { for i in $(seq 1 100); do still=0; for p in "$@"; do alive $p && still=1; done; [ $still -eq 0 ] && return 0; sleep 0.1; done; return 1; }

  $ctl daemon start > $out/daemon.log 2>&1 < /dev/null &
  for i in $(seq 1 150); do $ctl status > /dev/null 2>&1 && break; sleep 0.2; done
  grep -q "capability_module is the token authority" $out/daemon.log \
    || fail "the bundled capability_module is not the token authority"

  $ctl load-module test_ipc_new_api_module > load.json || fail "load: $(cat load.json)"
  $ctl call test_ipc_new_api_module callBasicEcho bundled_ok > call.json \
    || fail "a forwarded call failed: $(cat call.json)"
  [ "$(jq -r .result call.json)" = bundled_ok ] || fail "unexpected answer: $(cat call.json)"

  $ctl module info modules_state > info.json || fail "module info: $(cat info.json)"
  [ "$(jq -r .placement info.json)" = inproc ] \
    || fail "the bundled modules_state does not run in-process: $(cat info.json)"

  if $ctl call capability_module requestModule x test_basic_module > refused.json; then
    fail "an operator minted a module token: $(cat refused.json)"
  fi
  [ "$(jq -r .error.code refused.json)" = unauthorized ] \
    || fail "not refused as unauthorized: $(cat refused.json)"

  daemon=$(jq -r .pid $LOGOSCTL_CONFIG_DIR/daemon/state.json)
  runtime=$(child $daemon logos_runtime)
  [ -n "$runtime" ] || fail "the daemon spawned no logos_runtime"
  # The probe finding it here is the control for the absences below.
  has $runtime capability_module_plugin || fail "capability_module is not mapped in logos_runtime"
  has $daemon capability_module_plugin && fail "capability_module is mapped in the daemon"
  hosts=$(children $runtime)
  for m in package_manager package_downloader; do
    has $daemon "''${m}_plugin" && fail "$m is mapped in the daemon"
    has $runtime "''${m}_plugin" && fail "$m is mapped in logos_runtime"
    holder=""
    for h in $hosts; do has $h "''${m}_plugin" && holder=$h; done
    [ -n "$holder" ] || fail "$m runs in no host of its own"
    [ "$(exe $holder)" = logos_host_plain ] || fail "$m runs in $(exe $holder)"
  done

  $ctl daemon stop > /dev/null
  wait
  gone $runtime $hosts || fail "logos_runtime or a host outlived daemon stop"

  # SIGKILL on the daemon takes its runtime and every host with it.
  $ctl daemon start > $out/daemon-2.log 2>&1 < /dev/null &
  for i in $(seq 1 150); do $ctl status > /dev/null 2>&1 && break; sleep 0.2; done
  daemon=$(jq -r .pid $LOGOSCTL_CONFIG_DIR/daemon/state.json)
  runtime=$(child $daemon logos_runtime)
  [ -n "$runtime" ] || fail "the second daemon spawned no logos_runtime"
  hosts=$(children $runtime)
  kill -KILL $daemon
  wait 2> /dev/null   # the shell would report the kill as if it were a failure
  gone $runtime $hosts || fail "logos_runtime or a host outlived a killed daemon"
''
