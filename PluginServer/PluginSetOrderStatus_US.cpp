#include "stdafx.h"
#include "PluginSetOrderStatus_US.h"
#include "PluginUSTradeServer.h"
#include "Protocol/ProtoSetOrderStatus.h"
#include "IManage_SecurityNum.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define TIMER_ID_HANDLE_TIMEOUT_REQ	355
#define EVENT_ID_ACK_REQUEST		368

//tomodify 2
#define PROTO_ID_QUOTE			PROTO_ID_TDUS_SET_ORDER_STATUS
typedef CProtoSetOrderStatus	CProtoQuote;



//////////////////////////////////////////////////////////////////////////

CPluginSetOrderStatus_US::CPluginSetOrderStatus_US()
{	
	m_pTradeOp = NULL;
	m_pTradeServer = NULL;
	m_bStartTimerHandleTimeout = FALSE;
}

CPluginSetOrderStatus_US::~CPluginSetOrderStatus_US()
{
	Uninit();
}

void CPluginSetOrderStatus_US::Init(CPluginUSTradeServer* pTradeServer, ITrade_US*  pTradeOp)
{
	if ( m_pTradeServer != NULL )
		return;

	if ( pTradeServer == NULL || pTradeOp == NULL )
	{
		ASSERT(false);
		return;
	}

	m_pTradeServer = pTradeServer;
	m_pTradeOp = pTradeOp;
	m_TimerWnd.SetEventInterface(this);
	m_TimerWnd.Create();

	m_MsgHandler.SetEventInterface(this);
	m_MsgHandler.Create();

	m_stOrderIDCvt.Init(m_pTradeOp, this);
}

void CPluginSetOrderStatus_US::Uninit()
{
	if ( m_pTradeServer != NULL )
	{
		m_pTradeServer = NULL;
		m_pTradeOp = NULL;

		m_TimerWnd.Destroy();
		m_TimerWnd.SetEventInterface(NULL);

		m_MsgHandler.Close();
		m_MsgHandler.SetEventInterface(NULL);

		ClearAllReqAckData();
	}
}

void CPluginSetOrderStatus_US::SetTradeReqData(int nCmdID, const Json::Value &jsnVal, SOCKET sock)
{
	CHECK_RET(nCmdID == PROTO_ID_QUOTE && sock != INVALID_SOCKET, NORET);
	CHECK_RET(m_pTradeOp && m_pTradeServer, NORET);

	CProtoQuote proto;
	CProtoQuote::ProtoReqDataType	req;
	proto.SetProtoData_Req(&req);
	if ( !proto.ParseJson_Req(jsnVal) )
	{
		CHECK_OP(false, NORET);
		TradeAckType ack;
		ack.head = req.head;
		ack.head.ddwErrCode = PROTO_ERR_PARAM_ERR;
		CA::Unicode2UTF(L"��������", ack.head.strErrDesc);
		ack.body.nCookie = req.body.nCookie;
		ack.body.nSvrResult = Trade_SvrResult_Failed;
		HandleTradeAck(&ack, sock);
		return;
	}

	if (!IManage_SecurityNum::IsSafeSocket(sock))
	{
		CHECK_OP(false, NORET);
		TradeAckType ack;
		ack.head = req.head;
		ack.head.ddwErrCode = PROTO_ERR_UNKNOWN_ERROR;
		CA::Unicode2UTF(L"�����½�����", ack.head.strErrDesc);
		ack.body.nCookie = req.body.nCookie;
		ack.body.nSvrResult = Trade_SvrResult_Failed;
		HandleTradeAck(&ack, sock);
		return;
	}

	CHECK_RET(req.head.nProtoID == nCmdID && req.body.nCookie, NORET);
	SetOrderStatusReqBody &body = req.body;		

	StockDataReq *pReq = new StockDataReq;
	CHECK_RET(pReq, NORET);
	pReq->sock = sock;
	pReq->dwReqTick = ::GetTickCount();
	pReq->req = req;
	pReq->bWaitDelaySvrID = true; 

	DoTryProcessTradeOpt(pReq); 
}


