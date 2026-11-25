# Aurora Sunshine for windows

Upstream base: 63d35f702ee9e362e43263742981836ec0710384

Configure CMake with AURORA_SUNSHINE_HOST=ON.

After initializing submodules, apply the dependency patch:

    git -C third-party/libvirtualhid apply ../../aurora-patches/0007-libvirtualhid-skip-missing-broker.patch

Sources copied into this checkout are maintained in [Aurora](https://github.com/blowingwind05/Aurora). Update them there before replaying the integration.
