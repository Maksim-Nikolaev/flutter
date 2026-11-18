// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_presentation_tracker.h"

#include <time.h>
#include <wayland-client.h>

#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_wayland_display.h"
#include "flutter/shell/platform/linux/presentation_time_client_protocol.h"

struct _FlPresentationTracker {
  GObject parent_instance;

  // Engine to report samples to. Weak, the engine outlives the renderer but
  // shutdown must not be blocked by pending feedback.
  GWeakRef engine;

  // Display that owns the presentation proxy.
  FlWaylandDisplay* display;

  // Clock used by the presentation proxy. 0 if the compositor has not
  // reported it.
  uint32_t clock_id;

  // TRUE if @presentation exists and uses CLOCK_MONOTONIC.
  gboolean supported;

  // FALSE while the subsurface is synchronized with its parent or after the
  // owner asked to pause feedback. Accessed from the raster and GTK threads.
  gint enabled;

  // Guards @pending, which is written by the raster thread and read by the GTK
  // thread.
  GMutex mutex;

  // Feedback objects that have been requested but not completed. Keys are
  // struct wp_presentation_feedback*, values are FlPresentationSample*.
  GHashTable* pending;
};

typedef struct {
  FlPresentationTracker* tracker;
  gint64 commit_time_us;
} FlPresentationSample;

G_DEFINE_TYPE(FlPresentationTracker, fl_presentation_tracker, G_TYPE_OBJECT)

static void presentation_sample_free(gpointer data) {
  g_free(data);
}

// Allows distributions and users to turn the phase controller off for
// debugging without rebuilding the engine. Presentation feedback tracing is
// still available separately.
// Per-sample logging is only useful while measuring a compositor; keep it
// separate so the normal debug switch does not flood the log at 240Hz.
static gboolean presentation_tracker_verbose_enabled() {
  static gsize initialized = 0;
  static gboolean enabled = FALSE;
  if (g_once_init_enter(&initialized)) {
    const gchar* value =
        g_getenv("FLUTTER_LINUX_PRESENTATION_PHASE_DEBUG_VERBOSE");
    enabled =
        value != nullptr && value[0] != '\0' && g_strcmp0(value, "0") != 0;
    g_once_init_leave(&initialized, 1);
  }
  return enabled;
}

static gboolean presentation_phase_allowed() {
  static gsize initialized = 0;
  static gboolean allowed = TRUE;
  if (g_once_init_enter(&initialized)) {
    const gchar* value = g_getenv("FLUTTER_LINUX_PRESENTATION_PHASE");
    allowed = value == nullptr || g_strcmp0(value, "0") != 0;
    g_once_init_leave(&initialized, 1);
  }
  return allowed;
}

static void feedback_presented_cb(void* data,
                                  struct wp_presentation_feedback* feedback,
                                  uint32_t tv_sec_hi,
                                  uint32_t tv_sec_lo,
                                  uint32_t tv_nsec,
                                  uint32_t refresh,
                                  uint32_t seq_hi,
                                  uint32_t seq_lo,
                                  uint32_t flags) {
  FlPresentationSample* sample = static_cast<FlPresentationSample*>(data);
  FlPresentationTracker* self = sample->tracker;

  const uint64_t tv_sec = (static_cast<uint64_t>(tv_sec_hi) << 32) | tv_sec_lo;
  const gint64 presented_us =
      static_cast<gint64>(tv_sec) * 1000000 + tv_nsec / 1000;

  if (g_atomic_int_get(&self->enabled) && tv_sec <= (1ULL << 40) &&
      tv_nsec < 1000000000 && presented_us > sample->commit_time_us) {
    const gint64 latency_us = presented_us - sample->commit_time_us;
    g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&self->engine));
    if (engine != nullptr) {
      fl_engine_add_presentation_sample(engine, latency_us, refresh / 1000,
                                        flags);
    }
    if (presentation_tracker_verbose_enabled()) {
      g_debug("FlPresentationTracker: latency %" G_GINT64_FORMAT
              "us, refresh %" G_GINT64_FORMAT "us, msc %" G_GUINT64_FORMAT
              ", flags 0x%x",
              latency_us, static_cast<gint64>(refresh / 1000),
              (static_cast<uint64_t>(seq_hi) << 32) | seq_lo, flags);
    }
  }

  g_mutex_lock(&self->mutex);
  g_hash_table_remove(self->pending, feedback);
  g_mutex_unlock(&self->mutex);

  wp_presentation_feedback_destroy(feedback);
}

static void feedback_discarded_cb(void* data,
                                  struct wp_presentation_feedback* feedback) {
  FlPresentationSample* sample = static_cast<FlPresentationSample*>(data);
  FlPresentationTracker* self = sample->tracker;

  g_mutex_lock(&self->mutex);
  g_hash_table_remove(self->pending, feedback);
  g_mutex_unlock(&self->mutex);

  wp_presentation_feedback_destroy(feedback);
}

static void feedback_sync_output_cb(void* data,
                                    struct wp_presentation_feedback* feedback,
                                    struct wl_output* output) {
  (void)data;
  (void)feedback;
  (void)output;
}

static const struct wp_presentation_feedback_listener kFeedbackListener = {
    .sync_output = feedback_sync_output_cb,
    .presented = feedback_presented_cb,
    .discarded = feedback_discarded_cb,
};

