FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        dbus-x11 \
        firefox-esr \
        fonts-dejavu \
        bison \
        build-essential \
        curl \
        flex \
        grub-common \
        grub-pc-bin \
        libgmp3-dev \
        libisl-dev \
        libmpc-dev \
        libmpfr-dev \
        libzstd1 \
        make \
        mtools \
        nasm \
        novnc \
        python3 \
        qemu-system \
        supervisor \
        texinfo \
        wget \
        websockify \
        x11vnc \
        xorriso \
        xfce4 \
        xfce4-terminal \
        xvfb \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /bin/bash desktop \
    && mkdir -p /home/desktop/.config/autostart /run/novnc \
    && chown -R desktop:desktop /home/desktop

COPY firefox.desktop /home/desktop/.config/autostart/firefox.desktop
COPY supervisord.conf /etc/supervisor/conf.d/novnc-desktop.conf
COPY start-desktop.sh /usr/local/bin/start-desktop

RUN chmod 0755 /usr/local/bin/start-desktop

EXPOSE 6080
VOLUME ["/home/desktop"]
ENV PATH="/home/desktop/cross/bin:${PATH}"

ENTRYPOINT ["/usr/local/bin/start-desktop"]
