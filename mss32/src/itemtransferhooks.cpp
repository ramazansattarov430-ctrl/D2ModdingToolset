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

#include "itemtransferhooks.h"
#include "button.h"
#include "citystackinterf.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "exchangeinterf.h"
#include "exchangeinterfhooks.h"
#include "usstackleader.h"
#include "fortification.h"
#include "fortview.h"
#include "globaldata.h"
#include "interfmanager.h"
#include "itembase.h"
#include "itemcategory.h"
#include "itemutils.h"
#include "listbox.h"
#include "mempool.h"
#include "menubase.h"
#include "midbag.h"
#include "midgardmsgbox.h"
#include "midgardobjectmap.h"
#include "miditem.h"
#include "midmsgboxbuttonhandlerstd.h"
#include "midstack.h"
#include "netmessages.h"
#include "originalfunctions.h"
#include "phasegame.h"
#include "pickupdropinterf.h"
#include "midserver.h"
#include "scenarioinfo.h"
#include "scenarioview.h"
#include "settings.h"
#include "sitemerchantinterf.h"
#include "textids.h"
#include "utils.h"
#include "visitors.h"
#include <gameutils.h>
#include <optional>
#include "midscenvariables.h"
#include <spdlog/spdlog.h>
#include <vector>
#include <windows.h>
#include <usersettings.h>

namespace hooks {

    // Global cooldown value for city-to-capital transfers.
    // -1 = feature disabled
    //  0 = no cooldown
    // >0 = cooldown in turns
    static constexpr const char* CITY_CAPITAL_TRANSF_COOLDOWN_MOD = "CITY_CAPITAL_TRANSF_COOLDOWN_MOD";

    // Returns scenario variable name used to store the next allowed transfer turn
    // for a specific race.
    //
    // Race-based variables are used instead of player-based ones because race ids
    // are stable and deterministic across all scenarios.
    static const char* getRaceTransferVarName(int raceId)
{
    using namespace game;

    const auto& races = RaceCategories::get();

    if (raceId == (int)races.human->id)
        return "CITY_CAPITAL_TRANSF_COOLDOWN_HU";

    if (raceId == (int)races.undead->id)
        return "CITY_CAPITAL_TRANSF_COOLDOWN_UN";

    if (raceId == (int)races.heretic->id)
        return "CITY_CAPITAL_TRANSF_COOLDOWN_HE";

    if (raceId == (int)races.dwarf->id)
        return "CITY_CAPITAL_TRANSF_COOLDOWN_DW";

    if (raceId == (int)races.elf->id)
        return "CITY_CAPITAL_TRANSF_COOLDOWN_EL";

    return nullptr;
}

    // Resolves race-based cooldown variable for the fort owner.
    static const char* getFortTransferVarName(const game::CFortification* fort,
                                          const game::IMidgardObjectMap* objectMap)
{
    using namespace bindings;

    if (!fort || !objectMap)
        return nullptr;

    FortView fortView(fort, objectMap);

    auto owner = fortView.getOwner();

    if (!owner)
        return nullptr;

    int raceId = owner->getRaceCategoryId();

    return getRaceTransferVarName(raceId);
}

    bool hasItemsInInventory(const game::IMidgardObjectMap* objectMap,
                             const game::CMidgardID& fortId)
    {
        using namespace game;

        auto obj = objectMap->vftable->findScenarioObjectById(objectMap, &fortId);
        if (!obj)
            return false;

        auto fort = static_cast<CFortification*>(obj);

        const auto& items = fort->inventory.items;

        return items.bgn != items.end;
    }

    bool isCapital(const game::IMidgardObjectMap* objectMap, const game::CMidgardID& fortId)
    {
        using namespace game;

        auto obj = objectMap->vftable->findScenarioObjectById(objectMap, &fortId);
        if (!obj)
            return false;

        auto fort = static_cast<CFortification*>(obj);
        auto vftable = static_cast<const CFortificationVftable*>(fort->vftable);

        int tier = vftable->getTier(fort, objectMap);

        return tier == 6;
    }

    // Returns global cooldown value for city-to-capital transfers.
    //
    // Missing variable defaults to -1 which disables the feature.
    static int getTransferCooldown(const game::IMidgardObjectMap* objectMap)
    {
        using namespace game;

        if (!objectMap)
            return -1;

        return hooks::getScenarioVariableByName(hooks::getScenarioVariables(objectMap),
                                                CITY_CAPITAL_TRANSF_COOLDOWN_MOD, -1);
    }

    // Determines whether transfer-to-capital action is currently available.
    static bool isCityTransferAllowed(const game::IMidgardObjectMap* objectMap,
                                      game::CFortification* fort)
    {
        using namespace game;

        if (!fort)
            return false;

        int cooldown = getTransferCooldown(objectMap);

        // Feature disabled globally.
        if (cooldown == -1)
            return false;

        // Transfers from capital to itself are not allowed.
        if (isCapital(objectMap, fort->id))
            return false;

        // Cooldown disabled.
        if (cooldown == 0)
            return true;

        const char* varName = getFortTransferVarName(fort, objectMap);

        if (!varName) {
            spdlog::error("[CT] failed to resolve race cooldown variable");
            return false;
        }

        // Missing race variable blocks transfer but does not crash the game.
        if (!hooks::hasScenarioVariableByName(hooks::getScenarioVariables(objectMap), varName)) {
            spdlog::error("[CT] missing scenario variable {}", varName);
            return false;
        }

        auto* info = getScenarioInfo(objectMap);

        int currentTurn = info ? info->currentTurn : 0;

        int nextAllowedTurn = hooks::getScenarioVariableByName(hooks::getScenarioVariables(
                                                                   objectMap),
                                                               varName, 0);

        return currentTurn >= nextAllowedTurn;
    }


using ItemFilter = std::function<bool(game::IMidgardObjectMap* objectMap,
                                      const game::CMidgardID* itemId)>;

inline bool isCtrlPressed()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

inline bool isAltPressed()
{
    // VK_MENU == Alt key (both Left and Right)
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

enum class SellMode
{
    Normal,          // no modifiers
    IncludeEquipped, // Ctrl only
    InstantAll       // Alt (or Ctrl+Alt)
};

inline SellMode getSellMode()
{
    const bool ctrl = isCtrlPressed();
    const bool alt = isAltPressed();

    if (alt) // Alt has priority over Ctrl
        return SellMode::InstantAll;
    if (ctrl)
        return SellMode::IncludeEquipped;
    return SellMode::Normal;
}

static const game::LItemCategory* getItemCategoryById(game::IMidgardObjectMap* objectMap,
                                                      const game::CMidgardID* itemId)
{
    auto globalItem = getGlobalItemById(objectMap, itemId);
    return globalItem ? globalItem->vftable->getCategory(globalItem) : nullptr;
}

static bool isItemEquipped(const game::IdVector& equippedItems, const game::CMidgardID* itemId)
{
    for (const game::CMidgardID* item = equippedItems.bgn; item != equippedItems.end; item++) {
        if (*item == *itemId) {
            return true;
        }
    }

    return false;
}
static game::CFortification* getFortByCityId(game::IMidgardObjectMap* map,
                                             const game::CMidgardID* cityId)
{
    using namespace game;

    if (!map || !cityId)
        return nullptr;

    auto* obj = map->vftable->findScenarioObjectById(map, cityId);
    if (!obj)
        return nullptr;

    return static_cast<CFortification*>(obj);
}

static game::CMidgardID findFirstEmptyStackId(game::IMidgardObjectMap* map)
{
    using namespace game;
    if (!map)
        return invalidId;

    auto& idApi = CMidgardIDApi::get();

    IteratorPtr itBegin, itEnd;
    map->vftable->begin(map, &itBegin);
    map->vftable->end(map, &itEnd);

    IMidgardObjectMap::Iterator* it = itBegin.data;
    IMidgardObjectMap::Iterator* endIt = itEnd.data;

    if (!it || !endIt)
        return invalidId;

    for (; !it->vftable->end(it, endIt); it->vftable->advance(it)) {
        CMidgardID* objId = it->vftable->getObjectId(it);
        if (!objId)
            continue;

        if (idApi.getType(objId) == IdType::Stack) {
            auto obj = map->vftable->findScenarioObjectById(map, objId);
            if (!obj)
                continue;

            auto* stack = static_cast<CMidStack*>(obj);
            int total = stack->inventory.vftable->getItemsCount(&stack->inventory);
            if (total == 0) {
                spdlog::info("Found EMPTY Stack id={}", idToString(objId));
                return *objId;
            }
        }
    }

    spdlog::warn("No empty Stack found in scenario objects!");
    return invalidId;
}
static game::CMidgardID findFirstEmptyBagId(game::IMidgardObjectMap* map)
{
    using namespace game;
    if (!map)
        return invalidId;

    auto& idApi = CMidgardIDApi::get();

    IteratorPtr itBegin, itEnd;
    map->vftable->begin(map, &itBegin);
    map->vftable->end(map, &itEnd);

    IMidgardObjectMap::Iterator* it = itBegin.data;
    IMidgardObjectMap::Iterator* endIt = itEnd.data;

    if (!it || !endIt)
        return invalidId;

    for (; !it->vftable->end(it, endIt); it->vftable->advance(it)) {
        CMidgardID* objId = it->vftable->getObjectId(it);
        if (!objId)
            continue;

        if (idApi.getType(objId) == IdType::Bag) {
            auto obj = map->vftable->findScenarioObjectById(map, objId);
            if (!obj)
                continue;

            auto* bag = static_cast<CMidBag*>(obj);
            int total = bag->inventory.vftable->getItemsCount(&bag->inventory);
            if (total == 0) {
                spdlog::info("Found EMPTY Bag id={}", idToString(objId));
                return *objId;
            }
        }
    }

    spdlog::warn("No empty Bag found in scenario objects!");
    return invalidId;
}

#define CAT_MATCHER(name, field)                                                                   \
    static bool name(game::IMidgardObjectMap* m, const game::CMidgardID* id)                       \
    {                                                                                              \
        auto c = getItemCategoryById(m, id);                                                       \
        return c && game::ItemCategories::get().field                                              \
               && c->id == game::ItemCategories::get().field->id;                                  \
    }

CAT_MATCHER(isArmor, armor)
CAT_MATCHER(isJewel, jewel)
CAT_MATCHER(isWeapon, weapon)
CAT_MATCHER(isBanner, banner)
CAT_MATCHER(isPotionBoost, potionBoost)
CAT_MATCHER(isPotionHeal, potionHeal)
CAT_MATCHER(isPotionRevive, potionRevive)
CAT_MATCHER(isPotionPermanent, potionPermanent)
CAT_MATCHER(isScroll, scroll)
CAT_MATCHER(isWand, wand)
CAT_MATCHER(isValuable, valuable)
CAT_MATCHER(isOrb, orb)
CAT_MATCHER(isTalisman, talisman)
CAT_MATCHER(isTravel, travelItem)
CAT_MATCHER(isSpecial, special)
#undef CAT_MATCHER



static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::optional<ItemFilter> getFilterByName(const std::string& name)
{
    if (name == "armor")
        return isArmor;
    if (name == "jewel")
        return isJewel;
    if (name == "weapon")
        return isWeapon;
    if (name == "banner")
        return isBanner;
    if (name == "potionboost")
        return isPotionBoost;
    if (name == "potionheal")
        return isPotionHeal;
    if (name == "potionrevive")
        return isPotionRevive;
    if (name == "potionpermanent")
        return isPotionPermanent;
    if (name == "scroll")
        return isScroll;
    if (name == "wand")
        return isWand;
    if (name == "valuable")
        return isValuable;
    if (name == "orb")
        return isOrb;
    if (name == "talisman")
        return isTalisman;
    if (name == "travelitem")
        return isTravel;
    if (name == "special")
        return isSpecial;

    return std::nullopt;
}

static std::vector<ItemFilter> getDefaultFilters()
{
    return {
        isArmor,
        isJewel,
        isWeapon,
        isBanner,
        isPotionBoost,
        isPotionHeal,
        isPotionRevive,
        isPotionPermanent,
        isScroll,
        isWand,
        isValuable,
        isOrb,
        isTalisman,
        isTravel,
        isSpecial
    };
}

static std::vector<ItemFilter> getCustomFilters()
{
    std::vector<ItemFilter> result;

    const auto& order = hooks::userSettings().customSortOrder;

    for (const auto& nameRaw : order) {
        std::string name = toLower(nameRaw);

        auto f = getFilterByName(name);
        if (f)
            result.push_back(*f);
    }

    if (result.empty()) {
        return getDefaultFilters();
    }

    return result;
}

static bool isPotion(game::IMidgardObjectMap* objectMap, const game::CMidgardID* itemId)
{
    using namespace game;

    auto category = getItemCategoryById(objectMap, itemId);
    if (!category) {
        return false;
    }

    const auto& categories = ItemCategories::get();
    const auto id = category->id;

    return categories.potionBoost->id == id || categories.potionHeal->id == id
           || categories.potionPermanent->id == id || categories.potionRevive->id == id;
}

static bool isSpell(game::IMidgardObjectMap* objectMap, const game::CMidgardID* itemId)
{
    using namespace game;

    auto category = getItemCategoryById(objectMap, itemId);
    if (!category) {
        return false;
    }

    const auto& categories = ItemCategories::get();
    const auto id = category->id;

    return categories.scroll->id == id || categories.wand->id == id;
}

/** Transfers items from src object to dst. */
static void transferItems(const std::vector<game::CMidgardID>& items,
                          game::CPhaseGame* phaseGame,
                          const game::CMidgardID* dstObjectId,
                          const char* dstObjectName,
                          const game::CMidgardID* srcObjectId,
                          const char* srcObjectName)
{
    using namespace game;


    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    const auto& exchangeItem = VisitorApi::get().exchangeItem;
    const auto& sendExchangeItemMsg = NetMessagesApi::get().sendStackExchangeItemMsg;

    for (const auto& item : items) {
        sendExchangeItemMsg(phaseGame, srcObjectId, dstObjectId, &item, 0);

        if (!exchangeItem(srcObjectId, dstObjectId, &item, objectMap, 0)) {
            spdlog::error("Failed to transfer item {:s} from {:s} {:s} to {:s} {:s}",
                          idToString(&item), srcObjectName, idToString(srcObjectId), dstObjectName,
                          idToString(dstObjectId));
        }
    }
}

/** Transfers city items to visiting stack. */
static void transferCityToStack(game::CPhaseGame* phaseGame,
                                const game::CMidgardID* cityId,
                                std::optional<ItemFilter> itemFilter = std::nullopt)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto obj = objectMap->vftable->findScenarioObjectById(objectMap, cityId);
    if (!obj) {
        spdlog::error("Could not find city {:s}", idToString(cityId));
        return;
    }

    auto fortification = static_cast<CFortification*>(obj);
    if (fortification->stackId == emptyId) {
        return;
    }

    auto& inventory = fortification->inventory;
    std::vector<CMidgardID> items;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    for (int i = 0; i < itemsTotal; ++i) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (!itemFilter || (itemFilter && (*itemFilter)(objectMap, item))) {
            items.push_back(*item);
        }
    }

