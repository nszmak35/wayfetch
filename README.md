# wayfetch

A small fetch tool for Wayland, written in C.

## Build

Dependencies: a C compiler, `pkg-config` and the Wayland client library
(`wayland-client.h`, `libwayland-client`).

Void Linux:

    sudo xbps-install -S base-devel wayland-devel pkg-config

Then:

    make
    sudo make install    # optional, installs to /usr/bin

## Usage

    wayfetch        logo on the left, info on the right
    wayfetch -p     same, plus the protocols line
    wayfetch -u     logo on top, info below it
    wayfetch -n     info only, no logo
    wayfetch -v     print the version

Options can be combined: `wayfetch -pu`, `wayfetch -pn`.

`-v` works without Wayland. Unknown options print
`usage: wayfetch [-u] [-n] [-p] [-v]`.

## Example

          ........
        .:---------.
      ..#.--:------:#.     wayland&wayland
     .-:##:--::---:##-.    ---------------
    .--:##.-:##:--.#:--    wc: ofwc
    .-:-##.:#..#-:#.---.   lib: swc
    .---.###.-:#..#.---.   xwayland: true
     .--:###:--.###:--.    resolution: 1920x1080 @ 60 Hz, 15.6"
      .--.##---:##:--.
       .:-::----::-:.
          ...::...

The logo is yellow with the W filled in white. Colors are disabled when
`NO_COLOR` is set or when the output is not a terminal.

## How it works

The Wayland protocol does not expose the compositor's name, so wayfetch
inspects the process on the other end of the Wayland socket:

1. The compositor's PID comes from `SO_PEERCRED` on the connection.
2. **wc** is the name of `/proc/<pid>/exe`. For interpreters (python, node,
   ...) the script name is used, `.name-wrapped` Nix wrappers become `name`,
   and `comm` is the last resort.
3. **lib** comes from the libraries mapped in `/proc/<pid>/maps` (wlroots,
   scenefx, libweston, swc/neuswc, mutter, kwin, aquamarine, mir, louvre,
   qtwayland-compositor), with a version when the file name has one. For
   statically linked libraries, the binary itself is scanned for known
   symbols and strings.
4. If `/proc/<pid>` is not accessible (different user, sandbox, non-Linux),
   wc falls back to `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP`, and lib is
   taken from a built-in table for well-known compositors, marked `(guess)`.

Both values can be overridden manually; the overrides take priority:

    export WAYFETCH_WC="mycompositor"
    export WAYFETCH_LIB="wlroots"

If nothing can be detected, `unknown` is shown.

### protocols (`-p` only)

The interfaces the compositor advertises in `wl_registry`
(`wl_compositor`, `xdg_wm_base`, `zwlr_layer_shell_v1`, ...), deduplicated
and sorted. Long lists wrap to the terminal width.

### xwayland

`true` if the compositor has Xwayland. Detected via an `Xwayland` (or
`xwayland-satellite`) child process, a listening `/tmp/.X11-unix/X*` socket
(lazy start), or the `xwayland_shell_v1` global. Without `/proc`, `DISPLAY`
is used.

### resolution

Taken from the current `wl_output` mode, along with refresh rate, scale (if
greater than 1) and diagonal in inches (if the compositor reports the
physical size). One line is printed per monitor. Width and height are
swapped for 90/270 degree transforms.

## Without Wayland

wayfetch calls `wl_display_connect(NULL)`. If no compositor is reachable,
for example in a plain TTY, it prints:

    Wayfetch needs wayland :3

## Limitations

- Auto-detection of wc/lib relies on Linux features (`/proc`,
  `SO_PEERCRED`). On other systems only the overrides and `XDG_*` variables
  work.
- If the compositor runs behind waypipe or another proxy, the proxy is on
  the other end of the socket, so wc shows its name.
