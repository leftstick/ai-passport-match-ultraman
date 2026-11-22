#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local dead_strip_flag

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py
    python3 tools/collect_glyphs.py --check
    python3 tools/check_font_coverage.py
    python3 tools/check_assets.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_math_question.c main/core/math_question.c \
        -o "${test_dir}/test_math_question"
    "${test_dir}/test_math_question"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_hundred_sub.c main/core/math_question.c \
        -o "${test_dir}/test_hundred_sub"
    "${test_dir}/test_hundred_sub"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_campaign.c main/core/campaign.c \
        -o "${test_dir}/test_campaign"
    "${test_dir}/test_campaign"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_action.c main/core/action_config.c \
        -o "${test_dir}/test_action"
    "${test_dir}/test_action"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_cycle.c main/core/cycle.c main/core/math_question.c \
        -o "${test_dir}/test_cycle"
    "${test_dir}/test_cycle"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_battle.c main/core/battle.c main/core/beam_rhythm.c \
        main/core/campaign.c main/core/math_question.c main/core/ultimate.c \
        -o "${test_dir}/test_battle"
    "${test_dir}/test_battle"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_review.c main/core/review.c main/core/math_question.c \
        -o "${test_dir}/test_review"
    "${test_dir}/test_review"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_beam_rhythm.c main/core/beam_rhythm.c \
        -o "${test_dir}/test_beam_rhythm"
    "${test_dir}/test_beam_rhythm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_ultimate.c main/core/ultimate.c main/core/campaign.c \
        -o "${test_dir}/test_ultimate"
    "${test_dir}/test_ultimate"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_anim.c main/core/anim.c -o "${test_dir}/test_anim"
    "${test_dir}/test_anim"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_time_attack.c main/core/time_attack.c \
        main/core/math_question.c -o "${test_dir}/test_time_attack"
    "${test_dir}/test_time_attack"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/core \
        tests/test_progress.c main/core/progress.c \
        -o "${test_dir}/test_progress"
    "${test_dir}/test_progress"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local build_dir="${repo_root}/build"
    local gate_sdkconfig="${build_dir}/sdkconfig.gate"

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    mkdir -p "${build_dir}"
    # Regenerate the gate configuration from the tracked defaults so a stale
    # local config cannot silently change the delivered firmware.
    rm -f "${gate_sdkconfig}"
    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${build_dir}" -D "SDKCONFIG=${gate_sdkconfig}" build
    idf.py -B "${build_dir}" merge-bin \
        -o "${build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${build_dir}"
    # Content-addressed archives are opt-in so a normal gate does not create a
    # new directory on every build.
    if [[ "${PASSPORT_FIRMWARE_ARCHIVE:-0}" == "1" ]]; then
        PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
            "${build_dir}" --archive-root "${build_dir}/firmware"
    fi
    echo "Firmware image: ${build_dir}/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
