#include <hook_util.h>
#include <hk/hook/Trampoline.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "logger/logger.h"
#include "externals/cmn/GameData.h"
#include "externals/pml/battle/Exp.h"
#include "externals/pml/Capture.h"
#include "externals/pml/pokepara/InitialSpec.h"
#include "externals/pml/pokepara/CoreParam.h"
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
    constexpr uintptr_t CooldownFieldOffset = 0x64;
    constexpr int MoveCount = 4;
    constexpr int PartySize = 6;
    constexpr int ObjectQwordCount = 20;
    constexpr uintptr_t ObjectQwordStart = 0x08;
    constexpr int ObservedObjectCount = 64;
    constexpr float CooldownJumpThreshold = 0.25f;

    enum class CaptureTarget : int {
        None = 0,
        Player = 1,
        Opponent = 2,
    };

    struct ObservedObject {
        void* object = nullptr;
        float lastAfter[MoveCount] = {};
        uint64_t lastSeen = 0;
        bool initialized = false;
    };

    struct AtomicCapture {
        std::atomic<uint64_t> object{0};
        std::atomic<uint64_t> vtable{0};
        std::atomic<uint64_t> qwords[ObjectQwordCount]{};
        std::atomic<uint32_t> beforeBits[MoveCount]{};
        std::atomic<uint32_t> afterBits[MoveCount]{};
        std::atomic<uint32_t> eventMove{0xFFFFFFFFu};
        std::atomic<uint64_t> eventCounter{0};
        std::atomic_flag writeLock = ATOMIC_FLAG_INIT;
        std::atomic<uint32_t> sequence{0};
        std::atomic<uint32_t> captureNumber{0};
    };

    struct PartyPointers {
        uint64_t party = 0;
        uint64_t ikParam[PartySize] = {};
        uint64_t pmlParam[PartySize] = {};
        uint64_t accessor[PartySize] = {};
        uint64_t coreData[PartySize] = {};
        uint64_t calcData[PartySize] = {};
        uint32_t personalRnd[PartySize] = {};
        uint32_t trainerId[PartySize] = {};
        uint32_t colorRnd[PartySize] = {};
        uint16_t species[PartySize] = {};
        uint16_t form[PartySize] = {};
    };

    ObservedObject observedObjects[ObservedObjectCount]{};
    uint64_t hookCounter = 0;
    std::atomic<int> armedTarget{static_cast<int>(CaptureTarget::None)};
    AtomicCapture playerCapture{};
    AtomicCapture opponentCapture{};
    PartyPointers partyPointers{};
    char mapperText[16384]{};

    uint32_t floatToBits(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    float bitsToFloat(uint32_t bits) {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    ObservedObject* findObservedObject(void* object) {
        ObservedObject* empty = nullptr;
        ObservedObject* oldest = &observedObjects[0];

        for (auto& entry : observedObjects) {
            if (entry.object == object) {
                return &entry;
            }
            if (entry.object == nullptr && empty == nullptr) {
                empty = &entry;
            }
            if (entry.lastSeen < oldest->lastSeen) {
                oldest = &entry;
            }
        }

        auto* result = empty != nullptr ? empty : oldest;
        *result = {};
        result->object = object;
        return result;
    }

    struct CaptureSnapshot {
        uint64_t object = 0;
        uint64_t vtable = 0;
        uint64_t qwords[ObjectQwordCount] = {};
        uint32_t beforeBits[MoveCount] = {};
        uint32_t afterBits[MoveCount] = {};
        uint32_t eventMove = 0xFFFFFFFFu;
        uint64_t eventCounter = 0;
        uint32_t captureNumber = 0;
    };

    int appendFormat(char* output, int capacity, int used, const char* format, ...) {
        if (output == nullptr || capacity <= 0) {
            return 0;
        }
        if (used < 0) {
            used = 0;
        }
        if (used >= capacity - 1) {
            output[capacity - 1] = '\0';
            return capacity - 1;
        }

        va_list args;
        va_start(args, format);
        const int remaining = capacity - used;
        const int written = std::vsnprintf(output + used, static_cast<size_t>(remaining), format, args);
        va_end(args);

        if (written < 0) {
            output[used] = '\0';
            return used;
        }
        if (written >= remaining) {
            output[capacity - 1] = '\0';
            return capacity - 1;
        }
        return used + written;
    }

    void beginCaptureWrite(AtomicCapture& capture) {
        while (capture.writeLock.test_and_set(std::memory_order_acquire)) {
        }
        // Odd sequence values mean a writer is updating this record.
        capture.sequence.fetch_add(1, std::memory_order_acq_rel);
    }

    void endCaptureWrite(AtomicCapture& capture) {
        // Even sequence values mean readers can take a coherent snapshot.
        capture.sequence.fetch_add(1, std::memory_order_release);
        capture.writeLock.clear(std::memory_order_release);
    }

    void clearCapture(AtomicCapture& capture) {
        beginCaptureWrite(capture);
        capture.object.store(0, std::memory_order_relaxed);
        capture.vtable.store(0, std::memory_order_relaxed);
        for (auto& value : capture.qwords) {
            value.store(0, std::memory_order_relaxed);
        }
        for (auto& value : capture.beforeBits) {
            value.store(0, std::memory_order_relaxed);
        }
        for (auto& value : capture.afterBits) {
            value.store(0, std::memory_order_relaxed);
        }
        capture.eventMove.store(0xFFFFFFFFu, std::memory_order_relaxed);
        capture.eventCounter.store(0, std::memory_order_relaxed);
        capture.captureNumber.store(0, std::memory_order_relaxed);
        endCaptureWrite(capture);
    }

    void saveCapture(
        AtomicCapture& capture,
        void* object,
        const float before[MoveCount],
        const float after[MoveCount],
        uint32_t eventMove
    ) {
        // The allocation path for this concrete class requests 0xA8 bytes. Reading
        // aligned qwords from +0x08 through +0xA0 therefore stays within the object.
        const auto objectAddress = reinterpret_cast<uintptr_t>(object);
        const auto* qwords = reinterpret_cast<const uint64_t*>(objectAddress + ObjectQwordStart);
        const auto vtable = *reinterpret_cast<const uint64_t*>(objectAddress);

        beginCaptureWrite(capture);
        capture.object.store(objectAddress, std::memory_order_relaxed);
        capture.vtable.store(vtable, std::memory_order_relaxed);
        for (int i = 0; i < ObjectQwordCount; ++i) {
            capture.qwords[i].store(qwords[i], std::memory_order_relaxed);
        }
        for (int i = 0; i < MoveCount; ++i) {
            capture.beforeBits[i].store(floatToBits(before[i]), std::memory_order_relaxed);
            capture.afterBits[i].store(floatToBits(after[i]), std::memory_order_relaxed);
        }
        capture.eventMove.store(eventMove, std::memory_order_relaxed);
        capture.eventCounter.store(hookCounter, std::memory_order_relaxed);
        capture.captureNumber.fetch_add(1, std::memory_order_relaxed);
        endCaptureWrite(capture);
    }

    bool readCapture(const AtomicCapture& capture, CaptureSnapshot& snapshot) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t beforeSequence = capture.sequence.load(std::memory_order_acquire);
            if ((beforeSequence & 1u) != 0) {
                continue;
            }

            snapshot.object = capture.object.load(std::memory_order_relaxed);
            snapshot.vtable = capture.vtable.load(std::memory_order_relaxed);
            for (int i = 0; i < ObjectQwordCount; ++i) {
                snapshot.qwords[i] = capture.qwords[i].load(std::memory_order_relaxed);
            }
            for (int i = 0; i < MoveCount; ++i) {
                snapshot.beforeBits[i] = capture.beforeBits[i].load(std::memory_order_relaxed);
                snapshot.afterBits[i] = capture.afterBits[i].load(std::memory_order_relaxed);
            }
            snapshot.eventMove = capture.eventMove.load(std::memory_order_relaxed);
            snapshot.eventCounter = capture.eventCounter.load(std::memory_order_relaxed);
            snapshot.captureNumber = capture.captureNumber.load(std::memory_order_relaxed);

            const uint32_t afterSequence = capture.sequence.load(std::memory_order_acquire);
            if (beforeSequence == afterSequence && (afterSequence & 1u) == 0) {
                return true;
            }
        }
        return false;
    }

    const char* pointerMatch(uint64_t value, int& slotOut) {
        if (value == 0) {
            return nullptr;
        }
        if (value == partyPointers.party) {
            slotOut = -1;
            return "party";
        }
        for (int slot = 0; slot < PartySize; ++slot) {
            if (value == partyPointers.ikParam[slot]) {
                slotOut = slot;
                return "ik::PokemonParam";
            }
            if (value == partyPointers.pmlParam[slot]) {
                slotOut = slot;
                return "pml::pokepara::PokemonParam";
            }
            if (value == partyPointers.accessor[slot]) {
                slotOut = slot;
                return "Accessor";
            }
            if (value == partyPointers.coreData[slot]) {
                slotOut = slot;
                return "CoreData";
            }
            if (value == partyPointers.calcData[slot]) {
                slotOut = slot;
                return "CalcData";
            }
        }
        return nullptr;
    }

    const char* identityMatch32(uint32_t value, int& slotOut) {
        if (value == 0) {
            return nullptr;
        }
        for (int slot = 0; slot < PartySize; ++slot) {
            if (value == partyPointers.personalRnd[slot]) {
                slotOut = slot;
                return "personalRnd";
            }
            if (value == partyPointers.trainerId[slot]) {
                slotOut = slot;
                return "trainer ID";
            }
            if (value == partyPointers.colorRnd[slot]) {
                slotOut = slot;
                return "colorRnd";
            }
            const uint32_t speciesForm =
                static_cast<uint32_t>(partyPointers.species[slot]) |
                (static_cast<uint32_t>(partyPointers.form[slot]) << 16);
            if (value == speciesForm) {
                slotOut = slot;
                return "species|form";
            }
        }
        return nullptr;
    }

    int appendCaptureText(char* output, int capacity, int used, const char* label, const AtomicCapture& capture) {
        CaptureSnapshot snapshot{};
        if (!readCapture(capture, snapshot)) {
            return appendFormat(output, capacity, used, "\n%s capture: snapshot busy; reopen the window.\n", label);
        }

        used = appendFormat(
            output,
            capacity,
            used,
            "\n%s capture (number %u)\nObject: 0x%llX | VTable: 0x%llX | Event move: %d | Hook tick: %llu\n",
            label,
            snapshot.captureNumber,
            static_cast<unsigned long long>(snapshot.object),
            static_cast<unsigned long long>(snapshot.vtable),
            snapshot.eventMove < MoveCount ? static_cast<int>(snapshot.eventMove) : -1,
            static_cast<unsigned long long>(snapshot.eventCounter)
        );

        if (snapshot.object == 0) {
            return appendFormat(output, capacity, used, "No capture yet.\n");
        }

        used = appendFormat(output, capacity, used, "Cooldown before:");
        for (int i = 0; i < MoveCount; ++i) {
            used = appendFormat(output, capacity, used, " %.3f", bitsToFloat(snapshot.beforeBits[i]));
        }
        used = appendFormat(output, capacity, used, "\nCooldown after: ");
        for (int i = 0; i < MoveCount; ++i) {
            used = appendFormat(output, capacity, used, " %.3f", bitsToFloat(snapshot.afterBits[i]));
        }
        used = appendFormat(
            output,
            capacity,
            used,
            "\nObject qwords (+0x08 through +0xA0; direct pointer and 32-bit identity matches):\n"
        );

        for (int i = 0; i < ObjectQwordCount; ++i) {
            const auto offset = ObjectQwordStart + static_cast<uintptr_t>(i * sizeof(uint64_t));
            const auto value = snapshot.qwords[i];
            int pointerSlot = -2;
            const char* pointerLabel = pointerMatch(value, pointerSlot);

            used = appendFormat(
                output,
                capacity,
                used,
                "+0x%02llX: 0x%016llX",
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(value)
            );

            if (pointerLabel != nullptr && pointerSlot < 0) {
                used = appendFormat(output, capacity, used, "  <-- POINTER MATCH %s", pointerLabel);
            } else if (pointerLabel != nullptr) {
                used = appendFormat(
                    output,
                    capacity,
                    used,
                    "  <-- POINTER MATCH slot %d %s",
                    pointerSlot,
                    pointerLabel
                );
            }

            const uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            const uint32_t high = static_cast<uint32_t>(value >> 32);
            int lowSlot = -1;
            int highSlot = -1;
            const char* lowLabel = identityMatch32(low, lowSlot);
            const char* highLabel = identityMatch32(high, highSlot);
            if (lowLabel != nullptr) {
                used = appendFormat(
                    output,
                    capacity,
                    used,
                    "  <-- LOW32 MATCH slot %d %s",
                    lowSlot,
                    lowLabel
                );
            }
            if (highLabel != nullptr) {
                used = appendFormat(
                    output,
                    capacity,
                    used,
                    "  <-- HIGH32 MATCH slot %d %s",
                    highSlot,
                    highLabel
                );
            }
            used = appendFormat(output, capacity, used, "\n");
        }
        return used;
    }

}

