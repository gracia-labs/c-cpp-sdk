# manylinux brings GCC 14 and CMake, and keeps the executables on the glibc of its base: 2.28 on x86_64, and 2.34 on
# aarch64, the glibc of the SDK there.
# GLFW needs the X11 and Wayland headers at build time and loads both at run time.

FROM quay.io/pypa/manylinux_2_28 AS base-amd64
FROM quay.io/pypa/manylinux_2_34 AS base-arm64
FROM base-${TARGETARCH}

RUN dnf install -y \
        libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
        libxkbcommon-devel wayland-devel wayland-protocols-devel zip \
    && dnf clean all
