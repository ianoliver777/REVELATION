# Build REVELATION 1.0.0

REVELATION is a JUCE 8.0.12 / C++17 audio effect. By default CMake fetches the pinned JUCE release with Git; pass `-DJUCE_DIR=/absolute/path/to/JUCE` to use an existing checkout.

Every push to `main` runs **Actions → REVELATION build (macOS + Windows)**. It builds, tests and host-loads the macOS universal VST3 + AU and the Windows x64 VST3, then attaches `REVELATION-<version>-Mac-Windows.zip` (both platforms + INSTALL.md) to the `v<version>` release.

## macOS universal VST3 and AU

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREVELATION_TESTS=ON
cmake --build build --parallel 6 --target Revelation_VST3 Revelation_AU DspTest HostTest EditorPreview
ctest --test-dir build --output-on-failure
```

## Windows x64 VST3

From a Developer PowerShell for VS 2022 with CMake and Git on PATH:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build-Windows.ps1
```

The bundle lands in `build-windows-x64\Revelation_artefacts\Release\VST3\REVELATION.vst3`.

## Releasing a new version

Change the version in `CMakeLists.txt`, `INSTALL.md`, this file and `VERSION` in `.github/workflows/build.yml`, then push to `main`.

JUCE is a third-party dependency under the AGPLv3 / commercial JUCE licence; see JUCE-LICENSE.md.
