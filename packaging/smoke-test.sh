#!/usr/bin/env bash
# Install the built RPM and check it works. Run from the repository root inside the Fedora CI
# container. A failing check is reported as a GitHub annotation carrying its output, so the
# cause is visible on the run page without opening the log.
set -uo pipefail

annotate_failure() {
    local title=$1 log=$2 msg
    msg=$(tail -c 4000 "$log")
    msg=${msg//'%'/'%25'}
    msg=${msg//$'\r'/'%0D'}
    msg=${msg//$'\n'/'%0A'}
    echo "::error title=Smoke test failed: ${title}::${msg}"
}

check() {
    local title=$1
    shift
    local log
    log=$(mktemp)
    echo "::group::${title}"
    if (set -ex; "$@") >"$log" 2>&1; then
        cat "$log"
        echo "::endgroup::"
    else
        cat "$log"
        echo "::endgroup::"
        annotate_failure "$title" "$log"
        exit 1
    fi
}

install_rpm() {
    dnf install -y rpmbuild/RPMS/*/sdr-simulator-[0-9]*.rpm
    rpm -ql sdr-simulator
}

service_user() {
    getent group sdr-simulator
    getent passwd sdr-simulator
}

builtin_starter() {
    sdr-simulator --scenario-time-ns 0 --render-once-samples 1024 >/tmp/builtin.iq
    test "$(stat -c %s /tmp/builtin.iq)" -eq 4096
}

init_setup() {
    sdr-simulator --init /tmp/site-a
    SDR_SIMULATOR_CONFIG_DIR=/tmp/site-a sdr-simulator --scenario-time-ns 0 --render-once-samples 1024 >/tmp/site-a.iq
    test "$(stat -c %s /tmp/site-a.iq)" -eq 4096
}

check "install RPM" install_rpm
check "service user exists" service_user
check "built-in starter runs" builtin_starter
check "--init setup runs" init_setup
