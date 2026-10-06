# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Bazel build rules for TypeScript libraries and bundles."""

load("@aspect_rules_esbuild//esbuild:defs.bzl", "esbuild")

def ts_library(
        name,
        srcs = [],
        deps = [],
        testonly = False,
        visibility = None,
        **kwargs):
    """Exposes TypeScript sources and dependencies as a filegroup for esbuild.

    Args:
      name: Target name.
      srcs: TypeScript source files belonging to this library.
      deps: Transitive ts_library dependencies.
      testonly: Whether the target is restricted to test targets.
      visibility: Target visibility list.
      **kwargs: Additional ts_library attributes ignored by esbuild.
    """
    _ = kwargs
    native.filegroup(
        name = name,
        srcs = srcs + deps,
        testonly = testonly,
        visibility = visibility,
    )

def js_binary(
        name,
        srcs = [],
        deps = [],
        entry_point = None,
        visibility = None,
        **kwargs):
    """Bundles TypeScript sources into a single IIFE <name>.js file via esbuild.

    Args:
      name: Target name. Produces predeclared output <name>.js.
      srcs: Direct source files.
      deps: Transitive ts_library targets to include in the bundle.
      entry_point: Entry point TypeScript file for esbuild.
      visibility: Target visibility list.
      **kwargs: Additional js_binary attributes ignored by esbuild.
    """
    _ = kwargs
    esbuild(
        name = name,
        srcs = srcs + deps,
        entry_point = entry_point,
        format = "iife",
        minify = True,
        output = name + ".js",
        target = "es2020",
        visibility = visibility,
    )
