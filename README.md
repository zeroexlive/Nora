# Nora

Nora is a physically based renderer written in C99. It uses Metropolis light transport (Kelemen-style
primary sample space MLT) on top of a unidirectional path tracer with next event estimation, and renders in
either RGB or spectral mode. It has an interactive GUI built on [CUI](https://github.com/yappologistic/CUI),
a node-based shader editor that generates Open Shading Language, its own OSL-subset compiler and VM,
OBJ/MTL import, and a headless CLI.

| RGB, PSSMLT | Spectral, PSSMLT (dispersive glass) |
|---|---|
| ![Cornell box, RGB](renders/cornell_rgb_mlt.png) | ![Cornell box with a dispersive glass sphere, spectral](renders/cornell_glass_spectral_mlt.png) |

Both images are 512 x 512 with 95 s of MLT on two aarch64 cores (`nora-cli cornell --time 95` and
`nora-cli cornell-glass --mode spectral --time 95`).

## Features

**Light transport**
- Kelemen et al. 2002 primary sample space MLT (PSSMLT) over a path tracer with NEE and multiple
  importance sampling (power heuristic).
  - Large steps draw fresh uniform numbers; small steps use Kelemen's exponential perturbation
    (s1 = 1/1024, s2 = 1/64). Primary sample vectors are mutated lazily.
  - Bootstrap normalisation `b` from independent paths. Initial chain states are resampled from the bootstrap
    in proportion to their luminance.
  - Many independent chains (64 by default) run on worker threads. They splat expected values
    (accepted and rejected contributions weighted by `a` and `1 - a`).
- A plain progressive path tracer (`--integrator pt`). It is also the GUI's fast preview while the camera
  moves.
- Threads: pthreads on POSIX, Win32 threads on Windows, through a small shim (`src/core/nthread.c`).

**Colour**
- RGB mode: linear sRGB throughput.
- Spectral mode: hero wavelength sampling over 360 to 830 nm, with four wavelengths per path (one hero and
  three rotated companions).
  - Reflectances and emission colours are uplifted to spectra with the sigmoid-polynomial model of Jakob and
    Hanika 2019. The coefficient table is fitted at start-up (about 0.2 s) by Nora's own Gauss-Newton
    solver, so no third-party tables are used. Round-trip error is below 1% in the tests.
  - Spectra go through the CIE 1931 2-degree colour matching functions (1 nm table), then XYZ to linear sRGB.
    The white point is normalised so an RGB scene renders alike in both modes.
  - Wavelength-dependent IOR (Cauchy fit from IOR and Abbe number) collapses the path onto the hero
    wavelength, which gives real dispersion.

**Geometry and materials**
- Binned SAH BVH over triangles, Moller-Trumbore intersection.
- Materials:
  - Lambertian diffuse.
  - Conductor (smooth or rough GGX). Colours use Gulbrandsen's artist-friendly complex Fresnel
    (reflectance and edge tint), uplifted per wavelength in spectral mode.
  - Dielectric (smooth or rough GGX after Walter et al. 2007), with dispersion in spectral mode.
  - Plastic (diffuse under a GGX coat).
  - Emission on any material (one- or two-sided).
- GGX with visible normal sampling (Heitz 2018).
- Thin lens camera (aperture and focus distance).
- Output: PNG (stb_image_write from CUI's `third_party`), OpenEXR (uncompressed, 32-bit float) and PFM.
  PNG tone mapping is clamp, Reinhard or ACES, with an exposure control.

**Scenes**
- Built-in Cornell box (`cornell`), and a variant with a dispersive glass sphere (`cornell-glass`).
- OBJ/MTL import through the vendored [schmarta/obj](https://github.com/schmarta/obj) (MIT; see
  [third_party/obj/README.nora.md](third_party/obj/README.nora.md) for the fixes made to it). MTL maps to Nora
  materials as described in [MTL mapping](#mtl-mapping).

## Building

You need CMake 3.16 or newer and a C99 compiler. CUI is fetched with `FetchContent`, pinned to `v1.4.0`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Linux the GUI needs `libx11-dev` and `libgl1-mesa-dev`. CUI falls back to its software renderer when
OpenGL 3.3 is not available. To build the renderer without X11:

| Option | Default | Meaning |
|---|---|---|
| `NORA_BUILD_GUI` | `ON` | Build the `nora` GUI executable (CUI window layer). `OFF` builds CUI's core only (no X11) |
| `NORA_BUILD_TESTS` | `ON` | Unit tests, CLI smoke tests, headless GUI test |
| `NORA_WITH_OSL` | `OFF` | Also build the C++17 shim for the real OpenShadingLanguage (see below) |
| `NORA_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |

Targets:
- `nora-cli`: the headless renderer.
- `nora`: the GUI.
- `nora_tests`: unit tests.
- `nora_gui_headless`: drives the GUI through CUI's software renderer and writes a screenshot.

## Command line

```sh
nora-cli scene.obj --mode spectral --spp 64 -o out.png
nora-cli cornell --integrator pt --spp 256 -o out.exr -o out.png
nora-cli cornell-glass --mode spectral --time 120 --width 800 --height 800 -o glass.png
nora-cli scenes/shaderball.obj --shader examples/shaders/marble.osl --material Ball -o ball.png
nora-cli cornell --graph examples/graphs/copper.json --material White -o copper.png
nora-cli --graph examples/graphs/noisy_plastic.json --dump-osl
```

`--spp` means samples per pixel for the path tracer and mutations per pixel for MLT. Run `nora-cli --help`
for the full list: threads, size, max depth, large step probability, bootstrap and chain counts, seed,
exposure, tone mapping, field of view, aperture and focus, a constant environment light, and `--real-osl`.

OBJ files without emitters get a white constant environment light. The camera frames the scene bounds.

## GUI

```sh
nora                      # Cornell box with the glass sphere
nora scenes/shaderball.obj
```

The window is a CUI dockspace. You can drag tabs to re-dock them. The panels are:
- **Viewport**: the progressive render, drawn as a texture on a CUI canvas.
  - Left drag orbits, Shift+left or middle drag pans, right drag (or Ctrl+left) dollies.
  - `+`/`-` zoom, the arrow keys orbit, and `F` frames the scene.
  - While the camera moves, a low-resolution path traced preview runs. Half a second after it stops, Nora
    switches to progressive MLT at full resolution (this can be turned off in Render Settings).
  - CUI 1.4 does not pass mouse wheel events to custom canvases, so zoom uses drags and keys.
- **Node Editor**: CUI's node editor, holding the shader graph of the selected material.
  - Node types:
    - Inputs: Geometry, Value, RGB.
    - Math, with 12 operations.
    - Mix Color.
    - Textures: Image, Noise (fBm), Checker.
    - Closures: Diffuse BSDF, Microfacet GGX (reflect, refract, or both), Reflection, Refraction, Emission,
      Transparent, Add Shader and Mix Shader.
    - Material Output.
  - Unlinked inputs show a slider or colour picker inside the node.
  - Graphs save and load as JSON. *Compile & assign* generates OSL, compiles it and assigns it to the
    material.
- **OSL Source**: the generated OSL in CUI's code editor. It is live and editable: edits are checked with
  the built-in compiler as you type and can be assigned to the material.
- **Outliner**: objects and materials. Selecting an object selects its material.
- **Properties**: material type, colours, roughness, IOR, Abbe number and emission. Changes restart the
  preview.
- **Render Settings**:
  - RGB or spectral mode, MLT or PT.
  - Large step (mutation) probability, mutations or samples per pixel, time limit, threads, depth and
    resolution.
  - Exposure and tone mapping.
  - Render, Stop and Save image (PNG, EXR or PFM).
- **Log**: renderer and compiler messages.

## Open Shading Language

Nora supports OSL in two layers.

### (a) Built-in OSL-subset compiler (`src/osl`, C99, always on)

This is a lexer, a recursive descent parser, a type checker and a code generator for a register bytecode,
plus a VM. Shaders run per shading point on every render thread, each thread with its own VM state, in both
RGB and spectral modes. Closure weights are RGB and are uplifted to spectra at the BSDF.

Supported:

| Area | Coverage |
|---|---|
| Declarations | `surface` (also `shader`) with parameters and defaults; defaults may use globals and earlier parameters; `output` parameters; metadata `[[ ... ]]` is skipped; `#include` lines are ignored |
| Types | `float`, `int`, `color`, `point`, `vector`, `normal`, `string`, `closure color`, `void` (functions) |
| Expressions | arithmetic with float/triple broadcast, `%`, comparisons, `&&` `\|\|` `!` (short circuit), ternary, compound assignment, `++`/`--`, casts, component access `c[i]` (constant or computed), type constructors including `color("hsv", h, s, v)` |
| Statements | blocks with scoping, `if`/`else`, `for`, `while`, `do`/`while`, `break`, `continue`, `return` |
| Functions | user functions (inlined, `output` parameters by reference, recursion is reported) |
| Globals | `P I N Ng dPdu dPdv u v time Ci`, plus `M_PI` and the other math constants |
| Math library | `sin cos tan asin acos atan atan2 sinh cosh tanh sqrt inversesqrt exp exp2 log log2 log10 pow abs fabs floor ceil round trunc sign mod fmod min max clamp mix step smoothstep radians degrees hypot isnan isinf` |
| Geometry library | `dot cross length distance normalize faceforward reflect refract luminance transform` (identity: Nora shades in world space) `backfacing raytype` |
| Noise and textures | `noise snoise cellnoise` and `noise("perlin"/"uperlin"/"cell"/"simplex", p[, t])`, float or colour by context; `texture(file, s, t, ...)` through the host (bilinear, wrapping, `t = 0` at the top) |
| Closures | `diffuse`, `oren_nayar` (as diffuse), `microfacet("ggx", N, alpha, eta, refract)` and the 7-argument anisotropic form (averaged), `reflection(N[, eta])`, `refraction(N, eta)`, `emission()`, `transparent()`; `holdout` and `background` compile to nothing; closures add and scale by colours and floats |
| Diagnostics | `line:col: error: ...` for syntax and type errors; runtime guard against infinite loops |

Not supported: arrays, structs, matrices, `getattribute`, `pnoise` and other noise variants, derivatives
(`Dx`, `Dy`, `area`, `calculatenormal`), message passing, string functions, displacement, volume and light
shaders, and closures other than those listed. `N` is face-forwarded toward the viewer as in OSL's
testrender, so refractive shaders should use `backfacing() ? 1/eta : eta`, as the example shaders do.
`emission()` is treated as radiance equal to its weight.

Example shaders are in [examples/shaders](examples/shaders): `plastic`, `glass`, `checker`, `emissive`,
`marble` (a user function and loops), `brushed_metal` and `textured`. The test suite has about 200 checks
for the compiler and VM (`nora_tests osl`).

### (b) Real OSL through liboslcomp and liboslexec (`-DNORA_WITH_OSL=ON`, optional)

`src/osl/real/osl_real.cpp` is a C++17 shim behind a small C ABI (`osl_real.h`). With OpenShadingLanguage
found by `find_package(OSL CONFIG)`, it:
- compiles source with `OSLCompiler::compile_buffer`,
- loads it with `ShadingSystem::LoadMemoryCompiledShader`,
- builds a shader group and executes it per shading point (a thread-local `ShadingContext`),
- walks the `Ci` closure tree into Nora's closure list.

It registers `diffuse`, `oren_nayar`, `microfacet`, `reflection`, `refraction`, `emission` and
`transparent`. `nora-cli --real-osl` enables it (the GUI and the CLI fall back to the built-in compiler when
a shader fails).

If the flag is on but OSL is not found, the shim compiles as a stub and reports that it is unavailable, so
the flag never breaks a build. The default build never needs C++, LLVM or OSL.

Status: the stub mode is built and tested in CI. The real mode is a best-effort implementation against the
OSL 1.12/1.13 API. A non-blocking CI job tries it on macOS with Homebrew's `open-shading-language`; it was
not compiled in the development sandbox.

## Shader graph JSON

```json
{
  "format": "nora-shader-graph",
  "version": 1,
  "nodes": [ { "id": 2, "type": "microfacet", "x": 340, "y": 100, "op": 0, "text": "",
               "values": [[1, 1, 1], [0.35, 0, 0], [0, 0, 0], [0, 0, 0]] } ],
  "links": [ { "from": [3, 0], "to": [2, 0] } ]
}
```

`values` holds the unlinked input values in input order, and `op` is the operation index (Math, Microfacet).
A link goes from `[node, output slot]` to `[node, input slot]`.

## MTL mapping

| MTL | Nora |
|---|---|
| `d < 1`, `Tr > 0`, `Tf < 1` or `illum` 4/6/7/9 | dielectric: `Ni` is the IOR, `Tf` the tint, `Pr` the roughness |
| `Pm >= 0.5` or `illum 3` | conductor: `Ks` (else `Kd`) is the reflectance; roughness from `Pr`, else from `Ns` |
| `Ks > 0` with `illum >= 2` | plastic: `Kd` base and a GGX coat (`Ni`, `Ns`/`Pr`) |
| otherwise | diffuse `Kd` |
| `Ke` | emission (two-sided) |
| `map_Kd` | albedo texture (PNG, JPEG, HDR) |
| `nora_type`, `nora_abbe`, `nora_roughness`, `nora_edge r g b`, `nora_shader file.osl` | Nora extensions |

## Tests

`ctest` runs:
- `spectrum`: uplift round trip, CMFs, white normalisation.
- `bsdf`: Fresnel, GGX normalisation, furnace and energy tests, sample/eval consistency.
- `bvh`: against brute force.
- `obj`: quads, negative indices and every index form, MTL mapping.
- `osl`: about 200 checks.
- `graph`: every node type compiles, JSON round trip, cycles.
- `render`:
  - MLT agrees with the path tracer within 8%, and spectral with RGB within 5%.
  - Determinism, the async API, EXR/PFM/PNG writing.
- `gui_headless`: builds every panel with CUI's software renderer, previews and renders with MLT, assigns a
  graph shader, saves.
- Five CLI smoke renders, among them an OBJ with an OSL shader and a shader graph.

## Roadmap

- **Multiplexed MLT over bidirectional path tracing (MMLT/BDPT).** Not implemented in 0.1. The lazy primary
  sample space sampler, the bootstrap and the splatting would carry over; BDPT itself (light subpaths,
  connection MIS) and per-strategy stream splitting are the missing parts.
- Mouse wheel zoom once CUI exposes scroll input to canvases.
- Environment maps with importance sampling; more light types.
- OSL: arrays, matrices, `getattribute`, derivatives.
- Texture filtering (MIP maps), normal maps.
- Instancing and transforms in the scene format.

## License

Nora is MIT licensed (see [LICENSE](LICENSE)). Third-party parts:
- [CUI](https://github.com/yappologistic/CUI), MIT. It is fetched at configure time; its stb headers
  (public domain or MIT) are used for PNG writing and image loading.
- [obj](https://github.com/schmarta/obj), MIT, Copyright (c) 2024 Lucas Marta. Vendored in
  `third_party/obj`, with changes.
- CIE 1931 2-degree colour matching functions: public CIE standard data.
