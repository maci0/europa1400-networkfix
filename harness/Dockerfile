FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
ENV WINEDEBUG=-all
ENV WINEARCH=win32
ENV DISPLAY=:99
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        software-properties-common wget gnupg ca-certificates && \
    mkdir -p /etc/apt/keyrings && \
    wget -O /etc/apt/keyrings/winehq-archive.key https://dl.winehq.org/wine-builds/winehq.key && \
    wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        winehq-staging winetricks xvfb xauth xdotool x11-utils imagemagick iproute2 iputils-ping net-tools tini bash python3 python3-pip strace cabextract unzip fontconfig fonts-dejavu-core ffmpeg xserver-xephyr x11-xserver-utils xserver-xorg-video-dummy weston mesa-utils libgl1-mesa-dri && \
    apt-get clean && rm -rf /var/lib/apt/lists/*
RUN if ! id gilde 2>/dev/null; then if id ubuntu 2>/dev/null; then usermod -l gilde ubuntu && usermod -d /home/gilde -m gilde 2>/dev/null || useradd -m gilde; else useradd -m gilde; fi; fi; mkdir -p /home/gilde && chown gilde:gilde /home/gilde 2>/dev/null || chown 1000:1000 /home/gilde
ENV WINEPREFIX=/home/gilde/pfx
ENV GAMEDIR="C:\\Guild"
ENV WINEDLLOVERRIDES="mscoree,mshtml="
USER gilde
WORKDIR /home/gilde
COPY --chown=gilde:gilde entrypoint.sh /home/gilde/entrypoint.sh
COPY --chown=gilde:gilde drivers/ /home/gilde/drivers/
RUN chmod +x /home/gilde/entrypoint.sh
RUN mkdir -p /tmp/dxwrapper /harness && \
    wget -qO /tmp/dxwrapper/dx8.zip https://github.com/elishacloud/dxwrapper/releases/download/v1.7.8400.25/dx8.games.zip && \
    echo "6d301afdef0f4ba09cacf23275718d4b4541a97acab34e41ab08f5e43d15f9f9  /tmp/dxwrapper/dx8.zip" | sha256sum --check --strict && \
    unzip -o /tmp/dxwrapper/dx8.zip -d /tmp/dxwrapper && \
    cp /tmp/dxwrapper/d3d8.dll /tmp/dxwrapper/dxwrapper.dll /harness/ && \
    printf '[General]\nRealDllPath=AUTO\nWrapperMode=AUTO\nDisableLogging=0\n[Compatibility]\nD3d8to9=1\n[d3d9]\nSetPOW2Caps=1\n[dsound]\nPrimaryBufferBits=16\nPrimaryBufferSamples=44100\nPrimaryBufferChannels=2\n' > /harness/dxwrapper.ini && \
    cp /harness/dxwrapper.ini /tmp/dxwrapper/dxwrapper.ini && \
    rm /tmp/dxwrapper/dx8.zip
COPY setup_the_guild_gold_2.0.0.5.exe /tmp/setup.exe
RUN mkdir -p /home/gilde/pfx && \
    timeout 120 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24 -ac +extension GLX +render -noreset" wine wineboot --init 2>&1 | tail -n 30; sleep 2; \
    timeout 300 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24 -ac +extension GLX +render -noreset" wine /tmp/setup.exe /VERYSILENT /DIR=C:\\Guild 2>&1 | tail -n 100; \
    echo "--- install exit $? ---"; ls -lh /home/gilde/pfx/drive_c/Guild/ 2>&1 | head -n 80; \
    if [ -f /home/gilde/pfx/drive_c/Guild/game.ini ]; then sed -i 's/^Bildmodus=.*\r$/Bildmodus=DIRECTWINDOW\r/' /home/gilde/pfx/drive_c/Guild/game.ini || true; sed -i 's/^show_intro=.*\r$/show_intro=0\r/' /home/gilde/pfx/drive_c/Guild/game.ini || true; sed -i 's/^cur_res=.*\r$/cur_res=2\r/' /home/gilde/pfx/drive_c/Guild/game.ini || true; cat /home/gilde/pfx/drive_c/Guild/game.ini | head -n 40; fi; \
    cp /harness/d3d8.dll /harness/dxwrapper.dll /harness/dxwrapper.ini /home/gilde/pfx/drive_c/Guild/ 2>/dev/null || true; \
    ls -lh /home/gilde/pfx/drive_c/Guild/Europa1400Gold*.exe /home/gilde/pfx/drive_c/Guild/d3d8.dll 2>&1 | head -n 20; \
    cp /tmp/setup.exe /harness/setup_the_guild_gold_2.0.0.5.exe 2>/dev/null || true; \
    ls -lh /harness/setup*.exe 2>&1 | head -n 5
ENTRYPOINT ["/usr/bin/tini", "--", "/home/gilde/entrypoint.sh"]
