#pragma once

#include <cstdint>
#include <functional>

// The points at which first-party code observes and substitutes, and nothing
// else.
//
// The product is this executable, but the recording, blending and policy that
// make an interpolated frame are not part of it. They live in a separate
// library that registers here at startup. With nothing registered every hook
// is a no-op and this build behaves exactly as upstream, which is what a clean
// checkout of the fork must keep doing.
//
// The types crossing this boundary are plain data with no Latte headers behind
// them, so the library never has to include the renderer to implement a hook.
namespace LatteFrameHooks
{

	// Bounds the per-draw source list. Real draws use a handful; the cap only
	// stops a corrupt shader from overrunning a stack buffer.
	inline constexpr uint32_t kMaxUniformBlockSources = 16;

	// One indirect buffer the frame referenced, as the guest handed it over. The
	// pointer is into guest memory, which is recycled between frames -- 175 of 175
	// addresses measured recurring -- so an observer that wants it later copies it.
	struct DisplayList
	{
		uint32_t physicalAddress;
		const void* data;
		uint32_t sizeInBytes;
	};

	// One shader's assembled uniform buffer, after both Latte uniform modes have
	// converged on it. `data` is writable: this is the point where a transform is
	// substituted, and it is the last point before the buffer is uploaded.
	struct UniformAssembly
	{
		uint64_t shaderBaseHash;
		uint64_t shaderAuxHash;
		uint32_t stageIndex;
		float* data;
		uint32_t sizeInBytes;
		// Guest addresses of the uniform blocks this draw sourced, which is the
		// only identity for the object being drawn that survives a tick.
		const uint32_t* blockAddresses;
		uint32_t blockAddressCount;
	};

	class Observer
	{
	  public:
		virtual ~Observer() = default;

		virtual void OnDisplayList(const DisplayList& list) = 0;
		virtual void OnUniformAssembly(const UniformAssembly& assembly) = 0;
		virtual void OnFrameEnd() = 0;
	};

	// Registered once at startup by the first-party library, and never replaced
	// while a frame is in flight. Passing nullptr restores upstream behaviour.
	void SetObserver(Observer* observer);

	// One presented frame, as the user would see it: the scan buffer after the
	// title has drawn it and before any overlay. The bytes belong to the
	// caller of the capture and do not outlive the callback.
	struct FrameImage
	{
		const uint8_t* rgb;
		uint32_t byteCount;
		int width;
		int height;
		bool mainWindow;
	};

	using CaptureCallback = std::function<void(const FrameImage&)>;

	// Capture the next frame the title presents. One-shot: a capture that
	// repeated every frame would make a replayed image impossible to tell
	// from the one after it. False means no capture was armed, which is a
	// refusal rather than a capture that silently never arrives.
	bool RequestFrameCapture(CaptureCallback callback);

	// Feed a recorded buffer back to the command processor as if the guest had
	// referenced it. The caller owns the memory and it must outlive the call.
	// False means it was not submitted, which is a refusal and not a silent
	// no-op: a replay that quietly drew nothing is indistinguishable from one
	// that worked.
	bool SubmitDisplayList(const void* data, uint32_t sizeInBytes);

	// Null until something registers. Callers check it rather than paying a
	// virtual call per draw for a hook nobody installed.
	Observer* GetObserver();

} // namespace LatteFrameHooks

// Provided by the first-party library, and only when one is linked. Declared
// here because this header is already the boundary between the two, and called
// once from startup: a static initialiser inside a static library is dropped
// by the linker when nothing references its object file, which would leave a
// build that records nothing and says so nowhere.
extern "C" void wiiuport_install_hooks(void);

