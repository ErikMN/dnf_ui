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

- libdnf5
- gtk4

Build final and run:

```sh
FINAL=y make && ./dnf_ui
```

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

Run the test suite in Docker:

```sh
make dockertest
```

## Screenshot

<p align="center">
  <img src="img/latest_screenshot.png" width="900" alt="DNF UI screenshot"/>
</p>
