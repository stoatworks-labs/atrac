#include "Shaders.h"

namespace atrac::shaders
{
namespace
{

const char* const kVertexSource = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// convert: the host's picture to centred Y, Cb, Cr (Rec. 601), alpha kept.
// A flat mid grey has no DC, which is what keeps the scale-factor table's
// range meaningful for pictures.
//---------------------------------------------------------------------------
const char* const kConvertSource = R"(#version 410 core

uniform sampler2D InputTexture;

out vec4 fragColor;

void main()
{
	ivec2 px = ivec2( gl_FragCoord.xy );
	vec4 c   = texelFetch( InputTexture, px, 0 );
	float y  = dot( c.rgb, vec3( 0.299, 0.587, 0.114 ) ) - 0.5;
	float cb = dot( c.rgb, vec3( -0.168736, -0.331264, 0.5 ) );
	float cr = dot( c.rgb, vec3( 0.5, -0.418688, -0.081312 ) );
	fragColor = vec4( y, cb, cr, c.a );
}
)";

//---------------------------------------------------------------------------
// cells: one texel per N x N cell. 1 = short, 0 = long. In Adaptive mode the
// transient detector: the energy of the luma's first differences in each
// half of the cell, along x and along y; a ratio past kTransientRatio
// either way picks short blocks. Only differences INSIDE the cell are
// counted, so a cell's decision depends on that cell alone.
//---------------------------------------------------------------------------
const char* const kCellsSource = R"(#version 410 core

uniform sampler2D Picture;//Y Cb Cr from convert
uniform int N;
uniform int W;
uniform int H;
uniform int Mode;          //0 long, 1 short, 2 adaptive
uniform float Ratio;       //kTransientRatio
uniform float FloorEnergy; //kTransientFloorPerPixel x the differences in a half
uniform int ForceLong;     //perturb: every cell long

out vec4 fragColor;

float lumaAt( int x, int y )
{
	return texelFetch( Picture, ivec2( clamp( x, 0, W - 1 ), clamp( y, 0, H - 1 ) ), 0 ).r;
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	float shortCell = 0.0;
	if( ForceLong == 0 )
	{
		if( Mode == 1 )
			shortCell = 1.0;
		else if( Mode == 2 )
		{
			int x0 = cell.x * N;
			int y0 = cell.y * N;
			float eL = 0.0, eR = 0.0, eT = 0.0, eB = 0.0;
			for( int j = 0; j < N; ++j )
				for( int i = 1; i < N; ++i )
				{
					float dx = lumaAt( x0 + i, y0 + j ) - lumaAt( x0 + i - 1, y0 + j );
					float dy = lumaAt( x0 + j, y0 + i ) - lumaAt( x0 + j, y0 + i - 1 );
					if( i < N / 2 )
					{
						eL += dx * dx;
						eB += dy * dy;
					}
					else
					{
						eR += dx * dx;
						eT += dy * dy;
					}
				}
			eL += FloorEnergy;
			eR += FloorEnergy;
			eT += FloorEnergy;
			eB += FloorEnergy;
			if( eL > Ratio * eR || eR > Ratio * eL || eT > Ratio * eB || eB > Ratio * eT )
				shortCell = 1.0;
		}
	}
	fragColor = vec4( shortCell, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// The lapped transform's geometry, shared by the four transform passes.
//
// Along one axis a cell b of N samples is one block of N (long) or four
// blocks of Q = N/4 (short). A block starts at s, has M coefficients, and
// meets its neighbours across overlaps Ll and Lr: the smaller of the two
// blocks meeting at that boundary, and zero at the picture's edge. Its
// window spans indices n = 0..2M-1 for samples s - M/2 + n, and is a sine
// ramp of length L centred on each boundary, zero outside, one inside.
// The basis texture holds sqrt(2/M) cos( pi/M (n + 1/2 + M/2)(k + 1/2) ),
// long blocks in rows 0..N-1 and short ones in rows N..N+Q-1, from the CPU
// in double.
//---------------------------------------------------------------------------
const char* const kLappedSource = R"(
uniform sampler2D Cells;
uniform sampler2D Basis;
uniform int N;
uniform int Bx;
uniform int By;
uniform float RampLong[ 32 ];
uniform float RampShort[ 8 ];
uniform int RectWindow;

bool shortCell( int bx, int by )
{
	return texelFetch( Cells, ivec2( clamp( bx, 0, Bx - 1 ), clamp( by, 0, By - 1 ) ), 0 ).r > 0.5;
}

float ramp( int j, int L )
{
	return L == N ? RampLong[ j ] : RampShort[ j ];
}

//The window at index n of a block of M with overlaps Ll and Lr.
float windowAt( int n, int M, int Ll, int Lr )
{
	if( RectWindow == 1 )
		return 1.0;
	int p = n - M / 2;
	if( p < -Ll / 2 || p >= M + Lr / 2 )
		return 0.0;
	if( p < Ll / 2 )
		return ramp( p + Ll / 2, Ll );
	if( p >= M - Lr / 2 )
		return ramp( M + Lr / 2 - 1 - p, Lr );
	return 1.0;
}

//sqrt(2/M) cos(...) for window index n and coefficient k of a block of M.
float basisAt( int n, int k, int M )
{
	return M == N ? texelFetch( Basis, ivec2( n, k ), 0 ).r : texelFetch( Basis, ivec2( n, N + k ), 0 ).r;
}

//The block `sub` of cell b along an axis of `count` cells: its start, size,
//overlaps and the first coefficient index it owns. `sh` is the cell's own
//mode, `shL`/`shR` its neighbours'.
void blockOf( int b, int sub, bool sh, bool shL, bool shR, int count,
              out int s, out int M, out int Ll, out int Lr, out int col )
{
	int Q       = N / 4;
	bool edgeL  = b == 0;
	bool edgeR  = b == count - 1;
	if( !sh )
	{
		s   = b * N;
		M   = N;
		col = b * N;
		Ll  = edgeL ? 0 : ( shL ? Q : N );
		Lr  = edgeR ? 0 : ( shR ? Q : N );
	}
	else
	{
		s   = b * N + sub * Q;
		M   = Q;
		col = s;
		Ll  = sub > 0 ? Q : ( edgeL ? 0 : Q );
		Lr  = sub < 3 ? Q : ( edgeR ? 0 : Q );
	}
}
)";

//---------------------------------------------------------------------------
// The forward transform along one axis. `along` picks x or y: the sample
// axis is the one transformed, the other is carried. Written once as a
// function of abstract coordinates; the two passes are the same text with
// the axis swapped by the fetch/store helpers below.
//---------------------------------------------------------------------------
const char* const kForwardBody = R"(
void main()
{
	ivec2 fc = ivec2( gl_FragCoord.xy );
	int a    = alongCoord( fc );//coefficient index along the transformed axis
	int o    = otherCoord( fc );//the carried coordinate
	int b    = a / N;
	int c    = a - b * N;
	int oc   = o / N;
	int cnt  = cellCount();

	bool sh  = cellShort( b, oc );
	bool shL = cellShort( b - 1, oc );
	bool shR = cellShort( b + 1, oc );
	int Q    = N / 4;
	int sub  = sh ? c / Q : 0;
	int k    = sh ? c - sub * Q : c;

	int s, M, Ll, Lr, col;
	blockOf( b, sub, sh, shL, shR, cnt, s, M, Ll, Lr, col );

	vec3 acc  = vec3( 0.0 );
	int first = s - M / 2;
	for( int n = 0; n < 2 * M; ++n )
	{
		float w = windowAt( n, M, Ll, Lr );
		if( w == 0.0 )
			continue;
		acc += ( w * basisAt( n, k, M ) ) * sampleAt( first + n, o );
	}
	fragColor = vec4( acc, 1.0 );
}
)";

const char* const kMdctXHead = R"(#version 410 core

uniform sampler2D Source;//Y Cb Cr, W x H
uniform int W;
uniform int H;

out vec4 fragColor;
)";

