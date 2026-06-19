# Light Propagation Volumes GI (Unreal Engine 5)

A free, open-source **real-time dynamic global illumination** plugin for Unreal
Engine 5, based on cascaded **Light Propagation Volumes** (Kaplanyan &
Dachsbacher 2010). It plugs into UE5's official *Plugin* dynamic GI hook with
**no engine modification or recompile required**, and works on a stock binary engine.

It's a lightweight alternative to Lumen for diffuse GI: cheap enough for low-end
hardware, mobile and VR, while still giving stable, full-range, sun-driven
indirect lighting.

## Features

- No engine changes. Installs into any UE 5.6 project as a normal plugin.
- View-independent injection from a sun-aligned Reflective Shadow Map
  (SceneCapture, public API), so it's stable under camera motion and any AA.
- Cascaded volumes (up to 4) for both near detail and long range.
- SH light propagation with Global SDF occlusion to limit light leaking.
- Diffuse plus cheap glossy/specular GI sampled from the volume in the reflection direction.
- Temporal accumulation with grid-snapped reprojection, virtually flicker-free.
- Multi-bounce indirect lighting via light-field feedback.
- Sun shadows come for free (the captured flux is already shadowed).
- Driven entirely by `r.LPVGI.*` console variables.

## Requirements

- Unreal Engine **5.6** (deferred renderer, SM5/SM6).
- *Generate Mesh Distance Fields* enabled (for the SDF occlusion feature).

## Installation

1. Copy the `LightPropagationVolumesGI` folder into your project's `Plugins/`
   directory (create it if it doesn't exist).
2. Open the project and let it compile (a C++ project is required; if your
   project is Blueprint-only, add any empty C++ class first).
3. Enable **Light Propagation Volumes GI** in *Edit → Plugins* and restart.

## Enabling the effect

Set the dynamic GI method to **Plugin**:

- *Project Settings → Rendering → Dynamic Global Illumination Method → Plugin*, or
- a Post Process Volume's *Global Illumination → Method → Plugin*, or
- console: `r.DynamicGlobalIlluminationMethod 3`

The plugin uses the brightest/atmosphere **Directional Light** as the GI caster.
You'll usually also want to turn **Lumen Reflections off** (`r.ReflectionMethod 0`).

Recommended project settings (`Config/DefaultEngine.ini`):

```ini
[/Script/Engine.RendererSettings]
r.DynamicGlobalIlluminationMethod=3
r.ReflectionMethod=0

[SystemSettings]
r.AOGlobalDistanceField.DetailedNecessityCheck=0
```

## Console variables

| CVar | Default | Description |
|------|---------|-------------|
| `r.LPVGI.Enable` | 1 | Master on/off. |
| `r.LPVGI.Intensity` | 4 | Overall indirect light intensity. |
| `r.LPVGI.Specular` | 1 | Glossy/specular GI strength (0 = diffuse only). |
| `r.LPVGI.CellSize` | 50 | Cell size (cm) of the first (finest) cascade. |
| `r.LPVGI.NumCascades` | 3 | Number of nested cascades (1–4). Each is 2× coarser/larger. |
| `r.LPVGI.EdgeFade` | 0.15 | Cross-cascade blend region (fraction of a cascade). |
| `r.LPVGI.PropagationSteps` | 8 | Light propagation iterations. |
| `r.LPVGI.Feedback` | 0.6 | Multi-bounce strength (0 = single bounce). |
| `r.LPVGI.Occlusion` | 1 | Enable propagation occlusion. |
| `r.LPVGI.Occlusion.SDF` | 1 | Build occlusion from the Global SDF (vs injected surfaces). |
| `r.LPVGI.Occlusion.Thickness` | 1 | SDF blocker band half-width, in cells. |
| `r.LPVGI.OcclusionStrength` | 1 | Occlusion strength. |
| `r.LPVGI.Temporal` | 1 | Temporal accumulation (anti-flicker). |
| `r.LPVGI.TemporalBlend` | 0.1 | Current-frame weight (lower = smoother). |
| `r.LPVGI.RSM.Resolution` | 512 | Reflective Shadow Map resolution. |
| `r.LPVGI.InjectMode` | 1 | 1 = RSM (recommended), 0 = screen-space fallback. |
| `r.LPVGI.DebugView` | 0 | 1 = indirect only, 2 = raw light field. |

## How it works

Each frame, per cascade:

1. **Capture** an orthographic Reflective Shadow Map (lit scene colour + depth)
   from the sun's point of view (one shared capture covers all cascades).
2. **Inject** each RSM texel's flux as a spherical-harmonic cosine lobe into a
   32³ radiance volume (plus a second bounce from the previous frame's result).
3. **Build** an occlusion volume from the Global SDF.
4. **Propagate** the SH radiance between cells, attenuated by occlusion.
5. **Accumulate** temporally with grid-snapped reprojection.
6. **Composite** the indirect light into scene colour through the GI plugin hook.

## Known limitations

Light Propagation Volumes is a low-frequency technique. It does **not** provide
sharp contact shadows, fine ambient occlusion, or glossy reflections (combine
with Distance Field AO for contact detail). Enclosed volumes may still exhibit a
little light leaking. Quality scales with cascade resolution and cell size.

## Known issues

This plugin is still under active development and a few areas are not yet finished:

- **Performance tuning is ongoing.** The renderer works and is already reasonably
  cheap, but the optimization pass for low-end hardware isn't complete yet. Some
  passes (volumetric god-ray shadow tracing, the far cascade updates) can still be
  made faster without changing the output. Expect frame-cost improvements in later
  versions.
- **GI-coloured volumetric fog is experimental.** The optional volumetric pass
  (`r.LPVGI.Volumetric`) that tints fog with bounce light is new and still being
  polished. It's off by default; enable it if you want to try it.
- **A few rough edges remain.** Some parameters and edge cases (very large cell
  sizes, extreme intensities, tight enclosed rooms) still need polish and better
  defaults. Feedback and bug reports are welcome.

## License

MIT, see [LICENSE](LICENSE). Free for any use, including commercial.

## Credits

Built on the cascaded Light Propagation Volumes technique by Anton Kaplanyan and
Carsten Dachsbacher. Integrates through Unreal Engine's `FGlobalIlluminationPluginDelegates`
hook using only public engine APIs.
