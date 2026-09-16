# CATHODE  -  Scene catalog & authoring guide

A **scene** is a self-contained visual/simulation module implementing the
`Scene` vtable (`include/cathode/scene.h`). The engine drives every scene the
same way: `init → (update, render)* → destroy`, piping the rendered linear-RGB
framebuffer through the CRT chain to the terminal.

## The catalog (50+ scenes; 56 registered)

Run `bin/capture` with no args for the authoritative live list; indices shift as
scenes are added. Headless timing drives the same roster at **1440p**
(2560×1440) with a **&lt;2 ms/frame** gate - see `docs/BENCHMARKS.md`. Current
roster and backing language:

| name | what it is | backing |
|------|-----------|---------|
| starfield | 3D star-warp with motion streaks over an fbm nebula | C |
| galaxy | live Barnes-Hut N-body galaxy / collision | C |
| raymarch | SDF primitives, mandelbulb, infinite field (multithreaded) | C |
| solids | Phong-shaded procedural meshes via the rasterizer | C |
| fluid | stable Navier-Stokes colored dye (grid, Stam) | C |
| tunnel | classic demoscene tunnel | C |
| plasma | multi-sine + fbm plasma | C |
| terrain | flight over an infinite fractal landscape | C |
| mandelbrot | animated deep-zoom (double precision) | C |
| life | Conway's Game of Life with phosphor heat-trails | C |
| boids | Reynolds 3D flocking with heading-colored trails | C |
| attractor | Lorenz/Aizawa/Thomas/Halvorsen oscilloscope trails | C |
| slime | Physarum slime-mold transport network | C |
| lenia | continuous cellular automaton (lifelike creatures) | C |
| flow | de Jong / Clifford attractor density field | C |
| julia | animated Julia set via NEON fractal kernel | C + asm |
| ripple | 2D wave-equation water with NEON-blurred caustics | C + asm |
| cells | Worley/Voronoi living cell texture (domain-warped) | C |
| flame | fractal flame (chaos game + nonlinear variations) | C |
| ifs | iterated function systems (Barnsley fern, Sierpinski) | C |
| sph | smoothed-particle hydrodynamics fluid (Lagrangian) | C |
| buddhabrot | Buddhabrot / Nebulabrot escape-orbit density | C |
| tesseract | rotating 4D polytopes (real 4D→3D→2D projection) | C |
| pendulum | double-pendulum chaos array (RK4, sensitive deps) | C |
| bz | Belousov-Zhabotinsky excitable-media spiral waves | C |
| orbital | volume-rendered hydrogen \|ψ\|² electron clouds | C |
| wireworld | Wireworld electron cellular-automaton circuits | C |
| observatory | terrain + N-body galaxy, one camera (NEON projector) | C + asm |
| brain | Brian's Brain 3-state cellular automaton | C |
| parametric | rotating parametric surfaces: Möbius, trefoil, Klein | C |
| demoscene | copper raster bars + parallax stars + sine-scroller | C |
| bootscreen | retro power-on POST: ROM banner, RAM test, blink cursor | C |
| credits | perspective "star wars" credits crawl (bitmap font in 3D) | C |
| hyperbolic | animated {p,q} hyperbolic tilings (Poincaré disk, threaded) | C |
| qjulia | ray-marched quaternion Julia set (4D fractal, threaded) | C |
| physics | 2D rigid-body playground: polygons tumble, collide, stack | C |
| planet | procedurally-textured rotating planet (fbm, UV texture map) | C |
| voxel | Comanche-style voxel-heightmap terrain flyover (no polygons) | C |
| truchet | animated Truchet quarter-arc tilings (interlocking loops) | C |
| chladni | Chladni cymatics: sand collects on plate nodal lines | C |
| magnetic | magnetic-pendulum fractal basins of attraction (threaded) | C |
| fire | classic Doom-PSX fire effect + burning CATHODE text | C |
| reaction | Gray-Scott reaction-diffusion (Turing patterns) | **Rust** |
| cloth | waving flag: Verlet cloth + Phong rasterizer | **Rust** |
| dla | diffusion-limited aggregation crystal growth | **Rust** |
| wfc | Wave Function Collapse procedural tiles (live solve) | **Rust** |
| spectrogram | scrolling FFT waterfall of a synthesized signal | **Rust** |
| maze | maze generation + BFS flood-fill solve + path trace | **Rust** |
| lsystem | animated L-system plants (turtle-graphics grammar) | **Rust** |
| chiptune | live tracker UI (pattern grid) + synth + FFT spectrum | C + **C++** + **Rust** |
| pathtrace | Monte-Carlo BVH path tracer | **C++** |
| wireframe | neon vector-display wireframe city | **C++** |
| audioviz | synth spectrum + waveform + waterfall | **C++** |
| metaballs | marching-cubes isosurface + Phong rasterizer | **C++** |
| csg | constructive solid geometry: animated boolean solid | **C++** |
| softbody | bouncing pressurized soft-body blobs (Verlet + gas) | **C++** |

