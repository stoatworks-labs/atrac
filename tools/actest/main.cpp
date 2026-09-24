/**
	actest -- render Atrac offline, and read the coder back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context and measures the answer out of the picture it made, or out of
	the allocation texture the plugin's own shader wrote:

		actest --out /tmp/frame.png     a picture, on the moving test card
		actest --list                   every parameter, its kind and default
		actest --tdac                   at unlimited bits the lapped transform
		                                reconstructs the input within an l2
		                                bound derived from float rounding; a
		                                rectangular window does not
		actest --budget                 every cell spends at most its budget
		                                and wastes less than one increment,
		                                counted from the shader's own allocation
		actest --preecho                a step rings under two long blocks in
		                                Long, under half a cell in Short and
		                                in Adaptive, which picks short there
		actest --snr                    SNR on the card rises with Bit Rate
		actest --masking                a weak band beside a strong one gets
		                                zero bits with Masking on, bits with it off
		actest --skip                   a knock of L on a buffer of B breaks
		                                playback iff L > B, resuming on the
		                                closed form, read out of the picture
		actest --resize                 a resize during a mute keeps the held
		                                picture and the disc's state
		actest --prime                  no knock on the first frame of a loud clip
		actest --skipmodel              the disc's arithmetic against the closed
		                                form, no GL
		actest --tables                 the plugin's tables against the stated rules
		actest --names                  nothing a host will truncate; SW Atrac / AC01
		actest --negative               every check above can FAIL
		actest --offline                the checks that need no GL
		actest --bench                  the render cost
		actest --dump-shaders DIR       the exact GLSL the plugin compiles
		actest --pipe                   raw frames in, raw frames out

	The transform, the BFU rule, the scale-factor law, the masking model,
	the greedy and the disc's closed form are stated HERE, from their
	definitions, and never read out of Codec.h except where a check says it
	is comparing the plugin's table with the statement (`--tables`).
	AGENTS.md has one line per check on where each tolerance comes from.
*/

#include "Atrac.h"
#include "Codec.h"
#include "Controls.h"
#include "Disc.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace codec = atrac::codec;

int g_checks   = 0;
int g_failures = 0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::ofstream file( path, std::ios::binary );
	if( !file )
		return false;
	file.write( reinterpret_cast< const char* >( png.data() ), static_cast< std::streamsize >( png.size() ) );
	return static_cast< bool >( file );
}

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// The control laws, stated from their definitions (Controls.h's comments,
// which are the spec of each control), and their inverses for choosing a
// slider. A check converts the FLOAT it hands the plugin, so the stated
// value is exactly what the plugin was asked for.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double statedBpp( float v )
{
	return 0.05 * std::pow( 80.0, unit( v ) );
}
float sliderForBpp( double bpp )
{
	return static_cast< float >( std::log( bpp / 0.05 ) / std::log( 80.0 ) );
}
double statedBuffer( float v )
{
	return 10.0 * unit( v );
}
float sliderForBuffer( double seconds )
{
	return static_cast< float >( seconds / 10.0 );
}
double statedReadSpeed( float v )
{
	return 1.0 + 3.0 * unit( v );
}
float sliderForReadSpeed( double r )
{
	return static_cast< float >( ( r - 1.0 ) / 3.0 );
}
double statedKnock( float v )
{
	return 0.05 * std::pow( 100.0, unit( v ) );
}
float sliderForKnock( double seconds )
{
	return static_cast< float >( std::log( seconds / 0.05 ) / std::log( 100.0 ) );
}
double statedMargin( float v )
{
	return 2.5 - 2.35 * unit( v );
}
double statedRestart( double buffer )
{
	return 0.25 * buffer;
}
double statedMaskingOffsetDb( double m )
{
	return 60.0 * ( 1.0 - m );
}

//---------------------------------------------------------------------------
// The coder's rules, stated. The BFU master list, the scale-factor law and
// the word-length law are typed here from AGENTS.md; --tables holds the
// plugin's Codec.cpp to them, and every other check uses THESE.
//---------------------------------------------------------------------------
const std::vector< int > kStatedMaster = { 0, 1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 48, 63 };

