//-----------------------------------------------------------------------------
// Copyright (c) 2004 TENGWU Entertainment All rights reserved.
// filename: combat_handler.cpp
// author: Aslan
// actor:
// data: 2008-09-25
// last:
// brief: 战斗系统管理器
//-----------------------------------------------------------------------------
#include "stdafx.h"

#include "../WorldDefine/msg_combat.h"
#include "../ServerDefine/log_cmdid_define.h"
#include "../WorldDefine/container_define.h"

#include "unit.h"
#include "map.h"
#include "creature.h"
#include "role.h"
#include "combat_handler.h"
#include "script_mgr.h"
#include "title_mgr.h"
#include "creature_ai.h"

#define MAX(a,b)					((a)>(b)?(a):(b))
#define MIN(a,b)					((a)>(b)?(b):(a))

DWORD g_dwNeedPKLogRoleID[X_NEED_PK_LOG_ROLE_NUM] = {0};
static INT GetUnitFabaoZiZhi( Unit * pUnit );
static INT GetUnitElementInjury(Unit * pUnit,INT & nDeltaAddr);

//-----------------------------------------------------------------------------
// 使用技能
//-----------------------------------------------------------------------------
INT CombatHandler::UseSkill(DWORD dwSkillID, DWORD dwTargetUnitID, DWORD dwSerial)
{
	Skill* pSkill = m_pOwner->GetSkill(dwSkillID);
	if( !P_VALID(pSkill) ) return E_UseSkill_SkillNotExist;

	Unit* pTargetUnit = m_pOwner->GetMap()->FindUnit(dwTargetUnitID);

	// 检查能否对该目标发动技能
	INT nRet = CanCastSkill(pSkill, dwTargetUnitID);
	if( E_Success != nRet )	return nRet;

	if(m_pOwner->IsRole())
	{
		Role* pOwnerRole = static_cast<Role*>(m_pOwner);
		if( pOwnerRole->IsInRoleState(ERS_WaterWalk) )
		{
			ILOG->Write(_T("Role use skill in swim state!role id:%d,skill id:%d"),pOwnerRole->GetID(),dwSkillID);
		}
	}
	// 该机能有武器限制，则减少相应武器崭新度
	//if( EITE_Null != pSkill->GetProto()->nWeaponLimit && m_pOwner->IsRole() )
	//	((Role*)m_pOwner)->GetItemMgr().ProcEquipNewness();

	// 检查该技能是否能够移动施放
	if( !pSkill->IsMoveable() )
	{
		m_pOwner->GetMoveData().StopMoveForce();
	}

	// 如果目标存在且不是自己，则改变面向
	if( P_VALID(pTargetUnit) && m_pOwner->GetID() != pTargetUnit->GetID() )
	{
		m_pOwner->GetMoveData().SetFaceTo(pTargetUnit->GetCurPos() - m_pOwner->GetCurPos());
	}


	// 打断使用技能打断的buff
	m_pOwner->OnInterruptBuffEvent(EBIF_InterCombat);

	// 设置参数，准备发动
	m_dwSkillID			=	dwSkillID;
	m_dwTargetUnitID	=	dwTargetUnitID;
	m_dwSkillSerial		=	dwSerial;

	// 如果该技能需要起手，则设置起手倒计时，否则进入技能作用阶段
	INT nPrepareTime = pSkill->GetPrepareTime();
	if (nPrepareTime < 5000)
	{
		nPrepareTime = (INT)(nPrepareTime * m_fSkillPrepareModPct);
	}
	m_nSkillPrepareCountDown = nPrepareTime;

	if( m_nSkillPrepareCountDown > 0 )
	{
		m_bSkillPreparing	=	TRUE;
		m_bSkillOperating	=	FALSE;
	}
	else
	{
		// 可以发动，设置技能冷却
		m_pOwner->StartSkillCoolDown(pSkill);

		m_bSkillPreparing	=	FALSE;
		m_bSkillOperating	=	TRUE;
		m_nSkillOperateTime	=	0;
		m_nSkillCurDmgIndex	=	0;

		// 计算目标
		CalSkillTargetList();
	}

	return nRet;
}

//-------------------------------------------------------------------------------
// 使用物品
//-------------------------------------------------------------------------------
INT CombatHandler::UseItem(INT64 n64ItemID, DWORD dwTargetUnitID, DWORD dwSerial, DWORD &dwTypeID, bool& bImmediate)
{
	if( GT_INVALID == dwTargetUnitID )
		dwTargetUnitID = m_pOwner->GetID();

	// 检查是不是玩家
	if( !m_pOwner->IsRole() ) return E_UseItem_TargetInvalid;
	Role* pOwnerRole = static_cast<Role*>(m_pOwner);

	// 检查物品是否在背包里
	tagItem* pItem = pOwnerRole->GetItemMgr().GetBagItem(n64ItemID); 
	if(!P_VALID(pItem)) 
	{
		// 尝试从战场背包里取
		pItem = pOwnerRole->GetItemMgr().GetWarBag().GetItem(n64ItemID);
		if(!P_VALID(pItem))
			return E_UseItem_ItemNotExist;
	}

	// 检查能否使用物品
	INT nRet = E_Success;
	BOOL bIgnore = FALSE;		// 是否忽略能否使用物品的通用判断

	const ItemScript* pScript = g_ScriptMgr.GetItemScript(pItem->dwTypeID);
	if(P_VALID(pScript) && P_VALID(pOwnerRole->GetMap()))
	{
		// 该处的判断为了防止本没有注册脚本函数的物品在使用时误入此块
		//const ItemScript* pTmpScript = g_ScriptMgr.GetItemScript(pItem->dwTypeID);
		//if(!P_VALID(pTmpScript))
		//{
		//	return E_UseItem_ScriptError;
		//}

		// 检查脚本的物品使用限制
		nRet = /*pItem->*/pScript->CanUseItem(pOwnerRole->GetMap(), pItem->dwTypeID, dwTargetUnitID, bIgnore);

		// 重新获取物品指针
		pItem = pOwnerRole->GetItemMgr().GetBagItem(n64ItemID); 
		if( !P_VALID(pItem) )
		{
			pItem = pOwnerRole->GetItemMgr().GetWarBag().GetItem(n64ItemID);
			if(!P_VALID(pItem)) return E_UseItem_ItemNotExist;
		}

		if (nRet != E_Success)
			return nRet;
	}

	// 检查使用物品的通用判断
	if(!bIgnore && E_Success == nRet)
		nRet = CanUseItem(pItem);

	if( E_Success != nRet ) return nRet;

	// 如果该物品不能移动使用，则停下
	if( !pItem->pProtoType->bMoveable )
	{
		m_pOwner->GetMoveData().StopMoveForce();
	}

	// 打断使用物品打断的buff
	m_pOwner->OnInterruptBuffEvent(EBIF_InterCombat);

	// 如果检查通过，则设置上相应的参数，准备发动
	m_n64ItemID			=	n64ItemID;
	m_dwItemSerial		=	dwSerial;
	dwTypeID			=	pItem->dwTypeID;
	m_dwTargetUnitIDItem = dwTargetUnitID;

	// 如果使用物品需要起手，则设置起手倒计时，否则进入作用阶段
	m_nItemPrepareCountDown = pItem->pProtoType->nPrepareTime;
	if( m_nItemPrepareCountDown > 0 )
	{
		m_bItemPreparing	=	TRUE;
		m_bItemOperating	=	FALSE;
		bImmediate			=	false;
	}
	else
	{
		m_bItemPreparing	=	FALSE;
		m_bItemOperating	=	TRUE;
		bImmediate			=	true;
	}

	return nRet;
}

//-----------------------------------------------------------------------------
// 更新技能起手，如果起手结束了，则切换到进行状态
//-----------------------------------------------------------------------------
VOID CombatHandler::UpdateSkillPrepare()
{
	if( !IsUseSkill() ) return;
	if( !IsSkillPreparing() ) return;

	// 减去Tick时间
	m_nSkillPrepareCountDown -= TICK_TIME;

	// 起手时间结束，切换到更新状态
	if( m_nSkillPrepareCountDown <= 0 )
	{
		Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);

		if( P_VALID(pSkill) )
		{
			m_pOwner->StartSkillCoolDown(pSkill);
		}

		m_bSkillPreparing = FALSE;
		m_bSkillOperating = TRUE;
		m_nSkillOperateTime = 0;
		m_nSkillCurDmgIndex = 0;

		// 计算目标
		CalSkillTargetList();
	}
}

//-----------------------------------------------------------------------------
// 更新起手，如果起手结束了，则切换到进行状态
//-----------------------------------------------------------------------------
VOID CombatHandler::UpdateItemPrepare()
{
	if( !IsUseItem() ) return;
	if( !IsItemPreparing() ) return;

	// 减去Tick时间
	m_nItemPrepareCountDown -= TICK_TIME;

	// 起手时间结束，切换到更新状态
	if( m_nItemPrepareCountDown <= 0 )
	{
		m_bItemPreparing = FALSE;
		m_bItemOperating = TRUE;
	}
}


//-------------------------------------------------------------------------------
// 更新技能操作，如果到了计算伤害的时候，则计算伤害，如果伤害计算完了，则计算buff
//-------------------------------------------------------------------------------
VOID CombatHandler::UpdateSkillOperate()
{
	if( !IsUseSkill() ) return;
	if( !IsSkillOperating() ) return;

	// 首先找到这个技能
	Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
	if( !P_VALID(pSkill) )
	{
		EndUseSkill();
		return;
	}

	Map* pMap = m_pOwner->GetMap();
	if( !P_VALID(pMap) ) return;

	// 得到技能总伤害次数
	INT nDmgTimes = pSkill->GetDmgTimes();

	if (m_pOwner->IsRole())
	{
		((Role*)m_pOwner)->GetTitleMgr()->SigEvent(ETE_USE_SKILL, pSkill->GetTypeID(), GT_INVALID);
	}

	// 如果伤害次数为0，说明该技能无伤害，则直接进入到计算buff阶段
	if( nDmgTimes <= 0 )
	{
		// 计算buff
		m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listTargetID, ETEE_Use);

		// 如果是封印技能
		if (pSkill->GetTypeEx() == ESSTE_SealSkill)
		{
			const SkillScript* pScript = pSkill->GetSkillScript();
			if (P_VALID(pScript))
			{
				// 找到这个目标
				Unit* pTarget = pMap->FindUnit(m_dwTargetUnitID);
				if( !P_VALID(pTarget) ) 
				{
					EndUseSkill();
					return;
				}

				DWORD dwTargetTypeID = GT_INVALID;
				if (pTarget->IsCreature())
				{
					Creature* pCreature = (Creature*)pTarget;
					dwTargetTypeID = pCreature->GetTypeID();
				}
				// 调用脚本
				pScript->SealSkill(m_pOwner->GetMap(), pSkill->GetID(), m_pOwner->GetID(), m_dwTargetUnitID, dwTargetTypeID);
			}
		}
		// 凝聚神格
		//if( pSkill->GetTypeEx2() == ESSTE2_GodCondense )
		//{
		//	if( m_pOwner->IsRole() )
		//	{
		//		((Role*)m_pOwner)->StartCondense();
		//	}
		//}

		// 计算消耗
		CalculateCost(pSkill);
		// 结束
		EndUseSkill();
		return;
	}

	// 伤害次数不为0，则检测当前时间到了哪次伤害
	m_nSkillOperateTime += TICK_TIME;

	for(; m_nSkillCurDmgIndex < nDmgTimes; m_nSkillCurDmgIndex++)
	{
		// 本tick完成不了如此多的伤害计算，等到下个tick
		if( pSkill->GetDmgTime(m_nSkillCurDmgIndex) > m_nSkillOperateTime )
			break;

		// 时间到了，则开始计算伤害
		TList<DWORD>::TListIterator it = m_listTargetID.Begin();
		DWORD dwTargetID = GT_INVALID;

		while( m_listTargetID.PeekNext(it, dwTargetID) )
		{
			// 找到这个目标
			Unit* pTarget = pMap->FindUnit(dwTargetID);

			if( !P_VALID(pTarget) ) continue;

			// 计算伤害
			if (m_bNeedPKLog && m_pOwner->IsRole() && pTarget->IsRole())
			{
				TCHAR szSrcRoleAttStr[ERA_End*X_SHORT_STRING] = {0};
				TCHAR szTargetRoleAttStr[ERA_End*X_SHORT_STRING] = {0};

				for(int i=ERA_AttA_Start; i<ERA_End; ++i)
				{
					TCHAR szTmp[X_SHORT_STRING] = {0};
					_sntprintf(szTmp, X_SHORT_STRING, _T("%d|"), m_pOwner->GetAttValue(i));
					_tcscat(szSrcRoleAttStr, szTmp);
				}

				for(int i=ERA_AttA_Start; i<ERA_End; ++i)
				{
					TCHAR szTmp[X_SHORT_STRING] = {0};
					_sntprintf(szTmp, X_SHORT_STRING, _T("%d|"), pTarget->GetAttValue(i));
					_tcscat(szTargetRoleAttStr, szTmp);
				}
				
				ILOG->Write(_T("PK Log / Skill ID = %d / 发动者 ID = %d / 属性 = %s\n"), pSkill->GetID(), m_pOwner->GetID(), szSrcRoleAttStr);
				ILOG->Write(_T("PK Log / Skill ID = %d / 被击者 ID = %d / 属性 = %s\n"), pSkill->GetID(), pTarget->GetID(), szTargetRoleAttStr);
			}

			CalculateDmg(pSkill, pTarget, m_bNeedPKLog);

			////计算元神伤害
			//if ( m_pOwner->IsRole() )
			//{
			//	Role* pRole = (Role*)m_pOwner;
			//	bool bIsAwake = pRole->GetHolySoul().IsHolySoulAwake();
			//	if ( bIsAwake )
			//	{
			//		pRole->GetHolySoul().CalculateHolySoulDmg(pSkill,m_pOwner,pTarget);
			//	}
			//}

			// 计算圣灵伤害
			if ( m_pOwner->IsRole() )
			{
				Role* pRole1 = (Role*)m_pOwner;				
				tagHolyMan * pHoly2 = NULL;
				
				tagEquip * pEquip1 =pRole1->GetItemMgr().GetEquipBarEquip((INT16)EEP_Holy);
				tagHolyMan * pHoly1 = NULL;
				if( P_VALID(pEquip1) && P_VALID(pEquip1->pEquipProto) && MIsHoly(pEquip1) )
				{
					pHoly1 = (tagHolyMan*)pEquip1;
				}

				if ( pHoly1 && pRole1->IsInRoleState(ERS_CALLHOLY) && pTarget->IsRole() )
				{
					Role* pRole2 = (Role*)pTarget;
					tagEquip * pEquip2 =pRole2->GetItemMgr().GetEquipBarEquip((INT16)EEP_Holy);
					if( P_VALID(pEquip2) && P_VALID(pEquip2->pEquipProto) && MIsHoly(pEquip2))
					{
						pHoly2 = (tagHolyMan*)pEquip2;
					}

				}
				
				if (pHoly1 && pRole1->IsInRoleState(ERS_CALLHOLY))
				{
					// 攻击方装备圣灵，需要计算圣灵伤害
					CalculateHolyDmg(pSkill,m_pOwner,pTarget,pHoly1,pHoly2);
				}

			}

		}
	}

	// 检测所有伤害是否已经计算完毕
	if( m_nSkillCurDmgIndex >= nDmgTimes )
	{
		// 计算主动技能Buff
		if( m_dwTargetEffectFlag & ETEF_Hited )
		{
			m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listHitedTarget, ETEE_Hit);
			m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listDodgedTarget, ETEE_Dodged);
		}
		if( m_dwTargetEffectFlag & ETEF_Block )
			m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listBlockedTarget, ETEE_Blocked);
		if( m_dwTargetEffectFlag & ETEF_Crited )
			m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listCritedTarget, ETEE_Crit);
		m_pOwner->OnActiveSkillBuffTrigger(pSkill, m_listHitedTarget, ETEE_Use);

		// 找到目标
		Unit* pTarget = pMap->FindUnit(m_dwTargetUnitID);
		if( P_VALID(pTarget) )
		{
			// Buff触发
			if( m_dwTargetEffectFlag & ETEF_Hited )
			{
				// 命中
				m_pOwner->OnBuffTrigger(pTarget, ETEE_Hit);
			}
			else
			{
				// 被闪避
				m_pOwner->OnBuffTrigger(pTarget, ETEE_Dodged);
			}

			if( m_dwTargetEffectFlag & ETEF_Block )
			{
				// 被格挡
				m_pOwner->OnBuffTrigger(pTarget, ETEE_Blocked);
			}

			if( m_dwTargetEffectFlag & ETEF_Crited )
			{
				// 暴击
				m_pOwner->OnBuffTrigger(pTarget, ETEE_Crit);
			}

			// 计算被动技能和装备Buff
			if( m_pOwner->IsRole() )
			{
				// 针对第一目标进行计算
				Role* pOwnerRole = static_cast<Role*>(m_pOwner);

				if( m_dwTargetEffectFlag & ETEF_Hited )
				{
					// 命中
					pOwnerRole->OnPassiveSkillBuffTrigger(pTarget, ETEE_Hit);
					pOwnerRole->OnEquipmentBuffTrigger(NULL, ETEE_Hit);
				}
				else
				{
					// 被闪避
					pOwnerRole->OnPassiveSkillBuffTrigger(pTarget, ETEE_Dodged);
					pOwnerRole->OnEquipmentBuffTrigger(NULL, ETEE_Dodged);
				}

				if( m_dwTargetEffectFlag & ETEF_Block )
				{
					// 被格挡
					pOwnerRole->OnPassiveSkillBuffTrigger(pTarget, ETEE_Blocked);
					pOwnerRole->OnEquipmentBuffTrigger(NULL, ETEE_Blocked);
				}

				if( m_dwTargetEffectFlag & ETEF_Crited )
				{
					// 暴击
					pOwnerRole->OnPassiveSkillBuffTrigger(pTarget, ETEE_Crit);
					pOwnerRole->OnEquipmentBuffTrigger(NULL, ETEE_Crit);
				}
			}
		}

		// 计算消耗
		CalculateCost(pSkill);

		if (m_pOwner->IsRole())
		{
			Role *pRole = dynamic_cast<Role *>(m_pOwner);
			if (NULL != pRole)
			{
				pRole->GetTitleMgr()->SigEvent(ETE_USE_SKILL, pSkill->GetID(), GT_INVALID);
			}
		}

		// 技能结束
		EndUseSkill();
	
		// 魂晶技能使用完后删除
		if (ESSTE_SoulSubSkill == (ESkillTypeEx)pSkill->GetProto()->nType2 && m_pOwner->IsRole())
		{
			((Role*)m_pOwner)->RemoveSkill(pSkill->GetID());
		}
	}
}

