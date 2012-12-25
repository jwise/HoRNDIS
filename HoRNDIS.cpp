/* HoRNDIS.cpp
 * Implementation of IOKit-derived classes
 * HoRNDIS, a RNDIS driver for Mac OS X
 *
 *   Copyright (c) 2012 Joshua Wise.
 *
 * IOKit examples from Apple's USBCDCEthernet.cpp; not much of that code remains.
 *
 * RNDIS logic is from linux/drivers/net/usb/rndis_host.c, which is:
 *
 *   Copyright (c) 2005 David Brownell.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "HoRNDIS.h"

#define MYNAME "HoRNDIS"
#define V_DEBUG 0
#define V_NOTE 1
#define V_ERROR 2

#define DEBUGLEVEL V_NOTE
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog(MYNAME ": %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOEthernetController

OSDefineMetaClassAndStructors(HoRNDIS, IOEthernetController);
OSDefineMetaClassAndStructors(HoRNDISUSBInterface, HoRNDIS);
OSDefineMetaClassAndStructors(HoRNDISInterface, IOEthernetInterface);

bool HoRNDIS::init(OSDictionary *properties) {
	int i;

	LOG(V_NOTE, "HoRNDIS tethering driver for Snow Leopard+, by Joshua Wise");
	
	if (super::init(properties) == false) {
		LOG(V_ERROR, "initialize super failed");
		return false;
	}
	
	fDataDead = false;
	
	inbuf.mdp = NULL;
	inbuf.buf = NULL;
	fpDevice = NULL;
	fMediumDict = NULL;
	
	for (i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = NULL;
		outbufs[i].buf = NULL;
		outbufs[i].inuse = false;
	}

	return true;
}


/***** Driver setup and teardown language *****/

bool HoRNDISUSBInterface::start(IOService *provider) {
	IOUSBInterface *intf;

	intf = OSDynamicCast(IOUSBInterface, provider);
	if (!intf) {
		LOG(V_ERROR, "cast to IOUSBInterface failed?");
		return false;
	}
	
	fpDevice = intf->GetDevice();
	
	return HoRNDIS::start(provider);
}

bool HoRNDIS::start(IOService *provider) {
		LOG(V_DEBUG, "starting up");
	if(!super::start(provider))
		return false;

	if (!fpDevice) {
		stop(provider);
		return false;
	}

	if (!openInterfaces())
		goto bailout;
	if (!rndisInit())
		goto bailout;
	
	/* Looks like everything's good... publish the interface! */
	if (!createNetworkInterface())
		goto bailout;
	
	LOG(V_DEBUG, "successful");
	
	return true;

bailout:
	fpDevice->close(this);
	fpDevice = NULL;
	stop(provider);
	return false;
}

void HoRNDIS::stop(IOService *provider) {
	LOG(V_DEBUG, "stopping");
	
	if (fNetworkInterface) {
		fNetworkInterface->release();
		fNetworkInterface = NULL;
	}

	if (fCommInterface) {
		fCommInterface->close(this);
		fCommInterface->release();
		fCommInterface = NULL;	
	}
	
	if (fDataInterface) {
		fDataInterface->close(this);	
		fDataInterface->release();
		fDataInterface = NULL;	
	}

	if (fMediumDict) {
		fMediumDict->release();
		fMediumDict = NULL;
	}
		
	super::stop(provider);
}