std::vector< int > statedEdges( int M )
{
	std::vector< int > edges;
	for( int e : kStatedMaster )
	{
		if( e >= 2 * M - 1 )
			break;
		edges.push_back( e );
	}
	edges.push_back( 2 * M - 1 );
	return edges;
}

int statedBfuOf( int M, int d )
{
	const std::vector< int > e = statedEdges( M );
	for( size_t j = 0; j + 1 < e.size(); ++j )
		if( d >= e[ j ] && d < e[ j + 1 ] )
			return static_cast< int >( j );
	return -1;
}

int statedBfuCount( int M )
{
	return static_cast< int >( statedEdges( M ).size() ) - 1;
}

/// Coefficients of an M x M block in band j: count the lattice points on
/// each diagonal directly.
int statedBfuCoefficients( int M, int j )
{
	int count = 0;
	for( int kx = 0; kx < M; ++kx )
		for( int ky = 0; ky < M; ++ky )
			if( statedBfuOf( M, kx + ky ) == j )
				++count;
	return count;
}

double statedScaleFactor( int i )
{
	return std::pow( 2.0, ( i - 15 ) / 3.0 );
}

/// The window ramp and the orthonormal MLT basis, from the definition.
double statedRamp( int j, int L )
{
	return std::sin( M_PI * ( j + 0.5 ) / ( 2.0 * L ) );
}
double statedBasis( int n, int k, int M )
{
	return std::sqrt( 2.0 / M ) * std::cos( M_PI / M * ( n + 0.5 + M / 2.0 ) * ( k + 0.5 ) );
}

/// The window of a long block with full overlaps on both sides, index n of
/// 2M: the plain sine window, which is what an interior long block in Long
/// mode has.
double statedSineWindow( int n, int M )
{
	return std::sin( M_PI * ( n + 0.5 ) / ( 2.0 * M ) );
}

/// Every control a check can move, as the sliders the plugin sees.
struct Knobs
{
	float bitRate      = 0.525f;
	int blockSize      = 1;//16
	int blockMode      = codec::kAdaptive;
	float masking      = 0.6f;
	float chromaBits   = 0.25f;
	float buffer       = 0.2f;
	float readSpeed    = 1.0f / 3.0f;
	float sensitivity  = 0.5f;
	float knockLength  = 0.5f;
	bool showBlocks    = false;
	bool showBits      = false;
	float mix          = 1.0f;
};

int blockSizeOfOption( int option )
{
	const int sizes[ 3 ] = { 8, 16, 32 };
	return sizes[ std::clamp( option, 0, 2 ) ];
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

/// Read any texture back as float RGBA, through a scratch framebuffer.
std::vector< float > readTexture( GLuint texture, int width, int height )
{
	std::vector< float > out( static_cast< size_t >( width ) * height * 4 );
	if( texture == 0 || width <= 0 || height <= 0 )
		return out;
	GLint previous = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	GLuint fbo = makeFramebuffer( texture );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, out.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previous ) );
	glDeleteFramebuffers( 1, &fbo );
	return out;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Atrac::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Atrac& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Atrac::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Atrac& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Atrac& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Atrac& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

void apply( Atrac& p, const Knobs& k )
{
	set( p, "Bit Rate", k.bitRate );
	set( p, "Block Size", static_cast< float >( k.blockSize ) );
	set( p, "Block Mode", static_cast< float >( k.blockMode ) );
	set( p, "Masking", k.masking );
	set( p, "Chroma Bits", k.chromaBits );
	set( p, "Buffer", k.buffer );
	set( p, "Read Speed", k.readSpeed );
	set( p, "Sensitivity", k.sensitivity );
	set( p, "Knock Length", k.knockLength );
	set( p, "Show Blocks", k.showBlocks ? 1.0f : 0.0f );
	set( p, "Show Bits", k.showBits ? 1.0f : 0.0f );
	set( p, "Mix", k.mix );
}

/// Press the Knock button: the rising edge the host sends, then the release.
void knock( Atrac& p )
{
	set( p, "Knock", 1.0f );
	set( p, "Knock", 0.0f );
}

