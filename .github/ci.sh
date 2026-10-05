#!/bin/sh
# The whole suite on a stock Gentoo stage3, against the portage release it ships: run as root in
# the container, from the checkout's root. The dependencies come from the official binhost.
# Without the terminal interface: Notcurses is ~amd64 only, built over ffmpeg, and the interface
# does not touch portage, which is what this is here to watch.
set -eu

emerge-webrsync --quiet
getuto
# Only for this emerge: the tests' playgrounds set their own.
FEATURES="getbinpkg -news" EMERGE_DEFAULT_OPTS="--quiet-build --jobs=4" emerge --noreplace --quiet \
    dev-build/meson dev-cpp/cli11 dev-cpp/nlohmann_json dev-cpp/catch dev-python/pytest \
    dev-vcs/git app-shells/zsh app-text/mandoc

# The playground tests need portage's test keys, which only a checkout of the same release has.
version=$(python3 -c 'import portage; print(portage.VERSION)')
git clone --quiet --depth 1 --branch "portage-${version}" \
    https://github.com/gentoo/portage.git /var/tmp/portage

# As a user, as egraph runs day to day: its cache store, not the system one.
useradd --create-home ci
chown -R ci .
su ci -c "
    set -e
    meson setup build -Dtui=disabled -Dportage_test_keys=/var/tmp/portage/lib/portage/tests/.gnupg
    meson test -C build --print-errorlogs
"