bool HoRNDIS::openInterfaces() {
	IOUSBFindInterfaceRequest req;
	IOUSBFindEndpointRequest epReq;
	int rc;
	
	/* open up the RNDIS control interface */
	req.bInterfaceClass    = 0xE0;
	req.bInterfaceSubClass = 0x01;
	req.bInterfaceProtocol = 0x03;
	req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
	
	fCommInterface = fpDevice->FindNextInterface(NULL, &req);
	if (!fCommInterface) {
		/* Maybe it's one of those stupid Galaxy S IIs? (issue #5) */
		req.bInterfaceClass    = 0x02;
		req.bInterfaceSubClass = 0x02;
		req.bInterfaceProtocol = 0xFF;
		req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
		
		fCommInterface = fpDevice->FindNextInterface(NULL, &req);
		if (!fCommInterface) /* Okay, I really have no clue.  Oh well. */
			return false;
	}

	rc = fCommInterface->open(this);
	if (!rc)
		goto bailout1;

	/* open up the RNDIS data interface */
	req.bInterfaceClass    = 0x0A;
	req.bInterfaceSubClass = 0x00;
	req.bInterfaceProtocol = 0x00;
	req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
	
	fDataInterface = fpDevice->FindNextInterface(NULL, &req);	
	if (!fDataInterface)
		goto bailout2;
	
	rc = fDataInterface->open(this);
	if (!rc)
		goto bailout3;
	
	if (fDataInterface->GetNumEndpoints() < 2) {
		LOG(V_ERROR, "not enough endpoints on data interface?");
		goto bailout4;
	}
	
	fCommInterface->retain();
	fDataInterface->retain();
	
	/* open up the endpoints */
	epReq.type = kUSBBulk;
	epReq.direction = kUSBIn;
	epReq.maxPacketSize	= 0;
	epReq.interval = 0;
	fInPipe = fDataInterface->FindNextPipe(0, &epReq);
	if (!fInPipe) {
		LOG(V_ERROR, "no bulk input pipe");
		goto bailout5;
	}
	LOG(V_DEBUG, "bulk input pipe %p: max packet size %d, interval %d", fInPipe, epReq.maxPacketSize, epReq.interval);
	
	epReq.direction = kUSBOut;
	fOutPipe = fDataInterface->FindNextPipe(0, &epReq);
	if (!fOutPipe) {
		LOG(V_ERROR, "no bulk output pipe");
		goto bailout5;
	}
	LOG(V_DEBUG, "bulk output pipe %p: max packet size %d, interval %d", fOutPipe, epReq.maxPacketSize, epReq.interval);
	
	/* Currently, we don't even bother to listen on the interrupt pipe. */
	
	/* And we're done! */
	return true;
	
bailout5:
	fCommInterface->release();
	fDataInterface->release();
bailout4:
	fDataInterface->close(this);
bailout3:
	fDataInterface = NULL;
bailout2:
	fCommInterface->close(this);
bailout1:
	fCommInterface = NULL;
	return false;
}

/* We need our own createInterface (overriding the one in IOEthernetController) because we need our own subclass of IOEthernetInterface.  Why's that, you say?  Well, we need that because that's the only way to set a different default MTU.  Sigh... */

bool HoRNDISInterface::init(IONetworkController * controller, int mtu) {
	if (IOEthernetInterface::init(controller) == false)
		return false;
	LOG(V_NOTE, "starting up with MTU %d", mtu);
	setMaxTransferUnit(mtu);
	return true;
}

/* Overrides IOEthernetController::createInterface */
IONetworkInterface *HoRNDIS::createInterface() {
	HoRNDISInterface * netif = new HoRNDISInterface;
	
	if (!netif)
		return NULL;
	
	if (!netif->init(this, mtu)) {
		netif->release();
		return NULL;
	}
	
	return netif;
}

bool HoRNDIS::createNetworkInterface() {
	LOG(V_DEBUG, "attaching and registering interface");
	
	/* MTU is initialized before we get here, so this is a safe time to do this. */
	if (!attachInterface((IONetworkInterface **)&fNetworkInterface, true)) {
		LOG(V_ERROR, "attachInterface failed?");	  
		return false;
	}
	
	fNetworkInterface->registerService();
	
	return true;	
}

/***** Interface enable and disable logic *****/

/* Contains buffer alloc and dealloc, notably.  Why do that here?  Because that's what Apple did. */