//-----------------------------------------------------------------------------
// 更新使用物品效果
//-----------------------------------------------------------------------------
VOID CombatHandler::UpdateItemOperate()
{
	if( !IsUseItem() ) return;
	if( !IsItemOperating() ) return;
	if( !m_pOwner->IsRole() ) return;

	Role* pOwnerRole = static_cast<Role*>(m_pOwner);

	// 首先找到这个物品
	tagItem* pItem = pOwnerRole->GetItemMgr().GetBagItem(m_n64ItemID); 
	if(!P_VALID(pItem)) 
	{	
		pItem = pOwnerRole->GetItemMgr().GetWarBag().GetItem(m_n64ItemID);
		if(!P_VALID(pItem)) 
		{	
			EndUseItem();
			return ;
		}
	}

	DWORD	dwTypeID = pItem->dwTypeID;
	Map* pMap = pOwnerRole->GetMap();
	if( !P_VALID(pMap) ) return;

	// 发送命中目标给客户端
	tagNS_HitTarget send;
	send.dwRoleID = m_pOwner->GetID();
	send.dwSrcRoleID = m_pOwner->GetID();
	send.eCause = EHTC_Item;
	send.dwMisc = pItem->dwTypeID;
	send.dwSerial = m_dwItemSerial;
	pMap->SendBigVisTileMsg(m_pOwner, &send, send.dwSize);

	// 计算buff
	pOwnerRole->OnActiveItemBuffTrigger(pItem, ETEE_Use);

	// 计算物品的脚本使用效果
	const ItemScript* pScript = g_ScriptMgr.GetItemScript(pItem->dwTypeID);
	if(P_VALID(pScript) && P_VALID(pOwnerRole->GetMap()))
	{
		/*pItem->*/if( pScript->UseItem(pOwnerRole->GetMap(), pItem->dwTypeID, m_dwTargetUnitIDItem) == 1 )
		{
			EndUseItem();
			return;
		}
	}

	//使用了召唤元神技能书
// 	if ( HOLYSOUL_SKILLID == pItem->dwTypeID )
// 	{
// 		pOwnerRole->ActiveHolySoul();
// 	}

	// 称号消息
	pOwnerRole->GetTitleMgr()->SigEvent(ETE_USE_ITEM, dwTypeID, GT_INVALID);

	// 加入物品公共冷却时间
	pOwnerRole->GetItemMgr().Add2CDTimeMap(dwTypeID);

	// 处理物品消失
	pOwnerRole->GetItemMgr().ItemUsedFromBag(m_n64ItemID, 1, (DWORD)ELCLD_Item_Use);

	// 处理战场物品消失
	if( P_VALID(pItem) && P_VALID(pItem->pProtoType) && MIsWarItem(pItem->pProtoType))
		pOwnerRole->GetItemMgr().ItemUsedFromWarBag(m_n64ItemID, 1, (DWORD)ELCLD_Item_Use);

	EndUseItem();
}

//-----------------------------------------------------------------------------------
// 取消技能使用
//-----------------------------------------------------------------------------------
VOID CombatHandler::CancelSkillUse(DWORD dwSkillTypeID, DWORD dwSkillSerial/* = GT_INVALID*/)
{
	if( !IsValid() || !IsUseSkill() ) return;

	Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
	if( !P_VALID(pSkill) || pSkill->GetTypeID() != dwSkillTypeID  || (dwSkillSerial!=GT_INVALID && dwSkillSerial != m_dwSkillSerial))
		return;

	BOOL bCanCancel = FALSE;

	// 如果技能正在起手，一定可以
	if( IsSkillPreparing() )
	{
		bCanCancel = TRUE;
	}
	// 如果正在释放，则只有普通攻击才可以
	else
	{
		if( ESSTE_Default == pSkill->GetTypeEx() )
		{
			bCanCancel = TRUE;
		}else
		{
			if(m_nSkillCurDmgIndex>0) bCanCancel = TRUE; // 必须又一次伤害才能取消
		}
	}

	// 如果可以取消
	if( bCanCancel )
	{
		tagNS_SkillInterrupt send;
		send.dwRoleID = m_pOwner->GetID();
		send.dwSkillID = dwSkillTypeID;
		send.dwSerial	= m_dwSkillSerial;

		if( P_VALID(m_pOwner->GetMap()) )
		{
			m_pOwner->GetMap()->SendBigVisTileMsg(m_pOwner, &send, send.dwSize);
		}

		// 调用脚本
		Skill* pSkill1 = m_pOwner->GetSkill(m_dwSkillID);
		if( !P_VALID(pSkill1) || pSkill1->GetTypeID() != dwSkillTypeID )
			return;

		const SkillScript* pScript = pSkill1->GetSkillScript();
		if (P_VALID(pScript))
		{
			Map* pMap = m_pOwner->GetMap();
			if( !P_VALID(pMap) ) return;

			// 找到这个目标
			Unit* pTarget = pMap->FindUnit(m_dwTargetUnitID);
			if( !P_VALID(pTarget) ) return;

			DWORD dwTargetTypeID = GT_INVALID;
			if (pTarget->IsCreature())
			{
				Creature* pCreature = (Creature*)pTarget;
				dwTargetTypeID = pCreature->GetTypeID();
			}

			pScript->CancelSkill(m_pOwner->GetMap(), pSkill1->GetID(), m_pOwner->GetID(), m_dwTargetUnitID, dwTargetTypeID);
		}
		CalculateCost(pSkill1);

		EndUseSkill();
	}
}

//-----------------------------------------------------------------------------------
// 取消物品释放
//-----------------------------------------------------------------------------------
VOID CombatHandler::CancelItemUse(INT64 n64ItemSerial)
{
	if( !IsValid() || !IsUseItem() ) return;

	if( m_n64ItemID != n64ItemSerial ) return;

	BOOL bCanCancel = FALSE;

	// 物品只有在起手时才能取消
	if( IsItemPreparing() )
	{
		bCanCancel = TRUE;
	}

	if( bCanCancel )
	{
		tagNS_UseItemInterrupt send;
		send.dwRoleID = m_pOwner->GetID();
		send.n64ItemID = m_n64ItemID;
		send.dwTypeID = GT_INVALID;

		if( P_VALID(m_pOwner->GetMap()) )
		{
			m_pOwner->GetMap()->SendBigVisTileMsg(m_pOwner, &send, send.dwSize);
		}
		EndUseItem();
	}
}

