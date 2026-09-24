# AGENTS.md — Atrac

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

MiniDisc's perceptual coder, run on the picture, as an FFGL 2.1 effect (`AC01`,
shown as `SW Atrac`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/atrac`.

Built 2026-09-24 in one session from the fleet's templates and `specs/SPEC-atrac.md`
(with `BRIEF.md` and `BRIEF-ADDENDUM.md`): clamp for the harness, the verify script,
the `--pipe` contract, the negative-control pattern, `--offline`, the clock and the
GL-less CI; macroblock and vocoder for the FFT buffer parameter and the onset
detector; regauss and needle for the priming trap; tinsel for `PassBuffer` and the
trap list; graticule for the notes.

---

## The one idea

**ATRAC throws detail away by bits, not by resolution.**

The picture is cut into N × N cells. Along each axis a cell is one lapped block (or
four short ones); the block goes through the MDCT; its coefficients are grouped into
block floating units (BFUs) with a scale factor and a word length each; a fixed
budget of bits per cell is handed out by a greedy on noise-to-mask ratio; and the
coefficients are quantised, dequantised and transformed back. On top, a shock-proof
buffer that a knock drains: when it empties, the picture holds.

| what the coder does | what comes out |
| --- | --- |
| a fixed budget per cell, spent by priority | **fine texture goes first**; flat cells have bits to spare, busy ones starve |
| quantisation noise is a sum of basis functions over the whole window | **pre-echo**: an edge rings one block either side; short blocks confine it |
| the allocation is recomputed every frame | **the swirl**: bands' bits come and go, texture shimmers at low rates |
| a buffer filled at r×, drained at 1×, a knock stopping the fill | **the skip**: a knock longer than the buffer holds the picture until the refill |

### The arithmetic, in one place

    transform  X[k] = sqrt(2/M) sum_{n<2M} w[n] x[s - M/2 + n] cos( pi/M (n + 1/2 + M/2)(k + 1/2) )
               y[n] = w[n] sqrt(2/M) sum_k X[k] cos( ... ), overlap-added: the orthonormal MLT
    window     a sine ramp of length L centred on each boundary, 0 outside, 1 inside;
               L = min( M_left, M_right ), and 0 at the picture's edge
    cell       N x N; long = one block of N per axis; short = four of N/4 (16 sub-blocks)
    BFU        diagonal d = kx + ky; bands = {0,1,2,3,4,6,8,11,15,20,27,36,48,63} clipped at 2M-1;
               in a short cell a BFU is the union of the same band over the 16 sub-blocks
    scale      sf[i] = 2^((i-15)/3), i = 0..63 (ATRAC1: 2.007 dB steps); the smallest >= the peak
    word       0 or 2..16 bits (ATRAC1)
    masking    E_j = mean coefficient energy of band j
               T_j = max( floor, max_{i != j} E_i 10^(-slope |i-j| / 10) x 10^(-offset/10) )
               slope 10 dB per band upward (i < j), 24 dB per band downward; floor = (0.5/255)^2;
               offset = 60 (1 - Masking) dB
    greedy     queue key E_j / T_j; a step of 2 bits divides it by 16, every later bit by 4;
               the highest key whose increment (bits x coefficients) fits takes it; ties to the
               lowest band; a band with E_j <= floor is out of the queue; until nothing fits
    budget     floor( bpp N^2 ) for Y, floor( bpp N^2 x Chroma Bits ) for each of Cb and Cr
    quantise   q = round( c / sf x L ), L = 2^(wl-1) - 1, clamped; c' = q sf / L; wl = 0 zeroes
    disc       level' = level + dt ( r [reading] - 1 [playing] ), capped at B; a knock stops the
               read for L; level = 0 while playing = the break; broken: fills at r, plays again
               at level >= B/4. Breaks iff L > level at the knock; resumes at t0 + L + (B/4)/r.
    transient  Adaptive: short if the energy of first differences in one half of the cell is
               8x the other's (+ a floor of (1/255)^2 per difference), along x or along y

### What does not fall out, and is the honest limit

- **An MLT has no compact DC.** A flat field projects onto every band of a block
  (the k = 0 basis function is a windowed half-cosine, not a constant), the zig-zag
  BFU puts the large axis coefficients (k, 0) and (0, k) in with the small interior
  ones, and the scale factor is the large one's. So flat areas ripple faintly until
  the rate is high. That is the transform the spec asked for, and it is why the
  swirl shows even where nothing moves.
- **The masking model is stated, not tuned.** Slopes of 10 and 24 dB per band, an
  offset of 60(1 − m) dB and an absolute floor of half an 8-bit code are defensible
  numbers from the psychoacoustic shape; nothing here has been listened to or looked
  at against anything.
- **Side information is not in the budget.** MiniDisc's 212 bytes include the word
  lengths and scale factors; here the budget is coefficient bits only.
- **The transient detector looks inside the cell only**, so its decision is local
  and a cell's mode never depends on its neighbours; ATRAC1's compares with the
  previous block.
- **One spectrum per frame is the input's resolution** for the knock: a hit lands on
  the frame after it.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Codec.{h,cpp}` | The tables and laws: block sizes, the BFU rule, ATRAC1's scale factors, the ramps and basis in double, the masking constants, the `Perturb` bits. No GL. |
