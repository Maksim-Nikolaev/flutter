// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_TRACKER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_TRACKER_H_

#include <gtk/gtk.h>

struct wl_surface;

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlPresentationTracker,
                     fl_presentation_tracker,
                     FL,
                     PRESENTATION_TRACKER,
                     GObject)

typedef struct _FlEngine FlEngine;
typedef struct _FlWaylandDisplay FlWaylandDisplay;

/**
 * FlPresentationTracker:
 *
 * #FlPresentationTracker requests presentation feedback for commits on a
 * Wayland surface and reports the resulting commit-to-presentation latency to
 * an #FlEngine.
 *
 * The tracker is disabled if the compositor does not advertise
 * wp_presentation, or if its presentation clock is not CLOCK_MONOTONIC. In
 * that case all operations are no-ops and the engine keeps its natural frame
 * grid.
 */

/**
 * fl_presentation_tracker_new:
 * @engine: the #FlEngine to report samples to.
 * @display: the #FlWaylandDisplay the subsurface belongs to.
 *
 * Creates a tracker for @display. The tracker keeps a reference to @display so
 * the presentation proxy stays alive.
 *
 * Returns: a new #FlPresentationTracker.
 */
FlPresentationTracker* fl_presentation_tracker_new(FlEngine* engine,
                                                   FlWaylandDisplay* display);

/**
 * fl_presentation_tracker_record_commit:
 * @tracker: an #FlPresentationTracker.
 * @surface: the Wayland surface that is about to be committed.
 *
 * Requests presentation feedback for the next commit of @surface. Must be
 * called immediately before the commit, from the thread making the commit.
 */
void fl_presentation_tracker_record_commit(FlPresentationTracker* tracker,
                                           struct wl_surface* surface);

/**
 * fl_presentation_tracker_set_enabled:
 * @tracker: an #FlPresentationTracker.
 * @enabled: whether feedback should be used.
 *
 * Disables feedback while the subsurface is synchronized with its parent
 * surface, when the parent commit determines when the frame is shown.
 */
void fl_presentation_tracker_set_enabled(FlPresentationTracker* tracker,
                                         gboolean enabled);

/**
 * fl_presentation_tracker_reset:
 * @tracker: an #FlPresentationTracker.
 *
 * Cancels pending feedback and forgets the current run. Called when the
 * surface is hidden, recreated, or resized.
 */
void fl_presentation_tracker_reset(FlPresentationTracker* tracker);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_TRACKER_H_