//-----------------------------------------------------------------------------------
// 打断起手
//-----------------------------------------------------------------------------------
BOOL CombatHandler::InterruptPrepare(EInterruptType eType, BOOL bOrdinary, BOOL bForce)
{
	if( FALSE == IsValid() || FALSE == IsPreparing() )
		return TRUE;

	BOOL bSkill = FALSE;		// 是技能在起手还是物品在起手
	DWORD dwSkillTypeID = GT_INVALID;
	if( IsSkillPreparing() )	bSkill = TRUE;
	else						bSkill = FALSE;

	// 通过是使用物品还是使用技能来判断打断值
	BOOL bMoveInterrupt = FALSE;
	INT nInterruptSkillRate = 0;

	if( bSkill )
	{
		Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
		if( P_VALID(pSkill) )
		{
			const tagSkillProto* pProto = pSkill->GetProto();
			if( P_VALID(pProto) )
			{
				bMoveInterrupt = pProto->bInterruptMove;
				nInterruptSkillRate = (bOrdinary ? pProto->nInterruptSkillOrdRate : pProto->nInterruptSkillSpecRate);
			}
			dwSkillTypeID = pSkill->GetTypeID();
		}
	}
	else
	{
		Role* pRole = static_cast<Role*>(m_pOwner);
		tagItem* pItem = pRole->GetItemMgr().GetBagItem(m_n64ItemID);
		if( P_VALID(pItem) )
		{
			bMoveInterrupt = pItem->pProtoType->bInterruptMove;
			nInterruptSkillRate = pItem->pProtoType->nInterruptSkillOrdRate;
		}
	}

	BOOL bCanInterrupt = FALSE;	// 是否能够打断

	if( bForce )
	{
		bCanInterrupt = TRUE;
	}
	else
	{
		// 尝试打断
		switch(eType)
		{
		case EIT_Move:
			{
				if( bMoveInterrupt )
				{
					bCanInterrupt = TRUE;
				}
			}
			break;

		case EIT_Skill:
			{
				// 普通攻击打断几率
				if( IUTIL->Rand() % 10000 < nInterruptSkillRate )
				{
					bCanInterrupt = TRUE;
				}
			}
			break;

		default:
			break;
		}
	}

	if( bCanInterrupt )
	{
		// 发送打断给周围玩家
		if( bSkill )
		{
			tagNS_SkillInterrupt send;
			send.dwRoleID = m_pOwner->GetID();
			send.dwSkillID = dwSkillTypeID;
			send.dwSerial = m_dwSkillSerial;

			if( P_VALID(m_pOwner->GetMap()) )
			{
				m_pOwner->GetMap()->SendBigVisTileMsg(m_pOwner, &send, send.dwSize);
			}

			// 调用脚本
			Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
			if( !P_VALID(pSkill) || pSkill->GetTypeID() != dwSkillTypeID )
				return FALSE;

			const SkillScript* pScript = pSkill->GetSkillScript();
			if (P_VALID(pScript))
			{
				Map* pMap = m_pOwner->GetMap();
				if( !P_VALID(pMap) ) return FALSE;

				// 找到这个目标
				Unit* pTarget = pMap->FindUnit(m_dwTargetUnitID);
				if( !P_VALID(pTarget) ) return FALSE;

				DWORD dwTargetTypeID = GT_INVALID;
				if (pTarget->IsCreature())
				{
					Creature* pCreature = (Creature*)pTarget;
					dwTargetTypeID = pCreature->GetTypeID();
				}

				pScript->CancelSkill(m_pOwner->GetMap(), pSkill->GetID(), m_pOwner->GetID(), m_dwTargetUnitID, dwTargetTypeID);
			}

			EndUseSkill();
		}
		else
		{
			tagNS_UseItemInterrupt send;
			send.dwRoleID = m_pOwner->GetID();
			send.n64ItemID = m_n64ItemID;
			send.dwTypeID = GT_INVALID;

			if( P_VALID(m_pOwner->GetMap()) )
			{
				m_pOwner->GetMap()->SendBigVisTileMsg(m_pOwner, &send, send.dwSize);
			}
			EndUseItem();
		}

		return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------
// 打断释放
//-------------------------------------------------------------------------------
BOOL CombatHandler::InterruptOperate(EInterruptType eType, DWORD dwMisc, BOOL bForce/* =FALSE */)
{
	if( FALSE == IsValid() || FALSE == IsSkillOperating() )
		return TRUE;

	if( EIT_Move == eType )
	{
		EMoveState eState = (EMoveState)dwMisc;

		// 走和游泳相关的移动，则只有移动打断的普通攻击才打断
		if( EMS_Walk			== eState ||
			EMS_Swim			== eState ||
			EMS_CreaturePatrol	== eState ||
			EMS_CreatureWalk	== eState ||
			EMS_CreatureFlee	== eState )
		{
			Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
			if( P_VALID(pSkill) && ESSTE_Default == pSkill->GetTypeEx() && !pSkill->IsMoveable() )
			{
				EndUseSkill();
				return TRUE;
			}
		}
		// 其它移动方式，则只要是普通攻击就打断
		else
		{
			Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
			if( P_VALID(pSkill) && ESSTE_Default == pSkill->GetTypeEx() )
			{
				EndUseSkill();
				return TRUE;
			}
		}
	}

	return FALSE;
}


//-------------------------------------------------------------------------------
// 是否可以使用技能
//-------------------------------------------------------------------------------
INT CombatHandler::CanCastSkill(Skill* pSkill, DWORD dwTargetUnitID)
{
	if( !P_VALID(pSkill) )
		return E_SystemError;

	const tagSkillProto * pSkillProto = pSkill->GetProto();
	if( !P_VALID(pSkillProto) )
		return E_SystemError;
	// 飞升限制
	Unit * pOwner = m_pOwner;
	if( pSkillProto->bSoarLimitLearn && P_VALID(pOwner) && pOwner->IsRole() )
	{
		Role * pRole = (Role*)pOwner;
		if( pRole->GetAttValue(ERA_Soar) != ESV_SoaringUp )
			return E_UseSkill_SelfStateLimit;
	}

	if( CheckSkillConflict(pSkill) ) return E_UseSkill_Operating;

	INT nRet = E_Success;

	nRet = CheckSkillAbility(pSkill);
	if( E_Success != nRet ) return nRet;

	nRet = CheckOwnerLimitSkill();
	if( E_Success != nRet ) return nRet;

	nRet = CheckSkillLimit(pSkill);
	if( E_Success != nRet ) return nRet;

	nRet = CheckTargetLimit(pSkill, dwTargetUnitID);
	if( E_Success != nRet ) return nRet;

	nRet = CheckCostLimit(pSkill);
	if( E_Success != nRet ) return nRet;

	nRet = CheckVocationLimit(pSkill);
	if( E_Success != nRet ) return nRet;

	nRet = CheckMapLimit(pSkill);
	if( E_Success != nRet ) return nRet;

	nRet = CheckSoulActiveLimit(pSkill);
	if( E_Success != nRet ) return nRet;

	const SkillScript* pScript = pSkill->GetSkillScript();
	if (P_VALID(pScript))
	{
		nRet = pScript->CanCastSkill(m_pOwner->GetMap(), pSkill->GetID(), m_pOwner->GetID(), dwTargetUnitID);
		if( E_Success != nRet ) return nRet;
	}

	return nRet;
}

//-------------------------------------------------------------------------------
// 测试技能本身是否能够使用
//-------------------------------------------------------------------------------
INT CombatHandler::CheckSkillAbility(Skill* pSkill)
{
	if( !P_VALID(pSkill) ) return E_UseSkill_SkillNotExist;

	// 被动技能不可以使用
	if( pSkill->IsPassive() )
		return E_UseSkill_PassiveSkill;

	// 凝聚神格技能做特殊处理
	if( pSkill->GetTypeEx2() != ESSTE2_GodCondense )
	{
		// 如果技能的目标类型不是战斗目标或非战斗目标，则不可以使用
		ESkillTargetType eTargetType = pSkill->GetTargetType();
		if( ESTT_Combat != eTargetType && ESTT_NoCombat != eTargetType )
			return E_UseSkill_SkillTargetInvalid;
	}

	// 技能的冷却时间还没到，则不可以使用
	if( pSkill->GetCoolDownCountDown() > 0 )
		return E_UseSkill_CoolDowning;

	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试技能使用者是否能够使用技能
//-------------------------------------------------------------------------------
INT CombatHandler::CheckOwnerLimitSkill()
{
	// 是否处在不能使用技能的状态
	if( m_pOwner->IsInStateCantCastSkill() )
		return E_UseSkill_UseLimit;

	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试技能本身使用限制
//-------------------------------------------------------------------------------
INT CombatHandler::CheckSkillLimit(Skill* pSkill)
{
	if( !P_VALID(pSkill) ) return E_UseSkill_SkillNotExist;

	const tagSkillProto* pProto = pSkill->GetProto();
	if( !P_VALID(pProto) ) return E_UseSkill_SkillNotExist;

	//if(!pSkill->GetCanUseByMap())
	//	return E_UseSkill_MapLimit;

	// 体力低于或者真气低于
	if( pProto->nUseHPPctLimit > 0 )
	{
		if( m_pOwner->GetAttValue(ERA_MaxHP) <= 0 )
			return E_UseSkill_UseLimit;

		if( (FLOAT)m_pOwner->GetAttValue(ERA_HP) / (FLOAT)m_pOwner->GetAttValue(ERA_MaxHP) * 10000.0f < pProto->nUseHPPctLimit )
			return E_UseSkill_UseLimit;

	}
	if( pProto->nUseMPPctLimit > 0 )
	{
		if( m_pOwner->GetAttValue(ERA_MaxMP) <= 0 )
			return E_UseSkill_UseLimit;

		if( (FLOAT)m_pOwner->GetAttValue(ERA_MP) / (FLOAT)m_pOwner->GetAttValue(ERA_MaxMP) * 10000.0f < pProto->nUseMPPctLimit )
			return E_UseSkill_UseLimit;

	}

	// 性别限制
	if( pProto->eSexLimit != ESSL_Null )
	{
		if( ESSL_Man == pProto->eSexLimit )
		{
			if( 1 != m_pOwner->GetSex() )
				return E_UseSkill_SexLimit;
		}
		else if( ESSL_Woman == pProto->eSexLimit )
		{
			if( 0 != m_pOwner->GetSex() )
				return E_UseSkill_SexLimit;
		}
		else
		{

		}
	}

	// 职业限制

	// 武器限制
	if( EITE_Null != pProto->nWeaponLimit && m_pOwner->IsRole() )
	{
		Role* pRole = static_cast<Role*>(m_pOwner);
		tagEquip* pWeapon = pRole->GetItemMgr().GetEquipBarEquip((INT16)EEP_RightHand);
		if( !P_VALID(pWeapon) || pWeapon->pProtoType->eTypeEx != pProto->nWeaponLimit )
		{
			return E_UseSkill_WeaponLimit;
		}
	}

	// 特殊Buff限制
	if( P_VALID(pProto->dwBuffLimitID) )
	{
		if( !m_pOwner->IsHaveBuff(pProto->dwBuffLimitID) )
		{
			return E_UseSkill_SelfBuffLimit;
		}
	}

	// 检查自身状态限制
	DWORD dwSelfStateFlag = m_pOwner->GetStateFlag();
	// Jason 2010-2-3 v1.3.2骑乘状态，取消buff功能
	if( (dwSelfStateFlag & ESF_Mount) && (pProto->dwSelfStateLimit & ESF_Mount ) ) // 自身在骑乘状态，并且技能限制骑乘，则，取消玩家身上的骑乘buff
	{
		if( m_pOwner->IsHaveBuff (Buff::GetIDFromTypeID(MOUNT_BUFF_ID)) )
			m_pOwner->CancelBuff  (Buff::GetIDFromTypeID(MOUNT_BUFF_ID) );
		if( m_pOwner->IsHaveBuff  (Buff::GetIDFromTypeID(MOUNT2_BUFF_ID)) )
			m_pOwner->CancelBuff  (Buff::GetIDFromTypeID(MOUNT2_BUFF_ID) );
	}
	dwSelfStateFlag = m_pOwner->GetStateFlag(); // 状态应该重置，以免影响后续判断
	if( (dwSelfStateFlag & pProto->dwSelfStateLimit) != dwSelfStateFlag )
	{
		return E_UseSkill_SelfStateLimit;
	}
	if( m_pOwner->IsInState(ES_NoMovement) )
		return E_UseSkill_SelfStateLimit;

	if( pProto->nType3 == ESSTE2_GodCondense ) // 凝聚
	{
		if( !g_world.IsGodSystemOpen() )
			return E_UseSkill_CondenseNotOpen;
		if( m_pOwner->IsInState(ES_Dead) )
			return E_UseSkill_CannotCondenseInDeadState;
		if( m_pOwner->GetAttValue(ERA_God_Faith) < 5 )
		{
			Role* pRole = static_cast<Role*>(m_pOwner);
			if(P_VALID(pRole))
			{
				pRole->StopCondense(2,E_Success,TRUE);
			}
			return E_UseSkill_CannotCondenseFaithZero;
		}
		INT nMaxGodHead = 0;
		GetGodHeadLimit(m_pOwner->GetLevel(),nMaxGodHead);
		if( nMaxGodHead == 0 )
			return E_UseSkill_CannotCondenseGodHeadLimitZero;
		if( m_pOwner->GetAttValue(ERA_God_Godhead) >= nMaxGodHead )
			return E_UseSkill_CannotCondenseGodHeadFull;
	}
	if( pProto->nType2 == ESSTE_Transform )
	{
		if( m_pOwner->IsRole() )
		{
			if( !((Role*)m_pOwner)->IsInRoleState(ERS_Transform) )
				return E_UseSkill_SelfStateLimit;
		}
		else
			return E_UseSkill_SelfStateLimit;
	}

	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试目标限制
//-------------------------------------------------------------------------------
INT CombatHandler::CheckTargetLimit(Skill* pSkill, DWORD dwTargetUnitID)
{
	if( !P_VALID(pSkill) )
		return E_UseSkill_SkillNotExist;

	const tagSkillProto* pProto = pSkill->GetProto();
	if( !P_VALID(pProto) ) return E_UseSkill_SkillNotExist;

	// 如果TargetUnitID是GT_INVALID，则需要特殊判断一下
	if( GT_INVALID == dwTargetUnitID )
	{
		if( ESOPT_Explode == pSkill->GetOPType() && 0.0f == pSkill->GetOPDist() )
		{
			return E_Success;
		}
		else
		{
			return E_UseSkill_SkillTargetInvalid;
		}
	}

	Unit* pTarget = m_pOwner->GetMap()->FindUnit(dwTargetUnitID);
	if( !P_VALID(pTarget) ) return E_UseSkill_SkillTargetInvalid;

	// 目标对象逻辑限制
	INT nRet = CheckTargetLogicLimit(pSkill, pTarget);
	if( nRet != E_Success )	return nRet;

	// 位置限制，距离限制和范围判断
	if( m_pOwner != pTarget )
	{
		// 位置限制
		if( ESPT_NUll != pProto->ePosType )
		{
			if( ESPT_Front == pProto->ePosType )
			{
				if( FALSE == m_pOwner->IsInFrontOfTarget(*pTarget) )
					return E_UseSkill_PosLimitFront;
			}
			else if( ESPT_Back == pProto->ePosType )
			{
				if( TRUE == m_pOwner->IsInFrontOfTarget(*pTarget) )
					return E_UseSkill_PosLimitBack;
			}
		}

		// 目标距离判断
		if( FALSE == m_pOwner->IsInCombatDistance(*pTarget, pSkill->GetOPDist()) )
			return E_UseSkill_DistLimit;

		// 射线检测
		if( m_pOwner->IsRayCollide(*pTarget) )
			return E_UseSkill_RayLimit;
	}

	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试目标逻辑限制
//-------------------------------------------------------------------------------
INT CombatHandler::CheckTargetLogicLimit(Skill* pSkill, Unit* pTarget)
{
	if( !P_VALID(pSkill) || !P_VALID(pTarget) )
		return E_UseSkill_SkillNotExist;

	const tagSkillProto* pProto = pSkill->GetProto();
	if( !P_VALID(pProto) ) return E_UseSkill_SkillNotExist;

	// 检测目标是否不能被使用技能
	if( pTarget->IsInStateCantBeSkill() )
	{
		return E_UseSkill_TargetLimit;
	}

	bool bIsFairySoul = false,bSkillFairy = false;
	if( pTarget->IsCreature() )
	{
		Creature * pFairySoul = NULL;
		pFairySoul = (Creature*)pTarget;
		if( pFairySoul->IsFairySoul() )
			bIsFairySoul = true;
	}

	if( pProto->nType2 == (INT)ESSTE_FairySoul )
	{
		bSkillFairy = true;
	}

	// 妖精遗产，妖精宝箱
	if( pProto->nType2 == ESSTE_FairyHeitage )
	{
		if( pTarget->IsCreature() )
		{
			Creature * pCreature = (Creature*)pTarget;
			if( !pCreature->IsFairyHeritage() )
				return E_UseSkill_TargetLimit;
		}
	}

	if( (bIsFairySoul || bSkillFairy) && !(bIsFairySoul && bSkillFairy) )
	{
		return E_UseSkill_TargetLimit;
	}
	else if( bIsFairySoul && bSkillFairy )
	{
		if( !g_world.IsFairyContractOpen() )
			return E_UseSkill_TargetInvalid;

		if( m_pOwner->IsRole() )
		{
			Role * pSrcUnit = (Role*)m_pOwner;
			if( pSrcUnit->GetItemMgr().GetBagFreeSize() < 1 )
				return E_Bag_NotEnoughSpace;
		}
		return E_Success;
	}

	// 首先检测与目标的类型标志tbc:inves
	DWORD dwTargetFlag = m_pOwner->TargetTypeFlag(pTarget);
	if( !(dwTargetFlag & pProto->dwTargetLimit) )
		return E_UseSkill_TargetLimit;

	// 再检测目标的状态限制
	DWORD dwTargetStatFlag = pTarget->GetStateFlag();
	if( (dwTargetStatFlag & pProto->dwTargetStateLimit) != dwTargetStatFlag )
	{
		return E_UseSkill_TargetStateLimit;
	}

	// 检测目标Buff限制
	if( P_VALID(pProto->dwTargetBuffLimitID) )
	{
		if( !pTarget->IsHaveBuff(pProto->dwTargetBuffLimitID) )
		{
			return E_UseSkill_TargetBuffLimit;
		}
	}	

	// 再检测敌我判断
	if( m_pOwner != pTarget )
	{
		DWORD dwFriendEnemyFlag = m_pOwner->FriendEnemy(pTarget);

		DWORD dwFriendEnemyLimit = 0;

		if( pProto->bFriendly )		dwFriendEnemyLimit |= ETFE_Friendly;
		if( pProto->bHostile )		dwFriendEnemyLimit |= ETFE_Hostile;
		if( pProto->bIndependent )	dwFriendEnemyLimit |= ETFE_Independent;

		if( !(dwFriendEnemyLimit & dwFriendEnemyFlag) )
		{
			return E_UseSkill_TargetLimit;
		}
	}

	// 判断成功
	return E_Success;
}

//----------------------------------------------------------------------------------------
// 检测地图中技能限制
//----------------------------------------------------------------------------------------
INT CombatHandler::CheckMapLimit(Skill* pSkill)
{
	// 判断地图限制
	if(P_VALID(m_pOwner->GetMap()))
	{
		BOOL bUesAble = m_pOwner->GetMap()->CanUseSkill(pSkill->GetID());
		if( !bUesAble )	return E_UseSkill_MapLimit;
	}

	return E_Success;
}

//----------------------------------------------------------------------------------------
// 测试技能使用冲突，返回TRUE为冲突，FALSE为非冲突
//----------------------------------------------------------------------------------------
BOOL CombatHandler::CheckSkillConflict(Skill* pSkill)
{
	ASSERT( P_VALID(pSkill) );

	if( !IsValid() ) return FALSE;		// 当前没有使用任何技能和任何物品

	if( IsPreparing() ) return TRUE;	// 当前正在起手，则不能使用任何技能

	if( IsUseSkill() )
	{
		// 当前正在使用技能，则查看该技能是否是普通攻击
		Skill* pCurSkill = m_pOwner->GetSkill(m_dwSkillID);
		if( P_VALID(pSkill) && P_VALID(pCurSkill) && ESSTE_Default != pCurSkill->GetTypeEx() )
		{
			return TRUE;
		}
		else
		{
			EndUseSkill();
			return FALSE;
		}
	}

	return FALSE;
}

//-------------------------------------------------------------------------------
// 测试技能消耗
//-------------------------------------------------------------------------------
INT CombatHandler::CheckCostLimit(Skill* pSkill)
{
	// 检测体力消耗
	INT nHPCost = pSkill->GetCost(ESCT_HP);
	if( nHPCost > 0 && m_pOwner->GetAttValue(ERA_HP) < nHPCost )
		return E_UseSkill_HPLimit;


	// 检测真气消耗
	INT nMPCost = pSkill->GetCost(ESCT_MP);
	if( nMPCost > 0 && m_pOwner->GetAttValue(ERA_MP) < nMPCost )
		return E_UseSkill_MPLimit;


	// 检测怒气消耗
	INT nRageCost = pSkill->GetCost(ESCT_Rage);
	if( nRageCost > 0 && m_pOwner->GetAttValue(ERA_Rage) < nRageCost )
		return E_UseSkill_RageLimit;


	// 检测持久消耗
	INT nEnduranceCost = pSkill->GetCost(ESCT_Endurance);
	if( nEnduranceCost > 0 && m_pOwner->GetAttValue(ERA_Endurance) < nEnduranceCost )
		return E_UseSkill_EnduranceLimit;


	// 检测活力消耗
	INT nValicityCost = pSkill->GetCost(ESCT_Valicity);

	//INT nVitality = GetSpecSkillValue(ESSF_Valicity,ESSV_ALL,nValicityCost);

	if( nValicityCost > 0 && m_pOwner->GetAttValue(ERA_Vitality) < nValicityCost )
		return E_UseSkill_ValicityLimit;

	//if( nVitality > 0 && m_pOwner->GetAttValue(ERA_Vitality) < nVitality )
	//	return E_UseSkill_ValicityLimit;

	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试职业限制
//-------------------------------------------------------------------------------
INT CombatHandler::CheckVocationLimit(Skill* pSkill)
{
	//ASSERT(P_VALID(pSkill));
	if (!P_VALID(pSkill)) return E_UseSkill_SkillNotExist;

	if (!m_pOwner->IsRole()) return E_Success;

	const tagSkillProto* pProto = pSkill->GetProto();
	if( !P_VALID(pProto) ) return E_UseSkill_SkillNotExist;

	//INT nClass = (INT)((Role*)m_pOwner)->GetClass();
	//INT nClassEx = (INT)((Role*)m_pOwner)->GetClassEx();
	INT nClass = (INT)(static_cast<Role*> (m_pOwner)->GetClass());
	INT nClassEx = (INT)(static_cast<Role*> (m_pOwner)->GetClassEx());

	INT nTmpClass =  1 << ( nClass - 1 );
	INT nTmpClassEx = 0;
	INT nTmp = 0;

	if ( (INT)nClassEx != (INT)EHV_Base )
	{
		nTmpClassEx = 1 << ( nClassEx + 8 );
	}

	nTmp = nTmpClass + nTmpClassEx;

	if ( !( nTmp & pProto->dwVocationLimit ) )
		return E_UseSkill_VocationLimit;

	return E_Success;
}

//-------------------------------------------------------------------------------
// 计算攻击目标，放入到list中 
//-------------------------------------------------------------------------------
VOID CombatHandler::CalSkillTargetList()
{
	m_listTargetID.Clear();
	m_listHitedTarget.Clear();
	m_listDodgedTarget.Clear();
	m_listBlockedTarget.Clear();
	m_listCritedTarget.Clear();
	m_listIgnoreTarget.Clear();
	m_dwTargetEffectFlag = 0;

	// 根据该技能的攻击距离和攻击范围来判断
	Skill* pSkill = m_pOwner->GetSkill(m_dwSkillID);
	if( !P_VALID(pSkill) ) return;

	// 得到目标对象
	Unit* pTarget = NULL;
	if( GT_INVALID == m_dwTargetUnitID )	// 如果没有选目标，则目标就是自己
	{
		pTarget = m_pOwner;
	}
	else									// 如果选了目标，则找到目标
	{
		pTarget = m_pOwner->GetMap()->FindUnit(m_dwTargetUnitID);
	}
	if( !P_VALID(pTarget) ) return;

	// 根据作用类型，作用距离和作用半径来使用技能
	ESkillOPType eOPType = pSkill->GetOPType();
	FLOAT fOPDist = pSkill->GetOPDist();
	FLOAT fOPRadius = pSkill->GetOPRadius();
	INT iMaxAttack = pSkill->GetMaxAttackNum();

	if(iMaxAttack <= 0 ) return;
	// 先将目标加进去
	if( m_pOwner != pTarget )
	{
		m_listTargetID.PushBack(pTarget->GetID());
		m_dwTargetEffectFlag = CalculateSkillEffect(pSkill, pTarget);
		if(--iMaxAttack <= 0 ) return;
	}
	else if( ESOPT_Explode == eOPType && fOPDist == 0 && fOPRadius > 0 &&
			 ESDGT_Null != pSkill->GetDmgType() && !pSkill->IsFriendly() && pSkill->IsHostile() )
	{
		m_dwTargetEffectFlag |= ETEF_Hited;
		m_dwTargetEffectFlag |= ETEF_Block;
		m_dwTargetEffectFlag |= ETEF_Crited;
	}

	Role * pOwnerRole = NULL;
	if( m_pOwner->IsRole() )
		pOwnerRole = (Role*)m_pOwner;

	// 爆炸效果
	if( ESOPT_Explode == eOPType )
	{
		// 如果攻击范围为0，则直接返回
		if( 0.0f == fOPRadius )
			return;

		// 如果攻击范围不为0，则以目标为球心检测
		FLOAT fOPRadiusSQ = fOPRadius * fOPRadius;

		tagVisTile* pVisTile[ED_End] = {0};

		// 得到攻击范围内的vistile列表
		pTarget->GetMap()->GetVisTile(pTarget->GetCurPos(), fOPRadius, pVisTile);
		Role*		pRole		= NULL;
		Creature*	pCreature	= NULL;

		for(INT n = ED_Center; n < ED_End; n++)
		{
			if( !P_VALID(pVisTile[n]) ) continue;

			// 首先检测人物
			TMap<DWORD, Role*> mapRole = pVisTile[n]->mapRole; // 使用临时map 防止角色被打出该区域（律*风雷引）
			TMap<DWORD, Role*>::TMapIterator it = mapRole.Begin();

			while( mapRole.PeekNext(it, pRole) )
			{
				// 和目标一样，不做处理
				if( pRole == pTarget || pRole == m_pOwner ) continue;

				// 目标对象限制判断
				if( E_Success != CheckTargetLogicLimit(pSkill, pRole) )
					continue;

				// 技能距离判断
				FLOAT fDistSQ = Vec3DistSq(pTarget->GetCurPos(), pRole->GetCurPos());
				if( fDistSQ > fOPRadiusSQ  ) continue;

				// 射线检测

				// pk模式检查
				//if( P_VALID(pOwnerRole) && pSkill->IsFriendly() && !pOwnerRole->InSamePKState(pRole) )
				//	continue;

				// 判断通过，则将玩家加入到列表中
				m_listTargetID.PushBack(pRole->GetID());

				// 计算技能作用结果
				CalculateSkillEffect(pSkill, pRole);

				if(--iMaxAttack <= 0 ) return;
			}

			// 再检测生物
			TMap<DWORD, Creature*> mapCreature = pVisTile[n]->mapCreature;  // 使用临时map 防止skill打死怪物导致宕机
			TMap<DWORD, Creature*>::TMapIterator it2 = mapCreature.Begin();

			while( mapCreature.PeekNext(it2, pCreature) )
			{
				// 和目标一样，不做处理
				if( pCreature == pTarget || pCreature == m_pOwner ) continue;

				//if(!pVisTile[n]->mapCreature.IsExist(pCreature->GetID())) continue;	// 已经被删除

				// 目标对象限制判断
				if( E_Success != CheckTargetLogicLimit(pSkill, pCreature) )
					continue;

				// 技能距离判断
				FLOAT fDistSQ = Vec3DistSq(pTarget->GetCurPos(), pCreature->GetCurPos());
				if( fDistSQ > fOPRadiusSQ  ) continue;

				// 射线检测

				// 判断通过，则将生物加入到列表中
				m_listTargetID.PushBack(pCreature->GetID());

				// 计算技能作用结果
				CalculateSkillEffect(pSkill, pCreature);

				if(--iMaxAttack <= 0 ) return;
			}
		}
	}

	// 矩形
	else if( ESOPT_Rect == eOPType )
	{
		// 如果攻击范围或者攻击距离为0，则直接返回
		if( 0.0f == fOPRadius || 0.0f == fOPDist )	return;

		// 如果攻击范围不为0，则以目标为球心检测
		FLOAT fOPRadiusSQ = fOPRadius * fOPRadius;
		FLOAT fOPDistSQ = fOPDist * fOPDist;

		// 如果攻击范围和攻击距离均不为0，则以自身为基准检测
		FLOAT fTargetX = pTarget->GetCurPos().x;
		FLOAT fTargetY = pTarget->GetCurPos().y;
		FLOAT fTargetZ = pTarget->GetCurPos().z;
		FLOAT fSrcX = m_pOwner->GetCurPos().x;
		FLOAT fSrcY = m_pOwner->GetCurPos().y;
		FLOAT fSrcZ = m_pOwner->GetCurPos().z;

		// 自身到第一目标点的向量
		FLOAT fX2 = fTargetX - fSrcX;
		FLOAT fY2 = fTargetY - fSrcY;
		FLOAT fZ2 = fTargetZ - fSrcZ;

		// 如果目标就是自身，那么直接取自身的朝向向量
		if( m_pOwner == pTarget )
		{
			fX2 = m_pOwner->GetFaceTo().x;
			fZ2 = m_pOwner->GetFaceTo().z;
		}

		if( abs(fX2) < 0.001f && abs(fZ2) < 0.001f )
			return;

		// 自身到第一目标点的距离的平方
		FLOAT fDistSQ2 = fX2*fX2 + fY2*fY2 + fZ2*fZ2;

		tagVisTile* pVisTile[ED_End] = {0};

		// 得到vistile列表
		pTarget->GetMap()->GetVisTile(m_pOwner->GetVisTileIndex(), pVisTile);
		Role*		pRole		= NULL;
		Creature*	pCreature	= NULL;

		for(INT n = ED_Center; n < ED_End; n++)
		{
			if( !P_VALID(pVisTile[n]) ) continue;

			// 首先检测人物
			TMap<DWORD, Role*> mapRole = pVisTile[n]->mapRole;	 // 使用临时map 防止角色被打出该区域（律*风雷引）
			TMap<DWORD, Role*>::TMapIterator it = mapRole.Begin();

			while( mapRole.PeekNext(it, pRole) )
			{
				// 和目标一样，不做处理
				if( pRole == pTarget || pRole == m_pOwner ) continue;

				// 目标对象限制判断
				if( E_Success != CheckTargetLogicLimit(pSkill, pRole) )
					continue;

				// 自身到当前点的向量
				FLOAT fX1 = pRole->GetCurPos().x - fSrcX;
				FLOAT fY1 = pRole->GetCurPos().y - fSrcY;
				FLOAT fZ1 = pRole->GetCurPos().z - fSrcZ;

				// 先检查方位 cos(a) > 0 
				if( fX1*fX2	+ fZ1*fZ2 < 0.0f )
					continue;

				FLOAT fDist1 = fX1*fX2 + fY1*fY2 + fZ1*fZ2;
				FLOAT fDistSQ1 = fDist1 * fDist1;

				// 检查投影距离
				FLOAT fProjDistSQ = fDistSQ1 / fDistSQ2;
				if( fProjDistSQ > fOPDistSQ )
					continue;

				// 检查点到直线距离
				if( fX1*fX1 + fY1*fY1 + fZ1*fZ1 - fProjDistSQ > fOPRadiusSQ)
					continue;

				// 射线检测

				// pk模式检查
				if( P_VALID(pOwnerRole) && pSkill->IsFriendly() && !pOwnerRole->InSamePKState(pRole) )
					continue;

				// 判断通过，则将玩家加入到列表中
				m_listTargetID.PushBack(pRole->GetID());

				// 计算技能作用结果
				CalculateSkillEffect(pSkill, pRole);

				if(--iMaxAttack <= 0 ) return;
			}

			// 再检测生物
			TMap<DWORD, Creature*> mapCreature = pVisTile[n]->mapCreature;	// 使用临时map 防止skill打死怪物导致宕机
			TMap<DWORD, Creature*>::TMapIterator it2 = mapCreature.Begin();

			while( mapCreature.PeekNext(it2, pCreature) )
			{
				// 和目标一样，不做处理
				if( pCreature == pTarget || pCreature == m_pOwner ) continue;

				//if(!pVisTile[n]->mapCreature.IsExist(pCreature->GetID())) continue;	// 已经被删除

				// 目标对象限制判断
				if( E_Success != CheckTargetLogicLimit(pSkill, pCreature) )
					continue;

				// 自身到当前点的向量
				FLOAT fX1 = pCreature->GetCurPos().x - fSrcX;
				FLOAT fY1 = pCreature->GetCurPos().y - fSrcY;
				FLOAT fZ1 = pCreature->GetCurPos().z - fSrcZ;

				// 先检查方位 cos(a) > 0 
				if( fX1*fX2	+ fZ1*fZ2 < 0.0f )
					continue;

				// 检查投影距离
				FLOAT fProjDistSQ = (fX1*fX2 + fY1*fY2 + fZ1*fZ2) / fDistSQ2;
				if( fProjDistSQ > fOPDistSQ )
					continue;

				// 检查点到直线距离
				if( fX1*fX1 + fY1*fY1 + fZ1*fZ1 - fProjDistSQ > fOPRadiusSQ)
					continue;

				// 射线检测

				// 判断通过，则将玩家加入到列表中
				m_listTargetID.PushBack(pCreature->GetID());

				// 计算技能作用结果
				CalculateSkillEffect(pSkill, pCreature);

				if(--iMaxAttack <= 0 ) return;
			}
		}
	}
}

//-------------------------------------------------------------------------------
// 计算技能效果
//-------------------------------------------------------------------------------
DWORD CombatHandler::CalculateSkillEffect(Skill* pSkill, Unit* pTarget)
{
	DWORD dwTargetEffectFlag = 0;

	DWORD dwTargetID = pTarget->GetID();

	INT nDmgTimes = pSkill->GetDmgTimes();

	// 无伤害技能，必命中
	if( nDmgTimes <= 0 )
	{
		m_listHitedTarget.PushBack(dwTargetID);
		dwTargetEffectFlag |= ETEF_Hited;

		tagNS_HitTarget send;
		send.dwRoleID = pTarget->GetID();
		send.dwSrcRoleID = m_pOwner->GetID();
		send.eCause = EHTC_Skill;
		send.dwMisc = pSkill->GetTypeID();
		send.dwSerial = m_dwSkillSerial;

		if( P_VALID(pTarget->GetMap()) )
		{
			pTarget->GetMap()->SendBigVisTileMsg(pTarget, &send, send.dwSize);
		}

		pTarget->OnBeAttacked(m_pOwner, pSkill, TRUE, FALSE, FALSE);
		return dwTargetEffectFlag;
	}

	// 计算命中
	bool bIgnore = false;
	BOOL bHit = CalculateHit(pSkill, pTarget, bIgnore);
	if( FALSE == bHit )
	{
		// 未命中
		m_listDodgedTarget.PushBack(dwTargetID);
	}
	else
	{
		// 命中
		m_listHitedTarget.PushBack(dwTargetID);
		dwTargetEffectFlag |= ETEF_Hited;

		tagNS_HitTarget send;
		send.dwRoleID = pTarget->GetID();
		send.dwSrcRoleID = m_pOwner->GetID();
		send.eCause = EHTC_Skill;
		send.dwMisc = pSkill->GetTypeID();
		send.dwSerial = m_dwSkillSerial;

		if( P_VALID(pTarget->GetMap()) )
		{
			pTarget->GetMap()->SendBigVisTileMsg(pTarget, &send, send.dwSize);
		}

		if (bIgnore)
		{
			// 技能穿透无视 [3/20/2012 zhangzhihua]
			m_listIgnoreTarget.PushBack(dwTargetID);
			// 计算暴击
			BOOL bCrit = CalculateCritRate(pSkill, pTarget);
			if( TRUE == bCrit )
			{
				m_listCritedTarget.PushBack(dwTargetID);
				dwTargetEffectFlag |= ETEF_Crited;
			}
		}
		else
		{
			// 计算格挡
			BOOL bBlocked = CalculateBlock(pSkill, pTarget);

			if(TRUE == bBlocked)
			{
				// 被格挡
				m_listBlockedTarget.PushBack(dwTargetID);
				dwTargetEffectFlag |= ETEF_Block;
			}
			else
			{
				// 计算暴击
				BOOL bCrit = CalculateCritRate(pSkill, pTarget);
				if( TRUE == bCrit )
				{
					m_listCritedTarget.PushBack(dwTargetID);
					dwTargetEffectFlag |= ETEF_Crited;
				}
			}
		}
	}

	// 被攻击方的被攻击触发
	pTarget->OnBeAttacked(m_pOwner, pSkill,
		dwTargetEffectFlag & ETEF_Hited, dwTargetEffectFlag & ETEF_Block, dwTargetEffectFlag & ETEF_Crited);

	return dwTargetEffectFlag;
}


//--------------------------------------------------------------------------------
// 计算命中
// F-Project
// 2010-6-9 修改命中率计算，统一攻击计算公式
//--------------------------------------------------------------------------------
BOOL CombatHandler::CalculateHit(Skill* pSkill, Unit* pTarget, bool& bIgnore)
{
	FLOAT fHit = 0.0f;
	bIgnore = false;

	//// 外功攻击
	//if( pSkill->IsExAttackSkill() )
	//{
	//	// 命中率=0.9×[1-（防御方当前闪避-攻击方当前命中）/10000]×（1-攻击方攻击失误率）÷（1+防御方近、远程躲避率）+技能命中率
	//	fHit = 0.9f * (1.0f - (FLOAT)(pTarget->GetAttValue(ERA_Dodge) - m_pOwner->GetAttValue(ERA_HitRate)) / 10000.0f);
	//	if( fHit < 0.0f ) fHit = 0.0f;
	//	if( fHit > 1.0f ) fHit = 1.0f;

	//	fHit = fHit	* (1.0f - FLOAT(m_pOwner->GetAttValue(ERA_Attack_MissRate)) / 10000.0f);

	//	// 根据是近身还是远程决定选择哪个躲避率
	//	if( pSkill->IsMelee() )
	//	{
	//		fHit = fHit / (1.0f + FLOAT(pTarget->GetAttValue(ERA_CloseAttack_DodgeRate)) / 10000.0f);
	//	}
	//	else if( pSkill->IsRanged() )
	//	{
	//		fHit = fHit / (1.0f + FLOAT(pTarget->GetAttValue(ERA_RemoteAttack_DodgeRate)) / 10000.0f);
	//	}
	//}

	//// 内功攻击
	//else if( pSkill->IsInAttackSkill() )
	//{
	//	// 命中率=1×[1-（防御方当前闪避-攻击方当前命中）/8500]×（1-攻击方攻击失误率）+技能命中率
	//	fHit = 1.0f - (FLOAT)(pTarget->GetAttValue(ERA_Dodge) - m_pOwner->GetAttValue(ERA_HitRate)) / 8500.0f;
	//	if( fHit < 0.0f ) fHit = 0.0f;
	//	if( fHit > 1.0f ) fHit = 1.0f;

	//	fHit = fHit * (1.0f - FLOAT(m_pOwner->GetAttValue(ERA_Attack_MissRate)) / 10000.0f);
	//}

	//// 绝技攻击
	//else if( pSkill->IsStuntSkill() )
	//{
	//	// 命中率=100%×（1-攻击方攻击失误率）+技能命中率
	//	fHit = 1.0f - FLOAT(m_pOwner->GetAttValue(ERA_Attack_MissRate)) / 10000.0f;
	//}

	//// else
	//else
	//{

	//}

	//// 增加技能命中率
	//fHit += pSkill->GetHit();

	//// 范围：30——100%
	//if( fHit < 0.3f ) fHit = 0.3f;
	//if( fHit > 1.0f ) fHit = 1.0f;

	// F-Project start
	//元素穿透大于元素防御2倍以上打出无视效果为1%，3倍为2%以此类推  （（人物元素穿透+妖精通用技能元素穿透）/（人物元素防御+1）-1）/100*100%
	INT nPierce = m_pOwner->GetAttValue(ERA_EEP_VALUE);
	INT nResist = pTarget->GetAttValue(ERA_EER_START + m_pOwner->GetAttValue(ERA_EE_ATTR)) + pTarget->GetAttValue(ERA_EER_ALL);
	INT nIgnore = nPierce / (nResist + 1);
	if (nIgnore >= 2)
	{
		if (nIgnore > 15 && m_pOwner->IsRole())
		{
			nIgnore = 15;
		}
		else
		{
			nIgnore = (nPierce + 0/*m_pOwner->GetAttValue()*/) / (nResist + 1);
		}
		if (IUTIL->Probability(nIgnore))
		{
			bIgnore = true;
			return TRUE;
		}
	}

	/*
	命中率=0.9×[1-（防御方灵巧-攻击方精准）/10000]×（1-攻击方攻击失误率）÷（1+防御方近、远程躲避率）+技能命中率+[0.004166%*（穿透强度-穿透抵抗*45%）]
	其中，0.9×[1-（防御方灵巧-攻击方精准）/10000]的取值范围为30%~100% [0.004166%*（穿透强度-穿透抵抗*45%）]的取值范围在0%-15%
	*/
	FLOAT fBase = 0;
	fBase = 0.9 * ( 1- ( FLOAT( pTarget->GetAttValue(ERA_DefenseTec) - m_pOwner->GetAttValue(ERA_AttackTec) ) / 10000 ) );
	if( fBase < 0.3f ) fBase = 0.3f;
	if( fBase > 1.0f ) fBase = 1.0f;

	FLOAT fDodgeRate = 0; // 躲避率
	if( pSkill->IsMelee() )
	{
		fDodgeRate = (FLOAT)pTarget->GetAttValue(ERA_CloseAttack_DodgeRate) / 10000;
	}
	else if( pSkill->IsRanged() )
	{
		fDodgeRate = (FLOAT)pTarget->GetAttValue(ERA_RemoteAttack_DodgeRate) / 10000;
	}

	FLOAT fPierceRate = 0.00004166 * (nPierce - nResist * 0.45);
	if (fPierceRate < 0.0f) fPierceRate = 0.0f;
	if (fPierceRate > 0.25f) fPierceRate = 0.25f;
	
	fHit = fBase * ( 1 - (FLOAT)m_pOwner->GetAttValue(ERA_Attack_MissRate) / 10000 - fDodgeRate ) + pSkill->GetHit() + fPierceRate + (FLOAT)(m_pOwner->GetAttValue(ERA_HitRateAdd) / 10000);

	// F-Project end

	// 随机看是否能命中
	if	(IUTIL->Rand() % 100 < 50)
	{
		return TRUE;
	}
	else
	{
	return IUTIL->Probability(INT(fHit*100.0f));
	}
}

//----------------------------------------------------------------------------
// 计算格挡
// 2010-6-9 修改
//----------------------------------------------------------------------------
BOOL CombatHandler::CalculateBlock(Skill* pSkill, Unit* pTarget)
{
	//// 只有攻击放处在防御方前面时，防御方才可以格挡
	//if( FALSE == m_pOwner->IsInFrontOfTarget(*pTarget) )
	//	return FALSE;

	//// 格挡率
	//FLOAT fBlock = 0.0f;

	//// 外功攻击
	//if( pSkill->IsExAttackSkill() )
	//{
	//	// 远程攻击
	//	if( pSkill->IsMelee() )
	//	{
	//		// 基础格挡率=（防御方当前外功防御-（攻击方外功攻击+攻击方内功攻击）/4）/30000
	//		// 格挡率=[1+（防御方当前防御技巧-攻击方当前攻击技巧）/6000] ×基础格挡率+防御方格档几率加乘
	//		FLOAT fBaseBlock = (FLOAT(pTarget->GetAttValue(ERA_ExDefense)) - FLOAT(m_pOwner->GetAttValue(ERA_ExAttack) + m_pOwner->GetAttValue(ERA_InAttack)) / 4.0f) / 30000.0f;
	//		fBlock = (1.0f + FLOAT(pTarget->GetAttValue(ERA_DefenseTec) - m_pOwner->GetAttValue(ERA_AttackTec)) / 6000.0f) * fBaseBlock + (FLOAT)pTarget->GetAttValue(ERA_Block_Rate) / 10000.0f;
	//	}

	//	// 远程攻击
	//	else if( pSkill->IsRanged() )
	//	{
	//		// 格挡率=0
	//		fBlock = 0.0f;
	//	}		
	//}

	//// 内功攻击
	//else if( pSkill->IsInAttackSkill() )
	//{
	//	// 基础格挡率=（防御方当前内功防御-（攻击方外功攻击+攻击方内功攻击）/4）/30000
	//	// 格挡率=[1+（防御方当前防御技巧-攻击方当前攻击技巧）/6600] ×基础格挡率+防御方格档几率加乘
	//	FLOAT fBaseBlock = (FLOAT(pTarget->GetAttValue(ERA_InDefense)) - FLOAT(m_pOwner->GetAttValue(ERA_ExAttack) + m_pOwner->GetAttValue(ERA_InAttack)) / 4.0f) / 30000.0f;
	//	fBlock = (1.0f + FLOAT(pTarget->GetAttValue(ERA_DefenseTec) - m_pOwner->GetAttValue(ERA_AttackTec)) / 6600.0f) * fBaseBlock + (FLOAT)pTarget->GetAttValue(ERA_Block_Rate) / 10000.0f;
	//}

	//// 绝技攻击
	//else if( pSkill->IsStuntSkill() )
	//{
	//	fBlock = 0.0f;
	//}

	//// else
	//else
	//{

	//}

	//// 范围：0——100%
	//if( fBlock < 0.0f ) fBlock = 0.0f;
	//if( fBlock > 1.0f ) fBlock = 1.0f;


	// F-Project start
	// 格挡率
	FLOAT fBlock = 0.0f;
	// 远程外功击不可被格挡
	if( pSkill->IsRanged() )
		return FALSE;

	FLOAT fBaseBlock = /*(FLOAT)pTarget->GetAttValue(ERA_ExDefense) - ( (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) + (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) / 4 );
	fBaseBlock /= 30000*/
						pow((FLOAT)pTarget->GetAttValue(ERA_ExDefense) / 30000,0.5f) / 6;

	if(fBaseBlock < 0)
		fBaseBlock = 0;
	else if(fBaseBlock > 1)
		fBaseBlock = 1;

	FLOAT fBlock1 = 1 + ( (FLOAT)pTarget->GetAttValue(ERA_DefenseTec) - (FLOAT)m_pOwner->GetAttValue(ERA_AttackTec) ) / 6000;
	if( fBlock1 < 0 )
		fBlock1 = 0;
	else if( fBlock1 > 1 ) // 上限值待定
		fBlock1 = 1;

	fBlock = fBlock1 * fBaseBlock + (FLOAT)pTarget->GetAttValue(ERA_Block_Rate) / 10000;
	// F-Project end

	// 随机看是否能命中
		if	(IUTIL->Rand() % 100 < 50)
	{
		return FALSE;
	}
	else
	{
	return IUTIL->Probability(INT(fBlock*100.0f));
		}
}

//-----------------------------------------------------------------------------
// 计算致命率
// 2010-6-9 修改
//-----------------------------------------------------------------------------
BOOL CombatHandler::CalculateCritRate(Skill* pSkill, Unit* pTarget)
{
	//// 基础致命率=（攻击方当前攻击技巧/150000）^0.5
	//// 致命率=基础致命率×(1-（被攻击灵巧-攻击方精准）/5000)+（攻击方致命加成-防御方韧性）/1000+技能附带致命率
	//// （攻击方致命加成-防御方韧性）取值范围为（0，1000）

	FLOAT fCrit = m_pOwner->GetAttValue(ERA_AttackTec) / 150000.0f;
	fCrit = pow(fCrit,0.5f);
	fCrit = fCrit - fCrit * ( (FLOAT)pTarget->GetAttValue(ERA_DefenseTec) - (FLOAT)m_pOwner->GetAttValue(ERA_AttackTec) ) / 5000;
	FLOAT fRealCritRate =  (FLOAT)m_pOwner->GetAttValue(ERA_Crit_Rate) - (FLOAT)pTarget->GetAttValue(ERA_Toughness_god);
	if (fRealCritRate < 0)
		fRealCritRate = 0;
	else if (fRealCritRate > 1000.0f)
		fRealCritRate = 1000.0f;
	fCrit = fCrit + fRealCritRate/ 1000.0f + pSkill->GetCrit();

	// 范围：0——100%
	if( fCrit < 0.0f ) fCrit = 0.0f;
	if( fCrit > 1.0f ) fCrit = 1.0f;

	// F-Project start
	// F-Project end

	// 随机看是否能命中
	BOOL bRet = IUTIL->Probability(INT(fCrit*100.0f));
	if (bRet && m_pOwner->IsRole())
	{
		const RoleScript* pRoleScript = g_ScriptMgr.GetRoleScript();
		if (P_VALID(pRoleScript))
			bRet = pRoleScript->CanCalCrit((Role*)m_pOwner, pTarget->GetID());
	}

	return bRet;
}

//-----------------------------------------------------------------------------
// 计算致命量
// 2010-6-9 修改
//-----------------------------------------------------------------------------
FLOAT CombatHandler::CalculateCritAmount(Skill* pSkill, Unit* pTarget)
{
	// 外功攻击
	if( pSkill->IsExAttackSkill() )
	{
		// 致命伤害量加成=（攻击方当前外功攻击/2000）^0.25+（攻击方致命量加成（装备、技能、物品等影响）-防御方暴击抵消） / 10000
		// （攻击方致命量加成-防御方暴击抵消）取值范围为（0，10000）
		FLOAT fRealCritAmount = (FLOAT)m_pOwner->GetAttValue(ERA_Crit_Amount) - (FLOAT)pTarget->GetAttValue(ERA_Toughness_strength);
		if (fRealCritAmount < 0)
			fRealCritAmount = 0;
		else if (fRealCritAmount > 10000.0f)
			fRealCritAmount = 10000.0f;
		FLOAT fRet = pow(FLOAT(m_pOwner->GetAttValue(ERA_ExAttack) / 8000.0f), 0.25f) + fRealCritAmount / 10000.0f;
		return fRet;
	}

	// 内功攻击
	else if( pSkill->IsInAttackSkill() )
	{
		// 致命伤害量加成=（攻击方当前内功攻击/8000）^0.25+（攻击方致命量加成（装备、技能、物品等影响）-防御方暴击抵消） / 10000
		// （攻击方致命量加成-防御方暴击抵消）取值范围为（0，10000）
		FLOAT fRealCritAmount = (FLOAT)m_pOwner->GetAttValue(ERA_Crit_Amount) - (FLOAT)pTarget->GetAttValue(ERA_Toughness_strength);
		if (fRealCritAmount < 0)
			fRealCritAmount = 0;
		else if (fRealCritAmount > 10000.0f)
			fRealCritAmount = 10000.0f;
		FLOAT fRet = pow(FLOAT(m_pOwner->GetAttValue(ERA_InAttack) / 8000.0f), 0.25f) + fRealCritAmount / 10000.0f;
		return fRet;
	}

	// 绝技攻击
	else if( pSkill->IsStuntSkill() )
	{
		// 致命伤害量加成=（（攻击方当前内功攻击+攻击方当前外功攻击）/1000）^0.25+攻击方致命量加成（装备、技能、物品等影响）
		return pow(FLOAT((m_pOwner->GetAttValue(ERA_InAttack) + m_pOwner->GetAttValue(ERA_ExAttack)) / 1000.0f), 0.25f) + (FLOAT)m_pOwner->GetAttValue(ERA_Crit_Amount) / 10000.0f;
	}

	// else
	else
	{

	}

	return 0.0f;
}

//-----------------------------------------------------------------------------
// 计算技能伤害
//-----------------------------------------------------------------------------
VOID CombatHandler::CalculateDmg(Skill* pSkill, Unit* pTarget, bool bNeedLog)
{
	if(bNeedLog && pTarget->IsRole() && m_pOwner->IsRole())
		bNeedLog = true;
	else
		bNeedLog = false;

	// 目标已经死亡，直接返回
	if( pTarget->IsDead() ) return;

	DWORD dwTargetID = pTarget->GetID();

	// 暴击参数
	bool bCrit = false;
	FLOAT fCrit = 0.0f;
	bool bIgnore = false;
	FLOAT fIgnore = 0.0f;

	// 判断是否触发无视效果 [3/20/2012 zhangzhihua]
	if (m_listIgnoreTarget.IsExist(dwTargetID))
	{
		bIgnore = true;
		fIgnore = 1.35f;

		// 再判断是否暴击
		if( m_listCritedTarget.IsExist(dwTargetID) )
		{
			// 计算暴击参数
			bCrit = true;
			fCrit = CalculateCritAmount(pSkill, pTarget);
		}
	}

	// 首先判断该目标是否被闪避了
	else if( m_listDodgedTarget.IsExist(dwTargetID) )
	{
		// 发送未命中消息
		tagNS_RoleHPChange send;
		send.dwRoleID = pTarget->GetID();
		send.dwSrcRoleID = m_pOwner->GetID();
		send.eCause = ERHPCC_SkillDamage;
		send.bMiss = true;
		send.dwMisc = pSkill->GetTypeID();
		send.dwMisc2 = m_nSkillCurDmgIndex;
		send.dwSerial = m_dwSkillSerial;

		if( P_VALID(pTarget->GetMap()) )
		{
			pTarget->GetMap()->SendBigVisTileMsg(pTarget, &send, send.dwSize);
		}
		return;
	}

	// 再判断是否被格挡了
	else if( m_listBlockedTarget.IsExist(dwTargetID) )
	{
		// 发送格挡消息
		tagNS_RoleHPChange send;
		send.dwRoleID = pTarget->GetID();
		send.dwSrcRoleID = m_pOwner->GetID();
		send.eCause = ERHPCC_SkillDamage;
		send.bBlocked = true;
		send.dwMisc = pSkill->GetTypeID();
		send.dwMisc2 = m_nSkillCurDmgIndex;
		send.dwSerial = m_dwSkillSerial;

		if( P_VALID(pTarget->GetMap()) )
		{
			pTarget->GetMap()->SendBigVisTileMsg(pTarget, &send, send.dwSize);
		}
		return;
	}

	// 再判断是否暴击
	else if( m_listCritedTarget.IsExist(dwTargetID) )
	{
		// 计算暴击参数
		bCrit = true;
		fCrit = CalculateCritAmount(pSkill, pTarget);
	}

	FLOAT fBaseDmg		=	CalBaseDmg(pSkill,pTarget);						// 基础伤害

	if(bNeedLog)
		ILOG->Write(_T("PK Log / Skill ID = %d / 基础伤害 = %f\n"), pSkill->GetID(), fBaseDmg);

	FLOAT fBaseDmg0		= fBaseDmg;
	FLOAT fAttDefCoef = 1.0f;
	if( pTarget->IsCreature() )
		fAttDefCoef	=	CalCreatureAttackDefenceCoef(pSkill, pTarget);
	else
		fAttDefCoef	=	CalAttackDefenceCoef(pSkill, pTarget);	// 攻防影响
	//FLOAT fMoraleCoef	=	CalMoraleCoef(pTarget);					// 士气影响
	FLOAT fDerateCoef	=	CalDerateCoef(pSkill, pTarget);			// 减免影响
	FLOAT fInjuryCoef	=	CalInjuryCoef();						// 内伤影响
	FLOAT fLevelCoef	=	CalLevelCoef(pSkill, pTarget);			// 等级影响

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 攻防系数 = %f\n"), pSkill->GetID(), fAttDefCoef);
		ILOG->Write(_T("PK Log / Skill ID = %d / 减免系数 = %f\n"), pSkill->GetID(), fDerateCoef);
		ILOG->Write(_T("PK Log / Skill ID = %d / 内伤系数 = %f\n"), pSkill->GetID(), fInjuryCoef);
		ILOG->Write(_T("PK Log / Skill ID = %d / 等级系数 = %f\n"), pSkill->GetID(), fLevelCoef);
	}

	// 最终伤害
	FLOAT fDmg = fBaseDmg * fAttDefCoef /** fMoraleCoef*/ * fDerateCoef * fInjuryCoef * fLevelCoef;

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 最终伤害 = %f\n"), pSkill->GetID(), fDmg);
	}

 	FLOAT fActionTime(0.0);
 	fActionTime = pSkill->GetActionTime() + pSkill->GetPrepareTime() / 1000;
	if( fActionTime > 4 )
		fActionTime = 4;
	INT nDmgTimes = pSkill->GetDmgTimes();
	if( nDmgTimes <= 0 )
		nDmgTimes = 1;

	FLOAT fToughness	=	1;										// 韧性影响
	if( P_VALID(m_pOwner) )
	{
		if( m_pOwner->IsRole() )
		{
			FLOAT fTargetToughness = (FLOAT)pTarget->GetAttValue(ERA_Toughness);
			fToughness = 1- fTargetToughness / 10000 ;//* (0.65 + fBaseDmg0*(FLOAT)nDmgTimes/20000 );
		}
	}
	fDmg *= fToughness;

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 韧性系数 = %f\n"), pSkill->GetID(), fToughness);
	}

 	if ( 0.0 == fActionTime )
	{
		fDmg = fDmg + (FLOAT)m_pOwner->GetAttValue(ERA_ExDamage) - (FLOAT)pTarget->GetAttValue(ERA_ExDamage_Absorb);	
	}
 	else
	{
 		fDmg = fDmg + ((FLOAT)m_pOwner->GetAttValue(ERA_ExDamage) - (FLOAT)pTarget->GetAttValue(ERA_ExDamage_Absorb))*(fActionTime)/(nDmgTimes/*m_nSkillCurDmgIndex+1*/);
	}
	
	// 计算暴击参数	
	INT nDmg = INT(fDmg * (1.0f + fCrit + fIgnore));

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 暴击参数 = %f\n"), pSkill->GetID(), fCrit);
	}

	// 击飞 = 可击飞 + 暴击+20%
	// 击飞的距离先确定为15格子
	// 玩家不可击飞,怪物均可击飞
	if ( pTarget->IsCreature() && bCrit && fDmg >= pTarget->GetAttValue(ERA_HP)*0.2)
	{
		// 是否可被击飞
		Creature *pCreature = static_cast<Creature*>(pTarget);
		if ( 1 == pCreature->GetProto()->bCanHitFly )
			m_pOwner->HitFly(pTarget);
	}
	if( pTarget->IsCreature() )
	{
		Creature *pCreature = static_cast<Creature*>(pTarget);
		// Jason 2010-11-22 由于加上怪物仇恨目标不可到达判断，会导致怪物不攻击“非可到达”目标，但是该目标可以攻击怪
		// 在于策划等协商后，先播放怪被攻击效果，然后在判断怪物目标全不合法的情况下，给怪物回满血
		// Jason 2010-11-26 提到减血前，因为减血会清仇恨
		AIController* pController = pCreature->GetAI();
		if( P_VALID(pController) )
		{
			pController->CalMaxEnmity();
			if( pController->GetMaxEnmityUnitID() == GT_INVALID )
			{
				if(pTarget->GetCampType() == ECamp_Null)	//阵营怪物不回血
					pTarget->SetAttValue(ERA_HP, pTarget->GetAttValue(ERA_MaxHP));
				tagNS_CreatureCanntBeAck send;
				send.dwErrorCode = E_UseSkill_CreatureCannotAck_FullBlood;
				if( P_VALID(m_pOwner) )
				{
					if( m_pOwner->IsRole() )
					{
						Role * pMe = (Role*)m_pOwner;
						pMe->SendMessage(&send,send.dwSize);
					}
				}
				return ;
			}
		}
	}
	//伤害上限限制，要放在最后
	if (P_VALID(pSkill->GetProto()))
	{
		INT nCurDmg = pSkill->GetProto()->nChannelDmg[m_nSkillCurDmgIndex];
		nCurDmg = (nCurDmg>100000) ? (nCurDmg-100000) : nCurDmg;
		INT nSumDmg = 0;
		for (INT nIdx = 0; nIdx < MAX_CHANNEL_TIMES; nIdx++)
		{
			if (pSkill->GetProto()->nChannelDmg[nIdx] == 0)
				break;
			INT nChannelDmg = (pSkill->GetProto()->nChannelDmg[nIdx] > 100000) ?
				(pSkill->GetProto()->nChannelDmg[nIdx] - 100000) : pSkill->GetProto()->nChannelDmg[nIdx];
			nSumDmg += nChannelDmg;
		}
		INT nTopHit = pSkill->GetTopHit();
		INT nAttDmg = 0;
		if( pSkill->IsExAttackSkill() )
			nAttDmg = m_pOwner->GetAttValue(ERA_ExAttack);
		else if (pSkill->IsInAttackSkill())
			nAttDmg = m_pOwner->GetAttValue(ERA_InAttack);
		else
			nAttDmg = MAX(m_pOwner->GetAttValue(ERA_ExAttack), m_pOwner->GetAttValue(ERA_InAttack));


		FLOAT fTemp = 0.0f;
		if( (FLOAT)nAttDmg <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp = 0.0f;
		else
			fTemp = (FLOAT)nAttDmg - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		nTopHit += (INT)( ((FLOAT)(fTemp) /2.0f)*pSkill->GetDmgAddFactor());

		
		INT nCriticalValue = (INT)(((FLOAT)nTopHit)*(((FLOAT)nCurDmg)/((FLOAT)nSumDmg)));
		if (nDmg > nCriticalValue)
		{
			INT nExtraDmg = nDmg - nCriticalValue;
			if (nExtraDmg <= 20000)
				nDmg = nCriticalValue+sqrt((double)nExtraDmg)*15;
			else
				nDmg = nCriticalValue+2121+(nExtraDmg-20000)/10;
		}
	}

	// Jason 加入附加伤害计算，包括妖精元素伤害以及抗性
	//nDmg += CalcalateAdditionalDmg(pSkill,pTarget);
	FLOAT fElemDmg = 0;
	if( g_world.GetFabaoElemStuntFlag() )
		fElemDmg = CalElementInjury(pTarget) ;

	nDmg = (FLOAT)nDmg * ( 1 + fElemDmg);

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 附加伤害系数 = %f\n"), pSkill->GetID(), fElemDmg);
	}

	if( nDmg < 1 ) nDmg = 1;

	// （（攻击方元素伤害÷（1+防御方元素抗性÷3600））×（(攻击方资质+1)÷(防御方资质+1）)） [5/3/2012 zhangzhihua]
	FLOAT fAZiZhi = 50.0f, fDZiZhi = 50.0f;
	INT tem = GetUnitFabaoZiZhi(m_pOwner);
	if( tem > 0 )
	{
		fAZiZhi = tem;
	}
	tem = GetUnitFabaoZiZhi(pTarget);
	if( tem > 0 )
	{
		fDZiZhi = tem;
	}
	FLOAT fAInjury = 0, fDResis = 0;
	INT nDeltaAddr = 0;
	fAInjury = GetUnitElementInjury(m_pOwner,nDeltaAddr);
	if( nDeltaAddr >= 0 )
		fDResis = (pTarget->GetAttValue(ERA_EER_START + nDeltaAddr) + pTarget->GetAttValue(ERA_EER_ALL)/4.5);	//通用抗性除以4.5

	nDmg += (fAInjury / (1 + fDResis / 3600)) * ((fAZiZhi + 1) / (fDZiZhi + 1));

	// 减血
	pTarget->ChangeHP(-nDmg, m_pOwner, pSkill, NULL, bCrit, m_dwSkillSerial, m_nSkillCurDmgIndex,fElemDmg*100, bIgnore);

	//反弹要放到最后
	if (P_VALID(m_pOwner))
	{
		if (P_VALID(pTarget) && pTarget->IsRole() && !pTarget->IsHaveBuff(59328))	//妖精无敌buff不反弹伤害~
		{
			if( pSkill->IsExAttackSkill() )
			{
				//固定反弹数值
				INT nPhysicalReboundVal = pTarget->GetAttValue(ERA_Physical_damage_rebound_num);
				if (nPhysicalReboundVal > 0)
				{
					ReboundDamage(nPhysicalReboundVal, pTarget, FALSE);
				}

				//反弹比例
				FLOAT fPhysicalReboundRatio = pTarget->GetAttValue(ERA_Physical_damage_rebound_ratio);
				if (fPhysicalReboundRatio > 0)
				{
					INT nPhysicalReboundDmg = static_cast<INT>(static_cast<FLOAT>(nDmg)*fPhysicalReboundRatio/10000.0f);
					ReboundDamage(nPhysicalReboundDmg, pTarget, FALSE);
				}

			}
			else if( pSkill->IsInAttackSkill() )
			{
				//固定反弹数值
				INT nMagicReboundVal = pTarget->GetAttValue(ERA_Magic_damage_rebound_num);
				if (nMagicReboundVal > 0)
				{
					ReboundDamage(nMagicReboundVal, pTarget, TRUE);
				}

				//反弹比率
				FLOAT fMagicReboundRatio = pTarget->GetAttValue(ERA_Magic_damage_rebound_ratio);
				if (fMagicReboundRatio > 0)
				{
					INT nMagicReboundDmg = static_cast<INT>(static_cast<FLOAT>(nDmg)*fMagicReboundRatio/10000.0f);
					ReboundDamage(nMagicReboundDmg, pTarget, TRUE);
				}
			}
		}
		// 计算法力燃烧
		INT nMagicDmg = m_pOwner->GetAttValue(ERA_Mana_combustion);
		if (nMagicDmg > 0)
		{
			INT nRealMagicDmg = nMagicDmg;
			if (nRealMagicDmg > 0)
			{
				// 计算pTarget的法力燃烧抵抗
				INT nManaResistance = pTarget->GetAttValue(ERA_Mana_combustion_resistance);
				if (nManaResistance > 0)
					nRealMagicDmg -= nRealMagicDmg*nManaResistance/10000;
				if (nRealMagicDmg > 0)
				{
					pTarget->ChangeMP(-nRealMagicDmg);
					if(bNeedLog)
					{
						ILOG->Write(_T("PK Log / Skill ID = %d / 目标法力燃烧 = %d\n"), pSkill->GetID(), nRealMagicDmg);
					}
				}
			}
		}
	}

	if(bNeedLog)
	{
		ILOG->Write(_T("PK Log / Skill ID = %d / 最终目标减血 = %d\n"), pSkill->GetID(), nDmg);
	}
}

// 反弹伤害
VOID CombatHandler::ReboundDamage(INT nDamage, Unit * pTarget, BOOL bMagicDmg)
{
	INT nRealDmg = nDamage;
	// 先判断是否有相应的反弹伤害免疫
	if (P_VALID(m_pOwner) && m_pOwner->IsRole())
	{
		ERoleAttribute eImmuneAtt = bMagicDmg?ERA_Magic_damage_rebound_immune:ERA_Physical_damage_rebound_immune;
		INT nImmune = m_pOwner->GetAttValue(eImmuneAtt);
		nRealDmg -= nRealDmg*nImmune/10000;
		if (nRealDmg < 0)
			nRealDmg = 0;
	}
	m_pOwner->ChangeHP(-nRealDmg, pTarget);
}

static INT GetUnitFabaoZiZhi( Unit * pUnit )
{
	INT re = -1;
	if( pUnit->IsRole() )
	{
		Role * pRole = (Role*)pUnit;
		tagEquip * pEquip =pRole->GetItemMgr().GetEquipBarEquip((INT16)EEP_Face);
		tagFabao * pFabao = NULL;
		if( P_VALID(pEquip) && P_VALID(pEquip->pEquipProto) && MIsFaBao(pEquip) )
		{
			pFabao = (tagFabao*)pEquip;
		}
		if( P_VALID(pFabao) && pFabao->n16Stage >= 60 )
		{
			re = pFabao->n16NativeIntelligence;
		}
	}
	return re;
}
static INT GetUnitElementInjury(Unit * pUnit,INT & nDeltaAddr)
{
	for( INT i = ERA_EEI_START; i <= ERA_EEI_End; ++i )
	{
		INT n = pUnit->GetAttValue(i);
		if( n > 0 )
		{
			nDeltaAddr = i - ERA_EEI_START;
			return (n+pUnit->GetAttValue(ERA_EEI_ALL));	//加上全伤害
		}
	}
	nDeltaAddr = -1;
	return 0;
}

// （攻击方元素伤害*（1+攻击方资质/200）-防御方元素抗性*（1+防御方资质/50））/2000
FLOAT	CombatHandler::CalElementInjury(Unit * pTarget)
{
	FLOAT fAZiZhi = 50.0f, fDZiZhi = 50.0f;
	INT tem = GetUnitFabaoZiZhi(m_pOwner);
	if( tem > 0 )
	{
		fAZiZhi = tem;
	}
	tem = GetUnitFabaoZiZhi(pTarget);
	if( tem > 0 )
	{
		fDZiZhi = tem;
	}
	FLOAT fAInjury = 0, fDResis = 0;
	INT nDeltaAddr = 0;
	fAInjury = GetUnitElementInjury(m_pOwner,nDeltaAddr);
	if( nDeltaAddr >= 0 )
		fDResis = (pTarget->GetAttValue(ERA_EER_START + nDeltaAddr) + pTarget->GetAttValue(ERA_EER_ALL)/4.5);	//通用抗性除以4.5

	FLOAT fRe = ( (FLOAT)fAInjury*(1+fAZiZhi/200) - fDResis*(1+fDZiZhi/50) ) / 2000;
	if(fRe < 0)
		fRe = 0;
	else if(fRe > 0.5f)
		fRe = 0.5f;
	return fRe;
}

// 计算圣灵伤害
DWORD  CombatHandler::CalculateHolyDmg(Skill* pSkill,Unit* pSrc, Unit* pTarget, tagHolyMan * pSrcHoly, tagHolyMan * pTargetHoly)
{	// 目标已经死亡，直接返回
	if( pTarget->IsDead() ) return GT_INVALID;


	//方案一如果目标是角色，直接返回
	if( pTarget->IsRole() ) return GT_INVALID;


	DWORD dwTargetID = pTarget->GetID();

	// 暴击参数
	bool bCrit = false;
	FLOAT fCrit = 0.0f;

	bool bHit = CalculateHolyHit(pSrcHoly,pTargetHoly);
	// 首先判断该目标是否被闪避了
	if( !bHit )
	{
		// 发送未命中消息
		tagNS_RoleHPChange send;
		send.dwRoleID = pTarget->GetID();
		send.dwSrcRoleID = pSrc->GetID();
		send.eCause = ERHPCC_HolySoulDamage;
		send.bMiss = true;
		send.dwMisc = pSkill->GetTypeID();
		send.dwMisc2 = 0;
		send.dwSerial = 0;

		if( P_VALID(pTarget->GetMap()) )
		{
			pTarget->GetMap()->SendBigVisTileMsg(pTarget, &send, send.dwSize);
		}
		return E_Success;
	}

	// 计算攻击伤害
	float fDmg = CalculateHolyAttackDmg(pSkill,pTarget,pSrcHoly,pTargetHoly);
	INT nFinalDmg = 0;

	//计算是否致命
	bCrit = CalculateHolyCrit(pSkill,pSrcHoly,pTargetHoly);
	if(bCrit)
	{
		//致命伤害量加成
		fCrit = CalculateHolyCritAmount(pSrcHoly);
		// 致命伤害 = 最终伤害*1.5+最终伤害*致命伤害量加成*0.75
		nFinalDmg = (INT)(fDmg * (1.5 + fCrit * 0.75));
	}
	else
	{
		nFinalDmg = (INT)fDmg;
	}	

	/*// 减血方案二
	if( pTarget->IsRole() ) 
	{
		INT sshanghai = nFinalDmg * 0.1;  //测试圣灵对玩家无伤害
        pTarget->ChangeHP(-sshanghai, pSrc, pSkill, NULL, bCrit, -1, -1,true,false,true);
	}
	else
	{
	    pTarget->ChangeHP(-nFinalDmg, pSrc, pSkill, NULL, bCrit, -1, -1,true,false,true);
	}
	//原来 */
	pTarget->ChangeHP(-nFinalDmg, pSrc, pSkill, NULL, bCrit, -1, -1,true,false,true);

	return E_Success;
}

// 计算圣灵命中
bool  CombatHandler::CalculateHolyHit(tagHolyMan * pSrcHoly, tagHolyMan * pTargetHoly)
{

	// 命中率=0.9*（1-（防御方圣灵灵巧-攻击方圣灵精准属性）/100000） 取值范围30%~90%
	float fHolyHit = 0.0;
	if (pTargetHoly && pTargetHoly->dwLevelUpAtt[ESAT_NeglectToughness] >= pSrcHoly->dwLevelUpAtt[ESAT_AttackTec])
	{
		fHolyHit = 0.9 * (1 - (pTargetHoly->dwLevelUpAtt[ESAT_NeglectToughness] - pSrcHoly->dwLevelUpAtt[ESAT_AttackTec])/100000.0f);
	}
	else
	{
		fHolyHit = 0.9;	

	}
	
	// 范围：30——90%
	if( fHolyHit < 0.3f ) fHolyHit = 0.3f;
	if( fHolyHit > 0.9f ) fHolyHit = 0.9f;
	// 随机看是否能命中
	return (((IUTIL->Probability(INT(fHolyHit*100.0f)))== FALSE) ? false : true);
}

// 计算圣灵致命
bool  CombatHandler::CalculateHolyCrit(Skill* pSkill, tagHolyMan* pSrcHoly, tagHolyMan* pTargetHoly)
{
	// 基础致命率=（攻击方致命/（攻击方致命+250000））^0.75
	// 致命率=基础致命率*(1+(攻击方圣灵致命-防御方灵巧)/20000) +技能附带致命率*0.75
	float fBaseCrit = pow( pSrcHoly->dwLevelUpAtt[ESAT_Crit]/(float)(pSrcHoly->dwLevelUpAtt[ESAT_Crit] + 250000.f), 0.75f);		
	float fCrit = 0.0f;
	if (pTargetHoly)
	{
		fCrit = fBaseCrit * (1+(pSrcHoly->dwLevelUpAtt[ESAT_Crit] - pTargetHoly->dwLevelUpAtt[ESAT_NeglectToughness])/20000.0f)
			+  pSkill->GetCrit()*0.75;
	}
	else
	{
		fCrit = fBaseCrit * (1+pSrcHoly->dwLevelUpAtt[ESAT_Crit]/20000.0f) +  pSkill->GetCrit()*0.75;
	}
	 

	if (fCrit < 0.0f)
	{
		fCrit = 0.0f;
	}
	if (fCrit > 1.0f)
	{
		fCrit = 1.0f;
	}

	// 随机看是否能命中
	return (bool)IUTIL->Probability(INT(fCrit*100.0f));

}

// 计算圣灵致命伤害加成量
float  CombatHandler::CalculateHolyCritAmount(tagHolyMan* pSrcHoly)
{
	//致命伤害量加成=攻击方圣灵致命量/100000
	float fCritAmount = 0.0f;
	fCritAmount = pSrcHoly->dwLevelUpAtt[ESAT_CritRate]/100000.0f;	

	if (fCritAmount < 0.000f)
	{
		fCritAmount = 0.000f;
	}
	if (fCritAmount > 1.000f)
	{
		fCritAmount = 1.000f;
	}
	 
	return fCritAmount;
}

float CombatHandler::CalculateHolyAttackDmg(Skill* pSkill, Unit* pTarget, tagHolyMan* pSrcHoly, tagHolyMan* pTargetHoly)
{
	// 基础伤害=（圣灵伤害+技能威力）*（1+（圣灵伤害加深– 防御方圣灵防御）/100000）
	INT		nSkillDmg	=	pSkill->GetDmg(m_nSkillCurDmgIndex); // 技能威力
	if ( nSkillDmg > 10000)
	{
		nSkillDmg = nSkillDmg/10000;
	}
	float fBaseDmg = 0.0f;
	if (pTargetHoly)
	{
		DWORD dwTmpValue = 0;
		if (pSrcHoly->dwLevelUpAtt[ESAT_ExDmg] >= pTargetHoly->dwLevelUpAtt[ESAT_HolyDef])
		{
			dwTmpValue = pSrcHoly->dwLevelUpAtt[ESAT_ExDmg] - pTargetHoly->dwLevelUpAtt[ESAT_HolyDef];
		}
		fBaseDmg = ( pSrcHoly->dwLevelUpAtt[ESAT_Demage] +  nSkillDmg )
			* (1 + dwTmpValue/100000.0f);
	}
	else
	{
		fBaseDmg = ( pSrcHoly->dwLevelUpAtt[ESAT_Demage] +  nSkillDmg )
			* (1 + pSrcHoly->dwLevelUpAtt[ESAT_ExDmg]/100000.0f);	
	}

	// 默契值影响=默契值/100  取值（0.5~1）
	//float fCovalueCoef = (float)pSrcHoly->nCoValue / 100.0f;
	//// 范围：0.5~1
	//if( fCovalueCoef < 0.5f ) fCovalueCoef = 0.5f;
	//if( fCovalueCoef > 1.0f ) fCovalueCoef = 1.0f;
	//
	// 伤害减免：防御方等级/角色等级最大值*圣灵防御/(圣灵防御+100000)     
	float fDerateCoef = 0.0f;
	if (pTargetHoly)
	{
		fDerateCoef = pTarget->GetLevel()/(float)g_roleMgr.GetRoleLevelLimit() * pTargetHoly->dwLevelUpAtt[ESAT_HolyDef]/(float)(pTargetHoly->dwLevelUpAtt[ESAT_HolyDef] + 100000);			
	}	

	// 最终伤害=（基础伤害-基础伤害*伤害减免）
	float fDmg = (fBaseDmg - fBaseDmg * fDerateCoef);
	return fDmg;
}

//------------------------------------------------------------------------------
// 计算基础伤害
//------------------------------------------------------------------------------
//基础伤害=（武器伤害/技能攻击次数+技能威力）×（1+（攻击方攻击 – 防御方无视攻击）/15000）+（攻击方攻击 – 防御方无视攻击）×0.05/技能攻击次数
inline FLOAT CombatHandler::CalBaseDmg(Skill* pSkill, Unit* pTarget)
{
	FLOAT	fBaseDmg	=	1.0f;

	FLOAT	fWeaponDmg	=	FLOAT(IUTIL->RandomInRange(m_pOwner->GetAttValue(ERA_WeaponDmgMin), m_pOwner->GetAttValue(ERA_WeaponDmgMax))) / (FLOAT)pSkill->GetDmgTimes();
	FLOAT	fWeaponSoul	=	(FLOAT)m_pOwner->GetAttValue(ERA_WeaponSoul) / (FLOAT)pSkill->GetDmgTimes();
	INT		nSkillDmg	=	pSkill->GetDmg(m_nSkillCurDmgIndex);

	// 外功攻击基础伤害
	// （武器伤害/技能伤害次数+技能威力）×（1+攻击方当前外功攻击/10000）+攻击方当前外功攻击×0.02/技能攻击次数
	if( pSkill->IsExAttackSkill() )
	{
		if( nSkillDmg > 100000 )
		{
			// 取的是武器伤害的倍数
			fBaseDmg = fWeaponDmg * (FLOAT(nSkillDmg - 100000) / 10000.0f);
		}
		else
		{
			// 取的是技能伤害
			fBaseDmg = fWeaponDmg + (FLOAT)nSkillDmg;
		}

		FLOAT fTemp = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp = 0.0f;
		else
			fTemp = (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		fBaseDmg = fBaseDmg * (1.0f + (fTemp) / 15000.0f);
		fBaseDmg = fBaseDmg + (fTemp) * 0.05f / (FLOAT)pSkill->GetDmgTimes();
	}

	// 内功攻击基础伤害
	// （武魂/技能攻击次数+技能威力）×（1+攻击方当前内功攻击/10000）+攻击方当前内功攻击×0.02/技能攻击次数
	else if( pSkill->IsInAttackSkill() )
	{
		if( nSkillDmg > 100000 )
		{
			// 取的是武器伤害的倍数
			fBaseDmg = fWeaponSoul * ((FLOAT)(nSkillDmg - 100000) / 10000.0f);
		}

		else
		{
			// 取的是技能伤害
			fBaseDmg = fWeaponSoul + (FLOAT)nSkillDmg;
		}

		FLOAT fTemp = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp = 0.0f;
		else
			fTemp = (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		fBaseDmg = fBaseDmg * (1.0f + (fTemp) / 15000.0f);
		fBaseDmg = fBaseDmg + (fTemp) * 0.05f / (FLOAT)pSkill->GetDmgTimes();

	}

	// 绝技攻击基础伤害=（武器伤害【武器伤害平均值与法器伤害值比较最大值为提取值】/技能攻击次数+技能威力）×（1+攻击方当前攻击【取物理和法术伤害较高值】/15000）+攻击方当前攻击【取物理和法术伤害较高值】×0.05/技能攻击次数
	else if( pSkill->IsStuntSkill() )
	{
		FLOAT fStuntWeaponDmg = MAX(fWeaponDmg, fWeaponSoul);
		if( nSkillDmg > 100000 )
		{
			// 取的是武器伤害的倍数
			fBaseDmg = fStuntWeaponDmg * (FLOAT(nSkillDmg - 100000) / 10000.0f);
		}
		else
		{
			// 取的是技能伤害
			fBaseDmg = fStuntWeaponDmg + (FLOAT)nSkillDmg;
		}

		FLOAT fAttDmg = MAX((FLOAT)m_pOwner->GetAttValue(ERA_InAttack), (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack));

		FLOAT fTemp = 0.0f;
		if( (FLOAT)fAttDmg <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp = 0.0f;
		else
			fTemp = (FLOAT)fAttDmg - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		fBaseDmg = fBaseDmg * (1.0f + (fTemp) / 15000.0f);
		fBaseDmg = fBaseDmg + (fTemp) * 0.05f / (FLOAT)pSkill->GetDmgTimes();
	}

	return fBaseDmg;
}
// 怪攻防比公式，当目标是怪的时候这样做
FLOAT	CombatHandler::CalCreatureAttackDefenceCoef(Skill* pSkill, Unit* pTarget)
{
	// 外功攻击攻防影响
	// [1+（攻击方外功攻击-防御方外功防御）/（1000+攻方等级*25）]×[1+（攻击方攻击技巧-防御方防御技巧）/（1000+攻方等级*25）]
	// 乘法的两个因子分别在0.5——2.0之间
	if( pSkill->IsExAttackSkill() )
	{
		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_ExDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_ExDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);


		FLOAT fCoef1 = (FLOAT(fTemp1-fTemp2)) / ( FLOAT(m_pOwner->GetLevel())*70.0f);
		//FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / (1000.0f + FLOAT(m_pOwner->GetLevel())*25.0f);
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
		//if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
		//if( fCoef2 > 2.0f ) fCoef2 = 2.0f;

		return fCoef1 ;//* fCoef2;
	}

	// 内功攻击攻防影响
	// [1+（攻击方内功攻击-防御方内功防御）/（880+攻方等级*22）]×[1+（攻击方攻击技巧-防御方防御技巧）/（1400+攻方等级*35）]
	// 乘法的两个因子分别在0.5——2.0之间
	else if( pSkill->IsInAttackSkill() )
	{
		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_InDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_InDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);

		FLOAT fCoef1 = (FLOAT(fTemp1-fTemp2)) / ( FLOAT(m_pOwner->GetLevel())*70.0f);
		//FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / (1400.0f + FLOAT(m_pOwner->GetLevel())*35.0f);
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
		//if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
		//if( fCoef2 > 2.0f ) fCoef2 = 2.0f;

		return fCoef1 ;//* fCoef2;
	}

	// 绝技攻击攻防影响
	// 外功攻防影响+内功攻防影响
	else if( pSkill->IsStuntSkill() )
	{
		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_ExDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_ExDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);

		FLOAT fCoef1 = 1.0f + (FLOAT(fTemp1-fTemp2)) / 1000.0f;





		FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / 1000.0f;


		fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_InDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_InDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);

		FLOAT fCoef3 = 1.0f + (FLOAT(fTemp1-fTemp2)) / 800.0f;


		FLOAT fCoef4 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / 2000.0f;
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
		if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
		if( fCoef2 > 2.0f ) fCoef2 = 2.0f;
		if( fCoef3 < 0.5f ) fCoef3 = 0.5f;
		if( fCoef3 > 2.0f ) fCoef3 = 2.0f;
		if( fCoef4 < 0.5f ) fCoef4 = 0.5f;
		if( fCoef4 > 2.0f ) fCoef4 = 2.0f;

		return fCoef1 * fCoef2 * fCoef3 * fCoef4;
	}
	return 0.0f;
}
//-----------------------------------------------------------------------------
// 计算攻防影响
//-----------------------------------------------------------------------------
inline FLOAT CombatHandler::CalAttackDefenceCoef(Skill* pSkill, Unit* pTarget)
{
	// 外功攻击攻防影响
	// [1+（攻击方外功攻击-防御方外功防御）/（1000+攻方等级*25）]×[1+（攻击方攻击技巧-防御方防御技巧）/（1000+攻方等级*25）]
	// 乘法的两个因子分别在0.5——2.0之间
	if( pSkill->IsExAttackSkill() )
	{
		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_ExAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_ExDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_ExDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);

		FLOAT fCoef1 = (FLOAT(fTemp1-fTemp2)) / ( FLOAT(m_pOwner->GetLevel())*70.0f);
		//FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / (1000.0f + FLOAT(m_pOwner->GetLevel())*25.0f);
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
		//if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
		//if( fCoef2 > 2.0f ) fCoef2 = 2.0f;

		return fCoef1 ;//* fCoef2;
	}

	// 内功攻击攻防影响
	// [1+（攻击方内功攻击-防御方内功防御）/（880+攻方等级*22）]×[1+（攻击方攻击技巧-防御方防御技巧）/（1400+攻方等级*35）]
	// 乘法的两个因子分别在0.5——2.0之间
	else if( pSkill->IsInAttackSkill() )
	{
		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)m_pOwner->GetAttValue(ERA_InAttack) - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)pTarget->GetAttValue(ERA_InDefense) <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)pTarget->GetAttValue(ERA_InDefense) - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);

		FLOAT fCoef1 = (FLOAT(fTemp1-fTemp2)) / ( FLOAT(m_pOwner->GetLevel())*70.0f);
		//FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / (1400.0f + FLOAT(m_pOwner->GetLevel())*35.0f);
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
		//if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
		//if( fCoef2 > 2.0f ) fCoef2 = 2.0f;

		return fCoef1 ;//* fCoef2;
	}

	// 绝技攻击攻防影响
	// 	攻防影响= [（攻击方攻击【取物理和法术伤害较高值】-防御方防御【取物理和法术防御较低值】）/（攻方等级*70）]
	// 	其中，	50%<= [（攻击方攻击【取物理和法术伤害较高值】-防御方防御【取物理和法术防御较低值】）/（攻方等级*70）]<=200%
	else if( pSkill->IsStuntSkill() )
	{
		FLOAT fAttackDmg = (FLOAT)MAX(m_pOwner->GetAttValue(ERA_InAttack), m_pOwner->GetAttValue(ERA_ExAttack));
		FLOAT fDefenseDmg = (FLOAT)MIN(pTarget->GetAttValue(ERA_InDefense), m_pOwner->GetAttValue(ERA_ExDefense));

		FLOAT fTemp1 = 0.0f;
		if( (FLOAT)fAttackDmg <= (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack) )
			fTemp1 = 0.0f;
		else
			fTemp1 = (FLOAT)fAttackDmg - (FLOAT)pTarget->GetAttValue(ERA_Nosee_attack);

		FLOAT fTemp2 = 0.0f;
		if( (FLOAT)fDefenseDmg <= (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend) )
			fTemp2 = 0.0f;
		else
			fTemp2 = (FLOAT)fDefenseDmg - (FLOAT)m_pOwner->GetAttValue(ERA_Nosee_defend);


		FLOAT fCoef1 = (FLOAT(fTemp1-fTemp2)) / ( FLOAT(m_pOwner->GetLevel())*70.0f);
// 		FLOAT fCoef2 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / 1000.0f;
// 		FLOAT fCoef3 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_InAttack) - pTarget->GetAttValue(ERA_InDefense)) / 800.0f;
// 		FLOAT fCoef4 = 1.0f + FLOAT(m_pOwner->GetAttValue(ERA_AttackTec) - pTarget->GetAttValue(ERA_DefenseTec)) / 2000.0f;
		if( fCoef1 < 0.5f ) fCoef1 = 0.5f;
		if( fCoef1 > 2.0f ) fCoef1 = 2.0f;
// 		if( fCoef2 < 0.5f ) fCoef2 = 0.5f;
// 		if( fCoef2 > 2.0f ) fCoef2 = 2.0f;
// 		if( fCoef3 < 0.5f ) fCoef3 = 0.5f;
// 		if( fCoef3 > 2.0f ) fCoef3 = 2.0f;
// 		if( fCoef4 < 0.5f ) fCoef4 = 0.5f;
// 		if( fCoef4 > 2.0f ) fCoef4 = 2.0f;

		return fCoef1;/* * fCoef2 * fCoef3 * fCoef4*/
	}
	else
	{
		return 0.0f;
	}
}

