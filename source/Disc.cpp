#include "Disc.h"

#include "Codec.h"

#include <algorithm>
#include <cmath>

namespace atrac::disc
{

void Buffer::Reset( double bufferSeconds )
{
	level    = std::max( 0.0, bufferSeconds );
	stopLeft = 0.0;
	muted    = false;
	started  = true;
}

void Buffer::Step( double dt, bool knock, const Settings& s )
{
	if( !started )
		Reset( s.bufferSeconds );

	const double capacity = std::max( 0.0, s.bufferSeconds );
	//An operator shrinking the buffer mid-run cannot leave more in it than
	//it holds.
	level = std::min( level, capacity );

	if( knock )
	{
		stopLeft = std::max( stopLeft, s.knockSeconds );
		if( s.perturb & codec::kPerturbBufferIgnored )
			level = 0.0;
	}

	if( dt <= 0.0 )
	{
		if( !muted && level <= 0.0 && stopLeft > 0.0 )
			muted = true;
		return;
	}

	//The knock runs down first: the part of this frame the read was
	//stopped for, and the part it was back.
	const double stopped = std::min( dt, std::max( 0.0, stopLeft ) );
	const double reading = dt - stopped;
	stopLeft             = std::max( 0.0, stopLeft - dt );

	const double readRate = ( s.perturb & codec::kPerturbNoRefill ) ? 1.0 : std::max( 1.0, s.readSpeed );

	if( !muted )
	{
		//Playing: draining at 1x throughout, filling at readRate while read.
		level -= stopped;
		if( level <= 0.0 )
		{
			//Empty before the read came back: the break. What is left of the
			//frame is spent broken, filling if the read is back by then.
			level = 0.0;
			muted = true;
			level += readRate * reading;
		}
		else
			level += ( readRate - 1.0 ) * reading;
	}
	else
	{
		//Broken: nothing plays, so nothing drains; the read fills at full
		//speed once it is back.
		level += readRate * reading;
	}

	level = std::min( level, capacity );

	if( muted && stopLeft <= 0.0 && level >= std::min( capacity, s.restartLevel ) )
		muted = false;
}

//---------------------------------------------------------------------------
void Onsets::Reset()
{
	previous.fill( 0.0 );
	fluxMean   = 0.0;
	flux       = 0.0;
	bar        = 0.0;
	refractory = 0.0;
	primed     = false;
	onsets     = 0;
}

bool Onsets::Update( const float* bins, int count, double dt, double margin, int perturb )
{
	const int n = std::clamp( count, 0, kBins );

	std::array< double, kBins > now{};
	for( int i = 0; i < n; ++i )
		//sqrt because bin magnitudes bunch hard against zero (the fleet's
		//habit; whether the host sends magnitudes or powers is unmeasured).
		now[ i ] = std::sqrt( std::max( 0.0, static_cast< double >( bins ? bins[ i ] : 0.0f ) ) );

	if( !primed )
	{
		//The first frame: adopt this spectrum as the past. Nothing has risen.
		primed = true;
		if( !( perturb & codec::kPerturbNoPrime ) )
		{
			previous = now;
			flux     = 0.0;
			bar      = std::max( kFluxFloor, fluxMean * ( 1.0 + std::max( 0.0, margin ) ) );
			return false;
		}
	}

	if( dt <= 0.0 )
		return false;//a paused host is not a rising one

	double rise = 0.0;
	for( int i = 0; i < kBins; ++i )
	{
		rise += std::max( 0.0, now[ i ] - previous[ i ] );
		previous[ i ] = now[ i ];
	}
	flux = rise / kBins;

	//The mean is updated AFTER the comparison, so a hit is measured against
	//the quiet before it, not against itself.
	bar        = std::max( kFluxFloor, fluxMean * ( 1.0 + std::max( 0.0, margin ) ) );
	refractory = std::max( 0.0, refractory - dt );
	bool fired = false;
	if( margin >= 0.0 && refractory <= 0.0 && flux > bar )
	{
		fired      = true;
		refractory = kRefractory;
		++onsets;
	}

	const double coefficient = 1.0 - std::exp( -dt / kMeanSeconds );
	fluxMean += ( flux - fluxMean ) * coefficient;
	return fired;
}

} // namespace atrac::disc
