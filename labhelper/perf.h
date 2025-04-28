#pragma once

#include <chrono>
#include <string>

namespace labhelper
{
namespace perf
{

void pushTimer( const std::string& str );
void popTimer();

void synchProfilers();

void drawEventsWindow();

struct Scope
{
public:
	Scope( const std::string& name );
	~Scope();

private:
	Scope( const Scope& ) = delete;
	Scope( Scope&& ) = delete;
	Scope& operator=( const Scope& ) = delete;
	Scope& operator=( Scope&& ) = delete;
};

}
}   // namespace chag::perf

#define PROFILE_CAT_ID2(_id1_, _id2_) _id1_##_id2_
#define PROFILE_CAT_ID(_id1_, _id2_) PROFILE_CAT_ID2(_id1_, _id2_)

#if !defined(DISABLE_PROFILER)
#define PROFILE_SCOPE(_string_id_) \
	chag::perf::Scope PROFILE_CAT_ID(_scope_timer_, __COUNTER__)(_string_id_);
#else
#define PROFILE_SCOPE(n)
#endif

