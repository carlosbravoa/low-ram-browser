// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// lrb_picker: the desktop's file picker (the XDG desktop portal), for the
// coordinator. Confined instances can't open the user's files themselves, so
// the coordinator asks the user through this helper and hands over only
// what was chosen (coordinator.cc, "pick-open" / "pick-save").
//
//   lrb_picker open [--multiple] [--title=T] [--parent=HANDLE]
//   lrb_picker save [--name=FILE] [--title=T] [--parent=HANDLE]
//
// Prints the chosen paths, one per line. Exit status: 0 chosen, 1 cancelled,
// 2 no portal (no session bus, or no FileChooser), 3 bad arguments.
// HANDLE is the portal's parent window identifier ("x11:<hex>",
// "wayland:<handle>"), so the dialog stays on top of the page's window.

// C glue (argv, GLib's string arrays) with no //base for spans: this file
// opts out of Chromium's unsafe-buffer checks (docs/unsafe_buffers.md).
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

struct State {
  GMainLoop* loop = nullptr;
  int status = 1;
  std::vector<std::string> paths;
};

void OnResponse(GDBusConnection*,
                const char*,
                const char*,
                const char*,
                const char*,
                GVariant* parameters,
                gpointer data) {
  State* state = static_cast<State*>(data);
  guint32 response = 2;
  GVariant* results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (response == 0) {
    const char** uris = nullptr;
    if (g_variant_lookup(results, "uris", "^a&s", &uris)) {
      for (const char** uri = uris; *uri; ++uri) {
        GFile* file = g_file_new_for_uri(*uri);
        if (char* path = g_file_get_path(file)) {
          state->paths.push_back(path);
          g_free(path);
        }
        g_object_unref(file);
      }
      g_free(uris);
    }
    state->status = state->paths.empty() ? 1 : 0;
  }
  g_variant_unref(results);
  g_main_loop_quit(state->loop);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || (strcmp(argv[1], "open") && strcmp(argv[1], "save"))) {
    fprintf(stderr, "usage: lrb_picker open|save [options]\n");
    return 3;
  }
  const bool save = !strcmp(argv[1], "save");
  bool multiple = false;
  std::string title = save ? "Save file" : "Open file";
  std::string parent;
  std::string name;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--multiple") {
      multiple = true;
    } else if (arg.starts_with("--title=")) {
      title = arg.substr(8);
    } else if (arg.starts_with("--parent=")) {
      parent = arg.substr(9);
    } else if (arg.starts_with("--name=")) {
      name = arg.substr(7);
    } else {
      fprintf(stderr, "lrb_picker: unknown argument %s\n", arg.c_str());
      return 3;
    }
  }

  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) {
    fprintf(stderr, "lrb_picker: no session bus: %s\n", error->message);
    return 2;
  }

  // The portal answers on a Request object whose path it derives from our
  // bus name and a token we choose: subscribe before calling.
  std::string sender = g_dbus_connection_get_unique_name(bus) + 1;  // no ':'
  for (char& c : sender) {
    if (c == '.') {
      c = '_';
    }
  }
  const std::string token = "lrb" + std::to_string(getpid());
  const std::string request_path =
      "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  State state;
  state.loop = g_main_loop_new(nullptr, false);
  g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
      "Response", request_path.c_str(), nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, OnResponse, &state, nullptr);

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token",
                        g_variant_new_string(token.c_str()));
  g_variant_builder_add(&options, "{sv}", "modal",
                        g_variant_new_boolean(true));
  if (multiple) {
    g_variant_builder_add(&options, "{sv}", "multiple",
                          g_variant_new_boolean(true));
  }
  if (save && !name.empty()) {
    g_variant_builder_add(&options, "{sv}", "current_name",
                          g_variant_new_string(name.c_str()));
  }
  GVariant* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile",
      g_variant_new("(ssa{sv})", parent.c_str(), title.c_str(), &options),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (!reply) {
    fprintf(stderr, "lrb_picker: no file chooser portal: %s\n",
            error->message);
    return 2;
  }
  g_variant_unref(reply);

  g_main_loop_run(state.loop);
  for (const std::string& path : state.paths) {
    printf("%s\n", path.c_str());
  }
  return state.status;
}
