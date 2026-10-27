#pragma once

/*
 * Precompiled header shared by every DSBox target (see DSBoxPch.cmake).
 *
 * Rules for what may go here:
 *   - Standard library headers ONLY. No dsbox/* headers, no third-party
 *     headers (SDL, Vulkan, ...): DSBox modules are general-purpose and
 *     must not silently depend on each other or on any one platform
 *     library just because a precompiled header happened to pull it in.
 *   - Only headers that a large share of translation units include
 *     anyway. A header used by one or two modules belongs in that
 *     module's own sources, not here.
 *
 * This file is force-included into every C++ source of a target by the
 * compiler, so sources must keep their own explicit #includes: they
 * still have to compile when DSBOX_USE_PCH is OFF.
 */

// Language support / utilities
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

// Strings and containers
#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Memory
#include <memory>

// Concurrency and time
#include <atomic>
#include <chrono>
#include <mutex>
