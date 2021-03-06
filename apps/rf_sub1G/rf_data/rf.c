#include "rf.h"

uint32_t count = 0;
header msgHeader;
sendJob jobs[QUEUE_SIZE];
messageInfo infos[QUEUE_SIZE];
uint8_t tx_data[BUFF_LEN];

uint8_t rxBuff[MESSAGE_BUFF_LEN];
messageInfo rxMsgInfo;
messageInfo txMsgInfo;
messageInfo rqMsgInfo;
uint32_t last_send_tick = 0;

rqInfo rxRqInfo;
rqInfo txRqInfo;

uint8_t txBuff[512];

uint8_t rf_specific_settings[] = {
	CC1101_REGS(gdo_config[2]), 0x07,   // GDO_0 - Assert on CRC OK | Disable temp sensor
	CC1101_REGS(gdo_config[0]), 0x2E,   // GDO_2 - FIXME : do something usefull with it for tests
	CC1101_REGS(pkt_ctrl[0]), 0x0F,     // Accept all sync, CRC err auto flush, Append, Addr check and Bcast
};

const struct pio cc1101_cs_pin = LPC_GPIO_0_15;
const struct pio cc1101_miso_pin = LPC_SSP0_MISO_PIO_0_16;
const struct pio cc1101_gdo0 = LPC_GPIO_0_6;
const struct pio cc1101_gdo2 = LPC_GPIO_0_7;

volatile uint8_t check_rx = 0;
volatile uint8_t rx_done = 0;


// Calback when reciving a message via RF
void rf_rx_calback(uint32_t gpio)
{
	check_rx = 1;
}

// Calculates the CRC of a char array
uint8_t rc_crc8(uint8_t *data, size_t len)
{
	uint8_t crc = 0xff;
    size_t i, j;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if ((crc & 0x80) != 0)
                crc = (uint8_t)((crc << 1) ^ 0x31);
            else
                crc <<= 1;
        }
    }
    return crc;
}

void radio_stuff(){

	uint8_t status = 0;

	uint32_t tick = systick_get_tick_count();

	if(rxMsgInfo.addr != 0 && (tick-rxMsgInfo.tick) > WAIT_RX_TIME*(rxMsgInfo.asked+1)){
#if DEBUG > 0
		uprintf(UART0,"Waiting for this packets from %x with count %d:\n\r",rxMsgInfo.addr,rxMsgInfo.count);
#endif
		uint8_t i;
		for(i=0;i<=rxMsgInfo.nbpacket;i++){
#if DEBUG > 0
			if(!((rxMsgInfo.rxpack >> i) & 1)) uprintf(UART0,"%d ",(i+1));
#endif
		}
#if DEBUG > 0
		uprintf(UART0,"\n\r");
#endif
		rxMsgInfo.asked ++;

		if(rxMsgInfo.asked>MAX_RQ_ASK){
#if DEBUG > 0
		uprintf(UART0,"Asked too many times, ending packet\n\r");
#endif
		rxMsgInfo.addr = 0;
		rxMsgInfo.rxpack = 0;
		rxMsgInfo.asked = 0;
		} else {
#if DEBUG > 0
			uprintf(UART0,"Asking for resend (%d)\n\r",rxMsgInfo.asked);
#endif

			rqMsgInfo.count = count;
			rqMsgInfo.tick = systick_get_tick_count();
			rqMsgInfo.addr = rxMsgInfo.addr;
			rqMsgInfo.nbpacket = 0;
			rqMsgInfo.lastPacketLen = 4;
			rqMsgInfo.msgType = EVERYTYPE|RQ_DATA;
			rqMsgInfo.nbPayloadLast = 1;

			txRqInfo.count = rxMsgInfo.count;
			txRqInfo.addr = rxMsgInfo.addr;
			txRqInfo.rxpack = rxMsgInfo.rxpack;

			count ++;

			addRqJob();
		}
	}

	if(txMsgInfo.addr != 0 && (tick-txMsgInfo.tick) > WAIT_RX_TIME*2){
		txMsgInfo.addr = 0;
		txMsgInfo.tick = 0;
	}

	do	// Do not leave radio in an unknown or unwated state
	{
		status = (cc1101_read_status() & CC1101_STATE_MASK);
	} while (status == CC1101_STATE_TX);
	if (status != CC1101_STATE_RX)
	{
		static uint8_t loop = 0;
		loop++;
		if (loop > 10)
		{
			if (cc1101_rx_fifo_state() != 0)
			{
				cc1101_flush_rx_fifo();
			}
			cc1101_enter_rx_mode();
			loop = 0;
		}
	}
}