void cooldownMapperArmPlayer() {
    armedTarget.store(static_cast<int>(CaptureTarget::Player), std::memory_order_release);
    Logger::log("[CooldownMapper] Armed PLAYER capture\n");
}

void cooldownMapperArmOpponent() {
    armedTarget.store(static_cast<int>(CaptureTarget::Opponent), std::memory_order_release);
    Logger::log("[CooldownMapper] Armed OPPONENT capture\n");
}

void cooldownMapperClear() {
    armedTarget.store(static_cast<int>(CaptureTarget::None), std::memory_order_release);
    clearCapture(playerCapture);
    clearCapture(opponentCapture);
    Logger::log("[CooldownMapper] Cleared captures\n");
}

void cooldownMapperRefreshParty() {
    PartyPointers next{};
    auto party = cmn::GameData::GetPlayerParty();
    next.party = reinterpret_cast<uint64_t>(party.m_ptr);
    if (party.m_ptr != nullptr) {
        for (int slot = 0; slot < PartySize; ++slot) {
            auto param = party.m_ptr->GetMemberPtr(slot);
            next.ikParam[slot] = reinterpret_cast<uint64_t>(param.m_ptr);
            if (param.m_ptr == nullptr || param.m_ptr->fields.m_pp == nullptr) {
                continue;
            }

            auto* pp = param.m_ptr->fields.m_pp;
            next.pmlParam[slot] = reinterpret_cast<uint64_t>(pp);
            auto* core = pp->castTo<pml::pokepara::CoreParam>();
            if (core == nullptr || core->fields.m_accessor == nullptr) {
                continue;
            }

            auto* accessor = core->fields.m_accessor;
            next.accessor[slot] = reinterpret_cast<uint64_t>(accessor);
            next.coreData[slot] = reinterpret_cast<uint64_t>(accessor->coreData);
            next.calcData[slot] = reinterpret_cast<uint64_t>(accessor->calcData);

            if (accessor->coreData != nullptr) {
                accessor->StartFastMode();
                next.personalRnd[slot] = accessor->coreData->personalRnd;
                auto& blockA = accessor->coreData->GetCoreDataBlockA();
                next.trainerId[slot] = blockA.id;
                next.colorRnd[slot] = blockA.colorRnd;
                next.species[slot] = blockA.monsno;
                next.form[slot] = blockA.formno;
                accessor->EndFastMode();
            }
        }
    }
    partyPointers = next;
    Logger::log("[CooldownMapper] Refreshed party pointers: party=%p\n", party.m_ptr);
}

