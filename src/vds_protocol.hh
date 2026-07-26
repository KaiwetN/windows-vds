// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vds/ds5_protocol.h"

namespace vds {

constexpr std::size_t kBtHapticsReportSize = VDS_BT_HAPTICS_REPORT_SIZE;
constexpr std::size_t kBtInitReportSize = 142;
constexpr std::size_t kBtStateReportSize = VDS_BT_STATE_REPORT_SIZE;
constexpr std::size_t kHapticsSampleSize = VDS_HAPTICS_SAMPLE_SIZE;
constexpr std::size_t kUsbInputReportSize = VDS_USB_INPUT_REPORT_SIZE;
constexpr std::size_t kDsStateSize = 63;
constexpr std::size_t kSpeakerChannels = 2;
constexpr std::size_t kPcmWindowFrames = 512;
constexpr std::size_t kSpeakerOpusFrames = 480;
constexpr std::size_t kSpeakerOpusSize = 200;
constexpr std::size_t kMicOpusFrames = 480;
constexpr std::size_t kMicOpusSize = 71;
constexpr std::size_t kMicUsbChannels = 2;
constexpr std::size_t kMicPcmSize =
    kMicOpusFrames * kMicUsbChannels * sizeof(std::int16_t);

using BtReport = std::array<std::uint8_t, kBtHapticsReportSize>;
using BtInitReport = std::array<std::uint8_t, kBtInitReportSize>;
using BtStateReport = std::array<std::uint8_t, kBtStateReportSize>;
using HapticsChunk = std::array<std::int8_t, kHapticsSampleSize>;
using PcmWindow = std::array<std::int16_t, kPcmWindowFrames * kSpeakerChannels>;
using SpeakerChunk = std::array<std::uint8_t, kSpeakerOpusSize>;
using DsState = std::array<std::uint8_t, kDsStateSize>;
using UsbInputReport = std::array<std::uint8_t, kUsbInputReportSize>;

enum class BtInputPayloadType {
  Unknown,
  Control,
  Audio,
};

constexpr std::size_t kTriggerFfbSize = 11;
using TriggerFfb = std::array<std::uint8_t, kTriggerFfbSize>;

/*
 * Daemon-authored controller effects. Each field mirrors a section of
 * vds_set_state_data guarded by its own allow_* bit, so effects coexist with
 * the game's USB output reports: the controller applies a section only when
 * its allow bit is set, and apply_usb_output_report() only overwrites a
 * section when the game itself raised that bit.
 */
struct AuthoredEffects {
  std::optional<TriggerFfb> left_trigger;
  std::optional<TriggerFfb> right_trigger;
  std::optional<std::array<std::uint8_t, 3>> led_color;
  std::optional<std::uint8_t> player_lights; // bits 0-4 lights, bit 5 fade-in
  std::optional<std::uint8_t> mute_light;    // 0 off, 1 on, 2 breathing
  bool force = false; // reassert over game writes instead of yielding

  bool any() const {
    return left_trigger || right_trigger || led_color || player_lights ||
           mute_light;
  }
};

/*
 * DualSense adaptive-trigger effect builders. Byte formats follow the
 * community-documented firmware modes popularized by Nielk1's
 * TriggerEffectGenerator (MIT). All throw std::invalid_argument on
 * out-of-range parameters.
 */
TriggerFfb trigger_effect_off();
// position 0-9, strength 1-8: constant resistance from position onwards.
TriggerFfb trigger_effect_feedback(unsigned position, unsigned strength);
// start 2-7, end start+1-8, strength 1-8: resist between start/end, then snap.
TriggerFfb trigger_effect_weapon(unsigned start, unsigned end,
                                 unsigned strength);
// position 0-9, amplitude 1-8, frequency Hz 1-255: vibrate past position.
TriggerFfb trigger_effect_vibration(unsigned position, unsigned amplitude,
                                    unsigned frequency);
// start 0-8, end start+1-8, strength 1-8, snap_force 1-8.
TriggerFfb trigger_effect_bow(unsigned start, unsigned end, unsigned strength,
                              unsigned snap_force);
// start 0-8, end start+1-9, first_foot 0-6, second_foot first+1-7, freq Hz.
TriggerFfb trigger_effect_galloping(unsigned start, unsigned end,
                                    unsigned first_foot, unsigned second_foot,
                                    unsigned frequency);
// start 0-8, end start+1-9, strengths 0-7, frequency Hz, period x0.1s.
TriggerFfb trigger_effect_machine(unsigned start, unsigned end,
                                  unsigned strength_a, unsigned strength_b,
                                  unsigned frequency, unsigned period);
// Parses "off" / "feedback:4,6" / "weapon:2,6,8" / "vibration:0,6,25" /
// "bow:1,6,4,6" / "galloping:0,8,2,5,4" / "machine:1,8,6,6,30,2" /
// "raw:<22 hex digits>". Throws std::invalid_argument on malformed input.
TriggerFfb parse_trigger_effect_spec(std::string_view spec);

struct AudioChunk {
  HapticsChunk haptics;
  SpeakerChunk speaker;
  bool has_signal = false;
  bool has_haptics_signal = false;
};

void fill_output_report_checksum(std::span<std::uint8_t> report);
void fill_feature_report_checksum(std::span<std::uint8_t> report);
std::vector<std::uint8_t>
hidp_output_packet(std::span<const std::uint8_t> report);
std::vector<std::uint8_t> feature_get_packet(std::uint8_t report_id);
std::vector<std::uint8_t>
feature_set_packet(std::span<const std::uint8_t> report);
BtInputPayloadType bt_input_payload_type(std::span<const std::uint8_t> packet);
std::optional<std::span<const std::uint8_t, kMicOpusSize>>
bt_mic_opus_payload(std::span<const std::uint8_t> packet);
std::optional<UsbInputReport>
bt_input_to_usb_input(std::span<const std::uint8_t> packet);
std::optional<std::vector<std::uint8_t>>
bt_feature_to_usb_feature_reply(std::span<const std::uint8_t> packet);

class MicAudioDecoder {
public:
  MicAudioDecoder();
  ~MicAudioDecoder();

