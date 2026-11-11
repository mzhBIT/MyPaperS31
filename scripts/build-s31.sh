#!/usr/bin/env bash
# macOS/Linux entry for the default S31 system firmware.
set -eo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${IDF_PATH:-}" ]]; then
    activation="${IDF_S31_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
    if [[ ! -f "$activation" ]]; then
        echo "Activate ESP-IDF 6.1 first, or set IDF_S31_ACTIVATE to its activation script." >&2
        exit 1
    fi
    # EIM's Bash activation detects sourcing using $0, which must be "bash".
    exec /bin/bash -ec 'source "$1"; shift; exec /bin/bash "$@"' \
        bash "$activation" "$project_root/scripts/build-s31.sh" "$@"
fi

cd "$project_root"
python "$project_root/scripts/prepare-s31-deps.py"
if [[ $# -eq 0 ]]; then
    set -- build
fi
exec python "$IDF_PATH/tools/idf.py" -B build-s31 \
    -DMYPAPER_BOARD=MyPaperS31 -DMYPAPER_FIRMWARE=System \
    -DIDF_TARGET=esp32s31 \
    "-DSDKCONFIG=$project_root/build-s31/sdkconfig" \
    -DSDKCONFIG_DEFAULTS=configs/mypapers31/sdkconfig.defaults \
    "$@"
