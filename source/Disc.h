#pragma once

#include <array>

/**
	The disc: a shock-proof buffer, and the knock that empties it. No GL, no
	FFGL, all double; the plugin steps it once per host frame and the
	harness's `--skipmodel` and `--prime` step it with no context at all.

	**The buffer.** A MiniDisc player reads the disc faster than it plays
	into a memory buffer. Playback always drains that buffer at 1x. While the
	disc is being read it fills at Read Speed x, so at rest it sits full. A
	knock stops the read for Knock Length; the buffer drains; if it empties
	before the read comes back, playback breaks -- the output holds the last
	decoded picture -- and stays broken until the read has refilled the
	buffer past the restart level. While broken nothing plays, so the buffer
	fills at the full read speed. Everything here is exact arithmetic on
	elapsed seconds; the only approximation is that time arrives a frame at
	a time, which is why the harness allows one frame.

	**The knock.** Either the Knock button (a rising edge) or an onset in the
	audio: spectral flux -- the positive change in each bin's magnitude since
	the last frame, summed over ALL 64 bins -- against a running mean of
	itself times a margin. Summing over every bin means the detector does
	not care how Resolume lays its bins out in frequency (the fleet has
	never measured that; see AGENTS.md). Whether a bin is a magnitude or a
	power is also unmeasured; the square root of each is taken, as the fleet
	does, and because the bar is adaptive the law only reshapes the flux, it
	does not deafen the detector.

	**Primed on the first frame.** The first spectrum after a reset is
	adopted as the previous one, so nothing reads as "risen from silence" on
	the frame a clip is triggered; and a frame of zero length advances
	nothing, so a paused host cannot snap the running mean. Without both, a
	loud clip trigger knocks the disc every time (the fleet's trap).
*/
namespace atrac::disc
{

constexpr int kBins = 64;

struct Settings
{
	double bufferSeconds = 2.0;
	double readSpeed     = 2.0;
	double knockSeconds  = 0.5;
	double restartLevel  = 0.5;///< seconds the buffer must hold before a broken playback resumes
	int perturb          = 0;
};

class Buffer
{
public:
	/// Start full: a player that has just been switched on has had time to
	/// read ahead before anybody pressed play.
	void Reset( double bufferSeconds );

	/// Advance by `dt` seconds. `knock` stops the read for knockSeconds
	/// from now (a knock during a knock extends it).
	void Step( double dt, bool knock, const Settings& s );

	bool Muted() const
	{
		return muted;
	}
	double Level() const
	{
		return level;
	}
	bool Reading() const
	{
		return stopLeft <= 0.0;
	}

private:
	double level    = 0.0;///< seconds buffered
	double stopLeft = 0.0;///< seconds of knock still to run
	bool muted      = false;
	bool started    = false;
};

class Onsets
{
public:
	void Reset();

	/// `bins` are what the host wrote this frame, `count` how many. `dt` is
	/// the frame in seconds: negative on the first frame after a reset
	/// (prime), zero for a frame that took no time (hold). `margin` is the
	/// flux margin; a margin below zero means "never fire". Returns true on
	/// an onset.
	bool Update( const float* bins, int count, double dt, double margin, int perturb );

	double Flux() const
	{
		return flux;
	}
	double Bar() const
	{
		return bar;
	}
	unsigned long long Count() const
	{
		return onsets;
	}

	/// An onset cannot follow another within this: 80 ms.
	static constexpr double kRefractory = 0.08;
	/// The running mean's time constant.
	static constexpr double kMeanSeconds = 1.0;
	/// The absolute floor under the bar: the adaptive bar alone divides
	/// noise by noise in silence and finds onsets in it.
	static constexpr double kFluxFloor = 0.004;

private:
	std::array< double, kBins > previous{};
	double fluxMean          = 0.0;
	double flux              = 0.0;
	double bar               = 0.0;
	double refractory        = 0.0;
	bool primed              = false;
	unsigned long long onsets = 0;
};

} // namespace atrac::disc