//-----------------------------------------------------------------------
// 计算士气影响
//-----------------------------------------------------------------------
inline FLOAT CombatHandler::CalMoraleCoef(Unit* pTarget)
{
	// 士气影响=[1+（攻击方士气-防御方士气）/100]
	return FLOAT(m_pOwner->GetAttValue(ERA_Morale) - pTarget->GetAttValue(ERA_Morale)) / 100.0f;
}

//------------------------------------------------------------------------
// 计算减免影响
// F-Project 2010-6-9
//------------------------------------------------------------------------
inline FLOAT CombatHandler::CalDerateCoef(Skill* pSkill, Unit* pTarget)
{
	FLOAT fDerateCoef = 1.0f;

	ERoleAttribute eAtt = pTarget->SkillDmgType2ERA(pSkill->GetDmgType());
	if( ERA_Null == eAtt ) return fDerateCoef;

	// 外功攻击减免
	// 1 - 防御方伤害减免 - 防御方护甲减免
	if( pSkill->IsExAttackSkill() )
	{
		// 先计算护甲
		FLOAT fArmor = 1.0f;
		//if( ERA_Derate_Ordinary == eAtt )
		//{
			// 普通伤害护甲减免 = 护甲值/（31×防御方等级+267）
			fArmor = GetTargetArmor(*pTarget) / (31.0f * (FLOAT)pTarget->GetLevel() + 267.0f);
		//}
		//else
		//{
		//	// 外功伤害护甲减免 = 护甲值/（46.5×防御方等级+400）
		//	fArmor = GetTargetArmor(*pTarget) / (46.5f * (FLOAT)pTarget->GetLevel() + 400.0f);
		//}

		// 外功伤害减免=（##伤害减免 + 外功伤害减免 + 全部伤害减免– 人物等级）/ 1000
		fDerateCoef = fArmor + FLOAT(pTarget->GetAttValue(eAtt) + pTarget->GetAttValue(ERA_Derate_ExAttack) + pTarget->GetAttValue(ERA_Derate_ALL) - 
			m_pOwner->GetAttValue(m_pOwner->SkillDeepenDmgType2ERA(pSkill->GetDmgType())) - m_pOwner->GetAttValue(ERA_transform_ExAttack) - m_pOwner->GetAttValue(ERA_transform_ALL) -
			pTarget->GetLevel()) / 1000.0f;
	}

	// 内功攻击减免
	// 1 - 防御方伤害减免 - 防御方护甲减免
	else if( pSkill->IsInAttackSkill() )
	{
		// 内功伤害护甲减免 = 护甲值/（93×防御方等级+800）
		//FLOAT fArmor = GetTargetArmor(*pTarget) / (93.0f * (FLOAT)pTarget->GetLevel() + 800.0f);
		FLOAT fArmor = GetTargetArmor(*pTarget) / (31.0f * (FLOAT)pTarget->GetLevel() + 267.0f);
		// 内功伤害减免=（##伤害减免 + 内功伤害减免 + 全部伤害减免 – 人物等级）/ 1000
		fDerateCoef = fArmor + FLOAT(pTarget->GetAttValue(eAtt) + pTarget->GetAttValue(ERA_Derate_InAttack) + pTarget->GetAttValue(ERA_Derate_ALL) - 
			m_pOwner->GetAttValue(m_pOwner->SkillDeepenDmgType2ERA(pSkill->GetDmgType())) - m_pOwner->GetAttValue(ERA_transform_InAttack) - m_pOwner->GetAttValue(ERA_transform_ALL) -
			pTarget->GetLevel()) / 1000.0f;
	}

	// 绝技攻击减免
	// 减免影响=1-防御方伤害减免
	else if( pSkill->IsStuntSkill() )
	{

		// 绝技伤害减免 = （绝技伤害减免 + 全部伤害减免 – 人物等级）/ 1000
		fDerateCoef = (FLOAT)(pTarget->GetAttValue(eAtt) + pTarget->GetAttValue(ERA_Derate_ALL) -
			m_pOwner->GetAttValue(ERA_transform_ALL) - pTarget->GetLevel()) / 1000.0f;
	}

	// 计算最终值
	fDerateCoef = 1.0f - fDerateCoef;
	if( fDerateCoef < 0.2f ) fDerateCoef = 0.2f;
	if( fDerateCoef > 2.0f ) fDerateCoef = 2.0f;

	return fDerateCoef;
}

