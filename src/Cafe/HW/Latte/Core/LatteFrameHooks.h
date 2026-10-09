#pragma once

#include <cstdint>
#include <functional>
#include <vector>

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
	// UniformAssembly::stageIndex of a pixel shader's uniforms.
	inline constexpr uint32_t kPixelStageIndex = 1;

	struct UniformAssembly
	{
		uint64_t shaderBaseHash;
		uint64_t shaderAuxHash;
		uint32_t stageIndex;
		float* data;
		uint32_t sizeInBytes;
		// Word 0 of the uniform-block register bank the draw's *shader* names, as
		// (bufferId, value) pairs: blockAddressCount counts pairs, so the array
		// holds twice that many words.
		//
		// **This is not the block the draw sourced, and it is not an identity for
		// the object being drawn.** The comment used to say it was both. The
		// reader below picks the bank by the shader's own group
		// (`kcacheBankIdOffset`), while the guest writes the bank by the index it
		// passes to `GX2Set*UniformBlock` -- two different numbers, so the value
		// read is whatever last wrote that register slot, not this draw's block.
		// Measured on the real title: Wind Waker's binder (0x027ff88c) hands
		// `GX2Set*UniformBlock` the pair (0x40, 0x40) for every object, so the
		// register holds 0x40 where the binder wrote, and the 1,555 distinct values
		// observed over 382,575 sourced addresses come from the slots the binder
		// never touched. Over 142,682 exact per-object pairs, no word of the
		// descriptor record matched any of these values (best word: 3 hits).
		//
		// Kept, because it is a faithful reading of the register bank and a
		// consumer may want it -- but nothing may treat it as the draw's block
		// address or as an object's identity.
		const uint32_t* blockAddresses;
		uint32_t blockAddressCount;
		// **The guest address the title passed for the same slots, one per pair of
		// `blockAddresses`.** `blockAddresses` is what the register holds, which is
		// `memory_virtualToPhysical` of this, and the title's own descriptor record names its
		// block by the address it passed to `GX2Set*UniformBlock`. A consumer holding a record
		// word and wanting to know which draw it belongs to has to compare like with like, and a
		// consumer wanting to read or write the block's bytes has to use this one: a block is
		// reachable at its guest address, and not at its physical offset, by any reader a
		// runtime holds.
		const uint32_t* blockGuestAddresses;
		// Word 1 of the same register slots, which is `size - 1` as the guest wrote it.
		//
		// **This is the half that says the slot was written.** Word 0 is whatever last held the
		// slot, and the guest and the shader index these registers differently, so word 0 alone
		// cannot be told from register state the guest never set. Word 1 is a small constant the
		// guest does write -- Wind Waker's binder passes 0x40 for every object, so every slot it
		// filled holds 0x3f here -- and a slot holding 0x3f is a 64-byte block the title put there.
		// Parallel to `blockAddresses`: one word per pair.
		const uint32_t* blockSizes;
		uint32_t blockSizeCount;
		// Whether the draw writes any colour buffer. One that writes depth
		// alone renders a map a later draw of the frame looks up -- a shadow
		// map -- rather than anything seen.
		bool writesColour;
		// Whether this stage compares against a depth texture -- looks up a
		// map such as the light's, which the frame drew before this draw.
		bool looksUpDepthMap;
		// As in DisplayList: whose draw this is. It is also the moment a
		// substitution belongs to -- a blend edits the runtime's own replay
		// and never the frame the guest is drawing.
		bool fromRuntime;
		// Host address of the draw packet being executed, keyed like GX2's write position.
		uintptr_t packet;
	};

	// One draw about to be issued, after its uniforms were assembled. A draw
	// whose vertex shader reads no uniforms at all positions its geometry from
	// vertex data alone: nothing a uniform substitution writes can move it, so
	// a runtime that blends uniforms counts these as the draws it cannot.
	//
	// Its vertex buffers are handed over as the guest memory the draw reads,
	// up to the highest vertex and instance it fetches: whether that geometry
	// moves is then a question of whether those bytes change between frames.
	struct DrawPrepared
	{
		// One of a draw's vertex buffers, valid only during the callback.
		struct VertexBuffer
		{
			const void* data;
			uint32_t sizeInBytes;
			// Bytes from one vertex (or instance) to the next.
			uint32_t stride;
			// The attribute buffer slot the fetch shader reads it from.
			uint32_t slot;
			// Vertices the draw reads from it by vertex index; 0 for per-instance data only.
			uint32_t vertices;
		};

		// One value the fetch shader reads per vertex, as the guest laid it
		// out: `sizeInBytes` at `offset` into each stride of vertexBuffers[buffer].
		struct VertexAttribute
		{
			uint32_t buffer;
			uint32_t offset;
			uint32_t sizeInBytes;
			// Latte's data format, and its VertexFetchEndianMode.
			uint8_t format;
			uint8_t endianSwap;
			// Which vertex shader input it feeds.
			uint8_t semanticId;
			bool perInstance;
		};

		// The attribute buffers a fetch shader can source.
		static constexpr uint32_t kMaxVertexBuffers = 16;
		// Bounds the attribute list as kMaxUniformBlockSources bounds the
		// block list: real fetch shaders read a handful.
		static constexpr uint32_t kMaxVertexAttributes = 32;

		uint64_t vertexShaderBaseHash;
		uint64_t vertexShaderAuxHash;
		bool vertexUniforms;
		bool fromRuntime;
		VertexBuffer vertexBuffers[kMaxVertexBuffers];
		uint32_t vertexBufferCount;
		VertexAttribute vertexAttributes[kMaxVertexAttributes];
		uint32_t vertexAttributeCount;
		// Host address of the draw packet being executed, keyed like GX2's write position.
		uintptr_t packet;
	};

	// Bytes a draw reads in place of its vertex buffers: data[i],
	// when set, replaces vertexBuffers[i] and is as long, laid out alike. The
	// renderer copies them into memory of its own before the call returns,
	// so a replacement never writes the guest's vertex data -- which the
	// guest's own frames go on reading.
	struct VertexReplacements
	{
		const void* data[DrawPrepared::kMaxVertexBuffers]{};
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

	// How far along its way to the screen a shown frame's time was taken, as
	// far as the surface can report it: the later, the nearer what a player
	// sees.
	enum class ShownStage : uint8_t
	{
		// The GPU finished the present's queue operations.
		QueueDone,
		// The presentation engine took the image to be shown.
		Dequeued,
		// Its first pixel left for the display.
		FirstPixelOut,
		// Its first pixel was visible on the display.
		FirstPixelVisible,
	};

	// A frame handed to the display, and when it reached the screen.
	struct ShownFrame
	{
		bool fromRuntime;
		ShownStage stage;
		// In nanoseconds of the clock `timeDomainId` names: times of two
		// frames of the same domain can be subtracted, others not.
		uint64_t timeNanoseconds;
		uint64_t timeDomainId;
	};

	// One IT_SET_ALU_CONST packet as it reaches the register file.
	struct AluConstants
	{
		// The packet's first data word, as DrawPacket() names a draw.
		uintptr_t packet;
		// In 32-bit words from the ALU constant base; vertex constants start at 0x400.
		uint32_t firstWord;
		// The register file's copy, host order: an observer may rewrite it.
		uint32_t* values;
		uint32_t count;
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
		// Every frame handed to the display, the guest's or the runtime's, as
		// the renderer returns from presenting it. The interval between two is
		// the frame time a player sees, which the guest's swaps alone stop
		// describing once the runtime shows frames of its own.
		virtual void OnDisplayed(bool fromRuntime) = 0;
		// When a frame handed to the display was shown, as the presentation
		// engine reports it, a few frames after its present: what the player
		// sees, which OnDisplayed's times are not once a present returns
		// before its frame is shown. Only a surface that reports presentation
		// times (VK_EXT_present_timing) calls it, once per frame, in order.
		virtual void OnShown(const ShownFrame& shown) = 0;
		// Every draw the title issues, and whether it came out of a command
		// buffer the recorder was shown or straight from the ring. A recording
		// made of command buffers can only ever replay the first kind, so the
		// split is the denominator for how much of a frame a replay is.
		virtual void OnGuestDraw(bool fromCommandBuffer) = 0;
		// Every draw the renderer issues, the title's or the runtime's, once its
		// uniforms are assembled.
		// The draw reads any replacements the observer sets instead of the
		// guest's buffers; the draw after binds the guest's buffers again.
		virtual void OnDrawPrepared(const DrawPrepared& draw, VertexReplacements& replacements) = 0;
		// One per outermost runtime submission, after it has been processed.
		virtual void OnRuntimeSubmission(const SubmissionSummary& summary) = 0;
		// Every IT_SET_ALU_CONST packet, after its values reached the register
		// file and before any draw reads them. Returns whether it rewrote them.
		virtual bool OnAluConstants(const AluConstants& constants) = 0;
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

	// Capture the next frame the title presents. One-shot by default: a capture
	// that repeated every frame would make a replayed image impossible to tell
	// from the one after it. False means no capture was armed, which is a refusal
	// rather than a capture that silently never arrives.
	//
	// `count` asks for that many *consecutive* presents, which is a different
	// question from asking twice: the renderer holds one screenshot request at a
	// time, so a second arm taken after the first lands waits a whole frame, and
	// in a title that animates that is a different picture. Two consecutive
	// presents are the two paints of one pass when a stand-in paints twice, and
	// comparing them is the null case.
	bool RequestFrameCapture(CaptureCallback callback, int count = 1);

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
	// Where the calling guest core's next GX2 packet goes, as host addresses;
	// all zero with no command buffer open.
	struct CommandWritePosition
	{
		uintptr_t bufferStart;
		uintptr_t bufferEnd;
		uintptr_t write;
	};

	CommandWritePosition GetCommandWritePosition();

	// The draw packet the Latte thread is executing, as a host address into
	// the command buffer the guest wrote; 0 outside a draw packet.
	uintptr_t DrawPacket();

	// Hands an ALU constant packet to the observer; whether it rewrote the values.
	bool NoteAluConstants(const void* packet, uint32_t firstWord, uint32_t* values, uint32_t count);

	class DrawPacketScope
	{
	  public:
		explicit DrawPacketScope(const void* packet);
		~DrawPacketScope();
		DrawPacketScope(const DrawPacketScope&) = delete;
		DrawPacketScope& operator=(const DrawPacketScope&) = delete;
	};

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

	// One span of guest memory the emulator has mapped: where it sits in the
	// guest's address space, and where its bytes are on the host.
	struct GuestMemoryRegion
	{
		uint32_t guestAddress;
		const uint8_t* bytes;
		uint32_t size;
	};

	// Every span of guest memory mapped now, for a diagnostic that must see
	// whether the runtime changed anything the guest can read. The bytes are
	// written by the guest's own threads while they run.
	std::vector<GuestMemoryRegion> MappedGuestMemory();

	// The host bytes behind `size` bytes of the guest's *physical* space at
	// `physicalOffset`, or null unless all of them are inside it.
	//
	// **This is the other kind of address, and it is the one a uniform block is
	// registered by.** `UniformAssembly::blockAddresses` holds what the guest
	// passed to `GX2Set*UniformBlock`, and `LatteBufferData.cpp` reads it back as
	// `memory_base + physicalAddr` -- a physical offset, not a virtual one. Every
	// reader of that value has been handed it as though it were a guest address,
	// which is why the searches for a pose in a block found nothing there: they
	// were reading a different address space. A consumer that holds a block
	// address reaches the block's bytes through here, and through nothing else.
	const void* PhysicalBytes(uint32_t physicalOffset, uint32_t size);

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

