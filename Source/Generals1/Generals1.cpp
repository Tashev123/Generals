#include "Generals1.h"
#include "Modules/ModuleManager.h"
#if PLATFORM_WINDOWS && defined(_M_X64)
#include <math.h>
#endif

DEFINE_LOG_CATEGORY(LogGenerals);

/** The game's module: one thing to settle before anything of the game runs. */
class FGenerals1Module : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_WINDOWS && defined(_M_X64)
		// The C runtime's maths (sin, cos, atan2, pow, exp...) takes another path on processors with FMA3
		// instructions, which can come out different in the last bit. Every player's computer must work out
		// the same game (network games, replays): with that path off, the same program gets the same numbers
		// on any processor.
		_set_FMA3_enable(0);
#endif
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FGenerals1Module, Generals1, "Generals1");
