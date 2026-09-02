# Nitrux Workspace Session Manager | ![License](https://img.shields.io/badge/License-BSD--3--Clause-blue)

# Introduction

`nwsm` is a small Wayland session lifecycle manager for OpenRC user services.

It starts the supplied Wayland session command, waits for a verified Wayland socket, and activates the OpenRC user `desktop` runlevel.

OpenRC remains responsible for supervising long-running user services. `nwsm` binds those services to the authenticated graphical session, publishes the compositor environment, and refreshes it when the session creates a replacement Wayland socket.

## Features

- Verifies Wayland socket readiness before activating session services.
- Publishes the compositor environment through `nwsm finalize`.
- Activates and shuts down the OpenRC user `desktop` runlevel.
- Refreshes Wayland, compositor, and D-Bus environment after compositor replacement.
- Migrates existing users to the seeded OpenRC desktop runlevel without overwriting custom services.
- Stops the OpenRC runlevel and removes stale handoff data on exit.

## System Requirements

```
openrc (>= 0.60.0)
dbus
wayland
```

## Usage

```text
nwsm finalize
nwsm -- <wayland-session-command> [arguments...]
```

> [!NOTE]
> Readiness and finalization timeouts default to 60 and 30 seconds. Set `NWSM_READY_TIMEOUT` or `NWSM_FINALIZE_TIMEOUT` to adjust them.

# Licensing

The license for this repository and its contents is **BSD-3-Clause**.

# Issues

If you find problems with the contents of this repository, please create an issue and use the **🐞 Bug report** template.

## Submitting a bug report

Before submitting a bug, you should look at the [existing bug reports](https://github.com/Nitrux/nwsm/issues) to verify that no one has reported the bug already.

©2026 Nitrux Latinoamericana S.C.
