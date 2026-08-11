// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "jsonl.hh"
#include "vds_protocol.hh"
#include "vdsctl_common.hh"

namespace vds {

namespace {

std::string attach_request(const ControllerConfig &config) {
  std::string request = "{";
  request += jsonl_string_field("command", "attach");
  request += ',';
  request += jsonl_string_field("address", config.address);
  request += ',';
  request +=
      jsonl_string_field("profile", controller_profile_name(config.profile));
  request += ',';
  request += jsonl_unsigned_array_field("ports", config.ports);
  request += "}\n";
  return request;
}

std::string detach_request(std::string_view address) {
  std::string request = "{";
  request += jsonl_string_field("command", "detach");
  request += ',';
  request += jsonl_string_field("address", address);
  request += "}\n";
  return request;
}

std::string list_request() { return "{\"command\":\"list\"}\n"; }

std::string list_targets_request() {
  return "{\"command\":\"list-targets\"}\n";
}

std::string device_info_request() {
  return "{\"command\":\"device-info\"}\n";
}

std::string trace_request(const VdsctlTraceCommand &trace) {
  std::string request = "{";
  request += jsonl_string_field("command", "trace");
  request += ',';
  request += jsonl_string_field("mode", trace.enabled ? "on" : "off");
  request += ',';
  request += jsonl_string_field("scope", trace.scope);
  request += "}\n";
  return request;
}

std::string audio_buffer_request(const VdsctlAudioBufferCommand &command) {
  std::string request = "{";
  request += jsonl_string_field("command", "audio-buffer");
  if (command.chunks) {
    request += ',';
    request += jsonl_string_field("chunks", std::to_string(*command.chunks));
  }
  request += "}\n";
  return request;
}

} // namespace

std::string vdsctl_usage(std::string_view version,
                         std::string_view build_year) {
  std::string text = "vdsctl (";
  text += version;
  text += "): vDS management utility - Copyright (C) ";
  text += build_year;
  text += " Jihong Min\n"
          "usage:\n"
          "  vdsctl attach <address> [--ports 0[,1...]|\"\"] "
          "[--profile ds5|dse|\"\"]\n"
          "  vdsctl detach <address>\n"
          "  vdsctl list\n"
          "  vdsctl list-targets\n"
          "  vdsctl info\n"
          "  vdsctl audio-buffer [chunks]\n"
          "  vdsctl effects [--left-trigger SPEC|none] "
          "[--right-trigger SPEC|none]\n"
          "                 [--led R,G,B|none] [--player 0-63|none]\n"
          "                 [--mute-led off|on|breath|none] [--force on|off] "
          "[--clear]\n"
          "  vdsctl trace on|off [--scope <scope>[,<scope>...]]\n"
          "\n"
          "trigger SPEC:\n"
          "  off | feedback:pos,strength | weapon:start,end,strength |\n"
          "  vibration:pos,amplitude,freq | bow:start,end,strength,snap |\n"
          "  galloping:start,end,foot1,foot2,freq |\n"
          "  machine:start,end,strengthA,strengthB,freq,period | raw:<22 hex>\n"
          "\n"
          "trace scopes:\n"
          "  all, input, input-audio, input-control, output\n";
  return text;
}

VdsctlCommand parse_vdsctl_command(std::string_view command) {
  if (command == "attach") {
    return VdsctlCommand::Attach;
  }
  if (command == "detach") {
    return VdsctlCommand::Detach;
  }
  if (command == "list") {
    return VdsctlCommand::List;
  }
  if (command == "list-targets") {
    return VdsctlCommand::ListTargets;
  }
  if (command == "info") {
    return VdsctlCommand::Info;
  }
  if (command == "trace") {
    return VdsctlCommand::Trace;
  }
  if (command == "audio-buffer") {
    return VdsctlCommand::AudioBuffer;
  }
  if (command == "effects") {
    return VdsctlCommand::Effects;
  }
  throw std::runtime_error("unknown command: " + std::string(command));
}

int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year,
                   const VdsctlPlatform &platform) {
  try {
    if (argc < 2) {
      throw std::runtime_error("command is required");
    }

    if (std::string_view(argv[1]) == "-h" ||
        std::string_view(argv[1]) == "--help") {
      std::cerr << vdsctl_usage(version, build_year);
      return 0;
    }

    switch (parse_vdsctl_command(argv[1])) {
    case VdsctlCommand::Attach:
      std::cout << run_vdsctl_attach(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::Detach:
      std::cout << run_vdsctl_detach(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::List:
      std::cout << run_vdsctl_list(argc, platform.request_control);
      break;
    case VdsctlCommand::ListTargets:
      std::cout << run_vdsctl_list_targets(argc, platform.request_control);
      break;
    case VdsctlCommand::Info:
      std::cout << run_vdsctl_info(argc, platform.request_control);
      break;
    case VdsctlCommand::Trace:
      std::cout << run_vdsctl_trace(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::AudioBuffer:
      std::cout <<
          run_vdsctl_audio_buffer(argc, argv, platform.request_control);
      break;
    case VdsctlCommand::Effects:
      std::cout << run_vdsctl_effects(argc, argv, platform.request_control);
      break;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "vdsctl: " << error.what() << "\n";
    return 1;
  }
}

void require_vdsctl_arg_count(int argc, int expected) {
  if (argc != expected) {
    throw std::runtime_error("unexpected argument");
  }
}

ControllerConfig parse_vdsctl_attach(int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error("attach requires an address");
  }

  ControllerConfig config{
      .address = normalize_bluetooth_address(argv[2]),
      .profile = ControllerProfile::Unspecified,
      .ports = {},
  };

  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next_arg_is_value = [&] {
      return i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0;
    };
    if (arg == "--profile") {
      config.profile =
          parse_controller_profile(next_arg_is_value() ? argv[++i] : "");
    } else if (arg == "--ports") {
      config.ports = parse_ports(next_arg_is_value() ? argv[++i] : "");
    } else {
      throw std::runtime_error("unknown attach option: " + std::string(arg));
    }
  }

  return config;
}

VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    throw std::runtime_error("trace requires on or off with optional --scope");
  }

  const std::string_view mode = argv[2];
  VdsctlTraceCommand command{
      .enabled = false,
      .scope = "all",
  };
  if (argc == 5) {
    if (std::string_view(argv[3]) != "--scope") {
      throw std::runtime_error("trace option must be --scope");
    }
    command.scope = argv[4];
  }

  if (mode == "on") {
    command.enabled = true;
  } else if (mode == "off") {
    command.enabled = false;
  } else {
    throw std::runtime_error("trace requires on or off");
  }

  return command;
}

