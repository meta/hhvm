#!/usr/bin/env python
# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from setuptools import Extension, setup

setup(
    packages=["pywatchman"],
    ext_modules=[
        Extension(
            "pywatchman.bser",
            sources=["pywatchman/bsermodule.c", "pywatchman/bser.c"],
            include_dirs=["./pywatchman"],
        )
    ],
    zip_safe=True,
    test_suite="tests",
)