const char* const kMdctXHelpers = R"(
int alongCoord( ivec2 fc ) { return fc.x; }
int otherCoord( ivec2 fc ) { return fc.y; }
int cellCount() { return Bx; }
bool cellShort( int b, int oc ) { return shortCell( b, oc ); }
vec3 sampleAt( int pos, int o ) { return texelFetch( Source, ivec2( clamp( pos, 0, W - 1 ), o ), 0 ).rgb; }
)";

const char* const kMdctYHead = R"(#version 410 core

uniform sampler2D Source;//coefficients along x, Bx N x H
uniform int W;
uniform int H;

out vec4 fragColor;
)";

const char* const kMdctYHelpers = R"(
int alongCoord( ivec2 fc ) { return fc.y; }
int otherCoord( ivec2 fc ) { return fc.x; }
int cellCount() { return By; }
bool cellShort( int b, int oc ) { return shortCell( oc, b ); }
vec3 sampleAt( int pos, int o ) { return texelFetch( Source, ivec2( o, clamp( pos, 0, H - 1 ) ), 0 ).rgb; }
)";

//---------------------------------------------------------------------------
// The inverse along one axis: the sample at `a` is the windowed sum over
// its own block's coefficients plus, inside an overlap, the neighbour's.
//---------------------------------------------------------------------------
const char* const kInverseBody = R"(
vec3 blockContribution( int a, int s, int M, int Ll, int Lr, int col, int o )
{
	int n   = a - s + M / 2;
	float w = windowAt( n, M, Ll, Lr );
	if( w == 0.0 )
		return vec3( 0.0 );
	vec3 acc = vec3( 0.0 );
	for( int k = 0; k < M; ++k )
		acc += basisAt( n, k, M ) * coefficientAt( col + k, o );
	return w * acc;
}

