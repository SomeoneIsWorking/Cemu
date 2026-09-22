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
		// True when this came back out of a buffer the runtime submitted
		// rather than one the guest referenced. A recorder that cannot tell
		// the two apart records its own replay as part of the next frame.
		bool fromRuntime;
		// True for a buffer the title's command queue submitted, false for one
		// referenced from inside another. Only the first kind carries a frame's
		// draws; the nested ones are walked as part of it, so a recorder that
		// keeps both would replay their contents twice.
		bool topLevel;
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
		// As in DisplayList: whose draw this is. It is also the moment a
		// substitution belongs to -- a blend edits the runtime's own replay
		// and never the frame the guest is drawing.
		bool fromRuntime;
	};

	// The nine arguments of the packet that copies a colour buffer to a scan
	// buffer, in the order the guest writes them. They are what a present is
	// made of, so a first-party runtime that wants to present a frame of its
	// own has to have seen one first rather than inventing plausible values.
	struct PresentArguments
	{
		uint32_t physicalAddress;
		uint32_t width;
		uint32_t height;
		uint32_t pitch;
		uint32_t tileMode;
		uint32_t swizzle;
		uint32_t sliceIndex;
		uint32_t format;
		uint32_t renderTarget;
		// Which screen this copy feeds, decoded from renderTarget's bits here
		// because they belong to Latte. A title copies twice per frame -- the
		// TV and the GamePad -- so a runtime that keeps only the last one it
		// saw presents the GamePad's scan buffer into the main window.
		bool targetsTv;
		bool targetsDrc;
	};

	// What a runtime submission was not allowed to do. A replay re-issues a
	// frame the guest has already finished: its fences have been waited on,
	// its semaphores consumed, its timestamps read. Executing those packets a
	// second time would block the command processor on a semaphore nobody
	// will signal again, or tell the guest a fence retired that it never
	// submitted. Every class is withheld and counted, never silently dropped,
	// so a replay that needed one of them is visible as a number.
	enum class WithheldEffect : uint32_t
	{
		// Scan-buffer copies and swaps. The runtime presents through
		// SubmitPresent, which is the one place these run on its behalf.
		Presentation,
		// Waits on guest memory and semaphore signals/waits.
		Synchronisation,
		// Fence values, timestamps, and stream-out fill sizes written to guest
		// memory.
		GuestMemoryWrite,
		// Occlusion queries, whose results land in guest memory.
		OcclusionQuery,
		// Render targets mirrored back to guest memory.
		TextureReadback,
		Count
	};
	inline constexpr uint32_t kWithheldEffectCount = static_cast<uint32_t>(WithheldEffect::Count);

	// What one buffer the runtime submitted actually reached. A replay that
	// submits and draws nothing leaves the colour buffer exactly as it was,
	// which is also what a perfect replay looks like; these counts tell
	// the two apart.
	struct SubmissionSummary
	{
		uint32_t packetsProcessed;
		uint32_t drawsIssued;
		uint32_t withheld[kWithheldEffectCount]{};
	};

	class Observer
	{
	  public:
		virtual ~Observer() = default;

		virtual void OnDisplayList(const DisplayList& list) = 0;
		virtual void OnUniformAssembly(const UniformAssembly& assembly) = 0;
		virtual void OnPresent(const PresentArguments& present) = 0;
		// The guest's frame is finished -- every draw issued, its scan buffer
		// copied -- and its swap has not happened yet. This is the one moment
		// a frame of the runtime's own can be shown before the guest's, which
		// is where an in-between frame belongs.
		virtual void OnFrameComplete() = 0;
		// The guest's swap has been presented.
		virtual void OnFrameEnd() = 0;
		// Every draw the title issues, and whether it came out of a command
		// buffer the recorder was shown or straight from the ring. A recording
		// made of command buffers can only ever replay the first kind, so the
		// split is the denominator for how much of a frame a replay is.
		virtual void OnGuestDraw(bool fromCommandBuffer) = 0;
		// One per outermost runtime submission, after it has been processed.
		virtual void OnRuntimeSubmission(const SubmissionSummary& summary) = 0;
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

	// Present a frame the runtime has finished with, without waiting for the
	// guest's next swap. This builds the same two packets the guest emits --
	// the colour-buffer copy and the scan-buffer swap -- and feeds them
	// through the same command processor, so the encoding stays owned by the
	// code that already owns it and the runtime supplies only the arguments
	// it observed. False means nothing was submitted.
	bool SubmitPresent(const PresentArguments& present);

	// Copy a colour buffer to the scan buffer without swapping, so the guest's
	// own swap that follows presents it. Used to put the guest's frame back
	// after the runtime has drawn and presented one of its own in front of it.
	bool SubmitScanBufferCopy(const PresentArguments& present);

	// True while SubmitPresent is running. The swap packet it submits reaches
	// the same handler the guest's swap does, which would otherwise report a
	// frame end for a frame the guest never finished -- and re-enter whatever
	// the observer does there, from inside itself.
	bool InRuntimePresent();

	// True while a buffer the runtime submitted is being processed. The draws
	// and uniform buffers that come back during it are the runtime's own work.
	bool InRuntimeSubmission();

	// Marks the buffer being processed as the runtime's own, for as long as it
	// is in scope. Counted rather than set, so a nested submission does not
	// clear the mark when the inner one finishes.
	class RuntimeSubmission
	{
	  public:
		RuntimeSubmission();
		~RuntimeSubmission();
		RuntimeSubmission(const RuntimeSubmission&) = delete;
		RuntimeSubmission& operator=(const RuntimeSubmission&) = delete;
	};

	// Counted into the submission in progress, and ignored outside one. The
	// command processor calls these; nothing else should.
	void NoteRuntimePacket();
	void NoteRuntimeDraw();

	// True when a packet with this opcode must not execute because the
	// runtime submitted it, and counts it by class. Always false for the
	// guest's own buffers, and for the runtime's own present.
	bool WithholdFromRuntimeSubmission(uint32_t itCode);

	// The same decision for a render-target readback, which is initiated by
	// a draw rather than by a packet of its own.
	bool WithholdReadbackFromRuntimeSubmission();

	// True while a command buffer the guest submitted is being walked.
	bool InCommandBuffer();

	// Marks such a walk for as long as it is in scope. Counted, so a nested
	// buffer does not clear the mark when the inner one finishes.
	class CommandBufferWalk
	{
	  public:
		CommandBufferWalk();
		~CommandBufferWalk();
		CommandBufferWalk(const CommandBufferWalk&) = delete;
		CommandBufferWalk& operator=(const CommandBufferWalk&) = delete;
	};

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

