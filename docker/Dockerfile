FROM fedora:latest

# Install dependencies:
RUN dnf install -y \
    gtk4-devel \
    libdnf5-devel \
    gcc-c++ \
    make \
    pkgconf \
    cmake \
    git \
    mesa-libEGL \
    mesa-libGL \
    mesa-libGLES \
    dbus-daemon \
    dbus-glib \
    && dnf clean all

# Install latest Catch2:
RUN git clone https://github.com/catchorg/Catch2.git /tmp/Catch2 && \
    cd /tmp/Catch2 && \
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target install && \
    rm -rf /tmp/Catch2