//---------------------------------------------------------------------------
// Spectra, written the way the host writes them: one value per element.
//---------------------------------------------------------------------------
void writeSpectrum( Atrac& plugin, const float bins[ atrac::disc::kBins ] )
{
	for( int i = 0; i < atrac::disc::kBins; ++i )
		plugin.SetParamElementValue( Atrac::PT_AUDIO, static_cast< unsigned int >( i ), bins[ i ] );
}

void writeFlatSpectrum( Atrac& plugin, float level )
{
	float bins[ atrac::disc::kBins ];
	for( float& b : bins )
		b = level;
	writeSpectrum( plugin, bins );
}

/**
    The synthetic programme for `--feed`: something that looks like music to
    the onset detector. Without it the Disc group's Knock Sensitivity is
    correctly dead offline -- the host is the only thing that ever supplies
    bins. A kick every half second on a bed of steady mids and a little
    hiss, so there is a rise to find and a floor to find it against.
*/
void feedSpectrum( Atrac& plugin, int frame )
{
	const double seconds = frame / 60.0;
	const bool kick      = frame % 30 == 0;
	float bins[ atrac::disc::kBins ];
	for( int i = 0; i < atrac::disc::kBins; ++i )
	{
		const float t = static_cast< float >( i ) / 63.0f;
		float value   = 0.04f * std::exp( -3.0f * t );
		if( i >= 4 && i < 20 )
			value += 0.25f;
		if( i >= 20 )
			value += 0.10f + 0.04f * static_cast< float >( std::sin( 9.0 * t + seconds ) );
		if( kick && i < 6 )
			value += 0.8f;
		bins[ i ] = std::clamp( value, 0.0f, 1.0f );
	}
	writeSpectrum( plugin, bins );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Atrac plugin;
	int width        = 0;
	int height       = 0;
	double fps       = 60.0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	double timeOf( int64_t frame ) const
	{
		return static_cast< double >( frame ) / fps;
	}

	bool renderAt( int64_t frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( timeOf( frame ) );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %lld\n", static_cast< long long >( frame ) );
		return ok;
	}

	/// Pictures are handed over top-first, as a file holds them; GL wants
	/// row 0 at the bottom.
	bool render( int64_t frame, const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	bool render( int64_t frame, const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	/// A picture already in GL orientation (row 0 at the bottom), for checks
	/// that address cells by their GL row.
	bool renderGL( int64_t frame, const std::vector< float >& pixelsGL )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, pixelsGL.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	std::vector< float > readBackFloatGL()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// The plugin's coefficient plane, allocation and cells, as the shaders
	/// left them, in GL orientation (row 0 at the bottom).
	std::vector< float > coefficients( int& w, int& h )
	{
		const GLuint t = plugin.CoefficientTextureForTest( w, h );
		return readTexture( t, w, h );
	}
	std::vector< float > allocation( int& w, int& h )
	{
		const GLuint t = plugin.AllocationTextureForTest( w, h );
		return readTexture( t, w, h );
	}
	std::vector< float > cells( int& w, int& h )
	{
		const GLuint t = plugin.CellsTextureForTest( w, h );
		return readTexture( t, w, h );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// --set on the command line of a check: applied after the check's own
/// knobs, so a check can be re-run with one control moved.
std::vector< std::string > g_overrides;

/// A check's session: the knobs applied, the perturbation set.
void prepare( Session& s, const Knobs& k, int perturb )
{
	apply( s.plugin, k );
	for( const std::string& setting : g_overrides )
	{
		std::string error;
		if( !applySetting( s.plugin, setting, error ) )
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
	}
	s.plugin.SetPerturbForTest( perturb );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
std::vector< float > flat( int W, int H, double level )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}

/// PCG output mix: integer hashing, exact, the same on every machine.
uint32_t pcgHash( uint32_t input )
{
	const uint32_t state = input * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b, uint32_t c )
{
	return pcgHash( pcgHash( pcgHash( a ) + b ) + c ) / 4294967296.0;
}

/// The reconstruction card: a gradient, noise of three grains, a disc and
/// hard edges, in colour, so every band of every block has something in it.
std::vector< float > texturedCard( int W, int H, uint32_t seed )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double fx = ( x + 0.5 ) / W, fy = ( y + 0.5 ) / H;
			double r = 0.25 + 0.5 * fx, g = 0.25 + 0.5 * fy, b = 0.5;
			const double n1 = hash01( seed, static_cast< uint32_t >( x ), static_cast< uint32_t >( y ) ) - 0.5;
			const double n2 = hash01( seed + 1, static_cast< uint32_t >( x / 3 ), static_cast< uint32_t >( y / 3 ) ) - 0.5;
			r += 0.25 * n1 + 0.15 * n2;
			g += 0.20 * n1 - 0.10 * n2;
			b += 0.15 * n2;
			const double dx = fx - 0.5, dy = fy - 0.5;
			if( dx * dx + dy * dy < 0.06 )
			{
				r = 0.9 - 0.4 * n1;
				g = 0.3;
				b = 0.2;
			}
			if( fx > 0.7 && fx < 0.75 )
				r = g = b = 0.05;
			if( fy > 0.8 && fy < 0.82 )
				r = g = b = 0.95;
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ]   = static_cast< float >( std::clamp( r, 0.0, 1.0 ) );
			px[ 1 ]   = static_cast< float >( std::clamp( g, 0.0, 1.0 ) );
			px[ 2 ]   = static_cast< float >( std::clamp( b, 0.0, 1.0 ) );
			px[ 3 ]   = 1.0f;
		}
	return p;
}

/// The rate card: structure at every scale, none of it white noise --
/// gradients, three gratings, a soft disc, a hard edge and a coarse grain --
/// so more bits always have something compressible to spend themselves on.
/// White noise is incompressible: on it the SNR sits on the noise floor
/// until the rate reaches a bit per coefficient, and a monotonicity check
/// on it would be measuring the card.
std::vector< float > rateCard( int W, int H )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double fx = ( x + 0.5 ) / W, fy = ( y + 0.5 ) / H;
			double r = 0.45 + 0.25 * fx + 0.12 * std::sin( 2.0 * M_PI * ( 5.0 * fx + 2.0 * fy ) );
			double g = 0.45 + 0.25 * fy + 0.10 * std::sin( 2.0 * M_PI * ( 11.0 * fx - 3.0 * fy ) );
			double b = 0.50 + 0.08 * std::sin( 2.0 * M_PI * ( 23.0 * fy ) ) - 0.15 * fx;
			const double dx = fx - 0.4, dy = fy - 0.55;
			const double blob = std::exp( -( dx * dx + dy * dy ) / 0.02 );
			r += 0.3 * blob;
			g -= 0.2 * blob;
			const double grain = 0.06 * ( hash01( 21u, static_cast< uint32_t >( x / 6 ), static_cast< uint32_t >( y / 6 ) ) - 0.5 );
			r += grain;
			g += grain;
			b += grain;
			if( fx > 0.62 && fx < 0.64 )
				r = g = b = 0.08;
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ]   = static_cast< float >( std::clamp( r, 0.0, 1.0 ) );
			px[ 1 ]   = static_cast< float >( std::clamp( g, 0.0, 1.0 ) );
			px[ 2 ]   = static_cast< float >( std::clamp( b, 0.0, 1.0 ) );
			px[ 3 ]   = 1.0f;
		}
	return p;
}

