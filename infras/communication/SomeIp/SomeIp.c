/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2021 Parai Wang <parai@foxmail.com>
 *
 * Ref: Specification of SOME/IP Transformer AUTOSAR CP Release 4.4.0
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include "SomeIp.h"
#include "SomeIp_Priv.h"
#include "SomeIp_Cfg.h"
#include "Std_Debug.h"
#include "Std_Critical.h"
#include "Sd.h"
#include "NetMem.h"
#include <string.h>
#ifdef USE_PCAP
#include "pcap.h"
#endif
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_SOMEIP 0
#define AS_LOG_SOMEIPI 2
#define AS_LOG_SOMEIPW 3
#define AS_LOG_SOMEIPE 4

/* @SWS_SomeIpXf_00031 */
#define SOMEIP_MSG_REQUEST 0x00
#define SOMEIP_MSG_REQUEST_NO_RETURN 0x01
#define SOMEIP_MSG_NOTIFICATION 0x02
#define SOMEIP_MSG_RESPONSE 0x80
#define SOMEIP_MSG_ERROR 0x81

#define SOMEIP_MSG_REQUEST_ACK 0x40
#define SOMEIP_MSG_REQUEST_NO_RETURN_ACK 0x41
#define SOMEIP_MSG_NOTIFICATION_ACK 0x42

#define SOMEIP_MSG_RESPONSE_ACK 0xC0
#define SOMEIP_MSG_ERROR_ACK 0xC1

/* @PRS_SOMEIP_00367 */
#define SOMEIP_TP_FLAG 0x20

#define SOMEIP_SF_MAX 1396
#define SOMEIP_TP_MAX 1392

#define SOMEIP_TP_FRAME_MAX (SOMEIP_TP_MAX + 20)

#define SOMEIP_CONFIG (&SomeIp_Config)

#ifndef SOMEIP_ASYNC_REQUEST_MESSAGE_POOL_SIZE
#define SOMEIP_ASYNC_REQUEST_MESSAGE_POOL_SIZE 8
#endif

#ifndef SOMEIP_RX_TP_MESSAGE_POOL_SIZE
#define SOMEIP_RX_TP_MESSAGE_POOL_SIZE 8
#endif

#ifndef SOMEIP_TX_TP_MESSAGE_POOL_SIZE
#define SOMEIP_TX_TP_MESSAGE_POOL_SIZE 8
#endif

#ifndef SOMEIP_TX_TP_EVENT_MESSAGE_POOL_SIZE
#define SOMEIP_TX_TP_EVENT_MESSAGE_POOL_SIZE 8
#endif

#ifndef SOMEIP_TCP_BUFFER_POOL_SIZE
#define SOMEIP_TCP_BUFFER_POOL_SIZE 8
#endif

#ifndef SOMEIP_WAIT_RESPOSE_MESSAGE_POOL_SIZE
#define SOMEIP_WAIT_RESPOSE_MESSAGE_POOL_SIZE 8
#endif

#ifndef SOMEIP_TX_NOK_RETRY_MAX
#define SOMEIP_TX_NOK_RETRY_MAX 3
#endif

#ifdef USE_PCAP
#define PCAP_TRACE PCap_SomeIp
#else
#define PCAP_TRACE(serviceId, methodId, interfaceVersion, messageType, returnCode, payload,        \
                   payloadLength, clientId, sessionId, RemoteAddr, isTp, offset, more, isRx)
#endif

#define IS_TP_ENABLED(obj) (NULL != (obj)->onTpCopyTxData)

/* SQP: SOMEIP Queue and Pool */
#define DEF_SQP(T, size)                                                                           \
  static SomeIp_##T##Type someIp##T##Slots[size];                                                  \
  static mempool_t someIp##T##Pool;

#define DEC_SQP(T)                                                                                 \
  SomeIp_##T##Type *var;                                                                           \
  SomeIp_##T##Type *next

