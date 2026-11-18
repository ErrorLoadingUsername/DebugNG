// Papyrus side of DebugNG: SKSE ModEvent storage (found via RTTI like SaveGuard NG)
// and snapshots of the Papyrus stacks that are running right now.

#include "PCH.h"
#include "debug.h"
#include "win.h"

namespace
{
	constexpr std::string_view kStorageTypeName = ".?AVSKSEPersistentObjectStorage@@";

	// VM fields (identical for SE and AE, see CommonLibSSE-NG VirtualMachine.h)
	constexpr std::uintptr_t kVMStacksLock     = 0x9318;  // BSSpinLock runningStacksLock
	constexpr std::uintptr_t kVMStacksCapacity = 0x932C;  // BSTHashMap capacity (u32)
	constexpr std::uintptr_t kVMStacksEntries  = 0x9348;  // BSTHashMap entries
	constexpr std::uintptr_t kHashEntrySize    = 0x18;    // {u32 key, Stack* value, next}

	struct Entry
	{
		void*         obj;            // ISKSEObject*
		std::uint32_t owningStackId;  // Papyrus stack that created it
		std::uint32_t pad;
	};
	static_assert(sizeof(Entry) == 0x10);

	struct SkseStorageLayout
	{
		void*        vtbl;       // 00
		std::byte    lock[40];   // 08 ICriticalSection
		Entry*       begin;      // 30 std::vector<Entry>
		Entry*       end;        // 38
		Entry*       cap;        // 40
		std::size_t* freeBegin;  // 48 std::vector<size_t> free indices
		std::size_t* freeEnd;    // 50
		std::size_t* freeCap;    // 58
	};
	static_assert(offsetof(SkseStorageLayout, begin) == 0x30);
	static_assert(offsetof(SkseStorageLayout, freeBegin) == 0x48);

	SkseStorageLayout* g_storage = nullptr;

	void Append(char* a_buf, std::size_t a_len, std::size_t& a_pos, const char* a_text)
	{
		for (; a_text && *a_text && a_pos + 1 < a_len; ++a_text) {
			a_buf[a_pos++] = *a_text;
		}
		a_buf[a_pos] = '\0';
	}

