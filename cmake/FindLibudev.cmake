# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

find_package(PkgConfig QUIET)

pkg_check_modules(Libudev libudev IMPORTED_TARGET)

if (NOT TARGET PkgConfig::Libudev)
    set(Libudev_FOUND 0)
endif()
