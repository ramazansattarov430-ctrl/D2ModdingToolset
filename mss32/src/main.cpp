/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2020 Vladimir Makeev.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma comment(lib, "Lib/detours.lib")

#include "customaibattle.h"
#include "customattacks.h"
#include "custommodifiers.h"
#include "hooks.h"
#include "restrictions.h"
#include "settings.h"
#include "unitsforhire.h"
#include "utils.h"
#include "version.h"
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

HMODULE library{};
static HMODULE libraryMss23{};
static void* registerInterface{};
static void* unregisterInterface{};
std::thread::id mainThreadId;

extern "C" __declspec(naked) void __stdcall RIB_register_interface(void)
{
    __asm {
        jmp registerInterface;
    }
}

extern "C" __declspec(naked) void __stdcall RIB_unregister_interface(void)
{
    __asm {
        jmp unregisterInterface;
    }
}

template <typename T>
static void writeProtectedMemory(T* address, T value)
{
    DWORD oldProtection{};
    if (VirtualProtect(address, sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        *address = value;
        VirtualProtect(address, sizeof(T), oldProtection, &oldProtection);
        return;
    }

    spdlog::error("Failed to change memory protection for {:p}", (void*)address);
}

template <typename T>
static void adjustRestrictionMax(game::Restriction<T>* restriction,
                                 const T& value,
                                 const char* name)
{
    if (value >= restriction->min) {
        spdlog::debug("Set '{:s}' to {:d}", name, value);
        writeProtectedMemory(&restriction->max, value);
        return;
    }

    spdlog::error(
        "User specified '{:s}' value of {:d} is less than minimum value allowed in game ({:d}). Change rejected.",
        name, value, restriction->min);
}

static void adjustGameRestrictions()
{
    using namespace hooks;

    auto& restrictions = game::gameRestrictions();
    // Allow game to load and scenario editor to create scenarios with maximum allowed spells level
    // set to zero, disabling usage of magic in scenario
    writeProtectedMemory(&restrictions.spellLevel->min, 0);
    // Allow using units with tier higher than 5
    writeProtectedMemory(&restrictions.unitTier->max, 10);

    if (gameSettings().unitMaxDamage != baseGameSettings().unitMaxDamage) {
        adjustRestrictionMax(restrictions.attackDamage, gameSettings().unitMaxDamage,
                             "UnitMaxDamage");
    }

    if (gameSettings().unitMaxArmor != baseGameSettings().unitMaxArmor) {
        adjustRestrictionMax(restrictions.unitArmor, gameSettings().unitMaxArmor, "UnitMaxArmor");
    }

    if (gameSettings().stackScoutRangeMax != baseGameSettings().stackScoutRangeMax) {
        adjustRestrictionMax(restrictions.stackScoutRange, gameSettings().stackScoutRangeMax,
                             "StackMaxScoutRange");
    }

    if (executableIsGame()) {
        if (gameSettings().criticalHitDamage != baseGameSettings().criticalHitDamage) {
            spdlog::debug("Set 'criticalHitDamage' to {:d}", (int)gameSettings().criticalHitDamage);
            writeProtectedMemory(restrictions.criticalHitDamage, gameSettings().criticalHitDamage);
        }

        if (gameSettings().mageLeaderAttackPowerReduction
            != baseGameSettings().mageLeaderAttackPowerReduction) {
            spdlog::debug("Set 'mageLeaderPowerReduction' to {:d}",
                          (int)gameSettings().mageLeaderAttackPowerReduction);
            writeProtectedMemory(restrictions.mageLeaderAttackPowerReduction,
                                 gameSettings().mageLeaderAttackPowerReduction);
        }
    }
}

static bool setupHook(hooks::HookInfo& hook)
{
    spdlog::debug("Try to attach hook. Function {:p}, hook {:p}.", hook.target, hook.hook);

    // hook.original is an optional field that can point to where the new address of the original
    // function should be placed.
    void** pointer = hook.original ? hook.original : (void**)&hook.original;
    *pointer = hook.target;

    auto result = DetourAttach(pointer, hook.hook);
    if (result != NO_ERROR) {
        hooks::showErrorMessageBox(
            fmt::format("Failed to attach hook. Function {:p}, hook {:p}. Error code: {:d}.",
                        hook.target, hook.hook, result));
        return false;
    }

    return true;
}

static bool setupHooks()
{
    auto hooks{hooks::getHooks()};

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (auto& hook : hooks) {
        if (!setupHook(hook)) {
            return false;
        }
    }

    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        hooks::showErrorMessageBox(
            fmt::format("Failed to commit detour transaction. Error code: {:d}.", result));
        return false;
    }

    spdlog::debug("All hooks are set");
    return true;
}

