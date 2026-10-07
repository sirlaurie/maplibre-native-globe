# iOS Globe

## Source and scope

This fork extends the globe implementation by
[johncarmack1984 in MapLibre Native PR #4533](https://github.com/maplibre/maplibre-native/pull/4533).
The imported PR revision is `9e23aeaa2e2e94dba04e6f8cd5aadbc38d7ca957`; the
comparison mainline revision is `ba9fc57bc88725d7602fd652e601384e9f2e3183`.
The PR now points to a different history. This fork preserves the tested source
from the pinned PR snapshot.

The R9 additions cover:

- Darwin style projection and custom-layer projection APIs.
- Globe camera movement, bounds fitting, coordinate conversion, and feature queries.
- Antimeridian geometry, tile coverage, circles, lines, symbols, and fill extrusions.
- Metal shader preparation, frame presentation, offscreen rendering, and resource lifetime.
- Building roof vertex reuse and projection-buffer allocation and upload for symbols.
- Core, Metal, Darwin, and iOS regression tests.

The validation scope is iOS with Metal. Other platforms and rendering backends
have not been validated for these additions. Terrain and atmosphere effects are
outside this work's scope. Upstream licenses remain in [LICENSE.md](../LICENSE.md),
[LICENSES.core.md](../LICENSES.core.md), and dependency license files.

## Enable globe

Set the projection after the style loads. Repeat the assignment when replacing
the style:

```swift
func mapView(_ mapView: MLNMapView, didFinishLoading style: MLNStyle) {
    style.projection = NSExpression(forConstantValue: "globe")
}
```

The `globe` preset transitions from vertical perspective to Mercator between zoom
levels 11 and 12. Use `vertical-perspective` for a globe without that transition,
or `mercator` for the planar projection. Setting `projection` to `nil` restores
the default Mercator projection. Changing this property preserves the style's
sources, layers, and images.

## Build the iOS SDK

Use macOS, Xcode, and Bazelisk. Bazelisk selects Bazel 8.8.0 from `.bazelversion`.
The R9 builds used Xcode 27.1 (27A9269). The application integration targets iOS
18 and later; the upstream XCFramework target retains its iOS 15.5 deployment
setting. That setting is not an iOS 15.5 validation claim.

```sh
git clone --recurse-submodules https://github.com/sirlaurie/maplibre-native-globe.git
cd maplibre-native-globe
mkdir -p .build/bazelisk .build/tmp
export BAZELISK_HOME="$PWD/.build/bazelisk"
export TMPDIR="$PWD/.build/tmp"

bazel --output_user_root="$PWD/.build/bazel-user-root" build \
  //platform/ios:MapLibre.dynamic \
  --//:renderer=metal --compilation_mode=opt \
  --features=dead_strip,thin_lto --objc_enable_binary_stripping \
  --apple_generate_dsym --output_groups=+dsyms \
  --embed_label="maplibre_ios_$(cat platform/ios/VERSION)"
```

The archive is `bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip`.
It contains device arm64 and simulator arm64 / x86_64 frameworks. The matching
dSYMs are under `bazel-bin/platform/ios/MapLibre.dynamic_dsyms`. Keep each dSYM
with the framework from the same build and check their UUIDs with
`xcrun dwarfdump --uuid` before distribution.

The repository does not publish an XCFramework release or a Swift Package
distribution endpoint. Build the source to use these changes; upstream binary
packages do not contain them.

To run development apps on a device, copy
`platform/darwin/bazel/example_config.bzl` to
`platform/darwin/bazel/config.bzl` and set your signing team and bundle prefix.
The local configuration is ignored by Git. See the
[upstream iOS developer guide](mdbook/src/platforms/ios/README.md) for setup.

## Tests

Generate the Xcode project from the repository root:

```sh
bazel --output_user_root="$PWD/.build/bazel-user-root" run \
  //platform/ios:xcodeproj \
  --@rules_xcodeproj//xcodeproj:extra_common_flags=--//:renderer=metal
```

The generated project is `platform/ios/MapLibre.xcodeproj`. Select an iOS 18+
simulator and run the `CppUnitTests`, `RenderTest`, and `ios_test` schemes.
Use one explicit simulator UDID for command-line runs; finish
`xcrun simctl bootstatus <UDID> -b` before installing or launching apps.

The Darwin projection API has a Bazel test target:

```sh
bazel --output_user_root="$PWD/.build/bazel-user-root" test \
  //platform/ios/test:ios_globe_test \
  --test_output=errors --//:renderer=metal
```

The render runner compares existing expectations without rebaselining them.
`MLN_RENDER_TEST_FILTER` selects a subset when investigating a rendering issue.

## R9 validation record

The local acceptance run completed on 2026-10-05. Publishing this source does not
constitute a new CI or device test run.

- Device arm64 and simulator arm64 / x86_64 Release SDK builds passed, with
  matching framework and dSYM UUIDs.
- The selected Native Core regression suite passed 271 tests, with 4 existing
  disabled tests. The selected render suite passed 57 tests, with 3 existing
  ignored failures. These are selected suites, not the full upstream test matrix.
- The integrating application's 1,514 unit tests passed. Its Release build had
  no errors or warnings.
- The same iPhone 12 Pro running iOS 26.7.1 was used for the mainline comparison.
  Seven common application scenarios produced 42 retained measurement windows;
  all 126 recorded hitch values were zero. The tested interactions left no
  confirmed R9 fluency regression relative to the pinned mainline.

Resource costs are not at parity. Two street-level scenarios used median CPU
times about 0.182 s and 0.117 s above mainline. Style changes, sharing, and
foreground/background scenarios had median absolute memory about 73–82 MB above
mainline; the cause was not established. These results do not establish equal
power use, performance on every device, or absence of stalls outside the measured
interactions. The sharing test covered opening and closing its screen, without
asserting completion of the preview image.
