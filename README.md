# ESPHome Airmax / TCL UART Climate

Native ESPHome external component for Airmax, TCL and related split air
conditioners that use the TCL UART protocol at `9600 8E1`.

This modern implementation replaces the removed ESPHome `platform: custom`
approach, keeps the proven control-frame structure as the default, and makes
features from related models explicit and optional.

## Highlights

- ESP32-C3, standard ESP32 and ESP8266 support.
- Non-blocking UART parser with header, declared-length and XOR checksum
  validation.
- Startup heartbeat and confirmed detection of the response signature plus
  61, 65 or 68-byte status length.
- Explicit 31, 35 and 38-byte TX protocol profiles.
- Climate mode, target temperature, fan, vertical swing, presets and action.
- Optional Heat and horizontal swing support.
- Optional display, beep and health switches.
- Diagnostic sensors exposed only when configured.
- Safe state restoration after a blackout.
- No captive portal or web server; configuration remains on the encrypted
  native API and a bad Wi-Fi credential must be recovered over USB/serial.
- Native ESPHome schemas: no `includes:` and no custom lambda.

ESPHome 2026.5 or newer is required. The component and all three
microcontroller variants were compiled with ESPHome 2026.7.4.

## Quick start

Clone or copy this repository into your ESPHome configuration directory, then:

1. Copy `secrets.example.yaml` to `secrets.yaml` and replace every value.
2. Select the example for your board:
   - `example.yaml`: ESP32-C3
   - `example_esp32.yaml`: standard ESP32 / ESP32-WROOM
   - `example_esp8266.yaml`: ESP8266
3. Verify the UART pins and electrical levels.
4. Validate and upload:

   ```bash
   esphome config example.yaml
   esphome run example.yaml
   ```

The examples load `common.yaml`, which points at the local component in
`components/tcl_climate`.

To consume the component directly from GitHub in another configuration:

```yaml
external_components:
  - source: github://SkateWarp/ESPHome-Airmax@main
    components: [tcl_climate]
```

## UART configuration

```yaml
uart:
  id: uart_bus
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 9600
  data_bits: 8
  parity: EVEN
  stop_bits: 1

logger:
  baud_rate: 0
```

Serial logging must not share the TCL bus. The standard ESP32 example uses
GPIO17/GPIO16 (UART2); the ESP32-C3 and legacy ESP8266 examples use
GPIO1/GPIO3. All pins can be changed.

## Startup detection

The safe default is:

```yaml
climate:
  - platform: tcl_climate
    protocol_profile: tcl_35
    status_frame_length: auto
```

At startup the component sends the original heartbeat:

```text
BB 00 01 04 02 01 00 BD
```

It then validates the response header, type, declared length and XOR checksum.
Two matching replies from two separate heartbeat cycles are required before
control is enabled. Their model-dependent envelope bytes `[1]`, `[2]`, `[5]`
and `[6]`, together with the 61, 65 or 68-byte RX length, are learned and
locked in RAM until the next reboot. A different signature after that point is
ignored instead of being learned in service.

This covers units such as the verified Airmax variant that replies with
`BB 01 00 04 37 03 00 ...` as well as the commonly documented
`BB 01 00 04 37 04 00 ...` envelope. A checksum-valid reply is never enough by
itself to unlock control, and command responses cannot train the default
`tcl_35` detector.

Detection is deliberately limited to heartbeat replies: the component does
not probe different TX control layouts. Different TX profiles can produce
identical status-response lengths and envelopes, so sending trial commands
would not identify a model reliably and could change unsupported fields.

## Protocol profiles

| YAML value | TX size | Intended family |
|---|---:|---|
| `tcl_35` | 35 bytes | Default; preserves the newer working TCL/Airmax frame |
| `electriq_31` | 31 bytes | ElectriQ-style layout |
| `pioneer_31` | 31 bytes | Pioneer variant |
| `tyjw2_35` | 35 bytes | Experimental extended / half-degree layout |
| `tclac_38` | 38 bytes | `I-am-nightingale/tclac` layout |

Keep `tcl_35` when it already works. Select a different profile only when the
model's protocol is known; profile selection is never automatic.

The original `TCL.h` remains in the repository for historical reference. It
was reported working with the Airmax AWTBE12-C2 and uses the old ESPHome custom
component API plus a 31-byte command layout. It is not used by the modern
examples. Its fan encoding is not byte-identical to `electriq_31`, so that
profile must not be presented as a drop-in migration for the legacy
AWTBE12-C2 implementation without hardware validation.

## Blackout state restoration

The supplied configuration enables compact native ESPHome climate
preferences:

```yaml
preferences:
  flash_write_interval: 1min

climate:
  - platform: tcl_climate
    restore_state: true

switch:
  - platform: tcl_climate
    tcl_climate_id: tcl_instance
    restore_state:
      name: Restore State After Power Loss
      restore_mode: RESTORE_DEFAULT_ON
```

Mode, target temperature, fan, swing and preset are persisted. After power
returns, the component:

1. loads the saved state;
2. sends the heartbeat;
3. waits for a fresh, valid appliance response;
4. sends one combined restore command;
5. waits for a new status frame that confirms the observable fields.

The optional restore switch is a runtime gate for the next boot. It defaults
to ON, remembers only real ON/OFF changes through ESPHome preferences, and can
be disabled without changing the current air-conditioner state. While it is
OFF, climate snapshots continue to stay current, so re-enabling it later uses
the most recent state. A real switch change is flushed immediately so it also
survives a power loss before the normal `flash_write_interval`; unchanged
values still cause no flash write. The climate-level `restore_state: true`
remains the global permission; setting it to `false` disables restoration
regardless of the switch.

