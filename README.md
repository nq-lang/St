# QQQ Options Data Terminal — single-file build

Three files, total. That's it.

```
CMakeLists.txt
include/theme.hpp   (color constants only — nothing to debug here)
src/main.cpp        (everything else: data model, greeks math, mock feed,
                      IBKR stub, all 4 render panels, app loop)
```

## Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/qqq_terminal
```

First configure fetches GLFW, Dear ImGui, and ImPlot from GitHub (network needed once).
Runs on mock data immediately — no IBKR setup required to see the terminal.

## What's real vs. stubbed

- **Real and running today**: full layout (8 barcharts + summary panel, 3D-ish vol
  surfaces, IV heatmap table), Black-Scholes greeks (incl. vanna/charm derivation),
  a mock feed that generates internally-consistent, animating QQQ chain data.
- **Stubbed, not wired**: `ibkr::IbkrClient` in `main.cpp` (search `TODO(live-data)`)
  — has the right shape/interface but always reports "not connected" until you fill
  in the actual TWS API calls. The app falls back to mock data automatically.

## About the 3D surfaces

True 3D mesh rendering needs `ImPlot3D`, which is younger/less stable than the rest
of the stack. By default (`TERMINAL_USE_IMPLOT3D=ON`) it's fetched and used. If it
gives you build trouble:

```bash
cmake -B build -DTERMINAL_USE_IMPLOT3D=OFF
```

This switches Row 2 to a 2D heatmap (moneyness × DTE, color = IV) using plain ImPlot
— same information, zero extra OpenGL plumbing, one less moving part while you're
getting the rest of the terminal working. You can flip it back on later.

## Adding IBKR later

Open `main.cpp`, section 4 (`namespace ibkr`), and section 9 near the bottom where
`ibkr_client.connect_and_subscribe()` is called. Every `TODO(live-data)` comment marks
where a real `EClientSocket`/`EReader`/`EWrapper` call belongs. You don't need to
touch the render code (sections 5–8) — they only ever see a `data::MarketState`,
and it doesn't care whether `MockFeed` or `IbkrClient` filled it in.
