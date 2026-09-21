#!/bin/sh
# Check the exact compiler/linker used by Make, then install missing build tools.
set -eu

cc=$1
ar=$2
cross=$3

case "$cross" in
    '') packages='build-essential' ;;
    arm-linux-gnueabi-) packages='gcc-arm-linux-gnueabi libc6-dev-armel-cross binutils-arm-linux-gnueabi' ;;
    *) packages='' ;;
esac

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

probe() {
    command -v "$cc" >/dev/null 2>&1 || return 1
    command -v "$ar" >/dev/null 2>&1 || return 1
    printf '%s\n' '#include <math.h>' '#include <pthread.h>' \
        'int main(void) {' \
        '  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;' \
        '  volatile double x = 2.0;' \
        '  pthread_mutex_lock(&m); pthread_mutex_unlock(&m);' \
        '  return sqrt(x) < 0.0;' \
        '}' | "$cc" -std=c99 -x c - -o "$tmp_dir/check" -lm -lpthread \
            >"$tmp_dir/compiler.log" 2>&1
}

if probe; then
    exit 0
fi

if [ -z "$packages" ]; then
    echo "Build dependencies missing for compiler $cc. Install its toolchain and C development libraries manually." >&2
    cat "$tmp_dir/compiler.log" >&2 2>/dev/null || true
    exit 1
fi

if [ "${AUTO_INSTALL:-1}" = 0 ]; then
    echo "Build dependencies missing. Install: $packages" >&2
    cat "$tmp_dir/compiler.log" >&2 2>/dev/null || true
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "Automatic installation requires apt-get. Install manually: $packages" >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    run_apt() { apt-get "$@"; }
elif command -v sudo >/dev/null 2>&1; then
    run_apt() { sudo apt-get "$@"; }
else
    echo "Automatic installation requires root or sudo. Install manually: $packages" >&2
    exit 1
fi

echo "Installing build dependencies: $packages"
run_apt update
# The package names above are fixed by the selected toolchain.
# shellcheck disable=SC2086
run_apt install -y $packages

if ! probe; then
    echo "Build dependencies are still unavailable after installation:" >&2
    cat "$tmp_dir/compiler.log" >&2 2>/dev/null || true
    exit 1
fi