void main()
{
	ivec2 fc = ivec2( gl_FragCoord.xy );
	int a    = alongCoord( fc );//the sample position along the axis
	int o    = otherCoord( fc );
	int b    = a / N;
	int q    = a - b * N;
	int oc   = o / N;
	int cnt  = cellCount();

	bool sh  = cellShort( b, oc );
	bool shL = cellShort( b - 1, oc );
	bool shR = cellShort( b + 1, oc );
	int Q    = N / 4;
	int sub  = sh ? q / Q : 0;

	int s, M, Ll, Lr, col;
	blockOf( b, sub, sh, shL, shR, cnt, s, M, Ll, Lr, col );
	vec3 acc = blockContribution( a, s, M, Ll, Lr, col, o );

	int p = a - s;
	if( p < Ll / 2 )
	{
		//The block before: the previous sub-block, or the last block of the
		//cell to the left.
		int s2, M2, L2l, L2r, col2;
		if( sub > 0 )
			blockOf( b, sub - 1, sh, shL, shR, cnt, s2, M2, L2l, L2r, col2 );
		else
			blockOf( b - 1, shL ? 3 : 0, shL, cellShort( b - 2, oc ), sh, cnt, s2, M2, L2l, L2r, col2 );
		acc += blockContribution( a, s2, M2, L2l, L2r, col2, o );
	}
	if( p >= M - Lr / 2 )
	{
		int s2, M2, L2l, L2r, col2;
		if( sub < 3 && sh )
			blockOf( b, sub + 1, sh, shL, shR, cnt, s2, M2, L2l, L2r, col2 );
		else
			blockOf( b + 1, 0, shR, sh, cellShort( b + 2, oc ), cnt, s2, M2, L2l, L2r, col2 );
		acc += blockContribution( a, s2, M2, L2l, L2r, col2, o );
	}
	finish( acc, fc );
}
)";

