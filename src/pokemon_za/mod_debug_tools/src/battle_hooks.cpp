#include <hook_util.h>
#include <hk/hook/Trampoline.h>
#include <atomic>
#include <cstdint>
#include "logger/logger.h"
#include "externals/cmn/GameData.h"
#include "externals/pml/battle/Exp.h"
#include "externals/pml/Capture.h"
#include "externals/pml/pokepara/InitialSpec.h"
#include "externals/pml/pokepara/CalcTool.h"
#include "externals/ik/TrainerComponent.h"

// Settings
static bool isMustCapture = false;
static bool isExpShareOn = true;
static int expMultiplier = 1;
static bool expMultiplierInvert = false;
static bool alwaysMaxEnergy = false;

void setIsMustCapture(bool value) {
    isMustCapture = value;
}

void setIsExpShareOn(bool value) {
    isExpShareOn = value;
}

void setExpMultiplier(int value) {
    expMultiplier = value;
}

void setExpMultiplierInvert(bool value) {
    expMultiplierInvert = value;
}

void setAlwaysMaxEnergy(bool value) {
    alwaysMaxEnergy = value;
}

namespace {
    constexpr int PartySize = 6;
    constexpr int MoveCount = 4;
    constexpr uintptr_t CooldownFieldOffset = 0x64;

    std::atomic<bool> alwaysChargedPlayerParty{false};
    std::atomic<uintptr_t> playerPartyTargets[PartySize]{};
    std::atomic<int> cachedPlayerPartyTargetCount{0};

    int refreshPlayerPartyTargetsInternal() {
        // Stop writes while replacing the cache so the hook never observes a
        // partially refreshed set of party pointers.
        const bool resumeAfterRefresh =
            alwaysChargedPlayerParty.exchange(false, std::memory_order_acq_rel);

        uintptr_t nextTargets[PartySize]{};
        int targetCount = 0;

        auto party = cmn::GameData::GetPlayerParty();
        if (party.m_ptr != nullptr) {
            for (int slot = 0; slot < PartySize; ++slot) {
                auto member = party.m_ptr->GetMemberPtr(slot);
                nextTargets[slot] = reinterpret_cast<uintptr_t>(member.m_ptr);
                if (member.m_ptr != nullptr) {
                    ++targetCount;
                }
            }
        }

        for (int slot = 0; slot < PartySize; ++slot) {
            playerPartyTargets[slot].store(nextTargets[slot], std::memory_order_release);
        }
        cachedPlayerPartyTargetCount.store(targetCount, std::memory_order_release);

        if (resumeAfterRefresh) {
            alwaysChargedPlayerParty.store(true, std::memory_order_release);
        }

        return targetCount;
    }

    bool isCachedPlayerPartyTarget(const void* object) {
        const auto address = reinterpret_cast<uintptr_t>(object);
        if (address == 0) {
            return false;
        }

        for (int slot = 0; slot < PartySize; ++slot) {
            if (playerPartyTargets[slot].load(std::memory_order_acquire) == address) {
                return true;
            }
        }
        return false;
    }
}

void setAlwaysChargedPlayerParty(bool value) {
    if (!value) {
        alwaysChargedPlayerParty.store(false, std::memory_order_release);
        Logger::log("[PlayerCooldown] Always Charged Moves disabled\n");
        return;
    }

    const int targetCount = refreshPlayerPartyTargetsInternal();
    alwaysChargedPlayerParty.store(true, std::memory_order_release);
    Logger::log(
        "[PlayerCooldown] Always Charged Moves enabled with %d cached party target(s)\n",
        targetCount
    );
}

void refreshAlwaysChargedPartyTargets() {
    const int targetCount = refreshPlayerPartyTargetsInternal();
    Logger::log(
        "[PlayerCooldown] Refreshed party targets: %d cached target(s)\n",
        targetCount
    );
}

int getAlwaysChargedPartyTargetCount() {
    return cachedPlayerPartyTargetCount.load(std::memory_order_acquire);
}

PokemonData s_dataForEncounter = {};