const char* cooldownMapperGetText() {
    const auto armed = static_cast<CaptureTarget>(armedTarget.load(std::memory_order_acquire));
    const char* armedText = "not armed";
    if (armed == CaptureTarget::Player) {
        armedText = "PLAYER - use one move now";
    } else if (armed == CaptureTarget::Opponent) {
        armedText = "OPPONENT - wait for one move now";
    }

    int used = 0;
    used = appendFormat(
        mapperText,
        static_cast<int>(sizeof(mapperText)),
        used,
        "Read-only diagnostic. No cooldown values are changed.\n"
        "Capture state: %s\n"
        "Party object: 0x%llX\n",
        armedText,
        static_cast<unsigned long long>(partyPointers.party)
    );

    for (int slot = 0; slot < PartySize; ++slot) {
        used = appendFormat(
            mapperText,
            static_cast<int>(sizeof(mapperText)),
            used,
            "Slot %d | ik=0x%llX | pml=0x%llX | acc=0x%llX | core=0x%llX | calc=0x%llX\n"
            "       species=%u form=%u personalRnd=%08X trainerID=%08X colorRnd=%08X\n",
            slot,
            static_cast<unsigned long long>(partyPointers.ikParam[slot]),
            static_cast<unsigned long long>(partyPointers.pmlParam[slot]),
            static_cast<unsigned long long>(partyPointers.accessor[slot]),
            static_cast<unsigned long long>(partyPointers.coreData[slot]),
            static_cast<unsigned long long>(partyPointers.calcData[slot]),
            static_cast<unsigned int>(partyPointers.species[slot]),
            static_cast<unsigned int>(partyPointers.form[slot]),
            partyPointers.personalRnd[slot],
            partyPointers.trainerId[slot],
            partyPointers.colorRnd[slot]
        );
    }

    used = appendCaptureText(
        mapperText,
        static_cast<int>(sizeof(mapperText)),
        used,
        "PLAYER",
        playerCapture
    );
    appendCaptureText(
        mapperText,
        static_cast<int>(sizeof(mapperText)),
        used,
        "OPPONENT",
        opponentCapture
    );

    mapperText[sizeof(mapperText) - 1] = '\0';
    return mapperText;
}