const char* const kImdctYHead = R"(#version 410 core

uniform sampler2D Source;//quantised coefficients, Bx N x By N
uniform int W;
uniform int H;

out vec4 fragColor;
)";

const char* const kImdctYHelpers = R"(
int alongCoord( ivec2 fc ) { return fc.y; }
int otherCoord( ivec2 fc ) { return fc.x; }
int cellCount() { return By; }
bool cellShort( int b, int oc ) { return shortCell( oc, b ); }
vec3 coefficientAt( int idx, int o ) { return texelFetch( Source, ivec2( o, idx ), 0 ).rgb; }
void finish( vec3 acc, ivec2 fc ) { fragColor = vec4( acc, 1.0 ); }
)";

const char* const kImdctXHead = R"(#version 410 core

uniform sampler2D Source;      //samples along y, coefficients along x: Bx N x H
uniform sampler2D InputTexture;//for the alpha
uniform int W;
uniform int H;

out vec4 fragColor;
)";

const char* const kImdctXHelpers = R"(
int alongCoord( ivec2 fc ) { return fc.x; }
int otherCoord( ivec2 fc ) { return fc.y; }
int cellCount() { return Bx; }
bool cellShort( int b, int oc ) { return shortCell( b, oc ); }
vec3 coefficientAt( int idx, int o ) { return texelFetch( Source, ivec2( idx, o ), 0 ).rgb; }
void finish( vec3 acc, ivec2 fc )
{
	//Back to RGB (Rec. 601). The alpha is the input's.
	float y  = acc.r + 0.5;
	float cb = acc.g;
	float cr = acc.b;
	vec3 rgb = vec3( y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb );
	fragColor = vec4( rgb, texelFetch( InputTexture, fc, 0 ).a );
}
)";

//---------------------------------------------------------------------------
// The allocation's packing, shared by alloc (writes), quant and display
// (read). Per cell, channel and BFU: a word length (0, 2..16) and a
// scale-factor index (0..63). The alloc texture is 2 Bx wide and 3 By high:
// texel (2 bx + part, ch By + by) carries BFUs 8 part .. 8 part + 7, two
// per channel of the texel: value = (wl + 32 sf) + 2048 (wl' + 32 sf').
// Every value is an integer below 2^23, so a float holds it exactly.
//---------------------------------------------------------------------------
const char* const kAllocReadSource = R"(
uniform sampler2D Alloc;
uniform int Bx;
uniform int By;

//Word length and scale-factor index of BFU j, channel ch, cell (bx, by).
ivec2 allocOf( int bx, int by, int ch, int j )
{
	int part = j / 8;
	int slot = j - 8 * part;
	vec4 t   = texelFetch( Alloc, ivec2( 2 * bx + part, ch * By + by ), 0 );
	int v    = int( t[ slot / 2 ] + 0.5 );
	int hi   = v / 2048;
	int lo   = v - hi * 2048;
	int e    = ( slot - 2 * ( slot / 2 ) ) == 1 ? hi : lo;
	int sf   = e / 32;
	int wl   = e - sf * 32;
	return ivec2( wl, sf );
}
)";

//---------------------------------------------------------------------------
// alloc: per cell and channel, the whole psychoacoustic model and the
// greedy allocation. Two fragments per cell and channel do the same work
// and each writes its half of the BFUs, which costs a repeat of a small
// computation and saves a multi-target framebuffer.
//---------------------------------------------------------------------------
const char* const kAllocSource = R"(#version 410 core

