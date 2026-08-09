FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV WINEDEBUG=-all
ENV DISPLAY=:99

# Enable i386
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        software-properties-common wget gnupg ca-certificates && \
    mkdir -p /etc/apt/keyrings && \
    wget -O /etc/apt/keyrings/winehq-archive.key https://dl.winehq.org/wine-builds/winehq.key && \
    wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        winehq-staging \
        winetricks \
        xvfb \
        xauth \
        xdotool \
        x11-utils \
        imagemagick \
        iproute2 \
        iputils-ping \
        net-tools \
        tini \
        bash \
        python3 \
        python3-pip \
        strace \
        cabextract \
        unzip \
        fontconfig \
        ttf-freefont \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

RUN useradd -m -u 1000 gilde

# Wine prefix will be created at runtime (volume) or baked with silent install if installer present.
# We keep GAMEDIR logic compatible with reference: C:\Guild
ENV WINEPREFIX=/home/gilde/pfx
ENV GAMEDIR="C:\\Guild"
ENV WINEDLLOVERRIDES="mscoree,mshtml="

USER gilde
WORKDIR /home/gilde/pfx/drive_c/Guild

# Game.ini tweaks applied at runtime in entrypoint (if game.ini exists)
COPY --chown=gilde:gilde entrypoint.sh /home/gilde/entrypoint.sh
COPY --chown=gilde:gilde drivers/ /home/gilde/drivers/
RUN chmod +x /home/gilde/entrypoint.sh

# dxwrapper POW2 fix for D3D8 UI bug (muddykat-tech/Gold-Guild-Patch -> elishacloud/dxwrapper SetPOW2Caps=1)
# Downloaded at build time (not baked as binary to keep repo lean) — also staged to /harness for entrypoint
RUN mkdir -p /tmp/dxwrapper /harness && \
    wget -qO /tmp/dxwrapper/dx8.zip https://github.com/elishacloud/dxwrapper/releases/download/v1.7.8400.25/dx8.games.zip && \
    echo "6d301afdef0f4ba09cacf23275718d4b4541a97acab34e41ab08f5e43d15f9f9  /tmp/dxwrapper/dx8.zip" | sha256sum --check --strict && \
    unzip -o /tmp/dxwrapper/dx8.zip -d /tmp/dxwrapper && \
    cp /tmp/dxwrapper/d3d8.dll /tmp/dxwrapper/dxwrapper.dll /harness/ && \
    printf '[General]\nRealDllPath=AUTO\nWrapperMode=AUTO\nDisableLogging=0\n[Compatibility]\nD3d8to9=1\n[d3d9]\nSetPOW2Caps=1\n[dsound]\nPrimaryBufferBits=16\nPrimaryBufferSamples=44100\nPrimaryBufferChannels=2\n' > /harness/dxwrapper.ini && \
    cp /harness/dxwrapper.ini /tmp/dxwrapper/dxwrapper.ini && \
    rm /tmp/dxwrapper/dx8.zip

# Optional: if installer is built into image, uncomment:
# COPY --chown=gilde:gilde setup_the_guild_gold_2.0.0.5.exe /home/gilde/
# RUN tini -s -- xvfb-run --auto-servernum wine /home/gilde/setup_the_guild_gold_2.0.0.5.exe /VERYSILENT /DIR=C:\\Guild && rm /home/gilde/setup_the_guild_gold_2.0.0.5.exe

ENTRYPOINT ["/usr/bin/tini", "--", "/home/gilde/entrypoint.sh"]
