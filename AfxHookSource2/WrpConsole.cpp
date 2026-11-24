#include "WrpConsole.h"

#include <list>
#include <cstddef>
#include <cstring>

namespace {
	// VEngineCvar007 RegisterConVar input from the CS2 SDK (public/tier0/convar.h).
	// Its value buffers are deliberately byte-aligned, including int32 defaults.
	struct ConVarValueInfo {
		int version = 0;
		bool hasDefault = true, hasMin = true, hasMax = true;
		unsigned char initial[sizeof(SOURCESDK::CS2::CVValue_t)]{};
		unsigned char min[sizeof(SOURCESDK::CS2::CVValue_t)]{};
		unsigned char max[sizeof(SOURCESDK::CS2::CVValue_t)]{};
		void *changeProvider{}, *changeCallback{}, *filterProvider{}, *filterCallback{}, *enumInfo{};
		SOURCESDK::CS2::EConVarType type = SOURCESDK::CS2::EConVarType_Int32;
		void *completionCallback{};
		bool completionInterface = false;
	};
	struct ConVarCreation {
		const char *name, *help;
		uint64_t flags;
		ConVarValueInfo value{};
	};
	struct ConVarRef {
		uint16_t accessIndex = 0xffff;
		int registeredIndex = 0;
	};
	static_assert(offsetof(ConVarValueInfo, initial) == 7 && sizeof(ConVarRef) == 8);
}

SOURCESDK::CS2::Cvar_s *WrpRegisterIntConVar(const char *name, const char *help,
	int defaultValue, int minimum, int maximum) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return nullptr;
	auto existing = g_pCVar->FindConVar(name, false);
	if (existing.IsValid()) {
		auto data = g_pCVar->GetCvar(existing.Get());
		return data && data->m_eVarType == EConVarType_Int32 ? data : nullptr;
	}
	ConVarCreation info{name, help, SOURCESDK_CS2_FCVAR_CLIENTDLL | SOURCESDK_CS2_FCVAR_RELEASE |
		SOURCESDK_CS2_FCVAR_DONTRECORD};
	std::memcpy(info.value.initial, &defaultValue, sizeof(int));
	std::memcpy(info.value.min, &minimum, sizeof(int));
	std::memcpy(info.value.max, &maximum, sizeof(int));
	ConVarRef ref;
	Cvar_s *data = nullptr;
	// CS2's public/tier0/icvar.h places RegisterConVar after GetCompletionResults.
	// The vendored VEngineCvar007 declaration has an untyped placeholder in that slot.
	using Register = void (ICvar::*)(const ConVarCreation &, uint64_t, ConVarRef *, Cvar_s **);
	auto registerVar = reinterpret_cast<Register>(&ICvar::_Unknown_038);
	(g_pCVar->*registerVar)(info, 0, &ref, &data);
	return ref.accessIndex != 0xffff && data && data->m_eVarType == EConVarType_Int32 ? data : nullptr;
}

bool g_bComandsRegisterred = false;



std::list<class SOURCESDK::CS2::CCmd *> & GetCommands() {
    static std::list<class SOURCESDK::CS2::CCmd *> m_Commands;
    return m_Commands;
}

CWrpAddCommand::CWrpAddCommand(class SOURCESDK::CS2::CCmd * command) {
    GetCommands().emplace_back(command);
    if(SOURCESDK::CS2::g_pCVar) {
        SOURCESDK::CS2::g_pCVar->RegisterConCommand(command);
    }
}

void WrpRegisterCommands() {
    if(g_bComandsRegisterred) return;

    if(g_bComandsRegisterred = (nullptr != SOURCESDK::CS2::g_pCVar)) {
        for(auto it =  GetCommands().begin(); it !=  GetCommands().end(); it++) {
            SOURCESDK::CS2::g_pCVar->RegisterConCommand(*it);
        }
    }
}
