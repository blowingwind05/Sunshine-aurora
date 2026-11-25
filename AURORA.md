# Aurora Sunshine

This integration uses Sunshine v2026.914.233613 at 63d35f702ee9e362e43263742981836ec0710384 on both Windows and macOS.

Initialize the submodules and configure CMake with AURORA_SUNSHINE_HOST=ON. The local management, power and wallpaper endpoints share the Aurora implementation. The host exit signal and capture backends follow the platform. Live NVENC bitrate control remains a Windows capability.

For Windows, apply the dependency patch from this checkout:

    git -C third-party/libvirtualhid apply ../../aurora-patches/0007-libvirtualhid-skip-missing-broker.patch

For macOS, supply the Boost.Locale patch from Aurora's macOS Sunshine build recipe. That recipe configures the optional macOS privacy preview.

The bitrate host state machine staged here is maintained in [Aurora](https://github.com/blowingwind05/Aurora).