VdsctlAudioBufferCommand parse_vdsctl_audio_buffer(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    throw std::runtime_error("audio-buffer accepts an optional chunk count");
  }
  VdsctlAudioBufferCommand command;
  if (argc == 2) {
    return command;
  }
  const std::string value = argv[2];
  std::size_t consumed = 0;
  const unsigned long parsed = std::stoul(value, &consumed, 10);
  if (consumed != value.size() ||
      parsed > std::numeric_limits<unsigned>::max()) {
    throw std::runtime_error("audio-buffer chunks must be an unsigned integer");
  }
  command.chunks = static_cast<unsigned>(parsed);
  return command;
}

std::string run_vdsctl_attach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  const ControllerConfig config = parse_vdsctl_attach(argc, argv);
  return request_control(attach_request(config));
}

std::string run_vdsctl_detach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 3);
  return request_control(detach_request(normalize_bluetooth_address(argv[2])));
}

std::string run_vdsctl_list(
    int argc,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 2);
  return request_control(list_request());
}

std::string run_vdsctl_list_targets(
    int argc,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 2);
  return request_control(list_targets_request());
}

std::string run_vdsctl_info(
    int argc,
    const std::function<std::string(const std::string &)> &request_control) {
  require_vdsctl_arg_count(argc, 2);
  return request_control(device_info_request());
}

std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(trace_request(parse_vdsctl_trace(argc, argv)));
}

std::string run_vdsctl_audio_buffer(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  return request_control(
      audio_buffer_request(parse_vdsctl_audio_buffer(argc, argv)));
}

std::string run_vdsctl_effects(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control) {
  std::string request = "{";
  request += jsonl_string_field("command", "effects");

  const auto append = [&request](std::string_view key, std::string_view value) {
    request += ',';
    request += jsonl_string_field(key, value);
  };
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto take_value = [&]() -> std::string_view {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string(arg) + " requires a value");
      }
      return argv[++i];
    };
    if (arg == "--left-trigger") {
      append("left_trigger", take_value());
    } else if (arg == "--right-trigger") {
      append("right_trigger", take_value());
    } else if (arg == "--led") {
      append("led", take_value());
    } else if (arg == "--player") {
      append("player", take_value());
    } else if (arg == "--mute-led") {
      append("mute_led", take_value());
    } else if (arg == "--force") {
      append("force", take_value());
    } else if (arg == "--clear") {
      append("clear", "all");
    } else {
      throw std::runtime_error("unknown effects argument: " + std::string(arg));
    }
  }
  request += "}\n";

  // Validate trigger specs locally for a friendlier error than a round-trip.
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if ((arg == "--left-trigger" || arg == "--right-trigger") &&
        i + 1 < argc) {
      const std::string_view spec = argv[i + 1];
      if (spec != "none") {
        (void)parse_trigger_effect_spec(spec);
      }
    }
  }
  return request_control(request);
}

} // namespace vds
