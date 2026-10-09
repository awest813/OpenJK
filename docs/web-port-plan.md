# Web Port: Audit & Plan

Goal: run OpenJK (Jedi Academy SP first, then MP) in a browser using
Emscripten → WebAssembly + WebGL2, with the user supplying their own
retail assets.

## Status

Built and smoke-tested in headless Chromium, but **not yet run with real game
data**: there is none in the environment this was developed in. Everything
below was verified up to each client's main loop with a small made-up
`.pk3` (see `tools/web/smoke-test.mjs`); rendering actual menus and maps is
the next thing to check.

| Client | Page | Renderer | Verified |
|---|---|---|---|
| Single player | `openjk_sp.wasm32.html` | rd-vanilla on WebGL 1 via Emscripten's GL emulation | starts, renderer + sound + UI init, main loop, config persists |
| Multiplayer | `openjk.wasm32.html` | rd-vanilla, same | as SP, plus UI/cgame/game modules load, connects to a UDP server through the relay |
| Multiplayer (experimental) | `openjk_rend2.wasm32.html` | rd-rend2 on WebGL 2 | WebGL 2 context, all framebuffers complete, all 671 GLSL programs compile and link, main loop |

### Building and running

```sh
# with the Emscripten SDK activated (emsdk_env.sh)
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j

# serve the build and run the network relay (Node.js, no npm packages)
node tools/web/server.js --root build-web --port 8080
# open http://localhost:8080/

# headless smoke test (needs Playwright)
node tools/web/smoke-test.mjs build-web
```

