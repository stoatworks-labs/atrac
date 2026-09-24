# Attributions

Atrac is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks clamp and standards

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored so a closed stdout is a failed write), the negative-control pattern, --offline, check-shaders.sh, the verify script and the host clock (with readout's clock-unit voting) are clamp's, by way of standards.

### Audio input, onset detection and priming — Stoatworks macroblock, vocoder and regauss

<https://github.com/stoatworks-labs/macroblock>  
Licence: MIT  
Copyright: Stoatworks Labs

The FFT buffer parameter, reading it through FindParamInfo, the square root of each bin and the spectral-flux onset detector against its running mean are macroblock's and vocoder's; priming the detector on the first frame is the fleet's lesson from regauss and needle.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of clamp.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### ATRAC and MiniDisc

Sony's Adaptive Transform Acoustic Coding as shipped on MiniDisc: the modified discrete cosine transform with block switching, block floating units with a scale factor and a word length each, a fixed budget of bits per sound unit shared out by a masking model, and the shock-proof buffer that a knock drains. Built from the published description of the coder; no Sony code, no table copied from a product, and no product name beyond the format's. ATRAC and MiniDisc are trademarks of Sony Group Corporation; this project is not affiliated with or endorsed by Sony.

## Standards and published specifications

What the implementation is measured against.

- **Princen and Bradley, "Analysis/Synthesis Filter Bank Design Based on Time Domain Aliasing Cancellation" (IEEE Trans. ASSP, 1986); Malvar, Signal Processing with Lapped Transforms (1992)** — The modulated lapped transform, its orthonormal scaling and the window condition w² + w²_mirror = 1 that the transform's reconstruction rests on.
- **The ATRAC1 scale-factor law, 2^((i − 15)/3) for i = 0..63, and the word-length law 0, 2..16 bits** — As documented in the open-source ATRAC decoders' descriptions of the format (FFmpeg's libavcodec, atracdenc). The formula is used; no table or code was copied.
- **ITU-R BT.601** — The luma and colour-difference weights the picture is coded in.
- **Nicholas J. Higham, Accuracy and Stability of Numerical Algorithms** — The standard gamma-n rounding-error bounds the harness's float tolerances are derived from.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the harness uses for its noise, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
