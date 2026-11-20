// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_presentation_phase.h"

#include <algorithm>
#include <functional>

#include "gtest/gtest.h"

namespace {

constexpr gint64 kInterval60 = 16667;
constexpr gint64 kInterval144 = 6944;
constexpr gint64 kInterval240 = 4167;

// Distance the controller keeps from a deadline or target it walked to.
gint64 SafetyMargin(gint64 interval_us) {
  return std::min(std::max(static_cast<gint64>(900), interval_us / 8),
                  interval_us / 4);
}

// From the presentation-time protocol.
constexpr uint32_t kFixedRate = 0x10;
constexpr uint32_t kVariableRate = 0x20;

// A compositor that presents a commit on the first blank at least [lead_us]
// after it. At offset zero the commit is [natural_latency_us] before the blank
// that presents it, so starting the frame later reduces the latency until the
// commit passes the blank's deadline, after which it is presented one refresh
// later.
gint64 VblankLatency(gint64 offset_us,
                     gint64 natural_latency_us,
                     gint64 lead_us,
                     gint64 interval_us) {
  gint64 latency_us = natural_latency_us - offset_us - lead_us;
  latency_us %= interval_us;
  if (latency_us < 0) {
    latency_us += interval_us;
  }
  return latency_us + lead_us;
}

void RunSamples(FlPresentationPhase* phase,
                std::function<gint64(gint64)> latency_for_offset,
                gint64 interval_us,
                int frames,
                gint64* now_us) {
  for (int i = 0; i < frames; i++) {
    phase->AddSample(latency_for_offset(phase->offset_us()), interval_us,
                     kFixedRate, *now_us);
    *now_us += interval_us;
  }
}

// Runs the controller and counts frames whose presentation was a refresh
// later than the window's typical latency.
int CountLateFrames(FlPresentationPhase* phase,
                    std::function<gint64(gint64)> latency_for_offset,
                    gint64 interval_us,
                    int frames,
                    gint64* now_us) {
  int late = 0;
  gint64 previous = 0;
  for (int i = 0; i < frames; i++) {
    gint64 latency = latency_for_offset(phase->offset_us());
    if (previous != 0 && latency > previous + interval_us / 2) {
      late++;
    }
    previous = latency;
    phase->AddSample(latency, interval_us, kFixedRate, *now_us);
    *now_us += interval_us;
  }
  return late;
}

TEST(FlPresentationPhaseTest, LateCommitMovesLaterAndSettles) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  // The natural grid commits 14.7ms before the blank it is shown on, so the
  // frame waits almost a full refresh. Starting it later brings the commit
  // closer to the compositor deadline without missing it.
  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 14667, 500, kInterval60);
  };

  RunSamples(&phase, latency, kInterval60, 400, &now_us);

  EXPECT_TRUE(phase.enabled());
  EXPECT_GT(phase.offset_us(), kInterval60 / 2);
  EXPECT_LT(phase.offset_us(), kInterval60);
  const gint64 settled_latency = latency(phase.offset_us());
  EXPECT_LE(settled_latency,
            kInterval60 / 8 + 1000 + SafetyMargin(kInterval60));
  EXPECT_GE(settled_latency, 0);

  // The settled phase must be stable, not oscillate between probes.
  const gint64 converged_offset = phase.offset_us();
  RunSamples(&phase, latency, kInterval60, 300, &now_us);
  EXPECT_EQ(phase.offset_us(), converged_offset);
  EXPECT_EQ(CountLateFrames(&phase, latency, kInterval60, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, CommitAfterDeadlineWalksALargeStepLater) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  // The natural commit lands after the deadline, so every frame is presented a
  // refresh late. The useful phase is almost a full period later.
  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 500, 1000, kInterval60);
  };

  RunSamples(&phase, latency, kInterval60, 600, &now_us);

  EXPECT_GT(phase.offset_us(), kInterval60 * 3 / 4);
  EXPECT_LT(phase.offset_us(), kInterval60);
  const gint64 settled_latency = latency(phase.offset_us());
  EXPECT_LE(settled_latency,
            kInterval60 / 8 + 1000 + SafetyMargin(kInterval60));
  EXPECT_EQ(CountLateFrames(&phase, latency, kInterval60, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, StartupStallsDoNotFreezeTheSearch) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;
  int samples = 0;

  // The first frames of an app are slow (shader compilation, first paint).
  // Those frames must not look like a phase that crossed the deadline, or the
  // controller would stay at the unshifted grid for the rest of the run.
  auto latency = [&samples](gint64 offset) {
    gint64 value = VblankLatency(offset, 14667, 500, kInterval60);
    if (samples++ < 3) {
      value += kInterval60;
    }
    return value;
  };

  RunSamples(&phase, latency, kInterval60, 400, &now_us);

  EXPECT_GT(phase.offset_us(), 0);
  EXPECT_LE(latency(phase.offset_us()),
            kInterval60 / 8 + 1000 + SafetyMargin(kInterval60));
}