| `source/Controls.{h,cpp}` | Every 0..1 slider to its unit. |
| `source/Disc.{h,cpp}` | The buffer and the onset detector, in double. No GL. |
| `source/Clock.{h,cpp}` | clamp's clock: unit voting, origin + offset in double. |
| `source/Shaders.{h,cpp}` | Ten shaders assembled from snippets: vertex, convert, cells, mdctx, mdcty, alloc, quant, imdcty, imdctx, display. |
| `source/Atrac.{h,cpp}` | The plugin: parameters, the tables' upload, the passes, the disc step, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/actest/` | The offline harness: `main.cpp` (plumbing, cards, --pipe, --bench) and `checks.inc` (every check and the statements it compares against). |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Per host frame:

1. **clock, audio, disc** (CPU) — the spectrum is read from the FFT buffer, the
   detector primed or stepped, the buffer stepped; muted or not.
2. **convert** — W × H: Y − ½, Cb, Cr, alpha.
3. **cells** — Bx × By: long or short per cell.
4. If not muted (or nothing is held yet): **mdctx** (Bx N × H), **mdcty** (Bx N ×
   By N), **alloc** (2 Bx × 3 By, packed), **quant**, **imdcty** (into mdctx's
   buffer, spent by then), **imdctx** into `held` (W × H, RGB).
5. **display** — the host's framebuffer: mix, Show Bits, Show Blocks.

Five RGBA32F buffers the size of the picture or its padded plane: about 165 MB at
1080p and 660 MB at 4K.

---

## Traps

Roughly in the order they will bite.

### ☠️ The first pre-echo measurement read the whole picture as ringing

The first `--preecho` painted a dark/bright step and measured how far from the edge
the error's gradient exceeded one 8-bit code. It found 160 pixels — the whole
picture — in every mode, because a flat field is not one coefficient of an MLT (see
the honest limit): at a low rate every flat cell ripples. The check now paints mid
grey up to the edge, which is Y = 0 exactly and so codes to exactly zero in every
block that sees only grey, and measures how far BEFORE the edge the error reaches.
That is what pre-echo means. The right of the edge is not measured; a bright flat
field's own ripple would drown it. Long reaches exactly N (the block's support
starts N/2 before its cell, and the cell's centre is N/2 further), Short 3N/8.

### ☠️ Masked bands that can never be coded cap the SNR

