// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_presentation_phase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

// From the presentation-time protocol. The values are duplicated here so the
// scheduling code does not depend on the generated protocol header.
static constexpr uint32_t kPresentedFixedRate = 0x10;
static constexpr uint32_t kPresentedVariableRate = 0x20;

static gboolean presentation_phase_debug_enabled() {
  static gsize initialized = 0;
  static gboolean enabled = FALSE;
  if (g_once_init_enter(&initialized)) {
    const gchar* value = g_getenv("FLUTTER_LINUX_PRESENTATION_PHASE_DEBUG");
    enabled =
        value != nullptr && value[0] != '\0' && g_strcmp0(value, "0") != 0;
    g_once_init_leave(&initialized, 1);
  }
  return enabled;
}

FlPresentationPhase::FlPresentationPhase(gint64 interval_us)
    : interval_us_(interval_us > 0 ? interval_us : 16667) {}

FlPresentationPhase::~FlPresentationPhase() = default;

bool FlPresentationPhase::SetInterval(gint64 interval_us) {
  if (interval_us <= 0) {
    interval_us = 16667;
  }
  if (interval_us == interval_us_) {
    return false;
  }
  const bool was_enabled = enabled_;
  if (presentation_phase_debug_enabled()) {
    g_debug("FlPresentationPhase: interval %" G_GINT64_FORMAT
            " -> %" G_GINT64_FORMAT,
            interval_us_, interval_us);
  }
  interval_us_ = interval_us;
  Reset();
  // A variable refresh output stays disabled across rate changes. An explicit
  // Reset() (a new surface or a mode change reported elsewhere) re-enables it.
  enabled_ = was_enabled;
  return true;
}

void FlPresentationPhase::Reset() {
  if (presentation_phase_debug_enabled()) {
    g_debug("FlPresentationPhase: reset, interval %" G_GINT64_FORMAT "us",
            interval_us_);
  }
  ClearSamples();
  offset_us_ = 0;
  best_latency_us_ = 0;
  best_offset_us_ = 0;
  last_sample_us_ = 0;
  upper_offset_us_ = 0;
  bisect_probes_ = 0;
  crossed_ = false;
  rebaseline_ = false;
  regression_probes_ = 0;
  search_start_offset_us_ = 0;
  probing_earlier_ = false;
  probe_from_offset_us_ = 0;
  probe_baseline_us_ = 0;
  last_probe_us_ = 0;
  skip_samples_ = 0;
  enabled_ = true;
  frozen_ = false;
}

void FlPresentationPhase::ClearSamples() {
  sample_count_ = 0;
  discard_count_ = 0;
}

gint64 FlPresentationPhase::ClampOffset(gint64 offset_us) const {
  // A search walks at most one period past the phase it started from.
  return CLAMP(offset_us, static_cast<gint64>(0), 2 * interval_us_ - 1);
}

gint64 FlPresentationPhase::WrapOffset(gint64 offset_us) const {
  return ((offset_us % interval_us_) + interval_us_) % interval_us_;
}

gint64 FlPresentationPhase::TargetLatencyUs() const {
  // The commit should be close to the compositor's deadline so it is shown on
  // the nearest refresh, but not so close that scheduling jitter makes it miss.
  // This is a heuristic; the improvement test below decides whether moving
  // towards it actually helps on this compositor.
  return CLAMP(interval_us_ / 8, static_cast<gint64>(750), interval_us_ / 4);
}

gint64 FlPresentationPhase::MaxStepUs() const {
  return CLAMP(interval_us_ / 8, static_cast<gint64>(250),
               static_cast<gint64>(2000));
}

gint64 FlPresentationPhase::SafetyMarginUs() const {
  // Covers the measured commit time jitter (up to ~0.7ms at 240Hz) plus the
  // bisection resolution. Low refresh rates see more jitter and can afford a
  // larger margin.
  return MIN(MAX(kSafetyMarginUs, interval_us_ / 8), interval_us_ / 4);
}

gint64 FlPresentationPhase::EarlierProbeStepUs() const {
  // Less than half a period, so the engine never skips a refresh for it.
  return interval_us_ * 3 / 8;
}

gint64 FlPresentationPhase::RegressionThresholdUs() const {
  // While frozen the phase is kept unless the latency is worse than expected
  // by more than the margin. Less than that is noise; more means the deadline
  // moved later and the phase can be closer to it again.
  return SafetyMarginUs();
}