//------------------------------------------------------------------------
// 计算内伤影响
//------------------------------------------------------------------------
inline FLOAT CombatHandler::CalInjuryCoef()
{
	// 内伤影响=1－内伤×0.1÷（4＋内伤×0.06）
	return (1.0f -(FLOAT)m_pOwner->GetAttValue(ERA_Injury) * 0.1f / (4.0f + (FLOAT)m_pOwner->GetAttValue(ERA_Injury) * 0.06f));
}

//-------------------------------------------------------------------------
// 计算等级影响
//-------------------------------------------------------------------------
inline FLOAT CombatHandler::CalLevelCoef(Skill* pSkill, Unit* pTarget)
{
	// 外功攻击和内功攻击
	// 1-（防御方等级-攻击方等级）/75     取值（0.2~1.8）
	if( pSkill->IsExAttackSkill() || pSkill->IsInAttackSkill() )
	{
		FLOAT fCoef = 1.0f - FLOAT(pTarget->GetLevel() - m_pOwner->GetLevel()) / 75.0f;
		if( fCoef < 0.2f ) fCoef = 0.2f;
		if( fCoef > 1.8f ) fCoef = 1.8f;

		return fCoef;
	}
	else
	{
		return 1.0f;
	}
}

//----------------------------------------------------------------------------
// 计算技能消耗
//----------------------------------------------------------------------------
VOID CombatHandler::CalculateCost(Skill* pSkill)
{
	INT nDmgTimes = pSkill->GetDmgTimes();
	float pct = 1.;
	BOOL usePct = FALSE;
	if(nDmgTimes>0 && m_nSkillCurDmgIndex < pSkill->GetDmgTimes())
	{
		pct = min(float(m_nSkillCurDmgIndex) / nDmgTimes, 1);
		usePct = TRUE;
	}

	// 体力消耗
	INT nHPCost = pSkill->GetCost(ESCT_HP);
	if(usePct) nHPCost = floor(nHPCost*pct);
	if( nHPCost > 0  )
	{
		m_pOwner->ChangeHP(-nHPCost, m_pOwner);
	}

	// 真气消耗
	INT nMPCost = pSkill->GetCost(ESCT_MP);
	if(usePct) nMPCost = floor(nMPCost*pct);
	if( nMPCost > 0  )
	{
		m_pOwner->ChangeMP(-nMPCost);
	}

	// 怒气消耗
	INT nRageCost = pSkill->GetCost(ESCT_Rage);
	if(usePct) nRageCost = floor(nRageCost*pct);
	if( nRageCost > 0 )
	{
		m_pOwner->ChangeRage(-nRageCost);
	}


	// 持久消耗
	INT nEnduranceCost = pSkill->GetCost(ESCT_Endurance);
	if(usePct) nEnduranceCost = floor(nEnduranceCost*pct);
	if( nEnduranceCost > 0 )
	{
		m_pOwner->ChangeEndurance(-nEnduranceCost);
	}

	// 活力消耗
	INT nValicityCost = pSkill->GetCost(ESCT_Valicity);
	if(usePct) nValicityCost = floor(nValicityCost*pct);

	//INT nVitality = GetSpecSkillValue(ESSF_Valicity,ESSV_ALL,nValicityCost);

	if( nValicityCost > 0  )
	{
		m_pOwner->ChangeVitality(-nValicityCost);
	}

	//if( nVitality > 0  )
	//{
	//	m_pOwner->ChangeVitality(-nVitality);
	//}
}

