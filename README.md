# Switch Pro Controller adapter for macOS

Uses a Nintendo Switch Pro Controller over USB on macOS by turning its input
into keyboard and mouse events.

Plugged in over USB, the controller stops responding and macOS resets it,
usually within about two seconds. In tests, sending the `80 03` command
(high-speed mode) caused the same failure, while the `80 02` handshake alone
kept the controller connected. `procon_mapper` opens the controller
exclusively, sends only the `80 02` handshake, enables the full input report
mode and maps buttons and sticks to keys, mouse buttons, cursor movement and
scrolling.

While `procon_mapper` runs, macOS and other apps do not see the controller.
Games receive keyboard and mouse input instead.

## Requirements

- Apple Silicon Mac (the binaries in `dist/arm64` are arm64)
- hidapi from Homebrew: `brew install hidapi`
- Accessibility permission for the app that runs `procon_mapper` (e.g.
  Terminal): System Settings > Privacy & Security > Accessibility. Without it
  the program asks for the permission and exits.

## Usage

1. Unplug the controller's USB cable.
2. Start the mapper:
   ```
   ./dist/arm64/procon_mapper procon_mapper_gaming.conf
   ```
3. Plug the cable back in. The mapper prints `mapping input, Ctrl+C to quit`.

If the controller was still connected from an earlier session, it sometimes
sends no input. The mapper then prints `no input from the controller` — unplug
and reconnect the cable. The mapper reconnects automatically after every
unplug. Ctrl+C quits.

`--debug` prints the button bytes and stick positions four times a second:

```
./dist/arm64/procon_mapper --debug procon_mapper_gaming.conf
```

## Configurations

| File | Use |
|---|---|
| `procon_mapper.conf` | Desktop use. Loaded when no config file is given. |
| `procon_mapper_gaming.conf` | Games and emulators. |

`procon_mapper_gaming.conf`:

| Control | Mapped to |
|---|---|
| Left stick | WASD |
| Right stick | Mouse (camera) |
| A / B / X / Y | Z / X / C / V |
| L / R / ZL / ZR | E / U / Q / O |
| D-pad | Arrow keys |
| Plus / Minus | `=` / `-` |
| Home | Return (GameCube Start in Dolphin) |
| Left / right stick click | F / H |

The button layout follows the keyboard layout listed for the Ryujinx emulator.

### Config format

One `name = value` per line; `#` starts a comment.

- Buttons: `a b x y l r zl zr plus minus home capture lstick rstick up down left right`
- Button actions:
  - `key <name>`, with optional modifiers: `key space`, `key cmd+tab`, `key shift`
  - `mouse left|right|middle`
  - `none`
- Sticks (`left_stick`, `right_stick`):
  - `mouse`, `scroll`, `none`
  - `wasd`, `arrows`, `keys <up> <left> <down> <right>`: one key for straight
    directions, two for diagonals
- Settings:
  - `mouse_speed`, `scroll_speed`: pixels per second at full deflection;
    a negative `scroll_speed` inverts scrolling
  - `deadzone`: fraction of full deflection ignored around the center
  - `stick_key_threshold`: deflection at which a stick presses its keys

The full list of key names is in `procon_mapper.conf`.

## Building

```
clang++ -std=c++20 -O2 procon_mapper.cpp $(pkg-config --cflags --libs hidapi) \
    -framework ApplicationServices -framework CoreFoundation -o procon_mapper
clang++ -std=c++20 -O2 procon_init.cpp $(pkg-config --cflags --libs hidapi) -o procon_init
```

## procon_init (diagnostics)

Sends USB init commands to the controller and logs every input report, to test
how the controller reacts:

```
./procon_init --exclusive --seq 02 --player 1
```

- `--exclusive`: open the controller exclusively
- `--no-init`: send nothing, only log input
- `--seq 02,03,...`: the `80 xx` commands to send (default `02,03,02,04`)
- `--player N`: set player light N (1-4) after the sequence
