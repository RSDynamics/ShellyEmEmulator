# shelly_em_emulator

An [ESPHome](https://esphome.io) external component that emulates the **CoIoT status
broadcast of a Shelly EM (gen1)** energy meter, fed from your own ESPHome sensors
(for example, sensors from the `dsmr` platform reading a Dutch/Belgian/Luxembourg P1
smart meter).

## Why

Some devices discover their grid-power reading by listening for a Shelly EM's periodic
status broadcast on the local network (for example, certain EV chargers that support
"Shelly EM" as an external meter for load balancing). This component lets another power
source -- such as a P1 smart-meter reader -- stand in for a real Shelly EM, without
needing the Shelly hardware.

It does **not** emulate Shelly's HTTP/REST API, only the CoIoT (CoAP-over-UDP-multicast)
status broadcast. It was built after reverse-engineering a packet capture of a real
Shelly EM; see "Protocol notes" below for what was and wasn't verified.

## Installation

Add this repository as an external component in your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/<your-username>/<your-repo>
      ref: main
    components: [shelly_em_emulator]
```

## Usage example

This example assumes a `dsmr` sensor platform with explicit `id:`s set on the sensors
you want to feed into the emulator (see the ESPHome `dsmr` component docs for the full
sensor list):

```yaml
sensor:
  - platform: dsmr
    power_delivered:
      id: power_delivered_sensor
      name: "Power Consumed"
      unit_of_measurement: "W"
      accuracy_decimals: 0
      filters:
        - multiply: 1000
    power_returned:
      id: power_returned_sensor
      name: "Power Produced"
      unit_of_measurement: "W"
      accuracy_decimals: 0
      filters:
        - multiply: 1000
    energy_delivered_tariff1:
      id: energy_delivered_tariff1_sensor
      name: "Energy Consumed Tariff 1"
      state_class: total_increasing
      device_class: energy
    energy_delivered_tariff2:
      id: energy_delivered_tariff2_sensor
      name: "Energy Consumed Tariff 2"
      state_class: total_increasing
      device_class: energy
    energy_returned_tariff1:
      id: energy_returned_tariff1_sensor
      name: "Energy Produced Tariff 1"
      state_class: total_increasing
    energy_returned_tariff2:
      id: energy_returned_tariff2_sensor
      name: "Energy Produced Tariff 2"
      state_class: total_increasing
    voltage_l1:
      id: voltage_l1_sensor
      name: "Voltage Phase 1"

shelly_em_emulator:
  id: shelly_emulator        # needed if you wire up the on_value hook below
  power: power_delivered_sensor
  power_returned: power_returned_sensor
  energy_tariff1: energy_delivered_tariff1_sensor
  energy_tariff2: energy_delivered_tariff2_sensor
  energy_returned_tariff1: energy_returned_tariff1_sensor
  energy_returned_tariff2: energy_returned_tariff2_sensor
  voltage: voltage_l1_sensor
  heartbeat_interval: 15s   # optional, defaults to 15s -- matches a real Shelly EM's observed period
  power_delta: 1.0          # optional, Watts; defaults to 1.0
  update_interval: 1s       # optional (standard ESPHome polling option) -- see below for what this does
  # device_id: "SHEM#AABBCC#2"  # optional, see "Device identity" below -- leave unset unless you have a specific reason to set it
```

### Recommended: instant, event-driven updates

For truly instant updates -- sent the moment your power sensor changes, rather than on
the next poll tick -- call `check_power_update()` from an `on_value` trigger on your
(combined/net) power sensor:

```yaml
sensor:
  - platform: template
    name: "Power"
    id: power
    unit_of_measurement: "W"
    device_class: power
    state_class: measurement
    on_value:
      then:
        - lambda: id(shelly_emulator).check_power_update();
```

### What `update_interval` actually controls (and what it doesn't)

This component's heartbeat does **not** rely on polling: every actual send (whether
change-triggered or itself a heartbeat) schedules a precise, self-rescheduling one-shot
timer for exactly `heartbeat_interval` later, via ESPHome's `set_timeout()`. So the
heartbeat fires exactly `heartbeat_interval` after the *last real send*, regardless of
`update_interval` -- there's no polling-granularity slop to account for, and no need to
tune `update_interval` relative to `heartbeat_interval` at all.

`update_interval` only controls the **polling fallback** for *change detection*: if you
don't wire up the `on_value` hook above, this is the only way `check_power_update()` is
ever called, so `update_interval` becomes the resolution at which value changes are
noticed (e.g. every 1s). If you do use the `on_value` hook, `update_interval` still runs
in the background as a backstop (useful mainly for retrying the very first send if the
network wasn't up yet when the first value arrived), so there's no need to disable or
widen it -- the small default (`1s`) is fine either way.

If you'd rather have no polling path at all -- relying 100% on the `on_value` hook for
change detection, with zero chance of the poll ever doing anything, by construction
rather than by the delta check happening to evaluate to zero -- set:

```yaml
shelly_em_emulator:
  ...
  update_interval: never
```

This is a standard ESPHome polling-component value that disables `update()` entirely.
The heartbeat is unaffected either way, since it's scheduled independently (see above).
The only trade-off: the very first send, if attempted before the network is up, will
then only be retried on the next `on_value` trigger (i.e. the next telegram) rather than
on the next 1s poll tick -- in practice a negligible difference, since dsmr sensors
republish on every telegram regardless of whether the value changed.

## Configuration options

| Option | Required | Description |
|---|---|---|
| `power` | yes | Sensor: instantaneous import power, Watts, positive |
| `power_returned` | no | Sensor: instantaneous export power, Watts, positive. If set, export is reported as negative power |
| `energy_tariff1` / `energy_tariff2` | no | Sensors: cumulative imported energy, kWh. Summed into Shelly's `energy` field (Wh). If you only have one combined energy sensor, wire it into `energy_tariff1` only |
| `energy_returned_tariff1` / `energy_returned_tariff2` | no | Same, for exported energy (`energyReturned`) |
| `voltage` | no | Sensor: grid voltage, Volts. Defaults to 230.0 if unset |
| `device_id` | no | Override the CoIoT device-id string. Leave unset to auto-generate a unique id from this device's own MAC address (recommended) |
| `heartbeat_interval` | no, default `15s` | Minimum interval between broadcasts even without a value change |
| `power_delta` | no, default `1.0` | Minimum absolute power change (Watts) that triggers an immediate broadcast |
| `update_interval` | no, default `1s` | Standard ESPHome polling-component option: how often the component checks whether a broadcast is due |

## Device identity

By default, this component generates its own CoIoT device-id string
(`SHEM#<last 6 hex chars of this device's MAC>#2`), so it never collides with a real
Shelly EM on the same network. Point your consumer device (e.g. your EV charger's
external-meter setting) at this ESPHome device's IP address or mDNS hostname.

Only set `device_id` explicitly if you specifically want to impersonate a known,
already-decommissioned Shelly EM's identity (for example, to avoid reconfiguring a
consumer that matches on device-id rather than IP).

