# CLAUDE.md

macOS tools for using a Nintendo Switch Pro Controller (USB, `057e:2009`) on
macOS. See README.md for user-facing usage and the config format.

## Files

- `procon_mapper.cpp`: the main program. Holds the controller exclusively and
  posts keyboard/mouse events (CGEvent) from its input reports, based on a
  config file.
- `procon_init.cpp`: diagnostic tool. Sends configurable `80 xx` commands and
  logs every input report (`--exclusive`, `--no-init`, `--seq`, `--player`).
- `procon_mapper.conf` (desktop, default), `procon_mapper_gaming.conf` (games).
- `dist/arm64/`: committed arm64 binaries. They link Homebrew's
  `/opt/homebrew/opt/hidapi/lib/libhidapi.0.dylib`.

## Build

```
clang++ -std=c++20 -O2 -Wall -Wextra procon_mapper.cpp $(pkg-config --cflags --libs hidapi) \
    -framework ApplicationServices -framework CoreFoundation -o procon_mapper
clang++ -std=c++20 -O2 -Wall -Wextra procon_init.cpp $(pkg-config --cflags --libs hidapi) -o procon_init
```

- Include hidapi as `<hidapi.h>` / `<hidapi_darwin.h>`: Homebrew's pkg-config
  adds `include/hidapi` to the include path, so `<hidapi/hidapi.h>` fails.
- Builds must stay warning-free with `-Wall -Wextra`.
- After changing a program, copy the new build into `dist/arm64/`.
- Binaries in the repo root and `compile_flags.txt` (machine-specific editor
  include path) are git-ignored.

## Controller behavior established by testing

All observed on this Mac (macOS 15.7.3, Apple Silicon, controller f/w 0x4803).

- Plugged in without our program, the controller stops responding on USB and
  macOS resets it, usually ~1.9 s after enumeration. Kernel log: repeated
  `transaction error` (`0xe00002ed`) on endpoints 0x81/0x02, then ~0.7 s later
  `terminateDevice ... reset API call`.
- macOS's own driver (`WindowServer`, `com.apple.GameController.HID:JoyCon`)
  attaches 60-100 ms after enumeration in most sessions.
- Sending `80 03` makes the controller stop responding within ~5-15 ms of its
  ack, with the same kernel errors. Never send `80 03`.
- `80 02` alone with an exclusive open (`hid_darwin_set_open_exclusive(1)`)
  stays connected. A non-exclusive open with `80 02` alone still drops.
- `80 04` after `80 02` (without `80 03`) did not cause a drop in one test.
  It is not used.
- The controller does not always stream input after setup. The mapper sends
  subcommand `0x03` arg `0x30` (full input report mode) itself. If the
  controller stayed connected from an earlier session it sometimes still sends
  nothing (subcommand replies arrive, with a frozen timer byte); unplugging and
  reconnecting fixes it. The mapper detects this after 1 s, releases held
  input, retries once a second and tells the user to reconnect.
- A virtual HID gamepad is not possible without Apple's approval:
  `IOHIDUserDeviceCreateWithProperties` returns NULL without the entitlement
  (`... is not entitled` in the kernel log), and an ad-hoc signed binary
  claiming `com.apple.developer.hid.virtual.device` is killed by AMFI. That is
  why the mapper outputs keyboard and mouse events.

## Protocol notes (byte offsets include the report ID at [0])

- Output `80 xx` → reply input report `81 xx`.
- Subcommands: output report `01`, [1] packet counter (0-15), [2..9] rumble data
  (neutral: `00 01 40 40 00 01 40 40`), [10] subcommand ID, [11] argument.
  Reply: input report `21`, [13] ack, [14] subcommand ID.
  Used: `0x03` (input mode, arg `0x30`), `0x30` (player lights, bit mask).
- Input report `30`: [1] timer, [2] battery/connection, [3] right buttons
  (Y X B A - - R ZR), [4] shared (Minus Plus RStick LStick Home Capture),
  [5] left buttons (Down Up Right Left - - L ZL), [6..8] left stick,
  [9..11] right stick (12-bit x/y each). Sticks rest around 0x800.

## Testing

- Hardware tests need the controller plugged in. Before opening it, check with
  `pgrep -lf procon` whether the user's own mapper is running; it holds the
  controller exclusively. Don't kill the user's process.
- Running the mapper posts real keyboard and mouse events on this Mac.
- USB history for the controller:
  ```
  /usr/bin/log show --last 30m --style compact --predicate 'eventMessage CONTAINS "Pro Controller"' \
    | grep -E 'enumerated|JoyCon|transaction error|terminateDevice'
  ```
  Use `/usr/bin/log`; plain `log` is a zsh builtin.
- `./procon_mapper --debug <config>` prints buttons and sticks four times a
  second; `procon_init` logs raw reports.

## Workflow

- Commit and push only when the user asks.
