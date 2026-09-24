#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace atrac::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double BitsPerPixel( float value )
{
	return 0.05 * std::pow( 80.0, unit( value ) );
}

double ChromaFraction( float value )
{
	return unit( value );
}

double MaskingAmount( float value )
{
	return unit( value );
}

double BufferSeconds( float value )
{
	return 10.0 * unit( value );
}

double ReadSpeed( float value )
{
	return 1.0 + 3.0 * unit( value );
}

double KnockSeconds( float value )
{
	return 0.05 * std::pow( 100.0, unit( value ) );
}

double KnockMargin( float value )
{
	return 2.5 - 2.35 * unit( value );
}

double RestartLevel( double bufferSeconds )
{
	return 0.25 * bufferSeconds;
}

} // namespace atrac::controls