    transferItems(items, phaseGame, &fortification->stackId, "stack", cityId, "city");
}

static std::optional<bindings::PlayerView> getFortOwner(game::CFortification* fort,
                                                        const game::IMidgardObjectMap* map)
{
    if (!fort || !map)
        return std::nullopt;
    bindings::FortView view(fort, map);
    return view.getOwner();
}

static bool isSameOwner(const std::optional<bindings::PlayerView>& a,
                        const std::optional<bindings::PlayerView>& b)
{
    if (!a || !b)
        return false;

    const auto id1 = a->getId().id;
    const auto id2 = b->getId().id;

    return memcmp(&id1, &id2, sizeof(game::CMidgardID)) == 0;
}

static game::CFortification* findCapital(const game::IMidgardObjectMap* map,
                                         const std::optional<bindings::PlayerView>& owner)
{
    using namespace game;
    if (!map || !owner)
        return nullptr;

    IteratorPtr itBegin, itEnd;
    map->vftable->begin(map, &itBegin);
    map->vftable->end(map, &itEnd);

    IMidgardObjectMap::Iterator* it = itBegin.data;
    IMidgardObjectMap::Iterator* endIt = itEnd.data;

    for (; !it->vftable->end(it, endIt); it->vftable->advance(it)) {
        CMidgardID* objId = it->vftable->getObjectId(it);
        if (!objId)
            continue;

        auto* obj = map->vftable->findScenarioObjectById(map, objId);
        if (!obj || CMidgardIDApi::get().getType(objId) != IdType::Fortification)
            continue;

        auto* f = static_cast<game::CFortification*>(obj);
        bindings::FortView view(f, map);
        if (!view.isCapital())
            continue;

        auto capOwner = view.getOwner();
        if (isSameOwner(capOwner, owner))
            return f;
    }

    return nullptr;
}

// Transfers all city inventory items to the owner's capital.
//
// Cooldown state is stored in scenario variables and synchronized per race.
static void transferCityToCapital(game::CPhaseGame* phaseGame,
                                  const game::CMidgardID* cityId,
                                  const game::IMidgardObjectMap* objectMap)
{
    using namespace game;
    using namespace bindings;

    auto* info = hooks::getScenarioInfo(objectMap);
    if (!info) {
        spdlog::error("[CT] no scenario info");
        return;
    }

    int currentTurn = info->currentTurn;
    int cooldown = getTransferCooldown(objectMap);

    auto obj = objectMap->vftable->findScenarioObjectById(objectMap, cityId);
    if (!obj) {
        spdlog::error("[CT] city not found");
        return;
    }

    auto* fort = static_cast<CFortification*>(obj);

    // Capital cannot transfer items to itself.
    if (isCapital(objectMap, fort->id))
        return;

    if (!hasItemsInInventory(objectMap, fort->id))
        return;

    const char* varName = getFortTransferVarName(fort, objectMap);

    if (!varName) {
        spdlog::error("[CT] invalid race cooldown variable");
        return;
    }

    int nextAllowedTurn = hooks::getScenarioVariableByName(hooks::getScenarioVariables(objectMap),
                                                           varName, 0);

    // Cooldown check.
    if (cooldown > 0 && currentTurn < nextAllowedTurn)
        return;

    std::vector<CMidgardID> items;

    int total = fort->inventory.vftable->getItemsCount(&fort->inventory);

    for (int i = 0; i < total; ++i) {
        if (auto id = fort->inventory.vftable->getItem(&fort->inventory, i))
            items.push_back(*id);
    }

    if (items.empty())
        return;

    auto owner = getFortOwner(fort, objectMap);
    auto* capital = findCapital(objectMap, owner);

    if (!capital) {
        spdlog::error("[CT] no capital found");
        return;
    }

    CMidgardID capId = capital->id;

    transferItems(items, phaseGame, &capId, "cityStack", cityId, "city");

    // Save next allowed transfer turn.
    if (cooldown > 0) {
        bool ok = hooks::setScenarioVariableByName(const_cast<game::CMidScenVariables*>(
                                                       hooks::getScenarioVariables(objectMap)),
                                                   varName, currentTurn + cooldown);

        if (!ok)
            spdlog::error("[CT] failed to save cooldown");
    }
}

void __fastcall cityTransferBtn(game::CCityStackInterf* thisptr, int)
{
    using namespace game;

    // Server object map is required here because scenario variables must be
    // modified in live game state. Cached client maps may not propagate changes.
    auto* objectMap = hooks::getServerObjectMap();

    if (!objectMap)
        return;

    transferCityToCapital(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,objectMap);

  
    int cooldown = getTransferCooldown(objectMap);
    if (cooldown == 0)
        return;

    auto dialog = CDragAndDropInterfApi::get().getDialog(&thisptr->dragDropInterf);
    if (!dialog)
        return;

   auto* ctrl = CDialogInterfApi::get().findControl(dialog, "BTN_TRANSF_R_CITY_CAPITAL");
    if (ctrl) {
        auto* button = reinterpret_cast<game::CButtonInterf*>(ctrl);
        button->vftable->setEnabled(button, false);
    }
}

void __fastcall cityInterfTransferAllToStack(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferCityToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId);
}

void __fastcall cityInterfTransferPotionsToStack(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferCityToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isPotion);
}

void __fastcall cityInterfTransferSpellsToStack(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferCityToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isSpell);
}

void __fastcall cityInterfTransferValuablesToStack(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferCityToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isValuable);
}

/** Transfers visiting stack items to city. */
static void transferStackToCity(game::CPhaseGame* phaseGame,
                                const game::CMidgardID* cityId,
                                std::optional<ItemFilter> itemFilter = std::nullopt)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto obj = objectMap->vftable->findScenarioObjectById(objectMap, cityId);
    if (!obj) {
        spdlog::error("Could not find city {:s}", idToString(cityId));
        return;
    }

    auto fortification = static_cast<CFortification*>(obj);
    if (fortification->stackId == emptyId) {
        return;
    }

    auto stackObj = objectMap->vftable->findScenarioObjectById(objectMap, &fortification->stackId);
    if (!stackObj) {
        spdlog::error("Could not find stack {:s}", idToString(&fortification->stackId));
        return;
    }

    const auto dynamicCast = RttiApi::get().dynamicCast;
    const auto& rtti = RttiApi::rtti();

    auto stack = (CMidStack*)dynamicCast(stackObj, 0, rtti.IMidScenarioObjectType,
                                         rtti.CMidStackType, 0);
    if (!stack) {
        spdlog::error("Failed to cast scenario oject {:s} to stack",
                      idToString(&fortification->stackId));
        return;
    }

    auto& inventory = stack->inventory;
    std::vector<CMidgardID> items;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    for (int i = 0; i < itemsTotal; i++) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (isItemEquipped(stack->leaderEquippedItems, item)) {
            continue;
        }

        if (!itemFilter || (itemFilter && (*itemFilter)(objectMap, item))) {
            items.push_back(*item);
        }
    }

    transferItems(items, phaseGame, cityId, "city", &fortification->stackId, "stack");
}

