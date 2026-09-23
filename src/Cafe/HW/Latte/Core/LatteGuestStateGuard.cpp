#include "Cafe/HW/Latte/Core/LatteGuestStateGuard.h"

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LatteGuestStateGuard
{
	namespace
	{
		struct Subresource
		{
			LatteTexture* texture;
			sint32 sliceIndex;
			sint32 mipIndex;

			bool operator==(const Subresource&) const = default;
		};

		struct SubresourceHash
		{
			size_t operator()(const Subresource& subresource) const
			{
				size_t hash = std::hash<LatteTexture*>{}(subresource.texture);
				hash ^= std::hash<sint32>{}(subresource.sliceIndex) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
				hash ^= std::hash<sint32>{}(subresource.mipIndex) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
				return hash;
			}
		};

		// Owned by the Latte thread, which is the only thread that draws.
		struct Guard
		{
			bool open = false;
			// Kept between guards: the same targets are written every frame,
			// and allocating their copies once is the difference between a
			// copy that costs a transfer and one that costs an allocation.
			// A copy goes when its texture does.
			std::unordered_map<Subresource, std::unique_ptr<LatteTextureShadow>, SubresourceHash> shadows;
			// What this guard copied aside, in the order it did.
			std::vector<Subresource> kept;
			std::unordered_set<Subresource, SubresourceHash> keptSet;
			std::unordered_set<LatteTexture*> created;
			uint32_t uncopied = 0;
			uint32_t streamoutWrites = 0;
		};

		Guard s_guard;

		LatteTextureShadow* ShadowFor(const Subresource& subresource)
		{
			auto found = s_guard.shadows.find(subresource);
			if (found != s_guard.shadows.end())
			{
				return found->second.get();
			}
			std::unique_ptr<LatteTextureShadow> shadow =
				g_renderer->texture_createShadow(subresource.texture, subresource.sliceIndex, subresource.mipIndex);
			if (!shadow)
			{
				return nullptr;
			}
			return s_guard.shadows.emplace(subresource, std::move(shadow)).first->second.get();
		}
	} // namespace

	void Open()
	{
		cemu_assert_debug(!s_guard.open);
		s_guard.open = true;
		s_guard.kept.clear();
		s_guard.keptSet.clear();
		s_guard.created.clear();
		s_guard.uncopied = 0;
		s_guard.streamoutWrites = 0;
	}

	LatteFrameHooks::GuestStateRestore Close()
	{
		cemu_assert_debug(s_guard.open);
		LatteFrameHooks::GuestStateRestore restore{};
		for (const Subresource& subresource : s_guard.kept)
		{
			g_renderer->texture_copyFromShadow(subresource.texture, subresource.sliceIndex, subresource.mipIndex,
											   *s_guard.shadows.at(subresource));
			++restore.subresourcesRestored;
		}
		restore.subresourcesUncopied = s_guard.uncopied;
		restore.texturesCreated = static_cast<uint32_t>(s_guard.created.size());
		restore.streamoutWrites = s_guard.streamoutWrites;
		s_guard.open = false;
		s_guard.kept.clear();
		s_guard.keptSet.clear();
		s_guard.created.clear();
		return restore;
	}

	bool IsOpen()
	{
		return s_guard.open;
	}

	void NoteWrite(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex)
	{
		if (!s_guard.open || s_guard.created.contains(texture))
		{
			return;
		}
		Subresource subresource{texture, sliceIndex, mipIndex};
		if (!s_guard.keptSet.insert(subresource).second)
		{
			return;
		}
		LatteTextureShadow* shadow = ShadowFor(subresource);
		if (shadow == nullptr)
		{
			++s_guard.uncopied;
			return;
		}
		g_renderer->texture_copyToShadow(texture, sliceIndex, mipIndex, *shadow);
		s_guard.kept.push_back(subresource);
	}

	void NoteCreated(LatteTexture* texture)
	{
		if (s_guard.open)
		{
			s_guard.created.insert(texture);
		}
	}

	void NoteDeleted(LatteTexture* texture)
	{
		std::erase_if(s_guard.shadows, [texture](const auto& entry) { return entry.first.texture == texture; });
		if (!s_guard.open)
		{
			return;
		}
		// A texture created and deleted inside the guard leaves nothing
		// behind. One deleted after being copied aside has nothing left to
		// restore into; drawing the guest's frame again would have deleted
		// it too.
		s_guard.created.erase(texture);
		std::erase_if(s_guard.kept, [texture](const Subresource& subresource) { return subresource.texture == texture; });
		std::erase_if(s_guard.keptSet, [texture](const Subresource& subresource) { return subresource.texture == texture; });
	}

	void NoteStreamoutWrite()
	{
		if (s_guard.open)
		{
			++s_guard.streamoutWrites;
		}
	}
} // namespace LatteGuestStateGuard