Or with Docker: `docker build -f tools/web/Dockerfile -t openjk-web .` and
`docker run -p 8080:8080 openjk-web`. Only the runtime stage of that image
was tested here (its emsdk stage couldn't download the SDL2 port through
this environment's proxy).

Players add the `.pk3` files of their own copy of the game on the launcher
page; they are stored in the browser (OPFS) and loaded into memory on
Play, so a full install needs ~1.5 GB of RAM. Configs and saves live in
IndexedDB.

### How it works

- **Build** (`CMakeLists.txt`): `wasm32` architecture; on Emscripten the
  clients are built with bundled zlib/png/jpeg, SDL2 from `-sUSE_SDL=2`
  and `-fwasm-exceptions` (for `Com_Error`). The dedicated server, JK2 and
  tests aren't built.
- **Modules** (§3.1): renderer and game modules are static libraries,
  linked into each client. `tools/wasm/isolate_module.py` merges each one
  into a relocatable object, makes its hidden symbols local and prefixes
  its entry points (`GetModuleAPI` → `cgame_GetModuleAPI`), which keeps
  modules apart the way shared libraries are.
  `shared/sys/sys_static_modules.cpp` stands in for `dlopen`/`dlsym`
  behind the existing `Sys_LoadLibrary` macros.
- **Main loop**: `Sys_Frame()`, after which the engine waits for the
  browser's next animation frame; `Com_Frame` skips frames that come
  early instead of busy-waiting.
- **Loading screens**: every client is linked twice from the same objects,
  plainly and with JSPI (`*.jspi.js`). With JSPI the engine can wait for a
  browser frame in the middle of a map load (`shared/sys/sys_web.cpp`):
  when a second frame is presented within one main-loop frame (at most
  every 100 ms), so loading screens update. The launcher uses the JSPI
  build where the browser has JSPI (`?jspi=0` forces the other one, whose
  main loop is `emscripten_set_main_loop` and whose loading screen stays
  still). ASYNCIFY isn't an option: it doesn't work with the wasm
  exceptions `Com_Error` uses.
- **Console**: `con_passive.cpp`, never stdin (Emscripten would open
  `window.prompt()` dialogs).
- **Files**: `shared/web/shell.html` is the launcher and Emscripten shell
  for all clients; `FS_FCloseFile` tells it to sync the home directory to
  IndexedDB after writes.
- **Networking** (MP): `codemp/qcommon/net_web.cpp` replaces `net_ip.cpp`.
  Datagrams go over one WebSocket to `tools/web/server.js`, which sends
  them on as UDP; host names get placeholder addresses the relay
  resolves. The relay only relays to public addresses on ports ≥ 1024 by
  default (`--allow` to restrict further, `--allow-private` for LAN
  play), only passes back replies from addresses a client contacted, and
  rate-limits clients. Browsers can't accept connections, so a browser
  can host only local games (with bots), not servers for others.
- **rd-vanilla on WebGL**: texture names from `glGenTextures`, internal
  format = pixel format, no texture compression, `glDrawElements` only,
  glow only when supported, and stubs for GL1 calls the emulation lacks
  or aborts on.
- **rd-rend2 on WebGL 2** (separate client, because rd-vanilla needs the
  GL emulation and rend2 must not have it):
  - WebGL 2 function lookup with stand-ins for what WebGL lacks;
  - base vertex draws emulated through the attribute offsets;
  - GLSL ES 3.00 with layout locations;
  - no geometry shader programs, so no volume shadows, cube map
    prefiltering or weather particles;
  - copy-based buffer mapping (`-sFULL_ES3`);
  - ES-valid texture upload formats.
- **Fixes found on the way** (also wrong natively):
  - three function declaration/definition mismatches in MP;
  - z_off_t width differing between zlib's users;
  - int-to-float conversions in rend2 shaders;
  - rend2's depth upload formats and UBO flush offset.

### Known gaps

- Nothing has been rendered with real game data yet. Emscripten's GL
  emulation is "limited workarounds", and could still abort on texture
  environment combinations that only real shaders use.
- Mobile: no touch controls, and the game data needs more memory than most
  phones give a tab.
- Mods with their own native code can't be loaded; mods that are only
  `.pk3` files work.

## 1. Audit summary

| Area | Current state | Web impact | Effort |
|---|---|---|---|
| Build system | CMake, per-module shared libs, `Architecture` from `CMAKE_SYSTEM_PROCESSOR` | Needs an Emscripten toolchain branch (`wasm32` arch string); `emcmake` works with CMake | S |
| Platform header | `shared/qcommon/q_platform.h` has Win/Mac/Linux/BSD blocks; Emscripten defines `__unix__`/`__EMSCRIPTEN__`, not `__linux__` | Add an `__EMSCRIPTEN__` block (`OS_STRING "emscripten"`, LE, `DLL_EXT ".wasm"`) | S |
| Module loading | `Sys_LoadDll` / `Sys_LoadGameDll` / `Sys_LoadSPGameDll` / `Sys_LoadLegacyGameDll` use `dlopen` (`shared/sys/sys_main.cpp`, `sys_loadlib.h`). Renderer loaded via `GetRefAPI`, SP game via `GetGameAPI`, MP game/cgame/ui via `GetModuleAPI` (`codemp/qcommon/vm.cpp`) | Biggest structural decision — see §3.1 | M |
| Main loop | `main()` spins `while(1) Com_Frame();` (`shared/sys/sys_main.cpp:795`) | Must yield to the browser: `emscripten_set_main_loop` or ASYNCIFY/JSPI | M |
| Error handling | `Com_Error` uses C++ `throw`/`catch(int)` (`code/qcommon/common.cpp:326`, `codemp/qcommon/common.cpp:321`) | Build with `-fwasm-exceptions` (supported by all current browsers) | S |
| Inline asm | `codemp/client/snd_mix.cpp` (MSVC x86 mixer), `shared/qcommon/q_math.c` `SnapVector` — both already guarded by `_MSC_VER`/`!idx64` with C fallbacks | None | — |
| Threads | No pthread/SDL thread use in engine code | No `SharedArrayBuffer`/COOP-COEP requirement for the core port | — |
| Pointer size | Codebase supports 32-bit x86 (i386 builds in Dockerfile) | wasm32 is fine; also matches original savegame layouts | — |
| Windowing/input | SDL2 (`shared/sdl/sdl_window.cpp`, `sdl_input.cpp`) | Emscripten's SDL2 port (`-sUSE_SDL=2`) covers this; need pointer-lock, fullscreen-on-gesture, key mapping review | S–M |
| Audio | SDL audio device + software mixer (`shared/sdl/sdl_sound.cpp`, `snd_dma.cpp`); OpenAL/EAX only on Windows | SDL2 audio works via WebAudio; must resume `AudioContext` on first user gesture | S |
| Music/cinematics | Bundled `mp3code`, ROQ decoder in `cl_cin.cpp` | Pure C/C++, should compile as-is | S |
| Renderer: rd-vanilla | Fixed-function GL1.x: `qglBegin`/`glVertex` (~28 sites), `glMatrixMode`/`glPushMatrix`, client arrays, `glTexEnv`, fog, `glAlphaFunc`, display lists, NV register combiners, `glLockArraysEXT` | WebGL2 is GLES3-only. Needs GL1 emulation or a rewrite of the backend | L |
| Renderer: rd-rend2 (MP only) | GLSL `#version 150 core`, needs `ARB_buffer_storage`, `ARB_texture_storage`, timer queries, `GL_DEPTH_CLAMP`, `glPolygonMode`, `glMultiDrawElementsBaseVertex`, `glGetCompressedTexImage`, **geometry shaders** (`weather.glsl`, `shadowvolume.glsl`, `prefilterEnvMap.glsl`), some `qglBegin` | Closer to WebGL2 than vanilla, but geometry shaders & several GL3.2+/4.x features have no WebGL2 equivalent | L |
| Filesystem | `files.cpp` reads pk3 via bundled minizip; writes config/saves to `fs_homepath` | MEMFS/IDBFS or WASMFS+OPFS; assets must be provided by the user (copyright) | M |
| Networking (MP) | UDP sockets in `codemp/qcommon/net_ip.cpp`, SOCKS, master server queries | Browsers cannot open UDP. Needs WebSocket/WebRTC transport + proxy or browser-hosted server | L |
| Loading screens | `SCR_UpdateScreen` called from inside long synchronous loads | With a plain main loop the screen freezes during map load; ASYNCIFY/JSPI fixes it | S–M |
| Memory | Zone/hunk allocators, large BSP/ghoul2 data | Use `-sALLOW_MEMORY_GROWTH`, budget ~512 MB–1 GB; monitor on mobile | S |
| Code size | ~1.2 M lines incl. 3 game variants (JA SP, JA MP, JK2 SP) | Ship one variant per build | — |
| CI | GitHub Actions for Windows/macOS/Linux (`.github/workflows/build.yml`) | Add an Emscripten job (`emsdk` action) | S |

### Key observations

- **No blocking low-level issues.** No threads, asm is already
  fenced off, 32-bit is supported, and audio/input already go through SDL2.
- **The renderer is the main job.** rd-vanilla is entirely fixed-function;
  rd-rend2 targets desktop GL 3.2 core + extensions and uses geometry shaders.
- **Module loading and networking are design decisions**, not porting
  chores.
- **SP is the right first target**: only three modules (engine, `jagame`,
  `rdsp-vanilla`), no networking, no rend2.

## 2. Target & scope

1. **Milestone A — JA single-player**, rd-vanilla, local assets, desktop
   Chrome/Firefox/Safari.
2. **Milestone B — JA multiplayer** (client + listen server in the
   browser), WebSocket transport.
3. **Milestone C — rend2 on WebGL2** (optional, MP only).
4. JK2 SP (`codeJK2`) falls out of Milestone A almost for free.

Out of scope initially: dedicated server in wasm, legacy (`vm_legacy`)
mod DLLs, third-party native mods (those would need recompiling to wasm).

## 3. Design decisions

### 3.1 Module loading
Options:

- **(Recommended) Static linking + module registry.** Build engine,
  renderer and game modules into a single wasm. Replace `Sys_LoadDll*` on
  Emscripten with a lookup table: `"rdsp-vanilla" → GetRefAPI`,
  `"jagame" → GetGameAPI`, etc. The engine already talks to modules only
  through import/export function tables, so the call sites don't change.
  *Cost:* global-symbol collisions between modules (e.g. `Com_Printf` is
  defined in `common.cpp`, `game/g_main.cpp` and every `tr_subs.cpp`;
  shared `q_shared`/`q_math` code is compiled into every module). Fix by
  compiling each module with a symbol-prefix header (`-include
  module_prefix.h` that `#define`s colliding names), or by making
  module-local helpers `static`. Smallest, fastest output, no runtime
  linker.
- **Emscripten dynamic linking** (`MAIN_MODULE`/`SIDE_MODULE`, `dlopen`
  on preloaded `.wasm`). Keeps the code nearly unchanged and keeps mod
  support possible, but adds runtime size/startup cost and still hits
  symbol-visibility problems. Keep as a fallback / later option for mods.

### 3.2 Main loop
- Refactor `main()` so the body of the `while(1)` loop is a function
  passed to `emscripten_set_main_loop(…, 0, 1)` (vsync via rAF).
- Start with **ASYNCIFY** (or **JSPI** where available) so that
  `SCR_UpdateScreen` during loading can yield a frame and so that any
  remaining blocking waits (`Sys_Sleep`) don't hang the tab. Measure the
  size/speed hit; if too high, restrict with `ASYNCIFY_ONLY` to the loading
  path.

### 3.3 Renderer (rd-vanilla → WebGL2)
Ordered by effort:

1. **Emscripten `LEGACY_GL_EMULATION=1` + `GL_UNSAFE_OPTS=0`.**
   ioquake3 ships its Emscripten build this way, which is strong precedent
   for an id Tech 3 renderer. Gaps to patch: display lists (`qglCallList`/
   `qglGenLists`), `glPolygonMode`, `glClipPlane`, `glDrawBuffer`; NV
   register combiners and `glLockArraysEXT` are already optional
   extensions — just make sure they report as unavailable.
2. **gl4es compiled to wasm** if (1) is too slow or incomplete — more
   complete GL1 coverage, more code.
3. **Port the vanilla backend to a small GLES3 shader path** (replace
   `qglBegin` sites with vertex buffers, emulate texenv/fog/alpha test in a
   handful of shaders). Most work, best performance; good long-term goal
   once something runs.

Also: request a GLES/WebGL2 context in `sdl_window.cpp`
(`SDL_GL_CONTEXT_PROFILE_ES`, 3.0); avoid `glReadPixels` on hot paths
(screenshots only); prefer uncompressed or ETC/ASTC/S3TC-via-extension
textures depending on `WEBGL_compressed_texture_*` availability.

### 3.4 rend2 (Milestone C)
- Translate `#version 150 core` → `#version 300 es` + precision qualifiers
  in `tr_glsl.cpp`.
- Gate out geometry-shader features (weather particles, shadow volumes,
  env-map prefilter) or rewrite: weather → instanced quads with
  transform-feedback update (WebGL2 has TF); env prefilter → six draws.
- Replace `buffer_storage`/persistent mapping with `glBufferSubData`;
  drop timer queries, `GL_DEPTH_CLAMP`, `glPolygonMode`, BaseVertex
  draws (rebase indices on CPU), `glGetCompressedTexImage`.
- Float render targets need `EXT_color_buffer_float`.

### 3.5 Filesystem & assets
- Never ship retail pk3s. Provide a launcher page that lets the user pick
  their `GameData/base` folder or `assets*.pk3` files (File System Access
  API / `<input type=file webkitdirectory>`), copies them into **OPFS**
  once, and mounts them on start (WASMFS OPFS backend, or copy into MEMFS
  at boot).
- Mount `fs_homepath` on IDBFS/OPFS for `openjk_sp.cfg`, saves and
  screenshots; flush with `FS.syncfs` after config writes and saves.
- Set `fs_basepath`/`fs_homepath` from the launcher; force
  `BuildPortableVersion`-style paths.
- Ship the OpenJK-built `.pk3`s (if any) and the wasm via normal HTTP with
  gzip/brotli.

### 3.6 Networking (Milestone B)
- Introduce a transport abstraction under `NET_SendPacket`/
  `NET_GetPacket` in `net_ip.cpp`; keep the UDP impl for native.
- Web impl: **WebSocket** to a small relay/proxy (`websockify`-style
  WS↔UDP bridge) so browsers can join native servers. This is the
  approach used by browser Quake 3 ports.
- Optional later: **WebRTC DataChannels** (unreliable/unordered) for
  browser-hosted listen servers and lower latency.
- Disable SOCKS, LAN broadcast and master-server UDP queries on web; serve
  the server list over HTTPS from the relay.

### 3.7 Input & UX
- Pointer lock on canvas click; Esc handling conflicts with pointer lock
  exit — remap console/menu keys.
- Fullscreen + audio unlock on first gesture.
- Handle canvas resize → `vid_restart`-free resize of `glConfig`.
- Gamepad via SDL2 game controller (Emscripten supports the Gamepad API).

## 4. Work plan

### Phase 0 — Toolchain (≈1 week) — done
- [x] Build with `emcmake` (no separate toolchain file needed);
      `Architecture = wasm32`.
- [x] `__EMSCRIPTEN__` block in `shared/qcommon/q_platform.h`.
- [x] Building with Emscripten forces internal zlib/png/jpeg and
      `-sUSE_SDL=2`, and turns off dedicated, rend2, MP, tests and JK2
      (no separate `BuildForWeb` option needed).
- [x] `openjk_sp`, `jagame` and `rdsp-vanilla` compile under emcc.
      `sys_unix.cpp` needed no changes.
- [x] CI job using `mymindstorm/setup-emsdk`.

### Phase 1 — SP boots to the menu (≈2–3 weeks)
- [x] Static module registry for `rdsp-vanilla` and `jagame`; resolve
      duplicate symbols.
- [x] Main loop via `emscripten_set_main_loop`. ASYNCIFY isn't on yet.
- [x] `-fwasm-exceptions` for `Com_Error`.
- [x] `LEGACY_GL_EMULATION` (WebGL 1 for now); stub missing GL1 bits;
      texture names and formats fixed for WebGL.
- [x] Asset loader page (OPFS), IDBFS home path.
- [ ] Milestone check: main menu renders, console works, sound plays.
      (Engine, renderer, sound and UI initialise; needs real game data.)

### Phase 2 — SP playable (≈3–4 weeks)
- [ ] Load `t1_*` maps; fix renderer gaps (fog, sky, ghoul2 surfaces,
      dynamic glow, weather, saber trails, shadows).
- [ ] ROQ cinematics + MP3 music.
- [ ] Save/load round-trip, persisted across reloads. (Written files are
      persisted, tested with the config file.)
- [x] Pointer lock, fullscreen, resize (the canvas scales to the window).
      Gamepad untested.
- [ ] Perf pass: `-O3`, LTO, `--closure`, profile hot paths in Chrome
      DevTools; target 60 fps on mid-range laptops.

### Phase 3 — MP (≈4–6 weeks)
- [x] Add MP engine + `cgame`/`ui`/`jampgame` modules to the registry.
- [x] Network transport + WebSocket client (`net_web.cpp`).
- [x] WS↔UDP relay (`tools/web/server.js`, `tools/web/Dockerfile`).
- [x] Server browser: master server queries go through the relay like
      any other packet (no separate HTTPS list needed).
- [ ] Listen server in-browser (bots work offline): should work over the
      loopback, needs real game data to check.

### Phase 4 — Optional
- [ ] Native GLES3 path for rd-vanilla (drop GL1 emulation). Not started;
      worth doing only if the emulation turns out too slow or incomplete
      with real data.
- [x] rend2 on WebGL2 (§3.4), experimental, without the geometry shader
      features.
- [ ] Emscripten dynamic linking for mods. Not started.
- [ ] Mobile touch controls. Not started; memory is the bigger obstacle.
- [ ] WebGPU backend (long term). Not started.

## 5. Risks

| Risk | Mitigation |
|---|---|
| GL1 emulation too slow/incomplete for JA's shader stages | Fall back to gl4es; long term native GLES3 backend |
| Symbol collisions when statically linking modules | Per-module prefix header; or Emscripten side modules |
| ASYNCIFY bloat / slowdown | `ASYNCIFY_ONLY` list, or JSPI; or drop and accept frozen load screen |
| Multi-GB asset import into browser storage | OPFS (quota usually large); show progress; keep only needed pk3s |
| Safari WebGL2/OPFS/pointer-lock quirks | Test matrix in CI with Playwright; document supported browsers |
| UDP-only MP ecosystem | Relay service; can't connect to arbitrary servers without it |
| Licensing | GPLv2 code is fine to host; retail assets must come from the user |

## 6. First concrete PR

1. `q_platform.h` Emscripten block.
2. `cmake/Toolchains/emscripten.cmake` + `BuildForWeb` option.
3. `shared/sys/sys_main.cpp`: split loop body into `Sys_Frame()`, call it
   from `emscripten_set_main_loop` under `__EMSCRIPTEN__`.
4. Static module table behind `Sys_LoadDll` / `Sys_LoadSPGameDll` under
   `__EMSCRIPTEN__`.
5. CI job that just builds `openjk_sp.html` (no runtime test yet).