uniform sampler2D Coefficients;//Bx N x By N
uniform sampler2D Cells;
uniform int N;
uniform int Bx;
uniform int By;
uniform int BudgetLuma;
uniform int BudgetChroma;
uniform int DiagBfuLong[ 64 ];
uniform int DiagBfuShort[ 16 ];
uniform int CountLong[ 16 ];
uniform int CountShort[ 16 ];
uniform int BfuLong;
uniform int BfuShort;
uniform float SfTable[ 64 ];
uniform float SpreadUp[ 16 ];
uniform float SpreadDown[ 16 ];
uniform float OffsetFactor;
uniform float FloorEnergy;
uniform int OverBudget;
uniform int StopEarly;
uniform int IgnoreMasking;

out vec4 fragColor;

void main()
{
	int ax   = int( gl_FragCoord.x );
	int ay   = int( gl_FragCoord.y );
	int bx   = ax / 2;
	int part = ax - 2 * bx;
	int ch   = ay / By;
	int by   = ay - ch * By;

	bool sh = texelFetch( Cells, ivec2( bx, by ), 0 ).r > 0.5;
	int M   = sh ? N / 4 : N;
	int nb  = sh ? BfuShort : BfuLong;

	float peak[ 16 ];
	float energy[ 16 ];
	int cnt[ 16 ];
	for( int j = 0; j < 16; ++j )
	{
		peak[ j ]   = 0.0;
		energy[ j ] = 0.0;
		cnt[ j ]    = sh ? CountShort[ j ] : CountLong[ j ];
	}

	for( int j = 0; j < N; ++j )
		for( int i = 0; i < N; ++i )
		{
			int kx = i - ( i / M ) * M;
			int ky = j - ( j / M ) * M;
			int d  = kx + ky;
			int b  = sh ? DiagBfuShort[ d ] : DiagBfuLong[ d ];
			float c = texelFetch( Coefficients, ivec2( bx * N + i, by * N + j ), 0 )[ ch ];
			peak[ b ]   = max( peak[ b ], abs( c ) );
			energy[ b ] += c * c;
		}

	//Mean energy per coefficient, and the masked threshold: the largest
	//spread contribution from any OTHER band, less the offset, floored.
	float E[ 16 ];
	float T[ 16 ];
	for( int j = 0; j < nb; ++j )
		E[ j ] = energy[ j ] / float( max( cnt[ j ], 1 ) );
	for( int j = 0; j < nb; ++j )
	{
		float spread = 0.0;
		for( int i = 0; i < nb; ++i )
		{
			if( i == j )
				continue;
			float f = i < j ? SpreadUp[ j - i ] : SpreadDown[ i - j ];
			spread  = max( spread, E[ i ] * f );
		}
		T[ j ] = max( FloorEnergy, spread * OffsetFactor );
		if( IgnoreMasking == 1 )
			T[ j ] = FloorEnergy;
	}

	//The greedy: the highest noise-to-mask ratio takes the next increment.
	//ratio is E / T at zero bits; the first increment (2 bits) divides it
	//by 16, every later one by 4. A masked band (E below T) starts with a
	//ratio below one and so comes LAST in the queue -- it gets bits only
	//once every louder band has been driven below it, which on a short
	//budget is never. A band below the absolute floor is out of the queue.
	int budget = ch == 0 ? BudgetLuma : BudgetChroma;
	if( OverBudget == 1 )
		budget = ( budget * 5 ) / 4;
	int left = budget;
	float ratio[ 16 ];
	int wl[ 16 ];
	for( int j = 0; j < 16; ++j )
	{
		wl[ j ]    = 0;
		ratio[ j ] = ( j < nb && E[ j ] > FloorEnergy ) ? E[ j ] / T[ j ] : 0.0;
	}
	for( int iter = 0; iter < 256; ++iter )
	{
		int best    = -1;
		float bestR = 0.0;
		for( int j = 0; j < nb; ++j )
		{
			if( ratio[ j ] <= 0.0 || wl[ j ] >= 16 )
				continue;
			int cost  = ( wl[ j ] == 0 ? 2 : 1 ) * cnt[ j ];
			bool fits = cost <= left;
			if( ( StopEarly == 1 || fits ) && ratio[ j ] > bestR )
			{
				bestR = ratio[ j ];
				best  = j;
			}
		}
		if( best < 0 )
			break;
		int step = wl[ best ] == 0 ? 2 : 1;
		int cost = step * cnt[ best ];
		if( cost > left )
			break;//only under StopEarly: the top band did not fit
		wl[ best ] += step;
		left -= cost;
		ratio[ best ] *= step == 2 ? 0.0625 : 0.25;
	}

	//Scale factors: the smallest table entry not below the peak.
	int sf[ 16 ];
	for( int j = 0; j < 16; ++j )
	{
		sf[ j ] = 63;
		if( j < nb )
			for( int i = 0; i < 64; ++i )
				if( peak[ j ] <= SfTable[ i ] )
				{
					sf[ j ] = i;
					break;
				}
		else
			sf[ j ] = 0;
	}

	vec4 packed;
	for( int c = 0; c < 4; ++c )
	{
		int j0 = 8 * part + 2 * c;
		int j1 = j0 + 1;
		int e0 = j0 < nb ? wl[ j0 ] + 32 * sf[ j0 ] : 0;
		int e1 = j1 < nb ? wl[ j1 ] + 32 * sf[ j1 ] : 0;
		packed[ c ] = float( e0 + 2048 * e1 );
	}
	fragColor = packed;
}
)";