IOReturn HoRNDIS::enable(IONetworkInterface *netif) {
	IONetworkMedium	*medium;
	IOReturn rtn = kIOReturnSuccess;

	if (fNetifEnabled) {
		LOG(V_ERROR, "already enabled?");
		return kIOReturnSuccess;
	}
	
	if (!allocateResources())
		return kIOReturnNoMemory;
	
	if (!fMediumDict)
		if (!createMediumTables()) {
			rtn = kIOReturnNoMemory;
			goto bailout;
		}
	setCurrentMedium(IONetworkMedium::medium(kIOMediumEthernetAuto, 480 * 1000000));
	
	/* Kick off the first read. */
	inbuf.comp.target = this;
	inbuf.comp.action = dataReadComplete;
	inbuf.comp.parameter = NULL;
	
	rtn = fInPipe->Read(inbuf.mdp, &inbuf.comp, NULL);
	if (rtn != kIOReturnSuccess)
		goto bailout;

	/* Tell the world that the link is up... */
	medium = IONetworkMedium::getMediumWithType(fMediumDict, kIOMediumEthernetAuto);
	setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, medium, 480 * 1000000);
	
	/* ... and then listen for packets! */
	getOutputQueue()->setCapacity(TRANSMIT_QUEUE_SIZE);
	getOutputQueue()->start();
	LOG(V_DEBUG, "txqueue started");
	
	/* Tell the other end to start transmitting. */
	if (!rndisSetPacketFilter(RNDIS_DEFAULT_FILTER))
		goto bailout;
	
	/* Now we can say we're alive. */
	fNetifEnabled = true;
	
	return kIOReturnSuccess;
	
bailout:
	LOG(V_ERROR, "setting up the pipes failed");
	releaseResources();
	return rtn;
}
 
IOReturn HoRNDIS::disable(IONetworkInterface * netif) {
	/* Disable the queue (no more outputPacket), and then flush everything in the queue. */
	getOutputQueue()->stop();
	getOutputQueue()->setCapacity(0);
	getOutputQueue()->flush();
	
	/* Other end should stop xmitting, too. */
	rndisSetPacketFilter(0);
	
	setLinkStatus(0, 0);
	
	/* Release all resources */
	releaseResources();
	
	fNetifEnabled = false;
	
	/* Terminates also close the device in 'disable'. */
	if (fTerminate) {
		fpDevice->close(this);
		fpDevice = NULL;
	}

	return kIOReturnSuccess;
}

bool HoRNDIS::createMediumTables() {
	IONetworkMedium	*medium;
	
	fMediumDict = OSDictionary::withCapacity(1);
	if (fMediumDict == NULL)
		return false;
	
	medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 480 * 1000000);
	IONetworkMedium::addMedium(fMediumDict, medium);
	
	if (publishMediumDictionary(fMediumDict) != true)
		return false;
	
	return true;
}

bool HoRNDIS::allocateResources() {
	int i;
	
	/* Grab a memory descriptor pointer for data-in. */
	inbuf.mdp = IOBufferMemoryDescriptor::withCapacity(MAX_BLOCK_SIZE, kIODirectionIn);
	if (!inbuf.mdp)
		return false;
	inbuf.mdp->setLength(MAX_BLOCK_SIZE);
	inbuf.buf = (void *)inbuf.mdp->getBytesNoCopy();
	
	/* And a handful for data-out... */
	LOG(V_DEBUG, "allocating %d buffers", N_OUT_BUFS);
	outbuf_lock = IOLockAlloc();
	for (i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = IOBufferMemoryDescriptor::withCapacity(MAX_BLOCK_SIZE, kIODirectionOut);
		if (!outbufs[i].mdp) {
			LOG(V_ERROR, "allocate output descriptor failed");
			return false;
		}
		
		outbufs[i].mdp->setLength(MAX_BLOCK_SIZE);
		outbufs[i].buf = (UInt8*)outbufs[i].mdp->getBytesNoCopy();
		outbufs[i].inuse = false;
	}
	
	return true;
}

void HoRNDIS::releaseResources() {
	int i;
	
	for (i = 0; i < N_OUT_BUFS; i++)
		if (outbufs[i].mdp) {
			outbufs[i].mdp->release();
			outbufs[i].mdp = NULL;
		}
	
	if (inbuf.mdp) {
		inbuf.mdp->release();
		inbuf.mdp = NULL;
	}
	
	if (outbuf_lock)
		IOLockFree(outbuf_lock);
}

IOOutputQueue* HoRNDIS::createOutputQueue() {
	return IOBasicOutputQueue::withTarget(this, TRANSMIT_QUEUE_SIZE);
}

