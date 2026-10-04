// shelly_em_emulator.h
//
// ESPHome external component that emulates the CoIoT (CoAP) multicast status broadcast
// of a Shelly EM (gen1), fed from arbitrary ESPHome sensors (e.g. a dsmr sensor platform
// reading a Dutch P1 smart meter). Useful for consumers that discover their energy meter
// by listening for Shelly EM CoIoT broadcasts (some EV chargers use this to read grid
// power for load balancing), without requiring the real Shelly EM hardware.
//
// Written for the esp-idf framework (esp32: framework: type: esp-idf): this uses ESP-IDF's
// own lwip BSD sockets for UDP multicast, and ESPHome's framework-independent
// esphome::get_mac_address() helper -- it deliberately avoids Arduino-only APIs such as
// WiFiUDP/WiFi.h so it also works on esp-idf-only boards.
//
// Protocol notes (reverse-engineered from a packet capture of a real Shelly EM,
// firmware id "SHEM#B0F576#2"):
// - Status is sent purely via UDP multicast to 224.0.1.187:5683, as a CoAP
//   non-confirmable message with code 0.30. No inbound requests were observed, so this
//   component never listens for or answers requests.
// - Payload is JSON in Shelly's "G" status array form: [deviceIndex, id, value].
//     id 4105 = power (W)            id 4205 = power, channel 1            (device index 0
//     id 4106 = energy (Wh)          id 4206 = energy, channel 1            always 0 in the
//     id 4107 = energyReturned (Wh)  id 4207 = energyReturned, channel 1    observed capture)
//     id 4108 = voltage (V)          id 4208 = voltage, channel 1
//     id 4110 = powerFactor          id 4210 = powerFactor, channel 1
//   Channel 1 (ids 4205-4210) corresponds to the Shelly EM's second CT clamp; this
//   component always reports it as zero, since it is not needed to feed a consumer that
//   only cares about the grid connection point.
// - CoAP options observed: 11/11 (Uri-Path "cit"/"s"), 3332 (device-id string), 3412 (a
//   fixed 2-byte value, meaning unconfirmed but constant across all captured packets),
//   3420 (an incrementing 2-byte serial number, presumably used by receivers for
//   deduplication/staleness detection).
//
// Caveats / things to verify in your own setup:
// - powerFactor (ids 4110/4210) is always sent as 0.00.
// - The device-id defaults to one generated from this device's own MAC address
//   (format "SHEM#<last 6 hex chars of MAC, uppercase>#2"), so it never collides with a
//   real Shelly EM's identity. Override it explicitly only if you have a specific reason
//   to impersonate a known device id.
// - Whether a given consumer matches on source IP, hostname, or the CoIoT device-id
//   string is consumer-specific and was not verified here; point your consumer at this
//   device's IP/hostname as needed.

#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "lwip/sockets.h"

namespace esphome {
namespace shelly_em_emulator {

class ShellyEmEmulator : public PollingComponent {
 public:
  // update_interval (set via the standard ESPHome polling-component "update_interval"
  // option) controls how often we *check* for a change; it is independent of the
  // heartbeat_interval below, which controls the minimum guaranteed send rate.
  ShellyEmEmulator() = default;

  // Leave unset (default) to auto-generate a unique id from this device's own MAC
  // address. Only set this explicitly if you intend to impersonate a specific, known
  // Shelly EM identity (e.g. one that has been permanently decommissioned).
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  void set_heartbeat_interval(uint32_t ms) { this->heartbeat_interval_ = ms; }
  void set_power_delta(float w) { this->power_delta_ = w; }

  // Power: power_sensor (import, W, positive), optional power_returned_sensor (export,
  // W, positive). This component converts to Shelly's convention internally (negative
  // power = export).
  void set_power_sensor(sensor::Sensor *s) { this->power_sensor_ = s; }
  void set_power_returned_sensor(sensor::Sensor *s) { this->power_returned_sensor_ = s; }

  // Energy totals: provide the two tariff sensors (in kWh, as ESPHome's dsmr platform
  // reports them); they are summed and converted to Wh for Shelly's "energy"/
  // "energyReturned" fields. If your meter/region has a single combined energy sensor
  // instead of two tariffs, just wire the same sensor into both the tariff1 and tariff2
  // setters -- or leave tariff2 unset and only set tariff1.
  void set_energy_tariff1_sensor(sensor::Sensor *s) { this->energy_t1_sensor_ = s; }
  void set_energy_tariff2_sensor(sensor::Sensor *s) { this->energy_t2_sensor_ = s; }
  void set_energy_returned_tariff1_sensor(sensor::Sensor *s) { this->energy_ret_t1_sensor_ = s; }
  void set_energy_returned_tariff2_sensor(sensor::Sensor *s) { this->energy_ret_t2_sensor_ = s; }

  void set_voltage_sensor(sensor::Sensor *s) { this->voltage_sensor_ = s; }

  void setup() override;
  void update() override { this->check_power_update(); }

  // Checks whether power has changed by at least power_delta, or the heartbeat_interval
  // has elapsed, and if so builds and sends a CoIoT status update immediately. This is
  // called automatically every update_interval as a safety-net poll, so wiring this up
  // yourself is optional -- but for instant, event-driven updates (matching how the
  // dsmr sensors themselves update, rather than waiting for the next poll tick), call
  // this from an on_value trigger on your power sensor, e.g.:
  //
  //   sensor:
  //     - platform: template
  //       id: power
  //       ...
  //       on_value:
  //         then:
  //           - lambda: id(shelly_emulator_id).check_power_update();
  void check_power_update();

 protected:
  std::string generate_device_id_from_mac_();
  float sum_tariffs_wh_(sensor::Sensor *t1, sensor::Sensor *t2);
  void send_coiot_status_(float power);
  size_t encode_option_(uint8_t *buf, size_t pos, size_t buf_size, uint16_t &prev_number,
                         uint16_t number, const uint8_t *value, size_t value_len);
  size_t build_coap_packet_(uint8_t *buf, size_t buf_size, const char *json_payload);

  int sock_{-1};
  struct sockaddr_in dest_addr_ {};

  std::string device_id_{};  // empty = auto-generate from own MAC, see setup()
  uint32_t heartbeat_interval_{15000};
  float power_delta_{1.0f};

  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *power_returned_sensor_{nullptr};
  sensor::Sensor *energy_t1_sensor_{nullptr};
  sensor::Sensor *energy_t2_sensor_{nullptr};
  sensor::Sensor *energy_ret_t1_sensor_{nullptr};
  sensor::Sensor *energy_ret_t2_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};

  float last_sent_power_{0.0f};
  uint32_t last_sent_ms_{0};
  uint16_t msg_id_{1};
  uint16_t serial_{1};
};

}  // namespace shelly_em_emulator
}  // namespace esphome