//---------------------------------------------------------------------------
// quant: every coefficient through its BFU's word length and scale factor,
// ATRAC1's way: a signed integer of wl bits, symmetric, q = round( c / sf
// x L ), L = 2^(wl - 1) - 1, back to q sf / L. Word length 0 zeroes it.
//---------------------------------------------------------------------------
const char* const kQuantHead = R"(#version 410 core

uniform sampler2D Coefficients;
uniform sampler2D Cells;
uniform int N;
uniform int DiagBfuLong[ 64 ];
uniform int DiagBfuShort[ 16 ];
uniform float SfTable[ 64 ];
uniform int Bypass;

out vec4 fragColor;
)";

const char* const kQuantBody = R"(
float quantise( float c, int wl, int sfIndex )
{
	if( wl == 0 )
		return 0.0;
	float sf = SfTable[ sfIndex ];
	float L  = float( ( 1 << ( wl - 1 ) ) - 1 );
	float q  = floor( c / sf * L + 0.5 );
	q        = clamp( q, -L, L );
	return q * sf / L;
}

void main()
{
	ivec2 fc = ivec2( gl_FragCoord.xy );
	vec4 c   = texelFetch( Coefficients, fc, 0 );
	if( Bypass == 1 )
	{
		fragColor = c;
		return;
	}
	int bx  = fc.x / N;
	int by  = fc.y / N;
	int i   = fc.x - bx * N;
	int j   = fc.y - by * N;
	bool sh = texelFetch( Cells, ivec2( bx, by ), 0 ).r > 0.5;
	int M   = sh ? N / 4 : N;
	int kx  = i - ( i / M ) * M;
	int ky  = j - ( j / M ) * M;
	int d   = kx + ky;
	int b   = sh ? DiagBfuShort[ d ] : DiagBfuLong[ d ];

	vec3 out3;
	for( int ch = 0; ch < 3; ++ch )
	{
		ivec2 a    = allocOf( bx, by, ch, b );
		out3[ ch ] = quantise( c[ ch ], a.x, a.y );
	}
	fragColor = vec4( out3, c.a );
}
)";

//---------------------------------------------------------------------------
// display: the mix, and the two views. Show Bits paints each cell with its
// own luma allocation as if the cell were its coefficient plane: pixel
// (i, j) of the cell shows the word length of the BFU that coefficient
// (i, j) belongs to. Show Blocks draws the cell grid and tints short cells.
//---------------------------------------------------------------------------
const char* const kDisplayHead = R"(#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D Held;
uniform sampler2D Cells;
uniform int N;
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;
uniform float MixAmount;
uniform int ShowBlocks;
uniform int ShowBits;
uniform int DiagBfuLong[ 64 ];
uniform int DiagBfuShort[ 16 ];