The first allocation made a band below its masked threshold ineligible: zeroed,
whatever the budget. `--snr` then rose 1.5 dB from 0.1 to 0.8 bits per pixel and the
leftover budget at high rates had nowhere to go (`--budget` said "wasted 0.00 of an
increment" because every eligible band was at 16 bits). ATRAC's allocation is a
priority queue on noise-to-mask ratio and spends every bit: a masked band starts the
queue with E/T below one, comes last, and on a short budget is never reached — but
on a long one it is. Only a band below the absolute floor is out of the queue.
`--masking` therefore runs on 48 bits, a budget chosen from the statement so the
weak band is reached with Masking off (the strong band stops at 5 bits, 20; the weak
band's 22 fit) and not with it on (the strong band runs to 10 bits, 40; the 22 do
not fit).

### ☠️ White noise makes any SNR curve flat, and that is the card's fault

With the queue fixed the curve was unchanged, because the card was per-pixel white
noise: incompressible, so below a bit per coefficient nothing the budget buys can
touch the noise floor (~16 dB) and above it the SNR climbs 6 dB per extra bit per
coefficient. Exactly what the numbers showed. `--snr` runs on a card with structure
at every scale and no white noise (gradients, gratings, a blob, an edge, 6-pixel
grain). The claim is strict monotonicity; the step sizes are reported, not asserted.

### A pending Knock must survive InitGL

`--set "Knock=1"` is applied before `InitGL`, and the first `InitGL` cleared the
pending press, so the sweep read Buffer, Read Speed, Knock Length and Knock as dead.
A press is a press. A fresh instance cannot inherit one (the constructor starts it
clear), so nothing is lost by keeping it.

### `mix( src, held, 1.0 )` is not the held texel to the bit

The disc checks detect a held frame as one that did not change while the input did.
Byte-equality failed: with Mix at 1 the display's `mix` can return the held value
off by an ulp depending on the source, which moves. Held is "moved by less than
1e-5", playing is "the bar moved it by a lot"; the two are three orders of magnitude
apart.

### An alloc value is an integer in a float, and it has to stay below 2^24

Two 11-bit fields per channel (word length + 32 × scale-factor index, then the next
band × 2048): at most 4,194,303, exact in a float. Sixteen BFUs would not fit in
one texel, so a cell and channel take two, and the alloc pass runs twice per cell
(once per half) rather than through a multi-target framebuffer. 2× a small
computation.

### MSVC has no `M_PI`

The first Windows CI run failed on `M_PI` in `Codec.cpp`: MSVC's `<cmath>` defines it
only under `_USE_MATH_DEFINES`, and clang on the Mac never notices. The constant is
spelled out once as `kPi`. Nothing on this Mac would have found it; only CI did.

### What filming it found (2026-09-24)

The project video was rendered through `actest --pipe` over Resolume's demo clips
before the guide's numbers were fixed, and the survey settled three things the harness
cannot: **a starved cell falls to mid grey, not black** (Y is centred on zero and the
MLT has no compact DC, so a cell whose bands are all zeroed decodes to Y = 0 -- at
0.1 bpp a dark clip with a bright object becomes grey blocks); **Show Bits was nearly
black at the default rate** (most bands at 2 bits; the heat map's low end was lifted);
and **pre-echo on thin bright lines needs about a bit per pixel** before the halo shows
-- at a quarter of a bit the line itself starves to a dim trace and the halo is lost in
it. The defaults survived the survey unchanged.

### "Knock Sensitivity" is seventeen characters

`--names` failed on it. The name field a host reads is not null-terminated and
truncates silently past 16; the spec's name is `Sensitivity` in the Disc group.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the display, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` and table upload happens
before anything binds a texture; `FFGLFBO::Release()` leaks the colour texture, which
is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo` clamps a STANDARD
default into 0..1 and `SetParamInfof` reads its default out of `params[]` (so
`params[]` is filled first); an option's range reads back 0..1 whatever its element
count; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `FFGLShader::Set` has no array overload (`glUniform1fv`);
Resolume's clock overflows a float, so the clock is an origin and an offset in double
and nothing absolute reaches a shader; the onset detector is primed on the first
frame and a zero-length frame advances nothing; the buffer that outlives a frame
(`held`) is not re-ensured during a mute, so a resize cannot clear it; `nm | grep -q`
fails under pipefail when grep succeeds; a closed stdout must be a failed write, so
`--pipe` ignores SIGPIPE.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every rendered check ran
at 320×180 and 1280×720 in `verify.sh`, and at 333×187 by hand.

What makes them rasteriser-proof by construction: **every coordinate is an integer
computed in integers** (`gl_FragCoord`, never an interpolated uv, except the display's
read of the held picture, which no check measures to the bit); **every read is
`texelFetch`**; **the basis, the ramps, the scale factors and the spreading factors
are computed on the CPU in double** and handed over as a texture and uniform tables,
so no driver `cos`, `log` or `pow` is in any checked path; **the allocation is integer
arithmetic on floats that hold exact integers**; and **the disc is double on the
CPU**. The GPU multiplies, adds and compares in float32.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--tdac` | the l2 norm of output − input at unlimited bits, N = 8/16/32 × Long/Short/Adaptive | every stage is orthogonal, so the l2 norm of an error made at any stage arrives unchanged; per stage the rounding of a 2M-term dot product plus the float rounding of basis, ramp and product, κ = (2M+4)u·1.01, bounded per output by Cauchy–Schwarz and summed with the overlap counted twice: √2·κ·‖x‖₂; four stages; ‖x‖₂ ≤ ½√(3P) over the padded plane; the colour matrices add γ₃ terms and the inverse matrix's Frobenius norm (2.96) scales what it is handed; `mix` at 1 adds u | the bound grows with √P: 4.7e-3 (N=8) to 1.5e-2 (N=32) at 320×180, 1.9e-2 to 6.0e-2 at 1280×720, about 0.2 at 4K — loose there, stated; the measured error grows the same way, 3.4e-5 to 4.2e-5 at 320×180 and up to 1.7e-4 at 1280×720, 0.3% of the bound at both, so the check is not close |
| `--budget` | Σ wl_j × count_j against the budget, the leftover against the cheapest increment, the scale-factor index, bands below the floor | all integers, no tolerance; a band counts as "below the floor" only when a tenth of a dB clear of it, and a scale factor is only compared when the peak is more than 1e-5 relative from a table entry (the GPU's peak is a float, the harness's a double) | none: everything is per cell |
| `--preecho` | the farthest pixel before the edge whose error exceeds one 8-bit code | whole pixels; the bounds N and 3N/8 are the windows' supports exactly; the reference (grey = Y 0) is exactly zero by construction; 1/255 is the smallest step a display shows | none: the edge cell is Bx/2 and every extent is cell-relative |
| `--snr` | 10 log(Σx² / Σe²) at six rates on the rate card | strictly rising, no margin: the same allocation gives the same picture to the bit, so equality is the negative control's signature, not noise | values differ per raster (16.0 to 34.7 dB at 333×187; 20.1 to 37.9 dB with a smallest step of 0.73 dB at 1280×720), strictly rising at all three |
| `--masking` | the planted coefficients and the two bands' word lengths | the planted coefficient within 1e-4 (a double-synthesised basis function through the float transform; measured 1e-6); the allocation is integers | needs an interior 16-pixel cell: Bx, By ≥ 3 |
| `--skip` | the first and last held frame against the closed form | one frame each side: time arrives a frame at a time, so every event lands on the frame after it; held = moved by < 1e-5, playing = the bar moved it | none |
| `--resize` | muted at the resize; the held picture's mean before and after; the resume frame | mean within range × (1/W + 1/H): the half texel of picture each edge gains or loses under bilinear resampling; one frame for the resume | none |
| `--skipmodel` | the Buffer class a frame at a time against the closed form, nine cases | one frame | none (no GL) |
| `--prime` | booleans through the plugin's own frame path | none | none (no GL) |
| `--tables` | the plugin's tables against the statements; the stated transform's own reconstruction in double | 1e-12 (two arrangements of the same double formulas) | none (no GL) |

Deliberately NOT relied on: `mix(a, b, 1) == b`; exact cancellation anywhere; any
transcendental on the GPU in a checked path (`floor`, `abs`, `max` and integer
division of non-negative operands only); a texture unit's filtering (the held
picture's bilinear read is measured only through its mean, with that bound).

What might still differ on another rasteriser: a driver that does not round float +
and × correctly would break the tdac bound; GLSL 4.10 requires it. The software
context CI would fall back to is a different compiler for the same GLSL;
`check-shaders.sh` covers the syntax there and the rendered checks run with
`--allow-no-gl` so a runner without a context skips loudly.

### The negative controls

`actest --negative` runs nine against rendered checks and `--offline` three against
the model; `--perturb BITS` runs any check verbosely against one. Each perturbs the
*plugin's* model — a `Perturb` bit the shipped plugin carries at zero — never the
harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| a rectangular window over the whole support (the spec's) | `--tdac`: l2 error 4,000 to 5,000 times the bound in every combination |
| the shader spends 5/4 of the budget | `--budget`: cells over budget |
| the greedy stops at the first increment that does not fit | `--budget`: cells with a whole increment left |
| short blocks are really long | `--preecho`: Short and Adaptive reach N, past 3N/8 |
| Bit Rate ignored | `--snr`: 23.86 dB at every rate, not strictly rising |
| the threshold is the floor only (the spec's) | `--masking`: the weak band gets bits with Masking on |
| a knock empties the buffer at once | `--skip`, `--skipmodel`: L = 0.25 s on B = 0.5 s holds frames |
| a muted disc refills at 1× | `--skip`, `--skipmodel`: the resume lands 4 to 15 frames late |
| a resize clears the held picture | `--resize`: the mean after the resize is not the mean before |
| the detector starts from silence | `--prime`: a knock on frame one |

### The mutation

One character of the shipped GLSL, on a clean committed tree (`df9ce4b`): in the
lapped snippet's `windowAt`, `int p = n - M / 2;` → `int p = n - M / 3;` — the window
no longer centred on the block boundaries, so the ramps' squares no longer sum to
one where the blocks overlap. Caught at 320×180 by `--tdac` in all nine combinations
(l2 error 4,159 to 4,987 times the bound; 17,000 to 20,000 at 1280×720) and by
`--masking` (the planted 0.3 read back as 0.2322, the planted 0.0189 as −0.0207).
`--budget`, `--preecho`, `--snr`, `--skip` and `--resize` passed, correctly: the
accounting holds whatever the transform, the extents are the supports' whatever the
window, SNR still rises with bits, and the disc never touches the transform.
Reverted with `git checkout source/Shaders.cpp`; the tree was clean before and after.

---

## Decisions taken without asking

- **The transform is the orthonormal MLT** (scale √(2/M) both ways), the window a
  sine ramp per boundary with the overlap the smaller of the two blocks and zero at
  the picture's edge. This is the Edler/CELT low-overlap arrangement; it makes
  Adaptive exact with no start/stop windows and no transition bookkeeping.
- **Cells are N × N with N ∈ {8, 16, 32}; short = 4 × 4 sub-blocks of N/4**, both axes
  at once. The last cells are padded by clamping to the edge.
- **BFUs by the master list {0,1,2,3,4,6,8,11,15,20,27,36,48,63}** clipped at 2M − 1:
  the first four bands one diagonal each (DC on its own, as ATRAC1 keeps its low BFUs
  narrow), then widths of about 4/3 each. 8, 11 and 13 bands for M = 8, 16, 32; 3 and 6
  for the short blocks 2 and 4. A short cell's BFU is the union over its sixteen
  sub-blocks, as ATRAC1's short-mode BFUs span its short blocks.
- **Scale factors are ATRAC1's** 2^((i−15)/3), 64 of them; the smallest not below the
  peak. Word lengths 0, 2..16.
- **Masking**: spread 10 dB per band up, 24 dB per band down; the threshold is the
  largest neighbour's contribution less 60(1 − m) dB; floored by half an 8-bit code
  squared. A band below the floor is never coded; a masked band is last in the queue.
- **Bit Rate is bits per pixel, 0.05 × 80^v** (0.05 to 4, 0.5 at v = 0.525); the budget
  is floor(bpp N²) for Y and floor(bpp N² × Chroma Bits) for each of Cb and Cr.
  Default 0.5 bpp, N = 16, Adaptive, Masking 0.6, Chroma Bits 0.25.
- **Colour**: BT.601 Y, Cb, Cr, with Y centred at zero so a mid grey has no
  coefficients; the alpha passes through.
- **The disc**: Buffer 0–10 s (default 2), Read Speed 1–4× (default 2), Knock Length
  50 ms–5 s logarithmic (default 0.5 s), the restart level a quarter of the buffer,
  the buffer full at start. A knock during a knock extends it. A muted disc decodes
  nothing; the first frame decodes whatever the disc says so a knock on frame one
  holds a picture rather than black.
- **The onset detector** sums positive flux over all 64 bins (so the bins' frequency
  layout is irrelevant), takes the square root of each bin as the fleet does, compares
  against a running mean (τ = 1 s) times a margin of 2.5 − 2.35 × Sensitivity with an
  absolute floor of 0.004 and an 80 ms refractory period; Sensitivity 0 never knocks.
  No `Bin Law` control: the spec did not list one and the adaptive bar makes the law a
  matter of shape, not of deafness.
- **The transient detector** uses first differences inside the cell only, ratio 8,
  floor (1/255)² per difference.
- **Show Bits paints the luma allocation** per cell as its own coefficient plane.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove `--pipe`
  exits 1 on a failed render.
- **About and attributions are generated** (`StoatworksAbout.h` by sync-about.py,
  `ATTRIBUTIONS.md` by sync-attributions.py) since the project was registered on
  2026-09-24. The guide URL added the User guide button, so the parameter count went
  from 18 to 19 before the first tag; verify.sh was re-run on it.
- **Show Bits' heat map starts at a readable blue** (0.10, 0.16, 0.62) rather than the
  near-black it had, because the video survey over Resolume's demo clips found that at
  the default rate almost every band is at 2 bits and the view was black. Display only;
  nothing measured reads it.
- **The FFGL submodule was dissociated from the reference clone** (`repack -a -d`, the
  alternates file removed) so this repo does not depend on a path in `~/Projects`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 333×187 by hand.

- **TDAC.** Nine combinations, l2 error 3.4e-5 to 4.2e-5 against bounds of 4.7e-3 to
  1.5e-2 at 320×180 (under 1% of the bound), worst pixel 6.6e-7; Adaptive with both
  kinds of cell present.
- **Budget.** 3,660 cell-channels over three block sizes and three rates: none over,
  none with an increment left (worst 0.75 of one), none below the floor coded, every
  scale factor as stated (30,778 checked), every cell identical to the stated greedy.
- **Pre-echo.** Long reaches exactly N before the edge; Short and Adaptive 0, 2, 12
  against 3, 6, 12; Adaptive short at the edge, long beside it.
- **SNR.** 19.4 → 35.6 dB over five doublings, smallest step 1.9 dB.
- **Masking.** 12/0 bits on, 6/2 off, on 48 bits: the statement's own numbers.
- **Skip.** Held frames 42–74 (closed form 43, 74.75) and 42–72 (43, 72.88); nothing
  held when L < B.
- **Resize.** Muted through a 3/2 resize, the held mean unchanged to four decimals,
  the resume on the closed form.
- **Offline.** Tables exact; the stated transform reconstructs to 8.5e-15; the disc
  within a frame in nine cases; primed from origins of 0 and 40 s.
- **Negative controls.** All twelve fail their check.
- **Mutation.** Caught (above).
- **No dead controls**, all 13, with the four About buttons and the Audio buffer
  skipped.
- **Every shader compiles** through `glslc`, as the plugin hands it to the driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout after one byte.
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.atrac`,
  ad-hoc signs, and `oxbow` reports `SW Atrac` / `AC01` / `effect` and renders 120
  frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, on a GPU shared with seven other builds:

  | | N = 16 (default) | % of a 60 fps frame | N = 8 | N = 32 |
  | --- | --- | --- | --- | --- |
  | 1280×720 | 3.2 ms | 19% | 1.4 ms | 7.2 ms |
  | 1920×1080 | 4.9 ms | 30% | 3.0 ms | 15.1 ms |
  | 3840×2160 | 16.1 ms | 97% | 10.0 ms | 30.4 ms |

  Each pixel costs 2M taps in each of four lapped passes, each tap two texel fetches.
  At 4K on the default blocks that is the whole frame. Nothing has been optimised:
  the basis could be a packed uniform array rather than a texture, the alloc pass
  could write both halves through two render targets, and the picture buffers could
  be half-float if the tdac bound were restated for it.

### Assumed, or not done

- ☠️ **Never loaded into Resolume.** Everything was compiled, rendered and measured
  offline against the real plugin class in a headless CGL context, plus an `oxbow`
  load.
- **Never seen on footage.** Every picture so far is synthetic.
- **Resolume's FFT bins are unmeasured** — magnitude or power, window, scale. The
  detector does not depend on their frequency layout; it does take the square root
  of each, as the fleet does.
- **The masking model is not tuned** against anything seen or heard.
- **The clock-unit voting** is clamp's by way of standards, which has met Arena; this
  plugin has not.
- **Windows compiles in CI only**; nothing has run it in a host.
- **Not verified at 4K**, only benchmarked there; the tdac bound is loose there (about 0.2 in l2).
- **No OpenFX port, no factory presets.** There is a user guide (`docs/USER-GUIDE.md`,
  the site page and PDF are built from it) and a browser demo (`demo/`, see below).
- **Nothing has been through a show.**

---

## Open questions

- **Should the coder have a DC-compact stage?** A flat field's ripple is the MLT's
  and is faithful, but a picture has DC where audio does not. Subtracting each cell's
  mean before the transform (and coding it as its own BFU) would clean flat areas at
  the cost of no longer being the MDCT alone.
- **Should side information be in the budget?** Word lengths and scale factors cost
  bits on MiniDisc; here they are free.
- **Should a short cell's BFUs be per sub-block** rather than the union across the
  sixteen? The union is ATRAC1's; per sub-block would let the edge sub-blocks starve
  the flat ones less.
- **A `Bin Law` option** (magnitude / power) would turn an inherited assumption into
  a switch, as needle did.
- **Performance at 4K** — see the render cost above.
- **Should the mute also freeze the input side of the mix?** At Mix below 1 the
  held decode blends with a moving source; arguably right (the display is live, the
  disc is not), arguably odd.

---

## The browser demo

`demo/` is the page at atrac-demo.stoatworks-labs.com, built on the fleet's demo kit
(`demo/vendor/`, vendored by stoatworks-backend's `resolume-demo/sync.sh`; never edit
it). Two halves, not equally faithful:

- **The shaders are the plugin's.** `demo/plugin.js` carries all twenty GLSL snippets of
  `Shaders.cpp` verbatim (one backtick in a comment escaped) and joins them into the ten
  programs exactly as `Shaders.cpp` does. `demo/tools/check_shaders.py` compares every
  snippet character for character AND every join, and `tools/verify.sh` runs it.
- **The CPU half is a port.** `Codec.cpp`'s tables, `Disc.cpp`'s buffer and detector,
  `Clock.cpp` (unit declared as seconds) and `ProcessOpenGL`'s frame sequence are
  rewritten in JavaScript. Nothing checks a port but a reader.
- **No audio reaches the page.** The Audio buffer is absent; Sensitivity is present and
  does nothing (the detector is handed silence); Knock is a toggle the page releases
  after one frame, one rising edge. The page says all of this in its banner and its
  disclosure.
- The Worker is a **route** (`wrangler.toml`) behind a proxied `AAAA 100::` record made
  through the API on 2026-09-24, because stoatworks-labs.com is at Cloudflare's limit of
  100 Workers custom domains. Delete the record and the page goes dark on a green deploy.
  `deploy.yml` redeploys on every push to main; the live `<head>` check must say
  "serving this build".

## Siblings

- **clamp** — the harness, verify, CI and `--pipe` shapes, the negative controls,
  `--offline`, the clock.
- **macroblock**, **vocoder** — the FFT buffer parameter and the onset detector.
- **regauss**, **needle** — the priming trap, and needle's `Bin Law` and Parseval
  argument for summing over every bin.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **photofinish** — the resize-must-not-clear trap the held picture honours.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