// Check a recieved packet if it need to be interpreted
uint8_t checkPacket(uint8_t *data)
{

	header mHeader;
	memcpy(&mHeader,data,sizeof(header));
	uint32_t rxCount;
	memcpy(&rxCount,data+sizeof(header),sizeof(rxCount));
	uint8_t rxCrc,calcCrc;
	memcpy(&rxCrc,data+sizeof(header)+sizeof(rxCount),sizeof(rxCrc));
	calcCrc = rc_crc8(data+sizeof(header)+sizeof(rxCount)+sizeof(rxCrc),PAYLOAD_BLOC_LEN*(mHeader.mtype_nbpay&0x3));

#if DEBUG > 1
	uprintf(UART0, "Check packet:\n\r - destination: %x\n\r - source: %x\n\r - net id: %x\n\r - mtype: %d\n\r",
		mHeader.dest,
		mHeader.src,
		mHeader.netid,
		mHeader.mtype_nbpay >> 2
	);
	uprintf(UART0, " - nb blocs: %d\n\r - count: %d\n\r - nb packet: %d\n\r - id packet: %d\n\r",
		mHeader.mtype_nbpay & 0x03,
		rxCount,
		mHeader.nbpacket,
		mHeader.idpacket
	);
	uprintf(UART0, " - CRC rx: %02x - CRC calc: %02x\n\r",rxCrc,calcCrc);
#endif

	if(rxCrc != calcCrc) //Check CRC
		return 0;

	// Check Source & destination
	if (mHeader.dest != DEVICE_ADDRESS && mHeader.dest != BROADCAST) // Checks if it is the device's address
		return 0;
	if (mHeader.src != LINKED_ADDRESS) // Checks if it is the neighbour's address
		return 0;

	if (mHeader.netid != NETID) // Check NetworkID
		return 0;

	return 1;
}

// RF config
void rf_config(void)
{
	config_gpio(&cc1101_gdo0, LPC_IO_MODE_PULL_UP, GPIO_DIR_IN, 0);
	cc1101_init(0, &cc1101_cs_pin, &cc1101_miso_pin); // ssp_num, cs_pin, miso_pin
	cc1101_config();
	cc1101_update_config(rf_specific_settings, sizeof(rf_specific_settings));
	set_gpio_callback(rf_rx_calback, &cc1101_gdo0, EDGE_RISING);
	cc1101_set_address(DEVICE_ADDRESS);
#if DEBUG > 0
	uprintf(UART0, "CC1101 RF link init done.\n\r");
#endif

	rxMsgInfo.addr = 0;
	initJobs();

}

