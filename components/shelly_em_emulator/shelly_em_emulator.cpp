// shelly_em_emulator.cpp
// See shelly_em_emulator.h for a full description of what this component does and why.

#include "shelly_em_emulator.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include "lwip/netdb.h"
#include "arpa/inet.h"

namespace esphome {
namespace shelly_em_emulator {

static const char *const TAG = "shelly_em_emulator";
static const char *const COIOT_MCAST_IP = "224.0.1.187";
static const uint16_t COIOT_PORT = 5683;

void ShellyEmEmulator::setup() {
  if (this->device_id_.empty())
    this->device_id_ = this->generate_device_id_from_mac_();
  ESP_LOGCONFIG(TAG, "Shelly EM emulator device id: %s", this->device_id_.c_str());

  this->sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "Failed to create UDP socket");
    return;
  }
  memset(&this->dest_addr_, 0, sizeof(this->dest_addr_));
  this->dest_addr_.sin_family = AF_INET;
  this->dest_addr_.sin_port = htons(COIOT_PORT);
  this->dest_addr_.sin_addr.s_addr = inet_addr(COIOT_MCAST_IP);

  // TTL > 1 isn't needed on a single L2 segment, but a modest value keeps this working
  // even if your network ever has multiple switches/VLANs between the devices involved.
  uint8_t ttl = 8;
  setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
}

void ShellyEmEmulator::check_power_update() {
  if (this->power_sensor_ == nullptr || !this->power_sensor_->has_state())
    return;

  float power = this->power_sensor_->state;
  if (this->power_returned_sensor_ != nullptr && this->power_returned_sensor_->has_state()
      && this->power_returned_sensor_->state > 0.0f) {
    power = -this->power_returned_sensor_->state;
  }

  uint32_t now = millis();
  bool heartbeat_due = (now - this->last_sent_ms_) >= this->heartbeat_interval_;
  bool changed = std::fabs(power - this->last_sent_power_) >= this->power_delta_;

  if (this->last_sent_ms_ == 0 || heartbeat_due || changed) {
    this->send_coiot_status_(power);
    this->last_sent_power_ = power;
    this->last_sent_ms_ = now;
  }
}

// Builds "SHEM#<last 6 hex chars of MAC, uppercase>#2", mirroring a real Shelly EM's
// id format, but using this ESP's own MAC so the id is always unique on the network.
// esphome::get_mac_address() is framework-independent (works under esp-idf too) and
// returns lowercase hex without colons, e.g. "a1b2c3d4e5f6".
std::string ShellyEmEmulator::generate_device_id_from_mac_() {
  std::string mac = esphome::get_mac_address();
  std::string hex = mac.size() >= 6 ? mac.substr(mac.size() - 6) : mac;
  for (auto &c : hex)
    c = std::toupper(static_cast<unsigned char>(c));
  return "SHEM#" + hex + "#2";
}

// Sums two tariff sensors (kWh) into Wh. A sensor with no state yet counts as 0 instead
// of failing the whole sum, so partial data doesn't block a status update.
float ShellyEmEmulator::sum_tariffs_wh_(sensor::Sensor *t1, sensor::Sensor *t2) {
  float sum_kwh = 0.0f;
  if (t1 != nullptr && t1->has_state())
    sum_kwh += t1->state;
  if (t2 != nullptr && t2->has_state())
    sum_kwh += t2->state;
  return sum_kwh * 1000.0f;
}

void ShellyEmEmulator::send_coiot_status_(float power) {
  float energy = this->sum_tariffs_wh_(this->energy_t1_sensor_, this->energy_t2_sensor_);
  float energy_returned =
      this->sum_tariffs_wh_(this->energy_ret_t1_sensor_, this->energy_ret_t2_sensor_);
  float voltage = (this->voltage_sensor_ != nullptr && this->voltage_sensor_->has_state())
                       ? this->voltage_sensor_->state
                       : 230.0f;

  char json[256];
  snprintf(json, sizeof(json),
           "{\"G\":[[0,9103,0],[0,1101,0],[0,4105,%.2f],[0,4106,%.1f],[0,4107,%.1f],"
           "[0,4108,%.2f],[0,4110,0.00],[0,4205,0.00],[0,4206,0.0],[0,4207,0.0],"
           "[0,4208,%.2f],[0,4210,0.00],[0,6102,0]]}",
           power, energy, energy_returned, voltage, voltage);

  uint8_t buf[512];
  size_t len = this->build_coap_packet_(buf, sizeof(buf), json);
  if (len == 0) {
    ESP_LOGW(TAG, "CoAP packet did not fit in buffer, not sent");
    return;
  }

  if (this->sock_ >= 0) {
    sendto(this->sock_, buf, len, 0, (struct sockaddr *) &this->dest_addr_,
           sizeof(this->dest_addr_));
  }

  this->serial_++;
  this->msg_id_++;
  ESP_LOGD(TAG, "Sent CoIoT status: power=%.2fW energy=%.1fWh serial=%u", power, energy,
           this->serial_);
}