static void setupVftableHooks()
{
    for (const auto& hook : hooks::getVftableHooks()) {
        void** target = (void**)hook.target;
        if (hook.original)
            *hook.original = *target;

        writeProtectedMemory(target, hook.hook);
    }

    spdlog::debug("All vftable hooks are set");
}

static void setupDefaultLogger()
{
    auto logger = spdlog::default_logger();
    auto& sinks = logger->sinks();
    sinks.clear();

    // Original mss32.dll has no log file so we are free to use short name "mss32.log"
    // (instead of the old "mss32Proxy.log")
    auto fileName = hooks::gameFolder() / "mss32.log";
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(fileName.string(),
                                                                           5u << 20, 3);
    // TODO: setting for all available trace levels (not only debug vs non-debug)
    fileSink->set_level(hooks::gameSettings().debugMode ? spdlog::level::trace
                                                        : spdlog::level::info);
    sinks.push_back(std::move(fileSink));

    if (IsDebuggerPresent()) {
        auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>(false);
        msvcSink->set_level(spdlog::level::trace);
        sinks.push_back(std::move(msvcSink));
    }

    // Setting maximum log level for the logger so only the sinks levels will matter
    logger->set_level(spdlog::level::trace);
    // Using UTC helps to match logs from different users (with different timezones).
    // It also helps to match client logs with lobby server logs.
    logger->set_pattern("%D %H:%M:%S.%e %5t [%=8!n] [%L] %v", spdlog::pattern_time_type::utc);
    logger->flush_on(spdlog::level::trace);
}

BOOL APIENTRY DllMain(HMODULE hDll, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH) {
        FreeLibrary(libraryMss23);
        return TRUE;
    }

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    /*
        https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-disablethreadlibrarycalls#remarks
        Do not call this function from a DLL that is linked to the static C run-time library (CRT).
        The static CRT requires DLL_THREAD_ATTACH and DLL_THREAD_DETATCH notifications to function
        properly.
    */
    // DisableThreadLibraryCalls(hDll);

    setupDefaultLogger();

    library = hDll;
    mainThreadId = std::this_thread::get_id();

    libraryMss23 = LoadLibrary("Mss23.dll");
    if (!libraryMss23) {
        hooks::showErrorMessageBox("Failed to load Mss23.dll");
        return FALSE;
    }

    registerInterface = GetProcAddress(libraryMss23, "RIB_register_interface");
    unregisterInterface = GetProcAddress(libraryMss23, "RIB_unregister_interface");
    if (!registerInterface || !unregisterInterface) {
        hooks::showErrorMessageBox("Could not load Mss23.dll addresses");
        return FALSE;
    }

    const auto error = hooks::determineGameVersion(hooks::exePath());
    if (error || hooks::gameVersion() == hooks::GameVersion::Unknown) {
        hooks::showErrorMessageBox(
            fmt::format("Failed to determine target exe type.\nReason: {:s}.", error.message()));
        return FALSE;
    }

    if (hooks::executableIsGame() && !hooks::loadUnitsForHire()) {
        hooks::showErrorMessageBox("Failed to load new units. Check error log for details.");
        return FALSE;
    }

    adjustGameRestrictions();
    setupVftableHooks();
    if (!setupHooks()) {
        return FALSE;
    }

    // Lazy initialization is not optimal as the data can be accessed in parallel threads.
    // Thread sync is excessive because the data is read-only or thread-exclusive once initialized.
    hooks::initializeCustomAttacks();
    hooks::initializeCustomModifiers();
    hooks::initializeCustomAiBattleLogic();
    return TRUE;
}
