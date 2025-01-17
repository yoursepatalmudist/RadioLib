#include "LoRaWAN.h"

#include <string.h>

#if !defined(RADIOLIB_EXCLUDE_LORAWAN)

// flag to indicate whether we have received a downlink
static volatile bool downlinkReceived = false;

// interrupt service routine to handle downlinks automatically
#if defined(ESP8266) || defined(ESP32)
  IRAM_ATTR
#endif
static void LoRaWANNodeOnDownlink(void) {
  downlinkReceived = true;
}

// flag to indicate whether channel scan operation is complete
static volatile bool scanFlag = false;

// interrupt service routine to handle downlinks automatically
#if defined(ESP8266) || defined(ESP32)
  IRAM_ATTR
#endif
static void LoRaWANNodeOnChannelScan(void) {
  scanFlag = true;
}

LoRaWANNode::LoRaWANNode(PhysicalLayer* phy, const LoRaWANBand_t* band) {
  this->phyLayer = phy;
  this->band = band;
  this->FSK = false;
}

void LoRaWANNode::wipe() {
  Module* mod = this->phyLayer->getMod();
  mod->hal->wipePersistentStorage();
}

int16_t LoRaWANNode::begin() {
  int16_t state = this->setPhyProperties();
  RADIOLIB_ASSERT(state);

  // check the magic value
  Module* mod = this->phyLayer->getMod();
  if(mod->hal->getPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_MAGIC_ID) != RADIOLIB_LORAWAN_MAGIC) {
    // the magic value is not set, user will have to do perform the join procedure
    return(RADIOLIB_ERR_NETWORK_NOT_JOINED);
  }

  // pull all needed information from persistent storage
  this->devAddr = mod->hal->getPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_DEV_ADDR_ID);
  mod->hal->readPersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_APP_S_KEY_ID), this->appSKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->readPersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_FNWK_SINT_KEY_ID), this->fNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->readPersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_SNWK_SINT_KEY_ID), this->sNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->readPersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_NWK_SENC_KEY_ID), this->nwkSEncKey, RADIOLIB_AES128_BLOCK_SIZE);
  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::beginOTAA(uint64_t joinEUI, uint64_t devEUI, uint8_t* nwkKey, uint8_t* appKey, bool force) {
  // check if we actually need to send the join request
  Module* mod = this->phyLayer->getMod();
  if(!force && (mod->hal->getPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_MAGIC_ID) == RADIOLIB_LORAWAN_MAGIC)) {
    // the device has joined already, we can just pull the data from persistent storage
    return(this->begin());
  }

  // set the physical layer configuration
  int16_t state = this->setPhyProperties();
  RADIOLIB_ASSERT(state);

  // get dev nonce from persistent storage and increment it
  uint16_t devNonce = mod->hal->getPersistentParameter<uint16_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_DEV_NONCE_ID);
  mod->hal->setPersistentParameter<uint16_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_DEV_NONCE_ID, devNonce + 1);

  // build the join-request message
  uint8_t joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_LEN];
  
  // set the packet fields
  joinRequestMsg[0] = RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_REQUEST | RADIOLIB_LORAWAN_MHDR_MAJOR_R1;
  LoRaWANNode::hton<uint64_t>(&joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_JOIN_EUI_POS], joinEUI);
  LoRaWANNode::hton<uint64_t>(&joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_DEV_EUI_POS], devEUI);
  LoRaWANNode::hton<uint16_t>(&joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_DEV_NONCE_POS], devNonce);

  // add the authentication code
  uint32_t mic = this->generateMIC(joinRequestMsg, RADIOLIB_LORAWAN_JOIN_REQUEST_LEN - sizeof(uint32_t), nwkKey);
  LoRaWANNode::hton<uint32_t>(&joinRequestMsg[RADIOLIB_LORAWAN_JOIN_REQUEST_LEN - sizeof(uint32_t)], mic);

  // send it
  state = this->phyLayer->transmit(joinRequestMsg, RADIOLIB_LORAWAN_JOIN_REQUEST_LEN);
  RADIOLIB_ASSERT(state);
  
  // set the function that will be called when the reply is received
  this->phyLayer->setPacketReceivedAction(LoRaWANNodeOnDownlink);

  // downlink messages are sent with inverted IQ
  // TODO use downlink() for this
  if(!this->FSK) {
    state = this->phyLayer->invertIQ(true);
    RADIOLIB_ASSERT(state);
  }
  
  // start receiving
  uint32_t start = mod->hal->millis();
  downlinkReceived = false;
  state = this->phyLayer->startReceive();
  RADIOLIB_ASSERT(state);

  // wait for the reply or timeout
  while(!downlinkReceived) {
    if(mod->hal->millis() - start >= RADIOLIB_LORAWAN_JOIN_ACCEPT_DELAY_2_MS + 2000) {
      downlinkReceived = false;
      if(!this->FSK) {
        this->phyLayer->invertIQ(false);
      }
      return(RADIOLIB_ERR_RX_TIMEOUT);
    }
  }

  // we have a message, reset the IQ inversion
  downlinkReceived = false;
  this->phyLayer->clearPacketReceivedAction();
  if(!this->FSK) {
    state = this->phyLayer->invertIQ(false);
    RADIOLIB_ASSERT(state);
  }

  // build the buffer for the reply data
  uint8_t joinAcceptMsgEnc[RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN];

  // check received length
  size_t lenRx = this->phyLayer->getPacketLength(true);
  if((lenRx != RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN) && (lenRx != RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN - RADIOLIB_LORAWAN_JOIN_ACCEPT_CFLIST_LEN)) {
    RADIOLIB_DEBUG_PRINTLN("joinAccept reply length mismatch, expected %luB got %luB", RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN, lenRx);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // read the packet
  state = this->phyLayer->readData(joinAcceptMsgEnc, lenRx);
  // downlink frames are sent without CRC, which will raise error on SX127x
  // we can ignore that error
  if(state != RADIOLIB_ERR_LORA_HEADER_DAMAGED) {
    RADIOLIB_ASSERT(state);
  }

  // check reply message type
  if((joinAcceptMsgEnc[0] & RADIOLIB_LORAWAN_MHDR_MTYPE_MASK) != RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_ACCEPT) {
    RADIOLIB_DEBUG_PRINTLN("joinAccept reply message type invalid, expected 0x%02x got 0x%02x", RADIOLIB_LORAWAN_MHDR_MTYPE_JOIN_ACCEPT, joinAcceptMsgEnc[0]);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // decrypt the join accept message
  // this is done by encrypting again in ECB mode
  // the first byte is the MAC header which is not encrypted
  uint8_t joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN];
  joinAcceptMsg[0] = joinAcceptMsgEnc[0];
  RadioLibAES128Instance.init(nwkKey);
  RadioLibAES128Instance.encryptECB(&joinAcceptMsgEnc[1], RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN - 1, &joinAcceptMsg[1]);

  //Module::hexdump(joinAcceptMsg, lenRx);

  // check LoRaWAN revision (the MIC verification depends on this)
  uint8_t dlSettings = joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_DL_SETTINGS_POS];
  if(dlSettings & RADIOLIB_LORAWAN_JOIN_ACCEPT_R_1_1) {
    // 1.1 version, first we need to derive the join accept integrity key
    uint8_t keyDerivationBuff[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_JS_INT_KEY;
    LoRaWANNode::hton<uint64_t>(&keyDerivationBuff[1], devEUI);
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->jSIntKey);

    // prepare the buffer for MIC calculation
    uint8_t micBuff[3*RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
    micBuff[0] = RADIOLIB_LORAWAN_JOIN_REQUEST_TYPE;
    LoRaWANNode::hton<uint64_t>(&micBuff[1], joinEUI);
    LoRaWANNode::hton<uint16_t>(&micBuff[9], devNonce);
    memcpy(&micBuff[11], joinAcceptMsg, lenRx);

    //Module::hexdump(micBuff, lenRx + 11);
    
    if(!verifyMIC(micBuff, lenRx + 11, this->jSIntKey)) {
      return(RADIOLIB_ERR_CRC_MISMATCH);
    }
  
  } else {
    // 1.0 version
    if(!verifyMIC(joinAcceptMsg, lenRx, nwkKey)) {
      return(RADIOLIB_ERR_CRC_MISMATCH);
    }

  }

  // parse the contents
  uint32_t joinNonce = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_JOIN_NONCE_POS], 3);
  uint32_t homeNetId = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_HOME_NET_ID_POS], 3);
  this->devAddr = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_DEV_ADDR_POS]);
  this->rxDelays[0] = joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_RX_DELAY_POS]*1000;
  if(this->rxDelays[0] == 0) {
    this->rxDelays[0] = RADIOLIB_LORAWAN_RECEIVE_DELAY_1_MS;
  }
  this->rxDelays[1] = this->rxDelays[0] + 1000;

  // process CFlist if present
  if(lenRx == RADIOLIB_LORAWAN_JOIN_ACCEPT_MAX_LEN) {
    if(this->band->cfListType == RADIOLIB_LORAWAN_CFLIST_TYPE_FREQUENCIES) {
      // list of frequencies
      for(uint8_t i = 0; i < 5; i++) {
        uint32_t freq = LoRaWANNode::ntoh<uint32_t>(&joinAcceptMsg[RADIOLIB_LORAWAN_JOIN_ACCEPT_CFLIST_POS + 3*i], 3);
        availableChannelsFreq[i] = (float)freq/10000.0;
        RADIOLIB_DEBUG_PRINTLN("Channel %d frequency = %f MHz", i, availableChannelsFreq[i]);
      }

    } else {
      // TODO list of masks
      RADIOLIB_DEBUG_PRINTLN("CFlist masks not supported (yet)");
      return(RADIOLIB_ERR_UNSUPPORTED);

    }
  
  }

  // prepare buffer for key derivation
  uint8_t keyDerivationBuff[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  LoRaWANNode::hton<uint32_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_JOIN_NONCE_POS], joinNonce, 3);

  // check protocol version (1.0 vs 1.1)
  if(dlSettings & RADIOLIB_LORAWAN_JOIN_ACCEPT_R_1_1) {
    // 1.1 version, derive the keys
    LoRaWANNode::hton<uint64_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_JOIN_EUI_POS], joinEUI);
    LoRaWANNode::hton<uint16_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_DEV_NONCE_POS], devNonce);
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_APP_S_KEY;
    //Module::hexdump(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE);

    RadioLibAES128Instance.init(appKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->appSKey);
    //Module::hexdump(this->appSKey, RADIOLIB_AES128_BLOCK_SIZE);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_F_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->fNwkSIntKey);
    //Module::hexdump(this->fNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_S_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->sNwkSIntKey);
    //Module::hexdump(this->sNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_NWK_S_ENC_KEY;
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->nwkSEncKey);
    //Module::hexdump(this->nwkSEncKey, RADIOLIB_AES128_BLOCK_SIZE);

    // send the RekeyInd MAC command
    this->rev = 1;
    uint8_t serverRev = 0xFF;
    state = sendMacCommand(RADIOLIB_LORAWAN_MAC_CMD_REKEY_IND, &this->rev, sizeof(uint8_t), &serverRev, sizeof(uint8_t));
    RADIOLIB_ASSERT(state);

    // check the supported server version
    if(serverRev != this->rev) {
      return(RADIOLIB_ERR_INVALID_REVISION);
    }
  
  } else {
    // 1.0 version, just derive the keys
    this->rev = 0;
    LoRaWANNode::hton<uint32_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_HOME_NET_ID_POS], homeNetId, 3);
    LoRaWANNode::hton<uint16_t>(&keyDerivationBuff[RADIOLIB_LORAWAN_JOIN_ACCEPT_DEV_ADDR_POS], devNonce);
    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_APP_S_KEY;
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->appSKey);

    keyDerivationBuff[0] = RADIOLIB_LORAWAN_JOIN_ACCEPT_F_NWK_S_INT_KEY;
    RadioLibAES128Instance.init(nwkKey);
    RadioLibAES128Instance.encryptECB(keyDerivationBuff, RADIOLIB_AES128_BLOCK_SIZE, this->fNwkSIntKey);

    memcpy(this->sNwkSIntKey, this->fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
    memcpy(this->nwkSEncKey, this->fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  
  }

  // save the device address
  mod->hal->setPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_DEV_ADDR_ID, this->devAddr);

  // update the keys
  mod->hal->writePersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_APP_S_KEY_ID), this->appSKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->writePersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_FNWK_SINT_KEY_ID), this->fNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->writePersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_SNWK_SINT_KEY_ID), this->sNwkSIntKey, RADIOLIB_AES128_BLOCK_SIZE);
  mod->hal->writePersistentStorage(mod->hal->getPersistentAddr(RADIOLIB_PERSISTENT_PARAM_LORAWAN_NWK_SENC_KEY_ID), this->nwkSEncKey, RADIOLIB_AES128_BLOCK_SIZE);

  // all complete, reset device counters and set the magic number
  mod->hal->setPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_FCNT_UP_ID, 0);
  mod->hal->setPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_MAGIC_ID, RADIOLIB_LORAWAN_MAGIC);
  return(RADIOLIB_ERR_NONE);
}