TEST(FlPresentationPhaseTest, AlreadyEarlyGridIsLeftAlone) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  // The natural phase already commits well inside the useful window. There is
  // nothing to improve, so the controller must settle immediately.
  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 700, 500, kInterval60);
  };
  RunSamples(&phase, latency, kInterval60, 300, &now_us);

  EXPECT_EQ(phase.offset_us(), 0);
}

TEST(FlPresentationPhaseTest, QueueingCompositorKeepsNaturalPhase) {
  FlPresentationPhase phase(kInterval144);
  gint64 now_us = 1000000;

  // The compositor holds frames for a fixed time regardless of when they are
  // committed. There is no phase that can reduce the latency, so the
  // controller must settle near the natural phase instead of walking a whole
  // period.
  auto latency = [](gint64 offset) {
    (void)offset;
    return kInterval144;
  };
  RunSamples(&phase, latency, kInterval144, 600, &now_us);

  EXPECT_LT(phase.offset_us(), kInterval144 / 8);
}

TEST(FlPresentationPhaseTest, StepAcrossDeadlineBisectsInsteadOfGivingUp) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  // The compositor needs a lot of lead time, so the target latency is not
  // reachable. The first step already crosses the deadline; the controller
  // must bisect to find it and settle a safety margin before it, which wraps
  // the offset around the period.
  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 6000, 5500, kInterval60);
  };
  RunSamples(&phase, latency, kInterval60, 400, &now_us);

  EXPECT_GT(phase.offset_us(), kInterval60 / 2);
  EXPECT_GE(latency(phase.offset_us()), 5500 + SafetyMargin(kInterval60));
  EXPECT_LT(latency(phase.offset_us()), 5500 + SafetyMargin(kInterval60) + 500);
  EXPECT_EQ(CountLateFrames(&phase, latency, kInterval60, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, FrozenControllerRelearnsAfterWorkloadChange) {
  FlPresentationPhase phase(kInterval144);
  gint64 now_us = 1000000;

  auto first = [](gint64 offset) {
    return VblankLatency(offset, 6000, 500, kInterval144);
  };
  RunSamples(&phase, first, kInterval144, 400, &now_us);
  ASSERT_GT(phase.offset_us(), 0);
  const gint64 first_offset = phase.offset_us();

  // The compositor changes its phase: the frozen offset now commits just after
  // the deadline, which is a full refresh worse. The controller must notice
  // and lock to the new phase.
  now_us += 2 * 1000000;
  auto second = [](gint64 offset) {
    return VblankLatency(offset, 6000 + kInterval144 / 2, 500, kInterval144);
  };
  RunSamples(&phase, second, kInterval144, 600, &now_us);

  EXPECT_NE(phase.offset_us(), first_offset);
  EXPECT_LE(second(phase.offset_us()),
            kInterval144 / 8 + 1000 + SafetyMargin(kInterval144));
  EXPECT_EQ(CountLateFrames(&phase, second, kInterval144, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, SingleLateFrameDoesNotReopenASettledSearch) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 14667, 500, kInterval60);
      },
      kInterval60, 400, &now_us);
  const gint64 settled_offset = phase.offset_us();
  ASSERT_GT(settled_offset, 0);

  // One frame is shown a refresh late while the phase is settled. A single
  // stall is not a compositor phase change and must not restart the search.
  int sample = 0;
  auto latency = [&sample](gint64 offset) {
    gint64 value = VblankLatency(offset, 14667, 500, kInterval60);
    if (sample++ == 3) {
      value += kInterval60;
    }
    return value;
  };
  RunSamples(&phase, latency, kInterval60, 30, &now_us);

  EXPECT_EQ(phase.offset_us(), settled_offset);
}