static void sortInterfaceAuto(game::CPhaseGame* phaseGame,
                              const game::CMidgardID* leftId,
                              const game::CMidgardID* rightId,
                              const std::vector<ItemFilter>& filters,
                              bool modeR)
{
    using namespace game;

    if (!phaseGame || !leftId || !rightId)
        return;

    auto* map = CPhaseApi::get().getDataCache(&phaseGame->phase);
    if (!map)
        return;

    const auto& idApi = CMidgardIDApi::get();

    const CMidgardID* srcId = modeR ? rightId : leftId;
    const CMidgardID* dstId = modeR ? leftId : rightId;

    if (!srcId || !dstId || *srcId == invalidId || *srcId == emptyId || *dstId == invalidId
        || *dstId == emptyId)
        return;

    auto srcObj = map->vftable->findScenarioObjectById(map, srcId);
    auto dstObj = map->vftable->findScenarioObjectById(map, dstId);
    if (!srcObj || !dstObj)
        return;

    IdType srcType = idApi.getType(srcId);
    IdType dstType = idApi.getType(dstId);

    std::vector<CMidgardID> items;

    auto collectItems = [&](IMidScenarioObject* obj, IdType type, bool excludeEquipped) {
        std::vector<CMidgardID> result;
        if (!obj)
            return result;

        if (type == IdType::Fortification) {
            auto* fort = static_cast<CFortification*>(obj);
            auto& inv = fort->inventory;
            int total = inv.vftable->getItemsCount(&inv);
            for (int i = 0; i < total; ++i)
                if (auto id = inv.vftable->getItem(&inv, i))
                    result.push_back(*id);
        } else if (type == IdType::Stack) {
            auto* stack = static_cast<CMidStack*>(obj);
            auto& inv = stack->inventory;
            int total = inv.vftable->getItemsCount(&inv);
            for (int i = 0; i < total; ++i)
                if (auto id = inv.vftable->getItem(&inv, i))
                    if (!excludeEquipped || !isItemEquipped(stack->leaderEquippedItems, id))
                        result.push_back(*id);
        } else if (type == IdType::Bag) {
            auto* bag = static_cast<CMidBag*>(obj);
            auto& inv = bag->inventory;
            int total = inv.vftable->getItemsCount(&inv);
            for (int i = 0; i < total; ++i)
                if (auto id = inv.vftable->getItem(&inv, i))
                    result.push_back(*id);
        }
        return result;
    };

    bool excludeEquipped = (srcType == IdType::Stack);
    items = collectItems(srcObj, srcType, excludeEquipped);

    if (items.empty())
        return;

    auto original = items;

    std::stable_sort(items.begin(), items.end(), [&](const CMidgardID& a, const CMidgardID& b) {
        for (auto& f : filters) {
            bool aMatch = f(map, &a);
            bool bMatch = f(map, &b);
            if (aMatch != bMatch)
                return aMatch && !bMatch;
        }
        return false;
    });

    if (items == original) {
        spdlog::debug("sortInterfaceAuto: already sorted, skipping {}", idToString(srcId));
        return;
    }

    transferItems(items, phaseGame, dstId, "dst", srcId, "src");
    transferItems(items, phaseGame, srcId, "src", dstId, "dst");
}

static void sortCityInterface(game::CCityStackInterf* thisptr,
                              std::vector<ItemFilter> filters,
                              bool modeR)
{
    using namespace game;

    auto* phase = thisptr->dragDropInterf.phaseGame;
    auto* map = CPhaseApi::get().getDataCache(&phase->phase);
    if (!map)
        return;

    auto* city = static_cast<CFortification*>(
        map->vftable->findScenarioObjectById(map, &thisptr->data->fortificationId));
    if (!city || city->stackId == emptyId)
        return;

    sortInterfaceAuto(phase,
                      &thisptr->data->fortificationId, // left = city
                      &city->stackId,                  // right = stack
                      filters, modeR);
}

static void __fastcall sortCityCustomL(game::CCityStackInterf* thisptr, int)
{
    auto filters = getCustomFilters();

    sortCityInterface(thisptr, filters, true);
}

static void __fastcall sortCityCustomR(game::CCityStackInterf* thisptr, int)
{
    auto filters = getCustomFilters();
    sortCityInterface(thisptr, filters, false); // left -> right
}

static void sortPickupInterface(game::CPickUpDropInterf* thisptr,
                                std::vector<ItemFilter> filters,
                                bool modeR)
{
    using namespace game;
    auto* phase = thisptr->dragDropInterf.phaseGame;
    if (!phase)
        return;

    sortInterfaceAuto(phase,
                      &thisptr->data->stackId, // left = stack
                      &thisptr->data->bagId,   // right = bag
                      filters, modeR);
}

static void __fastcall sortPickupCustomL(game::CPickUpDropInterf* thisptr, int)
{
    auto filters = getCustomFilters();

    sortPickupInterface(thisptr, filters, false);
}

static void __fastcall sortPickupCustomR(game::CPickUpDropInterf* thisptr, int)
{
    auto filters = getCustomFilters();

    sortPickupInterface(thisptr, filters, true);
}

static void sortExchangeInterface(game::CExchangeInterf* thisptr,
                                  std::vector<ItemFilter> filters,
                                  bool modeR)
{
    using namespace game;
    auto* phase = thisptr->dragDropInterf.phaseGame;
    if (!phase)
        return;

    sortInterfaceAuto(phase,
                      &thisptr->data->stackLeftSideId,  // left = stack
                      &thisptr->data->stackRightSideId, // right = stack
                      filters, modeR);
}

static void __fastcall sortExchangeCustomL(game::CExchangeInterf* thisptr, int)
{
    auto filters = getCustomFilters();

    sortExchangeInterface(thisptr, filters, false);
}

static void __fastcall sortExchangeCustomR(game::CExchangeInterf* thisptr, int)
{
    auto filters = getCustomFilters();

    sortExchangeInterface(thisptr, filters, true);
}

template <typename T>
static void sortInterface(T* thisptr, std::vector<hooks::ItemFilter> filters, bool modeR)
{
    using namespace game;

    auto* phase = thisptr->dragDropInterf.phaseGame;
    if (!phase)
        return;

    const CMidgardID* leftId = nullptr;
    const CMidgardID* rightId = nullptr;

    // Determine the interface type and corresponding object IDs
    if constexpr (std::is_same_v<T, CCityStackInterf>) {
        auto* map = CPhaseApi::get().getDataCache(&phase->phase);
        if (!map)
            return;

        auto* city = static_cast<CFortification*>(
            map->vftable->findScenarioObjectById(map, &thisptr->data->fortificationId));
        if (!city || city->stackId == emptyId)
            return;

        leftId = &thisptr->data->fortificationId;
        rightId = &city->stackId;
    } else if constexpr (std::is_same_v<T, CExchangeInterf>) {
        leftId = &thisptr->data->stackLeftSideId;
        rightId = &thisptr->data->stackRightSideId;
    } else if constexpr (std::is_same_v<T, CPickUpDropInterf>) {
        leftId = &thisptr->data->stackId;
        rightId = &thisptr->data->bagId;
    } else {
        spdlog::error("sortInterface: unsupported interface type");
        return;
    }

    sortInterfaceAuto(phase, leftId, rightId, filters, modeR);
}

// Universal macro for creating a sorting function
#define SORT_MATCHER(prefix, fnSuffix, InterfaceType, sortFn, modeR, ...)                          \
    namespace {                                                                                    \
    static void __fastcall prefix##_##fnSuffix(InterfaceType* thisptr, int)                        \
    {                                                                                              \
        std::vector<hooks::ItemFilter> filters = {__VA_ARGS__};                                    \
        sortFn(thisptr, filters, modeR);                                                           \
    }                                                                                              \
    }

// clang-format off
// Universal sorting function
// ---- City sort matchers ----
SORT_MATCHER(sortCity, L_Armor, game::CCityStackInterf, sortCityInterface, true, isArmor)
SORT_MATCHER(sortCity, R_Armor, game::CCityStackInterf, sortCityInterface, false, isArmor)
SORT_MATCHER(sortCity, L_Jewel, game::CCityStackInterf, sortCityInterface, true, isJewel)
SORT_MATCHER(sortCity, R_Jewel, game::CCityStackInterf, sortCityInterface, false, isJewel)
SORT_MATCHER(sortCity, L_Weapon, game::CCityStackInterf, sortCityInterface, true, isWeapon)
SORT_MATCHER(sortCity, R_Weapon, game::CCityStackInterf, sortCityInterface, false, isWeapon)
SORT_MATCHER(sortCity, L_Banner, game::CCityStackInterf, sortCityInterface, true, isBanner)
SORT_MATCHER(sortCity, R_Banner, game::CCityStackInterf, sortCityInterface, false, isBanner)
SORT_MATCHER(sortCity, L_PotionBoost, game::CCityStackInterf, sortCityInterface, true, isPotionBoost)
SORT_MATCHER(sortCity, R_PotionBoost, game::CCityStackInterf, sortCityInterface, false, isPotionBoost)
SORT_MATCHER(sortCity, L_PotionHeal, game::CCityStackInterf, sortCityInterface, true, isPotionHeal)
SORT_MATCHER(sortCity, R_PotionHeal, game::CCityStackInterf, sortCityInterface, false, isPotionHeal)
SORT_MATCHER(sortCity, L_PotionRevive, game::CCityStackInterf, sortCityInterface, true, isPotionRevive)
SORT_MATCHER(sortCity, R_PotionRevive, game::CCityStackInterf, sortCityInterface, false, isPotionRevive)
SORT_MATCHER(sortCity, L_PotionPerm, game::CCityStackInterf, sortCityInterface, true, isPotionPermanent)
SORT_MATCHER(sortCity, R_PotionPerm, game::CCityStackInterf, sortCityInterface, false, isPotionPermanent)
SORT_MATCHER(sortCity, L_Scroll, game::CCityStackInterf, sortCityInterface, true, isScroll)
SORT_MATCHER(sortCity, R_Scroll, game::CCityStackInterf, sortCityInterface, false, isScroll)
SORT_MATCHER(sortCity, L_Wand, game::CCityStackInterf, sortCityInterface, true, isWand)
SORT_MATCHER(sortCity, R_Wand, game::CCityStackInterf, sortCityInterface, false, isWand)
SORT_MATCHER(sortCity, L_Valuable, game::CCityStackInterf, sortCityInterface, true, isValuable)
SORT_MATCHER(sortCity, R_Valuable, game::CCityStackInterf, sortCityInterface, false, isValuable)
SORT_MATCHER(sortCity, L_Orb, game::CCityStackInterf, sortCityInterface, true, isOrb)
SORT_MATCHER(sortCity, R_Orb, game::CCityStackInterf, sortCityInterface, false, isOrb)
SORT_MATCHER(sortCity, L_Talisman, game::CCityStackInterf, sortCityInterface, true, isTalisman)
SORT_MATCHER(sortCity, R_Talisman, game::CCityStackInterf, sortCityInterface, false, isTalisman)
SORT_MATCHER(sortCity, L_Travel, game::CCityStackInterf, sortCityInterface, true, isTravel)
SORT_MATCHER(sortCity, R_Travel, game::CCityStackInterf, sortCityInterface, false, isTravel)
SORT_MATCHER(sortCity, L_Special, game::CCityStackInterf, sortCityInterface, true, isSpecial)
SORT_MATCHER(sortCity, R_Special, game::CCityStackInterf, sortCityInterface, false, isSpecial)
SORT_MATCHER(sortCity, L_ArmorWeapon, game::CCityStackInterf, sortCityInterface, true, isArmor, isWeapon)
SORT_MATCHER(sortCity, R_ArmorWeapon, game::CCityStackInterf, sortCityInterface, false, isArmor, isWeapon)
SORT_MATCHER(sortCity, L_PotionRevivePotionHeal, game::CCityStackInterf, sortCityInterface, true, isPotionRevive, isPotionHeal)
SORT_MATCHER(sortCity, R_PotionRevivePotionHeal, game::CCityStackInterf, sortCityInterface, false, isPotionRevive, isPotionHeal)

