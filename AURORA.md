# Aurora Sunshine

This integration uses Sunshine v2026.914.233613 at 63d35f702ee9e362e43263742981836ec0710384 on both Windows and macOS.

Initialize the shared dependencies and the two GameStream children:

    git submodule update --init --depth 1 -- third-party/build-deps third-party/Simple-Web-Server third-party/libdisplaydevice third-party/libvirtualhid third-party/lizardbyte-common third-party/moonlight-common-c
    git -C third-party/moonlight-common-c submodule update --init --depth 1 -- enet nanors

Then initialize third-party/ViGEmClient and third-party/nvapi for Windows, or
third-party/TPCircularBuffer for macOS. Configure CMake with AURORA_SUNSHINE_HOST=ON. The local management, power and wallpaper endpoints share the Aurora implementation. The host exit signal and capture backends follow the platform. Live NVENC bitrate control remains a Windows capability.

For Windows, apply the dependency patch from this checkout:

    git -C third-party/libvirtualhid apply ../../aurora-patches/0007-libvirtualhid-skip-missing-broker.patch

For macOS, supply the Boost.Locale patch from Aurora's macOS Sunshine build recipe. That recipe configures the optional macOS privacy preview.

The root checkout retains the Windows and macOS runtime dependencies. The standalone tray, documentation theme and Linux packaging/Wayland dependencies are omitted.

The bitrate host state machine staged here is maintained in [Aurora](https://github.com/blowingwind05/Aurora).
