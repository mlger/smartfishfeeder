/**
 * @file feed_scheduler.h
 * @brief Native-test shim: forwards <feed_scheduler.h> to the real header.
 *
 * feed_state.h pulls in the scheduler API with ANGLE brackets
 * (#include <feed_scheduler.h>), but the [env:native] test environment
 * excludes src/ from the build (build_src_filter = -<*>) and its include
 * path only carries test/mocks + ../common. This shim, found via
 * -I test/mocks, keeps the real header reachable without touching
 * platformio.ini. Quoted includes from src/*.cpp still resolve inside
 * src/ first, so the other test TUs are unaffected.
 */
#pragma once

#include "../../src/feed_scheduler.h" // NOLINT (deliberate relative include)