/// A vertical step in grey: dark left of column `edge`, bright from it.
std::vector< float > stepCard( int W, int H, int edge, double dark, double bright )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( x < edge ? dark : bright );
			px[ 3 ] = 1.0f;
		}
	return p;
}

/// A picture that changes every frame, for the disc checks: the textured
/// card with a moving bar, so a held frame is unmistakable.
std::vector< float > movingCard( int W, int H, int frame )
{
	std::vector< float > p = texturedCard( W, H, 7u );
	const int barX         = ( frame * 3 ) % W;
	for( int y = 0; y < H; ++y )
		for( int x = std::max( 0, barX - 2 ); x < std::min( W, barX + 3 ); ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = 1.0f;
		}
	return p;
}

/// The moving card for --out, the sweep, the bench and a default --pipe:
/// bars, a ramp, a flashing block, a moving bar and a textured field, so
/// the coder has texture to starve and edges to ring, and every frame
/// differs from the last.
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const bool flash  = std::fmod( t, 0.4 ) < 0.2;
	const double barX = std::fmod( 40.0 + 240.0 * t, static_cast< double >( width ) );
	const unsigned char bars[ 7 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
		                                   { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 40, g = 40, b = 40;
			if( fy < 0.3 )
			{
				const unsigned char* c = bars[ std::min( 6, x * 7 / width ) ];
				r = c[ 0 ];
				g = c[ 1 ];
				b = c[ 2 ];
			}
			else if( fy < 0.45 )
				r = g = b = 255.0 * fx;//a ramp
			else if( fx > 0.3 && fx < 0.7 && fy > 0.5 && fy < 0.85 )
				r = g = b = flash ? 235.0 : 16.0;//the block that cuts
			else if( fy >= 0.5 )
			{
				//Texture: grain and a fine hatch, what a fixed budget starves first.
				const double n = hash01( 3u, static_cast< uint32_t >( x ), static_cast< uint32_t >( y ) ) - 0.5;
				const double h = ( ( x + y ) % 4 < 2 ) ? 30.0 : -30.0;
				r              = 120 + 60 * n + h;
				g              = 100 + 60 * n + h;
				b              = 80 + 40 * n;
			}
			if( fy > 0.9 )
			{
				r = 200;
				g = 60;
				b = 30;
			}
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 120.0 ) )
				r = g = b = 250.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( std::clamp( r, 0.0, 255.0 ) );
			px[ 1 ] = static_cast< unsigned char >( std::clamp( g, 0.0, 255.0 ) );
			px[ 2 ] = static_cast< unsigned char >( std::clamp( b, 0.0, 255.0 ) );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// Decoding the allocation texture (the packing stated in Shaders.cpp).