// Encodes one CoAP option (RFC 7252 section 3.1: delta/length with the extended 13/14
// scheme for values that don't fit in a single nibble).
size_t ShellyEmEmulator::encode_option_(uint8_t *buf, size_t pos, size_t buf_size,
                                         uint16_t &prev_number, uint16_t number,
                                         const uint8_t *value, size_t value_len) {
  uint16_t delta = number - prev_number;
  prev_number = number;

  uint8_t delta_nibble, delta_ext_len = 0;
  uint16_t delta_ext = 0;
  if (delta < 13) {
    delta_nibble = delta;
  } else if (delta < 269) {
    delta_nibble = 13;
    delta_ext = delta - 13;
    delta_ext_len = 1;
  } else {
    delta_nibble = 14;
    delta_ext = delta - 269;
    delta_ext_len = 2;
  }

  uint8_t len_nibble, len_ext_len = 0;
  uint16_t len_ext = 0;
  if (value_len < 13) {
    len_nibble = value_len;
  } else if (value_len < 269) {
    len_nibble = 13;
    len_ext = value_len - 13;
    len_ext_len = 1;
  } else {
    len_nibble = 14;
    len_ext = value_len - 269;
    len_ext_len = 2;
  }

  if (pos + 1 + delta_ext_len + len_ext_len + value_len > buf_size)
    return 0;

  buf[pos++] = (delta_nibble << 4) | len_nibble;
  if (delta_ext_len == 1) buf[pos++] = (uint8_t) delta_ext;
  if (delta_ext_len == 2) {
    buf[pos++] = (uint8_t) (delta_ext >> 8);
    buf[pos++] = (uint8_t) (delta_ext & 0xFF);
  }
  if (len_ext_len == 1) buf[pos++] = (uint8_t) len_ext;
  if (len_ext_len == 2) {
    buf[pos++] = (uint8_t) (len_ext >> 8);
    buf[pos++] = (uint8_t) (len_ext & 0xFF);
  }
  memcpy(buf + pos, value, value_len);
  pos += value_len;
  return pos;
}

size_t ShellyEmEmulator::build_coap_packet_(uint8_t *buf, size_t buf_size,
                                             const char *json_payload) {
  size_t pos = 0;
  if (buf_size < 4)
    return 0;

  // Header: Ver=1, Type=NON(1), TKL=0
  buf[pos++] = (1 << 6) | (1 << 4) | 0;
  buf[pos++] = 0x1E;  // Code 0.30
  buf[pos++] = (uint8_t) ((this->msg_id_ >> 8) & 0xFF);
  buf[pos++] = (uint8_t) (this->msg_id_ & 0xFF);

  uint16_t prev_number = 0;
  pos = this->encode_option_(buf, pos, buf_size, prev_number, 11, (const uint8_t *) "cit", 3);
  pos = this->encode_option_(buf, pos, buf_size, prev_number, 11, (const uint8_t *) "s", 1);
  pos = this->encode_option_(buf, pos, buf_size, prev_number, 3332,
                              (const uint8_t *) this->device_id_.c_str(),
                              this->device_id_.size());
  uint8_t o3412[2] = {0x96, 0x00};  // constant value, as observed on the real Shelly EM
  pos = this->encode_option_(buf, pos, buf_size, prev_number, 3412, o3412, 2);
  uint8_t o3420[2] = {(uint8_t) (this->serial_ & 0xFF), (uint8_t) ((this->serial_ >> 8) & 0xFF)};
  pos = this->encode_option_(buf, pos, buf_size, prev_number, 3420, o3420, 2);

  if (pos == 0)
    return 0;

  size_t json_len = strlen(json_payload);
  if (pos + 1 + json_len > buf_size)
    return 0;
  buf[pos++] = 0xFF;
  memcpy(buf + pos, json_payload, json_len);
  pos += json_len;

  return pos;
}

}  // namespace shelly_em_emulator
}  // namespace esphome
