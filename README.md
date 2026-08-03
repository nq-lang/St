# QQQ Options Data Terminal — GitHub Codespaces build

C++23, GLFW + Dear ImGui + ImPlot. 5 files total:

```
.devcontainer/devcontainer.json   codespace image + build/display config
.devcontainer/setup.sh            installs g++-13, cmake, GL/X11 headers
CMakeLists.txt                    fetches GLFW/ImGui/ImPlot, builds qqq_terminal
include/theme.hpp                 color constants only
src/main.cpp                      everything else (~1000 lines, one file)
run_terminal.sh                   launches against the codespace's virtual display
```

## Why a devcontainer at all

Codespaces containers have no physical display. GLFW opens a real OpenGL window,
which needs *some* display to draw into. The devcontainer uses the
[`desktop-lite`](https://github.com/devcontainers/features/tree/main/src/desktop-lite)
feature, which runs a virtual display (Xvfb) inside the container and streams it to
your browser over noVNC — no code changes to the terminal itself, it just draws into
a virtual screen you can see.

## Launching it

1. Open this repo in a Codespace (green "Code" button → Codespaces → Create).
2. Wait for the container to build — first boot runs `setup.sh` (installs toolchain)
   then `postCreateCommand` (fetches deps via CMake, builds the binary). Takes a
   few minutes the first time; it's cached after that.
3. In the **Ports** tab, find **"Terminal Display (noVNC)"** (port 6080) and open it
   in your browser (it should auto-open; if not, click the globe icon next to the
   port). You'll see a black/gray virtual desktop — that's the display the terminal
   will render into.
4. In the VS Code terminal panel, run:
   ```bash
   ./run_terminal.sh
   ```
5. Switch to the noVNC browser tab — the terminal window appears there.

## Toolchain

Built and verified against **g++ 13.3.0** with `-std=c++23 -Wall -Wextra`. The
`.devcontainer/setup.sh` script pins `gcc-13`/`g++-13` explicitly via
`update-alternatives` so the version is reproducible regardless of what the base
devcontainer image ships as default.

## If you'd rather skip the virtual display

If you just want to confirm the code builds (not actually see the GUI), you don't
need the noVNC step at all:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

A successful build here already tells you the C++23 code, greeks math, and mock
data pipeline are all correct — that's most of what could go wrong. Actually
running it just needs the DISPLAY variable pointed at Xvfb, which `run_terminal.sh`
handles.

## Everything else

Same as before — mock data by default, IBKR stub in `main.cpp` section 4
(search `TODO(live-data)`) not wired in yet, `TERMINAL_USE_IMPLOT3D=OFF` falls back
to a 2D IV heat-slice if ImPlot3D is troublesome.