TEST(FlPresentationPhaseTest, HighRefreshRateDoesNotOvershoot) {
  FlPresentationPhase phase(kInterval240);
  gint64 now_us = 1000000;

  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 3500, 400, kInterval240);
  };
  RunSamples(&phase, latency, kInterval240, 600, &now_us);

  EXPECT_GT(phase.offset_us(), 0);
  EXPECT_LT(phase.offset_us(), kInterval240);
  const gint64 settled_latency = latency(phase.offset_us());
  EXPECT_GT(settled_latency, 0);
  EXPECT_LE(settled_latency,
            kInterval240 / 8 + 1000 + SafetyMargin(kInterval240));
  EXPECT_EQ(CountLateFrames(&phase, latency, kInterval240, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, VariableRefreshRateDisablesController) {
  FlPresentationPhase phase(kInterval144);
  gint64 now_us = 1000000;

  // Learn a phase first, then switch the output to variable refresh. The
  // learned phase must be discarded, not left active.
  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 6000, 500, kInterval144);
      },
      kInterval144, 400, &now_us);
  ASSERT_GT(phase.offset_us(), 0);

  phase.AddSample(kInterval144, kInterval144, kVariableRate, now_us);
  EXPECT_FALSE(phase.enabled());
  EXPECT_EQ(phase.offset_us(), 0);

  now_us += kInterval144;
  RunSamples(
      &phase,
      [](gint64 offset) {
        (void)offset;
        return kInterval144;
      },
      kInterval144, 300, &now_us);
  EXPECT_EQ(phase.offset_us(), 0);
}

TEST(FlPresentationPhaseTest, MismatchedRefreshRateIsIgnored) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  // A sample from a different mode must not teach a phase for this interval.
  for (int i = 0; i < 100; i++) {
    phase.AddSample(kInterval60, kInterval144, kFixedRate, now_us);
    now_us += kInterval60;
  }
  EXPECT_EQ(phase.offset_us(), 0);
}

TEST(FlPresentationPhaseTest, InvalidLatencyIsIgnored) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  for (int i = 0; i < 100; i++) {
    phase.AddSample(0, kInterval60, kFixedRate, now_us);
    phase.AddSample(-1000, kInterval60, kFixedRate, now_us);
    phase.AddSample(kInterval60 * 5, kInterval60, kFixedRate, now_us);
    now_us += kInterval60;
  }
  EXPECT_EQ(phase.offset_us(), 0);
}

TEST(FlPresentationPhaseTest,
     VariableRefreshRateStaysDisabledAcrossRateChange) {
  FlPresentationPhase phase(kInterval144);
  gint64 now_us = 1000000;

  phase.AddSample(kInterval144, kInterval144, kVariableRate, now_us);
  ASSERT_FALSE(phase.enabled());
  EXPECT_TRUE(phase.SetInterval(120000));
  EXPECT_FALSE(phase.enabled());

  phase.Reset();
  EXPECT_TRUE(phase.enabled());
}

TEST(FlPresentationPhaseTest, IntervalChangeResetsLearnedPhase) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 14000, 500, kInterval60);
      },
      kInterval60, 400, &now_us);
  ASSERT_GT(phase.offset_us(), 0);

  EXPECT_TRUE(phase.SetInterval(kInterval144));
  EXPECT_EQ(phase.offset_us(), 0);
  EXPECT_FALSE(phase.SetInterval(kInterval144));
}

