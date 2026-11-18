// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_wayland_display.h"

#include <gdk/gdkwayland.h>
#include <wayland-client.h>

#include "flutter/shell/platform/linux/presentation_time_client_protocol.h"

// Key used to attach the #FlWaylandDisplay to the #GdkDisplay it was created
// for, so the Wayland globals are only bound once per display.
static const char* kWaylandDisplayKey = "fl-wayland-display";

struct _FlWaylandDisplay {
  GObject parent_instance;

  // Wayland globals used to create subsurfaces.
  struct wl_compositor* compositor;
  struct wl_subcompositor* subcompositor;

  // Optional presentation feedback. Null if the compositor does not support
  // wp_presentation, in which case the engine keeps its natural frame grid.
  struct wp_presentation* presentation;
  uint32_t presentation_name;
  uint32_t presentation_clock_id;
  gboolean presentation_clock_id_valid;
};

G_DEFINE_TYPE(FlWaylandDisplay, fl_wayland_display, G_TYPE_OBJECT)

static void presentation_clock_id_cb(void* data,
                                     struct wp_presentation* presentation,
                                     uint32_t clock_id) {
  FlWaylandDisplay* self = FL_WAYLAND_DISPLAY(data);
  (void)presentation;
  self->presentation_clock_id = clock_id;
  self->presentation_clock_id_valid = TRUE;
}

static const struct wp_presentation_listener kPresentationListener = {
    .clock_id = presentation_clock_id_cb,
};

// Wayland registry handling.
static void registry_global(void* data,
                            struct wl_registry* registry,
                            uint32_t name,
                            const char* interface,
                            uint32_t version) {
  FlWaylandDisplay* self = FL_WAYLAND_DISPLAY(data);
  if (g_strcmp0(interface, wl_compositor_interface.name) == 0) {
    self->compositor = static_cast<struct wl_compositor*>(wl_registry_bind(
        registry, name, &wl_compositor_interface, MIN(version, 4)));
  } else if (g_strcmp0(interface, wl_subcompositor_interface.name) == 0) {
    self->subcompositor = static_cast<struct wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  } else if (g_strcmp0(interface, wp_presentation_interface.name) == 0) {
    self->presentation = static_cast<struct wp_presentation*>(wl_registry_bind(
        registry, name, &wp_presentation_interface,
        MIN(version,
            static_cast<uint32_t>(wp_presentation_interface.version))));
    self->presentation_name = name;
    wp_presentation_add_listener(self->presentation, &kPresentationListener,
                                 self);
  }
}

static void registry_global_remove(void* data,
                                   struct wl_registry* registry,
                                   uint32_t name) {
  FlWaylandDisplay* self = FL_WAYLAND_DISPLAY(data);
  (void)registry;
  if (name == self->presentation_name && self->presentation != nullptr) {
    wp_presentation_destroy(self->presentation);
    self->presentation = nullptr;
    self->presentation_name = 0;
    self->presentation_clock_id = 0;
    self->presentation_clock_id_valid = FALSE;
  }
}

static const struct wl_registry_listener kRegistryListener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Binds the Wayland globals required by Flutter. Returns FALSE if any of them
// are not available.
static gboolean bind_globals(FlWaylandDisplay* self,
                             struct wl_display* display) {
  struct wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, self);
  wl_display_roundtrip(display);

  // wp_presentation is bound from the registry callback, after the roundtrip's
  // sync request was sent. A second roundtrip receives its clock_id event, so
  // users of the proxy see a usable presentation clock immediately.
  if (self->presentation != nullptr) {
    wl_display_roundtrip(display);
  }

  wl_registry_destroy(registry);

  return self->compositor != nullptr && self->subcompositor != nullptr;
}

static void fl_wayland_display_dispose(GObject* object) {
  FlWaylandDisplay* self = FL_WAYLAND_DISPLAY(object);

  if (self->presentation != nullptr) {
    wp_presentation_destroy(self->presentation);
    self->presentation = nullptr;
  }
  g_clear_pointer(&self->subcompositor, wl_subcompositor_destroy);
  g_clear_pointer(&self->compositor, wl_compositor_destroy);

  G_OBJECT_CLASS(fl_wayland_display_parent_class)->dispose(object);
}

static void fl_wayland_display_class_init(FlWaylandDisplayClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_wayland_display_dispose;
}

static void fl_wayland_display_init(FlWaylandDisplay* self) {}

FlWaylandDisplay* fl_wayland_display_open(struct wl_display* wl_display) {
  FlWaylandDisplay* self =
      FL_WAYLAND_DISPLAY(g_object_new(fl_wayland_display_get_type(), nullptr));

  if (!bind_globals(self, wl_display)) {
    g_warning("Required Wayland globals not available");
    g_object_unref(self);
    return nullptr;
  }

  return self;
}

FlWaylandDisplay* fl_wayland_display_get_for_display(GdkDisplay* gdk_display) {
  g_return_val_if_fail(GDK_IS_WAYLAND_DISPLAY(gdk_display), nullptr);

  FlWaylandDisplay* self = FL_WAYLAND_DISPLAY(
      g_object_get_data(G_OBJECT(gdk_display), kWaylandDisplayKey));
  if (self != nullptr) {
    return self;
  }

  self =
      fl_wayland_display_open(gdk_wayland_display_get_wl_display(gdk_display));
  if (self == nullptr) {
    return nullptr;
  }

  // Owned by the #GdkDisplay, so the globals are shared by everything using
  // that display and released with it.
  g_object_set_data_full(G_OBJECT(gdk_display), kWaylandDisplayKey, self,
                         g_object_unref);

  return self;
}

FlSubsurface* fl_wayland_display_create_subsurface(
    FlWaylandDisplay* self,
    struct wl_surface* parent_surface) {
  g_return_val_if_fail(FL_IS_WAYLAND_DISPLAY(self), nullptr);

  return fl_subsurface_new(self->compositor, self->subcompositor,
                           parent_surface);
}

struct wp_presentation* fl_wayland_display_get_presentation(
    FlWaylandDisplay* self) {
  g_return_val_if_fail(FL_IS_WAYLAND_DISPLAY(self), nullptr);
  return self->presentation;
}

uint32_t fl_wayland_display_get_presentation_clock_id(FlWaylandDisplay* self) {
  g_return_val_if_fail(FL_IS_WAYLAND_DISPLAY(self), 0);
  return self->presentation_clock_id_valid ? self->presentation_clock_id : 0;
}
