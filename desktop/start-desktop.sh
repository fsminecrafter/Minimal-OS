#!/bin/sh
set -eu

case "${NOVNC_PASSWORD:-}" in
    ????????)
        case "$NOVNC_PASSWORD" in
            *[!A-Za-z0-9]*)
                echo "NOVNC_PASSWORD must contain exactly eight alphanumeric characters." >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "NOVNC_PASSWORD must contain exactly eight alphanumeric characters." >&2
        exit 1
        ;;
esac

umask 077
x11vnc -storepasswd "$NOVNC_PASSWORD" /run/novnc/passwd >/dev/null
unset NOVNC_PASSWORD

mkdir -p /home/desktop/Documents
chown desktop:desktop /home/desktop/Documents
if [ ! -e /home/desktop/Documents/Minimal-OS ] && [ ! -L /home/desktop/Documents/Minimal-OS ]; then
    ln -s /Minimal-OS /home/desktop/Documents/Minimal-OS
fi
if [ ! -e /home/desktop/cross ] && [ ! -L /home/desktop/cross ]; then
    ln -s "${CROSS_TOOLCHAIN_PATH:?CROSS_TOOLCHAIN_PATH must point to the mounted cross-toolchain}" /home/desktop/cross
fi

exec /usr/bin/supervisord -c /etc/supervisor/supervisord.conf