//---------------------------------------------------------------------------
struct Allocation
{
	int Bx = 0, By = 0;
	std::vector< float > data;//2 Bx x 3 By RGBA, GL orientation

	/// Word length and scale-factor index of BFU j, channel ch, cell (bx, by).
	std::pair< int, int > of( int bx, int by, int ch, int j ) const
	{
		const int part = j / 8;
		const int slot = j - 8 * part;
		const size_t texel = ( static_cast< size_t >( ch * By + by ) * ( 2 * Bx ) + ( 2 * bx + part ) ) * 4;
		const int v        = static_cast< int >( std::lround( data[ texel + slot / 2 ] ) );
		const int hi       = v / 2048;
		const int lo       = v - hi * 2048;
		const int e        = ( slot % 2 == 1 ) ? hi : lo;
		return { e % 32, e / 32 };
	}
};

Allocation readAllocation( Session& s )
{
	Allocation a;
	int w = 0, h = 0;
	a.data = s.allocation( w, h );
	a.Bx   = w / 2;
	a.By   = h / 3;
	return a;
}

struct Cells
{
	int Bx = 0, By = 0;
	std::vector< float > data;
	bool isShort( int bx, int by ) const
	{
		return data[ ( static_cast< size_t >( by ) * Bx + bx ) * 4 ] > 0.5f;
	}
};

Cells readCells( Session& s )
{
	Cells c;
	c.data = s.cells( c.Bx, c.By );
	return c;
}

} // namespace

// The checks live in a second translation-unit-sized block below main's
// helpers so the file reads top-down: plumbing, then physics, then main.
#include "checks.inc"