void  CPluginSetOrderStatus_US::DoTryProcessTradeOpt(StockDataReq* pReq)
{
	CHECK_RET(m_pTradeOp && pReq, NORET); 

	bool bIsNewReq = !IsReqDataExist(pReq); 

	SetOrderStatusReqBody &body = pReq->req.body;
	TradeReqType& req = pReq->req; 
	SOCKET sock = pReq->sock; 

	//������µĵ��ã� ͨ������id���Ҷ���SvrID 
	if (bIsNewReq)
	{
		m_vtReqData.push_back(pReq); 

		if (0 == body.nSvrOrderID && 0 != body.nLocalOrderID) 
		{ 
			body.nSvrOrderID = m_stOrderIDCvt.FindSvrOrderID((Trade_Env)body.nEnvType, 
				body.nLocalOrderID, true);	

			// �ȴ�svrid ȡ������ʵ�ʵ��ýӿ�
			if (0 == body.nSvrOrderID)
			{ 
				return; 
			}
		} 
	} 
	// 
	bool bRet = false; 
	//����ֻ֧�ֳ����� ֻ����ʵ���׻���!!
	if (body.nSvrOrderID != 0 && body.nSetOrderStatus == 0 && body.nEnvType == Trade_Env_Real)
	{  
		pReq->bWaitDelaySvrID = false;
		bRet = m_pTradeOp->CancelOrder((UINT*)&pReq->dwLocalCookie, body.nSvrOrderID);
	} 

	if ( !bRet )
	{
		TradeAckType ack;
		ack.head = req.head;
		ack.head.ddwErrCode = PROTO_ERR_UNKNOWN_ERROR;
		CA::Unicode2UTF(L"����ʧ��", ack.head.strErrDesc);

		ack.body.nEnvType = body.nEnvType;
		ack.body.nCookie = body.nCookie;
		ack.body.nLocalOrderID = body.nLocalOrderID;
		ack.body.nSvrOrderID = body.nSvrOrderID;

		ack.body.nSvrResult = Trade_SvrResult_Failed;
		HandleTradeAck(&ack, sock);

		//���req���� 
		DoRemoveReqData(pReq);
		return ;
	}

	SetTimerHandleTimeout(true);
} 

void CPluginSetOrderStatus_US::NotifyOnSetOrderStatus(Trade_Env enEnv, UINT nCookie, Trade_SvrResult enSvrRet, UINT64 nOrderID, INT64 nErrCode)
{
	CHECK_RET(nCookie, NORET);
	CHECK_RET(m_pTradeOp && m_pTradeServer, NORET);

	VT_REQ_TRADE_DATA::iterator itReq = m_vtReqData.begin();
	StockDataReq *pFindReq = NULL;
	for ( ; itReq != m_vtReqData.end(); ++itReq )
	{
		StockDataReq *pReq = *itReq;
		CHECK_OP(pReq, continue);
		if ( pReq->dwLocalCookie == nCookie )
		{
			pFindReq = pReq;
			break;
		}
	}
	if (!pFindReq)
		return;

	CHECK_RET(pFindReq, NORET);

	TradeAckType ack;
	ack.head = pFindReq->req.head;
	ack.head.ddwErrCode = nErrCode;
	if ( nErrCode )
	{
		WCHAR szErr[256] = L"";
		if ( m_pTradeOp->GetErrDescV2(nErrCode, szErr) )
			CA::Unicode2UTF(szErr, ack.head.strErrDesc);
	}

	//tomodify 4
	ack.body.nEnvType = enEnv;
	ack.body.nCookie = pFindReq->req.body.nCookie;
	ack.body.nSvrOrderID = pFindReq->req.body.nSvrOrderID;
	ack.body.nLocalOrderID = pFindReq->req.body.nLocalOrderID; 

	ack.body.nSvrResult = enSvrRet;
	HandleTradeAck(&ack, pFindReq->sock);

	m_vtReqData.erase(itReq);
	delete pFindReq;
}

void CPluginSetOrderStatus_US::OnTimeEvent(UINT nEventID)
{
	if ( TIMER_ID_HANDLE_TIMEOUT_REQ == nEventID )
	{
		HandleTimeoutReq();
	}
}

void CPluginSetOrderStatus_US::OnMsgEvent(int nEvent,WPARAM wParam,LPARAM lParam)
{
	if ( EVENT_ID_ACK_REQUEST == nEvent )
	{		
	}	
}

