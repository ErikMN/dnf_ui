FROM fedora:latest

# Install dependencies:
RUN dnf install -y \
    gtk4-devel \
    libdnf5-devel \
    gcc-c++ \
    make \
    pkgconf \
    && dnf clean all