// ---- Exchange sort matchers ----
SORT_MATCHER(sortExchange, L_Armor, game::CExchangeInterf, sortExchangeInterface, false, isArmor)
SORT_MATCHER(sortExchange, R_Armor, game::CExchangeInterf, sortExchangeInterface, true, isArmor)
SORT_MATCHER(sortExchange, L_Jewel, game::CExchangeInterf, sortExchangeInterface, false, isJewel)
SORT_MATCHER(sortExchange, R_Jewel, game::CExchangeInterf, sortExchangeInterface, true, isJewel)
SORT_MATCHER(sortExchange, L_Weapon, game::CExchangeInterf, sortExchangeInterface, false, isWeapon)
SORT_MATCHER(sortExchange, R_Weapon, game::CExchangeInterf, sortExchangeInterface, true, isWeapon)
SORT_MATCHER(sortExchange, L_Banner, game::CExchangeInterf, sortExchangeInterface, false, isBanner)
SORT_MATCHER(sortExchange, R_Banner, game::CExchangeInterf, sortExchangeInterface, true, isBanner)
SORT_MATCHER(sortExchange, L_PotionBoost, game::CExchangeInterf, sortExchangeInterface, false, isPotionBoost)
SORT_MATCHER(sortExchange, R_PotionBoost, game::CExchangeInterf, sortExchangeInterface, true, isPotionBoost)
SORT_MATCHER(sortExchange, L_PotionHeal, game::CExchangeInterf, sortExchangeInterface, false, isPotionHeal)
SORT_MATCHER(sortExchange, R_PotionHeal, game::CExchangeInterf, sortExchangeInterface, true, isPotionHeal)
SORT_MATCHER(sortExchange, L_PotionRevive, game::CExchangeInterf, sortExchangeInterface, false, isPotionRevive)
SORT_MATCHER(sortExchange, R_PotionRevive, game::CExchangeInterf, sortExchangeInterface, true, isPotionRevive)
SORT_MATCHER(sortExchange, L_PotionPerm, game::CExchangeInterf, sortExchangeInterface, false, isPotionPermanent)
SORT_MATCHER(sortExchange, R_PotionPerm, game::CExchangeInterf, sortExchangeInterface, true, isPotionPermanent)
SORT_MATCHER(sortExchange, L_Scroll, game::CExchangeInterf, sortExchangeInterface, false, isScroll)
SORT_MATCHER(sortExchange, R_Scroll, game::CExchangeInterf, sortExchangeInterface, true, isScroll)
SORT_MATCHER(sortExchange, L_Wand, game::CExchangeInterf, sortExchangeInterface, false, isWand)
SORT_MATCHER(sortExchange, R_Wand, game::CExchangeInterf, sortExchangeInterface, true, isWand)
SORT_MATCHER(sortExchange, L_Valuable, game::CExchangeInterf, sortExchangeInterface, false, isValuable)
SORT_MATCHER(sortExchange, R_Valuable, game::CExchangeInterf, sortExchangeInterface, true, isValuable)
SORT_MATCHER(sortExchange, L_Orb, game::CExchangeInterf, sortExchangeInterface, false, isOrb)
SORT_MATCHER(sortExchange, R_Orb, game::CExchangeInterf, sortExchangeInterface, true, isOrb)
SORT_MATCHER(sortExchange, L_Talisman, game::CExchangeInterf, sortExchangeInterface, false, isTalisman)
SORT_MATCHER(sortExchange, R_Talisman, game::CExchangeInterf, sortExchangeInterface, true, isTalisman)
SORT_MATCHER(sortExchange, L_Travel, game::CExchangeInterf, sortExchangeInterface, false, isTravel)
SORT_MATCHER(sortExchange, R_Travel, game::CExchangeInterf, sortExchangeInterface, true, isTravel)
SORT_MATCHER(sortExchange, L_Special, game::CExchangeInterf, sortExchangeInterface, false, isSpecial)
SORT_MATCHER(sortExchange, R_Special, game::CExchangeInterf, sortExchangeInterface, true, isSpecial)
SORT_MATCHER(sortExchange, L_ArmorWeapon, game::CExchangeInterf, sortExchangeInterface, false, isArmor, isWeapon)
SORT_MATCHER(sortExchange, R_ArmorWeapon, game::CExchangeInterf, sortExchangeInterface, true, isArmor, isWeapon)
SORT_MATCHER(sortExchange, L_PotionRevivePotionHeal, game::CExchangeInterf, sortExchangeInterface, false, isPotionRevive, isPotionHeal)
SORT_MATCHER(sortExchange, R_PotionRevivePotionHeal, game::CExchangeInterf, sortExchangeInterface, true, isPotionRevive, isPotionHeal)

// ---- Pickup/Drop sort matchers ----
SORT_MATCHER(sortPickup, L_Armor, game::CPickUpDropInterf, sortPickupInterface, false, isArmor)
SORT_MATCHER(sortPickup, R_Armor, game::CPickUpDropInterf, sortPickupInterface, true, isArmor)
SORT_MATCHER(sortPickup, L_Jewel, game::CPickUpDropInterf, sortPickupInterface, false, isJewel)
SORT_MATCHER(sortPickup, R_Jewel, game::CPickUpDropInterf, sortPickupInterface, true, isJewel)
SORT_MATCHER(sortPickup, L_Weapon, game::CPickUpDropInterf, sortPickupInterface, false, isWeapon)
SORT_MATCHER(sortPickup, R_Weapon, game::CPickUpDropInterf, sortPickupInterface, true, isWeapon)
SORT_MATCHER(sortPickup, L_Banner, game::CPickUpDropInterf, sortPickupInterface, false, isBanner)
SORT_MATCHER(sortPickup, R_Banner, game::CPickUpDropInterf, sortPickupInterface, true, isBanner)
SORT_MATCHER(sortPickup, L_PotionBoost, game::CPickUpDropInterf, sortPickupInterface, false, isPotionBoost)
SORT_MATCHER(sortPickup, R_PotionBoost, game::CPickUpDropInterf, sortPickupInterface, true, isPotionBoost)
SORT_MATCHER(sortPickup, L_PotionHeal, game::CPickUpDropInterf, sortPickupInterface, false, isPotionHeal)
SORT_MATCHER(sortPickup, R_PotionHeal, game::CPickUpDropInterf, sortPickupInterface, true, isPotionHeal)
SORT_MATCHER(sortPickup, L_PotionRevive, game::CPickUpDropInterf, sortPickupInterface, false, isPotionRevive)
SORT_MATCHER(sortPickup, R_PotionRevive, game::CPickUpDropInterf, sortPickupInterface, true, isPotionRevive)
SORT_MATCHER(sortPickup, L_PotionPerm, game::CPickUpDropInterf, sortPickupInterface, false, isPotionPermanent)
SORT_MATCHER(sortPickup, R_PotionPerm, game::CPickUpDropInterf, sortPickupInterface, true, isPotionPermanent)
SORT_MATCHER(sortPickup, L_Scroll, game::CPickUpDropInterf, sortPickupInterface, false, isScroll)
SORT_MATCHER(sortPickup, R_Scroll, game::CPickUpDropInterf, sortPickupInterface, true, isScroll)
SORT_MATCHER(sortPickup, L_Wand, game::CPickUpDropInterf, sortPickupInterface, false, isWand)
SORT_MATCHER(sortPickup, R_Wand, game::CPickUpDropInterf, sortPickupInterface, true, isWand)
SORT_MATCHER(sortPickup, L_Valuable, game::CPickUpDropInterf, sortPickupInterface, false, isValuable)
SORT_MATCHER(sortPickup, R_Valuable, game::CPickUpDropInterf, sortPickupInterface, true, isValuable)
SORT_MATCHER(sortPickup, L_Orb, game::CPickUpDropInterf, sortPickupInterface, false, isOrb)
SORT_MATCHER(sortPickup, R_Orb, game::CPickUpDropInterf, sortPickupInterface, true, isOrb)
SORT_MATCHER(sortPickup, L_Talisman, game::CPickUpDropInterf, sortPickupInterface, false, isTalisman)
SORT_MATCHER(sortPickup, R_Talisman, game::CPickUpDropInterf, sortPickupInterface, true, isTalisman)
SORT_MATCHER(sortPickup, L_Travel, game::CPickUpDropInterf, sortPickupInterface, false, isTravel)
SORT_MATCHER(sortPickup, R_Travel, game::CPickUpDropInterf, sortPickupInterface, true, isTravel)
SORT_MATCHER(sortPickup, L_Special, game::CPickUpDropInterf, sortPickupInterface, false, isSpecial)
SORT_MATCHER(sortPickup, R_Special, game::CPickUpDropInterf, sortPickupInterface, true, isSpecial)
SORT_MATCHER(sortPickup, L_ArmorWeapon, game::CPickUpDropInterf, sortPickupInterface, false, isArmor, isWeapon)
SORT_MATCHER(sortPickup, R_ArmorWeapon, game::CPickUpDropInterf, sortPickupInterface, true, isArmor, isWeapon)
SORT_MATCHER(sortPickup, L_PotionRevivePotionHeal, game::CPickUpDropInterf, sortPickupInterface, false, isPotionRevive, isPotionHeal)
SORT_MATCHER(sortPickup, R_PotionRevivePotionHeal, game::CPickUpDropInterf, sortPickupInterface, true, isPotionRevive, isPotionHeal)

// clang-format on


static void setupCitySortButtons(const game::CCityStackInterfApi::Api& api,
                                 const game::CButtonInterfApi::Api& btn,
                                 const game::CDialogInterfApi::Api& dlg,
                                 game::CCityStackInterf* thisptr,
                                 game::CDialogInterf* dialog,
                                 game::SmartPointer& fun,
                                 game::CCityStackInterfApi::Api::ButtonCallback& cb,
                                 void(__thiscall* freeFn)(game::SmartPointer*, void*),
                                 const char* name)
{
    using Callback = game::CCityStackInterfApi::Api::ButtonCallback::Callback;

    struct Pair
    {
        const char* suffix;
        Callback left;
        Callback right;
    };

    const Pair pairs[] = {
        {"ARMOR", (Callback)sortCity_L_Armor, (Callback)sortCity_R_Armor},
        {"JEWEL", (Callback)sortCity_L_Jewel, (Callback)sortCity_R_Jewel},
        {"WEAPON", (Callback)sortCity_L_Weapon, (Callback)sortCity_R_Weapon},
        {"BANNER", (Callback)sortCity_L_Banner, (Callback)sortCity_R_Banner},
        {"POTION_BOOST", (Callback)sortCity_L_PotionBoost, (Callback)sortCity_R_PotionBoost},
        {"POTION_HEAL", (Callback)sortCity_L_PotionHeal, (Callback)sortCity_R_PotionHeal},
        {"POTION_REVIVE", (Callback)sortCity_L_PotionRevive, (Callback)sortCity_R_PotionRevive},
        {"POTION_PERM", (Callback)sortCity_L_PotionPerm, (Callback)sortCity_R_PotionPerm},
        {"SCROLL", (Callback)sortCity_L_Scroll, (Callback)sortCity_R_Scroll},
        {"WAND", (Callback)sortCity_L_Wand, (Callback)sortCity_R_Wand},
        {"VALUABLE", (Callback)sortCity_L_Valuable, (Callback)sortCity_R_Valuable},
        {"ORB", (Callback)sortCity_L_Orb, (Callback)sortCity_R_Orb},
        {"TALISMAN", (Callback)sortCity_L_Talisman, (Callback)sortCity_R_Talisman},
        {"TRAVEL", (Callback)sortCity_L_Travel, (Callback)sortCity_R_Travel},
        {"SPECIAL", (Callback)sortCity_L_Special, (Callback)sortCity_R_Special},
        {"ARMOR_WEAPON", (Callback)sortCity_L_ArmorWeapon, (Callback)sortCity_R_ArmorWeapon},
        {"POTION_REVIVE_POTION_HEAL", (Callback)sortCity_L_PotionRevivePotionHeal,
         (Callback)sortCity_R_PotionRevivePotionHeal},
    };

    for (auto& p : pairs) {
        std::string left = "BTN_SORT_L_" + std::string(p.suffix);
        std::string right = "BTN_SORT_R_" + std::string(p.suffix);

        if (dlg.findControl(dialog, left.c_str())) {
            cb.callback = p.left;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, left.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }

        if (dlg.findControl(dialog, right.c_str())) {
            cb.callback = p.right;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, right.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }
    }
}