TEST(FlPresentationPhaseTest, StaleSamplesDoNotResetTheOffset) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 14000, 500, kInterval60);
      },
      kInterval60, 400, &now_us);
  const gint64 offset = phase.offset_us();
  ASSERT_GT(offset, 0);

  // A long gap (window hidden) discards samples, but the learned phase is
  // still the best known one.
  now_us += kInterval60 * 100;
  phase.AddSample(kInterval60, kInterval60, kFixedRate, now_us);
  EXPECT_EQ(phase.offset_us(), offset);
}

TEST(FlPresentationPhaseTest, TwoHundredFortyHzConverges) {
  FlPresentationPhase phase(kInterval240);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 3800, 300, kInterval240);
      },
      kInterval240, 600, &now_us);

  EXPECT_GT(phase.offset_us(), 0);
  EXPECT_LT(phase.offset_us(), kInterval240);
  const gint64 latency =
      VblankLatency(phase.offset_us(), 3800, 300, kInterval240);
  EXPECT_LE(latency, kInterval240 / 8 + 1000 + SafetyMargin(kInterval240));
}

// A compositor that replaces a late commit with the next one: a frame whose
// commit jitter pushes it over the deadline is discarded instead of being
// presented a refresh late. Returns the number of discarded frames.
int RunWithDiscards(FlPresentationPhase* phase,
                    gint64 natural_latency_us,
                    gint64 lead_us,
                    gint64 jitter_us,
                    gint64 interval_us,
                    int frames,
                    gint64* now_us) {
  int discarded = 0;
  for (int i = 0; i < frames; i++) {
    const gint64 offset = phase->offset_us();
    const gint64 jitter = i % 2 == 0 ? 0 : jitter_us;
    const gint64 on_time =
        VblankLatency(offset, natural_latency_us, lead_us, interval_us);
    const gint64 latency = VblankLatency(offset + jitter, natural_latency_us,
                                         lead_us, interval_us);

    if (latency > on_time + interval_us / 2) {
      discarded++;
      phase->AddDiscard();
    } else {
      phase->AddSample(latency, interval_us, kFixedRate, *now_us);
    }
    *now_us += interval_us;
  }

  return discarded;
}

TEST(FlPresentationPhaseTest, DiscardedFramesCountAsMisses) {
  FlPresentationPhase phase(kInterval240);
  gint64 now_us = 1000000;

  // The target is not reachable, so the search ends at the deadline. Frames
  // past it are discarded and never reported as presented; the controller
  // must still see the crossing and keep a margin that covers the jitter.
  RunWithDiscards(&phase, 3500, 1500, 300, kInterval240, 800, &now_us);

  EXPECT_EQ(
      RunWithDiscards(&phase, 3500, 1500, 300, kInterval240, 1000, &now_us), 0);
}

TEST(FlPresentationPhaseTest, SettledPhaseBacksOffOnDiscards) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 14667, 500, kInterval60);
      },
      kInterval60, 400, &now_us);
  const gint64 settled_offset = phase.offset_us();
  ASSERT_GT(settled_offset, 0);

  // The workload became slower and commits now miss the deadline. Two
  // discarded frames move the phase earlier by the safety margin.
  EXPECT_FALSE(phase.AddDiscard());
  EXPECT_TRUE(phase.AddDiscard());
  EXPECT_EQ(phase.offset_us(), settled_offset - SafetyMargin(kInterval60));
}