//----------------------------------------------------------------------------
// 物品使用判断
//----------------------------------------------------------------------------
INT	CombatHandler::CanUseItem(tagItem *pItem)
{
	if( !P_VALID(pItem)  )
		return E_SystemError;

	if( CheckItemConflict(pItem) ) return E_UseItem_Operating;

	INT nRet = E_Success;

	nRet = CheckItemAbility(pItem);
	if( E_Success != nRet ) return nRet;

	nRet = CheckOwnerLimitItem();
	if( E_Success != nRet ) return nRet;

	nRet = CheckRoleProtoLimit(pItem);
	if( E_Success != nRet ) return nRet;

	nRet = CheckRoleStateLimit(pItem);
	if( E_Success != nRet ) return nRet;

	nRet = CheckRoleVocationLimit(pItem);
	if( E_Success != nRet ) return nRet;

	nRet = CheckMapLimit(pItem);
	if( E_Success != nRet ) return nRet;

	return nRet;
}


//----------------------------------------------------------------------------
// 检测物品本身
//----------------------------------------------------------------------------
INT	CombatHandler::CheckItemAbility(tagItem *pItem)
{
	if( !P_VALID(pItem) ) return E_UseItem_ItemNotExist;

	// 非装备需要检查物品是否被锁定
	if (pItem->bLock && !MIsEquipment(pItem->dwTypeID))
		return E_UseItem_ItemCanNotUse;

	// 物品是否是可使用物品
	if(MIsEquipment(pItem->dwTypeID) || pItem->pProtoType->dwBuffID0 == GT_INVALID)
		return E_UseItem_ItemCanNotUse;

	// 物品的冷却时间还没到，则不可以使用
	if(((Role*)m_pOwner)->GetItemMgr().IsItemCDTime(pItem->dwTypeID))
		return E_UseItem_CoolDowning;

	return E_Success;
}

