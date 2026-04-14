FROM fedora:latest

# Install dependencies:
RUN dnf install -y \
    catch-devel \
    gtk4-devel \
    libdnf5-devel \
    polkit-devel \
    polkit \
    gcc-c++ \
    make \
    meson \
    ninja-build \
    pkgconf \
    cmake \
    git \
    mesa-libEGL \
    mesa-libGL \
    mesa-libGLES \
    dbus-daemon \
    dbus-glib \
    && dnf clean all
