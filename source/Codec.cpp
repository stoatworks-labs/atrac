#include "Codec.h"

#include <algorithm>
#include <cmath>

namespace atrac::codec
{
namespace
{
//MSVC's <cmath> has no M_PI without _USE_MATH_DEFINES, and the first Windows CI
//run failed on exactly that. Spelled out once, to the double.
constexpr double kPi = 3.14159265358979323846;
} // namespace

int BlockSizeOf( int option )
{
	static const int sizes[ kBlockSizeCount ] = { 8, 16, 32 };
	return sizes[ std::clamp( option, 0, kBlockSizeCount - 1 ) ];
}

const std::vector< int >& MasterEdges()
{
	//The first four bands are one diagonal each -- DC on its own, as ATRAC1
	//keeps its lowest BFUs narrow -- then each band is about 4/3 the width
	//of the last. 63 closes the widest block (M = 32 has diagonals 0..62).
	static const std::vector< int > edges = { 0, 1, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 48, 63 };
	return edges;
}

std::vector< int > BfuEdges( int M )
{
	const int last = 2 * M - 1;//one past the last diagonal
	std::vector< int > edges;
	for( int e : MasterEdges() )
	{
		if( e >= last )
			break;
		edges.push_back( e );
	}
	edges.push_back( last );
	return edges;
}

int BfuCount( int M )
{
	return static_cast< int >( BfuEdges( M ).size() ) - 1;
}

int BfuCoefficients( int M, int j )
{
	const std::vector< int > edges = BfuEdges( M );
	if( j < 0 || j + 1 >= static_cast< int >( edges.size() ) )
		return 0;
	int count = 0;
	for( int d = edges[ j ]; d < edges[ j + 1 ]; ++d )
	{
		//Diagonal d of an M x M block holds min( d, M - 1 ) - max( 0, d - M + 1 ) + 1 cells.
		const int lo = std::max( 0, d - M + 1 );
		const int hi = std::min( d, M - 1 );
		count += hi - lo + 1;
	}
	return count;
}

int BfuOfDiagonal( int M, int d )
{
	const std::vector< int > edges = BfuEdges( M );
	for( size_t j = 0; j + 1 < edges.size(); ++j )
		if( d >= edges[ j ] && d < edges[ j + 1 ] )
			return static_cast< int >( j );
	return -1;
}

double ScaleFactor( int index )
{
	return std::pow( 2.0, ( std::clamp( index, 0, kScaleFactors - 1 ) - 15 ) / 3.0 );
}

int ScaleFactorIndex( double peak )
{
	for( int i = 0; i < kScaleFactors; ++i )
		if( ScaleFactor( i ) >= peak )
			return i;
	return kScaleFactors - 1;
}

std::vector< double > Ramp( int L )
{
	std::vector< double > w( static_cast< size_t >( std::max( 0, L ) ) );
	for( int j = 0; j < L; ++j )
		w[ j ] = std::sin( kPi * ( j + 0.5 ) / ( 2.0 * L ) );
	return w;
}

std::vector< double > Basis( int M )
{
	std::vector< double > table( static_cast< size_t >( 2 * M ) * M );
	const double scale = std::sqrt( 2.0 / M );
	for( int n = 0; n < 2 * M; ++n )
		for( int k = 0; k < M; ++k )
			table[ static_cast< size_t >( n ) * M + k ] = scale * std::cos( kPi / M * ( n + 0.5 + M / 2.0 ) * ( k + 0.5 ) );
	return table;
}

double MaskingOffsetDb( double masking )
{
	return kMaskingRangeDb * ( 1.0 - std::clamp( masking, 0.0, 1.0 ) );
}

} // namespace atrac::codec