out vec4 fragColor;
)";

const char* const kDisplayBody = R"(
vec3 heat( int wl )
{
	if( wl == 0 )
		return vec3( 0.0 );
	float t = float( wl ) / 16.0;
	vec3 a  = vec3( 0.05, 0.05, 0.35 );
	vec3 b  = vec3( 0.95, 0.45, 0.05 );
	vec3 c  = vec3( 1.0 );
	return t < 0.5 ? mix( a, b, t * 2.0 ) : mix( b, c, t * 2.0 - 1.0 );
}

void main()
{
	ivec2 px = ivec2( gl_FragCoord.xy ) - ivec2( VpX, VpY );
	vec2 uv  = ( vec2( px ) + 0.5 ) / vec2( VpW, VpH );
	vec4 src = texelFetch( InputTexture, px, 0 );
	vec4 dec = texture( Held, uv );
	vec3 rgb = mix( src.rgb, dec.rgb, MixAmount );

	int bx  = px.x / N;
	int by  = px.y / N;
	int i   = px.x - bx * N;
	int j   = px.y - by * N;
	bool sh = texelFetch( Cells, ivec2( bx, by ), 0 ).r > 0.5;

	if( ShowBits == 1 )
	{
		int M  = sh ? N / 4 : N;
		int kx = i - ( i / M ) * M;
		int ky = j - ( j / M ) * M;
		int b  = sh ? DiagBfuShort[ kx + ky ] : DiagBfuLong[ kx + ky ];
		rgb    = heat( allocOf( bx, by, 0, b ).x );
	}
	if( ShowBlocks == 1 )
	{
		if( sh )
		{
			rgb = mix( rgb, vec3( 1.0, 0.55, 0.1 ), 0.3 );
			int Q = N / 4;
			if( i - ( i / Q ) * Q == 0 || j - ( j / Q ) * Q == 0 )
				rgb = mix( rgb, vec3( 1.0, 0.8, 0.4 ), 0.5 );
		}
		if( i == 0 || j == 0 )
			rgb = vec3( 0.2, 0.9, 1.0 );
	}
	fragColor = vec4( rgb, src.a );
}
)";

std::string join( std::initializer_list< const char* > parts )
{
	std::string out;
	for( const char* p : parts )
		out += p;
	return out;
}

} // namespace

const std::string& Vertex()
{
	static const std::string s = kVertexSource;
	return s;
}
const std::string& Convert()
{
	static const std::string s = kConvertSource;
	return s;
}
const std::string& Cells()
{
	static const std::string s = kCellsSource;
	return s;
}
const std::string& MdctX()
{
	static const std::string s = join( { kMdctXHead, kLappedSource, kMdctXHelpers, kForwardBody } );
	return s;
}
const std::string& MdctY()
{
	static const std::string s = join( { kMdctYHead, kLappedSource, kMdctYHelpers, kForwardBody } );
	return s;
}
const std::string& Alloc()
{
	static const std::string s = kAllocSource;
	return s;
}
const std::string& Quant()
{
	static const std::string s = join( { kQuantHead, kAllocReadSource, kQuantBody } );
	return s;
}
const std::string& ImdctY()
{
	static const std::string s = join( { kImdctYHead, kLappedSource, kImdctYHelpers, kInverseBody } );
	return s;
}
const std::string& ImdctX()
{
	static const std::string s = join( { kImdctXHead, kLappedSource, kImdctXHelpers, kInverseBody } );
	return s;
}
const std::string& Display()
{
	static const std::string s = join( { kDisplayHead, kAllocReadSource, kDisplayBody } );
	return s;
}

} // namespace atrac::shaders