static void fl_presentation_tracker_dispose(GObject* object) {
  FlPresentationTracker* self = FL_PRESENTATION_TRACKER(object);

  // The owner normally resets before dropping the last reference. Be
  // defensive and release any remaining feedback proxies; the table frees the
  // associated samples below.
  g_mutex_lock(&self->mutex);
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, self->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    wp_presentation_feedback_destroy(
        static_cast<struct wp_presentation_feedback*>(key));
  }
  g_mutex_unlock(&self->mutex);

  g_weak_ref_clear(&self->engine);
  g_clear_object(&self->display);
  g_clear_pointer(&self->pending, g_hash_table_unref);
  g_mutex_clear(&self->mutex);

  G_OBJECT_CLASS(fl_presentation_tracker_parent_class)->dispose(object);
}

static void fl_presentation_tracker_class_init(
    FlPresentationTrackerClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_presentation_tracker_dispose;
}

static void fl_presentation_tracker_init(FlPresentationTracker* self) {
  g_weak_ref_init(&self->engine, nullptr);
  g_mutex_init(&self->mutex);
  self->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr,
                                        presentation_sample_free);
  g_atomic_int_set(&self->enabled, TRUE);
}

FlPresentationTracker* fl_presentation_tracker_new(FlEngine* engine,
                                                   FlWaylandDisplay* display) {
  g_return_val_if_fail(FL_IS_ENGINE(engine), nullptr);
  g_return_val_if_fail(FL_IS_WAYLAND_DISPLAY(display), nullptr);

  FlPresentationTracker* self = FL_PRESENTATION_TRACKER(
      g_object_new(fl_presentation_tracker_get_type(), nullptr));
  g_weak_ref_set(&self->engine, engine);
  self->display = FL_WAYLAND_DISPLAY(g_object_ref(display));
  self->clock_id = fl_wayland_display_get_presentation_clock_id(display);

  // wp_presentation is optional. If it is missing, or reports a clock that
  // cannot be compared with g_get_monotonic_time(), leave the engine's natural
  // frame grid alone.
  struct wp_presentation* presentation =
      fl_wayland_display_get_presentation(display);
  self->supported = presentation != nullptr &&
                    self->clock_id == CLOCK_MONOTONIC &&
                    presentation_phase_allowed();
  g_atomic_int_set(&self->enabled, self->supported);

  if (!self->supported) {
    g_debug(
        "FlPresentationTracker: presentation feedback disabled "
        "(presentation=%p, clock=%u, allowed=%d)",
        presentation, self->clock_id, presentation_phase_allowed());
  }

  return self;
}

void fl_presentation_tracker_record_commit(FlPresentationTracker* self,
                                           struct wl_surface* surface) {
  g_return_if_fail(FL_IS_PRESENTATION_TRACKER(self));

  if (!g_atomic_int_get(&self->enabled) || surface == nullptr) {
    return;
  }

  // Query the display each time: the global could have been removed, and the
  // display owns the proxy.
  struct wp_presentation* presentation =
      fl_wayland_display_get_presentation(self->display);
  if (presentation == nullptr) {
    return;
  }

  // Feedback objects are one-shot. Bound the number of outstanding ones in
  // case a compositor stops sending events; dropping a request is harmless.
  g_mutex_lock(&self->mutex);
  const guint pending = g_hash_table_size(self->pending);
  g_mutex_unlock(&self->mutex);
  if (pending >= 256) {
    return;
  }

  struct wp_presentation_feedback* feedback =
      wp_presentation_feedback(presentation, surface);
  if (feedback == nullptr) {
    return;
  }

  FlPresentationSample* sample = g_new0(FlPresentationSample, 1);
  sample->tracker = self;
  // The swap that follows performs the commit. Recording the time just before
  // it is within the noise floor of the commit and avoids sampling after a
  // potentially long buffer wait.
  sample->commit_time_us = g_get_monotonic_time();

  // Put the sample in the table before the listener can be called. Events are
  // dispatched on the GTK thread, but the insert keeps that path from racing
  // with the raster thread.
  g_mutex_lock(&self->mutex);
  g_hash_table_insert(self->pending, feedback, sample);
  g_mutex_unlock(&self->mutex);

  if (wp_presentation_feedback_add_listener(feedback, &kFeedbackListener,
                                            sample) != 0) {
    g_mutex_lock(&self->mutex);
    g_hash_table_remove(self->pending, feedback);
    g_mutex_unlock(&self->mutex);
    wp_presentation_feedback_destroy(feedback);
  }
}

void fl_presentation_tracker_set_enabled(FlPresentationTracker* self,
                                         gboolean enable) {
  g_return_if_fail(FL_IS_PRESENTATION_TRACKER(self));

  enable = enable && self->supported;
  if (enable == g_atomic_int_get(&self->enabled)) {
    return;
  }
  g_atomic_int_set(&self->enabled, enable);
  if (!enable) {
    fl_presentation_tracker_reset(self);
  }
}

void fl_presentation_tracker_reset(FlPresentationTracker* self) {
  g_return_if_fail(FL_IS_PRESENTATION_TRACKER(self));

  g_mutex_lock(&self->mutex);
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, self->pending);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    wp_presentation_feedback_destroy(
        static_cast<struct wp_presentation_feedback*>(key));
  }
  g_hash_table_remove_all(self->pending);
  g_mutex_unlock(&self->mutex);
}
