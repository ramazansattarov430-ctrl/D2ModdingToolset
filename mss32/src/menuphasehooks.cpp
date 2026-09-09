/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2021 Vladimir Makeev.
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

#include "menuphasehooks.h"
#include "mempool.h"
#include "menucustomloadskirmishmulti.h"
#include "menucustomlobby.h"
#include "menucustomnewskirmishmulti.h"
#include "menucustomrandomscenariomulti.h"
#include "menuphase.h"
#include "menurandomscenariomulti.h"
#include "menurandomscenariosingle.h"
#include "midgard.h"
#include "netcustomservice.h"
#include "originalfunctions.h"
#include "scenariotemplates.h"
#include <spdlog/spdlog.h>

namespace hooks {

game::CMenuBase* __stdcall createMenuCustomLobbyCallback(game::CMenuPhase* menuPhase)
{
    auto menu = (CMenuCustomLobby*)game::Memory::get().allocate(sizeof(CMenuCustomLobby));
    return new (menu) CMenuCustomLobby(menuPhase);
}

game::CMenuBase* __stdcall createMenuCustomNewSkirmishMultiCallback(game::CMenuPhase* menuPhase)
{
    auto menu = (CMenuCustomNewSkirmishMulti*)game::Memory::get().allocate(
        sizeof(CMenuCustomNewSkirmishMulti));
    return new (menu) CMenuCustomNewSkirmishMulti(menuPhase);
}

game::CMenuBase* __stdcall createMenuCustomLoadSkirmishMultiCallback(game::CMenuPhase* menuPhase)
{
    auto menu = (CMenuCustomLoadSkirmishMulti*)game::Memory::get().allocate(
        sizeof(CMenuCustomLoadSkirmishMulti));
    return new (menu) CMenuCustomLoadSkirmishMulti(menuPhase);
}

game::CMenuBase* __stdcall createMenuCustomRandomScenarioMultiCallback(game::CMenuPhase* menuPhase)
{
    auto menu = (CMenuCustomRandomScenarioMulti*)game::Memory::get().allocate(
        sizeof(CMenuCustomRandomScenarioMulti));
    return new (menu) CMenuCustomRandomScenarioMulti(menuPhase);
}

game::CMenuPhase* __fastcall menuPhaseCtorHooked(game::CMenuPhase* thisptr,
                                                 int /*%edx*/,
                                                 int a2,
                                                 int a3)
{
    getOriginalFunctions().menuPhaseCtor(thisptr, a2, a3);

    loadScenarioTemplates();

    return thisptr;
}

void __fastcall menuPhaseDtorHooked(game::CMenuPhase* thisptr, int /*%edx*/, char flags)
{
    freeScenarioTemplates();

    getOriginalFunctions().menuPhaseDtor(thisptr, flags);
}

void __fastcall menuPhaseSwitchPhaseHooked(game::CMenuPhase* thisptr,
                                           int /*%edx*/,
                                           game::MenuTransition transition)
{
    using namespace game;

    auto& midgardApi = CMidgardApi::get();
    auto& menuPhase = CMenuPhaseApi::get();

    auto data = thisptr->data;
    while (true) {
        switch (data->currentPhase) {
        default:
            spdlog::debug("Current is unknown - doing nothing");
            break;
        case MenuPhase::Session: {
            spdlog::debug("Current is Session");
            auto midgard = midgardApi.instance();
            midgardApi.setClientsNetProxy(midgard, thisptr);

            data->currentPhase = MenuPhase::Session2LobbyJoin;
            if (static_cast<int>(data->currentPhase) > static_cast<int>(MenuPhase::Last)) {
                // TODO: wtf is this check
                return;
            }
            continue;
        }
        case MenuPhase::Unknown: {
            spdlog::debug("Current is Unknown");
            data->currentPhase = transition == MenuTransition::Unknown2NewSkirmish
                                     ? MenuPhase::Unknown2NewSkirmish
                                     : MenuPhase::Unknown2LobbyHost;
            if (static_cast<int>(data->currentPhase) > static_cast<int>(MenuPhase::Last)) {
                // TODO: wtf is this check
                return;
            }
            continue;
        }
        case MenuPhase::Credits2Main:
            spdlog::debug("Current is Credits2Main");
            data->currentPhase = MenuPhase::Back2Main;
            menuPhase.switchToMain(thisptr);
            break;
        case MenuPhase::Back2Main:
            spdlog::debug("Current is Back2Main");
            menuPhase.switchToMain(thisptr);
            break;
        case MenuPhase::Main:
            spdlog::debug("Current is Main");
            if (transition == MenuTransition::Main2CustomLobby) {
                spdlog::debug("Show Main2CustomLobby");
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::Main2CustomLobby,
                                                  CMenuCustomLobby::transitionFromMainName);
            } else {
                menuPhase.transitionFromMain(thisptr, transition);
            }
            break;
        case MenuPhase::Main2Single:
            spdlog::debug("Current is Main2Single");
            menuPhase.switchToSingle(thisptr);
            break;
        case MenuPhase::Single:
            spdlog::debug("Current is Single");
            menuPhase.transitionFromSingle(thisptr, transition);
            break;
        case MenuPhase::Protocol: {
            spdlog::debug("Current is Protocol");
            menuPhase.transitionFromProto(thisptr, transition);
            break;
        }
        case MenuPhase::Main2CustomLobby:
        case MenuPhase::Back2CustomLobby: {
            spdlog::debug("Show CustomLobby");
            data->networkGame = true;
            CMenuPhaseApi::Api::CreateMenuCallback tmp = createMenuCustomLobbyCallback;
            auto* callback = &tmp;
            menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                               &data->currentMenu, &data->transitionAnimation,
                               MenuPhase::CustomLobby, nullptr, &callback);
            break;
        }
        case MenuPhase::CustomLobby: {
            spdlog::debug("Current is CustomLobby");
            if (transition == MenuTransition::CustomLobby2NewSkirmish) {
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::Multi2NewSkirmish, "TRANS_MULTI2HOST");
            } else if (transition == MenuTransition::CustomLobby2LoadSkirmish) {
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::Multi2LoadSkirmish,
                                                  "TRANS_MULTI2LOAD");
            } else if (transition == MenuTransition::CustomLobby2LobbyJoin) {
                menuPhase.switchToLobbyHostJoin(thisptr);
            }
            break;
        }
        case MenuPhase::Hotseat:
            spdlog::debug("Current is Hotseat");
            menuPhase.transitionFromHotseat(thisptr, transition);
            break;
        case MenuPhase::Single2RaceCampaign:
            spdlog::debug("Current is Single2RaceCampaign");
            menuPhase.switchToRaceCampaign(thisptr);
            break;
        case MenuPhase::RaceCampaign:
            spdlog::debug("Current is RaceCampaign");
            menuPhase.transitionGodToLord(thisptr, transition);
            break;
        case MenuPhase::Single2CustomCampaign:
            spdlog::debug("Current is Single2CustomCampaign");
            menuPhase.switchToCustomCampaign(thisptr);
            break;
        case MenuPhase::CustomCampaign:
            spdlog::debug("Current is CustomCampaign");
            menuPhase.transitionNewQuestToGod(thisptr, transition);
            break;
        case MenuPhase::Single2NewSkirmish:
            spdlog::debug("Current is Single2NewSkirmish");
            if (data->networkGame && CNetCustomService::get()) {
                spdlog::debug("Show CMenuCustomNewSkirmishMulti");
                CMenuPhaseApi::Api::CreateMenuCallback
                    tmp = createMenuCustomNewSkirmishMultiCallback;
                auto* callback = &tmp;
                menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                                   &data->currentMenu, &data->transitionAnimation,
                                   MenuPhase::NewSkirmishMulti, nullptr, &callback);
            } else {
                menuPhase.switchToNewSkirmish(thisptr);
            }
            break;
        case MenuPhase::NewSkirmishSingle:
            spdlog::debug("Current is NewSkirmishSingle");
            if (transition == MenuTransition::NewSkirmish2RaceSkirmish) {
                menuPhase.transitionFromNewSkirmish(thisptr);
            } else if (transition == MenuTransition::NewSkirmishSingle2RandomScenarioSingle) {
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::NewSkirmishSingle2RandomScenarioSingle,
                                                  "TRANS_NEWQUEST2RNDSINGLE");
            } else if (transition == MenuTransition::NewSkirmishMulti2RandomScenarioMulti) {
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::NewSkirmishMulti2RandomScenarioMulti,
                                                  "TRANS_HOST2RNDMULTI");
            } else {
                spdlog::error("Invalid menu transition from NewSkirmishSingle");
                return;
            }
            break;
        case MenuPhase::NewSkirmish2RaceSkirmish:
            spdlog::debug("Current is NewSkirmish2RaceSkirmish");
            menuPhase.switchToRaceSkirmish(thisptr);
            break;
        case MenuPhase::RaceSkirmish:
            spdlog::debug("Current is RaceSkirmish");
            menuPhase.transitionGodToLord(thisptr, transition);
            break;
        case MenuPhase::RaceSkirmish2Lord:
            spdlog::debug("Current is RaceSkirmish2Lord");
            menuPhase.switchToLord(thisptr);
            break;
        case MenuPhase::Single2LoadSkirmish:
            spdlog::debug("Current is Single2LoadSkirmish");
            if (data->networkGame && CNetCustomService::get()) {
                spdlog::debug("Show CMenuCustomLoadSkirmishMulti");
                CMenuPhaseApi::Api::CreateMenuCallback
                    tmp = createMenuCustomLoadSkirmishMultiCallback;
                auto* callback = &tmp;
                menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                                   &data->currentMenu, &data->transitionAnimation,
                                   MenuPhase::LoadSkirmishMulti, nullptr, &callback);
            } else {
                menuPhase.switchToLoadSkirmish(thisptr);
            }
            break;
        case MenuPhase::Single2LoadCampaign:
            spdlog::debug("Current is Single2LoadCampaign");
            menuPhase.switchToLoadCampaign(thisptr);
            break;
        case MenuPhase::Single2LoadCustomCampaign:
            spdlog::debug("Current is Single2LoadCustomCampaign");
            menuPhase.switchToLoadCustomCampaign(thisptr);
            break;
        case MenuPhase::Main2Intro:
            spdlog::debug("Current is Main2Intro");
            menuPhase.switchIntroToMain(thisptr);
            break;
        case MenuPhase::Main2Credits:
            spdlog::debug("Current is Main2Credits");
            menuPhase.transitionToCredits(thisptr, transition);
            break;
        case MenuPhase::Credits:
            spdlog::debug("Current is Credits");
            menuPhase.switchIntroToMain(thisptr);
            break;
        case MenuPhase::Main2Proto:
            spdlog::debug("Current is Main2Proto");
            menuPhase.switchToProtocol(thisptr);
            break;
        case MenuPhase::Protocol2Multi:
            spdlog::debug("Current is Protocol2Multi");
            menuPhase.switchToMulti(thisptr);
            break;
        case MenuPhase::Multi:
            spdlog::debug("Current is Multi");
            menuPhase.transitionFromMulti(thisptr, transition);
            break;
        case MenuPhase::Protocol2Hotseat:
            spdlog::debug("Current is Protocol2Hotseat");
            menuPhase.switchToHotseat(thisptr);
            break;
        case MenuPhase::Hotseat2NewSkirmishHotseat:
            spdlog::debug("Current is Hotseat2NewSkirmishHotseat");
            menuPhase.switchToNewSkirmishHotseat(thisptr);
            break;
        case MenuPhase::Hotseat2LoadSkirmishHotseat:
            spdlog::debug("Current is Hotseat2LoadSkirmishHotseat");
            menuPhase.switchToLoadSkirmishHotseat(thisptr);
            break;
        case MenuPhase::NewSkirmishHotseat: {
            spdlog::debug("Current is NewSkirmishHotseat");
            if (transition == MenuTransition::NewSkirmishHotseat2HotseatLobby) {
                menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase,
                                                  &data->interfManager, &data->currentMenu,
                                                  MenuPhase::NewSkirmishHotseat2HotseatLobby,
                                                  "TRANS_NEW2HSLOBBY");
            } else if (transition == MenuTransition::NewSkirmishHotseat2RandomScenarioHotseat) {
                // Reuse animation when transitioning from CMenuNewSkirmishHotseat
                // to CMenuRandomScenarioSingle
                menuPhase
                    .showFullScreenAnimation(thisptr, &data->currentPhase, &data->interfManager,
                                             &data->currentMenu,
                                             MenuPhase::NewSkirmishHotseat2RandomScenarioHotseat,
                                             "TRANS_NEWQUEST2RNDSINGLE");
            } else {
                spdlog::error("Invalid menu transition from NewSkirmishHotseat");
                return;
            }

            break;
        }
        case MenuPhase::NewSkirmishHotseat2HotseatLobby:
            spdlog::debug("Current is NewSkirmishHotseat2HotseatLobby");
            menuPhase.switchToHotseatLobby(thisptr);
            break;
        case MenuPhase::Multi2Session:
            spdlog::debug("Current is Multi2Session");
            menuPhase.switchToSession(thisptr);
            break;
        case MenuPhase::NewSkirmish2LobbyHost: {
            // MenuPhase::NewSkirmish2LobbyHost and MenuPhase::LoadSkirmishMulti share
            // the same underlying value, so this case is also entered right after a
            // saved game finished loading through CMenuCustomLoadSkirmishMulti (see
            // MenuPhase::Single2LoadSkirmish above). Without this check, a loaded save
            // falls through into the same interactive lord/race selection screen as a
            // brand new skirmish, letting the race be changed after the fact.
            auto loadedGame = CMenuCustomLoadSkirmishMulti::cast(
                reinterpret_cast<CMenuBase*>(data->currentMenu));
            if (loadedGame) {
                spdlog::debug("Current is LoadSkirmishMulti (loaded game), locking race selection");
                menuPhase.switchToWaitAndCreateClient(thisptr);
                break;
            }

            spdlog::debug("Current is NewSkirmish2LobbyHost");
            menuPhase.switchToLobbyHostJoin(thisptr);
            break;
        }
        case MenuPhase::WaitInterf:
            spdlog::debug("Current is WaitInterf");
            menuPhase.switchToWaitAndCreateClient(thisptr);
            break;
        case MenuPhase::NewSkirmishSingle2RandomScenarioSingle: {
            spdlog::debug("Current is NewSkirmishSingle2RandomScenarioSingle");
            // Create random scenario single menu window during fullscreen animation
            CMenuPhaseApi::Api::CreateMenuCallback tmp = createMenuRandomScenarioSingle;
            auto* callback = &tmp;
            spdlog::debug("Try to transition to RandomScenarioSingle");
            menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                               &data->currentMenu, &data->transitionAnimation,
                               MenuPhase::RandomScenarioSingle, nullptr, &callback);
            break;
        }
        case MenuPhase::RandomScenarioSingle: {
            spdlog::debug("Current is RandomScenarioSingle");
            menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase, &data->interfManager,
                                              &data->currentMenu,
                                              MenuPhase::NewSkirmish2RaceSkirmish,
                                              "TRANS_RNDSINGLE2GOD");
            break;
        }
        case MenuPhase::NewSkirmishHotseat2RandomScenarioHotseat: {
            spdlog::debug("Current is NewSkirmishHotseat2RandomScenarioHotseat");
            // Create random scenario single menu window during fullscreen animation.
            // Reuse CMenuRandomScenarioSingle for hotseat games
            CMenuPhaseApi::Api::CreateMenuCallback tmp = createMenuRandomScenarioSingle;
            auto* callback = &tmp;
            menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                               &data->currentMenu, &data->transitionAnimation,
                               MenuPhase::RandomScenarioHotseat, nullptr, &callback);
            break;
        }
        case MenuPhase::RandomScenarioHotseat: {
            spdlog::debug("Current is RandomScenarioHotseat");
            // CMenuRandomScenarioSingle state for hotseat game mode.
            menuPhase.showFullScreenAnimation(thisptr, &data->currentPhase, &data->interfManager,
                                              &data->currentMenu,
                                              MenuPhase::NewSkirmishHotseat2HotseatLobby,
                                              "TRANS_RND2HSLOBBY");
            break;
        }
        case MenuPhase::NewSkirmishMulti2RandomScenarioMulti: {
            spdlog::debug("Current is NewSkirmishMulti2RandomScenarioMulti");

            // Create random scenario multi menu window during fullscreen animaation
            CMenuPhaseApi::Api::CreateMenuCallback
                tmp = CNetCustomService::get() ? createMenuCustomRandomScenarioMultiCallback
                                               : createMenuRandomScenarioMulti;
            auto* callback = &tmp;
            spdlog::debug("Try to transition to RandomScenarioMulti");
            menuPhase.showMenu(thisptr, &data->currentPhase, &data->interfManager,
                               &data->currentMenu, &data->transitionAnimation,
                               MenuPhase::RandomScenarioMulti, nullptr, &callback);
            break;
        }
        case MenuPhase::RandomScenarioMulti: {
            spdlog::debug("Current is RandomScenarioMulti");
            menuPhase.transitionFromNewSkirmish(thisptr);
            break;
        }
        }

        break;
    }
}

void __fastcall menuPhaseTransitionToMainOrCloseGameHooked(game::CMenuPhase* thisptr,
                                                           int /*%edx*/,
                                                           bool showIntroTransition)
{
    using namespace game;

    // Back to main if a lobby user is not logged in
    auto service = CNetCustomService::get();
    if (!service || !service->loggedIn()) {
        getOriginalFunctions().menuPhaseTransitionToMainOrCloseGame(thisptr, showIntroTransition);
        return;
    }

    const auto& midgardApi = CMidgardApi::get();
    const auto& menuPhaseApi = CMenuPhaseApi::get();

    // Close the current scenario file
    menuPhaseApi.openScenarioFile(thisptr, nullptr);

    // Clear network state while preserving net service - because it holds connection with the
    // lobby server. GameSpy does the same.
    auto data = thisptr->data;
    midgardApi.clearNetworkState(data->midgard);

    // Back to lobby screen
    menuPhaseApi.showFullScreenAnimation(thisptr, &data->currentPhase, &data->interfManager,
                                         &data->currentMenu, MenuPhase::Back2CustomLobby,
                                         CMenuCustomLobby::transitionFromBlackName);
}

} // namespace hooks
