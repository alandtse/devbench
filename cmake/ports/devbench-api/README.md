# devbench-api vcpkg port

Vendor the devbench cross-plugin API (`DevBenchAPI.h` + `.cpp`) into your SKSE plugin
via vcpkg — **do not copy the files into your tree** (they drift). Mirrors the
SkyrimVRESL port.

## Consume (once devbench is published)

`vcpkg.json`:

```json
{ "dependencies": ["devbench-api"] }
```

`CMakeLists.txt`:

```cmake
find_package(devbench-api CONFIG REQUIRED)
target_link_libraries(YourPlugin PRIVATE DevBench::API)
```

Then, after SKSE sends `kPostLoad`:

```cpp
#include <DevBenchAPI.h>
if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
    dvb->RegisterTool("yourmod.dothing", R"({"description":"...","inputSchema":{...}})",
                      &YourHandler, yourCtx);
}
```

Linking `DevBench::API` puts `DevBenchAPI.h` on the include path and compiles
`DevBenchAPI.cpp` (the messaging-handshake helper) into your plugin. The API glue is
**MIT** (`DevBenchAPI.LICENSE.txt`); the devbench plugin itself is GPL-3.0.

## Interim: local overlay (before devbench has a published REF/SHA512)

`portfile.cmake`'s `REF`/`SHA512` are placeholders until devbench is pushed to GitHub.
Until then, consume from a local checkout via an overlay port. In the consumer's
`vcpkg-configuration.json`:

```json
{ "overlay-ports": ["<path-to-devbench>/cmake/ports"] }
```

and point the portfile at your local commit (e.g. `vcpkg_from_git` on a `file://`
path, or a `vcpkg_from_github` REF/SHA512 once a commit exists). Fill the real
`REF`/`SHA512` in `portfile.cmake` on the first GitHub release.
