// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_PHASE_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_PHASE_H_

#include <glib.h>

#include <array>
#include <cstdint>

// Adjusts the phase of the frame grid from Wayland presentation feedback.
//
// The engine starts frames on a monotonic grid of display refresh periods. That
// grid has an arbitrary phase relative to the compositor's vertical blanking
// interval, so a commit can arrive just after the compositor's deadline for a
// refresh and then waits almost a whole refresh before it is shown.
// Presentation feedback reports the time each committed surface actually
// reached the screen, which gives the commit-to-presentation latency.
//
// Starting a frame later also commits it later, which reduces the
// commit-to-presentation latency until the commit passes the compositor's
// deadline. At that point the frame is presented on the following refresh, or
// the next commit replaces it and it is discarded. The best phase is therefore
// as late as possible without missing the deadline. This class walks the grid
// later while the latency improves, halves the step as it approaches the
// target, and settles a safety margin before the best phase it measured when a
// step crosses the deadline. Once settled it moves earlier by that margin when
// frames miss the deadline, and searches again when the latency at that phase
// regresses, which means the compositor or the workload changed.
//
// The engine snaps the grid to the presentation times, so a settled offset is
// relative to the real vblank and does not drift.
//
// The offset is a delay: it moves the frame start (and therefore the commit)
// later than the natural grid. It is a phase, so it wraps around at one
// refresh period; the engine never starts two frames in one period when it
// wraps.
//
// This class can be used from tests with a synthetic clock. It must be used
// from a single thread in production; the engine calls it on the GTK thread.
class FlPresentationPhase {
 public:
  explicit FlPresentationPhase(gint64 interval_us);
  ~FlPresentationPhase();

  // Sets the display refresh interval. Returns true if the interval changed,
  // in which case the learned phase is discarded.
  bool SetInterval(gint64 interval_us);

  // Adds a commit-to-presentation latency sample in microseconds, and the
  // compositor's predicted refresh interval (zero if unknown). [flags] are
  // WP_PRESENTATION_FEEDBACK_KIND_* values. Returns true if the phase offset
  // changed.
  bool AddSample(gint64 latency_us,
                 gint64 refresh_us,
                 uint32_t flags,
                 gint64 now_us);

  // Reports a commit that a later commit replaced before it was shown, which
  // means it missed the compositor deadline. Returns true if the phase offset
  // changed.
  bool AddDiscard();

  // Discards learned state and returns the grid to its natural phase.
  void Reset();

  // How far later than the natural grid the next frame should start. This is
  // applied to the vsync delivery time, so it moves the commit, not only the
  // timestamps reported to the engine.
  gint64 offset_us() const { return WrapOffset(offset_us_); }

  // False when the controller must not be used, e.g. on a variable refresh
  // rate output or if the compositor reported an unusable clock.
  bool enabled() const { return enabled_; }

 private:
  // Number of samples used to evaluate one step.
  static constexpr int kSampleWindow = 8;
  // A step must reduce the latency by more than this to count as an
  // improvement.
  static constexpr gint64 kImprovementDeadbandUs = 150;
  // Bisection stops when the probes are this close.
  static constexpr gint64 kProbeResolutionUs = 250;
  // Frames ignored after a phase step. A step later makes the compositor queue
  // and then discard a few frames before the new phase is stable.
  static constexpr int kSkipSamplesAfterStep = 6;
  // Minimum distance kept from the deadline found by the search.
  static constexpr gint64 kSafetyMarginUs = 900;
  // Maximum number of bisection probes after a step overshoots the deadline.
  static constexpr int kMaxBisectProbes = 6;
  // Late or discarded frames in one window that count as a missed deadline.
  static constexpr int kMissedFrameLimit = 2;
  // Time between earlier probes while settled.
  static constexpr gint64 kEarlierProbeIntervalUs = 10 * 1000000;
  // Earlier probes for a phase that became a refresh late before searching.
  static constexpr int kMaxRegressionProbes = 3;

  gint64 TargetLatencyUs() const;
  gint64 MaxStepUs() const;
  gint64 SafetyMarginUs() const;
  gint64 EarlierProbeStepUs() const;
  gint64 RegressionThresholdUs() const;
  gint64 ClampOffset(gint64 offset_us) const;
  gint64 WrapOffset(gint64 offset_us) const;
  bool SetOffset(gint64 offset_us);
  bool Settle(gint64 now_us);
  bool BackOff();
  void ClearSamples();
  gint64 MedianSample() const;

  gint64 interval_us_ = 0;
  gint64 offset_us_ = 0;
  gint64 best_latency_us_ = 0;
  gint64 best_offset_us_ = 0;
  gint64 last_sample_us_ = 0;

  // Earliest probe known to be past the compositor deadline. Zero while the
  // search is still walking later.
  gint64 upper_offset_us_ = 0;
  int bisect_probes_ = 0;
  // True once a probe in this search was seen past the deadline.
  bool crossed_ = false;
  gint64 search_start_offset_us_ = 0;

  // An earlier probe tests a settled phase for a deadline that moved earlier.
  bool probing_earlier_ = false;
  gint64 probe_from_offset_us_ = 0;
  gint64 probe_baseline_us_ = 0;
  gint64 last_probe_us_ = 0;
  int regression_probes_ = 0;
  // True while the latency of a newly settled phase is not measured yet.
  bool rebaseline_ = false;

  std::array<gint64, kSampleWindow> samples_ = {};
  int sample_count_ = 0;
  int discard_count_ = 0;

  int skip_samples_ = 0;
  bool enabled_ = true;
  bool frozen_ = false;
};

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_PRESENTATION_PHASE_H_
