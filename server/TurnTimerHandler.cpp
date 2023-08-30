/*
 * TurnTimerHandler.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "TurnTimerHandler.h"
#include "CGameHandler.h"
#include "battles/BattleProcessor.h"
#include "queries/QueriesProcessor.h"
#include "processors/TurnOrderProcessor.h"
#include "../lib/battle/BattleInfo.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/CPlayerState.h"
#include "../lib/CStack.h"
#include "../lib/StartInfo.h"
#include "../lib/NetPacks.h"

TurnTimerHandler::TurnTimerHandler(CGameHandler & gh):
	gameHandler(gh)
{
	
}

void TurnTimerHandler::onGameplayStart(PlayerColor player)
{
	if(const auto * si = gameHandler.getStartInfo())
	{
		if(si->turnTimerInfo.isEnabled())
		{
			timers[player] = si->turnTimerInfo;
			timers[player].turnTimer = 0;
		}
	}
}

void TurnTimerHandler::onPlayerGetTurn(PlayerColor player)
{
	if(const auto * si = gameHandler.getStartInfo())
	{
		if(si->turnTimerInfo.isEnabled())
		{
			timers[player].baseTimer += timers[player].turnTimer;
			timers[player].turnTimer = si->turnTimerInfo.turnTimer;
			
			TurnTimeUpdate ttu;
			ttu.player = player;
			ttu.turnTimer = timers[player];
			gameHandler.sendAndApply(&ttu);
		}
	}
}

void TurnTimerHandler::onPlayerMakingTurn(PlayerColor player, int waitTime)
{
	const auto * gs = gameHandler.gameState();
	const auto * si = gameHandler.getStartInfo();
	if(!si || !gs)
		return;
	
	auto & state = gs->players.at(player);
	
	if(state.human && si->turnTimerInfo.isEnabled() && !gs->curB)
	{
		if(timers[player].turnTimer > 0)
		{
			timers[player].turnTimer -= waitTime;
			int frequency = (timers[player].turnTimer > turnTimePropagateThreshold ? turnTimePropagateFrequency : turnTimePropagateFrequencyCrit);
			
			if(state.status == EPlayerStatus::INGAME //do not send message if player is not active already
			   && timers[player].turnTimer % frequency == 0)
			{
				TurnTimeUpdate ttu;
				ttu.player = state.color;
				ttu.turnTimer = timers[player];
				gameHandler.sendAndApply(&ttu);
			}
		}
		else if(timers[player].baseTimer > 0)
		{
			timers[player].turnTimer = timers[player].baseTimer;
			timers[player].baseTimer = 0;
			onPlayerMakingTurn(player, 0);
		}
		else if(!gameHandler.queries->topQuery(state.color)) //wait for replies to avoid pending queries
			gameHandler.turnOrder->onPlayerEndsTurn(state.color);
	}
}

void TurnTimerHandler::onBattleStart()
{
	const auto * gs = gameHandler.gameState();
	const auto * si = gameHandler.getStartInfo();
	if(!si || !gs || !gs->curB || !si->turnTimerInfo.isBattleEnabled())
		return;
	
	auto attacker = gs->curB->getSidePlayer(BattleSide::ATTACKER);
	auto defender = gs->curB->getSidePlayer(BattleSide::DEFENDER);
	
	for(auto i : {attacker, defender})
	{
		if(i.isValidPlayer())
		{
			timers[i].battleTimer = si->turnTimerInfo.battleTimer;
			timers[i].creatureTimer = si->turnTimerInfo.creatureTimer;
			
			TurnTimeUpdate ttu;
			ttu.player = i;
			ttu.turnTimer = timers[i];
			gameHandler.sendAndApply(&ttu);
		}
	}
}

void TurnTimerHandler::onBattleNextStack(const CStack & stack)
{
	const auto * gs = gameHandler.gameState();
	const auto * si = gameHandler.getStartInfo();
	if(!si || !gs || !gs->curB || !si->turnTimerInfo.isBattleEnabled())
		return;
	
	auto player = stack.getOwner();
	
	if(!player.isValidPlayer())
		return;
		
	if(timers[player].battleTimer < si->turnTimerInfo.battleTimer)
		timers[player].battleTimer = timers[player].creatureTimer;
	timers[player].creatureTimer = si->turnTimerInfo.creatureTimer;
		
	TurnTimeUpdate ttu;
	ttu.player = player;
	ttu.turnTimer = timers[player];
	gameHandler.sendAndApply(&ttu);
}

void TurnTimerHandler::onBattleLoop(int waitTime)
{
	const auto * gs = gameHandler.gameState();
	const auto * si = gameHandler.getStartInfo();
	if(!si || !gs || !gs->curB)
		return;
	
	const auto * stack = gs->curB.get()->battleGetStackByID(gs->curB->getActiveStackID());
	if(!stack || !stack->getOwner().isValidPlayer())
		return;
	
	auto & state = gs->players.at(gs->curB->getSidePlayer(stack->unitSide()));
	
	auto turnTimerUpdateApplier = [&](TurnTimerInfo & tTimer, int waitTime)
	{
		if(tTimer.creatureTimer > 0)
		{
			tTimer.creatureTimer -= waitTime;
			int frequency = (tTimer.creatureTimer > turnTimePropagateThreshold
							 && si->turnTimerInfo.creatureTimer - tTimer.creatureTimer > turnTimePropagateThreshold)
			? turnTimePropagateFrequency : turnTimePropagateFrequencyCrit;
			
			if(state.status == EPlayerStatus::INGAME //do not send message if player is not active already
			   && tTimer.creatureTimer % frequency == 0)
			{
				TurnTimeUpdate ttu;
				ttu.player = state.color;
				ttu.turnTimer = tTimer;
				gameHandler.sendAndApply(&ttu);
			}
			return true;
		}
		return false;
	};
	
	if(state.human && si->turnTimerInfo.isBattleEnabled())
	{
		if(!turnTimerUpdateApplier(timers[state.color], waitTime))
		{
			if(timers[state.color].battleTimer > 0)
			{
				timers[state.color].creatureTimer = timers[state.color].battleTimer;
				timers[state.color].battleTimer = 0;
				turnTimerUpdateApplier(timers[state.color], 0);
			}
			else
			{
				BattleAction doNothing;
				doNothing.actionType = EActionType::DEFEND;
				doNothing.side = stack->unitSide();
				doNothing.stackNumber = stack->unitId();
				gameHandler.battles->makePlayerBattleAction(state.color, doNothing);
			}
		}
	}
}