static void setupExchangeSortButtons(const game::CExchangeInterfApi::Api& api,
                                     const game::CButtonInterfApi::Api& btn,
                                     const game::CDialogInterfApi::Api& dlg,
                                     game::CExchangeInterf* thisptr,
                                     game::CDialogInterf* dialog,
                                     game::SmartPointer& fun,
                                     game::CExchangeInterfApi::Api::ButtonCallback& cb,
                                     void(__thiscall* freeFn)(game::SmartPointer*, void*),
                                     const char* name)
{
    using Callback = game::CExchangeInterfApi::Api::ButtonCallback::Callback;

    struct Pair
    {
        const char* suffix;
        Callback left;
        Callback right;
    };

    const Pair pairs[] = {
        {"ARMOR", (Callback)sortExchange_L_Armor, (Callback)sortExchange_R_Armor},
        {"JEWEL", (Callback)sortExchange_L_Jewel, (Callback)sortExchange_R_Jewel},
        {"WEAPON", (Callback)sortExchange_L_Weapon, (Callback)sortExchange_R_Weapon},
        {"BANNER", (Callback)sortExchange_L_Banner, (Callback)sortExchange_R_Banner},
        {"POTION_BOOST", (Callback)sortExchange_L_PotionBoost,
         (Callback)sortExchange_R_PotionBoost},
        {"POTION_HEAL", (Callback)sortExchange_L_PotionHeal, (Callback)sortExchange_R_PotionHeal},
        {"POTION_REVIVE", (Callback)sortExchange_L_PotionRevive,
         (Callback)sortExchange_R_PotionRevive},
        {"POTION_PERM", (Callback)sortExchange_L_PotionPerm, (Callback)sortExchange_R_PotionPerm},
        {"SCROLL", (Callback)sortExchange_L_Scroll, (Callback)sortExchange_R_Scroll},
        {"WAND", (Callback)sortExchange_L_Wand, (Callback)sortExchange_R_Wand},
        {"VALUABLE", (Callback)sortExchange_L_Valuable, (Callback)sortExchange_R_Valuable},
        {"ORB", (Callback)sortExchange_L_Orb, (Callback)sortExchange_R_Orb},
        {"TALISMAN", (Callback)sortExchange_L_Talisman, (Callback)sortExchange_R_Talisman},
        {"TRAVEL", (Callback)sortExchange_L_Travel, (Callback)sortExchange_R_Travel},
        {"SPECIAL", (Callback)sortExchange_L_Special, (Callback)sortExchange_R_Special},
        {"ARMOR_WEAPON", (Callback)sortExchange_L_ArmorWeapon,
         (Callback)sortExchange_R_ArmorWeapon},
        {"POTION_REVIVE_POTION_HEAL", (Callback)sortExchange_L_PotionRevivePotionHeal,
         (Callback)sortExchange_R_PotionRevivePotionHeal},
    };

    for (auto& p : pairs) {
        std::string left = "BTN_SORT_L_" + std::string(p.suffix);
        std::string right = "BTN_SORT_R_" + std::string(p.suffix);

        if (dlg.findControl(dialog, left.c_str())) {
            cb.callback = p.left;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, left.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }

        if (dlg.findControl(dialog, right.c_str())) {
            cb.callback = p.right;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, right.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }
    }
}

static void setupPickupSortButtons(const game::CPickUpDropInterfApi::Api& api,
                                   const game::CButtonInterfApi::Api& btn,
                                   const game::CDialogInterfApi::Api& dlg,
                                   game::CPickUpDropInterf* thisptr,
                                   game::CDialogInterf* dialog,
                                   game::SmartPointer& fun,
                                   game::CPickUpDropInterfApi::Api::ButtonCallback& cb,
                                   void(__thiscall* freeFn)(game::SmartPointer*, void*),
                                   const char* name)
{
    using Callback = game::CPickUpDropInterfApi::Api::ButtonCallback::Callback;

    struct Pair
    {
        const char* suffix;
        Callback left;
        Callback right;
    };

    const Pair pairs[] = {
        {"ARMOR", (Callback)sortPickup_L_Armor, (Callback)sortPickup_R_Armor},
        {"JEWEL", (Callback)sortPickup_L_Jewel, (Callback)sortPickup_R_Jewel},
        {"WEAPON", (Callback)sortPickup_L_Weapon, (Callback)sortPickup_R_Weapon},
        {"BANNER", (Callback)sortPickup_L_Banner, (Callback)sortPickup_R_Banner},
        {"POTION_BOOST", (Callback)sortPickup_L_PotionBoost, (Callback)sortPickup_R_PotionBoost},
        {"POTION_HEAL", (Callback)sortPickup_L_PotionHeal, (Callback)sortPickup_R_PotionHeal},
        {"POTION_REVIVE", (Callback)sortPickup_L_PotionRevive, (Callback)sortPickup_R_PotionRevive},
        {"POTION_PERM", (Callback)sortPickup_L_PotionPerm, (Callback)sortPickup_R_PotionPerm},
        {"SCROLL", (Callback)sortPickup_L_Scroll, (Callback)sortPickup_R_Scroll},
        {"WAND", (Callback)sortPickup_L_Wand, (Callback)sortPickup_R_Wand},
        {"VALUABLE", (Callback)sortPickup_L_Valuable, (Callback)sortPickup_R_Valuable},
        {"ORB", (Callback)sortPickup_L_Orb, (Callback)sortPickup_R_Orb},
        {"TALISMAN", (Callback)sortPickup_L_Talisman, (Callback)sortPickup_R_Talisman},
        {"TRAVEL", (Callback)sortPickup_L_Travel, (Callback)sortPickup_R_Travel},
        {"SPECIAL", (Callback)sortPickup_L_Special, (Callback)sortPickup_R_Special},
        {"ARMOR_WEAPON", (Callback)sortPickup_L_ArmorWeapon, (Callback)sortPickup_R_ArmorWeapon},
        {"POTION_REVIVE_POTION_HEAL", (Callback)sortPickup_L_PotionRevivePotionHeal,
         (Callback)sortPickup_R_PotionRevivePotionHeal},
    };

    for (auto& p : pairs) {
        std::string left = "BTN_SORT_L_" + std::string(p.suffix);
        std::string right = "BTN_SORT_R_" + std::string(p.suffix);

        if (dlg.findControl(dialog, left.c_str())) {
            cb.callback = p.left;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, left.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }

        if (dlg.findControl(dialog, right.c_str())) {
            cb.callback = p.right;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, right.c_str(), name, &fun, 0);
            freeFn(&fun, nullptr);
        }
    }
}

void __fastcall cityInterfTransferAllToCity(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferStackToCity(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId);
}

void __fastcall cityInterfTransferPotionsToCity(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferStackToCity(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isPotion);
}

void __fastcall cityInterfTransferSpellsToCity(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferStackToCity(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isSpell);
}

void __fastcall cityInterfTransferValuablesToCity(game::CCityStackInterf* thisptr, int /*%edx*/)
{
    transferStackToCity(thisptr->dragDropInterf.phaseGame, &thisptr->data->fortificationId,
                        isValuable);
}

static void setupCityStackButtons(game::CCityStackInterf* thisptr, game::CDialogInterf* dialog)
{
    using namespace game;
    using CB = CCityStackInterfApi::Api::ButtonCallback;
    using Callback = CB::Callback;
    const auto& btn = CButtonInterfApi::get();
    const auto& api = CCityStackInterfApi::get();
    const auto& dlg = CDialogInterfApi::get();
    SmartPointer fun;
    auto free = SmartPointerApi::get().createOrFreeNoDtor;
    CB cb{};
    constexpr char name[] = "DLG_CITY_STACK";

    auto hook = [&](const char* ctrl, CB::Callback fn) {
        if (dlg.findControl(dialog, ctrl)) {
            cb.callback = fn;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, ctrl, name, &fun, 0);
            free(&fun, nullptr);
        }
    };

    // --- TRANSFER buttons ---
    hook("BTN_TRANSF_L_ALL", (CB::Callback)cityInterfTransferAllToStack);
    hook("BTN_TRANSF_R_ALL", (CB::Callback)cityInterfTransferAllToCity);
    hook("BTN_TRANSF_L_POTIONS", (CB::Callback)cityInterfTransferPotionsToStack);
    hook("BTN_TRANSF_R_POTIONS", (CB::Callback)cityInterfTransferPotionsToCity);
    hook("BTN_TRANSF_L_SPELLS", (CB::Callback)cityInterfTransferSpellsToStack);
    hook("BTN_TRANSF_R_SPELLS", (CB::Callback)cityInterfTransferSpellsToCity);
    hook("BTN_TRANSF_L_VALUABLES", (CB::Callback)cityInterfTransferValuablesToStack);
    hook("BTN_TRANSF_R_VALUABLES", (CB::Callback)cityInterfTransferValuablesToCity);
    if (dlg.findControl(dialog, "BTN_TRANSF_R_CITY_CAPITAL")) {
        cb.callback = (CB::Callback)cityTransferBtn;
        api.createButtonFunctor(&fun, 0, thisptr, &cb);

        auto* btnPtr = btn.assignFunctor(dialog, "BTN_TRANSF_R_CITY_CAPITAL", name, &fun, 0);
        free(&fun, nullptr);

        if (btnPtr) {

            /*auto* objectMap = game::CPhaseApi::get().getDataCache(
                &thisptr->dragDropInterf.phaseGame->phase);*/

            //auto* objectMap = hooks::getObjectMap();

            // Use live server object map here because button state depends on
            // scenario variables updated during gameplay.
            auto* objectMap = hooks::getServerObjectMap();

            if (!objectMap) {

                spdlog::error("[CT] no server objectMap");

                return;
            }


            if (objectMap) {
                auto obj = objectMap->vftable
                               ->findScenarioObjectById(objectMap, &thisptr->data->fortificationId);

                if (!obj)
                    return;

                auto* fort = static_cast<game::CFortification*>(obj);

                // Button state is derived entirely from scenario variables and cooldown logic.
                bool enabled = isCityTransferAllowed(objectMap, fort);

                btnPtr->vftable->setEnabled(btnPtr, enabled);
            }
        }
    }

    // --- Sort buttons ---
    setupCitySortButtons(api, btn, dlg, thisptr, dialog, fun, cb, free, name);
    hook("BTN_SORT_L_CUSTOM", (CB::Callback)sortCityCustomL);
    hook("BTN_SORT_R_CUSTOM", (CB::Callback)sortCityCustomR);
}

