#include "Atrac.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace atrac;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Atrac >,// Create method
	"AC01",                // Plugin unique ID of maximum length 4.
	"SW Atrac",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"MiniDisc's perceptual coder, run on the picture.\n\nThe picture is cut into overlapping blocks, each goes through a lapped transform (the MDCT), the coefficients are grouped into block floating units with a scale factor and a word length, and a fixed bit budget per block is shared out by a masking model. Fine texture goes first, hard edges ring one block either side (pre-echo), and the allocation shimmers from frame to frame. A knock -- an audio onset, or the button -- stops the disc read; when the shock-proof buffer empties, the picture holds until it refills.\n\nStart with Bit Rate low and Show Bits on to watch the budget being spent.",// Plugin description
	"Atrac FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kBlockSizeNames[ codec::kBlockSizeCount ] = { "8", "16", "32" };
const char* const kBlockModeNames[ codec::kBlockModeCount ] = { "Long", "Short", "Adaptive" };

/// The audio followers' clock cannot believe a delta longer than this: a
/// stall or a scrub is not half a minute of silence.
constexpr double kMaxFrameDelta = 0.5;

} // namespace

//---------------------------------------------------------------------------
Atrac::Atrac()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The disc runs in real time. It has to be the host's time, so an export
	//skips the same as the preview.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: half a bit per pixel on 16-pixel blocks with adaptive
	// switching -- texture visibly starved, edges ringing -- a moderate
	// masking model, a quarter of the budget to each chroma channel; a two
	// second buffer read at twice the play rate, half-second knocks on a
	// middling onset threshold.
	//
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_BIT_RATE ]          = 0.525f;//0.5 bits per pixel
	params[ PT_BLOCK_SIZE ]        = 1.0f;  //16
	params[ PT_BLOCK_MODE ]        = static_cast< float >( codec::kAdaptive );
	params[ PT_MASKING ]           = 0.6f;
	params[ PT_CHROMA_BITS ]       = 0.25f;
	params[ PT_BUFFER ]            = 0.2f;//2 s
	params[ PT_READ_SPEED ]        = 1.0f / 3.0f;//2x
	params[ PT_KNOCK_SENSITIVITY ] = 0.5f;
	params[ PT_KNOCK_LENGTH ]      = 0.5f;//0.5 s
	params[ PT_KNOCK ]             = 0.0f;
	params[ PT_SHOW_BLOCKS ]       = 0.0f;
	params[ PT_SHOW_BITS ]         = 0.0f;
	params[ PT_MIX ]               = 1.0f;

	SetParamInfof( PT_BIT_RATE, "Bit Rate", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_BLOCK_SIZE, "Block Size", codec::kBlockSizeCount, params[ PT_BLOCK_SIZE ] );
	for( int i = 0; i < codec::kBlockSizeCount; ++i )
		SetParamElementInfo( PT_BLOCK_SIZE, static_cast< unsigned int >( i ), kBlockSizeNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_BLOCK_MODE, "Block Mode", codec::kBlockModeCount, params[ PT_BLOCK_MODE ] );
	for( int i = 0; i < codec::kBlockModeCount; ++i )
		SetParamElementInfo( PT_BLOCK_MODE, static_cast< unsigned int >( i ), kBlockModeNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_MASKING, "Masking", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHROMA_BITS, "Chroma Bits", FF_TYPE_STANDARD );

	SetParamInfof( PT_BUFFER, "Buffer", FF_TYPE_STANDARD );
	SetParamInfof( PT_READ_SPEED, "Read Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_KNOCK_SENSITIVITY, "Sensitivity", FF_TYPE_STANDARD );
	SetParamInfof( PT_KNOCK_LENGTH, "Knock Length", FF_TYPE_STANDARD );
	//An event, which the host draws as a button: one rising edge per press.
	SetParamInfo( PT_KNOCK, "Knock", FF_TYPE_EVENT, false );
	//An FFT buffer: Resolume shows it as an audio-source picker and writes
	//one spectrum bin per element. Element defaults are zero on purpose --
	//with no audio routed nothing ever knocks.
	SetBufferParamInfo( PT_AUDIO, "Audio", disc::kBins, FF_USAGE_FFT );
	for( int i = 0; i < disc::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, static_cast< unsigned int >( i ), "", 0.0f );

	SetParamInfo( PT_SHOW_BLOCKS, "Show Blocks", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_SHOW_BITS, "Show Bits", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_BIT_RATE; i <= PT_CHROMA_BITS; ++i )
		SetParamGroup( i, "Coder" );
	for( FFUInt32 i = PT_BUFFER; i <= PT_AUDIO; ++i )
		SetParamGroup( i, "Disc" );
	for( FFUInt32 i = PT_SHOW_BLOCKS; i <= PT_MIX; ++i )
		SetParamGroup( i, "View" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Atrac effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Atrac::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const std::string* fragment;
		const char* name;
	} const stages[] = {
		{ &convertShader, &shaders::Convert(), "convert" },
		{ &cellsShader, &shaders::Cells(), "cells" },
		{ &mdctXShader, &shaders::MdctX(), "mdctx" },
		{ &mdctYShader, &shaders::MdctY(), "mdcty" },
		{ &allocShader, &shaders::Alloc(), "alloc" },
		{ &quantShader, &shaders::Quant(), "quant" },
		{ &imdctYShader, &shaders::ImdctY(), "imdcty" },
		{ &imdctXShader, &shaders::ImdctX(), "imdctx" },
		{ &displayShader, &shaders::Display(), "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::Vertex(), *stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Atrac: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &basisTexture );
	tablesFor = 0;

	//A clip trigger: the disc starts full, the detector primes on the first
	//spectrum, nothing is held. A Knock pressed before this is NOT cleared:
	//a press is a press, and it lands on the first frame (which is also how
	//the harness's `--set "Knock=1"` presses the button on frame 0).
	buffer.Reset( controls::BufferSeconds( params[ PT_BUFFER ] ) );
	onsets.Reset();
	audioClock  = -1.0;
	heldValid   = false;
	decodedOnce = false;
	heldWidth = heldHeight = 0;
	lastKnock = false;
	clock.Reset();

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Atrac::readAudio( float bins[ disc::kBins ], int& count )
{
	count = 0;
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;
	count      = static_cast< int >( std::min< size_t >( info->elements.size(), disc::kBins ) );
	float peak = 0.0f;
	for( int i = 0; i < count; ++i )
	{
		bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		peak      = std::max( peak, bins[ i ] );
	}
	//Once, when the first non-zero spectrum arrives. Whether an audio source
	//is routed at all is the first question anyone asks when the knock seems
	//deaf, and nothing else in FFGL answers it.
	if( !audioSeen && peak > 0.0f )
	{
		audioSeen = true;
		diag::info( "audio input active: " + std::to_string( count ) + " bins, peak " + std::to_string( peak ) );
	}
}

bool Atrac::stepDisc( double now )
{
	float bins[ disc::kBins ] = {};
	int count                 = 0;
	readAudio( bins, count );

	//The detector's own clock, off the normalised one. First frame primes; a
	//clock that has not moved holds; a jump is clamped.
	double dt = -1.0;
	if( audioClock >= 0.0 )
		dt = std::clamp( now - audioClock, 0.0, kMaxFrameDelta );
	audioClock = now;

	const float sensitivity = params[ PT_KNOCK_SENSITIVITY ];
	const double margin     = sensitivity <= 0.0f ? -1.0 : controls::KnockMargin( sensitivity );
	const bool onset        = onsets.Update( bins, count, dt, margin, perturb );

	const bool knock = onset || knockPending;
	knockPending     = false;

	disc::Settings s;
	s.bufferSeconds = controls::BufferSeconds( params[ PT_BUFFER ] );
	s.readSpeed     = controls::ReadSpeed( params[ PT_READ_SPEED ] );
	s.knockSeconds  = controls::KnockSeconds( params[ PT_KNOCK_LENGTH ] );
	s.restartLevel  = controls::RestartLevel( s.bufferSeconds );
	s.perturb       = perturb;
	buffer.Step( std::max( 0.0, dt ), knock, s );

	lastKnock = knock;
	return knock;
}

bool Atrac::StepDiscForTest( double seconds )
{
	clock.SetScaleForTest( 1.0 );
	hostTime = seconds;
	clock.Update( hostTime );
	return stepDisc( clock.Now() );
}

//---------------------------------------------------------------------------
bool Atrac::ensureBuffers( const Frame& f, bool decoding )
{
	const int cw = f.Bx * f.N;
	const int ch = f.By * f.N;
	bool ok      = converted.Ensure( f.width, f.height, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	          && cells.Ensure( f.Bx, f.By, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( decoding )
	{
		ok = ok && lapped.Ensure( cw, f.height, GL_RGBA32F, PassBuffer::Sampling::Nearest )
		     && coefficients.Ensure( cw, ch, GL_RGBA32F, PassBuffer::Sampling::Nearest )
		     && quantised.Ensure( cw, ch, GL_RGBA32F, PassBuffer::Sampling::Nearest )
		     && alloc.Ensure( 2 * f.Bx, 3 * f.By, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	}
	//The held picture outlives a mute. While the mute lasts it is left at
	//whatever size it was written at (a resize mid-mute must not clear the
	//one frame the operator is looking at -- photofinish's trap); the
	//display samples it with normalised coordinates, so a stale size still
	//maps. It is re-ensured only on a frame that is about to rewrite it.
	if( decoding || ( perturb & codec::kPerturbResizeClears ) )
	{
		ok = ok && held.Ensure( f.width, f.height, GL_RGBA32F, PassBuffer::Sampling::Linear );
		if( heldWidth != f.width || heldHeight != f.height )
		{
			heldWidth  = f.width;
			heldHeight = f.height;
			if( !decoding )
				heldValid = false;
		}
	}
	return ok;
}

void Atrac::uploadTexture( GLuint texture, int width, int height, const std::vector< float >& data, GLint format, GLenum layout )
{
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, layout, GL_FLOAT, data.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void Atrac::ensureTables( int N )
{
	if( tablesFor == N )
		return;
	tablesFor   = N;
	const int Q = N / 4;

	//The two bases in one texture: long in rows 0..N-1 (2N wide), short in
	//rows N..N+Q-1 (2Q wide, the rest zero). Computed in double, stored in
	//float: the only rounding a basis value ever gets.
	std::vector< float > basis( static_cast< size_t >( 2 * N ) * ( N + Q ), 0.0f );
	const std::vector< double > longBasis  = codec::Basis( N );
	const std::vector< double > shortBasis = codec::Basis( Q );
	for( int n = 0; n < 2 * N; ++n )
		for( int k = 0; k < N; ++k )
			basis[ static_cast< size_t >( k ) * 2 * N + n ] = static_cast< float >( longBasis[ static_cast< size_t >( n ) * N + k ] );
	for( int n = 0; n < 2 * Q; ++n )
		for( int k = 0; k < Q; ++k )
			basis[ static_cast< size_t >( N + k ) * 2 * N + n ] = static_cast< float >( shortBasis[ static_cast< size_t >( n ) * Q + k ] );
	uploadTexture( basisTexture, 2 * N, N + Q, basis, GL_R32F, GL_RED );

	rampLong.assign( 32, 0.0f );
	rampShort.assign( 8, 0.0f );
	const std::vector< double > rl = codec::Ramp( N );
	const std::vector< double > rs = codec::Ramp( Q );
	for( int j = 0; j < N; ++j )
		rampLong[ j ] = static_cast< float >( rl[ j ] );
	for( int j = 0; j < Q; ++j )
		rampShort[ j ] = static_cast< float >( rs[ j ] );

	diagBfuLong.assign( 64, 0 );
	diagBfuShort.assign( 16, 0 );
	for( int d = 0; d < 2 * N - 1; ++d )
		diagBfuLong[ d ] = codec::BfuOfDiagonal( N, d );
	for( int d = 0; d < 2 * Q - 1; ++d )
		diagBfuShort[ d ] = codec::BfuOfDiagonal( Q, d );
	bfuLong  = codec::BfuCount( N );
	bfuShort = codec::BfuCount( Q );
	countLong.assign( 16, 0 );
	countShort.assign( 16, 0 );
	for( int j = 0; j < bfuLong; ++j )
		countLong[ j ] = codec::BfuCoefficients( N, j );
	for( int j = 0; j < bfuShort; ++j )
		countShort[ j ] = 16 * codec::BfuCoefficients( Q, j );

	sfTable.assign( codec::kScaleFactors, 0.0f );
	for( int i = 0; i < codec::kScaleFactors; ++i )
		sfTable[ i ] = static_cast< float >( codec::ScaleFactor( i ) );

	spreadUp.assign( 16, 0.0f );
	spreadDown.assign( 16, 0.0f );
	for( int i = 0; i < 16; ++i )
	{
		spreadUp[ i ]   = static_cast< float >( std::pow( 10.0, -codec::kSpreadUpDb * i / 10.0 ) );
		spreadDown[ i ] = static_cast< float >( std::pow( 10.0, -codec::kSpreadDownDb * i / 10.0 ) );
	}
}

void Atrac::setFloats( GLuint program, const char* name, const std::vector< float >& values )
{
	//FFGLShader::Set has no array overload. The program is bound by the
	//caller's ScopedShaderBinding.
	glUniform1fv( glGetUniformLocation( program, name ), static_cast< GLsizei >( values.size() ), values.data() );
}

void Atrac::setInts( GLuint program, const char* name, const std::vector< int >& values )
{
	glUniform1iv( glGetUniformLocation( program, name ), static_cast< GLsizei >( values.size() ), values.data() );
}

/// The lapped snippet's uniforms. Cells on unit 1, the basis on unit 2; the
/// caller binds them.
void Atrac::setLapped( FFGLShader& shader, const Frame& f )
{
	shader.Set( "Cells", 1 );
	shader.Set( "Basis", 2 );
	shader.Set( "N", f.N );
	shader.Set( "Bx", f.Bx );
	shader.Set( "By", f.By );
	shader.Set( "W", f.width );
	shader.Set( "H", f.height );
	shader.Set( "RectWindow", ( perturb & codec::kPerturbRectWindow ) ? 1 : 0 );
	setFloats( shader.GetGLID(), "RampLong", rampLong );
	setFloats( shader.GetGLID(), "RampShort", rampShort );
}

void Atrac::setBfuTables( FFGLShader& shader )
{
	setInts( shader.GetGLID(), "DiagBfuLong", diagBfuLong );
	setInts( shader.GetGLID(), "DiagBfuShort", diagBfuShort );
	setFloats( shader.GetGLID(), "SfTable", sfTable );
}

//---------------------------------------------------------------------------
FFResult Atrac::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	clock.Update( hostTime );
	const double now = clock.Now();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clock.ClockScale() )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// The settings, in the coder's units.
	//---------------------------------------------------------------------
	Frame f;
	f.width  = static_cast< int >( picture.Width );
	f.height = static_cast< int >( picture.Height );
	f.N      = codec::BlockSizeOf( controls::OptionIndex( params[ PT_BLOCK_SIZE ], codec::kBlockSizeCount ) );
	f.mode   = controls::OptionIndex( params[ PT_BLOCK_MODE ], codec::kBlockModeCount );
	f.Bx     = ( f.width + f.N - 1 ) / f.N;
	f.By     = ( f.height + f.N - 1 ) / f.N;
	double bpp = controls::BitsPerPixel( params[ PT_BIT_RATE ] );
	if( perturb & codec::kPerturbBitsIgnored )
		bpp = 0.5;
	f.budgetLuma   = static_cast< int >( std::floor( bpp * f.N * f.N ) );
	f.budgetChroma = static_cast< int >( std::floor( bpp * f.N * f.N * controls::ChromaFraction( params[ PT_CHROMA_BITS ] ) ) );
	f.masking      = controls::MaskingAmount( params[ PT_MASKING ] );

	//---------------------------------------------------------------------
	// The disc: the knock, the buffer, the mute. A muted disc decodes
	// nothing -- there is nothing to decode -- and the display shows what
	// was held. The very first frame decodes whatever the disc says, so a
	// knock on frame one holds a picture rather than black.
	//---------------------------------------------------------------------
	stepDisc( now );
	const bool decoding = !buffer.Muted() || !heldValid;

	//---------------------------------------------------------------------
	// Buffers and tables. Every allocation happens here, before anything
	// binds a texture: FFGLFBO::Initialise sizes its colour texture under a
	// scoped binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	if( !ensureBuffers( f, decoding ) )
	{
		diag::error( "could not allocate the buffers at " + std::to_string( f.width ) + "x" + std::to_string( f.height ) );
		return FF_FAIL;
	}
	ensureTables( f.N );

	//---------------------------------------------------------------------
	// 1. convert: Y Cb Cr.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( converted.GetGLID(), ScopedFBOBinding::RB_REVERT );
		converted.ResizeViewPort();
		ScopedShaderBinding shader( convertShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( picture.Handle );
		convertShader.Set( "InputTexture", 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. cells: long or short.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( cells.GetGLID(), ScopedFBOBinding::RB_REVERT );
		cells.ResizeViewPort();
		ScopedShaderBinding shader( cellsShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( converted.TextureID() );
		cellsShader.Set( "Picture", 0 );
		cellsShader.Set( "N", f.N );
		cellsShader.Set( "W", f.width );
		cellsShader.Set( "H", f.height );
		cellsShader.Set( "Mode", f.mode );
		cellsShader.Set( "Ratio", static_cast< float >( codec::kTransientRatio ) );
		cellsShader.Set( "FloorEnergy", static_cast< float >( codec::kTransientFloorPerPixel * ( f.N / 2 ) * f.N ) );
		cellsShader.Set( "ForceLong", ( perturb & codec::kPerturbShortIsLong ) ? 1 : 0 );
		quad.Draw();
	}

	if( decoding )
	{
		//-----------------------------------------------------------------
		// 3. mdctx: converted -> lapped.
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( lapped.GetGLID(), ScopedFBOBinding::RB_REVERT );
			lapped.ResizeViewPort();
			ScopedShaderBinding shader( mdctXShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( converted.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			ScopedSamplerActivation s2( 2 );
			Scoped2DTextureBinding t2( basisTexture );
			mdctXShader.Set( "Source", 0 );
			setLapped( mdctXShader, f );
			quad.Draw();
		}

		//-----------------------------------------------------------------
		// 4. mdcty: lapped -> coefficients.
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( coefficients.GetGLID(), ScopedFBOBinding::RB_REVERT );
			coefficients.ResizeViewPort();
			ScopedShaderBinding shader( mdctYShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( lapped.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			ScopedSamplerActivation s2( 2 );
			Scoped2DTextureBinding t2( basisTexture );
			mdctYShader.Set( "Source", 0 );
			setLapped( mdctYShader, f );
			quad.Draw();
		}

		//-----------------------------------------------------------------
		// 5. alloc: the model and the greedy, per cell and channel.
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( alloc.GetGLID(), ScopedFBOBinding::RB_REVERT );
			alloc.ResizeViewPort();
			ScopedShaderBinding shader( allocShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( coefficients.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			allocShader.Set( "Coefficients", 0 );
			allocShader.Set( "Cells", 1 );
			allocShader.Set( "N", f.N );
			allocShader.Set( "Bx", f.Bx );
			allocShader.Set( "By", f.By );
			allocShader.Set( "BudgetLuma", f.budgetLuma );
			allocShader.Set( "BudgetChroma", f.budgetChroma );
			allocShader.Set( "BfuLong", bfuLong );
			allocShader.Set( "BfuShort", bfuShort );
			allocShader.Set( "OffsetFactor", static_cast< float >( std::pow( 10.0, -codec::MaskingOffsetDb( f.masking ) / 10.0 ) ) );
			allocShader.Set( "FloorEnergy", static_cast< float >( codec::kAbsoluteFloor ) );
			allocShader.Set( "OverBudget", ( perturb & codec::kPerturbOverBudget ) ? 1 : 0 );
			allocShader.Set( "StopEarly", ( perturb & codec::kPerturbStopEarly ) ? 1 : 0 );
			allocShader.Set( "IgnoreMasking", ( perturb & codec::kPerturbIgnoreMasking ) ? 1 : 0 );
			setBfuTables( allocShader );
			setInts( allocShader.GetGLID(), "CountLong", countLong );
			setInts( allocShader.GetGLID(), "CountShort", countShort );
			setFloats( allocShader.GetGLID(), "SpreadUp", spreadUp );
			setFloats( allocShader.GetGLID(), "SpreadDown", spreadDown );
			quad.Draw();
		}

		//-----------------------------------------------------------------
		// 6. quant.
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( quantised.GetGLID(), ScopedFBOBinding::RB_REVERT );
			quantised.ResizeViewPort();
			ScopedShaderBinding shader( quantShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( coefficients.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			ScopedSamplerActivation s2( 2 );
			Scoped2DTextureBinding t2( alloc.TextureID() );
			quantShader.Set( "Coefficients", 0 );
			quantShader.Set( "Cells", 1 );
			quantShader.Set( "Alloc", 2 );
			quantShader.Set( "N", f.N );
			quantShader.Set( "Bx", f.Bx );
			quantShader.Set( "By", f.By );
			quantShader.Set( "Bypass", ( perturb & codec::kPerturbNoQuantise ) ? 1 : 0 );
			setBfuTables( quantShader );
			quad.Draw();
		}

		//-----------------------------------------------------------------
		// 7. imdcty: quantised -> lapped (reused: mdctx's output is spent).
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( lapped.GetGLID(), ScopedFBOBinding::RB_REVERT );
			lapped.ResizeViewPort();
			ScopedShaderBinding shader( imdctYShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( quantised.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			ScopedSamplerActivation s2( 2 );
			Scoped2DTextureBinding t2( basisTexture );
			imdctYShader.Set( "Source", 0 );
			setLapped( imdctYShader, f );
			quad.Draw();
		}

		//-----------------------------------------------------------------
		// 8. imdctx: lapped -> held, back in RGB.
		//-----------------------------------------------------------------
		{
			ScopedFBOBinding fbo( held.GetGLID(), ScopedFBOBinding::RB_REVERT );
			held.ResizeViewPort();
			ScopedShaderBinding shader( imdctXShader.GetGLID() );
			ScopedSamplerActivation s0( 0 );
			Scoped2DTextureBinding t0( lapped.TextureID() );
			ScopedSamplerActivation s1( 1 );
			Scoped2DTextureBinding t1( cells.TextureID() );
			ScopedSamplerActivation s2( 2 );
			Scoped2DTextureBinding t2( basisTexture );
			ScopedSamplerActivation s3( 3 );
			Scoped2DTextureBinding t3( picture.Handle );
			imdctXShader.Set( "Source", 0 );
			imdctXShader.Set( "InputTexture", 3 );
			setLapped( imdctXShader, f );
			quad.Draw();
		}
		heldValid   = true;
		decodedOnce = true;
	}

	//---------------------------------------------------------------------
	// 9. display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( held.TextureID() );
		ScopedSamplerActivation s2( 2 );
		Scoped2DTextureBinding t2( cells.TextureID() );
		ScopedSamplerActivation s3( 3 );
		Scoped2DTextureBinding t3( alloc.TextureID() );

		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "Held", 1 );
		displayShader.Set( "Cells", 2 );
		displayShader.Set( "Alloc", 3 );
		displayShader.Set( "N", f.N );
		displayShader.Set( "Bx", f.Bx );
		displayShader.Set( "By", f.By );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		displayShader.Set( "ShowBlocks", params[ PT_SHOW_BLOCKS ] >= 0.5f ? 1 : 0 );
		displayShader.Set( "ShowBits", params[ PT_SHOW_BITS ] >= 0.5f ? 1 : 0 );
		setInts( displayShader.GetGLID(), "DiagBfuLong", diagBfuLong );
		setInts( displayShader.GetGLID(), "DiagBfuShort", diagBfuShort );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
GLuint Atrac::CoefficientTextureForTest( int& w, int& h ) const
{
	w = coefficients.GetWidth();
	h = coefficients.GetHeight();
	return coefficients.TextureID();
}

GLuint Atrac::AllocationTextureForTest( int& w, int& h ) const
{
	w = alloc.GetWidth();
	h = alloc.GetHeight();
	return alloc.TextureID();
}

GLuint Atrac::CellsTextureForTest( int& w, int& h ) const
{
	w = cells.GetWidth();
	h = cells.GetHeight();
	return cells.TextureID();
}

//---------------------------------------------------------------------------
FFResult Atrac::DeInitGL()
{
	convertShader.FreeGLResources();
	cellsShader.FreeGLResources();
	mdctXShader.FreeGLResources();
	mdctYShader.FreeGLResources();
	allocShader.FreeGLResources();
	quantShader.FreeGLResources();
	imdctYShader.FreeGLResources();
	imdctXShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();

	converted.Destroy();
	cells.Destroy();
	lapped.Destroy();
	coefficients.Destroy();
	quantised.Destroy();
	alloc.Destroy();
	held.Destroy();
	if( basisTexture != 0 )
		glDeleteTextures( 1, &basisTexture );
	basisTexture = 0;
	tablesFor    = 0;
	heldValid    = false;
	heldWidth = heldHeight = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Atrac::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_KNOCK )
	{
		//Rising edge only. An event parameter goes to 1 and back to 0 as the
		//button is pressed and released; a knock per edge would be two.
		if( value > 0.5f && knockValue <= 0.5f )
			knockPending = true;
		knockValue = value;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Atrac::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Atrac::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Atrac::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Atrac::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
