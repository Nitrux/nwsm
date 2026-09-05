# Nitrux Workspace Session Manager | ![License](https://img.shields.io/badge/License-BSD--3--Clause-blue)

# Introduction

`nwsm` is a small Wayland session lifecycle manager for OpenRC user services.

It starts the supplied Wayland session command, waits for a verified Wayland socket, and activates the OpenRC user `desktop` runlevel.

OpenRC remains responsible for supervising long-running user services. `nwsm` binds those services to the authenticated graphical session, publishes the compositor environment, and refreshes it when the session creates a replacement Wayland socket.

## Features

- Verifies new Wayland and required compositor IPC sockets before activating session services.
- Publishes manager-discovered compositor state automatically; `nwsm finalize` optionally adds compositor-provided variables.
- Activates and shuts down the OpenRC user `desktop` runlevel.
- Refreshes Wayland, compositor, and D-Bus environment after compositor replacement.
- Registers installed OpenRC user services through `rc-update -U` without overwriting custom services or blocking the graphical session.
- Reconciles stale user services before startup and restores the previous activation environment at shutdown.
- Provides `check`, `status`, and `stop` lifecycle controls.
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
nwsm check
nwsm status
nwsm stop
nwsm -- <wayland-session-command> [arguments...]
```

> [!NOTE]
> Readiness defaults to 60 seconds. Optional finalization has a one-second grace period and is never required for startup. Set `NWSM_READY_TIMEOUT` or `NWSM_FINALIZE_GRACE` to adjust them. Comma- or space-separate manager-discovered requirements such as `HYPRLAND_INSTANCE_SIGNATURE` in `NWSM_REQUIRED_VARS`.

# Licensing

The license for this repository and its contents is **BSD-3-Clause**.

# Issues

If you find problems with the contents of this repository, please create an issue and use the **🐞 Bug report** template.

## Submitting a bug report

Before submitting a bug, you should look at the [existing bug reports](https://github.com/Nitrux/nwsm/issues) to verify that no one has reported the bug already.

©2026 Nitrux Latinoamericana S.C.