game::CCityStackInterf* __fastcall cityStackInterfCtorHooked(game::CCityStackInterf* thisptr,
                                                             int /*%edx*/,
                                                             void* taskOpenInterf,
                                                             game::CPhaseGame* phaseGame,
                                                             game::CMidgardID* cityId)
{
    using namespace game;

    getOriginalFunctions().cityStackInterfCtor(thisptr, taskOpenInterf, phaseGame, cityId);

    auto dialog = CDragAndDropInterfApi::get().getDialog(&thisptr->dragDropInterf);
    setupCityStackButtons(thisptr, dialog);

    return thisptr;
}

/** Transfers items from stack with srcStackId to stack with dstStackId. */
static void transferStackToStack(game::CPhaseGame* phaseGame,
                                 const game::CMidgardID* dstStackId,
                                 const game::CMidgardID* srcStackId,
                                 std::optional<ItemFilter> itemFilter = std::nullopt)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto srcObj = objectMap->vftable->findScenarioObjectById(objectMap, srcStackId);
    if (!srcObj) {
        spdlog::error("Could not find stack {:s}", idToString(srcStackId));
        return;
    }

    const auto dynamicCast = RttiApi::get().dynamicCast;
    const auto& rtti = RttiApi::rtti();

    auto srcStack = (CMidStack*)dynamicCast(srcObj, 0, rtti.IMidScenarioObjectType,
                                            rtti.CMidStackType, 0);
    if (!srcStack) {
        spdlog::error("Failed to cast scenario oject {:s} to stack", idToString(srcStackId));
        return;
    }

    auto& inventory = srcStack->inventory;
    std::vector<CMidgardID> items;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    for (int i = 0; i < itemsTotal; i++) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (isItemEquipped(srcStack->leaderEquippedItems, item)) {
            continue;
        }

        if (!itemFilter || (itemFilter && (*itemFilter)(objectMap, item))) {
            items.push_back(*item);
        }
    }

    transferItems(items, phaseGame, dstStackId, "stack", srcStackId, "stack");
}

void __fastcall exchangeTransferAllToLeftStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackLeftSideId,
                         &thisptr->data->stackRightSideId);
}

void __fastcall exchangeTransferPotionsToLeftStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackLeftSideId,
                         &thisptr->data->stackRightSideId, isPotion);
}

void __fastcall exchangeTransferSpellsToLeftStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackLeftSideId,
                         &thisptr->data->stackRightSideId, isSpell);
}

void __fastcall exchangeTransferValuablesToLeftStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackLeftSideId,
                         &thisptr->data->stackRightSideId, isValuable);
}

void __fastcall exchangeTransferAllToRightStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackRightSideId,
                         &thisptr->data->stackLeftSideId);
}

void __fastcall exchangeTransferPotionsToRightStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackRightSideId,
                         &thisptr->data->stackLeftSideId, isPotion);
}

void __fastcall exchangeTransferSpellsToRightStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackRightSideId,
                         &thisptr->data->stackLeftSideId, isSpell);
}

void __fastcall exchangeTransferValuablesToRightStack(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    transferStackToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackRightSideId,
                         &thisptr->data->stackLeftSideId, isValuable);
}

/** Transfers bag items to stack. */
static void transferBagToStack(game::CPhaseGame* phaseGame,
                               const game::CMidgardID* stackId,
                               const game::CMidgardID* bagId,
                               std::optional<ItemFilter> itemFilter = std::nullopt)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto bagObj = objectMap->vftable->findScenarioObjectById(objectMap, bagId);
    if (!bagObj) {
        spdlog::error("Could not find bag {:s}", idToString(bagId));
        return;
    }

    auto bag = static_cast<CMidBag*>(bagObj);
    auto& inventory = bag->inventory;
    std::vector<CMidgardID> items;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    for (int i = 0; i < itemsTotal; i++) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (!itemFilter || (itemFilter && (*itemFilter)(objectMap, item))) {
            items.push_back(*item);
        }
    }

    transferItems(items, phaseGame, stackId, "stack", bagId, "bag");
}

void __fastcall pickupTransferAllToStack(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferBagToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId);
}

void __fastcall pickupTransferPotionsToStack(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferBagToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isPotion);
}

void __fastcall pickupTransferSpellsToStack(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferBagToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isSpell);
}

void __fastcall pickupTransferValuablesToStack(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferBagToStack(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isValuable);
}

/** Transfers stack items to bag. */
static void transferStackToBag(game::CPhaseGame* phaseGame,
                               const game::CMidgardID* stackId,
                               const game::CMidgardID* bagId,
                               std::optional<ItemFilter> itemFilter = std::nullopt)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto stackObj = objectMap->vftable->findScenarioObjectById(objectMap, stackId);
    if (!stackObj) {
        spdlog::error("Could not find stack {:s}", idToString(stackId));
        return;
    }

    const auto dynamicCast = RttiApi::get().dynamicCast;
    const auto& rtti = RttiApi::rtti();

    auto stack = (CMidStack*)dynamicCast(stackObj, 0, rtti.IMidScenarioObjectType,
                                         rtti.CMidStackType, 0);
    if (!stack) {
        spdlog::error("Failed to cast scenario oject {:s} to stack", idToString(stackId));
        return;
    }

    auto& inventory = stack->inventory;
    std::vector<CMidgardID> items;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    for (int i = 0; i < itemsTotal; i++) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (isItemEquipped(stack->leaderEquippedItems, item)) {
            continue;
        }

        if (!itemFilter || (itemFilter && (*itemFilter)(objectMap, item))) {
            items.push_back(*item);
        }
    }

    transferItems(items, phaseGame, bagId, "bag", stackId, "stack");
}

void __fastcall pickupTransferAllToBag(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferStackToBag(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId);
}

void __fastcall pickupTransferPotionsToBag(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferStackToBag(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isPotion);
}

void __fastcall pickupTransferSpellsToBag(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferStackToBag(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isSpell);
}

void __fastcall pickupTransferValuablesToBag(game::CPickUpDropInterf* thisptr, int /*%edx*/)
{
    transferStackToBag(thisptr->dragDropInterf.phaseGame, &thisptr->data->stackId,
                       &thisptr->data->bagId, isValuable);
}

static void setupPickupButtons(game::CPickUpDropInterf* thisptr, game::CDialogInterf* dialog)
{
    using namespace game;
    using CB = CPickUpDropInterfApi::Api::ButtonCallback;
    using Callback = CB::Callback;

    const auto& btn = CButtonInterfApi::get();
    const auto& api = CPickUpDropInterfApi::get();
    const auto& dlg = CDialogInterfApi::get();
    SmartPointer fun;
    auto free = SmartPointerApi::get().createOrFreeNoDtor;
    CB cb{};
    constexpr char name[] = "DLG_PICKUP_DROP";

    auto hook = [&](const char* ctrl, Callback fn) {
        spdlog::debug("setupExchangeButtons: hooking {:s}", ctrl);
        if (dlg.findControl(dialog, ctrl)) {
            spdlog::debug("setupExchangeButtons: {:s} found, assigning", ctrl);
            cb.callback = fn;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, ctrl, name, &fun, 0);
            free(&fun, nullptr);
            spdlog::debug("setupExchangeButtons: {:s} assigned OK", ctrl);
        } else {
            spdlog::debug("setupExchangeButtons: {:s} NOT found, skipping", ctrl);
        }
    };

    // --- TRANSFER buttons ---
    hook("BTN_TRANSF_L_ALL", (Callback)pickupTransferAllToStack);
    hook("BTN_TRANSF_R_ALL", (Callback)pickupTransferAllToBag);
    hook("BTN_TRANSF_L_POTIONS", (Callback)pickupTransferPotionsToStack);
    hook("BTN_TRANSF_R_POTIONS", (Callback)pickupTransferPotionsToBag);
    hook("BTN_TRANSF_L_SPELLS", (Callback)pickupTransferSpellsToStack);
    hook("BTN_TRANSF_R_SPELLS", (Callback)pickupTransferSpellsToBag);
    hook("BTN_TRANSF_L_VALUABLES", (Callback)pickupTransferValuablesToStack);
    hook("BTN_TRANSF_R_VALUABLES", (Callback)pickupTransferValuablesToBag);

    // --- SORT buttons ---
    setupPickupSortButtons(api, btn, dlg, thisptr, dialog, fun, cb, free, name);
    hook("BTN_SORT_L_CUSTOM", (CB::Callback)sortPickupCustomL);
    hook("BTN_SORT_R_CUSTOM", (CB::Callback)sortPickupCustomR);
}

// TEMP DEBUG: set to false to disable leadership check for position-diagnostics testing.
// !!! REMEMBER TO SET BACK TO true AFTER TESTING !!!
static constexpr bool ENFORCE_LEADERSHIP_LIMIT = true;

