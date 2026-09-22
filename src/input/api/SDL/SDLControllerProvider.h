#pragma once
#include <SDL3/SDL_joystick.h>
#include "input/motion/MotionHandler.h"
#include "input/api/ControllerProvider.h"

static bool operator==(const SDL_GUID& g1, const SDL_GUID& g2)
{
	return memcmp(&g1, &g2, sizeof(SDL_GUID)) == 0;
}

class SDLControllerProvider : public ControllerProviderBase
{
	friend class SDLController;
public:
	SDLControllerProvider();
	~SDLControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::SDLController;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
	
	int get_index(size_t guid_index, const SDL_GUID& guid) const;

	MotionSample motion_sample(SDL_JoystickID diid);

	// Only one thread may drain SDL's global event queue. A front end with an
	// SDL window of its own owns that loop, and says so before any provider is
	// created; this provider then handles the controller events the host hands
	// it instead of running its own thread. macOS always requires the main
	// thread to pump, so it is set there regardless.
	static void SetHostOwnsEventLoop(bool hostOwns);
	static bool HostOwnsEventLoop();

	// The host's side of that arrangement.
	static void InitSDL();
	static void ShutdownSDL();
	// Drains the queue itself, for a host with no window of its own.
	static void PumpSDLEvents();
	// Hands over one event, for a host that drains the queue for its window.
	static void HandleHostEvent(union SDL_Event& event);

private:
	void event_thread();
	static void HandleSDLEvent(union SDL_Event& event);

	inline static std::atomic_bool s_hostOwnsEventLoop{BOOST_OS_MACOS != 0};

	// there is only one SDL instance, for this reason all of our state can be static
	inline static std::atomic_int s_initCount{0};
	inline static std::shared_mutex s_mutex;
	inline static std::atomic_bool s_running = false;
	inline static std::thread s_thread;

	struct MotionInfoTracking
	{
		uint64 lastTimestampGyro{};
		uint64 lastTimestampAccel{};
		uint64 lastTimestampIntegrate{};
		bool hasGyro{};
		bool hasAcc{};
		glm::vec3 gyro{};
		glm::vec3 acc{};
	};

	struct MotionState
	{
		WiiUMotionHandler handler;
		MotionSample data;
		MotionInfoTracking tracking;

		MotionState() = default;
	};

	inline static std::unordered_map<SDL_JoystickID, MotionState> s_motion_states{};
};