gint64 FlPresentationPhase::MedianSample() const {
  if (sample_count_ == 0) {
    return 0;
  }
  std::array<gint64, kSampleWindow> sorted = samples_;
  std::sort(sorted.begin(), sorted.begin() + sample_count_);
  return sorted[sample_count_ / 2];
}

bool FlPresentationPhase::Settle(gint64 now_us) {
  if (presentation_phase_debug_enabled()) {
    g_debug("FlPresentationPhase: settled at offset %" G_GINT64_FORMAT
            "us, latency %" G_GINT64_FORMAT "us, target %" G_GINT64_FORMAT
            "us, interval %" G_GINT64_FORMAT "us",
            best_offset_us_, best_latency_us_, TargetLatencyUs(), interval_us_);
  }
  frozen_ = true;
  upper_offset_us_ = 0;
  bisect_probes_ = 0;
  last_probe_us_ = now_us;
  ClearSamples();
  // A search that moved the phase later stopped at the deadline or at a target
  // that can be right at it; frame time jitter must not push commits over it.
  if (crossed_ || best_offset_us_ != search_start_offset_us_) {
    best_offset_us_ = WrapOffset(best_offset_us_ - SafetyMarginUs());
  }
  crossed_ = false;
  regression_probes_ = 0;
  rebaseline_ = true;

  return SetOffset(best_offset_us_);
}

bool FlPresentationPhase::BackOff() {
  const gint64 next_offset = WrapOffset(offset_us_ - SafetyMarginUs());
  if (presentation_phase_debug_enabled()) {
    g_debug(
        "FlPresentationPhase: settled phase missed the deadline, offset "
        "%" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT "us",
        offset_us_, next_offset);
  }

  ClearSamples();
  best_offset_us_ = next_offset;
  rebaseline_ = true;

  return SetOffset(next_offset);
}

bool FlPresentationPhase::AddDiscard() {
  if (!enabled_ || interval_us_ <= 0 || skip_samples_ > 0) {
    return false;
  }

  discard_count_++;
  if (!frozen_ || probing_earlier_ || discard_count_ < kMissedFrameLimit) {
    return false;
  }

  return BackOff();
}

