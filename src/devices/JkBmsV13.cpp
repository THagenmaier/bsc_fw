// Copyright (c) 2022 tobias & maxe
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include "devices/JkBmsV13.h"
#include "BmsData.h"
#include "mqtt_t.h"
#include "log.h"

const char *TAG_V13 = "JK_BMS_V13";

#define JKV13_DEBUG

#define headerLen 4
#define JK_V13_SLAVE_ADDR 0


Stream *mPortJkV13;
uint8_t u8_mDevNrJkV13;
uint16_t u16_mLastRecvBytesCntJkV13;
static void (*callbackSetTxRxEn)(uint8_t, uint8_t) = NULL;
static serialDevData_s *mDevData;

enum SM_readDataV13 {SEARCH_START_BYTE1, SEARCH_START_BYTE2, SLAVE_ADDR, CMD_CODE, RECV_DATA};

void      JkBmsV13_sendMessage(uint8_t *sendMsg, uint32_t size);
bool      JkBmsV13_recvAnswer(uint8_t * t_outMessage);
void      JkBmsV13_parseData(uint8_t * t_message);


bool JkBmsV13_readBmsData(Stream *port, uint8_t devNr, void (*callback)(uint8_t, uint8_t), serialDevData_s *devData)
{
  bool bo_lRet=true;
  mDevData=devData;
  mPortJkV13 = port;
  u8_mDevNrJkV13 = devNr;
  callbackSetTxRxEn = callback;
  uint8_t getDataMsgV13[] = { 0x55, 0xAA, 0x00, 0xFF, 0x00, 0x00, 0xFE };

  uint8_t response[JKBMSV13_MAX_ANSWER_LEN];

  getDataMsgV13[2] = JK_V13_SLAVE_ADDR;

  //Berechne CRC
  getDataMsgV13[sizeof(getDataMsgV13)-1] = 0;
  for(uint32_t i=0;i<sizeof(getDataMsgV13)-1;i++){
    getDataMsgV13[sizeof(getDataMsgV13)-1] +=  getDataMsgV13[i];
  }

  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"Serial %i send: %.2X %.2X %.2X %.2X %.2X %.2X %.2X \n",u8_mDevNrJkV13, getDataMsgV13[0], getDataMsgV13[1], getDataMsgV13[2], getDataMsgV13[3], getDataMsgV13[4], getDataMsgV13[5], getDataMsgV13[6]);
  #endif

  JkBmsV13_sendMessage(getDataMsgV13, sizeof(getDataMsgV13));

  if(JkBmsV13_recvAnswer(response))
  {
    JkBmsV13_parseData(response);

    //mqtt
    mqttPublish(MQTT_TOPIC_BMS_BT, BT_DEVICES_COUNT+u8_mDevNrJkV13, MQTT_TOPIC2_TOTAL_VOLTAGE, -1, getBmsTotalVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13));
  }
  else bo_lRet=false;
  
  if(devNr>=2) callbackSetTxRxEn(u8_mDevNrJkV13,serialRxTx_RxTxDisable);
  return bo_lRet;  
}


void JkBmsV13_sendMessage(uint8_t *sendMsg , uint32_t size)
{
  callbackSetTxRxEn(u8_mDevNrJkV13,serialRxTx_TxEn);
  usleep(50);
  mPortJkV13->write(sendMsg, size);
  mPortJkV13->flush();  
  //usleep(1000);
  callbackSetTxRxEn(u8_mDevNrJkV13,serialRxTx_RxEn);
}