namespace
{
//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is built once per frame of a short loop and uploaded ahead, so
	//the upload is not in the figure; the plugin still sees a moving picture.
	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best   = 1e9;
	int64_t frame = warmup;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i, ++frame )
			session.renderAt( frame );
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame   Block Size 8    Block Size 32\n" );
	std::vector< std::string > small = settings, large = settings;
	small.push_back( "Block Size=0" );
	large.push_back( "Block Size=2" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		const double sm = benchAt( small, size.width, size.height, frames );
		const double lg = benchAt( large, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%          %7.3f          %7.3f\n", size.name, ms, ms / 16.667 * 100.0, sm, lg );
	}
	std::printf( "\nEach frame: convert, cells, two forward and two inverse lapped passes, the\n"
	             "allocation per cell, the quantiser and the display. Nothing is read back.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = atrac::shaders;
	const std::pair< const char*, const std::string* > files[] = {
		{ "vertex.vert", &sh::Vertex() },  { "convert.frag", &sh::Convert() }, { "cells.frag", &sh::Cells() },
		{ "mdctx.frag", &sh::MdctX() },    { "mdcty.frag", &sh::MdctY() },     { "alloc.frag", &sh::Alloc() },
		{ "quant.frag", &sh::Quant() },    { "imdcty.frag", &sh::ImdctY() },   { "imdctx.frag", &sh::ImdctX() },
		{ "display.frag", &sh::Display() },
	};
	static_assert( sizeof( files ) / sizeof( files[ 0 ] ) == sh::kShaderCount, "every shader is dumped" );
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << *f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"actest -- render and measure the Atrac perceptual coder\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/atrac.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options). Repeatable.\n"
		"  --feed              write a synthetic spectrum (a kick every half second) into the Audio buffer each frame\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --tdac              unlimited bits: the transform reconstructs the input within a derived l2 bound\n"
		"  --budget            every cell spends at most its budget and wastes less than one increment\n"
		"  --preecho           a step rings under 2N in Long, under N/2 in Short and Adaptive\n"
		"  --snr               SNR on the card rises with Bit Rate\n"
		"  --masking           a weak band beside a strong one is zeroed with Masking on, coded with it off\n"
		"  --skip              a knock breaks playback iff L > B, resuming on the closed form\n"
		"  --resize            a resize during a mute keeps the held picture and the disc's state\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Codec.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --prime             no knock on the first frame of a loud clip; a real onset still knocks\n"
		"  --skipmodel         the disc's arithmetic against the closed form\n"
		"  --tables            the plugin's tables and control laws against the statements\n"
		"  --names             nothing the host will silently truncate; the host reads SW Atrac / AC01\n"
		"  --offline           all four, and their negative controls; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/atrac.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool wantFeed  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--tdac", "--budget", "--preecho", "--snr", "--masking", "--skip", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--prime", "--skipmodel", "--tables", "--names", "--negative-offline" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--feed" )
			wantFeed = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--tables", "--names", "--skipmodel", "--prime", "--negative-offline" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Atrac plugin;
		std::printf( "%3s  %-18s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-18s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		g_overrides = settings;
		//The checks with no GL first; a context only if a rendering one asks.
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--tables" )
				runTables();
			else if( check == "--names" )
				runNames();
			else if( check == "--skipmodel" )
				runSkipModel( perturb );
			else if( check == "--prime" )
				runPrime( perturb );
			else if( check == "--negative-offline" )
			{
				runNegativeOffline();
				offlineRan = true;
			}
			else
			{
				needGL = true;
				continue;
			}
			std::printf( "\n" );
		}
		if( offlineRan )
			std::printf( "   OFFLINE: --tdac, --budget, --preecho, --snr, --masking, --skip, --resize and\n"
			             "   their negative controls were NOT run. Nothing here drew a pixel through a GL\n"
			             "   driver; the shaders were not exercised, only (in CI) compiled by glslc.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--tdac" )
						runTdac( width, height, perturb );
					else if( check == "--budget" )
						runBudget( width, height, perturb );
					else if( check == "--preecho" )
						runPreecho( width, height, perturb );
					else if( check == "--snr" )
						runSnr( width, height, perturb );
					else if( check == "--masking" )
						runMasking( width, height, perturb );
					else if( check == "--skip" )
						runSkip( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );
			if( wantFeed )
				feedSpectrum( session.plugin, index );

			//Frame n is clocked at n / fps, never the wall clock: a stall
			//upstream must not show up in the reel as the disc skipping.
			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
	{
		if( wantFeed )
			feedSpectrum( session.plugin, frame );
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