void handle_rf_rx_data()
{
	uint8_t data[RF_BUFF_LEN];
	uint8_t status = 0;

	// Check for received packet (and get it if any)
	uint8_t ret = 0;
	ret = cc1101_receive_packet(data, RF_BUFF_LEN, &status);
	
#if DEBUG > 1	
	uprintf(UART0, "Message recived (%d)\n\r",ret);
#endif

	if(ret == 0){
		rf_config();
		check_rx = 0;
		return;
	}

	// Go back to RX mode
	cc1101_enter_rx_mode();

	if (checkPacket(data)) // Checks packet
	{

		header mHeader;
		memcpy(&mHeader,data,sizeof(header));
		uint32_t rxCount;
		memcpy(&rxCount,data+sizeof(header),sizeof(rxCount));

#if DEBUG > 0
		uint8_t payloadLen = (mHeader.mtype_nbpay & 0x03) * PAYLOAD_BLOC_LEN;
		uprintf(UART0,"Packet Checked from %x, count: %d, payload is %d bytes long, nb %d of %d:\n\r",mHeader.src,rxCount,payloadLen,(mHeader.idpacket+1),(mHeader.nbpacket+1));
#endif

		if(((mHeader.mtype_nbpay>>2)&0xf) == RQ_DATA){
#if DEBUG > 1
			uprintf(UART0,"RQ_DATA\n\r");
#endif

			memcpy(&rxRqInfo,data+sizeof(header)+sizeof(rxCount),sizeof(rxRqInfo));

			uint8_t i;

			for(i=0;i<=txMsgInfo.nbpacket;i++){
				if(!(rxRqInfo.rxpack>>i)&1){
					addJob(txBuff+i*MAX_NB_BLOCK*PAYLOAD_BLOC_LEN,i);
				}
			}

		} else if(((mHeader.mtype_nbpay>>2)&0xf) == TERMINATION){
#if DEBUG > 1
			uprintf(UART0,"TERMINATION\n\r");
#endif


		} else if(((mHeader.mtype_nbpay>>2)&0xf) == FIRE_MNGMT){
#if DEBUG > 1
			uprintf(UART0,"FIRE_MNGMT\n\r");
#endif

			memcpy(rxBuff+mHeader.idpacket*MAX_NB_BLOCK*PAYLOAD_BLOC_LEN,data+sizeof(header)+sizeof(rxCount)+sizeof(uint8_t),(mHeader.mtype_nbpay&0x03)*PAYLOAD_BLOC_LEN);

			if(rxMsgInfo.addr == 0){

				rxMsgInfo.count = rxCount;
				rxMsgInfo.addr = mHeader.src;
				rxMsgInfo.nbpacket = mHeader.nbpacket;

			}
			
			if(rxMsgInfo.addr == mHeader.src && rxMsgInfo.count == rxCount){

				rxMsgInfo.rxpack = rxMsgInfo.rxpack | (1 << mHeader.idpacket);
				rxMsgInfo.tick = systick_get_tick_count();

			}

			uint8_t messageDone = 1;

			uint8_t i;
			for(i=0;i<=rxMsgInfo.nbpacket;i++){
				if(!((rxMsgInfo.rxpack >> i) & 1)){
					messageDone = 0;
#if DEBUG > 1
					uprintf(UART0,"Miss packet %d\n\r",(i+1));
#endif
				}
			}

			if(messageDone){
				rxMsgInfo.addr = 0;
				rxMsgInfo.rxpack = 0;
				rxMsgInfo.asked = 0;
				msgHeader = mHeader;
			}

			rx_done = messageDone;
		} else {
#if DEBUG > 1
			uprintf(UART0,"Unidentified\n\r");
#endif
		}

	}
}

void send_message(uint8_t *payload, uint16_t nbPayload, uint16_t messageLen, uint8_t destination, uint8_t msgType)
{
	uint8_t nbPacketToSend = nbPayload / MAX_NB_BLOCK;
	uint8_t nbBlocLast = nbPayload % MAX_NB_BLOCK + 1;
	if(nbBlocLast == 0) nbBlocLast = MAX_NB_BLOCK;

	memcpy(txBuff,payload,MESSAGE_BUFF_LEN);

	txMsgInfo.count = count;
	txMsgInfo.tick = systick_get_tick_count();
	txMsgInfo.addr = destination;
	txMsgInfo.nbpacket = nbPacketToSend;
	txMsgInfo.lastPacketLen = messageLen % (PAYLOAD_BLOC_LEN*MAX_NB_BLOCK);
	txMsgInfo.msgType = msgType;
	txMsgInfo.nbPayloadLast = nbBlocLast;

	uint8_t i;
	for(i=0;i<=nbPacketToSend;i++){
		addJob(txBuff+i*MAX_NB_BLOCK*PAYLOAD_BLOC_LEN,i);
	}
	count++;
}

void initJobs(){
	uint8_t i;
	for(i=0;i<QUEUE_SIZE;i++){
		jobs[i].data = NULL;
	}
}