//----------------------------------------------------------------------------
// 检测使用者本身
//----------------------------------------------------------------------------
INT CombatHandler::CheckOwnerLimitItem()
{
	// 是否处在不能使用技能的状态
	if( m_pOwner->IsInStateCantCastSkill() )
		return E_UseItem_UseLimit;

	return E_Success;
}

//----------------------------------------------------------------------------
// 检测人物属性限制
//----------------------------------------------------------------------------
INT CombatHandler::CheckRoleProtoLimit(tagItem *pItem)
{
	if( !P_VALID(pItem) ) return E_UseItem_ItemNotExist;

	// 性别限制
	if( pItem->pProtoType->eSexLimit != ESL_Null )
	{
		if( ESL_Man == pItem->pProtoType->eSexLimit )
		{
			if( 1 != m_pOwner->GetSex() )
				return E_UseItem_SexLimit;
		}
		else if( ESL_Woman == pItem->pProtoType->eSexLimit )
		{
			if( 0 != m_pOwner->GetSex() )
				return E_UseItem_SexLimit;
		}
		else
		{

		}
	}

	// 等级限制
	if(pItem->pProtoType->byMinUseLevel > m_pOwner->GetLevel() 
		|| pItem->pProtoType->byMaxUseLevel < m_pOwner->GetLevel())
		return E_UseItem_LevelLimit;

	// 职业限制

	return E_Success;
}


