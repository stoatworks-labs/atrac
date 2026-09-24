# atrac

MiniDisc's perceptual coder, run on the picture, as an FFGL **effect** for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`.
MIT.

Read `AGENTS.md` before changing the transform (`Shaders.cpp`'s lapped snippet), the
BFU rule or the laws (`Codec.*`), the allocation shader, or the disc (`Disc.*`).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/actest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Bit Rate=0.3" --set "Block Size=2" --set "Block Mode=0"`
  (0..1 for sliders, the element index for options; `--set "Knock=1"` presses the
  button on frame 0)
- A synthetic spectrum in the Audio buffer, a kick every half second: `--feed`
- List parameters, kinds, defaults and ranges: `./build/actest --list`
- The exact GLSL the plugin compiles: `./build/actest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps, default 60; the disc
  runs in real time, so this matters), `--feed` for a spectrum, and an optional
  `--script` of `frame Parameter Name value` cues, linearly interpolated between a
  name's cues and held before the first and after the last — an option index
  interpolated passes through the options between, so key a cut two cues a frame
  apart; `Knock` fires on the rising edge through 0.5. A cue naming no parameter exits
  2 before any frame; a partial frame at the end of stdin ends the stream with exit 0;
  a failed render or a closed stdout exits 1 (SIGPIPE is ignored so a closed stdout is
  a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/actest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~4 min)
- Unlimited bits reconstruct the input within a derived l2 bound, through long, short
  and mixed blocks: `./build/actest --tdac`
- Every cell spends at most its budget and wastes less than one increment, from the
  shader's own allocation: `./build/actest --budget`
- A step's noise reaches N before it in Long, 3N/8 in Short and Adaptive:
  `./build/actest --preecho`
- SNR on the rate card rises with Bit Rate: `./build/actest --snr`
- A masked band is zeroed, coded with Masking off: `./build/actest --masking`
- A knock breaks playback iff L > B, resuming on the closed form, read out of the
  picture: `./build/actest --skip`
- A resize mid-mute keeps the held picture and the disc's state: `./build/actest --resize`
- The checks can fail: `./build/actest --negative`; one perturbation verbosely:
  `./build/actest --tdac --perturb 1` (bits in `Codec.h`)
- Any check with one control moved: `./build/actest --snr --set "Masking=0"`
- No GL (what CI runs): `./build/actest --offline` = `--tables --names --skipmodel
  --prime` and their negative controls
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/actest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/actest --bench` (best of three; the GPU is shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Atrac.bundle`

## Notes
- **The GPU does everything per coefficient, the CPU everything per frame.**
  `Shaders.cpp` holds the ten passes; `Codec.cpp` builds the tables in double (the
  basis, the ramps, the BFU rule, the scale factors); `Disc.cpp` runs the buffer and
  the onset detector in double. A wrong picture is a GLSL fix; a wrong skip is C++.
- **The transform is one snippet, `kLappedSource`, shared by the four transform
  passes.** A block is (start, size, left overlap, right overlap); the overlap at a
  boundary is the smaller of the two blocks meeting there and zero at the picture's
  edge. That is what makes Adaptive exact with no transition windows.
- **Every buffer has GL's orientation** (row 0 at the bottom); the padded cells are
  on the right and at the top. The harness flips only for PNG files.
- **The allocation texture is packed**: two 11-bit (word length + 32 × scale-factor
  index) fields per channel, eight BFUs per texel, two texels per cell and channel.
  The packing is stated once in `kAllocReadSource` and decoded by quant, display and
  the harness.
- **A muted disc decodes nothing.** The mute costs no passes; the display samples the
  held picture with normalised coordinates, so a resize during a mute shows it scaled
  and never clears it.
- **The harness never re-types the tables it checks.** The BFU rule, the laws, the
  transform and the disc's closed form are stated in `actest` from AGENTS.md;
  `--tables` holds `Codec.cpp` to them, and every other check uses the statements.
- **`Perturb` bits are test hooks**, always 0 in the plugin. `kPerturbNoQuantise` is
  the "unlimited bits" `--tdac` runs at.
- **Parameter names must be unique and within 16 characters** — `--set` and the sweep
  find them by name, and `--names` fails a long one. "Knock Sensitivity" is
  `Sensitivity` for that reason.
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1).
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `atrac_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- `FFGLShader::Set` has no array overload: the ramp, BFU, count, scale-factor and
  spreading tables go in with `glUniform1fv` / `glUniform1iv` under the pass's own
  shader binding.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `AC01`, display name `SW Atrac`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. Never seen on footage, only on synthetic cards.
- No OpenFX port, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by stoatworks-backend's
  syncs (the project is registered); do not edit them here.
- The render cost at 4K is high (see AGENTS.md); nothing has been optimised.

## The browser demo
- `demo/` is the page at atrac-demo.stoatworks-labs.com: the twenty GLSL snippets
  verbatim in `demo/plugin.js`, joined as `Shaders.cpp` joins them, over a JS port of
  `Codec.cpp`, `Disc.cpp`, `Clock.cpp` and `ProcessOpenGL`'s sequence. No audio.
- `python3 demo/tools/check_shaders.py` holds every snippet and join to the C++;
  `tools/verify.sh` runs it. Change a shader: copy it across, never edit the JS copy.
- `demo/vendor/` is the shared kit, vendored by stoatworks-backend's
  `resolume-demo/sync.sh`; never edit it.
- Deploys on push to main (`.github/workflows/deploy.yml`), or by hand with
  `cf-run npx wrangler deploy`. The host is a Worker ROUTE behind a proxied AAAA
  record, not a custom domain (the zone is at Cloudflare's 100-domain limit).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/atrac/atrac.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\atrac\logs\atrac.YYYY-MM-DD.log   (Windows)
