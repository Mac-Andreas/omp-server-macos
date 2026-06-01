# open.mp — native macOS (Apple Silicon / arm64) build

This fork builds the [open.mp](https://github.com/openmultiplayer/open.mp) server
**natively on macOS arm64 (Apple Silicon)**. The resulting `omp-server` runs
directly on macOS — **no Wine, no Docker, no Linux VM required**.

- Base: upstream tag [`v1.5.8.3079`](https://github.com/openmultiplayer/open.mp/releases/tag/v1.5.8.3079)
  (commit `c6759bd8`, build 3079)
- Target: `macos / arm64`, static OpenSSL + static SQLite (fully self-contained)
- Prebuilt server: see this repo's [Releases](../../releases) →
  `open.mp-macos-arm64.tar.gz`

> Upstream officially ships only `linux-x86`, `linux-x86-dynssl` and `win-x86`.
> There is no upstream macOS build, and the source has one Apple-specific build
> break (documented below).

---

## The upstream macOS build issue

Building the upstream source on macOS arm64 fails at the **final link step** of
`omp-server` with:

```
ld: library 'atomic' not found
clang++: error: linker command failed with exit code 1 (use -v to see invocation)
make[2]: *** [Output/RelWithDebInfo/Server/omp-server] Error 1
make[1]: *** [Server/Source/CMakeFiles/Server.dir/all] Error 2
make: *** [all] Error 2
```

### Root cause

`Server/Source/CMakeLists.txt` unconditionally links `libatomic` for every
non-MSVC platform:

```cmake
else()
    target_link_libraries(Server PRIVATE dl atomic)
```

`libatomic` is a **GCC/Linux-only** library. On macOS with Apple Clang the
atomic builtins live in the compiler runtime, so there is no separate `atomic`
library to link — and the linker aborts. (`dl` is fine: those symbols are in
libSystem.)

### The fix

Guard `atomic` so it is only linked on non-Apple Unix:

```cmake
else()
    # libatomic is a separate library only on Linux/GCC; on macOS/clang
    # the atomic builtins are in the compiler runtime and no such lib exists.
    if(APPLE)
        target_link_libraries(Server PRIVATE dl)
    else()
        target_link_libraries(Server PRIVATE dl atomic)
    endif()
```

See [`Server/Source/CMakeLists.txt`](Server/Source/CMakeLists.txt).

### Before / after

**Before** (upstream, at 100% it fails to link the executable):

```
[100%] Linking CXX shared library ../../../Output/RelWithDebInfo/Server/components/Pawn.dylib
ld: library 'atomic' not found
clang++: error: linker command failed with exit code 1
make: *** [all] Error 2
```

**After** (this fork — links cleanly and finishes):

```
[ 36%] Linking CXX executable ../../Output/RelWithDebInfo/Server/omp-server
[ 79%] Built target Server
[100%] Built target Pawn
```

Resulting binary is native arm64 with no external (non-system) dependencies:

```
$ file Output/RelWithDebInfo/Server/omp-server
omp-server: Mach-O 64-bit executable arm64

$ otool -L Output/RelWithDebInfo/Server/omp-server | grep -v /usr/lib | grep -v /System
(none — OpenSSL/SQLite are statically linked)
```

And it boots, loading all 22 components:

```
Starting open.mp server (1.5.8.0) from commit 0000...
Running static OpenSSL build - plugins that use OpenSSL might crash ...
Loading component $CAPI.dylib
    Successfully loaded component C-API (1.5.8.0) ...
... (Dialogs, Actors, Vehicles, NPCs, Pawn, Databases, Objects, ... 22 total)
```

> The "Running static OpenSSL build" line is **expected on macOS** and harmless
> — see "About dynssl" below.

---

## Building from source

Requirements:

- macOS arm64 (Apple Silicon), Xcode command line tools (Apple Clang)
- CMake 3.19+
- [Conan **1.x**](https://conan.io/) (**not** 2.x). Conan 1.x needs Python ≤ 3.12.

```bash
git clone --recursive https://github.com/xyranaut/omp-server-macos.git
cd omp-server-macos
git checkout macos-arm64

# Conan 1.x in an isolated Python 3.12 venv (avoids system Python 3.13/3.14 issues)
python3.12 -m venv .conan-venv
./.conan-venv/bin/pip install "conan==1.66.0"
export PATH="$PWD/.conan-venv/bin:$PATH"

mkdir build && cd build
# CMAKE_POLICY_VERSION_MINIMUM is needed for CMake 4.x because the bundled
# `glm` submodule declares cmake_minimum_required(VERSION < 3.5).
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
cmake --build . --config RelWithDebInfo -j$(sysctl -n hw.ncpu)
```

Output lands in `build/Output/RelWithDebInfo/Server/`:

```
omp-server
components/*.dylib   (22 components)
```

### Notes on toolchain quirks

| Symptom | Cause | Handling |
| --- | --- | --- |
| `Compatibility with CMake < 3.5 has been removed` | bundled `glm` submodule | pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| `ld: library 'atomic' not found` | Linux-only `libatomic` link | fixed in this fork (see above) |
| Conan install fails on Python 3.13/3.14 | Conan 1.x predates them | use Python 3.12 venv |

---

## Running

### 1. Get the server files

Either build from source (above) or download the prebuilt
`open.mp-macos-arm64.tar.gz` from [Releases](../../releases) and unpack it:

```bash
tar -xzf open.mp-macos-arm64.tar.gz
cd Server     # contains omp-server + components/
```

If you downloaded it, clear the macOS Gatekeeper quarantine flag first, or the
binary/components will be blocked from loading:

```bash
xattr -dr com.apple.quarantine omp-server components
```

### 2. Create a `config.json`

`omp-server` needs a `config.json` next to it. A minimal one:

```json
{
    "name": "My open.mp server",
    "announce": false,
    "max_players": 50,
    "pawn": {
        "main_scripts": ["gamemode 1"],
        "side_scripts": [],
        "legacy_plugins": []
    }
}
```

Put your compiled gamemode at `gamemodes/gamemode.amx` (the
`"gamemode 1"` entry above means `gamemodes/gamemode.amx`, arg `1`). To compile
a `.pwn` gamemode you need the PAWN compiler + the open.mp includes (qawno /
`omp-stdlib`), which are not part of this server build.

### 3. Run it

```bash
./omp-server
```

You should see it load 22 components and then start listening (default port
`7777`). Logs are written to `log.txt` in the same folder. Stop with `Ctrl+C`.

```
$ ./omp-server
Starting open.mp server (1.5.8.0) ...
Loading component $CAPI.dylib
    Successfully loaded component C-API (1.5.8.0) ...
...
```

> Running with **no** `config.json`/gamemode still loads all components and then
> exits — that confirms the binary itself works, but a real server needs the
> config + a gamemode as above.

### Directory layout when running

```
Server/
├── omp-server
├── config.json
├── log.txt              (created on run)
├── components/          (the 22 bundled .dylib components)
├── gamemodes/
│   └── gamemode.amx
└── plugins/             (optional third-party legacy plugins)
```

---

## About dynssl (why there is no macOS dynssl build)

`dynssl` = **dyn**amically-linked **SSL** (OpenSSL). Upstream ships a Linux
`-dynssl` variant so plugins that load their *own* OpenSSL (e.g.
`discord-connector`, `pawn-requests`, the third-party MySQL plugin) don't crash
from clashing against a statically-linked copy.

- It is **Linux-only** upstream. On macOS the `SHARED_OPENSSL` option is disabled
  in `CMakeLists.txt` (`if(NOT APPLE)`), so the macOS build is **always static
  OpenSSL**. That is why you see the "Running static OpenSSL build" notice — it is
  informational, not an error.
- **MySQL is not part of open.mp.** The only built-in database support is SQLite
  (the `Databases` component). MySQL comes from a separate third-party plugin you
  would compile and drop into `components/` yourself.

---

## Relationship to upstream

This is a fork of `openmultiplayer/open.mp` for the sole purpose of producing a
native macOS arm64 server. The only source change vs. upstream tag
`v1.5.8.3079` is the `libatomic` link guard described above. All credit for
open.mp goes to the upstream project and its contributors.
