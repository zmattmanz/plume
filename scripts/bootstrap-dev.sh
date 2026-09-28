#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_DIR="${ROOT_DIR}/.arduino"
ARDUINO_DATA_DIR="${ARDUINO_DIR}/data"
ARDUINO_DOWNLOADS_DIR="${ARDUINO_DIR}/downloads"
ARDUINO_USER_DIR="${ARDUINO_DIR}/user"
ESP32_INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
ESP32_CORE_VERSION="3.3.10"

export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DATA_DIR}"
export ARDUINO_DIRECTORIES_DOWNLOADS="${ARDUINO_DOWNLOADS_DIR}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_USER_DIR}"

mkdir -p "${ARDUINO_DATA_DIR}" "${ARDUINO_DOWNLOADS_DIR}" "${ARDUINO_USER_DIR}"

if ! command -v arduino-cli >/dev/null 2>&1; then
  if command -v brew >/dev/null 2>&1; then
    echo "[bootstrap] Installing arduino-cli with Homebrew"
    brew install arduino-cli
  else
    echo "[bootstrap] arduino-cli is missing and Homebrew is unavailable." >&2
    echo "[bootstrap] Install arduino-cli, then run this script again." >&2
    exit 1
  fi
fi

echo "[bootstrap] Using $(arduino-cli version)"

if ! arduino-cli core list | awk -v version="${ESP32_CORE_VERSION}" \
    '$1 == "esp32:esp32" && $2 == version { found = 1 } END { exit !found }'; then
  echo "[bootstrap] Installing esp32:esp32@${ESP32_CORE_VERSION}"
  arduino-cli core update-index --additional-urls "${ESP32_INDEX_URL}"
  arduino-cli core install "esp32:esp32@${ESP32_CORE_VERSION}" \
    --additional-urls "${ESP32_INDEX_URL}"
else
  echo "[bootstrap] esp32:esp32@${ESP32_CORE_VERSION} already installed"
fi

ensure_library() {
  local name="$1"
  local version="$2"
  if arduino-cli lib list | awk -v name="${name}" -v version="${version}" \
      '$1 == name && $2 == version { found = 1 } END { exit !found }'; then
    echo "[bootstrap] ${name}@${version} already installed"
  else
    echo "[bootstrap] Installing ${name}@${version}"
    arduino-cli lib install "${name}@${version}"
  fi
}

ensure_library "M5Cardputer" "1.1.1"
ensure_library "M5Unified" "0.2.19"
ensure_library "M5GFX" "0.2.26"
ensure_library "NimBLE-Arduino" "2.5.0"
ensure_library "TinyGPSPlus" "1.0.3"

# Arduino's 5.8-arduino11 ctags bundle is x86_64-only on macOS. Apple Silicon
# cannot execute it without Rosetta. Build the same patched Arduino tag natively
# instead of substituting Universal Ctags, whose prototype output is incompatible
# with this sketch.
CTAGS_OVERRIDE_FILE="${ARDUINO_DIR}/ctags-path"
if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
  BUNDLED_CTAGS="$(find "${ARDUINO_DATA_DIR}/packages" -type f -path '*/tools/ctags/*/ctags' -print -quit 2>/dev/null || true)"
  if [[ -n "${BUNDLED_CTAGS}" ]] && "${BUNDLED_CTAGS}" --version >/dev/null 2>&1; then
    rm -f "${CTAGS_OVERRIDE_FILE}"
  else
    CTAGS_SRC="${ARDUINO_DIR}/tools/ctags-5.8-arduino11"
    if [[ ! -x "${CTAGS_SRC}/ctags" ]]; then
      echo "[bootstrap] Building Arduino ctags 5.8-arduino11 for Apple Silicon"
      if [[ ! -d "${CTAGS_SRC}/.git" ]]; then
        git clone --depth 1 --branch 5.8-arduino11 \
          https://github.com/arduino/ctags.git "${CTAGS_SRC}"
      fi
      if [[ ! -f "${CTAGS_SRC}/.plume-unused-renamed" ]]; then
        # Modern macOS headers define __unused__; Arduino's older ctags sources
        # use the same internal macro name. The rename is mechanical and local.
        perl -pi -e 's/__unused__/CTAGS_UNUSED/g' \
          "${CTAGS_SRC}"/*.c "${CTAGS_SRC}"/*.h
        touch "${CTAGS_SRC}/.plume-unused-renamed"
      fi
      (
        cd "${CTAGS_SRC}"
        ./configure
        make -j"$(sysctl -n hw.logicalcpu)"
      )
    fi
    printf '%s\n' "${CTAGS_SRC}" > "${CTAGS_OVERRIDE_FILE}"
  fi
fi

echo "[bootstrap] Toolchain is ready in ${ARDUINO_DIR}"
