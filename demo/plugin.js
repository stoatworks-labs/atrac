/**
 * Atrac — browser demo.
 *
 * A perceptual coder modelled on MiniDisc's ATRAC, run on the picture. The one
 * idea, from `source/Codec.h`: detail is thrown away by BITS, not by
 * resolution. The picture is cut into N × N cells, each axis of a cell is one
 * lapped block (or four short ones) through the MDCT, the coefficients are
 * grouped into block floating units with a scale factor and a word length
 * each, and a fixed budget of bits per cell is spent by a greedy on
 * noise-to-mask ratio. On top, a shock-proof buffer that a knock drains: when
 * it empties, the picture holds.
 *
 * Like clamp and galvo, this plugin is **not only a shader**, and the two
 * halves of the page are not equally faithful:
 *
 *   The shaders are the plugin's. The twenty GLSL snippets below are
 *   `source/Shaders.cpp`'s string constants, copied across unedited and
 *   joined into the ten programs exactly as `Shaders.cpp` joins them
 *   (`MdctX()` is head + lapped + helpers + forward body, and so on).
 *   `demo/tools/check_shaders.py` compares every snippet character for
 *   character and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Codec.cpp` (the BFU rule, the scale-factor
 *   law, the ramps and basis computed in double, the masking constants),
 *   `Controls.cpp`, `Disc.cpp` (the buffer and the onset detector), `Clock.cpp`
 *   and the frame sequence in `Atrac::ProcessOpenGL` — function for function.
 *   Nothing checks a port but a reader. `actest --tables`, `--tdac`,
 *   `--budget`, `--skipmodel` and the rest check the C++ originals and have no
 *   idea this page exists.
 *
 * ------------------------------------------------------ the tables
 *
 * The plugin computes the basis, the ramps, the scale factors and the
 * spreading factors on the CPU in double and hands them to the shaders as an
 * R32F texture and float/int uniform arrays, so no driver `cos`, `log` or
 * `pow` is in any measured path. The page does the same in JavaScript numbers,
 * which are IEEE doubles, and uploads the same tables.
 *
 * ------------------------------------------------------- the disc
 *
 * `Disc.cpp` is ported whole: the buffer's exact arithmetic on elapsed
 * seconds, and the onset detector. On this page the detector is handed no
 * spectrum — a browser has no Resolume FFT — which it reads exactly as the
 * plugin reads an unrouted Audio input: silence, no knock. Knock is the
 * plugin's FF_TYPE_EVENT; the kit has no event control, so it is a toggle the
 * page releases after one frame — one press, one rising edge, which is what
 * the plugin counts.
 *
 * ------------------------------------------------------- the clock
 *
 * The disc runs in real time on the host's clock. The page's clock is the
 * kit's `time` — seconds since the page started, paused by Pause and stepped
 * by Step — fed to the ported `Clock` with the unit DECLARED as seconds, the
 * way `actest` declares its own. The unit vote the plugin runs against
 * Resolume's millisecond clock never runs here. Restart sends the clock
 * backwards, which the port treats as the plugin treats a scrub: one nominal
 * frame on.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** The Audio FFT buffer is absent rather than present and
 * dead; Sensitivity is here as the plugin's control and does nothing, because
 * no spectrum ever arrives. **The About block is absent**, as on every page in
 * this suite. **The held picture is sampled with linear filtering only where
 * the browser offers OES_texture_float_linear**; elsewhere it is nearest,
 * which only matters while a hold is being scaled to a new raster.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here. The one
// backtick in a GLSL comment is escaped, and check_shaders.py unescapes it.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const CONVERT = `#version 410 core

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
`;

const CELLS = `#version 410 core

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
`;

const LAPPED = `
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

//The block \`sub\` of cell b along an axis of \`count\` cells: its start, size,
//overlaps and the first coefficient index it owns. \`sh\` is the cell's own
//mode, \`shL\`/\`shR\` its neighbours'.
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
`;

const FORWARD_BODY = `
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
`;

const MDCTX_HEAD = `#version 410 core

uniform sampler2D Source;//Y Cb Cr, W x H
uniform int W;
uniform int H;

out vec4 fragColor;
`;

const MDCTX_HELPERS = `
int alongCoord( ivec2 fc ) { return fc.x; }
int otherCoord( ivec2 fc ) { return fc.y; }
int cellCount() { return Bx; }
bool cellShort( int b, int oc ) { return shortCell( b, oc ); }
vec3 sampleAt( int pos, int o ) { return texelFetch( Source, ivec2( clamp( pos, 0, W - 1 ), o ), 0 ).rgb; }
`;

const MDCTY_HEAD = `#version 410 core

uniform sampler2D Source;//coefficients along x, Bx N x H
uniform int W;
uniform int H;

out vec4 fragColor;
`;

const MDCTY_HELPERS = `
int alongCoord( ivec2 fc ) { return fc.y; }
int otherCoord( ivec2 fc ) { return fc.x; }
int cellCount() { return By; }
bool cellShort( int b, int oc ) { return shortCell( oc, b ); }
vec3 sampleAt( int pos, int o ) { return texelFetch( Source, ivec2( o, clamp( pos, 0, H - 1 ) ), 0 ).rgb; }
`;

const INVERSE_BODY = `
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
`;

const IMDCTY_HEAD = `#version 410 core

uniform sampler2D Source;//quantised coefficients, Bx N x By N
uniform int W;
uniform int H;

out vec4 fragColor;
`;

const IMDCTY_HELPERS = `
int alongCoord( ivec2 fc ) { return fc.y; }
int otherCoord( ivec2 fc ) { return fc.x; }
int cellCount() { return By; }
bool cellShort( int b, int oc ) { return shortCell( oc, b ); }
vec3 coefficientAt( int idx, int o ) { return texelFetch( Source, ivec2( o, idx ), 0 ).rgb; }
void finish( vec3 acc, ivec2 fc ) { fragColor = vec4( acc, 1.0 ); }
`;

const IMDCTX_HEAD = `#version 410 core

uniform sampler2D Source;      //samples along y, coefficients along x: Bx N x H
uniform sampler2D InputTexture;//for the alpha
uniform int W;
uniform int H;

out vec4 fragColor;
`;

const IMDCTX_HELPERS = `
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
`;

const ALLOC_READ = `
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
`;

const ALLOC = `#version 410 core

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

	//Not \`packed\`: that is a reserved word in GLSL 4.10 (Mesa refuses it; Apple's
	//compiler and glslc at 4.5 let it through), and the first Arena load on
	//Windows failed on exactly that.
	vec4 slots;
	for( int c = 0; c < 4; ++c )
	{
		int j0 = 8 * part + 2 * c;
		int j1 = j0 + 1;
		int e0 = j0 < nb ? wl[ j0 ] + 32 * sf[ j0 ] : 0;
		int e1 = j1 < nb ? wl[ j1 ] + 32 * sf[ j1 ] : 0;
		slots[ c ] = float( e0 + 2048 * e1 );
	}
	fragColor = slots;
}
`;

const QUANT_HEAD = `#version 410 core

uniform sampler2D Coefficients;
uniform sampler2D Cells;
uniform int N;
uniform int DiagBfuLong[ 64 ];
uniform int DiagBfuShort[ 16 ];
uniform float SfTable[ 64 ];
uniform int Bypass;

out vec4 fragColor;
`;

const QUANT_BODY = `
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
`;

const DISPLAY_HEAD = `#version 410 core

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
`;

const DISPLAY_BODY = `
vec3 heat( int wl )
{
	if( wl == 0 )
		return vec3( 0.0 );
	float t = float( wl ) / 16.0;
	vec3 a  = vec3( 0.10, 0.16, 0.62 );//2 bits, the commonest word length at the default rate, must read as blue and not as black
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
`;


// Shaders.cpp's join(), snippet for snippet.
const MDCTX = MDCTX_HEAD + LAPPED + MDCTX_HELPERS + FORWARD_BODY;
const MDCTY = MDCTY_HEAD + LAPPED + MDCTY_HELPERS + FORWARD_BODY;
const QUANT = QUANT_HEAD + ALLOC_READ + QUANT_BODY;
const IMDCTY = IMDCTY_HEAD + LAPPED + IMDCTY_HELPERS + INVERSE_BODY;
const IMDCTX = IMDCTX_HEAD + LAPPED + IMDCTX_HELPERS + INVERSE_BODY;
const DISPLAY = DISPLAY_HEAD + ALLOC_READ + DISPLAY_BODY;

//===========================================================================
// Controls.cpp, ported. Every conversion from the host's 0..1 to the coder's
// unit lives there and nowhere else, so it lives here and nowhere else too.
//===========================================================================

const unit = (v) => Math.min(1, Math.max(0, v));
const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));

/// 0.05 x 80^v bits per pixel: 0.05 to 4.
const bitsPerPixel = (v) => 0.05 * Math.pow(80, unit(v));
const chromaFraction = (v) => unit(v);
const maskingAmount = (v) => unit(v);
/// 10 v seconds.
const bufferSeconds = (v) => 10 * unit(v);
/// 1 + 3 v: 1x to 4x.
const readSpeed = (v) => 1 + 3 * unit(v);
/// 0.05 x 100^v seconds: 50 ms to 5 s.
const knockSeconds = (v) => 0.05 * Math.pow(100, unit(v));
/// 2.5 - 2.35 v.
const knockMargin = (v) => 2.5 - 2.35 * unit(v);
const restartLevel = (buffer) => 0.25 * buffer;

//===========================================================================
// Codec.cpp, ported: the tables and laws, in double.
//===========================================================================

const BLOCK_SIZES = [8, 16, 32];
const K_BLOCK_SIZE_COUNT = 3;
const K_BLOCK_MODE_COUNT = 3;
const blockSizeOf = (option) => BLOCK_SIZES[Math.min(K_BLOCK_SIZE_COUNT - 1, Math.max(0, option))];

/// The master list of diagonal edges: DC on its own, as ATRAC1 keeps its lowest
/// BFUs narrow, then each band about 4/3 the width of the last.
const MASTER_EDGES = [0, 1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 48, 63];

function bfuEdges(M) {
  const last = 2 * M - 1;
  const edges = [];
  for (const e of MASTER_EDGES) {
    if (e >= last) break;
    edges.push(e);
  }
  edges.push(last);
  return edges;
}
const bfuCount = (M) => bfuEdges(M).length - 1;

function bfuCoefficients(M, j) {
  const edges = bfuEdges(M);
  if (j < 0 || j + 1 >= edges.length) return 0;
  let count = 0;
  for (let d = edges[j]; d < edges[j + 1]; d += 1) {
    const lo = Math.max(0, d - M + 1);
    const hi = Math.min(d, M - 1);
    count += hi - lo + 1;
  }
  return count;
}

function bfuOfDiagonal(M, d) {
  const edges = bfuEdges(M);
  for (let j = 0; j + 1 < edges.length; j += 1) {
    if (d >= edges[j] && d < edges[j + 1]) return j;
  }
  return -1;
}

const K_SCALE_FACTORS = 64;
/// ATRAC1's scale factors: 2^((i - 15)/3).
const scaleFactor = (index) => Math.pow(2, (Math.min(K_SCALE_FACTORS - 1, Math.max(0, index)) - 15) / 3);

/// w[j] = sin( pi (j + 1/2) / (2L) ), j < L.
function ramp(L) {
  const w = new Float64Array(Math.max(0, L));
  for (let j = 0; j < L; j += 1) w[j] = Math.sin(Math.PI * (j + 0.5) / (2 * L));
  return w;
}

/// [n][k] = sqrt(2/M) cos( pi/M (n + 1/2 + M/2)(k + 1/2) ), n < 2M, k < M, row-major.
function basis(M) {
  const table = new Float64Array(2 * M * M);
  const scale = Math.sqrt(2 / M);
  for (let n = 0; n < 2 * M; n += 1) {
    for (let k = 0; k < M; k += 1) {
      table[n * M + k] = scale * Math.cos(Math.PI / M * (n + 0.5 + M / 2) * (k + 0.5));
    }
  }
  return table;
}

const K_SPREAD_UP_DB = 10;
const K_SPREAD_DOWN_DB = 24;
const K_ABSOLUTE_FLOOR = (0.5 / 255) * (0.5 / 255);
const K_MASKING_RANGE_DB = 60;
const maskingOffsetDb = (m) => K_MASKING_RANGE_DB * (1 - Math.min(1, Math.max(0, m)));
const K_TRANSIENT_RATIO = 8;
const K_TRANSIENT_FLOOR_PER_PIXEL = (1 / 255) * (1 / 255);

//===========================================================================
// Disc.cpp, ported: the buffer and the onset detector, in double. The
// negative-control perturbations are harness-only and always 0 in the plugin,
// so they are not carried here.
//===========================================================================

const K_BINS = 64;

class Buffer {
  constructor() {
    this.level = 0;
    this.stopLeft = 0;
    this.muted = false;
    this.started = false;
  }

  reset(bufferSecs) {
    this.level = Math.max(0, bufferSecs);
    this.stopLeft = 0;
    this.muted = false;
    this.started = true;
  }

  step(dt, knock, s) {
    if (!this.started) this.reset(s.bufferSeconds);
    const capacity = Math.max(0, s.bufferSeconds);
    this.level = Math.min(this.level, capacity);

    if (knock) this.stopLeft = Math.max(this.stopLeft, s.knockSeconds);

    if (dt <= 0) {
      if (!this.muted && this.level <= 0 && this.stopLeft > 0) this.muted = true;
      return;
    }

    const stopped = Math.min(dt, Math.max(0, this.stopLeft));
    const reading = dt - stopped;
    this.stopLeft = Math.max(0, this.stopLeft - dt);
    const readRate = Math.max(1, s.readSpeed);

    if (!this.muted) {
      this.level -= stopped;
      if (this.level <= 0) {
        this.level = 0;
        this.muted = true;
        this.level += readRate * reading;
      } else {
        this.level += (readRate - 1) * reading;
      }
    } else {
      this.level += readRate * reading;
    }

    this.level = Math.min(this.level, capacity);
    if (this.muted && this.stopLeft <= 0 && this.level >= Math.min(capacity, s.restartLevel)) this.muted = false;
  }
}

class Onsets {
  constructor() {
    this.reset();
  }

  reset() {
    this.previous = new Float64Array(K_BINS);
    this.fluxMean = 0;
    this.flux = 0;
    this.bar = 0;
    this.refractory = 0;
    this.primed = false;
    this.onsets = 0;
  }

  /// `bins` null means the host wrote nothing: silence, as an unrouted input.
  update(bins, count, dt, margin) {
    const n = Math.min(K_BINS, Math.max(0, count));
    const now = new Float64Array(K_BINS);
    for (let i = 0; i < n; i += 1) now[i] = Math.sqrt(Math.max(0, bins ? bins[i] : 0));

    if (!this.primed) {
      this.primed = true;
      this.previous = now;
      this.flux = 0;
      this.bar = Math.max(Onsets.kFluxFloor, this.fluxMean * (1 + Math.max(0, margin)));
      return false;
    }
    if (dt <= 0) return false;

    let rise = 0;
    for (let i = 0; i < K_BINS; i += 1) {
      rise += Math.max(0, now[i] - this.previous[i]);
      this.previous[i] = now[i];
    }
    this.flux = rise / K_BINS;

    this.bar = Math.max(Onsets.kFluxFloor, this.fluxMean * (1 + Math.max(0, margin)));
    this.refractory = Math.max(0, this.refractory - dt);
    let fired = false;
    if (margin >= 0 && this.refractory <= 0 && this.flux > this.bar) {
      fired = true;
      this.refractory = Onsets.kRefractory;
      this.onsets += 1;
    }
    const coefficient = 1 - Math.exp(-dt / Onsets.kMeanSeconds);
    this.fluxMean += (this.flux - this.fluxMean) * coefficient;
    return fired;
  }
}
Onsets.kRefractory = 0.08;
Onsets.kMeanSeconds = 1.0;
Onsets.kFluxFloor = 0.004;

//===========================================================================
// Clock.cpp, ported, with the unit declared as seconds (SetScaleForTest(1),
// as actest does). A delta is believed unless it is backwards or longer than
// half a second, in which case the clock steps on by one nominal frame.
//===========================================================================

class Clock {
  constructor() {
    this.reset();
  }

  reset() {
    this.started = false;
    this.lastScaled = -1;
    this.anchor = 0;
    this.offset = 0;
    this.now = 0;
    this.jumped = false;
  }

  update(scaled) {
    this.jumped = false;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0;
      this.now = 0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0 || delta > Clock.kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + Clock.kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}
Clock.kMaxFrameSeconds = 0.5;
Clock.kNominalFrameSeconds = 1.0 / 60.0;

/// Atrac.cpp's kMaxFrameDelta: the audio clock cannot believe a longer delta.
const K_MAX_FRAME_DELTA = 0.5;

//===========================================================================
// The renderer: Atrac::ProcessOpenGL, in its order.
//
//   1. clock, audio, disc   CPU
//   2. convert              W x H       Y - 1/2, Cb, Cr, alpha
//   3. cells                Bx x By     long or short
//   if decoding:
//   4. mdctx                Bx N x H
//   5. mdcty                Bx N x By N
//   6. alloc                2 Bx x 3 By the model and the greedy, packed
//   7. quant                Bx N x By N
//   8. imdcty               into mdctx's buffer, spent by then
//   9. imdctx               into held, W x H, RGB
//  10. display              onto the canvas
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { N: 0, cells: 0, budgetLuma: 0, budgetChroma: 0, level: 0, capacity: 0, muted: false, knocks: 0, reading: true };

function createFloatTexture(gl) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return texture;
}

/// Program has float-array and scalar setters; the int arrays go straight to
/// glUniform1iv, as the plugin's setInts does.
function setInts(program, name, values) {
  const loc = program.location(`${name}[0]`) ?? program.location(name);
  if (loc !== null) program.gl.uniform1iv(loc, Int32Array.from(values));
}

function createRenderer(gl, quad) {
  const shaders = {
    convert: new Program(gl, VERTEX, CONVERT, 'convert'),
    cells: new Program(gl, VERTEX, CELLS, 'cells'),
    mdctx: new Program(gl, VERTEX, MDCTX, 'mdctx'),
    mdcty: new Program(gl, VERTEX, MDCTY, 'mdcty'),
    alloc: new Program(gl, VERTEX, ALLOC, 'alloc'),
    quant: new Program(gl, VERTEX, QUANT, 'quant'),
    imdcty: new Program(gl, VERTEX, IMDCTY, 'imdcty'),
    imdctx: new Program(gl, VERTEX, IMDCTX, 'imdctx'),
    display: new Program(gl, VERTEX, DISPLAY, 'display'),
  };

  const nearest = { filter: 'nearest' };
  const converted = new PassBuffer(gl, nearest);
  const cells = new PassBuffer(gl, nearest);
  const lapped = new PassBuffer(gl, nearest);
  const coefficients = new PassBuffer(gl, nearest);
  const quantised = new PassBuffer(gl, nearest);
  const alloc = new PassBuffer(gl, nearest);
  // The plugin samples the held picture linearly (a hold being scaled to a new
  // raster). RGBA32F with LINEAR needs OES_texture_float_linear; without it the
  // texture is incomplete and reads black, so fall back to nearest and say so.
  const floatLinear = gl.getExtension('OES_texture_float_linear') !== null;
  const held = new PassBuffer(gl, { filter: floatLinear ? 'linear' : 'nearest' });

  const basisTexture = createFloatTexture(gl);
  const tables = { N: 0 };
  const clock = new Clock();
  const buffer = new Buffer();
  const onsets = new Onsets();

  // The state Atrac carries across host frames.
  let audioClock = -1;
  let heldValid = false;
  let heldWidth = 0;
  let heldHeight = 0;
  let knockPending = false;
  let knockCount = 0;

  function ensureTables(N) {
    if (tables.N === N) return;
    tables.N = N;
    const Q = N / 4;

    // The two bases in one texture: long in rows 0..N-1 (2N wide), short in
    // rows N..N+Q-1 (2Q wide, the rest zero). Double in, float out: the only
    // rounding a basis value ever gets.
    const data = new Float32Array(2 * N * (N + Q));
    const longBasis = basis(N);
    const shortBasis = basis(Q);
    for (let n = 0; n < 2 * N; n += 1) {
      for (let k = 0; k < N; k += 1) data[k * 2 * N + n] = longBasis[n * N + k];
    }
    for (let n = 0; n < 2 * Q; n += 1) {
      for (let k = 0; k < Q; k += 1) data[(N + k) * 2 * N + n] = shortBasis[n * Q + k];
    }
    gl.bindTexture(gl.TEXTURE_2D, basisTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, 2 * N, N + Q, 0, gl.RED, gl.FLOAT, data);
    gl.bindTexture(gl.TEXTURE_2D, null);

    tables.rampLong = new Float32Array(32);
    tables.rampShort = new Float32Array(8);
    const rl = ramp(N);
    const rs = ramp(Q);
    for (let j = 0; j < N; j += 1) tables.rampLong[j] = rl[j];
    for (let j = 0; j < Q; j += 1) tables.rampShort[j] = rs[j];

    tables.diagBfuLong = new Array(64).fill(0);
    tables.diagBfuShort = new Array(16).fill(0);
    for (let d = 0; d < 2 * N - 1; d += 1) tables.diagBfuLong[d] = bfuOfDiagonal(N, d);
    for (let d = 0; d < 2 * Q - 1; d += 1) tables.diagBfuShort[d] = bfuOfDiagonal(Q, d);
    tables.bfuLong = bfuCount(N);
    tables.bfuShort = bfuCount(Q);
    tables.countLong = new Array(16).fill(0);
    tables.countShort = new Array(16).fill(0);
    for (let j = 0; j < tables.bfuLong; j += 1) tables.countLong[j] = bfuCoefficients(N, j);
    for (let j = 0; j < tables.bfuShort; j += 1) tables.countShort[j] = 16 * bfuCoefficients(Q, j);

    tables.sfTable = new Float32Array(K_SCALE_FACTORS);
    for (let i = 0; i < K_SCALE_FACTORS; i += 1) tables.sfTable[i] = scaleFactor(i);

    tables.spreadUp = new Float32Array(16);
    tables.spreadDown = new Float32Array(16);
    for (let i = 0; i < 16; i += 1) {
      tables.spreadUp[i] = Math.pow(10, -K_SPREAD_UP_DB * i / 10);
      tables.spreadDown[i] = Math.pow(10, -K_SPREAD_DOWN_DB * i / 10);
    }
  }

  /// The lapped snippet's uniforms. Cells on unit 1, the basis on unit 2.
  function setLapped(program, f) {
    program.setSampler('Cells', 1);
    program.setSampler('Basis', 2);
    program.setInt('N', f.N);
    program.setInt('Bx', f.Bx);
    program.setInt('By', f.By);
    program.setInt('W', f.width);
    program.setInt('H', f.height);
    program.setInt('RectWindow', 0);
    program.setArray('RampLong', tables.rampLong);
    program.setArray('RampShort', tables.rampShort);
  }

  function setBfuTables(program) {
    setInts(program, 'DiagBfuLong', tables.diagBfuLong);
    setInts(program, 'DiagBfuShort', tables.diagBfuShort);
    program.setArray('SfTable', tables.sfTable);
  }

  function pass(target, program, bindings, set) {
    target.bind();
    program.use();
    bindings.forEach((texture, unit) => bindTexture(gl, unit, texture));
    set(program);
    quad.draw();
  }

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const p = (id) => params.get(id);
      const picture = input;
      const width = picture.width;
      const height = picture.height;

      clock.update(time);
      const now = clock.now;

      //------------------------------------------------------------------
      // The settings, in the coder's units.
      //------------------------------------------------------------------
      const f = {};
      f.width = width;
      f.height = height;
      f.N = blockSizeOf(optionIndex(p('blockSize'), K_BLOCK_SIZE_COUNT));
      f.mode = optionIndex(p('blockMode'), K_BLOCK_MODE_COUNT);
      f.Bx = Math.floor((width + f.N - 1) / f.N);
      f.By = Math.floor((height + f.N - 1) / f.N);
      const bpp = bitsPerPixel(p('bitRate'));
      f.budgetLuma = Math.floor(bpp * f.N * f.N);
      f.budgetChroma = Math.floor(bpp * f.N * f.N * chromaFraction(p('chromaBits')));
      f.masking = maskingAmount(p('masking'));

      //------------------------------------------------------------------
      // The disc. Knock is an event in the plugin: the toggle is read as one
      // rising edge and released.
      //------------------------------------------------------------------
      if (p('knock') > 0.5) {
        knockPending = true;
        knockCount += 1;
        params.set('knock', 0);
      }
      let dt = -1;
      if (audioClock >= 0) dt = Math.min(K_MAX_FRAME_DELTA, Math.max(0, now - audioClock));
      audioClock = now;
      const sensitivity = p('sensitivity');
      const margin = sensitivity <= 0 ? -1 : knockMargin(sensitivity);
      // No spectrum reaches this page: silence, as an unrouted input.
      const onset = onsets.update(null, 0, dt, margin);
      const knock = onset || knockPending;
      knockPending = false;
      const settings = {
        bufferSeconds: bufferSeconds(p('buffer')),
        readSpeed: readSpeed(p('readSpeed')),
        knockSeconds: knockSeconds(p('knockLength')),
      };
      settings.restartLevel = restartLevel(settings.bufferSeconds);
      buffer.step(Math.max(0, dt), knock, settings);
      const decoding = !buffer.muted || !heldValid;

      //------------------------------------------------------------------
      // Buffers and tables.
      //------------------------------------------------------------------
      const cw = f.Bx * f.N;
      const ch = f.By * f.N;
      converted.ensure(width, height, gl.RGBA32F);
      cells.ensure(f.Bx, f.By, gl.RGBA32F);
      if (decoding) {
        lapped.ensure(cw, height, gl.RGBA32F);
        coefficients.ensure(cw, ch, gl.RGBA32F);
        quantised.ensure(cw, ch, gl.RGBA32F);
        alloc.ensure(2 * f.Bx, 3 * f.By, gl.RGBA32F);
        // The held picture outlives a mute and is only re-ensured on a frame
        // about to rewrite it (photofinish's trap: a resize mid-mute must not
        // clear the frame the operator is looking at).
        held.ensure(width, height, gl.RGBA32F);
        if (heldWidth !== width || heldHeight !== height) {
          heldWidth = width;
          heldHeight = height;
        }
      }
      ensureTables(f.N);
      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 2. convert: Y Cb Cr.
      //------------------------------------------------------------------
      pass(converted, shaders.convert, [picture.texture], (s) => {
        s.setSampler('InputTexture', 0);
      });

      //------------------------------------------------------------------
      // 3. cells: long or short.
      //------------------------------------------------------------------
      pass(cells, shaders.cells, [converted.texture], (s) => {
        s.setSampler('Picture', 0);
        s.setInt('N', f.N);
        s.setInt('W', f.width);
        s.setInt('H', f.height);
        s.setInt('Mode', f.mode);
        s.set('Ratio', K_TRANSIENT_RATIO);
        s.set('FloorEnergy', K_TRANSIENT_FLOOR_PER_PIXEL * (f.N / 2) * f.N);
        s.setInt('ForceLong', 0);
      });

      if (decoding) {
        //----------------------------------------------------------------
        // 4. mdctx: converted -> lapped.
        //----------------------------------------------------------------
        pass(lapped, shaders.mdctx, [converted.texture, cells.texture, basisTexture], (s) => {
          s.setSampler('Source', 0);
          setLapped(s, f);
        });

        //----------------------------------------------------------------
        // 5. mdcty: lapped -> coefficients.
        //----------------------------------------------------------------
        pass(coefficients, shaders.mdcty, [lapped.texture, cells.texture, basisTexture], (s) => {
          s.setSampler('Source', 0);
          setLapped(s, f);
        });

        //----------------------------------------------------------------
        // 6. alloc: the model and the greedy, per cell and channel.
        //----------------------------------------------------------------
        pass(alloc, shaders.alloc, [coefficients.texture, cells.texture], (s) => {
          s.setSampler('Coefficients', 0);
          s.setSampler('Cells', 1);
          s.setInt('N', f.N);
          s.setInt('Bx', f.Bx);
          s.setInt('By', f.By);
          s.setInt('BudgetLuma', f.budgetLuma);
          s.setInt('BudgetChroma', f.budgetChroma);
          s.setInt('BfuLong', tables.bfuLong);
          s.setInt('BfuShort', tables.bfuShort);
          s.set('OffsetFactor', Math.pow(10, -maskingOffsetDb(f.masking) / 10));
          s.set('FloorEnergy', K_ABSOLUTE_FLOOR);
          s.setInt('OverBudget', 0);
          s.setInt('StopEarly', 0);
          s.setInt('IgnoreMasking', 0);
          setBfuTables(s);
          setInts(s, 'CountLong', tables.countLong);
          setInts(s, 'CountShort', tables.countShort);
          s.setArray('SpreadUp', tables.spreadUp);
          s.setArray('SpreadDown', tables.spreadDown);
        });

        //----------------------------------------------------------------
        // 7. quant.
        //----------------------------------------------------------------
        pass(quantised, shaders.quant, [coefficients.texture, cells.texture, alloc.texture], (s) => {
          s.setSampler('Coefficients', 0);
          s.setSampler('Cells', 1);
          s.setSampler('Alloc', 2);
          s.setInt('N', f.N);
          s.setInt('Bx', f.Bx);
          s.setInt('By', f.By);
          s.setInt('Bypass', 0);
          setBfuTables(s);
        });

        //----------------------------------------------------------------
        // 8. imdcty: quantised -> lapped (reused: mdctx's output is spent).
        //----------------------------------------------------------------
        pass(lapped, shaders.imdcty, [quantised.texture, cells.texture, basisTexture], (s) => {
          s.setSampler('Source', 0);
          setLapped(s, f);
        });

        //----------------------------------------------------------------
        // 9. imdctx: lapped -> held, back in RGB.
        //----------------------------------------------------------------
        pass(held, shaders.imdctx, [lapped.texture, cells.texture, basisTexture, picture.texture], (s) => {
          s.setSampler('Source', 0);
          s.setSampler('InputTexture', 3);
          setLapped(s, f);
        });
        heldValid = true;
      }

      //------------------------------------------------------------------
      // 10. display, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      shaders.display.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, held.texture);
      bindTexture(gl, 2, cells.texture);
      bindTexture(gl, 3, alloc.texture);
      shaders.display.setSampler('InputTexture', 0);
      shaders.display.setSampler('Held', 1);
      shaders.display.setSampler('Cells', 2);
      shaders.display.setSampler('Alloc', 3);
      shaders.display.setInt('N', f.N);
      shaders.display.setInt('Bx', f.Bx);
      shaders.display.setInt('By', f.By);
      shaders.display.setInt('VpX', 0);
      shaders.display.setInt('VpY', 0);
      shaders.display.setInt('VpW', vpW);
      shaders.display.setInt('VpH', vpH);
      shaders.display.set('MixAmount', p('mix'));
      shaders.display.setInt('ShowBlocks', p('showBlocks') >= 0.5 ? 1 : 0);
      shaders.display.setInt('ShowBits', p('showBits') >= 0.5 ? 1 : 0);
      setInts(shaders.display, 'DiagBfuLong', tables.diagBfuLong);
      setInts(shaders.display, 'DiagBfuShort', tables.diagBfuShort);
      quad.draw();

      for (let unit = 3; unit >= 1; unit -= 1) bindTexture(gl, unit, null);
      gl.activeTexture(gl.TEXTURE0);

      telemetry.N = f.N;
      telemetry.cells = f.Bx * f.By;
      telemetry.budgetLuma = f.budgetLuma;
      telemetry.budgetChroma = f.budgetChroma;
      telemetry.level = buffer.level;
      telemetry.capacity = settings.bufferSeconds;
      telemetry.muted = buffer.muted;
      telemetry.reading = buffer.stopLeft <= 0;
      telemetry.knocks = knockCount;
    },
  };
}

//===========================================================================
// The controls, read out of Atrac::Atrac(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the Audio FFT buffer
// (not a control a host draws, and no spectrum exists here) and the About
// block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const seconds = (s) => (s < 1 ? `${(s * 1000).toPrecision(3)} ms` : `${s.toPrecision(3)} s`);

const demo = mountDemo({
  name: 'Atrac',
  pluginId: 'AC01',
  kind: 'effect',
  tagline:
    'A perceptual coder modelled on MiniDisc’s ATRAC, run on the picture. Detail is thrown away by bits, not by resolution: the picture is cut into overlapping blocks, each goes through the lapped transform (the MDCT), the coefficients are grouped into block floating units with a scale factor and a word length each, and a fixed budget of bits per cell is shared out by a masking model. Fine texture goes first, a hard edge rings one block either side, the allocation shimmers frame to frame, and a starved cell falls to grey. A knock drains the shock-proof buffer; when it empties, the picture holds. The shaders here are the plugin’s own; the tables, the disc and the clock are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/atrac',
  page: 'https://stoatworks-labs.com/software/atrac/',

  blurb:
    'It is Atrac’s own GLSL, ported from the repository to WebGL2, with the tables it computes on the CPU, its shock-proof buffer and its clock ported to JavaScript by hand — nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install. No audio reaches it: Knock is the only knock here.',

  // Every buffer in the chain is RGBA32F, as in the plugin: the coefficients,
  // the packed allocation (integers below 2^23 in a float) and the held picture.
  needFloat: true,

  params: [
    std('bitRate', 'Bit Rate', 0.525, 'Coder', {
      display: (v) => `${bitsPerPixel(v).toPrecision(3)} bits/px`,
      hint: 'Bits per pixel, 0.05 to 4, logarithmic. The budget for a cell is the rate times its area, for luma; times Chroma Bits for each of Cb and Cr. On MiniDisc the budget was 212 bytes a sound unit, always; here it is this.',
    }),
    opt('blockSize', 'Block Size', ['8', '16', '32'], 1, 'Coder',
      'The cell is N × N. Along each axis a long cell is one lapped block of N; a short one is four of N/4. A bigger cell spreads its budget over more coefficients: smoother inside, wider ringing, a bigger grey square when it starves.'),
    opt('blockMode', 'Block Mode', ['Long', 'Short', 'Adaptive'], 2, 'Coder',
      'Long: one block per axis. Short: four, so a transient’s noise stays within a quarter of the cell. Adaptive: short where the energy of a cell’s first differences in one half is eight times the other’s, along x or y. The switch is exact — the overlap at a boundary is the smaller of the two blocks meeting there.'),
    std('masking', 'Masking', 0.6, 'Coder', {
      display: (v) => `${maskingOffsetDb(v).toFixed(0)} dB below the spread`,
      hint: 'Each band’s threshold is the largest spread contribution from any other band (10 dB per band up, 24 down), less this offset: 60 dB at 0, level at 1. A masked band goes to the back of the queue rather than being thrown away.',
    }),
    std('chromaBits', 'Chroma Bits', 0.25, 'Coder', {
      display: (v) => `${(chromaFraction(v) * 100).toFixed(0)} % of luma`,
      hint: 'Each of Cb and Cr’s share of the luma budget. At 0 the picture is grey; at 1 colour costs as much as luma.',
    }),

    std('buffer', 'Buffer', 0.2, 'Disc', {
      display: (v) => seconds(bufferSeconds(v)),
      hint: 'Seconds of playback the shock-proof buffer holds when full, 0 to 10. It starts full; playback drains it at 1x; the read fills it at Read Speed.',
    }),
    std('readSpeed', 'Read Speed', 1 / 3, 'Disc', {
      display: (v) => `${readSpeed(v).toFixed(2)}x`,
      hint: 'How fast the disc is read, 1x to 4x the play rate. Broken, nothing plays, so it refills at the full rate.',
    }),
    std('sensitivity', 'Sensitivity', 0.5, 'Disc', {
      display: (v) => (v <= 0 ? 'never knocks' : `margin ×${(1 + knockMargin(v)).toFixed(2)}`),
      hint: 'The onset detector’s threshold on the Audio input: the spectral flux against a running mean times a margin. 0 never knocks. No audio reaches a browser page, so on this page it does nothing.',
    }),
    std('knockLength', 'Knock Length', 0.5, 'Disc', {
      display: (v) => seconds(knockSeconds(v)),
      hint: 'How long a knock stops the read, 50 ms to 5 s, logarithmic. A knock during a knock extends it. A knock longer than Buffer breaks playback; the picture then holds until a quarter of a buffer has been read back.',
    }),
    bool('knock', 'Knock', 0, 'Disc',
      'The plugin’s Knock is an event: one press, one knock. The kit has no event control, so this toggle is released by the page after one frame. Press it with Knock Length longer than Buffer and watch the picture hold, then resume.'),

    bool('showBlocks', 'Show Blocks', 0, 'View',
      'Draws the cell grid, tints the cells coded short, and draws their sub-block grid.'),
    bool('showBits', 'Show Bits', 0, 'View',
      'Paints every cell as its own coefficient plane: each pixel shows the word length of the luma band its coefficient belongs to, as heat from blue (2 bits) to white (16); black is a band that got nothing. Dim at low rates, where most bands sit at 2 bits.'),
    std('mix', 'Mix', 1.0, 'View'),
  ],

  // Texture and hard edges are what a coder starves and rings.
  sources: ['scene', 'detail', 'bars', 'ramp', 'grid', 'spot'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the sliders.
  presets: {
    'Starved (a tenth of a bit)': { bitRate: 0.158 },
    'Pre-echo on long blocks': { bitRate: 0.684, blockSize: 2, blockMode: 0 },
    'Short blocks, same rate': { bitRate: 0.684, blockSize: 2, blockMode: 1 },
    'Adaptive, grid shown': { bitRate: 0.684, blockSize: 2, blockMode: 2, showBlocks: 1 },
    'Where the bits go': { showBits: 1, bitRate: 0.7 },
    'No colour': { chromaBits: 0 },
    'A disc that skips': { buffer: 0.05, knockLength: 0.65 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Atrac computes its tables — the lapped-transform basis, the window ramps, the BFU rule, ATRAC1’s scale factors, the spreading factors — on the CPU in double (Codec.cpp), steps its shock-proof buffer and onset detector in double (Disc.cpp), and keeps a clock (Clock.cpp); that is ported here function for function, because without it the shaders have no tables and the disc has no state. Nothing checks a port but a reader; the repository’s actest checks the C++ and has never heard of this page.',
    'The GPU half is not a port. The ten programs — convert, cells, the two forward and two inverse lapped passes, alloc, quant and display — are assembled from the plugin’s own GLSL snippets exactly as Shaders.cpp assembles them, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the twenty drifts.',
    'No audio reaches a browser page. The plugin’s Audio FFT buffer (64 bins from Resolume) is absent rather than present and dead; the ported onset detector is handed no spectrum every frame, which it reads exactly as the plugin reads an unrouted input: silence. Sensitivity is the plugin’s control and is present, but here it does nothing. Knock is the only knock.',
    'Knock is FF_TYPE_EVENT in the plugin. The kit has no event control, so it is a toggle the page releases after one frame — one press, one rising edge, which is what the plugin counts.',
    'The disc runs on the page’s clock with its unit declared as seconds, as the repository’s harness declares it. The plugin votes on Resolume’s clock unit over its first frames; that vote never runs here. Restart is treated as the plugin treats a scrub: the clock steps on one frame.',
    'The held picture is an RGBA32F buffer sampled linearly, as in the plugin, where the browser offers OES_texture_float_linear; where it does not, it is sampled nearest, which only shows while a hold is being scaled to a new raster. Every other buffer is nearest in both.',
    'The plugin’s numerical proof — the transform’s reconstruction bound, the budget accounting, pre-echo extent, SNR against rate, the masking pair, the disc’s closed form — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported disc computed.',
    'Render cost is real here too: four lapped passes of 2N taps per pixel. The plugin costs 16 ms a frame at 4K on an M4 Max; a browser tab at a large raster will be slower still. Choose a smaller resolution or Block Size 8 if it stutters.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported disc's own numbers — how
// full the buffer is, whether it is reading, whether playback is broken —
// and the coder's budget per cell. A held frame on a still clip is easy to
// read as nothing happening, and the budget is the whole effect. Skipped in
// embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const t = telemetry;
      if (!t.N) return;
      line.textContent =
        `${t.N}×${t.N} cells, ${t.cells.toLocaleString('en-GB')} of them: ${t.budgetLuma} bits each for luma, `
        + `${t.budgetChroma} for each of Cb and Cr. `
        + `Buffer ${t.level.toFixed(2)} of ${t.capacity.toFixed(2)} s, `
        + `${t.reading ? 'reading' : 'knocked'}, ${t.muted ? 'playback BROKEN: the picture is held' : 'playing'}. `
        + `${t.knocks} knock${t.knocks === 1 ? '' : 's'}.`;
    }, 250);
  }
}
