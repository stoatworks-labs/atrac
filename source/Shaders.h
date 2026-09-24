#pragma once

#include <string>

/**
	The passes. Every read is `texelFetch` at integer coordinates computed
	in integers, the transform's basis and window ramps are computed on the
	CPU in double (`Codec.cpp`) and handed over as a texture and uniform
	tables, and the allocation is integer arithmetic on floats that hold
	exact integers -- so nothing here depends on a rasteriser's interpolated
	uv, on a texture unit's filtering, or on a driver's `cos`, `log` or
	`pow`. The GPU multiplies, adds and compares.

	Every buffer this plugin owns has GL's orientation: row 0 is the bottom,
	and the padded cells (where the picture is not a multiple of Block Size)
	are on the right and at the top. Nothing is flipped anywhere.

	  convert   host picture -> Y - 1/2, Cb, Cr, alpha                 (W x H)
	  cells     Y -> long or short per N x N cell (the transient detector,
	            or the Block Mode's answer)                            (Bx x By)
	  mdctx     Y Cb Cr -> coefficients along x, per row                (Bx N x H)
	  mdcty     those -> coefficients along y too: the 2-D block         (Bx N x By N)
	  alloc     coefficients -> per cell, channel and BFU: word length
	            and scale factor, packed (see the snippet)              (2 Bx x 3 By)
	  quant     coefficients + alloc -> quantised and dequantised        (Bx N x By N)
	  imdcty    those -> samples along y, coefficients along x           (Bx N x H)
	  imdctx    those -> the decoded picture, back in RGB, into `held`   (W x H)
	  display   host picture + held + cells + alloc -> the output

	The forward and inverse passes share one snippet (the block geometry and
	the window); quant and display share the allocation's packing. The
	shaders are assembled from those strings at start-up, which is why they
	are functions rather than literals -- and why `actest --dump-shaders`
	exists: what a GLSL compiler sees is the assembled text, not any one
	literal.
*/
namespace atrac::shaders
{

const std::string& Vertex();
const std::string& Convert();
const std::string& Cells();
const std::string& MdctX();
const std::string& MdctY();
const std::string& Alloc();
const std::string& Quant();
const std::string& ImdctY();
const std::string& ImdctX();
const std::string& Display();

/// The number of shaders above, for tools/check-shaders.sh.
constexpr int kShaderCount = 10;

} // namespace atrac::shaders