bool JkBmsV13_recvAnswer(uint8_t *p_lRecvBytes)
{
  uint8_t SMrecvState, u8_lRecvByte;
  uint16_t u16_lRecvDataLen;
  uint32_t u32_lStartTime=millis();
  SMrecvState=SEARCH_START_BYTE1;
  u16_mLastRecvBytesCntJkV13=0;
  uint8_t crc=0;
  char strTmp[1024];
  char *sptr = strTmp;

  for(;;)
  {
    //Timeout
    if(millis()-u32_lStartTime > 500) 
    {
      BSC_LOGD(TAG_V13,"Timeout: Serial=%i, u8_lRecvDataLen=%i, u8_lRecvBytesCnt=%i\n",u8_mDevNrJkV13, u16_lRecvDataLen, u16_mLastRecvBytesCntJkV13);
      //for(uint16_t x=0;x<u16_lRecvDataLen;x++)
      //{
      //  BSC_LOGD(TAG_V13,"%02x ",p_lRecvBytes[x]);
      //}
      //if(u16_lRecvDataLen != 0){
      //  BSC_LOGI(TAG_V13,"%s",strTmp);
      //}
      return false;
    }

    //Überprüfen ob Zeichen verfügbar
    if (mPortJkV13->available() > 0)
    {
      u16_lRecvDataLen++;
      u8_lRecvByte = mPortJkV13->read();

      if(u16_mLastRecvBytesCntJkV13<JKBMSV13_MAX_ANSWER_LEN-1){crc += u8_lRecvByte;}

      switch (SMrecvState)  {
        case SEARCH_START_BYTE1:
          if (u8_lRecvByte == 0xEB){SMrecvState=SEARCH_START_BYTE2;}
          break;
        case SEARCH_START_BYTE2:
          if (u8_lRecvByte == 0x90){SMrecvState=SLAVE_ADDR;}
          break;
        case SLAVE_ADDR:
          if (u8_lRecvByte == JK_V13_SLAVE_ADDR){SMrecvState=CMD_CODE;}
          break;   
        case CMD_CODE:
          if (u8_lRecvByte == 0xFF){SMrecvState=RECV_DATA;}
          break;                      
        case RECV_DATA:
          p_lRecvBytes[u16_mLastRecvBytesCntJkV13]=u8_lRecvByte;
          u16_mLastRecvBytesCntJkV13++;
          break;      
        default:
          SMrecvState=SEARCH_START_BYTE1;
          crc = 0;
          break;
        }

    }

    if(u16_mLastRecvBytesCntJkV13==JKBMSV13_MAX_ANSWER_LEN) break; //Recv Pakage complete
  }

#ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"recv cnt: %i",u16_mLastRecvBytesCntJkV13);
#endif
  //Überprüfe Cheksum
  //debugPrintf("crc=%i %i\n", crcB3, crcB4);
  if(p_lRecvBytes[u16_mLastRecvBytesCntJkV13-1]!=crc) return false; 

  return true;
}