int16_t LoRaWANNode::beginAPB(uint32_t addr, uint8_t* nwkSKey, uint8_t* appSKey, uint8_t* fNwkSIntKey, uint8_t* sNwkSIntKey) {
  this->devAddr = addr;
  memcpy(this->appSKey, appSKey, RADIOLIB_AES128_KEY_SIZE);
  memcpy(this->nwkSEncKey, nwkSKey, RADIOLIB_AES128_KEY_SIZE);
  if(fNwkSIntKey) {
    this->rev = 1;
    memcpy(this->fNwkSIntKey, fNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  } else {
    memcpy(this->fNwkSIntKey, nwkSKey, RADIOLIB_AES128_KEY_SIZE);
  }
  if(sNwkSIntKey) {
    memcpy(this->sNwkSIntKey, sNwkSIntKey, RADIOLIB_AES128_KEY_SIZE);
  }

  // set the physical layer configuration
  int16_t state = this->setPhyProperties();
  return(state);
}

#if defined(RADIOLIB_BUILD_ARDUINO)
int16_t LoRaWANNode::uplink(String& str, uint8_t port) {
  return(this->uplink(str.c_str(), port));
}
#endif

int16_t LoRaWANNode::uplink(const char* str, uint8_t port) {
  return(this->uplink((uint8_t*)str, strlen(str), port));
}

int16_t LoRaWANNode::uplink(uint8_t* data, size_t len, uint8_t port) {
  // check destination port
  if(port > 0xDF) {
    return(RADIOLIB_ERR_INVALID_PORT);
  }

  // check if there is a MAC command to piggyback
  uint8_t foptsLen = 0;
  if(this->command) {
    foptsLen = 1 + this->command->len;
  }

  // check maximum payload len as defined in phy
  if(len > this->band->payloadLenMax[this->dataRate]) {
    return(RADIOLIB_ERR_PACKET_TOO_LONG);
  }

  // check if sufficient time has elapsed since the last uplink
  Module* mod = this->phyLayer->getMod();
  if(mod->hal->millis() - this->rxDelayStart < rxDelays[1]) {
    // not enough time elapsed since the last uplink, we may still be in an RX window
    return(RADIOLIB_ERR_UPLINK_UNAVAILABLE);
  }

  // build the uplink message
  // the first 16 bytes are reserved for MIC calculation blocks
  size_t uplinkMsgLen = RADIOLIB_LORAWAN_FRAME_LEN(len, foptsLen);
  #if defined(RADIOLIB_STATIC_ONLY)
  uint8_t uplinkMsg[RADIOLIB_STATIC_ARRAY_SIZE];
  #else
  uint8_t* uplinkMsg = new uint8_t[uplinkMsgLen];
  #endif
  
  // set the packet fields
  uplinkMsg[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS] = RADIOLIB_LORAWAN_MHDR_MTYPE_UNCONF_DATA_UP | RADIOLIB_LORAWAN_MHDR_MAJOR_R1;
  LoRaWANNode::hton<uint32_t>(&uplinkMsg[RADIOLIB_LORAWAN_FHDR_DEV_ADDR_POS], this->devAddr);

  // TODO implement adaptive data rate
  uplinkMsg[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] = 0x00 | foptsLen;

  // get frame counter from persistent storage
  uint32_t fcnt = mod->hal->getPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_FCNT_UP_ID) + 1;
  mod->hal->setPersistentParameter<uint32_t>(RADIOLIB_PERSISTENT_PARAM_LORAWAN_FCNT_UP_ID, fcnt);
  LoRaWANNode::hton<uint16_t>(&uplinkMsg[RADIOLIB_LORAWAN_FHDR_FCNT_POS], (uint16_t)fcnt);

  // check if there is something in FOpts
  if(this->command) {
    // append MAC command
    uint8_t foptsBuff[RADIOLIB_AES128_BLOCK_SIZE];
    foptsBuff[0] = this->command->cid;
    for(size_t i = 1; i < this->command->len; i++) {
      foptsBuff[i] = this->command->payload[i];
    }

    // encrypt it
    processAES(foptsBuff, foptsLen, this->nwkSEncKey, &uplinkMsg[RADIOLIB_LORAWAN_FRAME_PAYLOAD_POS(0)], fcnt, RADIOLIB_LORAWAN_CHANNEL_DIR_UPLINK, 0x00, false);
  }

  // set the port
  uplinkMsg[RADIOLIB_LORAWAN_FHDR_FPORT_POS(foptsLen)] = port;

  // select encryption key based on the target port
  uint8_t* encKey = this->appSKey;
  if(port == RADIOLIB_LORAWAN_FPORT_MAC_COMMAND) {
    encKey = this->nwkSEncKey;
  }

  // encrypt the frame payload
  processAES(data, len, encKey, &uplinkMsg[RADIOLIB_LORAWAN_FRAME_PAYLOAD_POS(foptsLen)], fcnt, RADIOLIB_LORAWAN_CHANNEL_DIR_UPLINK, 0x00, true);

  // create blocks for MIC calculation
  uint8_t block0[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  block0[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_MIC_BLOCK_MAGIC;
  block0[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = RADIOLIB_LORAWAN_CHANNEL_DIR_UPLINK;
  LoRaWANNode::hton<uint32_t>(&block0[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], this->devAddr);
  LoRaWANNode::hton<uint32_t>(&block0[RADIOLIB_LORAWAN_BLOCK_FCNT_POS], fcnt);
  block0[RADIOLIB_LORAWAN_MIC_BLOCK_LEN_POS] = uplinkMsgLen - RADIOLIB_AES128_BLOCK_SIZE - sizeof(uint32_t);

  uint8_t block1[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  memcpy(block1, block0, RADIOLIB_AES128_BLOCK_SIZE);
  // TODO implement confirmed frames 
  block1[RADIOLIB_LORAWAN_MIC_DATA_RATE_POS] = this->dataRate;
  block1[RADIOLIB_LORAWAN_MIC_CH_INDEX_POS] = this->chIndex;

  //Module::hexdump(uplinkMsg, uplinkMsgLen);

  // calculate authentication codes
  memcpy(uplinkMsg, block1, RADIOLIB_AES128_BLOCK_SIZE);
  uint32_t micS = this->generateMIC(uplinkMsg, uplinkMsgLen - sizeof(uint32_t), this->sNwkSIntKey);
  memcpy(uplinkMsg, block0, RADIOLIB_AES128_BLOCK_SIZE);
  uint32_t micF = this->generateMIC(uplinkMsg, uplinkMsgLen - sizeof(uint32_t), this->fNwkSIntKey);

  // check LoRaWAN revision
  if(this->rev == 1) {
    uint32_t mic = ((uint32_t)(micF & 0x0000FF00) << 16) | ((uint32_t)(micF & 0x0000000FF) << 16) | ((uint32_t)(micS & 0x0000FF00) >> 0) | ((uint32_t)(micS & 0x0000000FF) >> 0);
    LoRaWANNode::hton<uint32_t>(&uplinkMsg[uplinkMsgLen - sizeof(uint32_t)], mic);
  } else {
    LoRaWANNode::hton<uint32_t>(&uplinkMsg[uplinkMsgLen - sizeof(uint32_t)], micF);
  }

  //Module::hexdump(uplinkMsg, uplinkMsgLen);

  // send it (without the MIC calculation blocks)
  uint32_t txStart = mod->hal->millis();
  uint32_t timeOnAir = this->phyLayer->getTimeOnAir(uplinkMsgLen - RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS) / 1000;
  int16_t state = this->phyLayer->transmit(&uplinkMsg[RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS], uplinkMsgLen - RADIOLIB_LORAWAN_FHDR_LEN_START_OFFS);
  #if !defined(RADIOLIB_STATIC_ONLY)
  delete[] uplinkMsg;
  #endif
  RADIOLIB_ASSERT(state);

  // set the timestamp so that we can measure when to start receiving
  this->command = NULL;
  this->rxDelayStart = txStart + timeOnAir;
  return(RADIOLIB_ERR_NONE);
}

#if defined(RADIOLIB_BUILD_ARDUINO)
int16_t LoRaWANNode::downlink(String& str) {
  int16_t state = RADIOLIB_ERR_NONE;

  // build a temporary buffer
  // LoRaWAN downlinks can have 250 bytes at most with 1 extra byte for NULL
  size_t length = 0;
  uint8_t data[251];

  // wait for downlink
  state = this->downlink(data, &length);
  if(state == RADIOLIB_ERR_NONE) {
    // add null terminator
    data[length] = '\0';

    // initialize Arduino String class
    str = String((char*)data);
  }

  return(state);
}
#endif

int16_t LoRaWANNode::downlink(uint8_t* data, size_t* len) {
  // check if there are any upcoming Rx windows
  Module* mod = this->phyLayer->getMod();
  const uint32_t scanGuard = 500;
  if(mod->hal->millis() - this->rxDelayStart > (this->rxDelays[1] + scanGuard)) {
    // time since last Tx is greater than RX2 delay + some guard period
    // we have nothing to downlink
    return(RADIOLIB_ERR_NO_RX_WINDOW);
  }

  // downlink messages are sent with inverted IQ
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  if(!this->FSK) {
    state = this->phyLayer->invertIQ(true);
    RADIOLIB_ASSERT(state);
  }

  // calculate the channel scanning timeout
  // according to the spec, this must be at least enough time to effectively detect a preamble
  uint32_t scanTimeout = this->phyLayer->getTimeOnAir(0)/1000;

  // set up everything for channel scan
  downlinkReceived = false;
  scanFlag = false;
  bool packetDetected = false;
  this->phyLayer->setChannelScanAction(LoRaWANNodeOnChannelScan);

  // perform listening in the two Rx windows
  for(uint8_t i = 0; i < 2; i++) {
    // wait for the start of the Rx window
    // the waiting duration is shortened a bit to cover any possible timing errors
    uint32_t waitLen = this->rxDelays[i] - (mod->hal->millis() - this->rxDelayStart);
    if(waitLen > scanGuard) {
      waitLen -= scanGuard;
    }
    mod->hal->delay(waitLen);

    // wait until we get a preamble
    uint32_t scanStart = mod->hal->millis();
    while((mod->hal->millis() - scanStart) < (scanTimeout + scanGuard)) {
      // check channel detection timeout
      state = this->phyLayer->startChannelScan();
      RADIOLIB_ASSERT(state);

      // wait with some timeout, though it should not be hit
      uint32_t cadStart = mod->hal->millis();
      while(!scanFlag) {
        mod->hal->yield();
        if(mod->hal->millis() - cadStart >= 3000) {
          // timed out, stop waiting
          break;
        }
      }

      // check the scan result
      scanFlag = false;
      state = this->phyLayer->getChannelScanResult();
      if((state == RADIOLIB_PREAMBLE_DETECTED) || (state == RADIOLIB_LORA_DETECTED)) {
        packetDetected = true;
        break;
      }
    
    }

    // check if we have a packet
    if(packetDetected) {
      break;

    } else if(i == 0) {
      // nothing in the first window, configure for the second
      state = this->phyLayer->setFrequency(this->band->backupChannel.freqStart);
      RADIOLIB_ASSERT(state);

      DataRate_t datr;
      findDataRate(RADIOLIB_LORAWAN_DATA_RATE_UNUSED, &datr, &this->band->backupChannel);
      state = this->phyLayer->setDataRate(datr);
      RADIOLIB_ASSERT(state);
    
    }
    
  }

  // check if we received a packet at all
  if(!packetDetected) {
    this->phyLayer->standby();
    if(!this->FSK) {
      this->phyLayer->invertIQ(false);
    }

    // restore the original uplink channel
    this->configureChannel(0, this->dataRate);

    return(RADIOLIB_ERR_RX_TIMEOUT);
  }

  // channel scan is finished, swap the actions
  this->phyLayer->clearChannelScanAction();
  downlinkReceived = false;
  this->phyLayer->setPacketReceivedAction(LoRaWANNodeOnDownlink);

  // start receiving
  state = this->phyLayer->startReceive();
  RADIOLIB_ASSERT(state);

  // wait for reception with some timeout
  uint32_t rxStart = mod->hal->millis();
  while(!downlinkReceived) {
    mod->hal->yield();
    // let's hope 30 seconds is long enough timeout
    if(mod->hal->millis() - rxStart >= 30000) {
      // timed out
      this->phyLayer->standby();
      if(!this->FSK) {
        this->phyLayer->invertIQ(false);
      }
      return(RADIOLIB_ERR_RX_TIMEOUT);
    }
  }

  // we have a message, clear actions, go to standby and reset the IQ inversion
  downlinkReceived = false;
  this->phyLayer->standby();
  this->phyLayer->clearPacketReceivedAction();
  if(!this->FSK) {
    state = this->phyLayer->invertIQ(false);
    RADIOLIB_ASSERT(state);
  }

  // get the packet length
  size_t downlinkMsgLen = this->phyLayer->getPacketLength();

  // check the minimum required frame length
  // an extra byte is subtracted because downlink frames may not have a port
  if(downlinkMsgLen < RADIOLIB_LORAWAN_FRAME_LEN(0, 0) - 1 - RADIOLIB_AES128_BLOCK_SIZE) {
    RADIOLIB_DEBUG_PRINTLN("Downlink message too short (%lu bytes)", downlinkMsgLen);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // build the buffer for the downlink message
  // the first 16 bytes are reserved for MIC calculation block
  #if !defined(RADIOLIB_STATIC_ONLY)
    uint8_t* downlinkMsg = new uint8_t[RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen];
  #else
    uint8_t downlinkMsg[RADIOLIB_STATIC_ARRAY_SIZE];
  #endif

  // set the MIC calculation block
  // TODO implement confirmed frames
  memset(downlinkMsg, 0x00, RADIOLIB_AES128_BLOCK_SIZE);
  downlinkMsg[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_MIC_BLOCK_MAGIC;
  LoRaWANNode::hton<uint32_t>(&downlinkMsg[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], this->devAddr);
  downlinkMsg[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = RADIOLIB_LORAWAN_CHANNEL_DIR_DOWNLINK;
  downlinkMsg[RADIOLIB_LORAWAN_MIC_BLOCK_LEN_POS] = downlinkMsgLen - sizeof(uint32_t);

  // read the data
  state = this->phyLayer->readData(&downlinkMsg[RADIOLIB_AES128_BLOCK_SIZE], downlinkMsgLen);
  // downlink frames are sent without CRC, which will raise error on SX127x
  // we can ignore that error
  if(state == RADIOLIB_ERR_LORA_HEADER_DAMAGED) {
    state = RADIOLIB_ERR_NONE;
  }

  //Module::hexdump(downlinkMsg, RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen);
  
  if(state != RADIOLIB_ERR_NONE) {
    #if !defined(RADIOLIB_STATIC_ONLY)
      delete[] downlinkMsg;
    #endif
    return(state);
  }

  // check the MIC
  if(!verifyMIC(downlinkMsg, RADIOLIB_AES128_BLOCK_SIZE + downlinkMsgLen, this->sNwkSIntKey)) {
    return(RADIOLIB_ERR_CRC_MISMATCH);
  }

  // check the address
  uint32_t addr = LoRaWANNode::ntoh<uint32_t>(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_DEV_ADDR_POS]);
  if(addr != this->devAddr) {
    RADIOLIB_DEBUG_PRINTLN("Device address mismatch, expected 0x%08X, got 0x%08X", this->devAddr, addr);
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // TODO cache the ADR bit?
  // TODO cache and check fcnt?
  uint16_t fcnt = LoRaWANNode::ntoh<uint16_t>(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCNT_POS]);

  // check fopts len
  uint8_t foptsLen = downlinkMsg[RADIOLIB_LORAWAN_FHDR_FCTRL_POS] & RADIOLIB_LORAWAN_FHDR_FOPTS_LEN_MASK;
  if(foptsLen > 0) {
    // there are some Fopts, decrypt them
    *len = foptsLen;

    // according to the specification, the last two arguments should be 0x00 and false,
    // but that will fail even for LoRaWAN 1.1.0 server
    processAES(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], foptsLen, this->nwkSEncKey, data, fcnt, RADIOLIB_LORAWAN_CHANNEL_DIR_DOWNLINK, 0x01, true);

    #if !defined(RADIOLIB_STATIC_ONLY)
      delete[] downlinkMsg;
    #endif
    return(RADIOLIB_ERR_NONE);
  }

  // no fopts, just payload
  // TODO implement decoding piggybacked Fopts?
  *len = downlinkMsgLen;
  processAES(&downlinkMsg[RADIOLIB_LORAWAN_FHDR_FOPTS_POS], downlinkMsgLen, this->appSKey, data, fcnt, RADIOLIB_LORAWAN_CHANNEL_DIR_DOWNLINK, 0x00, true);
  
  #if !defined(RADIOLIB_STATIC_ONLY)
    delete[] downlinkMsg;
  #endif

  return(state);
}

void LoRaWANNode::findDataRate(uint8_t dr, DataRate_t* datr, const LoRaWANChannelSpan_t* span) {
  uint8_t dataRateBand = span->dataRates[dr];
  this->dataRate = dr;
  if(dr == RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
    for(uint8_t i = 0; i < RADIOLIB_LORAWAN_CHANNEL_NUM_DATARATES; i++) {
      if(span->dataRates[i] != RADIOLIB_LORAWAN_DATA_RATE_UNUSED) {
        dataRateBand = span->dataRates[i];
        this->dataRate = i;
        break;
      }
    }
  }

  if(dataRateBand & RADIOLIB_LORAWAN_DATA_RATE_FSK_50_K) {
    datr->fsk.bitRate = 50;
    datr->fsk.freqDev = 25;
  
  } else {
    uint8_t bw = dataRateBand & 0x03;
    switch(bw) {
      case(RADIOLIB_LORAWAN_DATA_RATE_BW_125_KHZ):
        datr->lora.bandwidth = 125.0;
        break;
      case(RADIOLIB_LORAWAN_DATA_RATE_BW_250_KHZ):
        datr->lora.bandwidth = 250.0;
        break;
      case(RADIOLIB_LORAWAN_DATA_RATE_BW_500_KHZ):
        datr->lora.bandwidth = 500.0;
        break;
      default:
        datr->lora.bandwidth = 125.0;
    }
    
    datr->lora.spreadingFactor = ((dataRateBand & 0x70) >> 4) + 6;
  
  }

}

int16_t LoRaWANNode::configureChannel(uint8_t chan, uint8_t dr) {
  // find the span based on the channel ID
  uint8_t span = 0;
  uint8_t spanChannelId = 0;
  bool found = false;
  for(uint8_t chanCtr = 0; span < this->band->numChannelSpans; span++) {
    for(; spanChannelId < this->band->defaultChannels[span].numChannels; spanChannelId++) {
      if(chanCtr >= chan) {
        found = true;
        break;
      }
      chanCtr++;
    }
    if(found) {
      break;
    }
  }

  if(!found) {
    return(RADIOLIB_ERR_INVALID_CHANNEL);
  }

  this->chIndex = chan;

  // set the frequency
  float freq = this->band->defaultChannels[span].freqStart + this->band->defaultChannels[span].freqStep * (float)spanChannelId;
  int state = this->phyLayer->setFrequency(freq);
  RADIOLIB_ASSERT(state);

  // set the data rate
  DataRate_t datr;
  findDataRate(dr, &datr, &this->band->defaultChannels[span]);
  state = this->phyLayer->setDataRate(datr);

  return(state);
}

uint32_t LoRaWANNode::generateMIC(uint8_t* msg, size_t len, uint8_t* key) {
  if((msg == NULL) || (len == 0)) {
    return(0);
  }

  RadioLibAES128Instance.init(key);
  uint8_t cmac[RADIOLIB_AES128_BLOCK_SIZE];
  RadioLibAES128Instance.generateCMAC(msg, len, cmac);
  return(((uint32_t)cmac[0]) | ((uint32_t)cmac[1] << 8) | ((uint32_t)cmac[2] << 16) | ((uint32_t)cmac[3]) << 24);
}

bool LoRaWANNode::verifyMIC(uint8_t* msg, size_t len, uint8_t* key) {
  if((msg == NULL) || (len < sizeof(uint32_t))) {
    return(0);
  }

  // extract MIC from the message
  uint32_t micReceived = LoRaWANNode::ntoh<uint32_t>(&msg[len - sizeof(uint32_t)]);

  // calculate the expected value and compare
  uint32_t micCalculated = generateMIC(msg, len - sizeof(uint32_t), key);
  if(micCalculated != micReceived) {
    return(false);
  }

  return(true);
}

int16_t LoRaWANNode::setPhyProperties() {
  // set the physical layer configuration
  // TODO select channel span based on channel ID
  // TODO select channel randomly
  uint8_t channelId = 0;
  int16_t state = RADIOLIB_ERR_NONE;
  if(this->FSK) {
    state = this->phyLayer->setFrequency(this->band->fskFreq);
    RADIOLIB_ASSERT(state);
    DataRate_t dr;
    dr.fsk.bitRate = 50;
    dr.fsk.freqDev = 25;
    state = this->phyLayer->setDataRate(dr);
    RADIOLIB_ASSERT(state);
    state = this->phyLayer->setDataShaping(RADIOLIB_SHAPING_1_0);
    RADIOLIB_ASSERT(state);
    state = this->phyLayer->setEncoding(RADIOLIB_ENCODING_WHITENING);
  } else {
    state = this->configureChannel(channelId, this->band->defaultChannels[0].joinRequestDataRate);
  }
  RADIOLIB_ASSERT(state);

  state = this->phyLayer->setOutputPower(this->band->powerMax);
  RADIOLIB_ASSERT(state);

  uint8_t syncWord[3] = { 0 };
  uint8_t syncWordLen = 0;
  size_t preLen = 0;
  if(this->FSK) {
    preLen = 8*RADIOLIB_LORAWAN_GFSK_PREAMBLE_LEN;
    syncWord[0] = (uint8_t)(RADIOLIB_LORAWAN_GFSK_SYNC_WORD >> 16);
    syncWord[1] = (uint8_t)(RADIOLIB_LORAWAN_GFSK_SYNC_WORD >> 8);
    syncWord[2] = (uint8_t)RADIOLIB_LORAWAN_GFSK_SYNC_WORD;
    syncWordLen = 3;
  
  } else {
    preLen = RADIOLIB_LORAWAN_LORA_PREAMBLE_LEN;
    syncWord[0] = RADIOLIB_LORAWAN_LORA_SYNC_WORD;
    syncWordLen = 1;
  
  }

  state = this->phyLayer->setSyncWord(syncWord, syncWordLen);
  RADIOLIB_ASSERT(state);

  state = this->phyLayer->setPreambleLength(preLen);
  return(state);
}

int16_t LoRaWANNode::sendMacCommand(uint8_t cid, uint8_t* payload, size_t payloadLen, uint8_t* reply, size_t replyLen) {
  // build the command
  size_t macReqLen = 1 + payloadLen;
  #if !defined(RADIOLIB_STATIC_ONLY)
    uint8_t* macReqBuff = new uint8_t[macReqLen];
  #else
    uint8_t macReqBuff[RADIOLIB_STATIC_ARRAY_SIZE];
  #endif
  macReqBuff[0] = cid;
  memcpy(&macReqBuff[1], payload, payloadLen);

  // uplink it
  int16_t state = this->uplink(macReqBuff, macReqLen, RADIOLIB_LORAWAN_FPORT_MAC_COMMAND);
  #if !defined(RADIOLIB_STATIC_ONLY)
    delete[] macReqBuff;
  #endif
  RADIOLIB_ASSERT(state);

  // build the reply buffer
  size_t macRplLen = 1 + replyLen;
  #if !defined(RADIOLIB_STATIC_ONLY)
    uint8_t* macRplBuff = new uint8_t[this->band->payloadLenMax[this->dataRate]];
  #else
    uint8_t macRplBuff[RADIOLIB_STATIC_ARRAY_SIZE];
  #endif

  // wait for reply from the server
  size_t rxRplLen = 0;
  state = this->downlink(macRplBuff, &rxRplLen);
  if(state != RADIOLIB_ERR_NONE) {
    #if !defined(RADIOLIB_STATIC_ONLY)
      delete[] macRplBuff;
    #endif
    return(state);
  }

  //Module::hexdump(macRplBuff, rxRplLen);

  // check the length - it may be longer than expected
  // if the server decided to append more MAC commands, but never shorter
  // TODO how to handle the additional command(s)?
  if(rxRplLen < macRplLen) {
    #if !defined(RADIOLIB_STATIC_ONLY)
      delete[] macRplBuff;
    #endif
    return(RADIOLIB_ERR_DOWNLINK_MALFORMED);
  }

  // check the CID
  if(macRplBuff[0] != cid) {
    #if !defined(RADIOLIB_STATIC_ONLY)
      delete[] macRplBuff;
    #endif
    return(RADIOLIB_ERR_INVALID_CID);
  }

  // copy the data
  memcpy(reply, &macRplBuff[1], replyLen);
  #if !defined(RADIOLIB_STATIC_ONLY)
    delete[] macRplBuff;
  #endif

  return(state);
}

void LoRaWANNode::processAES(uint8_t* in, size_t len, uint8_t* key, uint8_t* out, uint32_t fcnt, uint8_t dir, uint8_t ctrId, bool counter) {
  // figure out how many encryption blocks are there
  size_t numBlocks = len/RADIOLIB_AES128_BLOCK_SIZE;
  if(len % RADIOLIB_AES128_BLOCK_SIZE) {
    numBlocks++;
  }

  // generate the encryption blocks
  uint8_t encBuffer[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  uint8_t encBlock[RADIOLIB_AES128_BLOCK_SIZE] = { 0 };
  encBlock[RADIOLIB_LORAWAN_BLOCK_MAGIC_POS] = RADIOLIB_LORAWAN_ENC_BLOCK_MAGIC;
  encBlock[RADIOLIB_LORAWAN_ENC_BLOCK_COUNTER_ID_POS] = ctrId;
  encBlock[RADIOLIB_LORAWAN_BLOCK_DIR_POS] = dir;
  LoRaWANNode::hton<uint32_t>(&encBlock[RADIOLIB_LORAWAN_BLOCK_DEV_ADDR_POS], this->devAddr);
  LoRaWANNode::hton<uint32_t>(&encBlock[RADIOLIB_LORAWAN_BLOCK_FCNT_POS], fcnt);

  //Module::hexdump(uplinkMsg, uplinkMsgLen);

  // now encrypt the input
  // on downlink frames, this has a decryption effect because server actually "decrypts" the plaintext
  size_t remLen = len;
  for(size_t i = 0; i < numBlocks; i++) {
    if(counter) {
      encBlock[RADIOLIB_LORAWAN_ENC_BLOCK_COUNTER_POS] = i + 1;
    }

    // encrypt the buffer
    RadioLibAES128Instance.init(key);
    RadioLibAES128Instance.encryptECB(encBlock, RADIOLIB_AES128_BLOCK_SIZE, encBuffer);

    // now xor the buffer with the input
    size_t xorLen = remLen;
    if(xorLen > RADIOLIB_AES128_BLOCK_SIZE) {
      xorLen = RADIOLIB_AES128_BLOCK_SIZE;
    }
    for(uint8_t j = 0; j < xorLen; j++) {
      out[i*RADIOLIB_AES128_BLOCK_SIZE + j] = in[i*RADIOLIB_AES128_BLOCK_SIZE + j] ^ encBuffer[j];
    }
    remLen -= xorLen;
  }
}

template<typename T>
T LoRaWANNode::ntoh(uint8_t* buff, size_t size) {
  uint8_t* buffPtr = buff;
  size_t targetSize = sizeof(T);
  if(size != 0) {
    targetSize = size;
  }
  T res = 0;
  for(size_t i = 0; i < targetSize; i++) {
    res |= (uint32_t)(*(buffPtr++)) << 8*i;
  }
  return(res);
}

template<typename T>
void LoRaWANNode::hton(uint8_t* buff, T val, size_t size) {
  uint8_t* buffPtr = buff;
  size_t targetSize = sizeof(T);
  if(size != 0) {
    targetSize = size;
  }
  for(size_t i = 0; i < targetSize; i++) {
    *(buffPtr++) = val >> 8*i;
  }
}

#endif
