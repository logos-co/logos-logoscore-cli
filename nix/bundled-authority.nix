# The shipped logosctl, laid out as users get it: its bundle is a symlink farm,
# and the bundled capability_module must still be the runtime's token authority.
{ pkgs, ctlPkg, modulesDir }:

pkgs.runCommand "logos-logoscore-cli-bundled-authority" {
  nativeBuildInputs = [ pkgs.jq ];
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

  $ctl daemon stop > /dev/null
  wait
''