TEST(FlPresentationPhaseTest, EarlierProbeFindsDeadlineThatMovedEarlier) {
  FlPresentationPhase phase(kInterval240);
  gint64 now_us = 1000000;

  // The compositor first queues every frame for one more refresh, so the
  // latency is above one refresh.
  auto queued = [](gint64 offset) {
    return VblankLatency(offset, 3500, 1500, kInterval240) + kInterval240;
  };
  RunSamples(&phase, queued, kInterval240, 800, &now_us);
  ASSERT_LT(queued(phase.offset_us()),
            kInterval240 + 1500 + SafetyMargin(kInterval240) + 500);

  // Then it stops queueing and its deadline moves 1.2ms earlier. The settled
  // phase is now a refresh late, which looks as stable as before. Only a
  // probe that moves the commit earlier can find the new deadline.
  auto direct = [](gint64 offset) {
    return VblankLatency(offset + 1200, 3500, 1500, kInterval240);
  };
  RunSamples(&phase, direct, kInterval240, 240 * 12, &now_us);

  EXPECT_LT(direct(phase.offset_us()), 1500 + SafetyMargin(kInterval240) + 500);
  EXPECT_EQ(CountLateFrames(&phase, direct, kInterval240, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, BackOffDoesNotStartANewSearch) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  auto latency = [](gint64 offset) {
    return VblankLatency(offset, 14667, 500, kInterval60);
  };
  RunSamples(&phase, latency, kInterval60, 400, &now_us);
  for (int i = 0; i < 2; i++) {
    phase.AddDiscard();
    ASSERT_TRUE(phase.AddDiscard());
    RunSamples(&phase, latency, kInterval60, 20, &now_us);
  }
  const gint64 backed_off = phase.offset_us();

  // Each step back makes the latency higher by the margin. That is expected
  // and must not look like a regression that restarts the search.
  EXPECT_EQ(CountLateFrames(&phase, latency, kInterval60, 600, &now_us), 0);
  EXPECT_EQ(phase.offset_us(), backed_off);
}

TEST(FlPresentationPhaseTest, DeadlineThatMovesEarlierIsFoundByMovingEarlier) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  RunSamples(
      &phase,
      [](gint64 offset) {
        return VblankLatency(offset, 14667, 500, kInterval60);
      },
      kInterval60, 400, &now_us);

  // The deadline moves 6ms earlier, more than the headroom, so every frame is
  // now a refresh late. The controller must move the phase earlier instead
  // of walking a whole period later and crossing the deadline again.
  auto shifted = [](gint64 offset) {
    return VblankLatency(offset + 6000, 14667, 500, kInterval60);
  };
  RunSamples(&phase, shifted, kInterval60, 40, &now_us);
  EXPECT_LT(shifted(phase.offset_us()), kInterval60 / 2);
  RunSamples(&phase, shifted, kInterval60, 300, &now_us);

  EXPECT_LT(shifted(phase.offset_us()),
            500 + kInterval60 / 8 + SafetyMargin(kInterval60) + 1000);
  EXPECT_EQ(CountLateFrames(&phase, shifted, kInterval60, 600, &now_us), 0);
}

TEST(FlPresentationPhaseTest, DeadlineThatMovesLaterIsFollowed) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  auto first = [](gint64 offset) {
    return VblankLatency(offset, 14667, 500, kInterval60);
  };
  RunSamples(&phase, first, kInterval60, 400, &now_us);
  const gint64 settled_latency = first(phase.offset_us());

  // The deadline moves 3.7ms later. Frames stay on time, but the latency is
  // higher than needed; the controller must find the new deadline.
  auto later = [](gint64 offset) {
    return VblankLatency(offset - 3700, 14667, 500, kInterval60);
  };
  RunSamples(&phase, later, kInterval60, 400, &now_us);

  EXPECT_LE(later(phase.offset_us()), settled_latency + 500);
  EXPECT_EQ(CountLateFrames(&phase, later, kInterval60, 300, &now_us), 0);
}

TEST(FlPresentationPhaseTest, DeadlineThatMovesEarlierKeepsTheMargin) {
  FlPresentationPhase phase(kInterval60);
  gint64 now_us = 1000000;

  auto first = [](gint64 offset) {
    return VblankLatency(offset, 14667, 500, kInterval60);
  };
  RunSamples(&phase, first, kInterval60, 400, &now_us);
  const gint64 settled_latency = first(phase.offset_us());

  // The deadline moves 1.5ms earlier. Frames are still on time, but closer
  // to the deadline than the margin allows.
  auto earlier = [](gint64 offset) {
    return VblankLatency(offset + 1500, 14667, 500, kInterval60);
  };
  RunSamples(&phase, earlier, kInterval60, 200, &now_us);

  EXPECT_GE(earlier(phase.offset_us()), settled_latency - 500);
}

}  // namespace