void CPluginSetOrderStatus_US::HandleTimeoutReq()
{
	if ( m_vtReqData.empty() )
	{
		SetTimerHandleTimeout(false);
		return;
	}

	DWORD dwTickNow = ::GetTickCount();	
	VT_REQ_TRADE_DATA::iterator it_req = m_vtReqData.begin();
	for ( ; it_req != m_vtReqData.end(); )
	{
		StockDataReq *pReq = *it_req;	
		if ( pReq == NULL )
		{
			CHECK_OP(false, NOOP);
			++it_req;
			continue;
		}		

		if ( int(dwTickNow - pReq->dwReqTick) > 8000 )
		{
			TradeAckType ack;
			ack.head = pReq->req.head;
			ack.head.ddwErrCode= PROTO_ERR_SERVER_TIMEROUT;
			CA::Unicode2UTF(L"Э�鳬ʱ", ack.head.strErrDesc);

			//tomodify 5
			ack.body.nEnvType = pReq->req.body.nEnvType;
			ack.body.nCookie = pReq->req.body.nCookie;
			ack.body.nSvrOrderID = pReq->req.body.nSvrOrderID;
			ack.body.nLocalOrderID = pReq->req.body.nLocalOrderID;

			ack.body.nSvrResult = Trade_SvrResult_Failed;
			HandleTradeAck(&ack, pReq->sock);

			it_req = m_vtReqData.erase(it_req);
			delete pReq;
		}
		else
		{
			++it_req;
		}
	}

	if ( m_vtReqData.empty() )
	{
		SetTimerHandleTimeout(false);
		return;
	}
}

void CPluginSetOrderStatus_US::HandleTradeAck(TradeAckType *pAck, SOCKET sock)
{
	CHECK_RET(pAck && pAck->body.nCookie && sock != INVALID_SOCKET, NORET);
	CHECK_RET(m_pTradeServer, NORET);

	CProtoQuote proto;
	proto.SetProtoData_Ack(pAck);

	Json::Value jsnValue;
	bool bRet = proto.MakeJson_Ack(jsnValue);
	CHECK_RET(bRet, NORET);

	std::string strBuf;
	CProtoParseBase::ConvJson2String(jsnValue, strBuf, true);
	m_pTradeServer->ReplyTradeReq(PROTO_ID_QUOTE, strBuf.c_str(), (int)strBuf.size(), sock);
}

void CPluginSetOrderStatus_US::SetTimerHandleTimeout(bool bStartOrStop)
{
	if ( m_bStartTimerHandleTimeout )
	{
		if ( !bStartOrStop )
		{			
			m_TimerWnd.StopTimer(TIMER_ID_HANDLE_TIMEOUT_REQ);
			m_bStartTimerHandleTimeout = FALSE;
		}
	}
	else
	{
		if ( bStartOrStop )
		{
			m_TimerWnd.StartMillionTimer(500, TIMER_ID_HANDLE_TIMEOUT_REQ);
			m_bStartTimerHandleTimeout = TRUE;
		}
	}
}

void CPluginSetOrderStatus_US::ClearAllReqAckData()
{
	VT_REQ_TRADE_DATA::iterator it_req = m_vtReqData.begin();
	for ( ; it_req != m_vtReqData.end(); )
	{
		StockDataReq *pReq = *it_req;
		delete pReq;
	}

	m_vtReqData.clear();
}


bool CPluginSetOrderStatus_US::IsReqDataExist( StockDataReq* pReq )
{
	VT_REQ_TRADE_DATA::iterator it = m_vtReqData.begin();
	while (it != m_vtReqData.end())
	{
		StockDataReq* pItem = *it;
		if (pItem && pItem == pReq) 
		{ 
			return true; 
		}
		++it; 
	}
	return false; 
}

void CPluginSetOrderStatus_US::DoRemoveReqData(StockDataReq* pReq)
{
	VT_REQ_TRADE_DATA::iterator it = m_vtReqData.begin();
	while (it != m_vtReqData.end())
	{
		StockDataReq* pItem = *it;
		if (pItem && pItem == pReq) 
		{ 
			it = m_vtReqData.erase(it);
			SAFE_DELETE(pItem); 
			return; 
		}
		++it; 
	}
}

void CPluginSetOrderStatus_US::OnCvtOrderID_Local2Svr( int nResult, Trade_Env eEnv, 
													  INT64 nLocalID, INT64 nServerID )
{
	VT_REQ_TRADE_DATA vtTmp = m_vtReqData;

	VT_REQ_TRADE_DATA::iterator it = vtTmp.begin();
	while (it != vtTmp.end())
	{
		StockDataReq* pItem = *it; 
		if (!pItem)
			continue;

		if (pItem->req.body.nEnvType == eEnv && pItem->req.body.nLocalOrderID == nLocalID)
		{ 
			if (0 == pItem->req.body.nSvrOrderID)
				pItem->req.body.nSvrOrderID = nServerID;  
			else 
				CHECK_OP(false, NOOP); 

			DoTryProcessTradeOpt(pItem);
		}
		++it; 
	}
}