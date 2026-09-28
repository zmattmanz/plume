#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_DIR="${ROOT_DIR}/.arduino"
BUILD_DIR="${ROOT_DIR}/.build/c5"
SKETCH_DIR="${ROOT_DIR}/.build/sketch/c5-sniffer"
SOURCE_FILE="${ROOT_DIR}/c5-sniffer/c5-sniffer.ino"
FQBN="esp32:esp32:esp32c5:CDCOnBoot=cdc"

if [[ "${PLUME_SKIP_BOOTSTRAP:-0}" != "1" ]]; then
  "${ROOT_DIR}/scripts/bootstrap-dev.sh"
fi

export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIR}/data"
export ARDUINO_DIRECTORIES_DOWNLOADS="${ARDUINO_DIR}/downloads"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIR}/user"

mkdir -p "${SKETCH_DIR}" "${BUILD_DIR}/work" "${BUILD_DIR}/output"
cp "${SOURCE_FILE}" "${SKETCH_DIR}/c5-sniffer.ino"

COMPILE_ARGS=(
  compile
  --fqbn "${FQBN}"
  --build-path "${BUILD_DIR}/work"
  --output-dir "${BUILD_DIR}/output"
  --warnings all
)

if [[ -f "${ARDUINO_DIR}/ctags-path" ]]; then
  CTAGS_PATH="$(sed -n '1p' "${ARDUINO_DIR}/ctags-path")"
  if [[ ! -x "${CTAGS_PATH}/ctags" ]]; then
    echo "[build] Native Arduino ctags override is missing: ${CTAGS_PATH}/ctags" >&2
    exit 1
  fi
  COMPILE_ARGS+=(--build-property "runtime.tools.ctags.path=${CTAGS_PATH}")
fi

echo "[build] FQBN: ${FQBN}"
echo "[build] Source: ${SOURCE_FILE}"
echo "[build] Output: ${BUILD_DIR}/output"
arduino-cli "${COMPILE_ARGS[@]}" "${SKETCH_DIR}"

echo
echo "[build] C5 build complete. No device was flashed."