## When does a real Shelly EM send a status update?

Per Shelly's own CoIoT specification, every CoIoT device is required to periodically
multicast its status, and every status message carries a mandatory option giving the
maximum time until the next publish -- this is what `heartbeat_interval` mirrors.
Each message also carries an incrementing serial number specifically so that a receiver
can skip reprocessing a payload if nothing has changed since the last one, which
confirms devices are expected to publish on change, not only periodically. The exact
numeric threshold a real Shelly EM uses internally to decide a change is "significant
enough" to publish immediately is not documented; `power_delta` is this component's own,
configurable equivalent.

In a packet capture of a real Shelly EM used to build this component, the observed
interval was consistently around 15 seconds (the default `heartbeat_interval` here),
with shorter gaps in between on value changes -- consistent with the above.

## Protocol notes / known limitations

Reverse-engineered from a packet capture of a real Shelly EM:

- Status is sent purely via UDP multicast to `224.0.1.187:5683` as a CoAP
  non-confirmable message, CoAP code `0.30`. No inbound requests were observed in the
  capture, so this component never listens for or answers requests (it is broadcast-only).
- Payload is JSON in Shelly's `"G"` status-array form: `[deviceIndex, id, value]`.
  - `4105` = power (W), `4106` = energy (Wh), `4107` = energyReturned (Wh),
    `4108` = voltage (V), `4110` = powerFactor -- all for device index `0` (channel 0 /
    the grid connection point).
  - `4205`-`4210` are the same fields for channel 1 (a Shelly EM's second CT clamp).
    This component always reports channel 1 as zero.
- CoAP options observed: `11`/`11` (Uri-Path `"cit"`/`"s"`), `3332` (device-id string),
  `3412` (a fixed 2-byte value, constant across every captured packet -- meaning
  unconfirmed), `3420` (an incrementing 2-byte serial number, presumably used by
  receivers for deduplication/staleness detection).
- `powerFactor` (ids `4110`/`4210`) is always sent as `0.00`; this component does not
  compute or report it.

This was verified against one consumer's behavior in one setup; if your consumer expects
something not covered here (e.g. it inspects `powerFactor`, or it polls instead of only
listening to multicast), please open an issue or a PR.

## License

MIT, see [LICENSE](LICENSE).
