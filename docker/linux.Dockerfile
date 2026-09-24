# manylinux_2_28 brings GCC 14 and CMake, and keeps the executables on glibc 2.28.
# GLFW needs the X11 and Wayland headers at build time and loads both at run time.

FROM quay.io/pypa/manylinux_2_28_x86_64

RUN dnf install -y \
        libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
        libxkbcommon-devel wayland-devel wayland-protocols-devel zip \
    && dnf clean all
