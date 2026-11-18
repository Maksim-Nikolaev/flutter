// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Included first as it collides with the X11 headers.
#include "flutter/shell/platform/linux/testing/linux_test.h"
#include "gtest/gtest.h"

#include <wayland-client.h>

#include "flutter/shell/platform/linux/fl_presentation_tracker.h"
#include "flutter/shell/platform/linux/fl_wayland_display.h"
#include "flutter/shell/platform/linux/testing/mock_wayland.h"

using flutter::testing::fl_mock_wayland_free_surface;
using flutter::testing::fl_mock_wayland_get_display;
using flutter::testing::fl_mock_wayland_get_surface;

class FlPresentationTrackerTest : public flutter::testing::LinuxTest {
 protected:
  ::testing::NiceMock<flutter::testing::MockWayland> wayland;
};

// A compositor without wp_presentation must leave the tracker inert.
TEST_F(FlPresentationTrackerTest, UnsupportedWithoutPresentation) {
  wayland.has_presentation = false;

  g_autoptr(FlWaylandDisplay) display =
      fl_wayland_display_open(fl_mock_wayland_get_display());
  ASSERT_NE(display, nullptr);

  g_autoptr(FlPresentationTracker) tracker =
      fl_presentation_tracker_new(engine, display);
  ASSERT_NE(tracker, nullptr);

  struct wl_surface* surface = fl_mock_wayland_get_surface();
  fl_presentation_tracker_record_commit(tracker, surface);
  fl_presentation_tracker_set_enabled(tracker, FALSE);
  fl_presentation_tracker_reset(tracker);
  fl_mock_wayland_free_surface(surface);
}

// A presentation global whose clock is unknown must not produce feedback.
TEST_F(FlPresentationTrackerTest, UnsupportedWithoutClock) {
  wayland.has_presentation = true;

  g_autoptr(FlWaylandDisplay) display =
      fl_wayland_display_open(fl_mock_wayland_get_display());
  ASSERT_NE(display, nullptr);
  EXPECT_NE(fl_wayland_display_get_presentation(display), nullptr);
  EXPECT_EQ(fl_wayland_display_get_presentation_clock_id(display), 0u);

  g_autoptr(FlPresentationTracker) tracker =
      fl_presentation_tracker_new(engine, display);
  ASSERT_NE(tracker, nullptr);

  struct wl_surface* surface = fl_mock_wayland_get_surface();
  fl_presentation_tracker_record_commit(tracker, surface);
  fl_mock_wayland_free_surface(surface);
}
