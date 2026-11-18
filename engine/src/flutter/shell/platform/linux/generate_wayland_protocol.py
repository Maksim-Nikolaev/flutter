#!/usr/bin/env python3
# Copyright 2013 The Flutter Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Generates the presentation-time Wayland client protocol.

The protocol definition is part of wayland-protocols, which is installed in
the build sysroot. The generated files live in the build directory; no
generated protocol code is checked in.

wayland-scanner 1.20 and later emit wl_proxy_marshal_flags() calls. The
engine's Debian bullseye sysroot still has libwayland 1.18, so the generated
header is given a fallback that uses wl_proxy_marshal() and
wl_proxy_marshal_constructor() when the newer function is unavailable.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys


DESTROY_FALLBACK = """static inline void
wp_presentation_destroy(struct wp_presentation* wp_presentation) {
#if defined(WL_MARSHAL_FLAG_DESTROY)
  wl_proxy_marshal_flags(
      (struct wl_proxy*)wp_presentation, WP_PRESENTATION_DESTROY, NULL,
      wl_proxy_get_version((struct wl_proxy*)wp_presentation),
      WL_MARSHAL_FLAG_DESTROY);
#else
  wl_proxy_marshal((struct wl_proxy*)wp_presentation, WP_PRESENTATION_DESTROY);
  wl_proxy_destroy((struct wl_proxy*)wp_presentation);
#endif
}"""

FEEDBACK_FALLBACK = """static inline struct wp_presentation_feedback*
wp_presentation_feedback(struct wp_presentation* wp_presentation,
                          struct wl_surface* surface) {
  struct wl_proxy* callback;

#if defined(WL_MARSHAL_FLAG_DESTROY)
  callback = wl_proxy_marshal_flags(
      (struct wl_proxy*)wp_presentation, WP_PRESENTATION_FEEDBACK,
      &wp_presentation_feedback_interface,
      wl_proxy_get_version((struct wl_proxy*)wp_presentation), 0, surface,
      NULL);
#else
  /* The NULL is the interface placeholder for the new_id argument. */
  callback = wl_proxy_marshal_constructor(
      (struct wl_proxy*)wp_presentation, WP_PRESENTATION_FEEDBACK,
      &wp_presentation_feedback_interface, surface, NULL);
#endif

  return (struct wp_presentation_feedback*)callback;
}"""

_DESTROY_RE = re.compile(
    r"static inline void\s+wp_presentation_destroy\([^)]*\)\s*\{.*?\n\}",
    re.DOTALL,
)
_FEEDBACK_RE = re.compile(
    r"static inline struct wp_presentation_feedback\s*\*\s*"
    r"wp_presentation_feedback\([^)]*\)\s*\{.*?\n\}",
    re.DOTALL,
)


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("xml")
  parser.add_argument("header")
  parser.add_argument("source")
  args = parser.parse_args()

  scanner = os.environ.get("WAYLAND_SCANNER") or shutil.which(
      "wayland-scanner")
  if scanner is None:
    sys.exit("wayland-scanner not found; install libwayland-bin or "
             "wayland-scanner")

  subprocess.check_call([scanner, "client-header", args.xml, args.header])
  subprocess.check_call([scanner, "private-code", args.xml, args.source])

  with open(args.header) as header_file:
    text = header_file.read()

  # Older wayland-scanner versions already emit the compatible calls. Only
  # patch output that uses the newer API.
  if "wl_proxy_marshal_flags" in text:
    text, destroy_count = _DESTROY_RE.subn(DESTROY_FALLBACK, text, count=1)
    text, feedback_count = _FEEDBACK_RE.subn(FEEDBACK_FALLBACK, text, count=1)
    if destroy_count != 1 or feedback_count != 1:
      sys.exit("Unsupported wayland-scanner output for presentation-time; "
               "the generated header changed")
    with open(args.header, "w") as header_file:
      header_file.write(text)


if __name__ == "__main__":
  main()
