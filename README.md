# DNF UI

<p align="center">
  <img src="img/logo.png" width="220" alt="DNF UI logo"/>
</p>

DNF UI is a graphical frontend for Fedora's DNF (Dandified YUM) package manager, inspired by [Synaptic](https://github.com/mvo5/synaptic).
It is built with GTK 4 and libdnf5 and aims to provide a **fast** and **dependable** package management workflow for Fedora.

## Status

DNF UI is in early development.
The project is active and progressing quickly, but it is not yet a stable replacement for existing package management tools.
Interfaces, behavior, and features may change while the application is being stabilized.

## Goals and development principles

- User experience first
- Stability, reliability and predictability
- Strong focus on code quality and maintainability
- No unnecessary complexity or bloat

## Current features

- Search available packages
- List installed and available packages
- View package details, files, dependencies, and changelog information
- Mark packages for install, reinstall, and removal
- Review a transaction summary before applying changes
- Apply transactions through a privileged system service with Polkit authorization
- Cancel long-running package queries
- Show search history
- Hide or show the history and information panes

## Why?

I started DNF UI because I am not satisfied with the current graphical package management options on Fedora.
I want a package manager frontend that feels fast, reliable, and easy to understand in daily use.

This project is also a practical way for me to learn more about libdnf5, GTK 4, and building a
maintainable desktop application.
The goal is not to experiment for its own sake, but to build something genuinely useful for me and others.

## Build

### Native build

Build dependencies:

- meson
- ninja-build
- libdnf5-devel
- gtk4-devel
- polkit
- polkit-devel

Meson handles the real build and install logic.
The `Makefile` is a thin task runner for the common developer commands.

Build and run:

```sh
make && ./dnf_ui
```

Build final and run:

```sh
FINAL=y make && ./dnf_ui
```

Run the Meson setup directly:

```sh
meson setup build/debug --prefix /usr --libexecdir libexec
meson compile -C build/debug
./build/debug/src/dnf_ui
```

## Polkit integration

DNF UI uses a small privileged transaction service for package apply operations.

The GUI runs as the regular desktop user, while the service runs on D-Bus and is
responsible for the privileged transaction step.

[Polkit](https://github.com/polkit-org/polkit) is used only for the apply step:

- Transaction preview is prepared through the service
- The GUI shows the summary dialog
- Apply is authorized by Polkit on the native system bus

This keeps the main application **unprivileged** while still allowing normal desktop
authentication when a transaction is applied.

### Native service install for development

For native Polkit testing from the source tree, install the service files with:

```sh
make
sudo make serviceinstall
```

Then run the app as a regular desktop user:

```sh
./dnf_ui
```

When you apply a transaction, the desktop Polkit prompt should appear.

Remove the development service install with:

```sh
sudo make serviceuninstall
```

**NOTE:**

- `serviceinstall` is a development helper, not the final packaging flow
- Choose a non critical installed package for native apply tests
- `sudo make serviceinstall` installs the already built Meson service files from the current build tree and does not rebuild

### Tests

Test dependencies:

- gio-2.0
- catch2-with-main

Run the test suite:

```sh
make test
```

### Docker

Build the development image:

```sh
make dockersetup
```

Run the application:

```sh
make dockerrun
```

Run the system bus service smoke tests in Docker:

```sh
make dockerservicesystemtest
make dockerservicesystemapplytest
```

Run the test suite in Docker:

```sh
make dockertest
```

Docker notes:

- `make dockerrun` uses the session bus service path for convenience
- Use the `dockerservicesystem*` targets to test the real system bus authorization flow
- Use native Fedora to test the real desktop Polkit prompt

## Screenshot

<p align="center">
  <img src="img/latest_screenshot.png" width="900" alt="DNF UI screenshot"/>
</p>
