/*
 * Can.cpp
 *
 *  Created on: 17 Sep 2018
 *      Author: David
 */

#include "CanInterface.h"

#include <CanSettings.h>
#include <CanMessageFormats.h>
#include <CanMessageBuffer.h>

#define SUPPORT_CAN		1		// needed by CanDriver.h
#include <CanDriver.h>
#include <hpl_user_area.h>

#if SAME5x
# include <hri_mclk_e54.h>
#elif SAMC21
# include <hri_mclk_c21.h>
# include <hri_gclk_c21.h>
#endif

const unsigned int NumCanBuffers = 4;

static CanUserAreaData canConfigData;
static CanAddress boardAddress;

class CanMessageQueue
{
public:
	CanMessageQueue();
	void AddMessage(CanMessageBuffer *buf);
	CanMessageBuffer *GetMessage();

private:
	CanMessageBuffer *pendingMessages;
	CanMessageBuffer *lastPendingMessage;			// only valid when pendingMessages != nullptr
};

CanMessageQueue::CanMessageQueue() : pendingMessages(nullptr) { }

void CanMessageQueue::AddMessage(CanMessageBuffer *buf)
{
	buf->next = nullptr;
	{
		if (pendingMessages == nullptr)
		{
			pendingMessages = lastPendingMessage = buf;
		}
		else
		{
			lastPendingMessage->next = buf;
		}
	}
}

// Fetch a message from the queue, or return nullptr if there are no messages
CanMessageBuffer *CanMessageQueue::GetMessage()
{
	CanMessageBuffer *buf;
	{
		buf = pendingMessages;
		if (buf != nullptr)
		{
			pendingMessages = buf->next;
		}
	}
	return buf;
}

static CanMessageQueue PendingMoves;
static CanMessageQueue PendingCommands;

static struct can_async_descriptor CAN_0 = { 0 };

#if SAME5x

/**
 * \brief CAN initialization function
 *
 * Enables CAN peripheral, clocks and initializes CAN driver
 */
void CAN_0_init(const CanTiming& timing)
{
	hri_mclk_set_AHBMASK_CAN1_bit(MCLK);
	hri_gclk_write_PCHCTRL_reg(GCLK, CAN1_GCLK_ID, GclkNum48MHz | GCLK_PCHCTRL_CHEN);
	can_async_init(&CAN_0, CAN1, timing);
	SetPinFunction(PortBPin(13), GpioPinFunction::H);
	SetPinFunction(PortBPin(12), GpioPinFunction::H);
}

#endif

#if SAMC21

/**
 * \brief CAN initialization function
 *
 * Enables CAN peripheral, clocks and initializes CAN driver
 */
static void CAN_0_init(const CanTiming& timing)
{
	hri_mclk_set_AHBMASK_CAN0_bit(MCLK);
	hri_gclk_write_PCHCTRL_reg(GCLK, CAN0_GCLK_ID, GclkNum48MHz | GCLK_PCHCTRL_CHEN);
	can_async_init(&CAN_0, CAN0, timing);
#ifdef SAMMYC21
	SetPinFunction(PortBPin(23), GpioPinFunction::G);
	SetPinFunction(PortBPin(22), GpioPinFunction::G);
	pinMode(CanStandbyPin, OUTPUT_LOW);					// take the CAN drivers out of standby
#else
	SetPinFunction(PortAPin(25), GpioPinFunction::G);
	SetPinFunction(PortAPin(24), GpioPinFunction::G);
#endif
}

#endif

// Get a received CAN message if there is one
bool CanInterface::GetCanMessage(CanMessageBuffer *buf)
{
	can_message msg;										// descriptor for the message
	msg.data = reinterpret_cast<uint8_t*>(&(buf->msg));		// set up where we want the message data to be stored
	const int32_t rslt = can_async_read(&CAN_0, &msg);		// fetch the message
	if (rslt == ERR_NONE)
	{
		buf->dataLength = msg.len;
		buf->id.SetReceivedId(msg.id);
		return true;
	}
	return false;
}

void CanInterface::Init(CanAddress defaultBoardAddress, bool doHardwareReset)
{
	// Read the CAN timing data from the top part of the NVM User Row
#if SAME5x
	const uint32_t CanUserAreaDataOffset = 512 - sizeof(CanUserAreaData);
#elif SAMC21
	const uint32_t CanUserAreaDataOffset = 256 - sizeof(CanUserAreaData);
#endif

	canConfigData = *reinterpret_cast<CanUserAreaData*>(NVMCTRL_USER + CanUserAreaDataOffset);

	if (doHardwareReset)
	{
		canConfigData.Clear();
		_user_area_write(reinterpret_cast<void*>(NVMCTRL_USER), CanUserAreaDataOffset, reinterpret_cast<const uint8_t*>(&canConfigData), sizeof(canConfigData));
	}
	CanTiming timing;
	canConfigData.GetTiming(timing);

	// Initialise the CAN hardware, using the timing data if it was valid
	CAN_0_init(timing);

	boardAddress = canConfigData.GetCanAddress(defaultBoardAddress);
	CanMessageBuffer::Init(NumCanBuffers);

	// Set up CAN receiver filtering
	can_filter filter;

	// First a filter for our own ID
	filter.id = (uint32_t)boardAddress << CanId::DstAddressShift;
	filter.mask = CanId::BoardAddressMask << CanId::DstAddressShift;
	can_async_set_filter(&CAN_0, 0, CAN_FMT_EXTID, &filter);

	// We ignore broadcast messages so no need to set up a filter for them

	can_async_enable(&CAN_0);
}

void CanInterface::Disable()
{
	can_async_disable(&CAN_0);			// disable CAN to prevent it receiving packets into RAM
}

CanAddress CanInterface::GetCanAddress()
{
	return boardAddress;
}

// Send a CAN message and free the buffer
void CanInterface::Send(CanMessageBuffer *buf)
{
	can_message msg;
	msg.id = buf->id.GetWholeId();
	msg.type = CAN_TYPE_DATA;
	msg.data = buf->msg.raw;
	msg.len = buf->dataLength;
	msg.fmt = CAN_FMT_EXTID;
	int32_t err;
	do
	{
		err = can_async_write(&CAN_0, &msg);
	} while (err != ERR_NONE);
	CanMessageBuffer::Free(buf);
}

// End
