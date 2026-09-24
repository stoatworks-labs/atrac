#pragma once

#include "Clock.h"
#include "Codec.h"
#include "Disc.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Atrac -- MiniDisc's perceptual coder, run on the picture, as an FFGL
	effect.

	**The one idea.** ATRAC does not throw detail away by resolution. It
	throws it away by BITS. The picture is cut into overlapping blocks, each
	goes through a lapped transform (the MDCT, whose overlapping halves
	cancel each other's aliasing), the coefficients are grouped into block
	floating units with a scale factor and a word length each, and a FIXED
	bit budget per block is shared out by a masking model: a band next to a
	loud band is masked and gets few bits or none. Every artefact -- the
	texture that goes first, the pre-echo either side of an edge, the
	shimmer as the allocation changes -- is a consequence of that budget and
	that transform, and none of it is drawn. On top sits the disc: a
	shock-proof buffer that a knock (an audio onset, or the button) drains,
	and when it empties the picture holds.

	**Two processors.** The GPU does everything per coefficient: the
	transform both ways, the allocation per cell, the quantiser
	(`Shaders.h`). The CPU builds the tables in double (`Codec.h`), converts
	the controls (`Controls.h`), and runs the disc and the onset detector in
	double (`Disc.h`). The state that carries across host frames is the
	disc's few doubles, the detector's previous spectrum, and the held
	picture the mute shows. See AGENTS.md for the traps and what is verified.
*/
class Atrac : public CFFGLPlugin
{
public:
	Atrac();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by actest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Negative-control hooks, a bitmask of `codec::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// Run the clock, the onset detector and the disc for one frame at this
	/// host time, with whatever spectrum is in the Audio buffer, and no GL.
	/// Returns true if a knock fired this frame. `--prime` and
	/// `--skipmodel` drive this directly.
	bool StepDiscForTest( double seconds );

	bool MutedForTest() const
	{
		return buffer.Muted();
	}
	double BufferLevelForTest() const
	{
		return buffer.Level();
	}
	bool KnockedForTest() const
	{
		return lastKnock;
	}
	unsigned long long OnsetsForTest() const
	{
		return onsets.Count();
	}

	/// The textures a check reads back: the 2-D coefficients (before the
	/// quantiser), the allocation, the cells. Valid after a ProcessOpenGL
	/// that decoded (not during a mute).
	GLuint CoefficientTextureForTest( int& w, int& h ) const;
	GLuint AllocationTextureForTest( int& w, int& h ) const;
	GLuint CellsTextureForTest( int& w, int& h ) const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Coder
		PT_BIT_RATE,
		PT_BLOCK_SIZE,
		PT_BLOCK_MODE,
		PT_MASKING,
		PT_CHROMA_BITS,

		//Disc
		PT_BUFFER,
		PT_READ_SPEED,
		PT_KNOCK_SENSITIVITY,
		PT_KNOCK_LENGTH,
		PT_KNOCK,
		PT_AUDIO,

		//View
		PT_SHOW_BLOCKS,
		PT_SHOW_BITS,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	struct Frame
	{
		int width  = 0;
		int height = 0;
		int N      = 16;
		int Bx     = 0;
		int By     = 0;
		int mode   = 0;
		int budgetLuma   = 0;
		int budgetChroma = 0;
		double masking   = 0.0;
	};

	/// The clock, the audio and the disc: the CPU half of a frame.
	bool stepDisc( double hostSeconds );
	void readAudio( float bins[ atrac::disc::kBins ], int& count );

	bool ensureBuffers( const Frame& f, bool decoding );
	void ensureTables( int N );
	void uploadTexture( GLuint texture, int width, int height, const std::vector< float >& data, GLint format, GLenum layout );
	void setFloats( GLuint program, const char* name, const std::vector< float >& values );
	void setInts( GLuint program, const char* name, const std::vector< int >& values );
	void setLapped( ffglex::FFGLShader& shader, const Frame& f );
	void setBfuTables( ffglex::FFGLShader& shader );

	ffglex::FFGLShader convertShader;
	ffglex::FFGLShader cellsShader;
	ffglex::FFGLShader mdctXShader;
	ffglex::FFGLShader mdctYShader;
	ffglex::FFGLShader allocShader;
	ffglex::FFGLShader quantShader;
	ffglex::FFGLShader imdctYShader;
	ffglex::FFGLShader imdctXShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	atrac::PassBuffer converted;///< W x H: Y - 1/2, Cb, Cr, alpha
	atrac::PassBuffer cells;    ///< Bx x By: 1 = short
	atrac::PassBuffer lapped;   ///< Bx N x H: coefficients along x (both directions of the chain)
	atrac::PassBuffer coefficients;///< Bx N x By N: the 2-D blocks
	atrac::PassBuffer quantised;   ///< Bx N x By N
	atrac::PassBuffer alloc;       ///< 2 Bx x 3 By
	atrac::PassBuffer held;        ///< W x H: the decoded picture, kept through a mute

	GLuint basisTexture = 0;///< 2N x (N + N/4): the two bases
	int tablesFor       = 0;///< the N the tables were built for

	//Tables, rebuilt when N changes.
	std::vector< float > rampLong;
	std::vector< float > rampShort;
	std::vector< int > diagBfuLong;
	std::vector< int > diagBfuShort;
	std::vector< int > countLong;
	std::vector< int > countShort;
	int bfuLong  = 0;
	int bfuShort = 0;
	std::vector< float > sfTable;
	std::vector< float > spreadUp;
	std::vector< float > spreadDown;

	//--- The disc's state, carried across host frames. All CPU, all double.
	atrac::disc::Buffer buffer;
	atrac::disc::Onsets onsets;
	double audioClock = -1.0;
	bool knockPending = false;
	bool lastKnock    = false;
	float knockValue  = 0.0f;
	bool audioSeen    = false;
	bool heldValid    = false;
	bool decodedOnce  = false;
	int heldWidth     = 0;
	int heldHeight    = 0;

	atrac::Clock clock;
	double hostTime = -1.0;
	int clockFrames = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