uint8_t execJob(){

	if(jobs[0].data == NULL)
		return 0;

	uint32_t t = systick_get_tick_count();

	if((t-last_send_tick)<SEND_TIMEOUT)
		return 0;

#if DEBUG > 0
	uprintf(UART0,"Executing Job (Rq) to %x (%d on %d) with type: %02x\n\r",infos[0].addr,jobs[0].idpacket,infos[0].nbpacket,infos[0].msgType);
#endif
	
	send_on_rf();
	last_send_tick = systick_get_tick_count();

	uint8_t i = 0;
	while(jobs[i+1].data != NULL && i < QUEUE_SIZE){
		infos[i] = infos[i+1];
		jobs[i].data = jobs[i+1].data;
		jobs[i].idpacket = jobs[i+1].idpacket;
		i++;
	}

	jobs[i].data = NULL;

	return 1;
}

uint8_t addJob(uint8_t* data, uint8_t idpacket){

	uint8_t i = 0;
	while(jobs[i].data != NULL && i < QUEUE_SIZE){
		i++;
	}

	if(i>=QUEUE_SIZE)
		return 0;

	infos[i] = txMsgInfo;
	jobs[i].data = data;
	jobs[i].idpacket = idpacket;

#if DEBUG > 0
	uprintf(UART0,"Added Job to %x (%d on %d)\n\r",i,infos[i].addr,jobs[i].idpacket,infos[i].nbpacket);
#endif

	return (i+1);
}

uint8_t addRqJob(){

	uint8_t i = 0;
	while(jobs[i].data != NULL && i < QUEUE_SIZE){
		i++;
	}

	if(i>=QUEUE_SIZE)
		return 0;

	uint8_t d[16];
	memcpy(d,&txRqInfo,sizeof(txRqInfo));

	infos[i] = rqMsgInfo;
	jobs[i].data = d;
	jobs[i].idpacket = 0;

#if DEBUG > 0
	uprintf(UART0,"Added Job (Rq) to %x (%d on %d)\n\r",infos[i].addr,jobs[i].idpacket,infos[i].nbpacket);
#endif

	return (i+1);
}

void send_on_rf(){

#if DEBUG > 0	
	uprintf(UART0, "Sending");
#endif

	header mHeader;
	mHeader.dest = infos[0].addr;
	mHeader.src = DEVICE_ADDRESS;
	mHeader.netid = NETID;
	mHeader.lastPacketLen = infos[0].lastPacketLen;
	mHeader.nbpacket = infos[0].nbpacket;
	mHeader.idpacket = jobs[0].idpacket;

#if DEBUG > 0	
	uprintf(UART0, ".");
#endif

	if(jobs[0].idpacket<infos[0].nbpacket){
		mHeader.mtype_nbpay = infos[0].msgType << 2 | MAX_NB_BLOCK;
	} else {
		mHeader.mtype_nbpay = infos[0].msgType << 2 | infos[0].nbPayloadLast;
	}

#if DEBUG > 0	
	uprintf(UART0, ".");
#endif

	mHeader.packetLen = sizeof(header)+sizeof(count)+1+((mHeader.mtype_nbpay&0x3)*PAYLOAD_BLOC_LEN);

	memcpy(tx_data,&mHeader,sizeof(header));
	memcpy(tx_data+sizeof(header),&(infos[0].count),sizeof(infos[0].count));

#if DEBUG > 0	
	uprintf(UART0, ".");
#endif

	uint8_t crc;
	memcpy(tx_data+sizeof(header)+sizeof(count)+sizeof(crc),jobs[0].data,PAYLOAD_BLOC_LEN*(mHeader.mtype_nbpay & 0x3));
	crc = rc_crc8(tx_data+sizeof(header)+sizeof(count)+sizeof(crc),PAYLOAD_BLOC_LEN*(mHeader.mtype_nbpay&0x3));
	memcpy(tx_data+sizeof(header)+sizeof(count),&crc,sizeof(crc));

#if DEBUG > 0	
	uprintf(UART0, ".");
#endif

	// Send   
	if (cc1101_tx_fifo_state() != 0)
	{
		cc1101_flush_tx_fifo();
	}

#if DEBUG > 0	
	uprintf(UART0, ". ");
#endif

#if DEBUG > 0	
	int ret = cc1101_send_packet(tx_data, mHeader.packetLen+1);
	uprintf(UART0, "Message sent: %d (%x to %x)\n\r",ret,mHeader.src,mHeader.dest);
#else
	cc1101_send_packet(tx_data, mHeader.packetLen+1);
#endif

}