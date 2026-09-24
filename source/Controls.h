#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Every conversion to a physical unit lives here and nowhere
	else; actest states the same laws from their definitions and --tables
	holds the two together.
*/
namespace atrac::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Bit Rate: bits per pixel, 0.05 x 80^v -- 0.05 to 4, logarithmic, 0.5 at
/// v = 0.525. The budget of a cell is this times its pixel count.
double BitsPerPixel( float value );

/// Chroma Bits: Cb and Cr each get this fraction of the luma budget, 0..1.
double ChromaFraction( float value );

/// Masking: the control's 0..1, handed to codec::MaskingOffsetDb.
double MaskingAmount( float value );

/// Buffer: the shock-proof buffer in seconds, 10 v -- 0 to 10.
double BufferSeconds( float value );

/// Read Speed: the disc is read at this multiple of the play rate, 1 + 3 v.
double ReadSpeed( float value );

/// Knock Length: how long a knock stops the read, 0.05 x 100^v s -- 50 ms
/// to 5 s, logarithmic, 0.5 s at the middle.
double KnockSeconds( float value );

/// Knock Sensitivity: 0 means the audio never knocks; above it, the onset
/// detector's flux margin, 2.5 - 2.35 v (higher sensitivity, lower bar).
double KnockMargin( float value );

/// The level the buffer must refill to before a muted disc plays again: a
/// quarter of the buffer.
double RestartLevel( double bufferSeconds );

} // namespace atrac::controls