No blind command is transmitted before the air conditioner responds. If three
consecutive valid states do not reflect a command, the component accepts and
publishes the appliance's real state instead of retrying forever.

The UART is still sampled at the configured polling interval, but identical
climate states are not sent to the native API on every frame. Discrete changes
and availability transitions publish immediately, temperature-only changes are
limited to once every five seconds, and an unchanged keepalive is published
every 30 seconds. This keeps Home Assistant responsive while preventing a
slow or congested API client from turning 500 ms TCL telemetry into an
unbounded send backlog.

The legacy TCL status protocol carries supply voltage in one byte. Values
above 255 V therefore wrap modulo 256 (`7` means `263 V`). The component
normalizes direct and wrapped candidates only inside a conservative
`90–285 V` residential range. A candidate outside that range is not published;
when a previous accepted sample is needed to disambiguate a future protocol
variant, the nearest candidate wins and an unresolved tie is rejected. Raw
zero is treated as a possible "no reading" sentinel and becomes 256 V only
when it continues a nearby, previously accepted high-voltage sample.

ESPHome compares the compact persistent state before touching flash. Repeated
heartbeats and telemetry-only changes therefore do not produce physical flash
writes when mode, target temperature, fan, swing and preset remain unchanged.
The one-minute interval also coalesces multiple real changes into one pending
state, further reducing flash wear.

A power cut immediately after a real change can therefore restore the
preceding state. Change the interval to `10s` if a shorter recovery window is
more important than minimizing flash activity.

On ESP32 and ESP32-C3, the final pending state is compared byte-for-byte with
NVS when the interval expires. ESP8266 compares updates with its flash-backed
RAM copy before marking the sector dirty. As an ESP8266-specific edge case, a
state changed and then reverted within the same interval can still cause one
sector write; avoiding that would require replacing ESPHome's native
preferences and is not worth the added restore risk.

ESP32 and ESP32-C3 use flash-backed preferences automatically. ESP8266 also
requires:

```yaml
esp8266:
  restore_from_flash: true
```

This is already present in `example_esp8266.yaml`.

## Main options

| Option | Default | Purpose |
|---|---|---|
| `protocol_profile` | `tcl_35` | TX command-frame layout |
| `status_frame_length` | `auto` | Learn `61`, `65` or `68`; an explicit value still learns and locks the response signature |
| `restore_state` | `false` | Restore climate state after a restart or blackout |
| `supports_heat` | `false` | Expose Heat only when supported |
| `supports_horizontal_swing` | `false` | Expose horizontal swing |
| `deep_sleep_active_low` | `true` | Interpret the related diagnostic bit |
| `temperature_samples` | `10` | Moving-average window, from 1 to 20 |
| `status_timeout` | `5s` | Reject control when appliance state is stale |
| `inter_byte_timeout` | `50ms` | Discard incomplete UART frames |

Model-dependent features stay disabled until explicitly enabled:

```yaml
climate:
  - platform: tcl_climate
    supports_heat: true
    supports_horizontal_swing: true
```

`common.yaml` enables the display, beep and health child switches to preserve
the supplied working setup; `health` intentionally starts on. Remove any of
those child entries when the connected model does not support them.

## Electrical safety

- Connect controller and air-conditioner ground.
- Cross UART signals: AC TX to ESP RX, AC RX to ESP TX.
- ESP32, ESP32-C3 and ESP8266 GPIOs are 3.3 V only. Verify the AC adapter's
  logic level and add level shifting when necessary.
- Do not leave two active transmitters connected to the same AC RX line.
- ESP8266 ROM output can briefly appear on GPIO1 during boot; isolate or limit
  the line if the appliance is sensitive to it.

## Repository layout

```text
ESPHome-Airmax/
├── components/tcl_climate/       native external component
├── common.yaml                   shared example configuration
├── example.yaml                  ESP32-C3
├── example_esp32.yaml            standard ESP32
├── example_esp8266.yaml          ESP8266
├── secrets.example.yaml          credential template
├── tests/                        protocol and compile matrices
├── scripts/verify.sh             local verification
└── TCL.h                         legacy custom component
```

## Verification

Run:

```bash
./scripts/verify.sh
```

The suite checks golden frames and checksums, 31/35/38-byte commands,
61/65/68-byte responses, corrupt and truncated frames, extended fields and
temperature boundaries. When ESPHome is installed it also validates schemas
and compiles ESP8266, standard ESP32 and ESP32-C3 test configurations.

## References

The implementation keeps the known-working component structure as its source
of truth and cross-checks model-specific protocol details against:

- [junkfix/tcl-electriq-split-ac](https://github.com/junkfix/tcl-electriq-split-ac)
- [ryan-lang/esphome-pioneer-minisplit](https://github.com/ryan-lang/esphome-pioneer-minisplit)
- [I-am-nightingale/tclac](https://github.com/I-am-nightingale/tclac)
- [ESPHome external components documentation](https://esphome.io/components/external_components/)
- [ESPHome custom-component removal notice](https://developers.esphome.io/blog/2025/02/19/about-the-removal-of-support-for-custom-components/)

The original repository credits xaxexa's
[ESPHome-TCLAC](https://github.com/xaxexa/ESPHome-TCLAC) and the early TCL
protocol decoding work by
[htmltiger](https://github.com/htmltiger/tcl-electriq-split-ac), later
continued in
[junkfix/tcl-electriq-split-ac](https://github.com/junkfix/tcl-electriq-split-ac).

## Credentials

Never commit `secrets.yaml`. The repository ignores it and includes only a
placeholder template. Rotate any API, OTA or Wi-Fi credentials that have
previously been posted or committed.