static void transferArmyDirection(const game::VisitorApi::Api& visitor,
                                  const game::CMidUnitGroupApi::Api& groupApi,
                                  game::CMidStack* srcStack,
                                  const game::CMidgardID& srcId,
                                  game::CMidStack* dstStack,
                                  const game::CMidgardID& dstId,
                                  game::IMidgardObjectMap* objectMap,
                                  bool& anyMoved)
{
    using namespace game;

    int dstLeadership = -1;
    auto dstLeaderUnit = static_cast<const CMidUnit*>(
        objectMap->vftable->findScenarioObjectById(objectMap, &dstStack->leaderId));
    if (dstLeaderUnit && dstLeaderUnit->unitImpl) {
        auto stackLeader = gameFunctions().castUnitImplToStackLeader(dstLeaderUnit->unitImpl);
        if (stackLeader) {
            dstLeadership = stackLeader->vftable->getLeadership(stackLeader);
        }
    }
    int dstUnitCount = 0;
    for (int p = 0; p < 6; ++p) {
        if (*groupApi.getUnitIdByPosition(&dstStack->group, p) != emptyId) {
            ++dstUnitCount;
        }
    }
    spdlog::debug("exchangeSwapArmies: dst leadership = {:d}, dst unit count = {:d}",
                  dstLeadership, dstUnitCount);
    if (ENFORCE_LEADERSHIP_LIMIT && dstLeadership >= 0 && dstUnitCount >= dstLeadership) {
        spdlog::debug("exchangeSwapArmies: dst leaders leadership is full, nothing to do");
        return;
    }

    for (int srcPos = 0; srcPos < 6; ++srcPos) {
        auto srcUnitId = *groupApi.getUnitIdByPosition(&srcStack->group, srcPos);
        if (srcUnitId == emptyId) {
            continue;
        }

        if (srcUnitId == srcStack->leaderId) {
            spdlog::debug("exchangeSwapArmies: skipping leader at pos {:d}", srcPos);
            continue;
        }

        int freePos = -1;
        auto sameSlotUnitId = *groupApi.getUnitIdByPosition(&dstStack->group, srcPos);
        if (sameSlotUnitId == emptyId) {
            freePos = srcPos;
        } else {
            for (int dstPos = 0; dstPos < 6; ++dstPos) {
                auto dstUnitId = *groupApi.getUnitIdByPosition(&dstStack->group, dstPos);
                if (dstUnitId == emptyId) {
                    freePos = dstPos;
                    break;
                }
            }
        }

        if (freePos == -1) {
            spdlog::debug("exchangeSwapArmies: no free slot in dst group, stopping direction");
            break;
        }

        int remaining = 0;
        for (int p = 0; p < 6; ++p) {
            auto id = *groupApi.getUnitIdByPosition(&srcStack->group, p);
            if (id != emptyId) {
                ++remaining;
            }
        }
        if (remaining <= 1) {
            spdlog::debug("exchangeSwapArmies: stopping before emptying src group entirely "
                          "(remaining = {:d})",
                          remaining);
            break;
        }

        bool swapped = false;
        if (visitor.swapUnitPosition(srcPos, &srcId, freePos, &dstId, objectMap, 0)) {
            swapped = visitor.swapUnitPosition(srcPos, &srcId, freePos, &dstId, objectMap, 1);
        } else {
            swapped = visitor.swapUnitPosition(freePos, &dstId, srcPos, &srcId, objectMap, 1);
        }

        spdlog::debug("exchangeSwapArmies: swap {:s} from src pos {:d} to dst pos {:d} = {:s}",
                      idToString(&srcUnitId), srcPos, freePos, swapped ? "OK" : "FAIL");
        spdlog::debug("exchangeSwapArmies: srcPos row={:d} col={:d}, freePos row={:d} col={:d}",
                      srcPos % 2, srcPos / 2, freePos % 2, freePos / 2);
        if (swapped) {
            anyMoved = true;
            ++dstUnitCount;
            if (ENFORCE_LEADERSHIP_LIMIT && dstLeadership >= 0 && dstUnitCount >= dstLeadership) {
                spdlog::debug("exchangeSwapArmies: dst leaders leadership reached, stopping direction");
                break;
            }
        } else {
            spdlog::error("exchangeSwapArmies: failed to swap unit {:s}", idToString(&srcUnitId));
        }
    }
}

static void __fastcall exchangeSwapArmies(game::CExchangeInterf* thisptr, int /*%edx*/)
{
    using namespace game;
    spdlog::debug("exchangeSwapArmies called, thisptr = {:p}", (void*)thisptr);
    if (!thisptr || !thisptr->data) {
        spdlog::error("exchangeSwapArmies: thisptr or thisptr->data is null, aborting");
        return;
    }
    auto objectMap = CPhaseApi::get().getDataCache(&thisptr->dragDropInterf.phaseGame->phase);
    const auto& visitor = VisitorApi::get();
    const auto& groupApi = CMidUnitGroupApi::get();
    auto& leftId = thisptr->data->stackLeftSideId;
    auto& rightId = thisptr->data->stackRightSideId;
    spdlog::debug("exchangeSwapArmies: leftId = {:s}, rightId = {:s}", idToString(&leftId),
                  idToString(&rightId));

    auto* leftStack = hooks::getStack(objectMap, &leftId);
    auto* rightStack = hooks::getStack(objectMap, &rightId);
    if (!leftStack || !rightStack) {
        spdlog::error("exchangeSwapArmies: leftStack = {:p}, rightStack = {:p}", (void*)leftStack,
                      (void*)rightStack);
        return;
    }

    bool anyMoved = false;

    spdlog::debug("exchangeSwapArmies: --- transferring right -> left ---");
    transferArmyDirection(visitor, groupApi, rightStack, rightId, leftStack, leftId, objectMap,
                          anyMoved);

    spdlog::debug("exchangeSwapArmies: --- transferring left -> right ---");
    transferArmyDirection(visitor, groupApi, leftStack, leftId, rightStack, rightId, objectMap,
                          anyMoved);

    if (anyMoved) {
        spdlog::debug("exchangeSwapArmies: forcing UI refresh for both stacks");
        exchangeInterfOnObjectChangedHooked(thisptr, 0, (IMidScenarioObject*)leftStack);
        exchangeInterfOnObjectChangedHooked(thisptr, 0, (IMidScenarioObject*)rightStack);
    }

    spdlog::debug("exchangeSwapArmies: finished, anyMoved = {:s}", anyMoved ? "true" : "false");
}

static void setupExchangeButtons(game::CExchangeInterf* thisptr, game::CDialogInterf* dialog)
{
    using namespace game;
    using CB = CExchangeInterfApi::Api::ButtonCallback;
    using Callback = CB::Callback;

    const auto& btn = CButtonInterfApi::get();
    const auto& api = CExchangeInterfApi::get();
    const auto& dlg = CDialogInterfApi::get();
    SmartPointer fun;
    auto free = SmartPointerApi::get().createOrFreeNoDtor;
    CB cb{};
    constexpr char name[] = "DLG_EXCHANGE";

    auto hook = [&](const char* ctrl, Callback fn) {
        spdlog::debug("setupExchangeButtons: hooking {:s}", ctrl);
        if (dlg.findControl(dialog, ctrl)) {
            spdlog::debug("setupExchangeButtons: {:s} found, assigning", ctrl);
            cb.callback = fn;
            api.createButtonFunctor(&fun, 0, thisptr, &cb);
            btn.assignFunctor(dialog, ctrl, name, &fun, 0);
            free(&fun, nullptr);
            spdlog::debug("setupExchangeButtons: {:s} assigned OK", ctrl);
        } else {
            spdlog::debug("setupExchangeButtons: {:s} NOT found, skipping", ctrl);
        }
    };

    // --- TRANSFER buttons ---
    hook("BTN_TRANSF_L_ALL", (Callback)exchangeTransferAllToLeftStack);
    hook("BTN_TRANSF_R_ALL", (Callback)exchangeTransferAllToRightStack);
    hook("BTN_TRANSF_L_POTIONS", (Callback)exchangeTransferPotionsToLeftStack);
    hook("BTN_TRANSF_R_POTIONS", (Callback)exchangeTransferPotionsToRightStack);
    hook("BTN_TRANSF_L_SPELLS", (Callback)exchangeTransferSpellsToLeftStack);
    hook("BTN_TRANSF_R_SPELLS", (Callback)exchangeTransferSpellsToRightStack);
    hook("BTN_TRANSF_L_VALUABLES", (Callback)exchangeTransferValuablesToLeftStack);
    hook("BTN_TRANSF_R_VALUABLES", (Callback)exchangeTransferValuablesToRightStack);

    // --- SORT buttons ---
    spdlog::debug("setupExchangeButtons: calling setupExchangeSortButtons");
    setupExchangeSortButtons(api, btn, dlg, thisptr, dialog, fun, cb, free, name);
    spdlog::debug("setupExchangeButtons: setupExchangeSortButtons finished");
    hook("BTN_SORT_L_CUSTOM", (Callback)sortExchangeCustomL);
    hook("BTN_SORT_R_CUSTOM", (Callback)sortExchangeCustomR);
    hook("BTN_SWAP_ARMY", (Callback)exchangeSwapArmies);
    spdlog::debug("setupExchangeButtons: finished");
}

game::CExchangeInterf* __fastcall exchangeInterfCtorHooked(game::CExchangeInterf* thisptr,
                                                           int /*%edx*/,
                                                           void* taskOpenInterf,
                                                           game::CPhaseGame* phaseGame,
                                                           game::CMidgardID* stackLeftSide,
                                                           game::CMidgardID* stackRightSide)
{
    using namespace game;

    spdlog::debug("exchangeInterfCtorHooked: started");
    getOriginalFunctions().exchangeInterfCtor(thisptr, taskOpenInterf, phaseGame, stackLeftSide,
                                              stackRightSide);
    spdlog::debug("exchangeInterfCtorHooked: original ctor finished");

    auto dialog = CDragAndDropInterfApi::get().getDialog(&thisptr->dragDropInterf);
    spdlog::debug("exchangeInterfCtorHooked: dialog = {:p}", (void*)dialog);
    setupExchangeButtons(thisptr, dialog);
    spdlog::debug("exchangeInterfCtorHooked: setupExchangeButtons finished");

    return thisptr;
}

game::CPickUpDropInterf* __fastcall pickupDropInterfCtorHooked(game::CPickUpDropInterf* thisptr,
                                                               int /*%edx*/,
                                                               void* taskOpenInterf,
                                                               game::CPhaseGame* phaseGame,
                                                               game::CMidgardID* stackId,
                                                               game::CMidgardID* bagId)
{
    using namespace game;

    getOriginalFunctions().pickupDropInterfCtor(thisptr, taskOpenInterf, phaseGame, stackId, bagId);
    auto dialog = CDragAndDropInterfApi::get().getDialog(&thisptr->dragDropInterf);
    setupPickupButtons(thisptr, dialog);

    return thisptr;
}

struct SellItemsMsgBoxHandler : public game::CMidMsgBoxButtonHandler
{
    game::CSiteMerchantInterf* merchantInterf;
    bool includeEquipped;
};

void __fastcall sellItemsMsgBoxHandlerDtor(SellItemsMsgBoxHandler* thisptr,
                                           int /*%edx*/,
                                           char flags)
{
    if (flags & 1) {
        // We can skip a call to CMidMsgBoxButtonHandler destructor
        // since we know it does not have any members and does not free anything except its memory.
        // Free our memory here
        game::Memory::get().freeNonZero(thisptr);
    }
}

/** Sends sell-item messages to the server. */
static void sellItemsToMerchant(game::CPhaseGame* phaseGame,
                                const game::CMidgardID* merchantId,
                                const game::CMidgardID* stackId,
                                std::optional<ItemFilter> itemFilter = std::nullopt,
                                bool includeEquipped = false)
{
    using namespace game;

    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto stackObj = objectMap->vftable->findScenarioObjectById(objectMap, stackId);
    if (!stackObj) {
        spdlog::error("Could not find stack {:s}", idToString(stackId));
        return;
    }

    const auto dynamicCast = RttiApi::get().dynamicCast;
    const auto& rtti = RttiApi::rtti();

    auto stack = (CMidStack*)dynamicCast(stackObj, 0, rtti.IMidScenarioObjectType,
                                         rtti.CMidStackType, 0);
    if (!stack) {
        spdlog::error("Failed to cast scenario object {:s} to stack", idToString(stackId));
        return;
    }

    auto& inventory = stack->inventory;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);

    std::vector<CMidgardID> itemsToSell;
    for (int i = 0; i < itemsTotal; i++) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (!includeEquipped && isItemEquipped(stack->leaderEquippedItems, item))
            continue;

        if (!itemFilter || (*itemFilter)(objectMap, item))
            itemsToSell.push_back(*item);
    }

    const auto& sendSellItemMsg = NetMessagesApi::get().sendSiteSellItemMsg;
    for (const auto& item : itemsToSell)
        sendSellItemMsg(phaseGame, merchantId, stackId, &item);
}