//----------------------------------------------------------------------------
// 检测人物状态限制
//----------------------------------------------------------------------------
INT CombatHandler::CheckRoleStateLimit(tagItem *pItem)
{
	// 特殊状态限制（死亡 ，眩晕）
	DWORD dwSelfStateFlag = m_pOwner->GetStateFlag();

	if( (dwSelfStateFlag & pItem->pProtoType->dwStateLimit) != dwSelfStateFlag )
	{
		return E_UseItem_SelfStateLimit;
	}

	Role *pRole = dynamic_cast<Role *>(m_pOwner);
	if(pRole != NULL)
	{
		DWORD dwRoleState = pRole->GetState();
		if(pRole->IsInRoleState(ERS_Wedding))
		{
			return E_UseItem_SelfStateLimit;
		}
		if( pRole->IsInState(ES_NoMovement) )
			return E_UseItem_SelfStateLimit;
	}
	// 玩家在活动中的限制

	// 副本限制
	return E_Success;
}

//----------------------------------------------------------------------------
// 检测人物职业限制
//----------------------------------------------------------------------------
INT CombatHandler::CheckRoleVocationLimit(tagItem *pItem)
{
	if(!P_VALID(pItem)) return E_UseItem_ItemNotExist;

	if(!m_pOwner->IsRole()) return E_Success;

	INT nClass = (INT)(static_cast<Role*> (m_pOwner)->GetClass());
	INT nClassEx = (INT)(static_cast<Role*> (m_pOwner)->GetClassEx());

	INT nTmpClass =  1 << ( nClass - 1 );
	INT nTmpClassEx = 0;

	if ( (INT)nClassEx != (INT)EHV_Base )
	{
		nTmpClassEx = 1 << ( nClassEx + 8 );
	}

	if ( ( nTmpClass + nTmpClassEx ) & pItem->pProtoType->dwVocationLimit )
		return E_Success;
	else
		return E_UseItem_VocationLimit;
}

//----------------------------------------------------------------------------
// 检测地图限制
//----------------------------------------------------------------------------
INT CombatHandler::CheckMapLimit(tagItem* pItem)
{
	// 判断地图限制
	if(P_VALID(m_pOwner->GetMap()))
	{
		BOOL bUesAble = m_pOwner->GetMap()->CanUseItem(pItem->dwTypeID);
		if( !bUesAble )	return E_UseItem_MapLimit;
	}
	
	return E_Success;
}

//-------------------------------------------------------------------------------
// 测试物品使用冲突，如果冲突则为TRUE，不冲突为FALSE
//-------------------------------------------------------------------------------
BOOL CombatHandler::CheckItemConflict(tagItem* pItem)
{
	if( IsUseItem() ) return TRUE;	// 当前正在使用物品，则不能使用

	if( IsUseSkill() )
	{
		// 如果物品是起手物品，则不可以使用
		if( pItem->pProtoType->nPrepareTime > 0 ) return TRUE;
		else return FALSE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------
// 获取buff影响后的目标护甲值
//-------------------------------------------------------------------------------
FLOAT CombatHandler::GetTargetArmor(Unit &target)
{
	return (FLOAT)target.GetAttValue(ERA_Armor) * m_fTargetArmorLeftPct;
}

FLOAT	CombatHandler::CalcalateAdditionalDmg(Skill * pSkill,Unit*pTarget)
{
	if( !P_VALID(pTarget) )
		return m_pOwner->CalAdditionalDmg(pSkill);
	return m_pOwner->CalAdditionalDmg(pSkill) - pTarget->CalAdditionalResistance(pSkill);
}

INT CombatHandler::CheckSoulActiveLimit(Skill* pSkill)
{
	if(!P_VALID(pSkill))
	{
		return GT_INVALID;
	}

	const tagForceSkillProto* pForceSkillProto = g_attRes.GetForceSkillProtoEx(pSkill->GetTypeID());
	if(!P_VALID(pForceSkillProto))
	{
		return E_Success;
	}

	if(!pForceSkillProto->bForerver)
	{
		if( CalcTimeDiff(pSkill->GetActiveTime(),GetCurrentDWORDTime()) <= 0 )
		{
			return E_UseSkill_Soul;
		}
	}

	// 检查是不是玩家
	if( !m_pOwner->IsRole() ) 
		return E_UseItem_TargetInvalid;

	Role* pOwnerRole = static_cast<Role*>(m_pOwner);

	DWORD dwTempID = pForceSkillProto->bySide * 10 + pForceSkillProto->dwLevel;
	const tagForceLevelProto* pForceLevelProto = g_attRes.GetForceLevelProto(dwTempID);
	if(!P_VALID(pForceLevelProto))
	{
		return E_UseSkill_Soul;
	}

	if(0 == pForceSkillProto->bySide)
	{
		if( pOwnerRole->GetGodPoint() < pForceLevelProto->dwPoint )
		{
			return E_UseSkill_Soul;
		}
	}
	else
	{
		if( pOwnerRole->GetMonsterPoint() < pForceLevelProto->dwPoint )
		{
			return E_UseSkill_Soul;
		}
	}

	return E_Success;
}