bool FlPresentationPhase::AddSample(gint64 latency_us,
                                    gint64 refresh_us,
                                    uint32_t flags,
                                    gint64 now_us) {
  if (interval_us_ <= 0) {
    return false;
  }

  // A variable refresh output has no useful fixed phase to lock to. Leave the
  // natural grid alone and forget any phase learned from a fixed-rate mode.
  if ((flags & kPresentedVariableRate) != 0) {
    const bool changed = SetOffset(0);
    if (enabled_) {
      enabled_ = false;
      ClearSamples();
    }
    return changed;
  }

  if (!enabled_) {
    return false;
  }

  // Compositors may report a slightly different interval than GDK (fractional
  // rates, rounding). Reject samples from a different mode rather than
  // learning a phase for the wrong refresh period.
  if (refresh_us > 0) {
    const gint64 tolerance = MAX(interval_us_ / 10, static_cast<gint64>(100));
    if (std::llabs(refresh_us - interval_us_) > tolerance) {
      return false;
    }
  } else if ((flags & kPresentedFixedRate) != 0) {
    // The protocol says a fixed-rate output must report a refresh interval.
    return false;
  }

  // A commit can legitimately be presented several refreshes later on a busy
  // or triple-buffered compositor, but a value outside this range is more
  // likely a clock mismatch or a stale sample.
  if (latency_us <= 0 || latency_us > interval_us_ * 4) {
    return false;
  }

  if (last_sample_us_ != 0 && now_us - last_sample_us_ > interval_us_ * 4) {
    // The window was hidden or the compositor stopped sending feedback. The
    // learned offset may still be useful, but old samples are not.
    ClearSamples();
  }
  last_sample_us_ = now_us;

  if (skip_samples_ > 0) {
    skip_samples_--;
    return false;
  }

  samples_[sample_count_++] = latency_us;
  if (sample_count_ < kSampleWindow) {
    return false;
  }

  const gint64 median_us = MedianSample();
  // A probe that crosses the compositor deadline presents its frames a refresh
  // late, or the next commit replaces them and they are discarded. Require
  // more than one such frame so a single stall (shader compilation, a garbage
  // collection) does not abort a search or freeze the controller at a bad
  // phase.
  int late_count = 0;
  for (int i = 0; i < sample_count_; i++) {
    if (samples_[i] > median_us + interval_us_ / 2) {
      late_count++;
    }
  }
  const bool missed =
      late_count >= kMissedFrameLimit || discard_count_ >= kMissedFrameLimit;
  ClearSamples();

  if (median_us <= 0 || median_us > interval_us_ * 4) {
    return false;
  }

  if (probing_earlier_) {
    probing_earlier_ = false;
    last_probe_us_ = now_us;

    // Moving the commit earlier can only lower the latency by most of a
    // refresh if the settled phase had fallen past a deadline that moved
    // earlier. The deadline is then between the probe and the settled phase.
    if (!missed && median_us + interval_us_ / 2 < probe_baseline_us_) {
      if (presentation_phase_debug_enabled()) {
        g_debug(
            "FlPresentationPhase: earlier probe lowered latency "
            "%" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT "us, searching again",
            probe_baseline_us_, median_us);
      }
      frozen_ = false;
      crossed_ = true;
      search_start_offset_us_ = probe_from_offset_us_;
      best_latency_us_ = median_us;
      best_offset_us_ = offset_us_;
      upper_offset_us_ = offset_us_ + EarlierProbeStepUs();
      bisect_probes_ = 1;

      return SetOffset((best_offset_us_ + upper_offset_us_) / 2);
    }

    // A probe for a late phase stays earlier; the next window probes further.
    if (regression_probes_ > 0) {
      return false;
    }

    return SetOffset(probe_from_offset_us_);
  }

  const gint64 target_us = TargetLatencyUs();
  // A probe that crosses the deadline is never an improvement, even if its
  // median happens to look good. The first window has no baseline yet, so a
  // startup stall in it only sets the baseline.
  const bool improved =
      best_latency_us_ == 0 ||
      (!missed && median_us + kImprovementDeadbandUs < best_latency_us_);

  if (frozen_) {
    // The phase was settled. Missed frames step it back. A latency change at
    // the same phase means the deadline moved, and the phase follows it
    // without probing past the deadline again.
    if (missed) {
      return BackOff();
    }

    // The window after a phase change measures what that phase gives, unless
    // the phase is still a refresh late.
    const bool rebaseline = rebaseline_;
    rebaseline_ = false;
    if (rebaseline && median_us <= best_latency_us_ + interval_us_ / 2) {
      best_latency_us_ = median_us;
      return false;
    }

    if (median_us <= best_latency_us_ + RegressionThresholdUs()) {
      // A lower latency at the same phase means the deadline moved earlier,
      // so the commit is closer to it. Restore the distance.
      const gint64 drop_us = best_latency_us_ - median_us;
      if (drop_us > SafetyMarginUs() / 2) {
        if (presentation_phase_debug_enabled()) {
          g_debug("FlPresentationPhase: latency dropped by %" G_GINT64_FORMAT
                  "us, moving earlier",
                  drop_us);
        }
        ClearSamples();
        best_offset_us_ = WrapOffset(offset_us_ - drop_us);
        rebaseline_ = true;

        return SetOffset(best_offset_us_);
      }

      // A compositor can move its deadline earlier at once, for example when
      // it leaves triple buffering. Every frame is then shown a refresh late,
      // which looks stable. An earlier phase cannot miss a deadline, so it is
      // a safe probe for that case. A latency below one refresh would show a
      // late phase as a regression, so it needs no probe.
      if (median_us <= interval_us_ ||
          now_us - last_probe_us_ < kEarlierProbeIntervalUs) {
        return false;
      }
      probing_earlier_ = true;
      probe_from_offset_us_ = offset_us_;
      probe_baseline_us_ = median_us;

      return SetOffset(WrapOffset(offset_us_ - EarlierProbeStepUs()));
    }
    // About a refresh worse means the deadline moved earlier past the phase.
    // Searching later would walk the whole period with late frames and cross
    // the deadline at the end; moving earlier finds it without a miss.
    if (median_us > best_latency_us_ + interval_us_ / 2 &&
        regression_probes_ < kMaxRegressionProbes) {
      regression_probes_++;
      probing_earlier_ = true;
      probe_from_offset_us_ = offset_us_;
      probe_baseline_us_ = median_us;

      return SetOffset(WrapOffset(offset_us_ - EarlierProbeStepUs()));
    }
    if (presentation_phase_debug_enabled()) {
      g_debug("FlPresentationPhase: frozen phase regressed to %" G_GINT64_FORMAT
              "us (best %" G_GINT64_FORMAT "us), searching again",
              median_us, best_latency_us_);
    }
    frozen_ = false;
    crossed_ = false;
    best_latency_us_ = 0;
    upper_offset_us_ = 0;
    bisect_probes_ = 0;
    offset_us_ = WrapOffset(offset_us_);
    search_start_offset_us_ = offset_us_;
  }

  if (best_latency_us_ != 0 &&
      (missed || median_us > best_latency_us_ + interval_us_ / 2)) {
    crossed_ = true;
  }

  if (best_latency_us_ == 0 || improved) {
    best_latency_us_ = median_us;
    best_offset_us_ = offset_us_;

    if (median_us <= target_us) {
      // The commit is in the useful part of the refresh period. Hold this
      // phase instead of probing further and risking a missed refresh.
      return Settle(now_us);
    }

    // A probe past the deadline means the useful phase is between the best
    // offset and this one. Bisect instead of throwing away the improvement
    // that was already measured.
    if (upper_offset_us_ > best_offset_us_) {
      const gint64 midpoint = (best_offset_us_ + upper_offset_us_) / 2;
      if (bisect_probes_ >= kMaxBisectProbes ||
          midpoint - best_offset_us_ <= kProbeResolutionUs) {
        return Settle(now_us);
      }
      bisect_probes_++;
      return SetOffset(midpoint);
    }

    // Move the commit later. The error halves each window, so the last steps
    // are fine enough to stop close to the target without overshooting.
    const gint64 error_us = median_us - target_us;
    const gint64 step_us =
        CLAMP(error_us / 2, static_cast<gint64>(1), MaxStepUs());
    const gint64 next_offset = ClampOffset(offset_us_ + step_us);
    if (next_offset == offset_us_) {
      // Already at the latest representable phase. The compositor always
      // queues the frame for at least a period, so there is nothing more to
      // gain.
      return Settle(now_us);
    }
    if (presentation_phase_debug_enabled()) {
      g_debug("FlPresentationPhase: latency %" G_GINT64_FORMAT
              "us, target %" G_GINT64_FORMAT "us, offset %" G_GINT64_FORMAT
              " -> %" G_GINT64_FORMAT "us",
              median_us, target_us, offset_us_, next_offset);
    }
    return SetOffset(next_offset);
  }

  // The step made the latency worse, so the commit crossed the compositor
  // deadline between the best offset and this one. Bisect to find the latest
  // offset that still presents on the nearest refresh.
  if (offset_us_ > best_offset_us_) {
    upper_offset_us_ = offset_us_;
    const gint64 midpoint = (best_offset_us_ + upper_offset_us_) / 2;
    if (bisect_probes_ < kMaxBisectProbes &&
        midpoint - best_offset_us_ > kProbeResolutionUs) {
      bisect_probes_++;
      return SetOffset(midpoint);
    }
  }

  if (presentation_phase_debug_enabled()) {
    g_debug("FlPresentationPhase: latency %" G_GINT64_FORMAT
            "us is worse than best %" G_GINT64_FORMAT
            "us, settling at offset %" G_GINT64_FORMAT "us",
            median_us, best_latency_us_, best_offset_us_);
  }
  return Settle(now_us);
}

bool FlPresentationPhase::SetOffset(gint64 offset_us) {
  offset_us = ClampOffset(offset_us);
  if (offset_us == offset_us_) {
    return false;
  }
  offset_us_ = offset_us;
  // Frames already in the pipeline were started with the previous phase.
  // Ignore the next few samples so a step is judged on frames it affected.
  skip_samples_ = kSkipSamplesAfterStep;
  if (presentation_phase_debug_enabled()) {
    g_debug("FlPresentationPhase: offset %" G_GINT64_FORMAT
            "us, interval %" G_GINT64_FORMAT "us",
            offset_us_, interval_us_);
  }
  return true;
}