  MicAudioDecoder(MicAudioDecoder &&) noexcept;
  MicAudioDecoder &operator=(MicAudioDecoder &&) noexcept;

  MicAudioDecoder(const MicAudioDecoder &) = delete;
  MicAudioDecoder &operator=(const MicAudioDecoder &) = delete;

  std::vector<std::uint8_t>
  decode(std::span<const std::uint8_t, kMicOpusSize> payload);

private:
  struct Decoder;

  std::unique_ptr<Decoder> decoder_;
};

class HapticsPacketBuilder {
public:
  BtReport
  build_packet(std::span<const std::int8_t, kHapticsSampleSize> haptics,
               std::span<const std::uint8_t, kSpeakerOpusSize> speaker,
               std::span<const std::uint8_t, kDsStateSize> state,
               bool audio_sections_enabled, bool headset_plugged);

private:
  std::uint8_t report_sequence_ = 0;
  std::uint8_t packet_sequence_ = 0;
};

class DsOutputState {
public:
  DsOutputState();

  bool apply_usb_output_report(std::span<const std::uint8_t> report);
  void apply_authored_effects(const AuthoredEffects &effects);
  void set_audio_out_stream_active(bool active, bool headset_plugged = false);
  void set_headset_mic_plugged(bool plugged);
  BtStateReport build_bt_mic_state_report(bool active, bool muted);
  BtInitReport build_bt_mic_report(bool active);
  BtInitReport build_bt_init_report();
  BtStateReport build_bt_state_report();
  BtStateReport build_bt_rumble_report(std::uint8_t right_motor,
                                       std::uint8_t left_motor);
  const DsState &state() const { return state_; }

private:
  void apply_mic_select();
  DsState state_{};
  std::array<std::uint8_t, 3> light_color_{};
  std::uint8_t headphones_volume_ = 0;
  std::uint8_t speaker_volume_ = 0;
  std::uint8_t mic_volume_ = 0;
  std::uint8_t audio_control_ = 0;
  std::uint8_t audio_output_path_ = 0;
  std::uint8_t audio_control2_ = 0;
  std::uint8_t power_save_control_ = 0;
  std::uint8_t light_brightness_ = 0;
  bool emulate_light_brightness_ = false;
  bool headset_mic_plugged_ = false;
  std::uint8_t report_sequence_ = 0;
  std::uint8_t mic_sequence_ = 0;
};

class PcmAudioExtractor {
public:
  explicit PcmAudioExtractor(std::size_t pcm_window_frames = kPcmWindowFrames);
  ~PcmAudioExtractor();

  PcmAudioExtractor(PcmAudioExtractor &&) noexcept;
  PcmAudioExtractor &operator=(PcmAudioExtractor &&) noexcept;

  PcmAudioExtractor(const PcmAudioExtractor &) = delete;
  PcmAudioExtractor &operator=(const PcmAudioExtractor &) = delete;

  std::vector<AudioChunk>
  push_usb_audio(std::span<const std::uint8_t> pcm_bytes);

private:
  struct SpeakerEncoder;

  std::unique_ptr<SpeakerEncoder> speaker_encoder_;
  std::array<std::uint8_t, VDS_AUDIO_CHANNELS * sizeof(std::int16_t)>
      pending_frame_{};
  PcmWindow speaker_pcm_{};
  PcmWindow haptics_pcm_{};
  std::size_t pcm_window_frames_ = kPcmWindowFrames;
  std::size_t pending_frame_pos_ = 0;
  std::size_t pcm_window_frame_pos_ = 0;
  bool chunk_has_signal_ = false;
  bool chunk_has_haptics_signal_ = false;
};

std::vector<std::uint8_t> frame_bytes(std::uint16_t type,
                                      std::span<const std::uint8_t> payload);
std::string frame_type_name(std::uint16_t type);

} // namespace vds
