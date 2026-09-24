#pragma once

#include <vector>

/**
	The coder as arithmetic. No GL, no FFGL: the plugin builds its tables
	from this and the harness's offline `--tables` check holds it to the
	rules stated in AGENTS.md.

	**The transform.** A separable 2-D lapped transform: along each axis, a
	modulated lapped transform (the MDCT with a Princen-Bradley window),
	orthonormal, so the forward and the inverse use the same table:

	    X[k] = sqrt(2/M) sum_{n<2M} w[n] x[s - M/2 + n] cos( pi/M (n + 1/2 + M/2)(k + 1/2) )
	    y[n] = w[n] sqrt(2/M) sum_{k<M} X[k] cos( ... ),   overlap-added

	for a block of M coefficients starting at sample s. **The window is a
	sine ramp of length L centred on each block boundary**, zero outside it
	and one inside: with L = M on both sides that is the plain sine window
	with 50% overlap; with L smaller the block has a flat top. Perfect
	reconstruction (TDAC) needs only that the two blocks sharing a boundary
	agree on L and that the ramp satisfies w^2 + w_mirror^2 = 1, which a
	sine ramp does. That is what makes block switching exact with no
	transition windows: **the overlap at a boundary is the smaller of the
	two blocks meeting there**, and at the picture's edge it is zero.

	**Blocks.** The picture is cut into N x N cells (`Block Size`). A long
	cell is one block of N along each axis; a short cell is four blocks of
	N/4 along each axis (sixteen sub-blocks), with N/4 overlaps inside it.

	**BFUs.** Within a block the coefficient (kx, ky) belongs to diagonal
	d = kx + ky, and diagonals are grouped into block floating units by one
	master list of edges clipped to the block's 2M - 1 diagonals (the rule is
	in AGENTS.md). In a short cell a BFU is the union over the sixteen
	sub-blocks of the same band, as ATRAC1 does across its short blocks.

	**Scale factors** are ATRAC1's table, 2^((i - 15)/3) for i = 0..63 -- a
	2.007 dB step -- and a BFU's is the smallest entry not below its peak.
	**Word lengths** are ATRAC1's: 0 (the BFU is zeroed) or 2..16 bits.
*/
namespace atrac::codec
{

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
enum Perturb : int
{
	kPerturbNone           = 0,
	kPerturbRectWindow     = 1 << 0,///< the window is 1 over the whole 2M support (the spec's tdac negative)
	kPerturbNoQuantise     = 1 << 1,///< the quantiser is bypassed (the hook --tdac uses: unlimited bits)
	kPerturbOverBudget     = 1 << 2,///< the shader spends 5/4 of the declared budget
	kPerturbStopEarly      = 1 << 3,///< the greedy stops at the first increment that does not fit
	kPerturbShortIsLong    = 1 << 4,///< every cell is coded long, whatever Block Mode says
	kPerturbBitsIgnored    = 1 << 5,///< the budget is fixed, Bit Rate ignored
	kPerturbIgnoreMasking  = 1 << 6,///< the threshold is the absolute floor only (the spec's masking negative)
	kPerturbBufferIgnored  = 1 << 7,///< a knock mutes at once, whatever the buffer holds
	kPerturbNoRefill       = 1 << 8,///< a muted disc refills at 1x, not at Read Speed
	kPerturbNoPrime        = 1 << 9,///< the onset detector starts from silence, not from the first spectrum
	kPerturbResizeClears   = 1 << 10,///< a resize during a mute clears the held picture
};

/// The block sizes Block Size offers, by option index.
constexpr int kBlockSizeCount = 3;
int BlockSizeOf( int option );

/// Block Mode, by option index.
enum BlockMode
{
	kLong     = 0,
	kShort    = 1,
	kAdaptive = 2,
	kBlockModeCount
};

/// The most BFUs any block has (the master list has 13 bands; 16 is the
/// packing's ceiling and the shaders' array size).
constexpr int kMaxBfu = 16;

/// The master list of diagonal edges. Band j is diagonals [edge j, edge j+1)
/// once the list is clipped at 2M - 1 (the diagonal count of an M x M block).
const std::vector< int >& MasterEdges();

/// The BFU edges of an M x M block: the master list clipped, closing at 2M - 1.
std::vector< int > BfuEdges( int M );

/// How many BFUs an M x M block has.
int BfuCount( int M );

/// Coefficients in band j of an M x M block (one sub-block; a short cell has
/// sixteen of these per BFU).
int BfuCoefficients( int M, int j );

/// The band of diagonal d in an M x M block, or -1 past the last.
int BfuOfDiagonal( int M, int d );

/// ATRAC1's scale factors: 2^((i - 15)/3), i = 0..63.
constexpr int kScaleFactors = 64;
double ScaleFactor( int index );

/// The smallest scale-factor index whose value is not below `peak`
/// (63 if none is).
int ScaleFactorIndex( double peak );

/// Word length law: index 0 is 0 bits, index i >= 1 is i + 1 bits, to 16.
constexpr int kMaxWordLength = 16;

/// The window ramp of length L: w[j] = sin( pi (j + 1/2) / (2L) ), j < L.
std::vector< double > Ramp( int L );

/// The orthonormal basis of an M-block: entry [n][k] = sqrt(2/M) cos( pi/M
/// (n + 1/2 + M/2)(k + 1/2) ), n < 2M, k < M, as one row-major table.
std::vector< double > Basis( int M );

/// The masking model's constants, all in linear energy.
///
/// Spreading: a masker in band i raises the threshold of band j by its
/// energy times 10^( -slope |i - j| / 10 ), with kSpreadUpDb per band
/// above the masker and kSpreadDownDb per band below it (masking spreads
/// further up in frequency than down). The threshold is the largest such
/// contribution from any OTHER band, less the Masking offset, floored by
/// kAbsoluteFloor: half an 8-bit code, squared -- a band whose mean
/// coefficient energy is below what an 8-bit display can show gets nothing.
constexpr double kSpreadUpDb     = 10.0;
constexpr double kSpreadDownDb   = 24.0;
constexpr double kAbsoluteFloor  = ( 0.5 / 255.0 ) * ( 0.5 / 255.0 );
constexpr double kMaskingRangeDb = 60.0;///< the offset at Masking = 0

/// The Masking control's offset in dB below the spread: 60 (1 - m).
double MaskingOffsetDb( double masking );

/// The transient detector's ratio between a cell's two halves past which
/// Adaptive picks short blocks: 8 (9 dB) either way.
constexpr double kTransientRatio = 8.0;
/// The energy floor added to both halves: a 1/255 step per pixel.
constexpr double kTransientFloorPerPixel = ( 1.0 / 255.0 ) * ( 1.0 / 255.0 );

} // namespace atrac::codec
