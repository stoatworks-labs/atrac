// The checks. Included by main.cpp after the plumbing; not a translation unit
// of its own. Everything a check compares against is STATED here.

namespace
{
//---------------------------------------------------------------------------
// The stated 1-D lapped transform, in double, with per-boundary overlaps:
// the rule the shaders implement, written independently of them. Used by
// --tables to prove the arithmetic reconstructs (TDAC through a mixture of
// long and short blocks) before any GPU is involved, and by --masking to
// synthesise a picture out of basis functions.
//---------------------------------------------------------------------------
struct Block1D
{
	int s, M, Ll, Lr, col;
};

/// The blocks along an axis of `cells` cells of N, given each cell's mode.
std::vector< Block1D > statedBlocks( int N, const std::vector< bool >& isShort )
{
	const int cells = static_cast< int >( isShort.size() );
	const int Q     = N / 4;
	std::vector< Block1D > out;
	for( int b = 0; b < cells; ++b )
	{
		const bool sh  = isShort[ b ];
		const bool shL = b > 0 && isShort[ b - 1 ];
		const bool shR = b + 1 < cells && isShort[ b + 1 ];
		if( !sh )
			out.push_back( { b * N, N, b == 0 ? 0 : ( shL ? Q : N ), b == cells - 1 ? 0 : ( shR ? Q : N ), b * N } );
		else
			for( int sub = 0; sub < 4; ++sub )
				out.push_back( { b * N + sub * Q, Q, sub > 0 ? Q : ( b == 0 ? 0 : Q ), sub < 3 ? Q : ( b == cells - 1 ? 0 : Q ), b * N + sub * Q } );
	}
	return out;
}

double statedWindow( int n, const Block1D& blk )
{
	const int p = n - blk.M / 2;
	if( p < -blk.Ll / 2 || p >= blk.M + blk.Lr / 2 )
		return 0.0;
	if( p < blk.Ll / 2 )
		return statedRamp( p + blk.Ll / 2, blk.Ll );
	if( p >= blk.M - blk.Lr / 2 )
		return statedRamp( blk.M + blk.Lr / 2 - 1 - p, blk.Lr );
	return 1.0;
}

/// Forward: `x` has cells x N samples (padded); returns the same number of
/// coefficients, block by block.
std::vector< double > statedForward( const std::vector< double >& x, int N, const std::vector< bool >& isShort )
{
	const std::vector< Block1D > blocks = statedBlocks( N, isShort );
	std::vector< double > X( x.size(), 0.0 );
	const int L = static_cast< int >( x.size() );
	for( const Block1D& blk : blocks )
		for( int k = 0; k < blk.M; ++k )
		{
			double acc = 0.0;
			for( int n = 0; n < 2 * blk.M; ++n )
			{
				const int pos = std::clamp( blk.s - blk.M / 2 + n, 0, L - 1 );
				acc += statedWindow( n, blk ) * statedBasis( n, k, blk.M ) * x[ pos ];
			}
			X[ blk.col + k ] = acc;
		}
	return X;
}

std::vector< double > statedInverse( const std::vector< double >& X, int N, const std::vector< bool >& isShort )
{
	const std::vector< Block1D > blocks = statedBlocks( N, isShort );
	std::vector< double > y( X.size(), 0.0 );
	const int L = static_cast< int >( X.size() );
	for( const Block1D& blk : blocks )
		for( int n = 0; n < 2 * blk.M; ++n )
		{
			const int pos  = blk.s - blk.M / 2 + n;
			const double w = statedWindow( n, blk );
			if( w == 0.0 || pos < 0 || pos >= L )
				continue;
			double acc = 0.0;
			for( int k = 0; k < blk.M; ++k )
				acc += statedBasis( n, k, blk.M ) * X[ blk.col + k ];
			y[ pos ] += w * acc;
		}
	return y;
}

//---------------------------------------------------------------------------
// --tables: the plugin's tables and laws against the statements. No GL.
//---------------------------------------------------------------------------
int runTables( bool quiet = false )
{
	int failures = 0;

	//The BFU rule, for every block size the plugin can use.
	for( int M : { 2, 4, 8, 16, 32 } )
	{
		const std::vector< int > stated = statedEdges( M );
		const std::vector< int > plugin = codec::BfuEdges( M );
		bool same                       = stated == plugin && codec::BfuCount( M ) == statedBfuCount( M );
		int total                       = 0;
		for( int j = 0; j < statedBfuCount( M ); ++j )
		{
			same &= codec::BfuCoefficients( M, j ) == statedBfuCoefficients( M, j );
			total += codec::BfuCoefficients( M, j );
		}
		for( int d = 0; d < 2 * M - 1; ++d )
			same &= codec::BfuOfDiagonal( M, d ) == statedBfuOf( M, d );
		failures += report( same && total == M * M, quiet, "tables: M=%2d has %2d BFUs partitioning %d coefficients, edges as stated", M,
		                    codec::BfuCount( M ), total );
	}

	//The scale-factor law and the lookup.
	{
		double worst = 0.0;
		for( int i = 0; i < codec::kScaleFactors; ++i )
			worst = std::max( worst, std::fabs( codec::ScaleFactor( i ) / statedScaleFactor( i ) - 1.0 ) );
		bool lookup = true;
		for( int t = 0; t < 400; ++t )
		{
			const double peak = std::pow( 2.0, -6.0 + 23.0 * t / 399.0 ) * ( 1.0 + 0.001 * ( t % 7 ) );
			int stated        = 63;
			for( int i = 0; i < 64; ++i )
				if( statedScaleFactor( i ) >= peak )
				{
					stated = i;
					break;
				}
			lookup &= codec::ScaleFactorIndex( peak ) == stated;
		}
		failures += report( worst < 1e-12 && lookup, quiet, "tables: 64 scale factors 2^((i-15)/3) (worst %.1e), lookup is the smallest not below the peak",
		                    worst );
	}

	//The ramps satisfy Princen-Bradley, and the plugin's basis is the stated one.
	{
		double worstPb = 0.0, worstBasis = 0.0;
		for( int L : { 2, 4, 8, 16, 32 } )
		{
			const std::vector< double > w = codec::Ramp( L );
			for( int j = 0; j < L; ++j )
			{
				worstPb = std::max( worstPb, std::fabs( w[ j ] * w[ j ] + w[ L - 1 - j ] * w[ L - 1 - j ] - 1.0 ) );
				worstPb = std::max( worstPb, std::fabs( w[ j ] - statedRamp( j, L ) ) );
			}
		}
		for( int M : { 2, 4, 8, 16, 32 } )
		{
			const std::vector< double > b = codec::Basis( M );
			for( int n = 0; n < 2 * M; ++n )
				for( int k = 0; k < M; ++k )
					worstBasis = std::max( worstBasis, std::fabs( b[ static_cast< size_t >( n ) * M + k ] - statedBasis( n, k, M ) ) );
		}
		failures += report( worstPb < 1e-12 && worstBasis < 1e-12, quiet, "tables: ramps are Princen-Bradley (worst %.1e), basis as stated (worst %.1e)", worstPb,
		                    worstBasis );
	}

	//The stated transform reconstructs through a mixture of long and short
	//blocks, in double: the arithmetic behind --tdac, with no GPU in it.
	{
		double worst = 0.0;
		for( int N : { 8, 16, 32 } )
		{
			const std::vector< bool > modes = { false, true, false, false, true, true, false };
			std::vector< double > x( modes.size() * N );
			for( size_t i = 0; i < x.size(); ++i )
				x[ i ] = hash01( 11u, static_cast< uint32_t >( i ), static_cast< uint32_t >( N ) ) - 0.5;
			const std::vector< double > y = statedInverse( statedForward( x, N, modes ), N, modes );
			for( size_t i = 0; i < x.size(); ++i )
				worst = std::max( worst, std::fabs( y[ i ] - x[ i ] ) );
		}
		failures += report( worst < 1e-12, quiet, "tables: the stated lapped transform reconstructs through long/short mixtures in double (worst %.1e)", worst );
	}

	//A rectangular window does not: the offline half of the tdac negative.
	{
		double worst = 0.0;
		const int N  = 16;
		const std::vector< bool > modes = { false, false, false };
		std::vector< double > x( modes.size() * N );
		for( size_t i = 0; i < x.size(); ++i )
			x[ i ] = hash01( 12u, static_cast< uint32_t >( i ), 0u ) - 0.5;
		//The same transform with every window value forced to 1.
		const std::vector< Block1D > blocks = statedBlocks( N, modes );
		std::vector< double > X( x.size(), 0.0 ), y( x.size(), 0.0 );
		for( const Block1D& blk : blocks )
			for( int k = 0; k < blk.M; ++k )
			{
				double acc = 0.0;
				for( int n = 0; n < 2 * blk.M; ++n )
					acc += statedBasis( n, k, blk.M ) * x[ std::clamp( blk.s - blk.M / 2 + n, 0, static_cast< int >( x.size() ) - 1 ) ];
				X[ blk.col + k ] = acc;
			}
		for( const Block1D& blk : blocks )
			for( int n = 0; n < 2 * blk.M; ++n )
			{
				const int pos = blk.s - blk.M / 2 + n;
				if( pos < 0 || pos >= static_cast< int >( x.size() ) )
					continue;
				double acc = 0.0;
				for( int k = 0; k < blk.M; ++k )
					acc += statedBasis( n, k, blk.M ) * X[ blk.col + k ];
				y[ pos ] += acc;
			}
		for( size_t i = 0; i < x.size(); ++i )
			worst = std::max( worst, std::fabs( y[ i ] - x[ i ] ) );
		failures += report( worst > 0.05, quiet, "tables: with a rectangular window the aliasing does not cancel (worst %.3f, expected order 0.1)", worst );
	}

	//The control laws.
	{
		double worst = 0.0;
		for( int t = 0; t <= 10; ++t )
		{
			const float v = t / 10.0f;
			worst         = std::max( worst, std::fabs( atrac::controls::BitsPerPixel( v ) / statedBpp( v ) - 1.0 ) );
			worst         = std::max( worst, std::fabs( atrac::controls::BufferSeconds( v ) - statedBuffer( v ) ) );
			worst         = std::max( worst, std::fabs( atrac::controls::ReadSpeed( v ) - statedReadSpeed( v ) ) );
			worst         = std::max( worst, std::fabs( atrac::controls::KnockSeconds( v ) / statedKnock( v ) - 1.0 ) );
			worst         = std::max( worst, std::fabs( atrac::controls::KnockMargin( v ) - statedMargin( v ) ) );
			worst         = std::max( worst, std::fabs( codec::MaskingOffsetDb( v ) - statedMaskingOffsetDb( v ) ) );
			worst         = std::max( worst, std::fabs( atrac::controls::RestartLevel( 10.0 * v ) - statedRestart( 10.0 * v ) ) );
		}
		bool sizes = codec::BlockSizeOf( 0 ) == 8 && codec::BlockSizeOf( 1 ) == 16 && codec::BlockSizeOf( 2 ) == 32;
		failures += report( worst < 1e-12 && sizes, quiet, "tables: control laws as stated (worst %.1e); Block Size is 8, 16, 32", worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames( bool quiet = false )
{
	Atrac plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;

	//The name the host reads: plugMain's info block, 16 bytes, not
	//null-terminated.
	const FFMixed info            = plugMain( FF_GET_INFO, FFMixed{ 0 }, 0 );
	const PluginInfoStruct* block = static_cast< const PluginInfoStruct* >( info.PointerValue );
	std::string name, id;
	if( block )
	{
		name.assign( block->PluginName, strnlen( block->PluginName, 16 ) );
		id.assign( block->PluginUniqueID, 4 );
	}
	return report( bad == 0 && name == "SW Atrac" && id == "AC01" && block && block->PluginType == FF_EFFECT, quiet,
	               "names: %zu parameters, unique and within 16 characters; the host reads '%s' / %s / %s", seen.size(), name.c_str(),
	               id.c_str(), block && block->PluginType == FF_EFFECT ? "effect" : "not an effect" );
}

//---------------------------------------------------------------------------
// The disc's closed form, stated. A knock at t0 of length L on a buffer of
// B seconds, full, read at r: playback breaks iff L > B, at t0 + B; it
// resumes when the read (back at t0 + L) has refilled to the restart level
// R = B / 4 at rate r: t0 + L + R / r. A knock landing on a buffer holding
// `level` breaks iff L > level, at t0 + level.
//---------------------------------------------------------------------------
struct Closed
{
	bool breaks;
	double muteAt;
	double resumeAt;
};

Closed closedForm( double level, double B, double r, double L, double t0 )
{
	Closed c;
	c.breaks   = L > level;
	c.muteAt   = t0 + level;
	c.resumeAt = t0 + L + statedRestart( B ) / r;
	return c;
}

//---------------------------------------------------------------------------
// --skipmodel: the plugin's Buffer, stepped a frame at a time, against the
// closed form. No GL. The tolerance is one frame: time arrives a frame at a
// time, so every event lands on the frame after it happened.
//---------------------------------------------------------------------------
int runSkipModel( int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double fps = 60.0;
	const int k0     = 30;
	struct Case
	{
		double B, r, L;
	};
	const Case cases[] = {
		{ 0.5, 2.0, 1.0 },   { 0.5, 2.0, 0.25 }, { 0.5, 4.0, 1.0 }, { 2.0, 1.5, 2.5 }, { 2.0, 1.5, 1.5 },
		{ 2.0, 4.0, 4.0 },   { 0.0, 2.0, 0.5 },  { 5.0, 2.0, 5.5 },
	};
	for( const Case& c : cases )
	{
		atrac::disc::Settings s;
		s.bufferSeconds = c.B;
		s.readSpeed     = c.r;
		s.knockSeconds  = c.L;
		s.restartLevel  = 0.25 * c.B;
		s.perturb       = perturb;
		atrac::disc::Buffer buffer;
		buffer.Reset( c.B );
		int firstMuted = -1, firstResumed = -1;
		const int frames = static_cast< int >( ( k0 / fps + c.L + c.B + 3.0 ) * fps );
		for( int k = 1; k <= frames; ++k )
		{
			buffer.Step( 1.0 / fps, k == k0, s );
			if( buffer.Muted() && firstMuted < 0 )
				firstMuted = k;
			if( firstMuted >= 0 && !buffer.Muted() && firstResumed < 0 )
				firstResumed = k;
		}
		const Closed expect = closedForm( c.B, c.B, c.r, c.L, k0 / fps );
		bool ok             = true;
		char detail[ 160 ];
		if( !expect.breaks )
		{
			ok = firstMuted < 0;
			std::snprintf( detail, sizeof detail, "never breaks: %s", firstMuted < 0 ? "held nothing" : "BROKE" );
		}
		else
		{
			const double muteFrame   = expect.muteAt * fps;
			const double resumeFrame = expect.resumeAt * fps;
			ok = firstMuted >= 0 && firstResumed >= 0 && std::fabs( firstMuted - muteFrame ) <= 1.0
			     && std::fabs( firstResumed - resumeFrame ) <= 1.0;
			std::snprintf( detail, sizeof detail, "mute at frame %d (closed form %.2f), resume at %d (closed form %.2f)", firstMuted, muteFrame,
			               firstResumed, resumeFrame );
		}
		failures += report( ok, quiet, "skipmodel: B=%.1f s r=%.1fx L=%.2f s: %s", c.B, c.r, c.L, detail );
	}

	//A second knock on a part-filled buffer: after a resume the buffer holds
	//R and fills at r - 1 while playing; a knock k frames later breaks after
	//R + ( r - 1 ) k / fps seconds.
	{
		const double B = 1.0, r = 2.0, L1 = 1.5, L2 = 1.0;
		atrac::disc::Settings s;
		s.bufferSeconds = B;
		s.readSpeed     = r;
		s.knockSeconds  = L1;
		s.restartLevel  = 0.25 * B;
		s.perturb       = perturb;
		atrac::disc::Buffer buffer;
		buffer.Reset( B );
		const Closed first    = closedForm( B, B, r, L1, k0 / fps );
		const int resumeFrame = static_cast< int >( std::ceil( first.resumeAt * fps ) );
		const int k1          = resumeFrame + 12;
		const double level    = std::min( B, statedRestart( B ) + ( r - 1.0 ) * 12.0 / fps );
		const Closed second   = closedForm( level, B, r, L2, k1 / fps );
		int secondMute        = -1;
		for( int k = 1; k <= k1 + 200; ++k )
		{
			if( k == k1 )
				s.knockSeconds = L2;
			buffer.Step( 1.0 / fps, k == k0 || k == k1, s );
			if( k > k1 && buffer.Muted() && secondMute < 0 )
				secondMute = k;
		}
		const bool ok = second.breaks && secondMute >= 0 && std::fabs( secondMute - second.muteAt * fps ) <= 1.0;
		failures += report( ok, quiet, "skipmodel: a second knock on a part-filled buffer (%.3f s) breaks at frame %d (closed form %.2f)", level,
		                    secondMute, second.muteAt * fps );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --prime: through the plugin's own frame path with no GL. A loud spectrum
// from the first frame must not knock; a real rise later must.
//---------------------------------------------------------------------------
int runPrime( int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( double origin : { 0.0, 40.0 } )
	{
		Atrac plugin;
		plugin.SetPerturbForTest( perturb );
		set( plugin, "Knock Sensitivity", 1.0f );
		set( plugin, "Buffer", 0.0f );//so a knock is a break, visible at once
		writeFlatSpectrum( plugin, 0.8f );
		const bool firstKnock = plugin.StepDiscForTest( origin );
		bool steadyKnock      = false;
		for( int k = 1; k <= 12; ++k )
			steadyKnock |= plugin.StepDiscForTest( origin + k / 60.0 );
		//Silence, then a rise: the detector is alive.
		writeFlatSpectrum( plugin, 0.0f );
		for( int k = 13; k <= 60; ++k )
			plugin.StepDiscForTest( origin + k / 60.0 );
		writeFlatSpectrum( plugin, 0.8f );
		const bool laterKnock = plugin.StepDiscForTest( origin + 61 / 60.0 );
		failures += report( !firstKnock && !steadyKnock && laterKnock, quiet,
		                    "prime: first SetTime %.0f s: frame one %s, steady loud %s, a rise after silence %s", origin,
		                    firstKnock ? "KNOCKED" : "quiet", steadyKnock ? "KNOCKED" : "quiet", laterKnock ? "knocks" : "DEAF" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// Rendered checks.
//---------------------------------------------------------------------------
constexpr double kU = 1.0 / 16777216.0;

/// The l2 bound on the reconstruction at unlimited bits. Every stage of the
/// transform is orthogonal, so the l2 norm of an error made at any stage
/// arrives at the output unchanged; each stage's rounding is a dot product
/// of 2M terms (plus the float rounding of the basis, the ramp and their
/// product: three more units), bounded per output by Cauchy-Schwarz and
/// summed over outputs with the overlap counted twice. The colour matrices
/// add their own three-term products, and the inverse matrix's Frobenius
/// norm bounds how it scales what it is handed. See AGENTS.md.
double tdacBound( int N, int Bx, int By, int W, int H )
{
	const double P      = static_cast< double >( Bx ) * N * By * N;//the padded plane
	const double xNorm  = 0.5 * std::sqrt( 3.0 * P );                //|Y-1/2|, |Cb|, |Cr| <= 1/2
	const double kappa  = ( 2.0 * N + 4.0 ) * kU * 1.01;
	const double stages = 4.0 * std::sqrt( 2.0 ) * kappa * xNorm * 1.01;
	const double gamma3 = 3.0 * kU * 1.01;
	const double inErr  = gamma3 * std::sqrt( 3.0 * P );
	const double inv[ 9 ] = { 1.0, 0.0, 1.402, 1.0, -0.344136, -0.714136, 1.0, 1.772, 0.0 };
	double frob           = 0.0;
	for( double a : inv )
		frob += a * a;
	frob                 = std::sqrt( frob );
	const double outErr  = 2.0 * gamma3 * std::sqrt( 3.0 * W * H );
	const double mixErr  = kU * std::sqrt( 3.0 * W * H );
	return frob * ( stages + inErr ) + outErr + mixErr;
}

int runTdac( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const char* const modeNames[] = { "Long", "Short", "Adaptive" };
	for( int option = 0; option < 3; ++option )
		for( int mode = 0; mode < 3; ++mode )
		{
			const int N  = blockSizeOfOption( option );
			const int Bx = ( W + N - 1 ) / N, By = ( H + N - 1 ) / N;
			Session s;
			Knobs k;
			k.blockSize = option;
			k.blockMode = mode;
			k.mix       = 1.0f;
			prepare( s, k, perturb | codec::kPerturbNoQuantise );
			if( !s.begin( W, H ) )
				return report( false, quiet, "tdac: no session" );
			const std::vector< float > in = texturedCard( W, H, 5u );
			s.render( 0, in );
			const std::vector< float > out = s.readBackFloat();
			double sum = 0.0, worst = 0.0;
			for( size_t i = 0; i < in.size(); i += 4 )
				for( int c = 0; c < 3; ++c )
				{
					const double e = static_cast< double >( out[ i + c ] ) - in[ i + c ];
					sum += e * e;
					worst = std::max( worst, std::fabs( e ) );
				}
			const double l2    = std::sqrt( sum );
			const double bound = tdacBound( N, Bx, By, W, H );
			bool mixed         = true;
			if( mode == codec::kAdaptive )
			{
				const Cells cells = readCells( s );
				int shortCount    = 0;
				for( int by = 0; by < cells.By; ++by )
					for( int bx = 0; bx < cells.Bx; ++bx )
						shortCount += cells.isShort( bx, by ) ? 1 : 0;
				mixed = shortCount > 0 && shortCount < cells.Bx * cells.By;
			}
			s.end();
			failures += report( l2 <= bound && mixed, quiet, "tdac: N=%2d %-8s l2 error %.2e against a bound of %.2e (%.3f of it), worst pixel %.1e%s", N,
			                    modeNames[ mode ], l2, bound, l2 / bound, worst, mixed ? "" : " -- adaptive picked no mixture" );
		}
	return failures;
}

//---------------------------------------------------------------------------
// The masking model and the greedy, stated in double, from a cell's
// coefficients. --budget compares the shader's accounting with this;
// --masking synthesises a picture the statement predicts.
//---------------------------------------------------------------------------
struct CellModel
{
	int nb = 0;
	std::vector< double > peak, energy, threshold;
	std::vector< int > count;
	std::vector< bool > eligible;
};

CellModel statedCellModel( const std::vector< float >& coef, int cw, int N, int bx, int by, bool sh, int ch, double maskingOffsetDb )
{
	CellModel m;
	const int M = sh ? N / 4 : N;
	m.nb        = statedBfuCount( M );
	m.peak.assign( m.nb, 0.0 );
	m.energy.assign( m.nb, 0.0 );
	m.count.assign( m.nb, 0 );
	for( int j = 0; j < m.nb; ++j )
		m.count[ j ] = ( sh ? 16 : 1 ) * statedBfuCoefficients( M, j );
	for( int j = 0; j < N; ++j )
		for( int i = 0; i < N; ++i )
		{
			const int d    = ( i % M ) + ( j % M );
			const int b    = statedBfuOf( M, d );
			const double c = coef[ ( static_cast< size_t >( by * N + j ) * cw + ( bx * N + i ) ) * 4 + ch ];
			m.peak[ b ]    = std::max( m.peak[ b ], std::fabs( c ) );
			m.energy[ b ] += c * c;
		}
	std::vector< double > E( m.nb );
	for( int j = 0; j < m.nb; ++j )
		E[ j ] = m.energy[ j ] / m.count[ j ];
	m.threshold.assign( m.nb, 0.0 );
	m.eligible.assign( m.nb, false );
	const double floorE = ( 0.5 / 255.0 ) * ( 0.5 / 255.0 );
	const double offset = std::pow( 10.0, -maskingOffsetDb / 10.0 );
	for( int j = 0; j < m.nb; ++j )
	{
		double spread = 0.0;
		for( int i = 0; i < m.nb; ++i )
		{
			if( i == j )
				continue;
			const double slope = i < j ? 10.0 : 24.0;
			spread             = std::max( spread, E[ i ] * std::pow( 10.0, -slope * std::abs( i - j ) / 10.0 ) );
		}
		m.threshold[ j ] = std::max( floorE, spread * offset );
		//In the queue at all: above the absolute floor. Its place in the
		//queue: E / T, below one for a masked band.
		m.eligible[ j ] = E[ j ] > floorE;
	}
	return m;
}

/// The stated greedy: the band with the highest E / T (divided by 16 for
/// its first two bits and by 4 for every bit after) takes the next
/// increment that fits; ties to the lowest band; until nothing fits.
std::vector< int > statedGreedy( const CellModel& m, int budget )
{
	std::vector< int > wl( m.nb, 0 );
	std::vector< double > ratio( m.nb, 0.0 );
	for( int j = 0; j < m.nb; ++j )
		ratio[ j ] = m.eligible[ j ] ? ( m.energy[ j ] / m.count[ j ] ) / m.threshold[ j ] : 0.0;
	int left = budget;
	for( ;; )
	{
		int best     = -1;
		double bestR = 0.0;
		for( int j = 0; j < m.nb; ++j )
		{
			if( ratio[ j ] <= 0.0 || wl[ j ] >= 16 )
				continue;
			const int cost = ( wl[ j ] == 0 ? 2 : 1 ) * m.count[ j ];
			if( cost <= left && ratio[ j ] > bestR )
			{
				bestR = ratio[ j ];
				best  = j;
			}
		}
		if( best < 0 )
			break;
		const int step = wl[ best ] == 0 ? 2 : 1;
		left -= step * m.count[ best ];
		wl[ best ] += step;
		ratio[ best ] *= step == 2 ? 1.0 / 16.0 : 0.25;
	}
	return wl;
}

int runBudget( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int option = 0; option < 3; ++option )
		for( double bpp : { 0.2, 0.5, 1.5 } )
		{
			const int N = blockSizeOfOption( option );
			Session s;
			Knobs k;
			k.blockSize = option;
			k.blockMode = codec::kAdaptive;
			k.bitRate   = sliderForBpp( bpp );
			k.masking   = 0.6f;
			k.chromaBits = 0.25f;
			prepare( s, k, perturb );
			if( !s.begin( W, H ) )
				return report( false, quiet, "budget: no session" );
			s.render( 0, texturedCard( W, H, 9u ) );
			int cw = 0, chh = 0;
			const std::vector< float > coef = s.coefficients( cw, chh );
			const Allocation alloc          = readAllocation( s );
			const Cells cells               = readCells( s );
			s.end();

			const double statedBppValue = statedBpp( k.bitRate );
			const int budgetLuma        = static_cast< int >( std::floor( statedBppValue * N * N ) );
			const int budgetChroma      = static_cast< int >( std::floor( statedBppValue * N * N * 0.25 ) );
			const double offsetDb       = statedMaskingOffsetDb( 0.6 );

			int cellsChecked = 0, overspent = 0, wasteful = 0, floorGotBits = 0, badWord = 0, badSf = 0, sfChecked = 0, sameAsStated = 0;
			double worstSpend = 0.0, worstWaste = 0.0;
			for( int by = 0; by < cells.By; ++by )
				for( int bx = 0; bx < cells.Bx; ++bx )
				{
					const bool sh = cells.isShort( bx, by );
					for( int ch = 0; ch < 3; ++ch )
					{
						const CellModel m = statedCellModel( coef, cw, N, bx, by, sh, ch, offsetDb );
						const int budget  = ch == 0 ? budgetLuma : budgetChroma;
						const std::vector< int > stated = statedGreedy( m, budget );
						int cost          = 0;
						int minIncrement  = -1;
						bool same         = true;
						const double floorE = ( 0.5 / 255.0 ) * ( 0.5 / 255.0 );
						for( int j = 0; j < m.nb; ++j )
						{
							const auto [ wl, sf ] = alloc.of( bx, by, ch, j );
							if( wl == 1 || wl > 16 )
								++badWord;
							cost += wl * m.count[ j ];
							same &= wl == stated[ j ];
							//A band clearly below the absolute floor must be zero. Clearly: a
							//tenth of a dB clear of it, so a float/double disagreement at the
							//boundary is not read as a silent band being coded.
							const double margin = m.energy[ j ] / m.count[ j ] / floorE;
							if( margin < std::pow( 10.0, -0.01 ) && wl > 0 )
								++floorGotBits;
							if( margin > std::pow( 10.0, 0.01 ) && wl < 16 )
							{
								const int inc = ( wl == 0 ? 2 : 1 ) * m.count[ j ];
								minIncrement  = minIncrement < 0 ? inc : std::min( minIncrement, inc );
							}
							//The scale factor: the smallest entry not below the peak, unless
							//the peak sits within a float of an entry.
							int stated = 63;
							for( int i = 0; i < 64; ++i )
								if( statedScaleFactor( i ) >= m.peak[ j ] )
								{
									stated = i;
									break;
								}
							const bool nearEdge = std::fabs( statedScaleFactor( stated ) / std::max( m.peak[ j ], 1e-30 ) - 1.0 ) < 1e-5
							                      || ( stated > 0 && std::fabs( statedScaleFactor( stated - 1 ) / std::max( m.peak[ j ], 1e-30 ) - 1.0 ) < 1e-5 );
							if( m.peak[ j ] > 0.0 && !nearEdge )
							{
								++sfChecked;
								if( sf != stated )
									++badSf;
							}
						}
						++cellsChecked;
						sameAsStated += same ? 1 : 0;
						worstSpend = std::max( worstSpend, static_cast< double >( cost ) / std::max( budget, 1 ) );
						if( cost > budget )
							++overspent;
						if( minIncrement > 0 )
						{
							worstWaste = std::max( worstWaste, static_cast< double >( budget - cost ) / minIncrement );
							if( budget - cost >= minIncrement )
								++wasteful;
						}
					}
				}
			//The pass/fail is the accounting, read from the shader's own allocation.
			//The stated greedy's agreement is reported, not asserted: two near-equal
			//ratios can order differently in float and in double, and a swap between
			//them is a different allocation of the same budget, not a wrong one.
			const bool ok = overspent == 0 && wasteful == 0 && floorGotBits == 0 && badWord == 0 && badSf == 0;
			failures += report( ok, quiet,
			                    "budget: N=%2d %.1f bpp (%d/%d bits): %d cell-channels, %d over budget (worst %.3f of it), %d wasted an increment (worst %.2f of one), %d silent bands coded, %d bad word lengths, %d/%d scale factors off; %d identical to the stated greedy",
			                    N, bpp, budgetLuma, budgetChroma, cellsChecked, overspent, worstSpend, wasteful, worstWaste, floorGotBits, badWord, badSf, sfChecked, sameAsStated );
		}
	return failures;
}

//---------------------------------------------------------------------------
// --preecho: how far before a step its quantisation noise reaches. The
// picture is mid grey up to the edge and bright from it. Mid grey is Y = 0
// exactly, so every block whose support holds only grey has every
// coefficient at zero and decodes to grey exactly: left of the edge the
// only error there can be is noise from a block that saw the step. That is
// pre-echo, and its extent is a property of the window: a long block whose
// support holds the edge reaches N before it; a short block 3N/8. The
// right of the edge is not measured -- a bright flat field is not a single
// coefficient of an MLT and its own ripple would drown the measurement.
//---------------------------------------------------------------------------
int preechoExtent( const std::vector< float >& in, const std::vector< float >& out, int W, int H, int edge )
{
	//One 8-bit code: the smallest difference a display could show.
	const double tau = 1.0 / 255.0;
	int extent       = 0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < edge; ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * W + x ) * 4;
			if( std::fabs( static_cast< double >( out[ i ] ) - in[ i ] ) > tau )
				extent = std::max( extent, edge - x );
		}
	return extent;
}

int runPreecho( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int option = 0; option < 3; ++option )
	{
		const int N      = blockSizeOfOption( option );
		const int Bx     = ( W + N - 1 ) / N;
		const int bxEdge = Bx / 2;
		const int edge   = bxEdge * N + N / 2;
		const std::vector< float > in = stepCard( W, H, edge, 0.5, 0.85 );
		int ext[ 3 ] = { 0, 0, 0 };
		bool adaptiveShortAtEdge = false, adaptiveLongBeside = false;
		for( int mode = 0; mode < 3; ++mode )
		{
			Session s;
			Knobs k;
			k.blockSize = option;
			k.blockMode = mode;
			k.bitRate   = sliderForBpp( 0.15 );
			prepare( s, k, perturb );
			if( !s.begin( W, H ) )
				return report( false, quiet, "preecho: no session" );
			s.render( 0, in );
			ext[ mode ] = preechoExtent( in, s.readBackFloat(), W, H, edge );
			if( mode == codec::kAdaptive )
			{
				const Cells cells   = readCells( s );
				adaptiveShortAtEdge = true;
				adaptiveLongBeside  = true;
				for( int by = 0; by < cells.By; ++by )
				{
					adaptiveShortAtEdge &= cells.isShort( bxEdge, by );
					adaptiveLongBeside &= !cells.isShort( bxEdge - 1, by ) && !cells.isShort( bxEdge + 1, by );
				}
			}
			s.end();
		}
		const int longBound  = N;
		const int shortBound = 3 * N / 8;
		const bool ok = ext[ 0 ] <= longBound && ext[ 1 ] <= shortBound && ext[ 2 ] <= shortBound && ext[ 0 ] > ext[ 1 ]
		                && adaptiveShortAtEdge && adaptiveLongBeside;
		failures += report( ok, quiet,
		                    "preecho: N=%2d reaches %d px before the edge in Long (bound %d), %d in Short (bound %d), %d in Adaptive (bound %d); adaptive cells at the edge %s, beside it %s",
		                    N, ext[ 0 ], longBound, ext[ 1 ], shortBound, ext[ 2 ], shortBound, adaptiveShortAtEdge ? "short" : "NOT SHORT",
		                    adaptiveLongBeside ? "long" : "NOT LONG" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --snr
//---------------------------------------------------------------------------
int runSnr( int W, int H, int perturb = 0, bool quiet = false )
{
	const std::vector< float > in = rateCard( W, H );
	double signal                 = 0.0;
	for( size_t i = 0; i < in.size(); i += 4 )
		for( int c = 0; c < 3; ++c )
			signal += static_cast< double >( in[ i + c ] ) * in[ i + c ];
	std::vector< double > snr;
	std::string detail;
	for( double bpp : { 0.1, 0.2, 0.4, 0.8, 1.6, 3.2 } )
	{
		Session s;
		Knobs k;
		k.bitRate = sliderForBpp( bpp );
		prepare( s, k, perturb );
		if( !s.begin( W, H ) )
			return report( false, quiet, "snr: no session" );
		s.render( 0, in );
		const std::vector< float > out = s.readBackFloat();
		s.end();
		double noise = 0.0;
		for( size_t i = 0; i < in.size(); i += 4 )
			for( int c = 0; c < 3; ++c )
			{
				const double e = static_cast< double >( out[ i + c ] ) - in[ i + c ];
				noise += e * e;
			}
		snr.push_back( 10.0 * std::log10( signal / std::max( noise, 1e-30 ) ) );
		char buf[ 48 ];
		std::snprintf( buf, sizeof buf, " %.1f bpp: %.2f dB", bpp, snr.back() );
		detail += buf;
	}
	//Strictly rising: the claim is monotonic, and the same allocation gives
	//the same picture to the bit, so "equal" is a real failure (the Bit Rate
	//ignored negative) and not noise.
	bool rising      = true;
	double worstStep = 1e9;
	for( size_t i = 1; i < snr.size(); ++i )
	{
		worstStep = std::min( worstStep, snr[ i ] - snr[ i - 1 ] );
		rising &= snr[ i ] > snr[ i - 1 ];
	}
	return report( rising, quiet, "snr: %s -- strictly rising, smallest step per doubling %.2f dB", detail.c_str(), worstStep );
}

//---------------------------------------------------------------------------
// --masking: a picture made of two basis functions of one long block, so
// the cell's coefficients are known exactly: a strong one in BFU 3 and a
// weak one 24 dB down in BFU 4, the band beside it. On a budget of 48 bits
// (0.19 bpp x 256): with Masking on the weak band's place in the queue is
// E/T = -18 dB, the strong band is driven down to it at 10 bits (40 of the
// 48), and the weak band's first increment (2 bits x 11 coefficients = 22)
// no longer fits, so it is zeroed. With Masking off the weak band's E/T is
// +9 dB, the strong band reaches that at 5 bits (20), and the weak band's
// 22 fit. Both from the statement above, before the shader was asked.
//---------------------------------------------------------------------------
int runMasking( int W, int H, int perturb = 0, bool quiet = false )
{
	const int N  = 16;
	const int Bx = ( W + N - 1 ) / N, By = ( H + N - 1 ) / N;
	if( Bx < 3 || By < 3 )
		return report( false, quiet, "masking: the raster is too small for an interior cell" );
	const int bx = Bx / 2, by = By / 2;
	const double A = 0.3, B = A * std::pow( 10.0, -24.0 / 20.0 );

	//The picture, in GL orientation: grey, plus the two functions in cell
	//(bx, by), whose window is the plain sine window (interior, long).
	std::vector< float > pic( static_cast< size_t >( W ) * H * 4 );
	auto p1 = []( int pos, int s, int k, int M ) {
		const int n = pos - ( s - M / 2 );
		if( n < 0 || n >= 2 * M )
			return 0.0;
		return statedSineWindow( n, M ) * statedBasis( n, k, M );
	};
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double v = 0.5 + p1( y, by * N, 0, N ) * ( A * p1( x, bx * N, 3, N ) + B * p1( x, bx * N, 4, N ) );
			float* px      = pic.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( v );
			px[ 3 ] = 1.0f;
		}

	int failures = 0;
	for( float masking : { 1.0f, 0.0f } )
	{
		Session s;
		Knobs k;
		k.blockSize = 1;
		k.blockMode = codec::kLong;
		k.bitRate   = sliderForBpp( 0.19 );
		k.masking   = masking;
		prepare( s, k, perturb );
		if( !s.begin( W, H ) )
			return report( false, quiet, "masking: no session" );
		s.renderGL( 0, pic );
		int cw = 0, chh = 0;
		const std::vector< float > coef = s.coefficients( cw, chh );
		const Allocation alloc          = readAllocation( s );
		s.end();
		const double c3 = coef[ ( static_cast< size_t >( by * N ) * cw + bx * N + 3 ) * 4 ];
		const double c4 = coef[ ( static_cast< size_t >( by * N ) * cw + bx * N + 4 ) * 4 ];
		const auto [ wl3, sf3 ] = alloc.of( bx, by, 0, 3 );
		const auto [ wl4, sf4 ] = alloc.of( bx, by, 0, 4 );
		const bool coefficientsAsPlanted = std::fabs( c3 - A ) < 1e-4 && std::fabs( c4 - B ) < 1e-4;
		const int budget = static_cast< int >( std::floor( statedBpp( k.bitRate ) * N * N ) );
		const bool ok = coefficientsAsPlanted && budget == 48 && wl3 > 0 && ( masking > 0.5f ? wl4 == 0 : wl4 > 0 );
		failures += report( ok, quiet, "masking: Masking %s, %d bits: coefficient (3,0) = %.4f (planted %.4f), (4,0) = %.5f (planted %.5f); BFU 3 gets %d bits, BFU 4 gets %d (%s)",
		                    masking > 0.5f ? "on " : "off", budget, c3, A, c4, B, wl3, wl4, masking > 0.5f ? "must be 0" : "must be > 0" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --skip: read out of the picture. A held frame is one that did not change
// while the input did; the moving card changes every frame.
//---------------------------------------------------------------------------
struct HeldRun
{
	int first = -1, last = -1, runs = 0;
};

HeldRun heldFrames( const std::vector< bool >& held )
{
	HeldRun r;
	for( size_t k = 0; k < held.size(); ++k )
	{
		if( held[ k ] && ( k == 0 || !held[ k - 1 ] ) )
			++r.runs;
		if( held[ k ] )
		{
			if( r.first < 0 )
				r.first = static_cast< int >( k );
			r.last = static_cast< int >( k );
		}
	}
	return r;
}

/// Held if the output moved by less than a few float ulps between frames
/// (the mix at 1.0 may not return the held texel bit-exactly); playing if
/// the moving bar moved it by a lot.
bool isHeld( const std::vector< float >& a, const std::vector< float >& b )
{
	double worst = 0.0;
	for( size_t i = 0; i < a.size(); ++i )
		worst = std::max( worst, std::fabs( static_cast< double >( a[ i ] ) - b[ i ] ) );
	return worst < 1e-5;
}

int runSkip( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Case
	{
		double B, r, L;
	};
	const int k0       = 12;
	const Case cases[] = { { 0.5, 2.0, 1.0 }, { 0.5, 2.0, 0.25 }, { 0.5, 4.0, 1.0 } };
	for( const Case& c : cases )
	{
		Session s;
		Knobs k;
		k.buffer      = sliderForBuffer( c.B );
		k.readSpeed   = sliderForReadSpeed( c.r );
		k.knockLength = sliderForKnock( c.L );
		k.sensitivity = 0.0f;
		prepare( s, k, perturb );
		if( !s.begin( W, H ) )
			return report( false, quiet, "skip: no session" );
		const Closed expect = closedForm( c.B, c.B, c.r, c.L, k0 / 60.0 );
		const int frames    = static_cast< int >( expect.resumeAt * 60.0 ) + 20;
		std::vector< bool > held( frames, false );
		std::vector< float > previous;
		for( int f = 0; f < frames; ++f )
		{
			if( f == k0 )
				knock( s.plugin );
			s.render( f, movingCard( W, H, f ) );
			std::vector< float > out = s.readBackFloat();
			if( f > 0 )
				held[ f ] = isHeld( previous, out );
			previous = std::move( out );
		}
		s.end();
		const HeldRun run = heldFrames( held );
		bool ok;
		char detail[ 200 ];
		if( !expect.breaks )
		{
			ok = run.first < 0;
			std::snprintf( detail, sizeof detail, "never breaks: %s", run.first < 0 ? "no frame held" : "HELD" );
		}
		else
		{
			//The first held frame is the one after the break (it repeats the
			//last decoded one); the last held frame is the one before the
			//resume. One frame of slack either way: time arrives a frame at a
			//time.
			const double muteFrame   = expect.muteAt * 60.0 + 1.0;
			const double resumeFrame = expect.resumeAt * 60.0;
			ok = run.runs == 1 && std::fabs( run.first - muteFrame ) <= 1.0 && std::fabs( run.last - ( resumeFrame - 1.0 ) ) <= 1.0;
			std::snprintf( detail, sizeof detail, "held frames %d..%d in %d run(s); closed form: first held %.2f, last held %.2f", run.first, run.last,
			               run.runs, muteFrame, resumeFrame - 1.0 );
		}
		failures += report( ok, quiet, "skip: B=%.2f s r=%.0fx L=%.2f s knock at frame %d: %s", c.B, c.r, c.L, k0, detail );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize: a resize in the middle of a mute keeps the held picture (shown
// scaled) and the disc's state.
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb = 0, bool quiet = false )
{
	const double B = 0.5, r = 2.0, L = 2.0;
	const int k0   = 12;
	Session s;
	Knobs k;
	k.buffer      = sliderForBuffer( B );
	k.readSpeed   = sliderForReadSpeed( r );
	k.knockLength = sliderForKnock( L );
	k.sensitivity = 0.0f;
	prepare( s, k, perturb );
	if( !s.begin( W, H ) )
		return report( false, quiet, "resize: no session" );
	const Closed expect = closedForm( B, B, r, L, k0 / 60.0 );
	const int resizeAt  = static_cast< int >( expect.muteAt * 60.0 ) + 15;
	const int frames    = static_cast< int >( expect.resumeAt * 60.0 ) + 20;
	const int W2 = W + W / 2, H2 = H + H / 2;
	std::vector< bool > held( frames, false );
	std::vector< float > previous;
	double meanBefore = 0.0, meanAfter = 0.0;
	bool mutedAtResize = false;
	for( int f = 0; f < frames; ++f )
	{
		if( f == k0 )
			knock( s.plugin );
		if( f == resizeAt )
		{
			//The picture on show just before, for its mean.
			for( size_t i = 0; i < previous.size(); i += 4 )
				meanBefore += 0.299 * previous[ i ] + 0.587 * previous[ i + 1 ] + 0.114 * previous[ i + 2 ];
			meanBefore /= static_cast< double >( W ) * H;
			mutedAtResize = s.plugin.MutedForTest();
			s.resize( W2, H2 );
		}
		const int w = f < resizeAt ? W : W2, h = f < resizeAt ? H : H2;
		s.render( f, movingCard( w, h, f ) );
		std::vector< float > out = s.readBackFloat();
		if( f == resizeAt )
		{
			for( size_t i = 0; i < out.size(); i += 4 )
				meanAfter += 0.299 * out[ i ] + 0.587 * out[ i + 1 ] + 0.114 * out[ i + 2 ];
			meanAfter /= static_cast< double >( W2 ) * H2;
		}
		if( f > 0 && f != resizeAt )
			held[ f ] = isHeld( previous, out );
		previous = std::move( out );
	}
	s.end();
	const HeldRun run = heldFrames( held );
	//The mean of a bilinearly resampled picture differs from the original's
	//by at most the half texel of picture that each edge gains or loses:
	//range x ( 1/W + 1/H ).
	const double meanBound   = 1.0 / W + 1.0 / H;
	const double resumeFrame = expect.resumeAt * 60.0;
	const bool ok = mutedAtResize && std::fabs( meanAfter - meanBefore ) <= meanBound && run.first > 0 && std::fabs( run.last - ( resumeFrame - 1.0 ) ) <= 1.0;
	return report( ok, quiet, "resize: %dx%d -> %dx%d at frame %d while %s; held mean %.4f before, %.4f after (bound %.4f); last held frame %d (closed form %.2f)",
	               W, H, W2, H2, resizeAt, mutedAtResize ? "muted" : "NOT MUTED", meanBefore, meanAfter, meanBound, run.last, resumeFrame - 1.0 );
}

//---------------------------------------------------------------------------
// Negative controls.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-64s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegativeOffline()
{
	return summariseNegatives( {
		{ "skipmodel: a knock empties the buffer at once", caught( [] { return runSkipModel( codec::kPerturbBufferIgnored, true ); } ) },
		{ "skipmodel: a muted disc refills at 1x", caught( [] { return runSkipModel( codec::kPerturbNoRefill, true ); } ) },
		{ "prime: the detector starts from silence", caught( [] { return runPrime( codec::kPerturbNoPrime, true ); } ) },
	} );
}

int runNegative( int W, int H )
{
	return summariseNegatives( {
		{ "tdac: a rectangular window (the spec's)", caught( [ & ] { return runTdac( W, H, codec::kPerturbRectWindow, true ); } ) },
		{ "budget: the shader spends 5/4 of the budget", caught( [ & ] { return runBudget( W, H, codec::kPerturbOverBudget, true ); } ) },
		{ "budget: the greedy stops at the first increment that does not fit", caught( [ & ] { return runBudget( W, H, codec::kPerturbStopEarly, true ); } ) },
		{ "preecho: short blocks are really long", caught( [ & ] { return runPreecho( W, H, codec::kPerturbShortIsLong, true ); } ) },
		{ "snr: Bit Rate ignored", caught( [ & ] { return runSnr( W, H, codec::kPerturbBitsIgnored, true ); } ) },
		{ "masking: allocation that ignores masking (the spec's)", caught( [ & ] { return runMasking( W, H, codec::kPerturbIgnoreMasking, true ); } ) },
		{ "skip: a knock empties the buffer at once", caught( [ & ] { return runSkip( W, H, codec::kPerturbBufferIgnored, true ); } ) },
		{ "skip: a muted disc refills at 1x", caught( [ & ] { return runSkip( W, H, codec::kPerturbNoRefill, true ); } ) },
		{ "resize: a resize clears the held picture", caught( [ & ] { return runResize( W, H, codec::kPerturbResizeClears, true ); } ) },
	} );
}

} // namespace
