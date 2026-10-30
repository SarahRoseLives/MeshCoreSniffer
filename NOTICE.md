# Notices

MeshCoreSniffer is released under the MIT license; see `LICENSE`.

## OpenITC-derived GUI code

The optional GUI (`src/gui`, `third_party/`) adapts the fixed dashboard layout,
theme, slippy-map tile cache/view and SQLite store pattern from **OpenITC**
(Copyright SarahRoseLives). Those portions are used here under the MIT license
with permission of the copyright owner.

## Bundled libraries

- **Dear ImGui** — MIT; see `third_party/imgui/LICENSE.txt`.
- **stb_image** — public domain / MIT; see the header comments in
  `third_party/stb/stb_image.h`.
- **GLFW** — zlib/libpng license; provided by MSYS2 (`mingw-w64-x86_64-glfw`).
- **SQLite** — public domain; provided by MSYS2 (`mingw-w64-x86_64-sqlite3`).
- **librtlsdr / libusb** — GPL-2.0 / LGPL-2.1; runtime DLLs are loaded by the
  application and are provided by the RTL-SDR / MSYS2 packages.
