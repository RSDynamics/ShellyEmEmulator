"""ESPHome external component: shelly_em_emulator.

Emulates a Shelly EM (gen1) CoIoT status broadcast (CoAP over UDP multicast to
224.0.1.187:5683), fed from existing ESPHome sensors, so that any consumer which
discovers its grid-power reading by listening for a Shelly EM on the network can be fed
by this device instead -- e.g. a P1 smart-meter reader standing in for a Shelly EM that
an EV charger uses for load balancing.

See README.md in this repository for a full usage example and protocol notes.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

CODEOWNERS = ["@RSDynamics"]
DEPENDENCIES = ["wifi"]

shelly_em_emulator_ns = cg.esphome_ns.namespace("shelly_em_emulator")
ShellyEmEmulator = shelly_em_emulator_ns.class_("ShellyEmEmulator", cg.PollingComponent)

CONF_POWER = "power"
CONF_POWER_RETURNED = "power_returned"
CONF_ENERGY_TARIFF1 = "energy_tariff1"
CONF_ENERGY_TARIFF2 = "energy_tariff2"
CONF_ENERGY_RETURNED_TARIFF1 = "energy_returned_tariff1"
CONF_ENERGY_RETURNED_TARIFF2 = "energy_returned_tariff2"
CONF_VOLTAGE = "voltage"
CONF_DEVICE_ID = "device_id"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_POWER_DELTA = "power_delta"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ShellyEmEmulator),
        # Required: instantaneous power import, in Watts, positive.
        cv.Required(CONF_POWER): cv.use_id(sensor.Sensor),
        # Optional: instantaneous power export (returned/produced), in Watts, positive.
        # If set, export is reported as negative power, matching Shelly's convention.
        cv.Optional(CONF_POWER_RETURNED): cv.use_id(sensor.Sensor),
        # Optional: cumulative energy sensors in kWh. Tariff1/2 are summed for Shelly's
        # single "energy"/"energyReturned" field. If your meter only has one combined
        # energy sensor, wire it into *_tariff1 and leave *_tariff2 unset.
        cv.Optional(CONF_ENERGY_TARIFF1): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_TARIFF2): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_RETURNED_TARIFF1): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_RETURNED_TARIFF2): cv.use_id(sensor.Sensor),
        # Optional: grid voltage, in Volts. Defaults to 230.0 if not set / not yet valid.
        cv.Optional(CONF_VOLTAGE): cv.use_id(sensor.Sensor),
        # Optional: override the CoIoT device-id string sent in the broadcast. Leave
        # unset (recommended) to auto-generate a unique id from this device's own MAC
        # address, so it never collides with a real Shelly EM's identity.
        cv.Optional(CONF_DEVICE_ID): cv.string_strict,
        # Minimum interval between broadcasts even without a value change (a "heartbeat"),
        # mirroring a real Shelly EM's periodic status broadcast.
        cv.Optional(CONF_HEARTBEAT_INTERVAL, default="15s"): cv.positive_time_period_milliseconds,
        # Minimum absolute power change (Watts) that triggers an immediate broadcast,
        # independent of the heartbeat.
        cv.Optional(CONF_POWER_DELTA, default=1.0): cv.positive_float,
    }
).extend(cv.polling_component_schema("1s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # register_polling_component (not the plain register_component) is required for a
    # PollingComponent subclass: it's what actually applies the configured
    # update_interval via set_update_interval(). Without it, update_interval in your
    # YAML is silently ignored.
    await cg.register_polling_component(var, config)

    power_sensor = await cg.get_variable(config[CONF_POWER])
    cg.add(var.set_power_sensor(power_sensor))

    if CONF_POWER_RETURNED in config:
        s = await cg.get_variable(config[CONF_POWER_RETURNED])
        cg.add(var.set_power_returned_sensor(s))

    if CONF_ENERGY_TARIFF1 in config:
        s = await cg.get_variable(config[CONF_ENERGY_TARIFF1])
        cg.add(var.set_energy_tariff1_sensor(s))

    if CONF_ENERGY_TARIFF2 in config:
        s = await cg.get_variable(config[CONF_ENERGY_TARIFF2])
        cg.add(var.set_energy_tariff2_sensor(s))

    if CONF_ENERGY_RETURNED_TARIFF1 in config:
        s = await cg.get_variable(config[CONF_ENERGY_RETURNED_TARIFF1])
        cg.add(var.set_energy_returned_tariff1_sensor(s))

    if CONF_ENERGY_RETURNED_TARIFF2 in config:
        s = await cg.get_variable(config[CONF_ENERGY_RETURNED_TARIFF2])
        cg.add(var.set_energy_returned_tariff2_sensor(s))

    if CONF_VOLTAGE in config:
        s = await cg.get_variable(config[CONF_VOLTAGE])
        cg.add(var.set_voltage_sensor(s))

    if CONF_DEVICE_ID in config:
        cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_power_delta(config[CONF_POWER_DELTA]))
