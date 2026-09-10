/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Minimal placeholder pixel shader for pipeline hot-swap.
// Used temporarily while the real shader compiles in the background.
// Opaque black so alpha-tested draws still write depth/color instead of
// punching holes until the real pipeline is ready.

layout(location = 0) out vec4 oC0;

void main() {
  oC0 = vec4(0.0, 0.0, 0.0, 1.0);
}