HkTrampoline<void, void*, float> CooldownUpdateHook = hk::hook::trampoline([](void* object, float deltaTime) {
    // Preserve the game's normal update for every combatant first.
    CooldownUpdateHook.orig(object, deltaTime);

    if (
        !alwaysChargedPlayerParty.load(std::memory_order_acquire) ||
        object == nullptr ||
        !isCachedPlayerPartyTarget(object)
    ) {
        return;
    }

    // Tests on two different party slots confirmed that the object passed to
    // this function is the same ik::PokemonParam pointer returned by
    // GetPlayerParty(), and that its four move cooldown floats begin at +0x64.
    auto* cooldowns = reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(object) + CooldownFieldOffset
    );
    for (int move = 0; move < MoveCount; ++move) {
        cooldowns[move] = 0.0f;
    }
});

HkTrampoline<void, pml::Capture*> CaptureHook = hk::hook::trampoline([](pml::Capture* param_1) {
    CaptureHook.orig(param_1);

    if (isMustCapture) {
        param_1->m_result.isCaptured = true;
        param_1->m_result.yureCount = 3;
        for (float & i : param_1->m_result.m_indicator) {
            i = 1.0;
        }
    }
});

HkTrampoline<void, pml::battle::Exp::CalcResult*, pml::battle::Exp::CalcParam*> ExpCalcHook = hk::hook::trampoline([](pml::battle::Exp::CalcResult* result, pml::battle::Exp::CalcParam* param) {
    ExpCalcHook.orig(result, param);
    if (!param->didFight && !isExpShareOn) {
        result->exp = 0;
    } else {
        if (expMultiplier == 0) {
            result->exp = 0;
        } else {
            if (expMultiplierInvert) {
                auto expf = (float)result->exp;
                result->exp = (int)(expf / (float)expMultiplier);
            } else {
                result->exp *= expMultiplier;
            }
        }
    }
});

HkTrampoline<void, pml::pokepara::InitialSpec*> ChangeEncounterHook = hk::hook::trampoline([](pml::pokepara::InitialSpec* spec) {
    spec->rareTryCount *= s_dataForEncounter.shinyMultiplier;
    ChangeEncounterHook.orig(spec);
    if (s_dataForEncounter.forceShiny) {
        spec->colorRnd = pml::pokepara::CalcTool::CorrectColorRndForRare(spec->id, spec->colorRnd);
    }
    if (s_dataForEncounter.forceAlpha) {
        spec->oybn = true;
        spec->attributeScaling = 0xFF;
        spec->ev[0] = 0xFC;
        spec->level = std::min(spec->level + 10, 100);  // This seems to be loaded from romfs, but I cba to write parsing for that mess
        for (int i = 0; i < 6; i++) {  // This is also loaded from romfs but uses this logic as fallback
            spec->iv[i] = 0x1F;
        }
    }
    if (s_dataForEncounter.forceModify) {
        spec->monsNo = s_dataForEncounter.species;
        spec->formNo = s_dataForEncounter.form;
        spec->level = s_dataForEncounter.level;
        spec->sex = s_dataForEncounter.sex;
        spec->nature = s_dataForEncounter.nature;
        spec->natureMint = s_dataForEncounter.nature;
        spec->abilityIndex = s_dataForEncounter.ability;
        for (int i = 0; i < 6; i++) {
            spec->iv[i] = s_dataForEncounter.iv[i];
            spec->ev[i] = s_dataForEncounter.ev[i];
        }
    }
});

HkTrampoline<void, ik::TrainerComponent*, float> MegaEnergySetHook = hk::hook::trampoline([](ik::TrainerComponent* param_1, float param_2){
    if (alwaysMaxEnergy) {
        MegaEnergySetHook.orig(param_1, param_1->m_megaEnergyMax);
    } else {
        MegaEnergySetHook.orig(param_1, param_2);
    }
});

void battle_hooks() {
    CooldownUpdateHook.installAtSym<"ZA_DebugTools_CooldownUpdate202">();
    CaptureHook.installAtPtr(pun<void*>(&pml::Capture::Judge));
    ExpCalcHook.installAtPtr(&pml::battle::Exp::CalcExp);
    ChangeEncounterHook.installAtPtr(pun<void*>(&pml::pokepara::InitialSpec::FixInitSpec));
    MegaEnergySetHook.installAtPtr(pun<void*>(&ik::TrainerComponent::SetMegaEnergy));
}
