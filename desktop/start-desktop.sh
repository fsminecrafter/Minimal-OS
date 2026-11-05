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

exec /usr/bin/supervisord -c /etc/supervisor/supervisord.conf