#define SQP_INIT(T)                                                                                \
  do {                                                                                             \
    mp_init(&someIp##T##Pool, (uint8_t *)&someIp##T##Slots, sizeof(SomeIp_##T##Type),              \
            ARRAY_SIZE(someIp##T##Slots));                                                         \
  } while (0)

#define SQP_FIRST(T)                                                                               \
  do {                                                                                             \
    var = STAILQ_FIRST(&context->pending##T##s);                                                   \
  } while (0)

#define SQP_NEXT()                                                                                 \
  do {                                                                                             \
    EnterCritical();                                                                               \
    next = STAILQ_NEXT(var, entry);                                                                \
    ExitCritical();                                                                                \
  } while (0)

#define SQP_WHILE(T)                                                                               \
  SQP_FIRST(T);                                                                                    \
  while (NULL != var) {                                                                            \
    SQP_NEXT();

#define SQP_WHILE_END()                                                                            \
  var = next;                                                                                      \
  }

/* CRM: context RM */
#define SQP_CRM_AND_FREE(T)                                                                        \
  do {                                                                                             \
    EnterCritical();                                                                               \
    STAILQ_REMOVE(&context->pending##T##s, var, SomeIp_##T##_s, entry);                            \
    ExitCritical();                                                                                \
    mp_free(&someIp##T##Pool, (uint8_t *)var);                                                     \
  } while (0)

/* LRM: list RM */
#define SQP_LRM_AND_FREE(T)                                                                        \
  do {                                                                                             \
    EnterCritical();                                                                               \
    STAILQ_REMOVE(pending##T##s, var, SomeIp_##T##_s, entry);                                      \
    ExitCritical();                                                                                \
    mp_free(&someIp##T##Pool, (uint8_t *)var);                                                     \
  } while (0)

/* Context Append */
#define SQP_CAPPEND(T)                                                                             \
  do {                                                                                             \
    EnterCritical();                                                                               \
    STAILQ_INSERT_TAIL(&context->pending##T##s, var, entry);                                       \
    ExitCritical();                                                                                \
  } while (0)

/* List Append */
#define SQP_LAPPEND(T)                                                                             \
  do {                                                                                             \
    EnterCritical();                                                                               \
    STAILQ_INSERT_TAIL(pending##T##s, var, entry);                                                 \
    ExitCritical();                                                                                \
  } while (0)

#define SQP_ALLOC(T)                                                                               \
  do {                                                                                             \
    var = (SomeIp_##T##Type *)mp_alloc(&someIp##T##Pool);                                          \
  } while (0)

#define SQP_FREE(T)                                                                                \
  do {                                                                                             \
    mp_free(&someIp##T##Pool, (uint8_t *)var);                                                     \
  } while (0)

#define SQP_CLEAR(T)                                                                               \
  do {                                                                                             \
    SomeIp_##T##Type *var;                                                                         \
    EnterCritical();                                                                               \
    var = STAILQ_FIRST(&context->pending##T##s);                                                   \
    while (NULL != var) {                                                                          \
      STAILQ_REMOVE_HEAD(&context->pending##T##s, entry);                                          \
      mp_free(&someIp##T##Pool, (uint8_t *)var);                                                   \
      var = STAILQ_FIRST(&context->pending##T##s);                                                 \
    }                                                                                              \
    ExitCritical();                                                                                \
  } while (0)
/* ================================ [ TYPES     ] ============================================== */
/* ================================ [ DECLARES  ] ============================================== */
extern const SomeIp_ConfigType SomeIp_Config;
/* ================================ [ DATAS     ] ============================================== */
DEF_SQP(AsyncReqMsg, SOMEIP_ASYNC_REQUEST_MESSAGE_POOL_SIZE)
DEF_SQP(RxTpMsg, SOMEIP_RX_TP_MESSAGE_POOL_SIZE)
DEF_SQP(TxTpMsg, SOMEIP_TX_TP_MESSAGE_POOL_SIZE)
#define someIpRxTpEvtMsgPool someIpRxTpMsgPool
DEF_SQP(TxTpEvtMsg, SOMEIP_TX_TP_EVENT_MESSAGE_POOL_SIZE)
DEF_SQP(WaitResMsg, SOMEIP_WAIT_RESPOSE_MESSAGE_POOL_SIZE)
/* ================================ [ LOCALS    ] ============================================== */
static Std_ReturnType SomeIp_DecodeHeader(const uint8_t *data, uint32_t length,
                                          SomeIp_HeaderType *header) {
  Std_ReturnType ret = E_OK;
  if (length >= 16) {
    if (data[12] != 1) { /* @SWS_SomeIpXf_00029 */
      ASLOG(SOMEIP, ("invlaid protocol version\n"));
      ret = SOMEIPXF_E_WRONG_PROTOCOL_VERSION;
    } else {
      header->length =
        ((uint32_t)data[4] << 24) + ((uint32_t)data[5] << 16) + ((uint32_t)data[6] << 8) + data[7];
      header->serviceId = ((uint16_t)data[0] << 8) + data[1];
      header->methodId = ((uint16_t)data[2] << 8) + data[3];
      header->clientId = ((uint16_t)data[8] << 8) + data[9];
      header->sessionId = ((uint16_t)data[10] << 8) + data[11];
      header->interfaceVersion = data[13];
      header->messageType = data[14];
      header->returnCode = data[15];
      if (header->messageType & SOMEIP_TP_FLAG) {
        header->messageType &= ~SOMEIP_TP_FLAG;
        header->isTpFlag = TRUE;
        if ((header->length > (8 + 4 + SOMEIP_TP_MAX)) || (header->length <= (8 + 4))) {
          ASLOG(SOMEIPE, ("TP length not valid\n"));
          ret = SOMEIPXF_E_MALFORMED_MESSAGE;
        }
      } else {
        header->isTpFlag = FALSE;
      }
      if (header->length < 8) {
        ret = E_NOT_OK;
        ASLOG(SOMEIPE, ("invalid length\n"));
      } else if ((header->length + 8) > length) {
        ret = SOMEIP_E_MSG_TOO_SHORT;
      } else if ((header->length + 8) < length) {
        ret = SOMEIP_E_MSG_TOO_LARGE;
      } else {
      }
    }
  } else {
    ASLOG(SOMEIPE, ("message too short\n"));
    ret = SOMEIPXF_E_MALFORMED_MESSAGE;
  }
  return ret;
}

static Std_ReturnType SomeIp_DecodeMsg(const PduInfoType *PduInfoPtr, SomeIp_MsgType *msg) {
  Std_ReturnType ret;
  uint32_t offset;

  ret = SomeIp_DecodeHeader(PduInfoPtr->SduDataPtr, PduInfoPtr->SduLength, &msg->header);
  if (E_OK == ret) {
    msg->RemoteAddr = *(const TcpIp_SockAddrType *)PduInfoPtr->MetaDataPtr;
    if (msg->header.isTpFlag) {
      offset = ((uint32_t)PduInfoPtr->SduDataPtr[16] << 24) +
               ((uint32_t)PduInfoPtr->SduDataPtr[17] << 16) +
               ((uint32_t)PduInfoPtr->SduDataPtr[18] << 8) + PduInfoPtr->SduDataPtr[19];
      msg->tpHeader.offset = offset & 0xFFFFFFF0UL;
      msg->tpHeader.moreSegmentsFlag = offset & 0x01;
      msg->req.data = &PduInfoPtr->SduDataPtr[20];
      msg->req.length = PduInfoPtr->SduLength - 20;
    } else {
      msg->req.data = &PduInfoPtr->SduDataPtr[16];
      msg->req.length = PduInfoPtr->SduLength - 16;
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_DecodeMsgTcp(uint8_t *header, uint8_t *data, uint32_t length,
                                          const TcpIp_SockAddrType *RemoteAddr,
                                          SomeIp_MsgType *msg) {
  Std_ReturnType ret;
  uint32_t offset;

  ret = SomeIp_DecodeHeader(header, length + 16, &msg->header);
  if (E_OK == ret) {
    msg->RemoteAddr = *RemoteAddr;
    if (msg->header.isTpFlag) {
      offset =
        ((uint32_t)data[0] << 24) + ((uint32_t)data[1] << 16) + ((uint32_t)data[2] << 8) + data[3];
      msg->tpHeader.offset = offset & 0xFFFFFFF0UL;
      msg->tpHeader.moreSegmentsFlag = offset & 0x01;
      msg->req.data = &data[4];
      msg->req.length = length - 4;
    } else {
      msg->req.data = data;
      msg->req.length = length;
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_HandleTcpHeader(SomeIp_TcpBufferType *tcpBuf, PduInfoType *PduInfoPtr,
                                             SomeIp_MsgType *msg) {
  Std_ReturnType ret;
  uint32_t leftLen;
  leftLen = sizeof(tcpBuf->header) - tcpBuf->lenOfHd;
  if (PduInfoPtr->SduLength < leftLen) {
    leftLen = PduInfoPtr->SduLength;
  }
  if (leftLen > 0) {
    memcpy(&tcpBuf->header[tcpBuf->lenOfHd], PduInfoPtr->SduDataPtr, leftLen);
    PduInfoPtr->SduDataPtr = &PduInfoPtr->SduDataPtr[leftLen];
    PduInfoPtr->SduLength -= leftLen;
    tcpBuf->lenOfHd += leftLen;
  }

  ASLOG(SOMEIP, ("header %d(+%d), data %d\n", tcpBuf->lenOfHd, leftLen, PduInfoPtr->SduLength));

  if (sizeof(tcpBuf->header) == tcpBuf->lenOfHd) {
    ret = SomeIp_DecodeMsgTcp(tcpBuf->header, PduInfoPtr->SduDataPtr, PduInfoPtr->SduLength,
                              (const TcpIp_SockAddrType *)PduInfoPtr->MetaDataPtr, msg);
  } else {
    ret = SOMEIP_E_PENDING;
  }

  return ret;
}

static void SomeIp_BuildHeader(uint8_t *header, uint16_t serviceId, uint16_t methodId,
                               uint16_t clientId, uint16_t sessionId, uint8_t interfaceVersion,
                               uint8_t messageType, uint8_t returnCode, uint32_t payloadLength) {
  uint32_t length = payloadLength + 8;
  header[0] = (serviceId >> 8) & 0xFF;
  header[1] = serviceId & 0xFF;
  header[2] = (methodId >> 8) & 0xFF;
  header[3] = methodId & 0xFF;
  header[4] = (length >> 24) & 0xFF;
  header[5] = (length >> 16) & 0xFF;
  header[6] = (length >> 8) & 0xFF;
  header[7] = length & 0xFF;
  header[8] = (clientId >> 8) & 0xFF;
  header[9] = clientId & 0xFF;
  header[10] = (sessionId >> 8) & 0xFF;
  header[11] = sessionId & 0xFF;
  header[12] = 1;
  header[13] = interfaceVersion;
  header[14] = messageType;
  header[15] = returnCode;

  ASLOG(SOMEIP, ("build: service 0x%x:0x%x:%d message type %d return code %d payload %d bytes from "
                 "client 0x%x:%d\n",
                 serviceId, methodId, interfaceVersion, messageType, returnCode, payloadLength,
                 clientId, sessionId));
}

static Std_ReturnType SomeIp_Transmit(PduIdType TxPduId, TcpIp_SockAddrType *RemoteAddr,
                                      uint8_t *data, uint16_t serviceId, uint16_t methodId,
                                      uint16_t clientId, uint16_t sessionId,
                                      uint8_t interfaceVersion, uint8_t messageType,
                                      uint8_t returnCode, uint32_t payloadLength) {
  Std_ReturnType ret;
  PduInfoType PduInfo;

  PduInfo.MetaDataPtr = (uint8_t *)RemoteAddr;
  PduInfo.SduDataPtr = data;
  PduInfo.SduLength = payloadLength + 16;
  SomeIp_BuildHeader(data, serviceId, methodId, clientId, sessionId, interfaceVersion, messageType,
                     returnCode, payloadLength);
  ret = SoAd_IfTransmit(TxPduId, &PduInfo);
  if (E_OK != ret) {
    ASLOG(SOMEIPE, ("Fail to send msg\n"));
  } else {
    PCAP_TRACE(serviceId, methodId, interfaceVersion, messageType, returnCode, &data[16],
               payloadLength, clientId, sessionId, RemoteAddr, FALSE, 0, FALSE, FALSE);
  }
  return ret;
}

static Std_ReturnType SomeIp_TransError(PduIdType TxPduId, TcpIp_SockAddrType *RemoteAddr,
                                        uint16_t serviceId, uint16_t methodId, uint16_t clientId,
                                        uint16_t sessionId, uint8_t interfaceVersion,
                                        uint8_t messageType, uint8_t returnCode) {
  Std_ReturnType ret;
  PduInfoType PduInfo;
  uint8_t errMsg[16];

  PduInfo.MetaDataPtr = (uint8_t *)RemoteAddr;
  PduInfo.SduDataPtr = errMsg;
  PduInfo.SduLength = 16;
  SomeIp_BuildHeader(errMsg, serviceId, methodId, clientId, sessionId, interfaceVersion,
                     messageType, returnCode, 0);
  ret = SoAd_IfTransmit(TxPduId, &PduInfo);
  if (E_OK != ret) {
    ASLOG(SOMEIPE, ("Fail to send error\n"));
  } else {
    PCAP_TRACE(serviceId, methodId, interfaceVersion, messageType, returnCode, NULL, 0, clientId,
               sessionId, RemoteAddr, FALSE, 0, FALSE, FALSE);
  }
  return ret;
}

static Std_ReturnType SomeIp_ReplyError(PduIdType TxPduId, SomeIp_MsgType *msg, uint8_t errorCode) {
  Std_ReturnType ret = SomeIp_TransError(
    TxPduId, &msg->RemoteAddr, msg->header.serviceId, msg->header.methodId, msg->header.clientId,
    msg->header.sessionId, msg->header.interfaceVersion, SOMEIP_MSG_ERROR, errorCode);
  return ret;
}

static void SomeIp_InitServer(const SomeIp_ServerServiceType *config) {
  int i;
  for (i = 0; i < config->numOfConnections; i++) {
    memset(config->connections[i].context, 0, sizeof(SomeIp_ServerConnectionContextType));
    STAILQ_INIT(&(config->connections[i].context->pendingAsyncReqMsgs));
    STAILQ_INIT(&(config->connections[i].context->pendingRxTpMsgs));
    STAILQ_INIT(&(config->connections[i].context->pendingTxTpMsgs));
    if (NULL != config->connections[i].tcpBuf) {
      memset(config->connections[i].tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
    }
  }
  memset(config->context, 0, sizeof(SomeIp_ServerContextType));
  STAILQ_INIT(&(config->context->pendingTxTpEvtMsgs));
}

static void SomeIp_InitClient(const SomeIp_ClientServiceType *config) {
  SomeIp_ClientServiceContextType *context = config->context;
  memset(context, 0, sizeof(SomeIp_ClientServiceContextType));
  STAILQ_INIT(&(context->pendingRxTpEvtMsgs));
  STAILQ_INIT(&(context->pendingRxTpMsgs));
  STAILQ_INIT(&(context->pendingTxTpMsgs));
  STAILQ_INIT(&(context->pendingWaitResMsgs));
  if (NULL != config->tcpBuf) {
    memset(config->tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
  }
}

static SomeIp_RxTpMsgType *SomeIp_RxTpMsgFind(SomeIp_RxTpMsgList *pendingRxTpMsgs,
                                              uint16_t methodId) {
  SomeIp_RxTpMsgType *rxTpMsg = NULL;
  SomeIp_RxTpMsgType *var;
  EnterCritical();
  STAILQ_FOREACH(var, pendingRxTpMsgs, entry) {
    if (var->methodId == methodId) {
      rxTpMsg = var;
      break;
    }
  }
  ExitCritical();
  return rxTpMsg;
}

static Std_ReturnType
SomeIp_ProcessRxTpMsg(uint16_t conId, SomeIp_RxTpMsgList *pendingRxTpMsgs, uint16_t methodId,
                      SomeIp_OnTpCopyRxDataFncType onTpCopyRxData, SomeIp_MsgType *msg)

{
  Std_ReturnType ret = E_OK;
  SomeIp_TpMessageType tpMsg;
  SomeIp_RxTpMsgType *var = NULL;
  if (NULL != onTpCopyRxData) {
    var = SomeIp_RxTpMsgFind(pendingRxTpMsgs, methodId);
    if (NULL == var) {
      if ((0 == msg->tpHeader.offset) && (msg->tpHeader.moreSegmentsFlag)) {
        ASLOG(SOMEIP, ("%x:%x:%x:%d FF lenght = %d\n", msg->header.serviceId, msg->header.methodId,
                       msg->header.clientId, msg->header.sessionId, msg->req.length));
        SQP_ALLOC(RxTpMsg);
        if (NULL == var) {
          ret = SOMEIP_E_NOMEM;
          ASLOG(SOMEIPE, ("%x:%x:%x:%d OoM for Tp Rx\n", msg->header.serviceId,
                          msg->header.methodId, msg->header.clientId, msg->header.sessionId));
        } else {
          var->offset = 0;
          var->clientId = msg->header.clientId;
          var->sessionId = msg->header.sessionId;
          var->RemoteAddr = msg->RemoteAddr;
          var->methodId = methodId;
          var->timer = SOMEIP_CONFIG->TpRxTimeoutTime;
          SQP_LAPPEND(RxTpMsg);
        }
      } else {
        ret = SOMEIPXF_E_MALFORMED_MESSAGE;
        ASLOG(SOMEIPE, ("%x:%x:%x:%d Tp message malformed or loss\n", msg->header.serviceId,
                        msg->header.methodId, msg->header.clientId, msg->header.sessionId));
      }
    } else {
      if ((var->clientId == msg->header.clientId) && (var->sessionId == msg->header.sessionId) &&
          (var->offset == msg->tpHeader.offset) &&
          (0 == memcmp(&var->RemoteAddr, &msg->RemoteAddr, sizeof(TcpIp_SockAddrType)))) {
        var->timer = SOMEIP_CONFIG->TpRxTimeoutTime;
        ASLOG(SOMEIP, ("%x:%x:%x:%d %s lenght = %d, offset = %d\n", msg->header.serviceId,
                       msg->header.methodId, msg->header.clientId, msg->header.sessionId,
                       msg->tpHeader.moreSegmentsFlag ? "CF" : "LF", msg->req.length, var->offset));
      } else {
        SQP_LRM_AND_FREE(RxTpMsg);
        ret = SOMEIPXF_E_MALFORMED_MESSAGE;
        ASLOG(SOMEIPE,
              ("%x:%x:%x:%d Tp message not as expected, loss maybe\n", msg->header.serviceId,
               msg->header.methodId, msg->header.clientId, msg->header.sessionId));
      }
    }
  } else {
    ret = SOMEIPXF_E_MALFORMED_MESSAGE;
  }

  if (E_OK == ret) {
    tpMsg.data = msg->req.data;
    tpMsg.length = msg->req.length;
    tpMsg.offset = msg->tpHeader.offset;
    tpMsg.moreSegmentsFlag = msg->tpHeader.moreSegmentsFlag;
    ret = onTpCopyRxData(conId, &tpMsg);
    if (E_OK == ret) {
      var->offset += msg->req.length;
      if (msg->tpHeader.moreSegmentsFlag) {
        ret = SOMEIP_E_OK_SILENT;
      } else {
        msg->req.data = tpMsg.data;
        msg->req.length = var->offset;
        SQP_LRM_AND_FREE(RxTpMsg);
      }
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_SendNextTxTpMsg(PduIdType TxPduId, uint16_t conId, uint16_t serviceId,
                                             uint16_t methodId, uint8_t interfaceVersion,
                                             uint8_t messageType,
                                             SomeIp_OnTpCopyTxDataFncType onTpCopyTxData,
                                             SomeIp_TxTpMsgType *var) {
  Std_ReturnType ret = E_OK;
  SomeIp_TpMessageType tpMsg;
  uint32_t len = var->length - var->offset;
  uint32_t offset = var->offset;
  uint8_t *data;

  if (len > SOMEIP_TP_MAX) {
    len = SOMEIP_TP_MAX;
    offset |= 0x01; /* setup more flag */
    tpMsg.moreSegmentsFlag = TRUE;
  } else {
    tpMsg.moreSegmentsFlag = FALSE;
  }

  data = Net_MemAlloc(len + 20);
  if (NULL != data) {
    tpMsg.data = &data[20];
    tpMsg.length = len;
    tpMsg.offset = var->offset;
    ret = onTpCopyTxData(conId, &tpMsg);
    if (E_OK == ret) {
      data[16] = (offset >> 24) & 0xFF;
      data[17] = (offset >> 16) & 0xFF;
      data[18] = (offset >> 8) & 0xFF;
      data[19] = offset & 0xFF;
      ret = SomeIp_Transmit(TxPduId, &var->RemoteAddr, data, serviceId, methodId, var->clientId,
                            var->sessionId, interfaceVersion, messageType | SOMEIP_TP_FLAG, E_OK,
                            len + 4);
      if (E_OK == ret) {
        var->offset += len;
#ifndef DISABLE_SOMEIP_TX_NOK_RETRY
      } else if ((E_NOT_OK == ret) && (var->retryCounter < SOMEIP_TX_NOK_RETRY_MAX)) {
        var->retryCounter++;
        ASLOG(SOMEIPW, ("Tx TP NOK, try %d\n", var->retryCounter));
        ret = E_OK;
#endif
      } else {
        ASLOG(SOMEIPE, ("Tx TP NOK\n"));
      }
    }
  } else {
    ASLOG(SOMEIPW, ("OoM, schedule Tx TP msg next time\n"));
    ret = E_OK;
  }

  if (NULL != data) {
    Net_MemFree(data);
  }

  return ret;
}

static Std_ReturnType SomeIp_TransmitEvtMsg(const SomeIp_ServerServiceType *config,
                                            const SomeIp_ServerEventType *event, uint8_t *data,
                                            uint32_t payloadLength, bool isTp,
                                            Sd_EventHandlerSubscriberType *Subscribers,
                                            uint16_t numOfSubscribers, uint32_t *mask) {
  Std_ReturnType ret = E_NOT_OK;
  Std_ReturnType ret2;
  Sd_EventHandlerSubscriberType *sub;
  uint16_t i;
  uint8_t messageType = SOMEIP_MSG_NOTIFICATION;

  if (isTp) {
    messageType |= SOMEIP_TP_FLAG;
  }

  for (i = 0; i < numOfSubscribers; i++) {
    sub = &Subscribers[i];
    if (((*mask) & (1 << i)) && (sub->flags)) {
      ret2 = SomeIp_Transmit(sub->TxPduId, &sub->RemoteAddr, data, config->serviceId,
                             event->eventId, config->clientId, sub->sessionId,
                             event->interfaceVersion, messageType, 0, payloadLength);
      if (E_OK == ret2) {
        ret = E_OK;
      } else {
        *mask &= ~(1 << i);
        ASLOG(SOMEIPE, ("Failed to notify event %x:%x to %d.%d.%d.%d:%d\n", config->serviceId,
                        event->eventId, sub->RemoteAddr.addr[0], sub->RemoteAddr.addr[1],
                        sub->RemoteAddr.addr[2], sub->RemoteAddr.addr[3], sub->RemoteAddr.port));
      }
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_SendNextTxTpEvtMsg(const SomeIp_ServerServiceType *config,
                                                const SomeIp_ServerEventType *event,
                                                SomeIp_TxTpEvtMsgType *var,
                                                Sd_EventHandlerSubscriberType *Subscribers,
                                                uint16_t numOfSubscribers) {
  Std_ReturnType ret = E_OK;
  SomeIp_TpMessageType tpMsg;
  uint32_t len = var->length - var->offset;
  uint32_t offset = var->offset;
  uint8_t *data;

  if (len > SOMEIP_TP_MAX) {
    len = SOMEIP_TP_MAX;
    offset |= 0x01; /* setup more flag */
    tpMsg.moreSegmentsFlag = TRUE;
  } else {
    tpMsg.moreSegmentsFlag = FALSE;
  }

  data = Net_MemAlloc(len + 20);
  if (NULL != data) {
    tpMsg.data = &data[20];
    tpMsg.length = len;
    tpMsg.offset = var->offset;
    ret = event->onTpCopyTxData(0, &tpMsg);
    if (E_OK == ret) {
      data[16] = (offset >> 24) & 0xFF;
      data[17] = (offset >> 16) & 0xFF;
      data[18] = (offset >> 8) & 0xFF;
      data[19] = offset & 0xFF;
      ret = SomeIp_TransmitEvtMsg(config, event, data, len + 4, TRUE, Subscribers, numOfSubscribers,
                                  &var->mask);
      if (E_OK == ret) {
        var->offset += len;
      } else {
        ASLOG(SOMEIPE, ("Tx TP EVT NOK\n"));
      }
    }
  } else {
    ASLOG(SOMEIPW, ("OoM, schedule Tx TP EVT msg next time\n"));
    ret = E_OK;
  }

  if (NULL != data) {
    Net_MemFree(data);
  }

  return ret;
}

static Std_ReturnType SomeIp_ReplyRequest(const SomeIp_ServerServiceType *config, uint16_t conId,
                                          uint16_t methodId, uint16_t clientId, uint16_t sessionId,
                                          TcpIp_SockAddrType *RemoteAddr, SomeIp_MessageType *res) {
  Std_ReturnType ret = E_OK;
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  const SomeIp_ServerMethodType *method = &config->methods[methodId];
  SomeIp_TxTpMsgType *var;

  if (IS_TP_ENABLED(method) && (res->length > SOMEIP_SF_MAX)) {
    SQP_ALLOC(TxTpMsg);
    if (NULL != var) {
      var->clientId = clientId;
      var->sessionId = sessionId;
      var->methodId = methodId;
      var->RemoteAddr = *RemoteAddr;
      var->offset = 0;
      var->length = res->length;
#ifndef DISABLE_SOMEIP_TX_NOK_RETRY
      var->retryCounter = 0;
#endif
      var->timer = config->SeparationTime;
      ret = SomeIp_SendNextTxTpMsg(connection->TxPduId, conId, config->serviceId, method->methodId,
                                   method->interfaceVersion, SOMEIP_MSG_RESPONSE,
                                   method->onTpCopyTxData, var);
      if (E_OK == ret) {
        SQP_CAPPEND(TxTpMsg);
      } else {
        SQP_FREE(TxTpMsg);
      }
    } else {
      ret = SOMEIP_E_NOMEM;
      ASLOG(SOMEIPE, ("OoM for cache Tp Tx\n"));
    }
  } else {
    ret = SomeIp_Transmit(connection->TxPduId, RemoteAddr, res->data, config->serviceId,
                          method->methodId, clientId, sessionId, method->interfaceVersion,
                          SOMEIP_MSG_RESPONSE, E_OK, res->length);
  }

  return ret;
}

static Std_ReturnType SomeIp_WaitResponse(const SomeIp_ClientServiceType *config, uint16_t methodId,
                                          uint16_t sessionId) {
  Std_ReturnType ret = E_OK;
  SomeIp_ClientServiceContextType *context = config->context;
  SomeIp_WaitResMsgType *var;

  SQP_ALLOC(WaitResMsg);
  if (NULL != var) {
    var->methodId = methodId;
    var->sessionId = sessionId;
    var->timer = config->ResponseTimeout;
    SQP_CAPPEND(WaitResMsg);
  } else {
    ASLOG(SOMEIPE, ("OoM for wait res msg\n"));
    ret = E_NOT_OK;
  }

  return ret;
}

static Std_ReturnType SomeIp_SendRequest(const SomeIp_ClientServiceType *config, uint16_t methodId,
                                         uint16_t clientId, uint16_t sessionId,
                                         TcpIp_SockAddrType *RemoteAddr, SomeIp_MessageType *req,
                                         uint8_t messageType) {
  Std_ReturnType ret = E_OK;
  SomeIp_ClientServiceContextType *context = config->context;
  const SomeIp_ClientMethodType *method = &config->methods[methodId];
  SomeIp_TxTpMsgType *var;
  uint8_t *data;

  if (IS_TP_ENABLED(method) && (req->length > SOMEIP_SF_MAX)) {
    SQP_ALLOC(TxTpMsg);
    if (NULL != var) {
      var->clientId = clientId;
      var->sessionId = sessionId;
      var->methodId = methodId;
      var->RemoteAddr = *RemoteAddr;
      var->offset = 0;
      var->length = req->length;
      var->messageType = messageType;
#ifndef DISABLE_SOMEIP_TX_NOK_RETRY
      var->retryCounter = 0;
#endif
      var->timer = config->SeparationTime;
      ret =
        SomeIp_SendNextTxTpMsg(config->TxPduId, 0, config->serviceId, method->methodId,
                               method->interfaceVersion, messageType, method->onTpCopyTxData, var);
      if (E_OK == ret) {
        SQP_CAPPEND(TxTpMsg);
      } else {
        SQP_FREE(TxTpMsg);
      }
    } else {
      ret = SOMEIP_E_NOMEM;
      ASLOG(SOMEIPE, ("OoM for cache Tp Tx\n"));
    }
  } else {
    data = Net_MemAlloc(req->length + 16);
    if (NULL != data) {
      memcpy(&data[16], req->data, req->length);
      ret = SomeIp_Transmit(config->TxPduId, RemoteAddr, data, config->serviceId, method->methodId,
                            clientId, sessionId, method->interfaceVersion, messageType, E_OK,
                            req->length);
      Net_MemFree(data);
      if (E_OK == ret) {
        ret = SomeIp_WaitResponse(config, methodId, sessionId);
      }
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_RequestOrFire(uint16_t TxMethodId, uint8_t *data, uint32_t length,
                                           uint8_t messageType) {
  Std_ReturnType ret = E_OK;
  const SomeIp_ClientServiceType *config;
  SomeIp_ClientServiceContextType *context;
  uint16_t index;
  TcpIp_SockAddrType RemoteAddr;
  SomeIp_MessageType msg;

  if (TxMethodId < SOMEIP_CONFIG->numOfTxMethods) {
    index = SOMEIP_CONFIG->TxMethod2ServiceMap[TxMethodId];
    config = (const SomeIp_ClientServiceType *)SOMEIP_CONFIG->services[index].service;
    context = config->context;
    if (FALSE == context->online) {
      ret = E_NOT_OK;
    }
  } else {
    ret = E_NOT_OK;
  }

  if (E_OK == ret) {
    index = SOMEIP_CONFIG->TxMethod2PerServiceMap[TxMethodId];
    ret = Sd_GetProviderAddr(config->sdHandleID, &RemoteAddr);
    if (E_OK == ret) {
      msg.data = data;
      msg.length = length;
    }
  }

  if (E_OK == ret) {
    if (0 == context->sessionId) {
      context->sessionId = 1;
    }

    ret = SomeIp_SendRequest(config, index, config->clientId, context->sessionId, &RemoteAddr, &msg,
                             messageType);
    if (E_OK == ret) {
      context->sessionId++;
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_SendNotification(const SomeIp_ServerServiceType *config,
                                              uint16_t eventId, SomeIp_MessageType *req,
                                              Sd_EventHandlerSubscriberType *Subscribers,
                                              uint16_t numOfSubscribers, uint32_t mask) {
  Std_ReturnType ret = E_OK;
  SomeIp_ServerContextType *context = config->context;
  const SomeIp_ServerEventType *event =
    &config->events[SOMEIP_CONFIG->TxEvent2PerServiceMap[eventId]];
  SomeIp_TxTpEvtMsgType *var;
  uint8_t *data;

  if (IS_TP_ENABLED(event) && (req->length > SOMEIP_SF_MAX)) {
    SQP_ALLOC(TxTpEvtMsg);
    if (NULL != var) {
      var->eventId = eventId;
      var->offset = 0;
      var->length = req->length;
      var->mask = mask;
      var->timer = config->SeparationTime;
      ret = SomeIp_SendNextTxTpEvtMsg(config, event, var, Subscribers, numOfSubscribers);
      if (E_OK == ret) {
        SQP_CAPPEND(TxTpEvtMsg);
      } else {
        SQP_FREE(TxTpEvtMsg);
      }
    } else {
      ret = SOMEIP_E_NOMEM;
      ASLOG(SOMEIPE, ("OoM for cache Tp Tx\n"));
    }
  } else {
    data = Net_MemAlloc(req->length + 16);
    if (NULL != data) {
      memcpy(&data[16], req->data, req->length);
      ret = SomeIp_TransmitEvtMsg(config, event, data, req->length, FALSE, Subscribers,
                                  numOfSubscribers, &mask);
      Net_MemFree(data);
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_ProcessRequest(const SomeIp_ServerServiceType *config, uint16_t conId,
                                            uint16_t methodId, SomeIp_MsgType *msg) {
  Std_ReturnType ret = E_OK;
  uint8_t *resData = NULL;
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  const SomeIp_ServerMethodType *method = &config->methods[methodId];
  SomeIp_AsyncReqMsgType *var;
  SomeIp_MessageType res;

  resData = Net_MemAlloc(method->resMaxLen + 16);
  if (NULL == resData) {
    ASLOG(SOMEIPE, ("OoM for client request\n"));
    ret = SOMEIP_E_NOMEM;
  } else {
    res.data = &resData[16];
    res.length = method->resMaxLen;
    ret = method->onRequest(conId, &msg->req, &res);
    res.data = resData;
  }

  if (E_OK == ret) {
    if (IS_TP_ENABLED(method) && (res.length > SOMEIP_SF_MAX)) {
      Net_MemFree(resData);
      resData = NULL;
    }
  }

  if (E_OK == ret) {
    ret = SomeIp_ReplyRequest(config, conId, methodId, msg->header.clientId, msg->header.sessionId,
                              &msg->RemoteAddr, &res);
  } else if (SOMEIP_E_PENDING == ret) {
    /* response pending */
    SQP_ALLOC(AsyncReqMsg);
    if (NULL != var) {
      var->clientId = msg->header.clientId;
      var->sessionId = msg->header.sessionId;
      var->methodId = methodId;
      var->RemoteAddr = msg->RemoteAddr;
      SQP_CAPPEND(AsyncReqMsg);
    } else {
      ret = SOMEIP_E_NOMEM;
      ASLOG(SOMEIPE, ("OoM for cache request\n"));
    }
  }

  if (NULL != resData) {
    Net_MemFree(resData);
  }

  return ret;
}

static Std_ReturnType SomeIp_HandleServerMessage_Request(const SomeIp_ServerServiceType *config,
                                                         uint16_t conId, SomeIp_MsgType *msg) {
  Std_ReturnType ret = SOMEIPXF_E_UNKNOWN_METHOD;
  uint16_t methodId;
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  const SomeIp_ServerMethodType *method = NULL;

  for (methodId = 0; methodId < config->numOfMethods; methodId++) {
    if (config->methods[methodId].methodId == msg->header.methodId) {
      if (((0xFF == config->methods[methodId].interfaceVersion) ||
           (config->methods[methodId].interfaceVersion == msg->header.interfaceVersion))) {
        method = &config->methods[methodId];
        ret = E_OK;
        break;
      } else {
        ret = SOMEIPXF_E_WRONG_INTERFACE_VERSION;
      }
    }
  }

  if (E_OK == ret) {
    if (msg->header.isTpFlag) {
      ret = SomeIp_ProcessRxTpMsg(conId, &context->pendingRxTpMsgs, methodId,
                                  method->onTpCopyRxData, msg);
    }
  }

  if (E_OK == ret) {
    if (SOMEIP_MSG_REQUEST == msg->header.messageType) {
      ret = SomeIp_ProcessRequest(config, conId, methodId, msg);
    } else {
      ret = method->onFireForgot(conId, &msg->req);
      if (E_OK == ret) {
        ret = SOMEIP_E_OK_SILENT;
      }
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_IsResponseExpected(const SomeIp_ClientServiceType *config,
                                                uint16_t methodId, SomeIp_MsgType *msg) {
  Std_ReturnType ret = E_NOT_OK;
  SomeIp_ClientServiceContextType *context = config->context;
  SomeIp_WaitResMsgType *var;

  EnterCritical();
  if ((FALSE == msg->header.isTpFlag) || (0 == msg->tpHeader.offset)) {
    STAILQ_FOREACH(var, &context->pendingWaitResMsgs, entry) {
      if ((var->methodId == methodId) && (var->sessionId == msg->header.sessionId)) {
        ret = E_OK;
        SQP_CRM_AND_FREE(WaitResMsg);
        break;
      }
    }
  } else { /* For TP msg with offset not 0, no check */
    ret = E_OK;
  }
  ExitCritical();

  return ret;
}

static Std_ReturnType SomeIp_HandleClientMessage_Respose(const SomeIp_ClientServiceType *config,
                                                         SomeIp_MsgType *msg) {
  Std_ReturnType ret = SOMEIPXF_E_UNKNOWN_METHOD;
  const SomeIp_ClientMethodType *method = NULL;
  SomeIp_ClientServiceContextType *context = config->context;
  uint16_t methodId;

  for (methodId = 0; methodId < config->numOfMethods; methodId++) {
    if (config->methods[methodId].methodId == msg->header.methodId) {
      if (((0xFF == config->methods[methodId].interfaceVersion) ||
           (config->methods[methodId].interfaceVersion == msg->header.interfaceVersion))) {
        method = &config->methods[methodId];
        ret = E_OK;
        break;
      } else {
        ret = SOMEIPXF_E_WRONG_INTERFACE_VERSION;
      }
    }
  }

  if (E_OK == ret) {
    ret = SomeIp_IsResponseExpected(config, methodId, msg);
    if (E_OK != ret) {
      ASLOG(SOMEIPE, ("response %x:%x:%d not as expected\n", config->serviceId, method->methodId,
                      msg->header.sessionId));
    }
  }

  if (E_OK == ret) {
    if (msg->header.isTpFlag) {
      ret =
        SomeIp_ProcessRxTpMsg(0, &context->pendingRxTpMsgs, methodId, method->onTpCopyRxData, msg);
    }
  }

  if (E_OK == ret) {
    if (E_OK == msg->header.returnCode) {
      ret = method->onResponse(&msg->req);
    } else {
      ret = method->onError(msg->header.returnCode);
    }
  }

  return ret;
}

static Std_ReturnType SomeIp_HandleServerMessage(const SomeIp_ServerServiceType *config,
                                                 uint16_t conId, SomeIp_MsgType *msg) {
  Std_ReturnType ret = E_OK;

  if (conId < config->numOfConnections) {
    if (config->serviceId != msg->header.serviceId) {
      ASLOG(SOMEIPE, ("unknown service\n"));
      ret = SOMEIPXF_E_UNKNOWN_SERVICE;
    }
  } else {
    ASLOG(SOMEIPE, ("invalid connection ID\n"));
    ret = E_NOT_OK;
  }

  if (E_OK == ret) {
    switch (msg->header.messageType) {
    case SOMEIP_MSG_REQUEST:
    case SOMEIP_MSG_REQUEST_NO_RETURN:
      ret = SomeIp_HandleServerMessage_Request(config, conId, msg);
      break;
    default:
      ASLOG(SOMEIPE, ("server: unsupported message type 0x%x\n", msg->header.messageType));
      ret = SOMEIPXF_E_WRONG_MESSAGE_TYPE;
      break;
    }
  }

  if (SOMEIP_E_PENDING == ret) {
    /* ok silent */
  } else if (E_OK != ret) {
    (void)SomeIp_ReplyError(config->connections[conId].TxPduId, msg, ret);
  } else {
    /* ok, pass */
  }

  return ret;
}

static Std_ReturnType
SomeIp_HandleClientMessage_Notification(const SomeIp_ClientServiceType *config,
                                        SomeIp_MsgType *msg) {
  Std_ReturnType ret = E_NOT_OK;
  SomeIp_ClientServiceContextType *context = config->context;
  const SomeIp_ClientEventType *event = NULL;
  uint16_t eventId;
  for (eventId = 0; eventId < config->numOfEvents; eventId++) {
    if (config->events[eventId].eventId == msg->header.methodId) {
      if (((0xFF == config->events[eventId].interfaceVersion) ||
           (config->events[eventId].interfaceVersion == msg->header.interfaceVersion))) {
        event = &config->events[eventId];
        ret = E_OK;
        break;
      } else {
        ret = SOMEIPXF_E_WRONG_INTERFACE_VERSION;
      }
    }
  }

  if (E_OK == ret) {
    if (msg->header.isTpFlag) {
      ret =
        SomeIp_ProcessRxTpMsg(0, &context->pendingRxTpEvtMsgs, eventId, event->onTpCopyRxData, msg);
    }
  }

  if (E_OK == ret) {
    ret = event->onNotification(&msg->req);
  } else {
    ret = SOMEIPXF_E_UNKNOWN_METHOD;
  }

  return ret;
}

static Std_ReturnType SomeIp_HandleClientMessage(const SomeIp_ClientServiceType *config,
                                                 SomeIp_MsgType *msg) {
  Std_ReturnType ret = E_OK;

  if (config->serviceId != msg->header.serviceId) {
    ASLOG(SOMEIPE, ("unknown service\n"));
    ret = SOMEIPXF_E_UNKNOWN_SERVICE;
  }

  if (E_OK == ret) {
    switch (msg->header.messageType) {
    case SOMEIP_MSG_NOTIFICATION:
      ret = SomeIp_HandleClientMessage_Notification(config, msg);
      break;
    case SOMEIP_MSG_RESPONSE:
      ret = SomeIp_HandleClientMessage_Respose(config, msg);
      break;
    case SOMEIP_MSG_ERROR:
      ret = SomeIp_HandleClientMessage_Respose(config, msg);
      break;
    default:
      ASLOG(SOMEIPE, ("client: unsupported message type 0x%x\n", msg->header.messageType));
      ret = SOMEIPXF_E_WRONG_MESSAGE_TYPE;
      break;
    }
  }

  return ret;
}

static void SomeIp_MainServerAsyncRequest(const SomeIp_ServerServiceType *config, uint16_t conId) {
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  DEC_SQP(AsyncReqMsg);
  const SomeIp_ServerMethodType *method = NULL;
  uint8_t *resData;
  SomeIp_MessageType res;
  Std_ReturnType ret;

  SQP_WHILE(AsyncReqMsg) {
    method = &config->methods[var->methodId];
    resData = Net_MemAlloc(method->resMaxLen + 16);
    if (NULL != resData) {
      res.data = &resData[16];
      res.length = method->resMaxLen;
      ret = method->onAsyncRequest(conId, &res);
      res.data = resData;
      if (E_OK == ret) {
        ret = SomeIp_ReplyRequest(config, conId, var->methodId, var->clientId, var->sessionId,
                                  &var->RemoteAddr, &res);
        if (E_OK != ret) {
          (void)SomeIp_TransError(connection->TxPduId, &var->RemoteAddr, config->serviceId,
                                  method->methodId, var->clientId, var->sessionId,
                                  method->interfaceVersion, SOMEIP_MSG_ERROR, ret);
        }

        SQP_CRM_AND_FREE(AsyncReqMsg);
      } else if (SOMEIP_E_PENDING == ret) {
        /* do nothing */
      } else {
        (void)SomeIp_TransError(connection->TxPduId, &var->RemoteAddr, config->serviceId,
                                method->methodId, var->clientId, var->sessionId,
                                method->interfaceVersion, SOMEIP_MSG_RESPONSE, ret);

        SQP_CRM_AND_FREE(AsyncReqMsg);
      }
      Net_MemFree(resData);
    } else {
      ASLOG(SOMEIPE, ("OoM for async request\n"));
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainServerRxTpMsg(const SomeIp_ServerServiceType *config, uint16_t conId) {
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  DEC_SQP(RxTpMsg);
  const SomeIp_ServerMethodType *method = NULL;
  Std_ReturnType ret;

  SQP_WHILE(RxTpMsg) {
    method = &config->methods[var->methodId];

    if (var->timer > 0) {
      var->timer--;
      if (0 == var->timer) {
        ASLOG(SOMEIPE,
              ("server method %x:%x:%x:%d Rx Tp msg timeout, offset %d\n", config->serviceId,
               method->methodId, var->clientId, var->sessionId, var->offset));
        ret = SomeIp_TransError(connection->TxPduId, &var->RemoteAddr, config->serviceId,
                                method->methodId, var->clientId, var->sessionId,
                                method->interfaceVersion, SOMEIP_MSG_ERROR, SOMEIPXF_E_TIMEOUT);
        if (E_NOT_OK == ret) {
          var->timer = 1; /* retry next time */
        } else {
          SQP_CRM_AND_FREE(RxTpMsg);
        }
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainServerTxTpMsg(const SomeIp_ServerServiceType *config, uint16_t conId) {
  const SomeIp_ServerConnectionType *connection = &config->connections[conId];
  SomeIp_ServerConnectionContextType *context = connection->context;
  DEC_SQP(TxTpMsg);
  const SomeIp_ServerMethodType *method = NULL;
  Std_ReturnType ret;

  SQP_WHILE(TxTpMsg) {
    method = &config->methods[var->methodId];
    if (var->timer > 0) {
      var->timer--;
    }
    if (0 == var->timer) {
      ret = SomeIp_SendNextTxTpMsg(connection->TxPduId, conId, config->serviceId, method->methodId,
                                   method->interfaceVersion, SOMEIP_MSG_RESPONSE,
                                   method->onTpCopyTxData, var);
      if (E_OK == ret) {
        if (var->offset >= var->length) {
          SQP_CRM_AND_FREE(TxTpMsg);
        } else {
          var->timer = config->SeparationTime;
        }
      } else { /* abort this tx */
        SQP_CRM_AND_FREE(TxTpMsg);
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainServerTxTpEvtMsg(const SomeIp_ServerServiceType *config) {
  SomeIp_ServerContextType *context = config->context;
  DEC_SQP(TxTpEvtMsg);
  const SomeIp_ServerEventType *event = NULL;
  Sd_EventHandlerSubscriberType *Subscribers;
  uint16_t numOfSubscribers;
  Std_ReturnType ret;

  SQP_WHILE(TxTpEvtMsg) {
    event = &config->events[SOMEIP_CONFIG->TxEvent2PerServiceMap[var->eventId]];
    if (var->timer > 0) {
      var->timer--;
    }
    if (0 == var->timer) {
      ret = Sd_GetSubscribers(event->sdHandleID, &Subscribers, &numOfSubscribers);
      if (E_OK == ret) {
        ret = SomeIp_SendNextTxTpEvtMsg(config, event, var, Subscribers, numOfSubscribers);
        if (E_OK == ret) {
          if (var->offset >= var->length) {
            SQP_CRM_AND_FREE(TxTpEvtMsg);
          } else {
            var->timer = config->SeparationTime;
          }
        } else { /* abort this tx */
          SQP_CRM_AND_FREE(TxTpEvtMsg);
        }
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainServer(const SomeIp_ServerServiceType *config) {
  uint16_t conId;

  for (conId = 0; conId < config->numOfConnections; conId++) {
    SomeIp_MainServerAsyncRequest(config, conId);
    SomeIp_MainServerTxTpMsg(config, conId);
    SomeIp_MainServerRxTpMsg(config, conId);
  }

  SomeIp_MainServerTxTpEvtMsg(config);
}

static void SomeIp_MainClientRxTpMsg(const SomeIp_ClientServiceType *config) {
  SomeIp_ClientServiceContextType *context = config->context;
  DEC_SQP(RxTpMsg);
  const SomeIp_ClientMethodType *method = NULL;

  SQP_WHILE(RxTpMsg) {
    method = &config->methods[var->methodId];

    if (var->timer > 0) {
      var->timer--;
      if (0 == var->timer) {
        ASLOG(SOMEIPE,
              ("client method %x:%x:%x:%d Rx Tp msg timeout, offset %d\n", config->serviceId,
               method->methodId, var->clientId, var->sessionId, var->offset));
        method->onError(SOMEIPXF_E_TIMEOUT);
        SQP_CRM_AND_FREE(RxTpMsg);
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainClientRxTpEvtMsg(const SomeIp_ClientServiceType *config) {
  SomeIp_ClientServiceContextType *context = config->context;
  DEC_SQP(RxTpEvtMsg);

  SQP_WHILE(RxTpEvtMsg) {
    if (var->timer > 0) {
      var->timer--;
      if (0 == var->timer) {
        ASLOG(SOMEIPE,
              ("client event %x:%x:%x:%d Rx Tp msg timeout, offset %d\n", config->serviceId,
               config->events[var->methodId].eventId, var->clientId, var->sessionId, var->offset));
        SQP_CRM_AND_FREE(RxTpEvtMsg);
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainClientTxTpMsg(const SomeIp_ClientServiceType *config) {
  SomeIp_ClientServiceContextType *context = config->context;
  DEC_SQP(TxTpMsg);
  const SomeIp_ClientMethodType *method = NULL;
  Std_ReturnType ret;

  SQP_WHILE(TxTpMsg) {
    method = &config->methods[var->methodId];
    if (var->timer > 0) {
      var->timer--;
    }
    if (0 == var->timer) {
      ret = SomeIp_SendNextTxTpMsg(config->TxPduId, 0, config->serviceId, method->methodId,
                                   method->interfaceVersion, SOMEIP_MSG_REQUEST,
                                   method->onTpCopyTxData, var);
      if (E_OK == ret) {
        if (var->offset >= var->length) {
          SQP_CRM_AND_FREE(TxTpMsg);
          (void)SomeIp_WaitResponse(config, var->methodId, var->sessionId);
        }
      } else { /* abort this tx */
        SQP_CRM_AND_FREE(TxTpMsg);
      }
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainClientWaitResMsg(const SomeIp_ClientServiceType *config) {
  SomeIp_ClientServiceContextType *context = config->context;
  DEC_SQP(WaitResMsg);
  const SomeIp_ClientMethodType *method = NULL;

  SQP_WHILE(WaitResMsg) {
    method = &config->methods[var->methodId];
    if (var->timer > 0) {
      var->timer--;
    }
    if (0 == var->timer) {
      method->onError(SOMEIPXF_E_TIMEOUT);
      SQP_CRM_AND_FREE(WaitResMsg);
    }
  }
  SQP_WHILE_END()
}

static void SomeIp_MainClient(const SomeIp_ClientServiceType *config) {
  SomeIp_MainClientTxTpMsg(config);
  SomeIp_MainClientRxTpMsg(config);
  SomeIp_MainClientRxTpEvtMsg(config);
  SomeIp_MainClientWaitResMsg(config);
}

static void SomeIp_ServerServiceModeChg(const SomeIp_ServerServiceType *service, uint16_t conId,
                                        SoAd_SoConModeType Mode) {
  SomeIp_ServerConnectionContextType *context = service->connections[conId].context;
  if (SOAD_SOCON_OFFLINE == Mode) {
    SQP_CLEAR(AsyncReqMsg);
    SQP_CLEAR(RxTpMsg);
    SQP_CLEAR(TxTpMsg);
    SQP_CLEAR(TxTpEvtMsg);
    context->online = FALSE;
  } else {
    context->online = TRUE;
  }
}

static void SomeIp_ClientServiceModeChg(const SomeIp_ClientServiceType *service,
                                        SoAd_SoConModeType Mode) {
  SomeIp_ClientServiceContextType *context = service->context;

  if (SOAD_SOCON_OFFLINE == Mode) {
    SQP_CLEAR(RxTpEvtMsg);
    SQP_CLEAR(RxTpMsg);
    SQP_CLEAR(TxTpMsg);
    context->online = FALSE;
    service->onAvailability(FALSE);
  } else {
    context->online = TRUE;
    context->sessionId = 0;
    service->onAvailability(TRUE);
  }
}
static Std_ReturnType SomeIp_HandleRxMsg(PduIdType RxPduId, SomeIp_MsgType *msg) {
  Std_ReturnType ret;
  uint16_t index;
  ASLOG(SOMEIP, ("[%d] service 0x%x:0x%x:%d message type %d return code %d payload %d bytes "
                 "from client 0x%x:%d %d.%d.%d.%d:%d\n",
                 RxPduId, msg->header.serviceId, msg->header.methodId, msg->header.interfaceVersion,
                 msg->header.messageType, msg->header.returnCode, msg->header.length - 8,
                 msg->header.clientId, msg->header.sessionId, msg->RemoteAddr.addr[0],
                 msg->RemoteAddr.addr[1], msg->RemoteAddr.addr[2], msg->RemoteAddr.addr[3],
                 msg->RemoteAddr.port));
  PCAP_TRACE(msg->header.serviceId, msg->header.methodId, msg->header.interfaceVersion,
             msg->header.messageType, msg->header.returnCode, msg->req.data, msg->req.length,
             msg->header.clientId, msg->header.sessionId, &msg->RemoteAddr, msg->header.isTpFlag,
             msg->tpHeader.offset, msg->tpHeader.moreSegmentsFlag, TRUE);
  if (RxPduId < SOMEIP_CONFIG->numOfPIDs) {
    index = SOMEIP_CONFIG->PID2ServiceMap[RxPduId];
    if (index < SOMEIP_CONFIG->numOfService) {
      if (SOMEIP_CONFIG->services[index].isServer) {
        ret = SomeIp_HandleServerMessage(
          (const SomeIp_ServerServiceType *)SOMEIP_CONFIG->services[index].service,
          SOMEIP_CONFIG->PID2ServiceConnectionMap[RxPduId], msg);
      } else {
        ret = SomeIp_HandleClientMessage(
          (const SomeIp_ClientServiceType *)SOMEIP_CONFIG->services[index].service, msg);
      }
    } else {
      ret = E_NOT_OK;
      ASLOG(SOMEIPE, ("invalid configuration for PID2ServiceMap\n"));
    }
  } else {
    ret = SOMEIPXF_E_NOT_REACHABLE;
  }

  return ret;
}
/* ================================ [ FUNCTIONS ] ============================================== */
void SomeIp_RxIndication(PduIdType RxPduId, const PduInfoType *PduInfoPtr) {
  SomeIp_MsgType msg;
  Std_ReturnType ret;

  ret = SomeIp_DecodeMsg(PduInfoPtr, &msg);
  if (E_OK == ret) {
    SomeIp_HandleRxMsg(RxPduId, &msg);
  } else {
    ASLOG(SOMEIPE, ("IF message malformed\n"));
  }
}

BufReq_ReturnType SomeIp_SoAdTpStartOfReception(PduIdType RxPduId, const PduInfoType *PduInfoPtr,
                                                PduLengthType TpSduLength,
                                                PduLengthType *bufferSizePtr) {
  return BUFREQ_OK;
}

BufReq_ReturnType SomeIp_SoAdTpCopyRxData(PduIdType RxPduId, const PduInfoType *PduInfoPtr,
                                          PduLengthType *bufferSizePtr) {
  BufReq_ReturnType bret = BUFREQ_E_NOT_OK;
  Std_ReturnType ret = E_NOT_OK;
  SomeIp_MsgType msg;
  const SomeIp_ClientServiceType *cs;
  const SomeIp_ServerServiceType *ss;
  SomeIp_TcpBufferType *tcpBuf = NULL;
  PduInfoType PduInfo;
  uint16_t index;
  uint32_t leftLen;

  if (RxPduId < SOMEIP_CONFIG->numOfPIDs) {
    index = SOMEIP_CONFIG->PID2ServiceMap[RxPduId];
    if (index < SOMEIP_CONFIG->numOfService) {
      if (SOMEIP_CONFIG->services[index].isServer) {
        ss = (const SomeIp_ServerServiceType *)SOMEIP_CONFIG->services[index].service;
        index = SOMEIP_CONFIG->PID2ServiceConnectionMap[RxPduId];
        tcpBuf = ss->connections[index].tcpBuf;
      } else {
        cs = (const SomeIp_ClientServiceType *)SOMEIP_CONFIG->services[index].service;
        tcpBuf = cs->tcpBuf;
      }
    }
  }

  *bufferSizePtr = 0;
  if (NULL != tcpBuf) {
    PduInfo = *PduInfoPtr;
    ASLOG(SOMEIP, ("Tcp input(%d)\n", PduInfo.SduLength));
    while (PduInfo.SduLength > 0) {
      if (NULL == tcpBuf->data) {
        ret = SomeIp_HandleTcpHeader(tcpBuf, &PduInfo, &msg);
        if (E_OK == ret) {
          ASLOG(SOMEIP, ("consume all\n"));
          SomeIp_HandleRxMsg(RxPduId, &msg);
          *bufferSizePtr += msg.header.length + 8;
          memset(tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
          PduInfo.SduLength = 0;
          bret = BUFREQ_OK;
        } else if (SOMEIP_E_PENDING == ret) {
          /* do nothing as header not full */
          ASLOG(SOMEIP, ("not enough header, wait\n"));
        } else if (SOMEIP_E_MSG_TOO_SHORT == ret) {
          tcpBuf->data = Net_MemAlloc(msg.header.length - 8);
          if (NULL != tcpBuf->data) {
            tcpBuf->length = msg.header.length - 8;
            tcpBuf->offset = PduInfo.SduLength;
            memcpy(tcpBuf->data, PduInfo.SduDataPtr, PduInfo.SduLength);
            *bufferSizePtr += msg.header.length + 8;
            PduInfo.SduLength = 0;
            bret = BUFREQ_OK;
            ASLOG(SOMEIP, ("too short, offset %d, length %d\n", tcpBuf->offset, tcpBuf->length));
          } else {
            memset(tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
            ASLOG(SOMEIPE, ("OoM for tcp buffer, abort\n"));
          }
        } else if (SOMEIP_E_MSG_TOO_LARGE == ret) {
          leftLen = PduInfo.SduLength - (msg.header.length - 8);
          PduInfo.SduLength = msg.header.length - 8;
          ASLOG(SOMEIP, ("too large, consume %d, left %d\n", PduInfo.SduLength, leftLen));
          ret = SomeIp_DecodeMsgTcp(tcpBuf->header, PduInfo.SduDataPtr, PduInfo.SduLength,
                                    (const TcpIp_SockAddrType *)PduInfo.MetaDataPtr, &msg);
          if (E_OK == ret) {
            SomeIp_HandleRxMsg(RxPduId, &msg);
          } else {
            ASLOG(SOMEIPE, ("Fatal Protol error\n"));
          }
          memset(tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
          PduInfo.SduDataPtr = &PduInfo.SduDataPtr[PduInfo.SduLength];
          PduInfo.SduLength = leftLen;
        } else {
          memset(tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
          ASLOG(SOMEIPE, ("TP message malformed, abort\n"));
        }
      } else {
        leftLen = tcpBuf->length - tcpBuf->offset;
        if (leftLen >= PduInfo.SduLength) {
          leftLen = PduInfo.SduLength;
        }
        memcpy(&tcpBuf->data[tcpBuf->offset], PduInfo.SduDataPtr, leftLen);
        PduInfo.SduDataPtr = &PduInfo.SduDataPtr[leftLen];
        PduInfo.SduLength -= leftLen;
        tcpBuf->offset += leftLen;
        if (tcpBuf->offset >= tcpBuf->length) {
          ret = SomeIp_DecodeMsgTcp(tcpBuf->header, tcpBuf->data, tcpBuf->length,
                                    (const TcpIp_SockAddrType *)PduInfo.MetaDataPtr, &msg);
          if (E_OK == ret) {
            SomeIp_HandleRxMsg(RxPduId, &msg);
          } else {
            ASLOG(SOMEIPE, ("Fatal Protol error\n"));
          }
          Net_MemFree(tcpBuf->data);
          memset(tcpBuf, 0, sizeof(SomeIp_TcpBufferType));
          *bufferSizePtr += tcpBuf->length + 16;
          bret = BUFREQ_OK;
        }
      }
    }
  } else {
    ASLOG(SOMEIPE, ("Invalid TP RxPduId = %d\n", RxPduId));
  }

  return bret;
}

void SomeIp_SoConModeChg(SoAd_SoConIdType SoConId, SoAd_SoConModeType Mode) {
  Std_ReturnType ret = E_NOT_OK;
  const SomeIp_ServiceType *service = NULL;
  const SomeIp_ServerServiceType *ss = NULL;
  SomeIp_ServerContextType *context;
  uint16_t i, j;
  uint16_t conId = 0;

  ASLOG(SOMEIP, ("SoConId %d Mode %d\n", SoConId, Mode));
  for (i = 0; (i < SOMEIP_CONFIG->numOfService) && (E_NOT_OK == ret); i++) {
    service = &SOMEIP_CONFIG->services[i];
    if (service->SoConId == SoConId) {
      ret = E_OK;
    } else if (service->isServer) {
      ss = (const SomeIp_ServerServiceType *)service->service;
      for (j = 0; (j < ss->numOfConnections) && (E_NOT_OK == ret); j++) {
        if (ss->connections[j].SoConId == SoConId) {
          conId = j;
          ret = E_OK;
        }
      }
    } else {
      /* not match */
    }
  }

  if (E_OK == ret) {
    if (service->isServer) {
      if (service->SoConId == SoConId) {
        ss = (const SomeIp_ServerServiceType *)service->service;
        context = ss->context;
        if (SOAD_SOCON_OFFLINE == Mode) {
          SQP_CLEAR(TxTpEvtMsg);
          context->online = FALSE;
        } else {
          context->online = TRUE;
        }
        if (TCPIP_IPPROTO_TCP == ss->protocol) {
          if (SOAD_SOCON_OFFLINE == Mode) {
            for (j = 0; j < ss->numOfConnections; j++) {
              (void)SoAd_CloseSoCon(ss->connections[j].SoConId, TRUE);
            }
          }
        } else {
          SomeIp_ServerServiceModeChg(ss, 0, Mode);
        }
      } else {
        SomeIp_ServerServiceModeChg((const SomeIp_ServerServiceType *)service->service, conId,
                                    Mode);
      }
    } else {
      SomeIp_ClientServiceModeChg((const SomeIp_ClientServiceType *)service->service, Mode);
    }
  } else {
    ASLOG(SOMEIPE, ("SoConId %d unknown\n", SoConId));
  }
}

Std_ReturnType SomeIp_Request(uint16_t TxMethodId, uint8_t *data, uint32_t length) {
  return SomeIp_RequestOrFire(TxMethodId, data, length, SOMEIP_MSG_REQUEST);
}

Std_ReturnType SomeIp_FireForgot(uint16_t TxMethodId, uint8_t *data, uint32_t length) {
  return SomeIp_RequestOrFire(TxMethodId, data, length, SOMEIP_MSG_REQUEST_NO_RETURN);
}

Std_ReturnType SomeIp_Notification(uint16_t TxEventId, uint8_t *data, uint32_t length) {
  Std_ReturnType ret = E_OK;
  const SomeIp_ServerServiceType *config;
  const SomeIp_ServerConnectionType *connection;
  const SomeIp_ServerEventType *event;
  uint16_t index;
  Sd_EventHandlerSubscriberType *Subscribers;
  uint16_t numOfSubscribers;
  uint16_t i;
  SomeIp_MessageType msg;
  uint32_t mask;
  TcpIp_SockAddrType RemoteAddr;

  if (TxEventId < SOMEIP_CONFIG->numOfTxEvents) {
    index = SOMEIP_CONFIG->TxEvent2ServiceMap[TxEventId];
    config = (const SomeIp_ServerServiceType *)SOMEIP_CONFIG->services[index].service;
    if (FALSE == config->context->online) {
      ret = E_NOT_OK;
    }
  } else {
    ret = E_NOT_OK;
  }

  if (E_OK == ret) {
    index = SOMEIP_CONFIG->TxEvent2PerServiceMap[TxEventId];
    event = &config->events[index];
    ret = Sd_GetSubscribers(event->sdHandleID, &Subscribers, &numOfSubscribers);
  }

  if (E_OK == ret) {
    for (i = 0; i < numOfSubscribers; i++) {
      if (0 != Subscribers[i].flags) {
        if (TCPIP_IPPROTO_UDP == config->protocol) {
          mask |= (1 << i);
          Subscribers[i].TxPduId = config->connections[0].TxPduId;
        } else {
          for (index = 0; index < config->numOfConnections; index++) {
            connection = &config->connections[index];
            ret = SoAd_GetRemoteAddr(connection->SoConId, &RemoteAddr);
            if ((E_OK == ret) && (0 == memcmp(Subscribers[i].RemoteAddr.addr, RemoteAddr.addr,
                                              sizeof(RemoteAddr.addr)))) {
              mask |= (1 << i);
              Subscribers[i].TxPduId = connection->TxPduId;
              break;
            }
          }
          if (index >= config->numOfConnections) {
            ASLOG(SOMEIPE, ("can't find remote for subscriber\n"));
          }
        }
      }
    }
    ret = E_OK;
    if (0 == mask) {
      ret = E_NOT_OK;
    }
  }

  if (E_OK == ret) {
    msg.data = data;
    msg.length = length;

    /* increase the session Id */
    for (i = 0; i < numOfSubscribers; i++) {
      if (mask & (1 << i)) {
        Subscribers[i].sessionId++;
        if (0 == Subscribers[i].sessionId) {
          Subscribers[i].sessionId = 1;
        }
      }
    }
  }

  if (E_OK == ret) {
    ret = SomeIp_SendNotification(config, TxEventId, &msg, Subscribers, numOfSubscribers, mask);
  }

  return ret;
}

void SomeIp_Init(const SomeIp_ConfigType *ConfigPtr) {
  int i;
  SQP_INIT(AsyncReqMsg);
  SQP_INIT(TxTpMsg);
  SQP_INIT(RxTpMsg);
  SQP_INIT(TxTpEvtMsg);
  SQP_INIT(WaitResMsg);
  for (i = 0; i < SOMEIP_CONFIG->numOfService; i++) {
    if (SOMEIP_CONFIG->services[i].isServer) {
      SomeIp_InitServer((const SomeIp_ServerServiceType *)SOMEIP_CONFIG->services[i].service);
    } else {
      SomeIp_InitClient((const SomeIp_ClientServiceType *)SOMEIP_CONFIG->services[i].service);
    }
  }
}

void SomeIp_MainFunction(void) {
  int i;
  for (i = 0; i < SOMEIP_CONFIG->numOfService; i++) {
    if (SOMEIP_CONFIG->services[i].isServer) {
      SomeIp_MainServer((const SomeIp_ServerServiceType *)SOMEIP_CONFIG->services[i].service);
    } else {
      SomeIp_MainClient((const SomeIp_ClientServiceType *)SOMEIP_CONFIG->services[i].service);
    }
  }
}
