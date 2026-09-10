/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "menulordhooks.h"
#include "button.h"
#include "dialoginterf.h"
#include "menubase.h"
#include "menulord.h"
#include "originalfunctions.h"
#include <spdlog/spdlog.h>

namespace hooks {

bool lockLordFaceButton = false;
bool isLoadingSkirmishMultiSave = false;

void __fastcall menuLordFaceButtonClickHooked(game::CMenuLord* thisptr, int /*%edx*/)
{
    spdlog::debug("menuLordFaceButtonClickHooked called, lockLordFaceButton = {}", lockLordFaceButton);

    if (lockLordFaceButton) {
        spdlog::debug("Lord face button click ignored (locked after save load)");
        return;
    }

    getOriginalFunctions().menuLordFaceButtonClick(thisptr);
}

game::CMenuLord* __fastcall menuLordCtorHooked(game::CMenuLord* thisptr,
                                               int /*%edx*/,
                                               game::CMenuPhase* menuPhase)
{
    using namespace game;

    spdlog::debug("menuLordCtorHooked called, lockLordFaceButton = {}", lockLordFaceButton);

    getOriginalFunctions().menuLordCtor(thisptr, menuPhase);

    if (lockLordFaceButton) {
        auto dialog = CMenuBaseApi::get().getDialogInterface(thisptr);
        auto button = CDialogInterfApi::get().findButton(dialog, "BTN_LORD");
        if (button) {
            spdlog::debug("Disabling BTN_LORD button (locked after save load)");
            button->vftable->setEnabled(button, false);
        } else {
            spdlog::debug("BTN_LORD button not found, nothing to disable");
        }

        // Reset the flag so it does not affect subsequent CMenuLord screens
        lockLordFaceButton = false;
    }

    return thisptr;
}


void __fastcall hotseatLobbyRaceLordButtonClickHooked(void* thisptr, int /*%edx*/)
{
    spdlog::debug("hotseatLobbyRaceLordButtonClickHooked called, lockLordFaceButton = {}", lockLordFaceButton);

    if (lockLordFaceButton) {
        spdlog::debug("Hotseat lobby lord button click ignored (locked after save load)");
        return;
    }

    getOriginalFunctions().hotseatLobbyRaceLordButtonClick(thisptr);
}

void __fastcall lobbyLordButtonClickHooked(void* thisptr, int /*%edx*/)
{
    spdlog::debug("lobbyLordButtonClickHooked called, lockLordFaceButton = {}", lockLordFaceButton);

    if (lockLordFaceButton) {
        spdlog::debug("Lobby lord button click ignored (locked after save load)");
        return;
    }

    getOriginalFunctions().lobbyLordButtonClick(thisptr);
}
} // namespace hooks
