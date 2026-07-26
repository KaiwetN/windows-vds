// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <dbus/dbus.h>

#include "vds_bluez.hh"
#include "vds_config.hh"
#include "vds_profile.hh"

namespace {

constexpr const char *kBluezService = "org.bluez";
constexpr const char *kBluezRootPath = "/";
constexpr const char *kBluezDeviceInterface = "org.bluez.Device1";
constexpr const char *kDbusObjectManagerInterface =
    "org.freedesktop.DBus.ObjectManager";
constexpr const char *kDbusPropertiesInterface =
    "org.freedesktop.DBus.Properties";
constexpr const char *kDbusUnknownPropertyError =
    "org.freedesktop.DBus.Error.UnknownProperty";

struct DBusConnectionDeleter {
  void operator()(DBusConnection *connection) const {
    ::dbus_connection_unref(connection);
  }
};

struct DBusMessageDeleter {
  void operator()(DBusMessage *message) const { ::dbus_message_unref(message); }
};

using DBusConnectionPtr =
    std::unique_ptr<DBusConnection, DBusConnectionDeleter>;
using DBusMessagePtr = std::unique_ptr<DBusMessage, DBusMessageDeleter>;

class DBusErrorGuard {
public:
  DBusErrorGuard() { ::dbus_error_init(&error_); }
  ~DBusErrorGuard() { ::dbus_error_free(&error_); }

  DBusErrorGuard(const DBusErrorGuard &) = delete;
  DBusErrorGuard &operator=(const DBusErrorGuard &) = delete;

  DBusError *get() { return &error_; }

  bool has_name(const char *name) const {
    return ::dbus_error_has_name(&error_, name) != 0;
  }