void JkBmsV13_parseData(uint8_t * t_message)
{
  uint32_t u32_lBalanceCapacity=0;
  uint16_t u16_lCycle=0;

  // Variables for 0x79
  uint32_t u32_lZellVoltage=0;
  uint32_t u16_lAVGZellVoltage=0;
  uint8_t  u8_error;
  uint16_t u16_lZellDifferenceVoltage = 0;
  uint32_t u32_lBalanceCurrent=0;
  uint8_t  u8_isBalanceActive = 0;
  uint8_t u8_lNumOfCells = 0;
  
  uint16_t u16_lZellVoltage = 0;
  uint16_t u16_lZellMinVoltage = 0xFFFF;
  uint16_t u16_lZellMaxVoltage = 0;
  uint16_t u16_lCellLow = 0xFFFF; 
  uint16_t u16_lCellHigh = 0x0;

  int16_t i16_temperature = 0;


  u32_lZellVoltage = ((t_message[4-headerLen]<<8) | t_message[5]);
  //setBmsTotalVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, u32_lZellVoltage/100.0);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"Vstack=%i", u32_lZellVoltage);
  #endif


  u16_lAVGZellVoltage = ((t_message[6-headerLen]<<8) | t_message[7-headerLen]);
  setBmsAvgVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lAVGZellVoltage/100.0);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"Vavgcell=%i", u16_lAVGZellVoltage);
  #endif

  u8_error = (t_message[12-headerLen]);
  setBmsErrors(BT_DEVICES_COUNT+u8_mDevNrJkV13, u8_error);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"BMSerror=%i", u8_error);
  #endif

  u16_lZellDifferenceVoltage = ((t_message[6-headerLen]<<8) | t_message[7-headerLen]);
  setBmsMaxCellDifferenceVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lZellDifferenceVoltage/100.0);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"Vcdiff=%i", u16_lZellDifferenceVoltage);
  #endif

  u32_lBalanceCurrent = ((t_message[15-headerLen]<<8) | t_message[16-headerLen]);
  setBmsBalancingCurrent(BT_DEVICES_COUNT+u8_mDevNrJkV13, u32_lBalanceCurrent*0.001f);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"VbalC=%i", u32_lBalanceCurrent);
  #endif

  u8_isBalanceActive = t_message[21-headerLen]; 
  setBmsIsBalancingActive(BT_DEVICES_COUNT+u8_mDevNrJkV13, u8_isBalanceActive);
  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"VbalA=%i", u8_isBalanceActive);
  #endif

  u8_lNumOfCells = t_message[22-headerLen]; 

  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"CellCount=%i", u8_lNumOfCells);
  #endif

  uint32_t sum = 0;
  for(uint16_t n=0; n<u8_lNumOfCells;n++)
  {
    u16_lZellVoltage = ((t_message[23-headerLen+(n*2)]<<8) | t_message[24-headerLen+(n*2)]);
    sum += u16_lZellVoltage;
    setBmsCellVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13,n, u16_lZellVoltage);

    if(u16_lZellVoltage <= u16_lZellMinVoltage){
      u16_lZellMinVoltage = u16_lZellVoltage;
      u16_lCellLow = n;
    }

    if(u16_lZellVoltage >= u16_lZellMaxVoltage){
      u16_lZellMaxVoltage = u16_lZellVoltage;
      u16_lCellHigh = n;
    }
     
  }

  setBmsTotalVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, sum/1000.0);
  setBmsMaxCellVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lZellMaxVoltage);
  setBmsMinCellVoltage(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lZellMinVoltage);
  setBmsMaxVoltageCellNumber(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lCellHigh+1);
  setBmsMinVoltageCellNumber(BT_DEVICES_COUNT+u8_mDevNrJkV13, u16_lCellLow+1);

  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"TotalV=%i, MaxV=%i, MinV=%i", sum, u16_lZellMaxVoltage, u16_lZellMinVoltage);
  #endif

  //Rufe SOC funktion auf ... dummy 0, sollte anhand einstellungen berechnet werden
  setBmsChargePercentage(BT_DEVICES_COUNT+u8_mDevNrJkV13, 0);

  //#ifdef JKV13_DEBUG 
  //BSC_LOGD(TAG_V13,"SOC=%i, SumV=%i", soc,sum);
  //#endif

  i16_temperature = ((t_message[71-headerLen]<<8) | t_message[72-headerLen]);
  setBmsTempature(BT_DEVICES_COUNT+u8_mDevNrJkV13, 0, i16_temperature);

  #ifdef JKV13_DEBUG 
  BSC_LOGD(TAG_V13,"BMStemp=%i", i16_temperature);
  #endif


  setBmsLastDataMillis(BT_DEVICES_COUNT+u8_mDevNrJkV13,millis());

  /*if(millis()>(mqttSendeTimer_jk_v13+10000))
  {
    //Nachrichten senden
    mqttPublish("bms/"+String(BT_DEVICES_COUNT+u8_mDevNrJkV13)+"/BalanceCapacity", u32_lBalanceCapacity);
    mqttPublish("bms/"+String(BT_DEVICES_COUNT+u8_mDevNrJkV13)+"/Cycle", u16_lCycle);

    mqttSendeTimer_jk_v13=millis();
  }*/

}

