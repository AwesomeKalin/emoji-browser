// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/status_icons/dbus_properties_interface.h"

#include <dbus/dbus-shared.h>

#include "base/bind.h"
#include "base/stl_util.h"
#include "chrome/browser/ui/views/status_icons/success_barrier_callback.h"
#include "dbus/exported_object.h"
#include "dbus/message.h"

namespace {

// Methods.
const char kMethodPropertiesGetAll[] = "GetAll";
const char kMethodPropertiesGet[] = "Get";
const char kMethodPropertiesSet[] = "Set";

// Signals.
const char kSignalPropertiesChanged[] = "PropertiesChanged";

}  // namespace

DbusPropertiesInterface::DbusPropertiesInterface(
    dbus::ExportedObject* exported_object,
    InitializedCallback callback)
    : exported_object_(exported_object), weak_factory_(this) {
  static constexpr struct {
    const char* name;
    void (DbusPropertiesInterface::*callback)(
        dbus::MethodCall*,
        dbus::ExportedObject::ResponseSender);
  } methods[3] = {
      {kMethodPropertiesGetAll, &DbusPropertiesInterface::OnGetAllProperties},
      {kMethodPropertiesGet, &DbusPropertiesInterface::OnGetProperty},
      {kMethodPropertiesSet, &DbusPropertiesInterface::OnSetProperty},
  };

  barrier_ = SuccessBarrierCallback(base::size(methods), std::move(callback));
  for (const auto& method : methods) {
    exported_object_->ExportMethod(
        DBUS_INTERFACE_PROPERTIES, method.name,
        base::BindRepeating(method.callback, weak_factory_.GetWeakPtr()),
        base::BindRepeating(&DbusPropertiesInterface::OnExported,
                            weak_factory_.GetWeakPtr()));
  }
}

DbusPropertiesInterface::~DbusPropertiesInterface() = default;

void DbusPropertiesInterface::RegisterInterface(const std::string& interface) {
  bool inserted =
      properties_.emplace(interface, std::map<std::string, DbusVariant>{})
          .second;
  DCHECK(inserted);
}

void DbusPropertiesInterface::OnExported(const std::string& interface_name,
                                         const std::string& method_name,
                                         bool success) {
  barrier_.Run(success);
}

void DbusPropertiesInterface::OnGetAllProperties(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  // org.freedesktop.DBus.Properties.GetAll(in STRING interface_name,
  //                                        out DICT<STRING,VARIANT> props);
  dbus::MessageReader reader(method_call);
  std::string interface;
  if (!reader.PopString(&interface)) {
    response_sender.Run(nullptr);
    return;
  }

  std::unique_ptr<dbus::Response> response =
      dbus::Response::FromMethodCall(method_call);
  dbus::MessageWriter writer(response.get());

  if (base::Contains(properties_, interface)) {
    dbus::MessageWriter array_writer(nullptr);
    dbus::MessageWriter dict_entry_writer(nullptr);
    writer.OpenArray("{sv}", &array_writer);
    for (const auto& pair : properties_[interface]) {
      array_writer.OpenDictEntry(&dict_entry_writer);
      dict_entry_writer.AppendString(pair.first);
      pair.second.Write(&dict_entry_writer);
      array_writer.CloseContainer(&dict_entry_writer);
    }
    writer.CloseContainer(&array_writer);
  } else if (interface == DBUS_INTERFACE_PROPERTIES) {
    // There are no properties to give for this interface.
    DbusArray<DbusDictEntry<DbusString, DbusVariant>>().Write(&writer);
  } else {
    // The given interface is not supported, so return a null response.
    response = nullptr;
  }

  response_sender.Run(std::move(response));
}

void DbusPropertiesInterface::OnGetProperty(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  // org.freedesktop.DBus.Properties.Get(in STRING interface_name,
  //                                     in STRING property_name,
  //                                     out VARIANT value);
  dbus::MessageReader reader(method_call);
  std::string interface;
  std::string property_name;
  if (!reader.PopString(&interface) || !reader.PopString(&property_name) ||
      !base::Contains(properties_, interface) ||
      !base::Contains(properties_[interface], property_name)) {
    response_sender.Run(nullptr);
    return;
  }

  std::unique_ptr<dbus::Response> response =
      dbus::Response::FromMethodCall(method_call);
  dbus::MessageWriter writer(response.get());
  properties_[interface][property_name].Write(&writer);
  response_sender.Run(std::move(response));
}

void DbusPropertiesInterface::OnSetProperty(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  // Not needed for now.
  NOTIMPLEMENTED();
  response_sender.Run(dbus::Response::FromMethodCall(method_call));
}

void DbusPropertiesInterface::EmitPropertiesChangedSignal(
    const std::string& interface,
    const std::string& property_name,
    bool send_change) {
  // |signal| follows the PropertiesChanged API:
  // org.freedesktop.DBus.Properties.PropertiesChanged(
  //     STRING interface_name,
  //     DICT<STRING,VARIANT> changed_properties,
  //     ARRAY<STRING> invalidated_properties);
  dbus::Signal signal(DBUS_INTERFACE_PROPERTIES, kSignalPropertiesChanged);
  dbus::MessageWriter writer(&signal);
  writer.AppendString(interface);

  if (send_change) {
    // Changed properties.
    dbus::MessageWriter array_writer(nullptr);
    writer.OpenArray("{sv}", &array_writer);
    dbus::MessageWriter dict_entry_writer(nullptr);
    array_writer.OpenDictEntry(&dict_entry_writer);
    dict_entry_writer.AppendString(property_name);
    properties_[interface][property_name].Write(&dict_entry_writer);
    array_writer.CloseContainer(&dict_entry_writer);
    writer.CloseContainer(&array_writer);

    // Invalidated properties.
    writer.AppendArrayOfStrings({});
  } else {
    // Changed properties.
    DbusArray<DbusDictEntry<DbusString, DbusVariant>>().Write(&writer);

    // Invalidated properties.
    writer.AppendArrayOfStrings({property_name});
  }

  exported_object_->SendSignal(&signal);
}
