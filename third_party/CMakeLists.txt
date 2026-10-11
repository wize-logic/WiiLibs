# SPDX-License-Identifier: MIT
# Vendored third-party code, unmodified (licences next to each).

# greatest: single-header C test framework, used by tests/.
add_library(greatest INTERFACE)
target_include_directories(greatest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/greatest)