  std::string message() const {
    if (error_.message != nullptr) {
      return error_.message;
    }
    if (error_.name != nullptr) {
      return error_.name;
    }
    return "unknown D-Bus error";
  }

private:
  DBusError error_;
};

void require_message_type(DBusMessageIter *iter, int expected,
                          const char *context) {
  const int type = ::dbus_message_iter_get_arg_type(iter);
  if (type != expected) {
    throw std::runtime_error(std::string(context) + ": unexpected D-Bus type");
  }
}

const char *read_dbus_string(DBusMessageIter *iter, const char *context) {
  require_message_type(iter, DBUS_TYPE_STRING, context);
  const char *value = nullptr;
  ::dbus_message_iter_get_basic(iter, &value);
  if (value == nullptr) {
    throw std::runtime_error(std::string(context) + ": empty D-Bus string");
  }
  return value;
}

std::optional<std::string> read_dbus_variant_string(DBusMessageIter *iter) {
  if (::dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT) {
    return std::nullopt;
  }

  DBusMessageIter variant;
  ::dbus_message_iter_recurse(iter, &variant);
  if (::dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRING) {
    return std::nullopt;
  }

  const char *value = nullptr;
  ::dbus_message_iter_get_basic(&variant, &value);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

std::optional<bool> read_dbus_variant_bool(DBusMessageIter *iter) {
  if (::dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT) {
    return std::nullopt;
  }

  DBusMessageIter variant;
  ::dbus_message_iter_recurse(iter, &variant);
  if (::dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) {
    return std::nullopt;
  }

  dbus_bool_t value = FALSE;
  ::dbus_message_iter_get_basic(&variant, &value);
  return value != FALSE;
}

DBusConnectionPtr open_system_bus() {
  DBusErrorGuard error;
  DBusConnection *connection = ::dbus_bus_get(DBUS_BUS_SYSTEM, error.get());
  if (connection == nullptr) {
    throw std::runtime_error("failed to connect to system D-Bus: " +
                             error.message());
  }
  ::dbus_connection_set_exit_on_disconnect(connection, false);
  return DBusConnectionPtr(connection);
}

DBusMessagePtr call_dbus_method(DBusConnection *connection, DBusMessage *call,
                                const char *context) {
  DBusErrorGuard error;
  DBusMessage *reply = ::dbus_connection_send_with_reply_and_block(
      connection, call, DBUS_TIMEOUT_USE_DEFAULT, error.get());
  if (reply == nullptr) {
    throw std::runtime_error(std::string(context) + ": " + error.message());
  }
  return DBusMessagePtr(reply);
}

std::optional<std::string> read_device_address(DBusMessageIter *properties) {
  while (::dbus_message_iter_get_arg_type(properties) != DBUS_TYPE_INVALID) {
    require_message_type(properties, DBUS_TYPE_DICT_ENTRY,
                         "BlueZ Device1 property");

    DBusMessageIter property;
    ::dbus_message_iter_recurse(properties, &property);
    const char *name = read_dbus_string(&property, "BlueZ property name");
    if (!::dbus_message_iter_next(&property)) {
      throw std::runtime_error("missing BlueZ property value");
    }

    if (std::strcmp(name, "Address") == 0) {
      const auto address = read_dbus_variant_string(&property);
      if (!address) {
        return std::nullopt;
      }
      try {
        return vds::normalize_bluetooth_address(*address);
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }

    ::dbus_message_iter_next(properties);
  }

  return std::nullopt;
}

std::optional<vds::ControllerTarget>
read_controller_target(DBusMessageIter *properties) {
  std::optional<std::string> address;
  std::optional<std::string> name;
  std::optional<std::string> alias;
  std::optional<std::string> modalias;
  bool paired = false;
  bool connected = false;

  while (::dbus_message_iter_get_arg_type(properties) != DBUS_TYPE_INVALID) {
    require_message_type(properties, DBUS_TYPE_DICT_ENTRY,
                         "BlueZ Device1 property");

    DBusMessageIter property;
    ::dbus_message_iter_recurse(properties, &property);
    const char *property_name =
        read_dbus_string(&property, "BlueZ property name");
    if (!::dbus_message_iter_next(&property)) {
      throw std::runtime_error("missing BlueZ property value");
    }

    if (std::strcmp(property_name, "Address") == 0) {
      if (const auto value = read_dbus_variant_string(&property)) {
        try {
          address = vds::normalize_bluetooth_address(*value);
        } catch (const std::exception &) {
          return std::nullopt;
        }
      }
    } else if (std::strcmp(property_name, "Name") == 0) {
      name = read_dbus_variant_string(&property);
    } else if (std::strcmp(property_name, "Alias") == 0) {
      alias = read_dbus_variant_string(&property);
    } else if (std::strcmp(property_name, "Modalias") == 0) {
      modalias = read_dbus_variant_string(&property);
    } else if (std::strcmp(property_name, "Paired") == 0) {
      paired = read_dbus_variant_bool(&property).value_or(false);
    } else if (std::strcmp(property_name, "Connected") == 0) {
      connected = read_dbus_variant_bool(&property).value_or(false);
    }

    ::dbus_message_iter_next(properties);
  }

  if (!paired || !address || !modalias) {
    return std::nullopt;
  }

  const auto profile = vds::controller_profile_from_modalias(*modalias);
  if (!profile) {
    return std::nullopt;
  }

  std::string display_name = name.value_or(alias.value_or(""));
  if (display_name.empty()) {
    display_name = *address;
  }
  return vds::ControllerTarget{
      .address = *address,
      .name = std::move(display_name),
      .profile = *profile,
      .online = connected,
  };
}

std::optional<std::string> find_device_path(DBusConnection *connection,
                                            const std::string &address) {
  DBusMessagePtr call(::dbus_message_new_method_call(
      kBluezService, kBluezRootPath, kDbusObjectManagerInterface,
      "GetManagedObjects"));
  if (!call) {
    throw std::runtime_error("failed to allocate BlueZ D-Bus method call");
  }

  DBusMessagePtr reply =
      call_dbus_method(connection, call.get(), "failed to query BlueZ objects");
  DBusMessageIter root;
  if (!::dbus_message_iter_init(reply.get(), &root)) {
    return std::nullopt;
  }
  require_message_type(&root, DBUS_TYPE_ARRAY, "BlueZ object list");

  DBusMessageIter objects;
  ::dbus_message_iter_recurse(&root, &objects);
  while (::dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_INVALID) {
    require_message_type(&objects, DBUS_TYPE_DICT_ENTRY, "BlueZ object");

    DBusMessageIter object;
    ::dbus_message_iter_recurse(&objects, &object);
    require_message_type(&object, DBUS_TYPE_OBJECT_PATH, "BlueZ object path");
    const char *object_path = nullptr;
    ::dbus_message_iter_get_basic(&object, &object_path);
    if (object_path == nullptr || !::dbus_message_iter_next(&object)) {
      throw std::runtime_error("malformed BlueZ object entry");
    }
    require_message_type(&object, DBUS_TYPE_ARRAY, "BlueZ interface list");

    DBusMessageIter interfaces;
    ::dbus_message_iter_recurse(&object, &interfaces);
    while (::dbus_message_iter_get_arg_type(&interfaces) != DBUS_TYPE_INVALID) {
      require_message_type(&interfaces, DBUS_TYPE_DICT_ENTRY,
                           "BlueZ interface entry");

      DBusMessageIter interface_entry;
      ::dbus_message_iter_recurse(&interfaces, &interface_entry);
      const char *interface =
          read_dbus_string(&interface_entry, "BlueZ interface name");
      if (!::dbus_message_iter_next(&interface_entry)) {
        throw std::runtime_error("malformed BlueZ interface entry");
      }
      require_message_type(&interface_entry, DBUS_TYPE_ARRAY,
                           "BlueZ interface properties");

      if (std::strcmp(interface, kBluezDeviceInterface) == 0) {
        DBusMessageIter properties;
        ::dbus_message_iter_recurse(&interface_entry, &properties);
        if (read_device_address(&properties) == address) {
          return object_path;
        }
      }

      ::dbus_message_iter_next(&interfaces);
    }

    ::dbus_message_iter_next(&objects);
  }

  return std::nullopt;
}

std::optional<std::string>
get_device_string_property(DBusConnection *connection, const std::string &path,
                           const char *property) {
  DBusMessagePtr call(::dbus_message_new_method_call(
      kBluezService, path.c_str(), kDbusPropertiesInterface, "Get"));
  if (!call) {
    throw std::runtime_error("failed to allocate BlueZ property call");
  }

  const char *interface = kBluezDeviceInterface;
  if (!::dbus_message_append_args(call.get(), DBUS_TYPE_STRING, &interface,
                                  DBUS_TYPE_STRING, &property,
                                  DBUS_TYPE_INVALID)) {
    throw std::runtime_error("failed to append BlueZ property arguments");
  }

  DBusErrorGuard error;
  DBusMessage *raw_reply = ::dbus_connection_send_with_reply_and_block(
      connection, call.get(), DBUS_TIMEOUT_USE_DEFAULT, error.get());
  if (raw_reply == nullptr) {
    if (error.has_name(kDbusUnknownPropertyError)) {
      return std::nullopt;
    }
    throw std::runtime_error("failed to query BlueZ property: " +
                             error.message());
  }
  DBusMessagePtr reply(raw_reply);

  DBusMessageIter root;
  if (!::dbus_message_iter_init(reply.get(), &root)) {
    return std::nullopt;
  }
  return read_dbus_variant_string(&root);
}

} // namespace

namespace vds {

std::optional<std::string> bluez_device_modalias(const std::string &address) {
  const std::string normalized_address = normalize_bluetooth_address(address);
  DBusConnectionPtr connection = open_system_bus();
  const auto path = find_device_path(connection.get(), normalized_address);
  if (!path) {
    return std::nullopt;
  }
  return get_device_string_property(connection.get(), *path, "Modalias");
}

std::vector<ControllerTarget> list_bluez_controller_targets() {
  DBusConnectionPtr connection = open_system_bus();
  DBusMessagePtr call(::dbus_message_new_method_call(
      kBluezService, kBluezRootPath, kDbusObjectManagerInterface,
      "GetManagedObjects"));
  if (!call) {
    throw std::runtime_error("failed to allocate BlueZ D-Bus method call");
  }

  DBusMessagePtr reply = call_dbus_method(connection.get(), call.get(),
                                          "failed to query BlueZ objects");
  DBusMessageIter root;
  if (!::dbus_message_iter_init(reply.get(), &root)) {
    return {};
  }
  require_message_type(&root, DBUS_TYPE_ARRAY, "BlueZ object list");

  std::vector<ControllerTarget> targets;
  DBusMessageIter objects;
  ::dbus_message_iter_recurse(&root, &objects);
  while (::dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_INVALID) {
    require_message_type(&objects, DBUS_TYPE_DICT_ENTRY, "BlueZ object");

    DBusMessageIter object;
    ::dbus_message_iter_recurse(&objects, &object);
    require_message_type(&object, DBUS_TYPE_OBJECT_PATH, "BlueZ object path");
    if (!::dbus_message_iter_next(&object)) {
      throw std::runtime_error("malformed BlueZ object entry");
    }
    require_message_type(&object, DBUS_TYPE_ARRAY, "BlueZ interface list");

    DBusMessageIter interfaces;
    ::dbus_message_iter_recurse(&object, &interfaces);
    while (::dbus_message_iter_get_arg_type(&interfaces) != DBUS_TYPE_INVALID) {
      require_message_type(&interfaces, DBUS_TYPE_DICT_ENTRY,
                           "BlueZ interface entry");

      DBusMessageIter interface_entry;
      ::dbus_message_iter_recurse(&interfaces, &interface_entry);
      const char *interface =
          read_dbus_string(&interface_entry, "BlueZ interface name");
      if (!::dbus_message_iter_next(&interface_entry)) {
        throw std::runtime_error("malformed BlueZ interface entry");
      }
      require_message_type(&interface_entry, DBUS_TYPE_ARRAY,
                           "BlueZ interface properties");

      if (std::strcmp(interface, kBluezDeviceInterface) == 0) {
        DBusMessageIter properties;
        ::dbus_message_iter_recurse(&interface_entry, &properties);
        if (auto target = read_controller_target(&properties)) {
          targets.push_back(std::move(*target));
        }
      }

      ::dbus_message_iter_next(&interfaces);
    }

    ::dbus_message_iter_next(&objects);
  }
  return targets;
}

bool disconnect_bluez_device(const std::string &address) {
  const std::string normalized_address = normalize_bluetooth_address(address);
  DBusConnectionPtr connection = open_system_bus();
  const auto path = find_device_path(connection.get(), normalized_address);
  if (!path) {
    return false;
  }

  DBusMessagePtr call(::dbus_message_new_method_call(
      kBluezService, path->c_str(), kBluezDeviceInterface, "Disconnect"));
  if (!call) {
    throw std::runtime_error("failed to allocate BlueZ disconnect call");
  }
  (void)call_dbus_method(connection.get(), call.get(),
                         "failed to disconnect BlueZ device");
  return true;
}

} // namespace vds
