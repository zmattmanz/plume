#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CARDPUTER="${ROOT_DIR}/FlockDetection_Cardputer_ADV.ino"
C5="${ROOT_DIR}/c5-sniffer/c5-sniffer.ino"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

extract_cardputer_ouis() {
  perl -0777 -ne '
    if (/static const OuiEntry mac_prefixes\[\] = \{(.*?)\n\};/s) {
      my $block = $1;
      while ($block =~ /\{"([0-9a-f:]{8})",\s*OUI_(SPECIFIC|GENERIC)\}/g) {
        print "$1 ", ($2 eq "SPECIFIC" ? 1 : 2), "\n";
      }
    }
  ' "${CARDPUTER}"
}

extract_c5_ouis() {
  perl -0777 -ne '
    if (/static const char\* kMacTier1\[\] = \{(.*?)\};/s) {
      my $block = $1;
      while ($block =~ /"([0-9a-f:]{8})"/g) { print "$1 1\n"; }
    }
    if (/static const char\* kMacTier2\[\] = \{(.*?)\};/s) {
      my $block = $1;
      while ($block =~ /"([0-9a-f:]{8})"/g) { print "$1 2\n"; }
    }
  ' "${C5}"
}

extract_cardputer_ouis > "${TMP_DIR}/cardputer-ouis"
extract_c5_ouis > "${TMP_DIR}/c5-ouis"

if ! diff -u "${TMP_DIR}/cardputer-ouis" "${TMP_DIR}/c5-ouis"; then
  echo "Signature OUI tables differ between Cardputer and C5." >&2
  exit 1
fi

cardputer_threshold="$(awk '/^#define CONFIDENCE_ALARM_THRESHOLD / { print $3; exit }' "${CARDPUTER}")"
c5_threshold="$(awk '/^#define ALARM_THRESHOLD / { print $3; exit }' "${C5}")"
if [[ -z "${cardputer_threshold}" || "${cardputer_threshold}" != "${c5_threshold}" ]]; then
  echo "Alarm thresholds differ: Cardputer=${cardputer_threshold:-missing} C5=${c5_threshold:-missing}" >&2
  exit 1
fi

cardputer_oui_max="$(awk '/^#define MAX_OUI_RT / { print $3; exit }' "${CARDPUTER}")"
c5_oui_max="$(awk '/^#define RT_OUI_MAX / { print $3; exit }' "${C5}")"
if (( c5_oui_max < cardputer_oui_max )); then
  echo "C5 OUI capacity ${c5_oui_max} is below Cardputer capacity ${cardputer_oui_max}." >&2
  exit 1
fi

if grep -qi '^00:25:df ' "${TMP_DIR}/cardputer-ouis"; then
  echo "Axon's OUI must not enter the Flock signature table." >&2
  exit 1
fi

grep -q '^#define AXON_MFG_COMPANY_ID 0x034D$' "${CARDPUTER}"
grep -q '^#define AXON_SERVICE_UUID   0xFC81$' "${CARDPUTER}"
grep -q '^static const uint8_t AXON_OUI\[3\] = {0x00, 0x25, 0xDF};$' "${CARDPUTER}"

# The research-reported UUID is corroboration only. This expression is the
# policy boundary that prevents a UUID-only advertisement from classifying.
grep -q 'bool is_axon_classification = got_axon_mfg || got_axon_oui;' "${CARDPUTER}"
grep -q 'bool axon_alarm_eligible = got_axon_mfg;' "${CARDPUTER}"

echo "Signature sync checks passed: $(wc -l < "${TMP_DIR}/cardputer-ouis" | tr -d ' ') OUIs, threshold ${cardputer_threshold}."
