# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_subdirectory(pywatchman)

add_fb_python_executable(watchman-diag
  MAIN_MODULE pywatchman.watchman_diag:main
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-diag)

add_fb_python_executable(watchman-make
  MAIN_MODULE pywatchman.watchman_make:main
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-make)

add_fb_python_executable(watchman-replicate-subscription
  MAIN_MODULE pywatchman.watchman_replicate_subscription:main
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-replicate-subscription)

add_fb_python_executable(watchman-wait
  MAIN_MODULE pywatchman.watchman_wait:main
  DEPENDS
    pywatchman
)
install_fb_python_executable(watchman-wait)