## The Scene vtable

```c
struct Scene {
    const char *name;         // short id, also the CLI selector
    const char *description;  // one-liner
    void  (*init)(Scene*, i32 w, i32 h);   // allocate state at this resolution
    void  (*update)(Scene*, f32 dt, f32 t);// advance sim; dt=frame delta, t=abs time
    void  (*render)(Scene*, Framebuffer*); // write LINEAR RGB (HDR ok, >1.0 fine)
    void  (*on_key)(Scene*, int key);      // a Key enum value (see tui.h)
    void  (*destroy)(Scene*);              // free everything
    CrtConfig (*preferred_crt)(Scene*);    // pick a CRT look for this scene
    void  *state;                          // your per-scene struct
};
```

## Authoring checklist (copy an existing scene as a template)

1. **Create** `src/scenes/scene_<name>.c`. Include `cathode/scene.h`,
   `cathode/tui.h` (for the `Key` enum in `on_key`), `framebuffer.h`, `vec.h`,
   and whatever subsystem you use (`noise.h`, `raster.h`, `sdf.h`, `physics.h`,
   `rustcore.h`, `cppcore.h`, `fractalkernel.h`, …).
2. **Define** a `<Name>State` struct; stash it in `sc->state`.
3. **Implement** the vtable functions. Rules:
   - `render` writes **linear RGB**; never tonemap (the CRT/TUI do that). HDR
     values > 1.0 are encouraged  -  they drive bloom.
   - Keep per-frame work **bounded** (no unbounded loops). Interactive TUI
     sizes are modest; headless / bench still expects the scene to meet the
     **1440p &lt;2 ms/frame** gate when measured with CRT enabled. Downscale a
     sim grid and upscale on render if the per-cell cost is high (see
     `reaction`, `lenia`).
   - Never read stdin or block.
   - Handle any framebuffer size (the app rebuilds buffers on terminal resize);
     derive grid sizes from `w,h` in `init`, clamped to sane bounds.
4. **Register** it: add the factory prototype to `scene.h`, a
   `scene_register("<name>", scene_<name>_create)` call in
   `src/app/registry.c` (in `scenes_register_all` for pure-C, or
   `scenes_register_polyglot` for Rust/C++-backed), and the factory itself at
   the bottom of your `.c`.
5. **Build & look**: `make` then
   `bin/capture <name> <frames> /tmp/x.png <w> <h>`  -  open the PNG. For motion,
   `bin/capture --gif <name> <frames> /tmp/x.gif` makes an animated loop.
6. **Tune** against the render. Common fixes seen in this codebase:
   - washed-out / all-one-color → your value→color mapping needs contrast
     (normalize to the field's real range, add a gamma/sqrt, see `reaction`).
   - a sim that dies to a uniform field → parameters outside the stable regime;
     do an **offline parameter sweep** to find a living configuration before
     wiring it in (this is how `lenia`'s mu/sigma were found).
   - too dim → the CRT tonemap is Reinhard; push brighter values / add an HDR
     core so highlights bloom (see `galaxy`, `starfield`).

## Choosing a CRT preset

`preferred_crt` returns a `CrtConfig`; start from a preset and tweak:
`crt_config_preset("trinitron"|"broadcast"|"vhs"|"arcade"|"clean")`. Bump
`bloom`/`persistence` for glowing or trailing content (fluids, attractors,
life); use `clean` for crisp geometry.

## Threading

The app renders on the main thread today. For an expensive per-pixel scene, the
`ThreadPool` (`src/app/threadpool.h`, `tp_run_bands`) splits a row range across
cores; the SDF marcher's `sdf_render_band` is band-friendly. Wire a scene's
`render` to `tp_run_bands` if it becomes the frame bottleneck.