	bool DescribeStackRaw(void* a_stack, char* a_buf, std::size_t a_len)
	{
		std::size_t pos = 0;
		a_buf[0]        = '\0';
		__try {
			auto stack = static_cast<RE::BSScript::Stack*>(a_stack);
			auto frame = stack->top;
			for (int depth = 0; frame && depth < 4; ++depth, frame = frame->previousFrame) {
				auto fn = frame->owningFunction.get();
				if (!fn) {
					continue;
				}
				if (pos) {
					Append(a_buf, a_len, pos, " <- ");
				}
				Append(a_buf, a_len, pos, fn->GetObjectTypeName().c_str());
				Append(a_buf, a_len, pos, ".");
				Append(a_buf, a_len, pos, fn->GetName().c_str());
			}
			return true;
		} __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
			return false;
		}
	}

	std::uintptr_t FindStorageByRTTI(std::uintptr_t a_base, std::size_t& a_candidates)
	{
		a_candidates = 0;
		std::uintptr_t rdata = 0, data = 0;
		std::size_t    rdataSize = 0, dataSize = 0;
		if (!dbg::win::Section(a_base, ".rdata", rdata, rdataSize) || !dbg::win::Section(a_base, ".data", data, dataSize)) {
			SKSE::log::error("SKSE sections not found");
			return 0;
		}

		// 1) type descriptor: { vftable*, spare, "name" } - the name lives in .data
		std::uintptr_t td = 0;
		{
			const auto  name = kStorageTypeName;
			const auto* p    = reinterpret_cast<const char*>(data);
			for (std::size_t i = 0x10; i + name.size() < dataSize; ++i) {
				if (p[i] == name[0] && std::memcmp(p + i, name.data(), name.size()) == 0 && p[i + name.size()] == '\0') {
					td = data + i - 0x10;
					break;
				}
			}
		}
		if (!td) {
			SKSE::log::error("RTTI type {} not found in SKSE", kStorageTypeName);
			return 0;
		}
		const auto tdRVA = static_cast<std::uint32_t>(td - a_base);

		// 2) complete object locator in .rdata: { 1, offset 0, cdOffset, pTypeDescriptor, pClassHierarchy, pSelf }
		std::uintptr_t col = 0;
		for (std::size_t i = 0; i + 24 <= rdataSize; i += 4) {
			const auto* c    = reinterpret_cast<const std::uint32_t*>(rdata + i);
			const auto  self = static_cast<std::uint32_t>(rdata + i - a_base);
			if (c[0] == 1 && c[1] == 0 && c[3] == tdRVA && c[5] == self) {
				col = rdata + i;
				break;
			}
		}
		if (!col) {
			SKSE::log::error("RTTI object locator not found");
			return 0;
		}

		// 3) vtable: the qword in .rdata pointing at the locator, vtable starts right after it
		std::uintptr_t vtbl = 0;
		for (std::size_t i = 0; i + 16 <= rdataSize; i += 8) {
			if (*reinterpret_cast<const std::uintptr_t*>(rdata + i) == col) {
				vtbl = rdata + i + 8;
				break;
			}
		}
		if (!vtbl) {
			SKSE::log::error("vtable of SKSEPersistentObjectStorage not found");
			return 0;
		}

		// 4) the static instance: an object in .data whose first qword is that vtable
		std::uintptr_t found = 0;
		for (std::size_t i = 0; i + sizeof(SkseStorageLayout) <= dataSize; i += 8) {
			if (*reinterpret_cast<const std::uintptr_t*>(data + i) == vtbl) {
				++a_candidates;
				if (!found) {
					found = data + i;
				}
			}
		}
		SKSE::log::info("RTTI: type descriptor +{:X}, locator +{:X}, vtable +{:X}, {} instance(s)", tdRVA, col - a_base,
			vtbl - a_base, a_candidates);
		return a_candidates == 1 ? found : 0;
	}


	// all running stacks (pointer + id) - raw, guarded
	std::size_t CollectStacksRaw(std::uintptr_t a_vm, void** a_stacks, std::uint32_t* a_ids, std::size_t a_max, std::size_t& a_total)
	{
		std::size_t n = 0;
		a_total       = 0;
		__try {
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(a_vm + kVMStacksCapacity);
			const auto entries  = *reinterpret_cast<const std::uintptr_t*>(a_vm + kVMStacksEntries);
			if (!entries) {
				return 0;
			}
			for (std::uint32_t i = 0; i < capacity; ++i) {
				const auto e = entries + i * kHashEntrySize;
				if (*reinterpret_cast<const std::uintptr_t*>(e + 0x10) == 0) {
					continue;  // empty slot
				}
				const auto stack = *reinterpret_cast<void* const*>(e + 8);
				if (!stack) {
					continue;
				}
				++a_total;
				if (n < a_max) {
					a_stacks[n] = stack;
					a_ids[n]    = *reinterpret_cast<const std::uint32_t*>(e);
					++n;
				}
			}
		} __except (1) {
		}
		return n;
	}

	// the stack with this id, or nullptr
	void* FindStackRaw(std::uintptr_t a_vm, std::uint32_t a_id)
	{
		__try {
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(a_vm + kVMStacksCapacity);
			const auto entries  = *reinterpret_cast<const std::uintptr_t*>(a_vm + kVMStacksEntries);
			for (std::uint32_t i = 0; entries && i < capacity; ++i) {
				const auto e = entries + i * kHashEntrySize;
				if (*reinterpret_cast<const std::uintptr_t*>(e + 0x10) != 0 && *reinterpret_cast<const std::uint32_t*>(e) == a_id) {
					return *reinterpret_cast<void* const*>(e + 8);
				}
			}
		} __except (1) {
		}
		return nullptr;
	}

	std::uintptr_t VM() { return reinterpret_cast<std::uintptr_t>(RE::BSScript::Internal::VirtualMachine::GetSingleton()); }

	// ---- ModEvent tracking -----------------------------------------------------------
	struct Tracked
	{
		void*                                 obj{ nullptr };
		std::string                           name;
		std::string                           script;
		std::chrono::steady_clock::time_point created;
	};
	std::unordered_map<std::uint32_t, Tracked> g_tracked;  // handle -> entry
}

namespace dbg
{
	bool InitStorage()
	{
		std::wstring wname;
		const auto   base = win::FindSkseModule(wname);
		if (!base || win::CriticalSectionSize() != 40) {
			Log(kModEventStore, "SKSE DLL not found - ModEvent tracking off");
			return false;
		}
		std::size_t candidates = 0;
		const auto  address    = FindStorageByRTTI(base, candidates);
		if (!address) {
			Log(kModEventStore, "SKSEPersistentObjectStorage not found - ModEvent tracking off");
			return false;
		}
		g_storage = reinterpret_cast<SkseStorageLayout*>(address);
		Log(kModEventStore, fmt::format("SKSE object storage at +{:X}", address - base));
		return true;
	}

