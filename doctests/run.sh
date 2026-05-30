#!/usr/bin/env bash
#
# Execute the logoscore daemon doc-test end-to-end and regenerate its Markdown.
#
# The runner is the shared `doctest` CLI
# (https://github.com/logos-co/logos-doctest), invoked directly via its flake.
# `doctest run` executes every command in a temp directory (building logoscore,
# preparing modules, starting the daemon, calling methods) and asserts on the
# output; `doctest generate` renders the same spec to Markdown under outputs/;
# `doctest clean` strips build artifacts so only the generated docs remain.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

# Run from this doctests/ directory regardless of where the script is invoked from.
cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"
OUTPUT_DIR="./outputs"
SPEC="logoscore-daemon.test.yaml"

echo "==> Clearing previous ${OUTPUT_DIR}/"
rm -rf "${OUTPUT_DIR}"

echo "==> Running ${SPEC} into ${OUTPUT_DIR}/"
"${DOCTEST[@]}" run "${SPEC}" \
  --verbose \
  --continue-on-fail \
  --output-dir "${OUTPUT_DIR}/"

echo "==> Generating ${OUTPUT_DIR}/logoscore-daemon.md"
mkdir -p "${OUTPUT_DIR}"
"${DOCTEST[@]}" generate "${SPEC}" \
  -o "${OUTPUT_DIR}/logoscore-daemon.md"

if [ ! -d "${OUTPUT_DIR}" ]; then
  echo "==> No ${OUTPUT_DIR}/ produced; nothing to clean."
  exit 0
fi

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered doc is in ${OUTPUT_DIR}/logoscore-daemon.md"