void __fastcall sellValuablesMsgBoxHandlerFunction(SellItemsMsgBoxHandler* thisptr,
                                                   int /*%edx*/,
                                                   game::CMidgardMsgBox* msgBox,
                                                   bool okPressed)
{
    using namespace game;

    auto merchant = thisptr->merchantInterf;
    auto phaseGame = merchant->dragDropInterf.phaseGame;

    if (okPressed) {
        auto data = merchant->data;
        sellItemsToMerchant(phaseGame, &data->merchantId, &data->stackId, isValuable);
    }

    auto manager = phaseGame->data->interfManager.data;
    auto vftable = manager->CInterfManagerImpl::CInterfManager::vftable;
    vftable->hideInterface(manager, msgBox);

    if (msgBox) {
        msgBox->vftable->destructor(msgBox, 1);
    }
}

game::CMidMsgBoxButtonHandlerVftable sellValuablesMsgBoxHandlerVftable{
    (game::CMidMsgBoxButtonHandlerVftable::Destructor)sellItemsMsgBoxHandlerDtor,
    (game::CMidMsgBoxButtonHandlerVftable::Handler)sellValuablesMsgBoxHandlerFunction};

/** Handles the “Sell all items” confirmation box. */
void __fastcall sellAllItemsMsgBoxHandlerFunction(SellItemsMsgBoxHandler* thisptr,
                                                  int /*%edx*/,
                                                  game::CMidgardMsgBox* msgBox,
                                                  bool okPressed)
{
    using namespace game;
    auto merchant = thisptr->merchantInterf;
    auto phaseGame = merchant->dragDropInterf.phaseGame;

    if (okPressed) {
        auto data = merchant->data;
        if (thisptr->includeEquipped) {
            spdlog::info("[Merchant] Selling ALL items including equipped (CTRL pressed)");
            sellItemsToMerchant(phaseGame, &data->merchantId, &data->stackId, std::nullopt, true);
        } else {
            spdlog::info("[Merchant] Selling unequipped items only");
            sellItemsToMerchant(phaseGame, &data->merchantId, &data->stackId);
        }
    }

    auto manager = phaseGame->data->interfManager.data;
    manager->CInterfManagerImpl::CInterfManager::vftable->hideInterface(manager, msgBox);
    if (msgBox)
        msgBox->vftable->destructor(msgBox, 1);
}


game::CMidMsgBoxButtonHandlerVftable sellAllItemsMsgBoxHandlerVftable{
    (game::CMidMsgBoxButtonHandlerVftable::Destructor)sellItemsMsgBoxHandlerDtor,
    (game::CMidMsgBoxButtonHandlerVftable::Handler)sellAllItemsMsgBoxHandlerFunction};

static std::string bankToPriceMessage(const game::Bank& bank)
{
    using namespace game;

    std::string priceText;
    if (bank.gold) {
        auto text = getInterfaceText("X005TA0055");
        replace(text, "%QTY%", fmt::format("{:d}", bank.gold));
        priceText.append(text + '\n');
    }

    if (bank.runicMana) {
        auto text = getInterfaceText("X005TA0056");
        replace(text, "%QTY%", fmt::format("{:d}", bank.runicMana));
        priceText.append(text + '\n');
    }

    if (bank.deathMana) {
        auto text = getInterfaceText("X005TA0057");
        replace(text, "%QTY%", fmt::format("{:d}", bank.deathMana));
        priceText.append(text + '\n');
    }

    if (bank.lifeMana) {
        auto text = getInterfaceText("X005TA0058");
        replace(text, "%QTY%", fmt::format("{:d}", bank.lifeMana));
        priceText.append(text + '\n');
    }

    if (bank.infernalMana) {
        auto text = getInterfaceText("X005TA0059");
        replace(text, "%QTY%", fmt::format("{:d}", bank.infernalMana));
        priceText.append(text + '\n');
    }

    if (bank.groveMana) {
        auto text = getInterfaceText("X160TA0001");
        replace(text, "%QTY%", fmt::format("{:d}", bank.groveMana));
        priceText.append(text + '\n');
    }

    return priceText;
}

/** Computes total selling price for all items matching filter. */
static game::Bank computeItemsSellPrice(game::IMidgardObjectMap* objectMap,
                                        const game::CMidgardID* stackId,
                                        std::optional<ItemFilter> itemFilter = std::nullopt,
                                        bool includeEquipped = false)
{
    using namespace game;

    auto stackObj = objectMap->vftable->findScenarioObjectById(objectMap, stackId);
    if (!stackObj) {
        spdlog::error("Could not find stack {:s}", idToString(stackId));
        return {};
    }

    const auto dynamicCast = RttiApi::get().dynamicCast;
    const auto& rtti = RttiApi::rtti();
    auto stack = (CMidStack*)dynamicCast(stackObj, 0, rtti.IMidScenarioObjectType,
                                         rtti.CMidStackType, 0);
    if (!stack) {
        spdlog::error("Failed to cast scenario object {:s} to stack", idToString(stackId));
        return {};
    }

    auto& inventory = stack->inventory;
    const int itemsTotal = inventory.vftable->getItemsCount(&inventory);
    auto getSellingPrice = CMidItemApi::get().getSellingPrice;
    auto bankAdd = BankApi::get().add;

    Bank sellPrice{};
    for (int i = 0; i < itemsTotal; ++i) {
        auto item = inventory.vftable->getItem(&inventory, i);
        if (!includeEquipped && isItemEquipped(stack->leaderEquippedItems, item))
            continue;
        if (!itemFilter || (*itemFilter)(objectMap, item)) {
            Bank price{};
            getSellingPrice(&price, objectMap, item);
            bankAdd(&sellPrice, &price);
        }
    }

    return sellPrice;
}

/** Merchant: “Sell all valuables” button callback. */
void __fastcall merchantSellValuables(game::CSiteMerchantInterf* thisptr, int /*%edx*/)
{
    using namespace game;

    auto phaseGame = thisptr->dragDropInterf.phaseGame;
    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto stackId = &thisptr->data->stackId;

    const SellMode mode = getSellMode();
    const bool instantSell = (mode == SellMode::InstantAll);

    // ---- Instant sell (Alt or Ctrl+Alt) ----
    if (instantSell) {
        spdlog::info("[Merchant] ALT pressed — instant sell of ALL valuables");
        sellItemsToMerchant(phaseGame, &thisptr->data->merchantId, stackId, isValuable);
        return;
    }

    // ---- Confirmation dialog ----
    const auto sellPrice = computeItemsSellPrice(objectMap, stackId, isValuable);
    if (BankApi::get().isZero(&sellPrice))
        return;

    const auto priceText = bankToPriceMessage(sellPrice);
    std::string message;

    message = getInterfaceText(textIds().interf.sellAllValuables.c_str());
    if (!message.empty())
        replace(message, "%PRICE%", priceText);
    else
        message = fmt::format("Do you want to sell all valuables? Revenue will be:\n{:s}",
                              priceText);

    auto memAlloc = Memory::get().allocate;
    auto handler = (SellItemsMsgBoxHandler*)memAlloc(sizeof(SellItemsMsgBoxHandler));
    handler->vftable = &sellValuablesMsgBoxHandlerVftable;
    handler->merchantInterf = thisptr;
    handler->includeEquipped = false; // redundant, but harmless

    hooks::showMessageBox(message, handler, true);
}




/** Merchant: “Sell all items” button callback. */
void __fastcall merchantSellAll(game::CSiteMerchantInterf* thisptr, int /*%edx*/)
{
    using namespace game;

    auto phaseGame = thisptr->dragDropInterf.phaseGame;
    auto objectMap = CPhaseApi::get().getDataCache(&phaseGame->phase);
    auto stackId = &thisptr->data->stackId;

    const SellMode mode = getSellMode();
    const bool includeEquipped = (mode != SellMode::Normal);
    const bool instantSell = (mode == SellMode::InstantAll);

    // ---- Instant sell (Alt or Ctrl+Alt) ----
    if (instantSell) {
        spdlog::info("[Merchant] ALT pressed — instant sell of ALL items (including equipped)");
        sellItemsToMerchant(phaseGame, &thisptr->data->merchantId, stackId, std::nullopt, true);
        return;
    }

    // ---- Confirmation dialog ----
    const auto sellPrice = computeItemsSellPrice(objectMap, stackId, std::nullopt, includeEquipped);
    if (BankApi::get().isZero(&sellPrice))
        return;

    const auto priceText = bankToPriceMessage(sellPrice);
    std::string message;

    if (includeEquipped) {
        // Try to use specific text id for equipped sale
        message = getInterfaceText(textIds().interf.sellAllItems.c_str());
        if (!message.empty())
            replace(message, "%PRICE%", priceText);
        else
            message = fmt::format(
                "Do you want to sell ALL items (including equipped)? Revenue will be:\n{:s}",
                priceText);
    } else {
        message = getInterfaceText(textIds().interf.sellAllItemsUnequipped.c_str());
        if (!message.empty())
            replace(message, "%PRICE%", priceText);
        else
            message = fmt::format(
                "Do you want to sell all unequipped items? Revenue will be:\n{:s}", priceText);
    }

    auto memAlloc = Memory::get().allocate;
    auto handler = (SellItemsMsgBoxHandler*)memAlloc(sizeof(SellItemsMsgBoxHandler));
    handler->vftable = &sellAllItemsMsgBoxHandlerVftable;
    handler->merchantInterf = thisptr;
    handler->includeEquipped = includeEquipped;

    hooks::showMessageBox(message, handler, true);
}



/** Hook merchant interface constructor and assign buttons. */
game::CSiteMerchantInterf* __fastcall siteMerchantInterfCtorHooked(
    game::CSiteMerchantInterf* thisptr,
    int /*%edx*/,
    void* taskOpenInterf,
    game::CPhaseGame* phaseGame,
    game::CMidgardID* stackId,
    game::CMidgardID* merchantId)
{
    using namespace game;

    getOriginalFunctions().siteMerchantInterfCtor(thisptr, taskOpenInterf, phaseGame, stackId,
                                                  merchantId);

    const auto& button = CButtonInterfApi::get();
    const auto freeFunctor = SmartPointerApi::get().createOrFreeNoDtor;
    const char dialogName[] = "DLG_MERCHANT";
    auto dialog = CDragAndDropInterfApi::get().getDialog(&thisptr->dragDropInterf);

    using ButtonCallback = CSiteMerchantInterfApi::Api::ButtonCallback;
    ButtonCallback callback{};
    SmartPointer functor;

    const auto& dialogApi = CDialogInterfApi::get();
    const auto& merchantApi = CSiteMerchantInterfApi::get();

    if (dialogApi.findControl(dialog, "BTN_SELL_ALL_VALUABLES")) {
        callback.callback = (ButtonCallback::Callback)merchantSellValuables;
        merchantApi.createButtonFunctor(&functor, 0, thisptr, &callback);
        button.assignFunctor(dialog, "BTN_SELL_ALL_VALUABLES", dialogName, &functor, 0);
        freeFunctor(&functor, nullptr);
    }

    if (dialogApi.findControl(dialog, "BTN_SELL_ALL")) {
        callback.callback = (ButtonCallback::Callback)merchantSellAll;
        merchantApi.createButtonFunctor(&functor, 0, thisptr, &callback);
        button.assignFunctor(dialog, "BTN_SELL_ALL", dialogName, &functor, 0);
        freeFunctor(&functor, nullptr);
    }

    return thisptr;
}

} // namespace hooks