	// called every frame: logs new ModEvents (with the script that created them)
	// and the ones that were sent / released
	void PollModEvents()
	{
		if (!g_storage) {
			return;
		}
		struct Seen
		{
			std::uint32_t handle;
			void*         obj;
			std::uint32_t stack;
		};
		std::vector<Seen> now;
		win::Lock(g_storage->lock);
		for (auto e = g_storage->begin; e && e < g_storage->end; ++e) {
			if (e->obj) {
				now.push_back({ static_cast<std::uint32_t>(e - g_storage->begin) + 1, e->obj, e->owningStackId });
			}
		}
		win::Unlock(g_storage->lock);

		const auto tick = std::chrono::steady_clock::now();
		std::unordered_set<std::uint32_t> present;
		for (const auto& s : now) {
			present.insert(s.handle);
			auto it = g_tracked.find(s.handle);
			if (it != g_tracked.end() && it->second.obj == s.obj) {
				continue;  // known
			}
			Tracked t;
			t.obj     = s.obj;
			t.created = tick;
			char buf[128]{};
			if (win::SafeClassName(s.obj, buf, sizeof(buf))) {
				t.name = buf;
				if (t.name == "PapyrusModEvent" && win::SafeStringAt(s.obj, 0x10, buf, sizeof(buf))) {
					t.name = buf;
				}
			}
			if (auto vm = VM()) {
				auto&               lock = *reinterpret_cast<RE::BSSpinLock*>(vm + kVMStacksLock);
				RE::BSSpinLockGuard guard(lock);
				if (auto stack = FindStackRaw(vm, s.stack)) {
					char where[256]{};
					if (DescribeStackRaw(stack, where, sizeof(where))) {
						t.script = where;
					}
				}
			}
			Log(kModEventStore, fmt::format("create  #{:<4} {:<32} {}", s.handle, t.name, t.script.empty() ? "(script already finished)" : t.script));
			g_tracked[s.handle] = std::move(t);
		}
		for (auto it = g_tracked.begin(); it != g_tracked.end();) {
			if (!present.contains(it->first)) {
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tick - it->second.created).count();
				LogDebug(kModEventStore, fmt::format("gone    #{:<4} {:<32} after {} ms (sent / released)", it->first, it->second.name, ms));
				it = g_tracked.erase(it);
			} else {
				++it;
			}
		}
	}

	std::size_t RunningStackCount()
	{
		auto vm = VM();
		if (!vm) {
			return 0;
		}
		void*               stacks[1];
		std::uint32_t       ids[1];
		std::size_t         total = 0;
		auto&               lock  = *reinterpret_cast<RE::BSSpinLock*>(vm + kVMStacksLock);
		RE::BSSpinLockGuard guard(lock);
		CollectStacksRaw(vm, stacks, ids, 0, total);
		return total;
	}

	std::vector<std::string> StackLines(const char* a_reason, int a_max)
	{
		std::vector<std::string> out;
		auto vm = VM();
		if (!vm) {
			return out;
		}
		constexpr std::size_t kMax = 64;
		void*                 stacks[kMax];
		std::uint32_t         ids[kMax];
		std::size_t           total = 0;
		std::vector<std::string> lines;
		{
			auto&               lock = *reinterpret_cast<RE::BSSpinLock*>(vm + kVMStacksLock);
			RE::BSSpinLockGuard guard(lock);
			const auto          n = CollectStacksRaw(vm, stacks, ids, kMax, total);
			(void)ids;
			for (std::size_t i = 0; i < n; ++i) {
				char where[256]{};
				if (DescribeStackRaw(stacks[i], where, sizeof(where)) && where[0]) {
					lines.emplace_back(where);
				}
			}
		}
		// the same script usually shows up many times - group them
		std::unordered_map<std::string, int> count;
		std::vector<std::string>             order;
		for (const auto& l : lines) {
			if (count[l]++ == 0) {
				order.push_back(l);
			}
		}
		std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) { return count[a] > count[b]; });
		out.push_back(fmt::format("  Papyrus ({}): {} running stack(s), {} with script frames", a_reason, total, lines.size()));
		int shown = 0;
		for (const auto& key : order) {
			if (a_max > 0 && shown++ >= a_max) {
				out.push_back(fmt::format("    ... {} more", order.size() - a_max));
				break;
			}
			out.push_back(fmt::format("    {:>3}x  {}", count[key], key));
		}
		return out;
	}

	void DumpStacks(const char* a_reason, int a_max)
	{
		for (const auto& line : StackLines(a_reason, a_max)) {
			LogWarn(kHitch, line);
		}
	}
}