bool HoRNDIS::configureInterface(IONetworkInterface *netif) {
	IONetworkData *nd;
	
	if (super::configureInterface(netif) == false)
	{
		LOG(V_ERROR, "super failed");
		return false;
	}
	
	nd = netif->getNetworkData(kIONetworkStatsKey);
	if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer()))
	{
		LOG(V_ERROR, "network statistics buffer unavailable?");
		return false;
	}
	
	return true;
}


/***** All-purpose IOKit network routines *****/

IOReturn HoRNDIS::getPacketFilters(const OSSymbol *group, UInt32 *filters) const {
	IOReturn	rtn = kIOReturnSuccess;
	
	if (group == gIOEthernetWakeOnLANFilterGroup)
		*filters = 0;
	else if (group == gIONetworkFilterGroup)
		*filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast;
	else
		rtn = super::getPacketFilters(group, filters);

	return rtn;
}

IOReturn HoRNDIS::getMaxPacketSize(UInt32 * maxSize) const {
	*maxSize = mtu;
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::selectMedium(const IONetworkMedium *medium) {
	setSelectedMedium(medium);
	
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::getHardwareAddress(IOEthernetAddress *ea) {
	UInt32	  i;
	void *buf;
	unsigned char *bp;
	int rlen = -1;
	int rv;
	
	buf = IOMalloc(RNDIS_CMD_BUF_SZ);
	if (!buf)
		return kIOReturnNoMemory;
	
	rv = rndisQuery(buf, OID_802_3_PERMANENT_ADDRESS, 48, (void **) &bp, &rlen);
	if (rv < 0) {
		LOG(V_ERROR, "getHardwareAddress OID failed?");
		IOFree(buf, RNDIS_CMD_BUF_SZ);
		return kIOReturnIOError;
	}
	LOG(V_DEBUG, "MAC Address %02x:%02x:%02x:%02x:%02x:%02x -- rlen %d",
	      bp[0], bp[1], bp[2], bp[3], bp[4], bp[5],
	      rlen);
	
	for (i=0; i<6; i++)
		ea->bytes[i] = bp[i];
	
	IOFree(buf, RNDIS_CMD_BUF_SZ);
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::message(UInt32 type, IOService *provider, void *argument) {
	IOReturn	ior;
	
	switch (type) {
	case kIOMessageServiceIsTerminated:
		LOG(V_NOTE, "kIOMessageServiceIsTerminated");
		
		if (!fNetifEnabled) {
			if (fCommInterface) {
				fCommInterface->close(this);
				fCommInterface->release();
				fCommInterface = NULL;
			}
			
			if (fDataInterface) {
				fDataInterface->close(this);
				fDataInterface->release();
				fDataInterface = NULL;
			}
			
			fpDevice->close(this);
			fpDevice = NULL;
		}
		
		fTerminate = true;
		return kIOReturnSuccess;
	case kIOMessageServiceIsSuspended:
		LOG(V_NOTE, "kIOMessageServiceIsSuspended");
		break;
	case kIOMessageServiceIsResumed:
		LOG(V_NOTE, "kIOMessageServiceIsResumed");
		break;
	case kIOMessageServiceIsRequestingClose:
		LOG(V_NOTE, "kIOMessageServiceIsRequestingClose");
		break;
	case kIOMessageServiceWasClosed:
		LOG(V_NOTE, "kIOMessageServiceWasClosed");
		break;
	case kIOMessageServiceBusyStateChange:
		LOG(V_NOTE, "kIOMessageServiceBusyStateChange");
		break;
	case kIOUSBMessagePortHasBeenResumed:
		LOG(V_NOTE, "kIOUSBMessagePortHasBeenResumed");
		
		/* Try to resurrect any dead reads. */
		if (fDataDead) {
			ior = fInPipe->Read(inbuf.mdp, &inbuf.comp, NULL);
			if (ior == kIOReturnSuccess)
				fDataDead = false;
			else 
				LOG(V_ERROR, "failed to queue Data pipe read");
		}
		
		break;
	case kIOUSBMessageHubResumePort:
		LOG(V_NOTE, "kIOUSBMessageHubResumePort");
		break;
	case kIOMessageServiceIsAttemptingOpen:
		LOG(V_NOTE, "kIOMessageServiceIsAttemptingOpen");
		break;
	default:
		LOG(V_NOTE, "unknown message type %08x", (unsigned int) type);
		break;
	}
	
	return kIOReturnUnsupported;
}


/***** Packet transmit logic *****/

UInt32 HoRNDIS::outputPacket(mbuf_t packet, void *param) {
	mbuf_t m;
	size_t pktlen = 0;
	IOReturn ior = kIOReturnSuccess;
	UInt32 poolIndx;
	int i;

	LOG(V_DEBUG, "");
	
	/* Count the total size of this packet */
	m = packet;
	while (m) {
		pktlen += mbuf_len(m);
		m = mbuf_next(m);
	}
	
	LOG(V_DEBUG, "%ld bytes", pktlen);
	
	if (pktlen > (mtu + 14)) {
		LOG(V_ERROR, "packet too large (%ld bytes, but I told you you could have %d!)", pktlen, mtu);
		fpNetStats->outputErrors++;
		return false;
	}
	
	/* Find an output buffer in the pool */
	IOLockLock(outbuf_lock);
	for (i = 0; i < OUT_BUF_MAX_TRIES; i++) {
		uint64_t ivl, deadl;
		
		for (poolIndx = 0; poolIndx < N_OUT_BUFS; poolIndx++)
			if (!outbufs[poolIndx].inuse) {
				outbufs[poolIndx].inuse = true;
				break;
			}
		if (poolIndx != N_OUT_BUFS)
			break;
		
		/* "while", not "if".  See Symphony X's seminal work on this topic, /Paradise Lost/ (2007). */
		nanoseconds_to_absolutetime(OUT_BUF_WAIT_TIME, &ivl);
		clock_absolutetime_interval_to_deadline(ivl, &deadl);
		LOG(V_NOTE, "waiting for buffer...");
		
		IOLockSleepDeadline(outbuf_lock, outbufs, *(AbsoluteTime *)&deadl, THREAD_INTERRUPTIBLE);
	}
	IOLockUnlock(outbuf_lock);
	
	if (poolIndx == N_OUT_BUFS) {
		LOG(V_ERROR, "timed out waiting for buffer");
		return kIOReturnTimeout;
	}
	
	/* Start filling in the send buffer */
	struct rndis_data_hdr *hdr;
	hdr = (struct rndis_data_hdr *)outbufs[poolIndx].buf;
	
	outbufs[poolIndx].inuse = true;
	
	outbufs[poolIndx].mdp->setLength(pktlen + sizeof *hdr);
	
	memset(hdr, 0, sizeof *hdr);
	hdr->msg_type = RNDIS_MSG_PACKET;
	hdr->msg_len = cpu_to_le32(pktlen + sizeof *hdr);
	hdr->data_offset = cpu_to_le32(sizeof(*hdr) - 8);
	hdr->data_len = cpu_to_le32(pktlen);
	mbuf_copydata(packet, 0, pktlen, hdr + 1);
	
	freePacket(packet);
	
	/* Now, fire it off! */
	outbufs[poolIndx].comp.target    = this;
	outbufs[poolIndx].comp.parameter = (void *)poolIndx;
	outbufs[poolIndx].comp.action    = dataWriteComplete;
	
	ior = fOutPipe->Write(outbufs[poolIndx].mdp, &outbufs[poolIndx].comp);
	if (ior != kIOReturnSuccess) {
		LOG(V_ERROR, "write failed");
		if (ior == kIOUSBPipeStalled) {
			fOutPipe->Reset();
			ior = fOutPipe->Write(outbufs[poolIndx].mdp, &outbufs[poolIndx].comp);
			if (ior != kIOReturnSuccess) {
				LOG(V_ERROR, "write really failed");
				fpNetStats->outputErrors++;
				return ior;
			}
		}
	}
	fpNetStats->outputPackets++;
	
	return kIOReturnOutputSuccess;
}

void HoRNDIS::dataWriteComplete(void *obj, void *param, IOReturn rc, UInt32 remaining) {
	HoRNDIS	*me = (HoRNDIS *)obj;
	unsigned long poolIndx = (unsigned long)param;
	
	poolIndx = (unsigned long)param;
	
	LOG(V_DEBUG, "(rc %08x, poolIndx %ld)", rc, poolIndx);
	
	/* Free the buffer, and hand it off to anyone who might be waiting for one. */
	me->outbufs[poolIndx].inuse = false;
	IOLockWakeup(me->outbuf_lock, me->outbufs, true);
	
	if (rc == kIOReturnSuccess)
		return;
	
	/* Sigh.  Try to clean up. */
	LOG(V_ERROR, "I/O error: %08x", rc);
		
	if (rc != kIOReturnAborted) {
		rc = me->clearPipeStall(me->fOutPipe);
		if (rc != kIOReturnSuccess)
			LOG(V_ERROR, "clear stall failed (trying to continue)");
	}
}

IOReturn HoRNDIS::clearPipeStall(IOUSBPipe *thePipe) {
	IOReturn rc;
	
	if (thePipe->GetPipeStatus() != kIOUSBPipeStalled) {
		LOG(V_ERROR, "pipe not stalled?");
		return kIOReturnSuccess;
	}
	
	rc = thePipe->ClearPipeStall(true);
	LOG(V_ERROR, "pipe stall clear: rv %08x", rc);
	
	return rc;
}


/***** Packet receive logic *****/

void HoRNDIS::dataReadComplete(void *obj, void *param, IOReturn rc, UInt32 remaining) {
	HoRNDIS	*me = (HoRNDIS*)obj;
	IOReturn ior;
	
	if (rc == kIOReturnAborted || rc == kIOReturnNotResponding) {
		LOG(V_ERROR, "I/O aborted: device unplugged?");
		return;
	}
	
	if (rc == kIOReturnSuccess) {
		/* Got one?  Hand it to the back end. */
		LOG(V_DEBUG, "%d bytes", (int)(MAX_BLOCK_SIZE - remaining));
		me->receivePacket(me->inbuf.buf, MAX_BLOCK_SIZE - remaining);
	} else {
		LOG(V_ERROR, "dataReadComplete: I/O error: %08x", rc);
		
		rc = me->clearPipeStall(me->fInPipe);
		if (rc != kIOReturnSuccess)
			LOG(V_ERROR, "clear stall failed (trying to continue)");
	}
	
	/* Queue the next one up. */
	ior = me->fInPipe->Read(me->inbuf.mdp, &me->inbuf.comp, NULL);
	if (ior != kIOReturnSuccess) {
		LOG(V_ERROR, "failed to queue read");
		if (ior == kIOUSBPipeStalled) {
			me->fInPipe->Reset();
			ior = me->fInPipe->Read(me->inbuf.mdp, &me->inbuf.comp, NULL);
			if (ior != kIOReturnSuccess) {
				LOG(V_ERROR, "failed, read dead");
				me->fDataDead = true;
			}
		}
	}
}

void HoRNDIS::receivePacket(void *packet, UInt32 size) {
	mbuf_t m;
	UInt32 submit;
	IOReturn rv;
	
	LOG(V_DEBUG, "sz %d", (int)size);
	
	if (size > MAX_BLOCK_SIZE) {
		LOG(V_ERROR, "packet size error, packet dropped");
		fpNetStats->inputErrors++;
		return;
	}
	
	while (size) {
		struct rndis_data_hdr *hdr = (struct rndis_data_hdr *)packet;
		uint32_t msg_len, data_ofs, data_len;
		
		msg_len = le32_to_cpu(hdr->msg_len);
		data_ofs = le32_to_cpu(hdr->data_offset);
		data_len = le32_to_cpu(hdr->data_len);
		
		if (hdr->msg_type != RNDIS_MSG_PACKET) {
			LOG(V_ERROR, "non-PACKET over data channel? (msg_type %08x)", hdr->msg_type);
			return;
		}
		
		if (hdr->msg_len > size) {
			LOG(V_ERROR, "msg_len too big?");
			return;
		}
		
		if ((data_ofs + data_len + 8) > msg_len) {
			LOG(V_ERROR, "data bigger than msg?");
			return;
		}
	
		m = allocatePacket(data_len);
		if (!m) {
			LOG(V_ERROR, "allocatePacket for data_len %d failed", data_len);
			fpNetStats->inputErrors++;
			return;
		}
		
		rv = mbuf_copyback(m, 0, data_len, (char *)packet + data_ofs + 8, MBUF_WAITOK);
		if (rv) {
			LOG(V_ERROR, "mbuf_copyback failed, rv %08x", rv);
			fpNetStats->inputErrors++;
			freePacket(m);
			return;
		}

		submit = fNetworkInterface->inputPacket(m, data_len);
		LOG(V_DEBUG, "submitted pkt sz %d", data_len);
		fpNetStats->inputPackets++;
		
		size -= hdr->msg_len;
		packet = (char *)packet + hdr->msg_len;
	}
}


/***** RNDIS command logic *****/

int HoRNDIS::rndisCommand(struct rndis_msg_hdr *buf, int buflen) {
	int count;
	int rc = kIOReturnSuccess;
	IOUSBDevRequestDesc rq;
	IOBufferMemoryDescriptor *txdsc = IOBufferMemoryDescriptor::withCapacity(le32_to_cpu(buf->msg_len), kIODirectionOut);
	IOBufferMemoryDescriptor *rxdsc = IOBufferMemoryDescriptor::withCapacity(RNDIS_CMD_BUF_SZ, kIODirectionIn);

	if (buf->msg_type != RNDIS_MSG_HALT && buf->msg_type != RNDIS_MSG_RESET) {
		/* lock? */
		buf->request_id = cpu_to_le32(xid++);
		if (!buf->request_id)
			buf->request_id = cpu_to_le32(xid++);
	}
		
	memcpy(txdsc->getBytesNoCopy(), buf, le32_to_cpu(buf->msg_len));
	rq.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
	rq.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface);
	rq.wValue = 0;
	rq.wIndex = fCommInterface->GetInterfaceNumber();
	rq.pData = txdsc;
	rq.wLength = cpu_to_le32(buf->msg_len);
		
	if ((rc = fCommInterface->DeviceRequest(&rq)) != kIOReturnSuccess)
		goto bailout;
	
	/* Linux polls on the status channel, too; hopefully this shouldn't be needed if we're just talking to Android. */
	
	/* Now we wait around a while for the device to get back to us. */
	for (count = 0; count < 10; count++) {
		struct rndis_msg_hdr *inbuf = (struct rndis_msg_hdr *) rxdsc->getBytesNoCopy();
		IOUSBDevRequestDesc rxrq;
		
		memset(inbuf, 0, RNDIS_CMD_BUF_SZ);
		rxrq.bRequest = USB_CDC_GET_ENCAPSULATED_RESPONSE;
		rxrq.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface);
		rxrq.wValue = 0;
		rxrq.wIndex = fCommInterface->GetInterfaceNumber();
		rxrq.pData = rxdsc;
		rxrq.wLength = RNDIS_CMD_BUF_SZ;
				
		if ((rc = fCommInterface->DeviceRequest(&rxrq)) != kIOReturnSuccess)
			goto bailout;
		
		if (rxrq.wLenDone < 8) {
			LOG(V_ERROR, "short read on control request?");
			IOSleep(20);
			continue;
		}
		
		if (inbuf->msg_type == (buf->msg_type | RNDIS_MSG_COMPLETION)) {
			if (inbuf->request_id == buf->request_id) {
				if (inbuf->msg_type == RNDIS_MSG_RESET_C)
					break;
				if (inbuf->status == RNDIS_STATUS_SUCCESS) {
					/* ...and copy it out! */
					LOG(V_DEBUG, "RNDIS command completed");
					memcpy(buf, inbuf, le32_to_cpu(rxrq.wLenDone));
					break;
				}
				LOG(V_ERROR, "RNDIS command returned status %08x", inbuf->status);
				rc = -1;
				break;
			} else {
				LOG(V_ERROR, "RNDIS return had incorrect xid?");
			}
		} else {
			switch (inbuf->msg_type) {
				case RNDIS_MSG_INDICATE:
					LOG(V_ERROR, "unsupported: RNDIS_MSG_INDICATE");
					break;
				case RNDIS_MSG_KEEPALIVE:
					LOG(V_ERROR, "unsupported: RNDIS_MSG_KEEPALIVE");
					break;
				default:
					LOG(V_ERROR, "unexpected msg type %08x, msg_len %08x", inbuf->msg_type, inbuf->msg_len);
					break;
			}
		}
		
		IOSleep(20);
	}
	if (count == 10) {
		LOG(V_ERROR, "command timed out?");
		rc = kIOReturnTimeout;
	}
	
bailout:
	txdsc->complete();
	txdsc->release();
	rxdsc->complete();
	rxdsc->release();
	
	return rc;
}

int HoRNDIS::rndisQuery(void *buf, uint32_t oid, uint32_t in_len, void **reply, int *reply_len) {
	int rc;
	
	union {
		void *buf;
		struct rndis_msg_hdr *hdr;
		struct rndis_query *get;
		struct rndis_query_c *get_c;
	} u;
	uint32_t off, len;
	
	u.buf = buf;
	
	memset(u.get, 0, sizeof(*u.get) + in_len);
	u.get->msg_type = RNDIS_MSG_QUERY;
	u.get->msg_len = (uint32_t)cpu_to_le32(sizeof(*u.get) + in_len);
	u.get->oid = oid;
	u.get->len = cpu_to_le32(in_len);
	u.get->offset = cpu_to_le32(20);
	
	rc = rndisCommand(u.hdr, 1025);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "RNDIS_MSG_QUERY failure? %08x", rc);
		return rc;
	}
	
	off = le32_to_cpu(u.get_c->offset);
	len = le32_to_cpu(u.get_c->len);
	LOG(V_DEBUG, "RNDIS query completed");
	
	if ((8 + off + len) > 1025)
		goto fmterr;
	if (*reply_len != -1 && len != *reply_len)
		goto fmterr;
	
	*reply = ((unsigned char *) &u.get_c->request_id) + off;
	*reply_len = len;
	
	return 0;