void cooldownMapperLogSnapshot() {
    cooldownMapperRefreshParty();
    Logger::log("[CooldownMapper]\n%s\n[/CooldownMapper]\n", cooldownMapperGetText());
}

PokemonData s_dataForEncounter = {};


HkTrampoline<void, void*, float> CooldownUpdateHook = hk::hook::trampoline([](void* object, float deltaTime) {
    float before[MoveCount] = {};
    float after[MoveCount] = {};

    if (object != nullptr) {
        const auto* cooldowns = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(object) + CooldownFieldOffset);
        for (int i = 0; i < MoveCount; ++i) {
            before[i] = cooldowns[i];
        }
    }

    CooldownUpdateHook.orig(object, deltaTime);

    if (object == nullptr) {
        return;
    }

    const auto* cooldowns = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(object) + CooldownFieldOffset);
    for (int i = 0; i < MoveCount; ++i) {
        after[i] = cooldowns[i];
    }

    ++hookCounter;
    auto* observed = findObservedObject(object);
    observed->lastSeen = hookCounter;

    uint32_t jumpedMove = 0xFFFFFFFFu;
    if (observed->initialized) {
        for (int i = 0; i < MoveCount; ++i) {
            if (
                std::isfinite(before[i]) &&
                std::isfinite(observed->lastAfter[i]) &&
                before[i] > CooldownJumpThreshold &&
                before[i] > observed->lastAfter[i] + CooldownJumpThreshold
            ) {
                jumpedMove = static_cast<uint32_t>(i);
                break;
            }
        }
    }

    for (int i = 0; i < MoveCount; ++i) {
        observed->lastAfter[i] = after[i];
    }
    observed->initialized = true;

    if (jumpedMove == 0xFFFFFFFFu) {
        return;
    }

    const auto target = static_cast<CaptureTarget>(
        armedTarget.exchange(static_cast<int>(CaptureTarget::None), std::memory_order_acq_rel)
    );
    if (target == CaptureTarget::Player) {
        saveCapture(playerCapture, object, before, after, jumpedMove);
        Logger::log(
            "[CooldownMapper] Captured PLAYER cooldown object=%p vtable=%p move=%u before=%.3f after=%.3f\n",
            object,
            *reinterpret_cast<void**>(object),
            jumpedMove,
            before[jumpedMove],
            after[jumpedMove]
        );
    } else if (target == CaptureTarget::Opponent) {
        saveCapture(opponentCapture, object, before, after, jumpedMove);
        Logger::log(
            "[CooldownMapper] Captured OPPONENT cooldown object=%p vtable=%p move=%u before=%.3f after=%.3f\n",
            object,
            *reinterpret_cast<void**>(object),
            jumpedMove,
            before[jumpedMove],
            after[jumpedMove]
        );
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
