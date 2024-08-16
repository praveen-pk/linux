#!/usr/bin/env bash
set -x
set -e

MSHV_DIR="${1:-/mshv}"
LINUX_DOM0_DIR="${2:-/linux_dom0}"

tdnf update
tdnf install clang-devel rsync curl kernel-headers git build-essential python3 -y
curl https://sh.rustup.rs -sSf | sh -s -- -y

CARGO_ENV_PATH="/root/.cargo/env"
# shellcheck source=/dev/null
. "${CARGO_ENV_PATH}"

cd ${MSHV_DIR}
cargo install bindgen-cli
./scripts/generate_binding.py --kernel "${LINUX_DOM0_DIR}" --log-level debug --bindgen "--with-derive-eq --with-derive-ord"
ret=$?

if [ $ret -ne 0 ]; then
  echo "Binding generation failed"
  exit $ret
fi