fmterr:
	LOG(V_ERROR, "protocol error?");
	return -1;
}

bool HoRNDIS::rndisInit() {
	int rc;
	union {
		void *buf;
		struct rndis_msg_hdr *hdr;
		struct rndis_init *init;
		struct rndis_init_c *init_c;
	} u;
	
	u.buf = IOMalloc(RNDIS_CMD_BUF_SZ);
	if (!u.buf) {
		LOG(V_ERROR, "out of memory?");
		return false;
	}
	
	u.init->msg_type = RNDIS_MSG_INIT;
	u.init->msg_len = cpu_to_le32(sizeof *u.init);
	u.init->major_version = cpu_to_le32(1);
	u.init->minor_version = cpu_to_le32(0);
	u.init->mtu = MAX_MTU + sizeof(struct rndis_data_hdr);
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "INIT not successful?");
		IOFree(u.buf, RNDIS_CMD_BUF_SZ);
		return false;
	}
	
	mtu = (uint32_t)(le32_to_cpu(u.init_c->mtu) - sizeof(struct rndis_data_hdr) - 36 /* hard_header_len on Linux */ - 14 /* ethernet headers */);
	if (mtu > MAX_MTU)
		mtu = MAX_MTU;
	LOG(V_NOTE, "their MTU %d", mtu);
	
	IOFree(u.buf, RNDIS_CMD_BUF_SZ);
	
	return true;
}

bool HoRNDIS::rndisSetPacketFilter(uint32_t filter) {
	union {
		unsigned char *buf;
		struct rndis_msg_hdr *hdr;
		struct rndis_set *set;
		struct rndis_set_c *set_c;
	} u;
	int rc;
	
	u.buf = (unsigned char *)IOMalloc(RNDIS_CMD_BUF_SZ);
	if (!u.buf) {
		LOG(V_ERROR, "out of memory?");
		return false;;
	}
	
	memset(u.buf, 0, sizeof *u.set);
	u.set->msg_type = RNDIS_MSG_SET;
	u.set->msg_len = cpu_to_le32(4 + sizeof *u.set);
	u.set->oid = OID_GEN_CURRENT_PACKET_FILTER;
	u.set->len = cpu_to_le32(4);
	u.set->offset = cpu_to_le32((sizeof *u.set) - 8);
	*(uint32_t *)(u.buf + sizeof *u.set) = filter;
	
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "SET not successful?");
		IOFree(u.buf, RNDIS_CMD_BUF_SZ);
		return false;
	}
	
	IOFree(u.buf, RNDIS_CMD_BUF_SZ);
	
	return true;
}
