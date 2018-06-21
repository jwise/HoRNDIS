/* HoRNDIS.cpp
 * Implementation of IOKit-derived classes
 * HoRNDIS, a RNDIS driver for Mac OS X
 *
 *   Copyright (c) 2012 Joshua Wise.
 *
 *   Modifications: Copyright (c) 2018 Mikhail Iakhiaev
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

#include <mach/kmod.h>
#include <IOKit/IOKitKeys.h>

#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
// #include <IOKit/pwr_mgt/RootDomain.h>


#define V_PTR 0
#define V_PACKET 1
#define V_DEBUG 2
#define V_NOTE 3
#define V_ERROR 4

#define DEBUGLEVEL V_DEBUG
// V_NOTE
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog("HoRNDIS: %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOEthernetController

OSDefineMetaClassAndStructors(HoRNDIS, IOEthernetController);
OSDefineMetaClassAndStructors(HoRNDISInterface, IOEthernetInterface);


// Detects the 224/1/3 - RNDIS control interface.
static inline bool isRNDISControlInterface(const InterfaceDescriptor* idesc) {
	return idesc->bInterfaceClass == 224  // Wireless Controller
		&& idesc->bInterfaceSubClass == 1  // Radio Frequency
		&& idesc->bInterfaceProtocol == 3;  // RNDIS protocol
}

// Detects the class 10 - CDC data interface.
static inline bool isCDCDataInterface(const InterfaceDescriptor* idesc) {
	// Check for CDC class. Sub-class and Protocol are undefined:
	return idesc->bInterfaceClass == 10;
}

bool HoRNDIS::init(OSDictionary *properties) {
	extern kmod_info_t kmod_info;  // Getting the version from generated file.
	LOG(V_NOTE, "HoRNDIS tethering driver for Mac OS X, %s", kmod_info.version);
	
	if (super::init(properties) == false) {
		LOG(V_ERROR, "initialize superclass failed");
		return false;
	}

	LOG(V_PTR, "PTR: I am: %p", this);
	
	fNetworkInterface = NULL;
	fpNetStats = NULL;

	fReadyToTransfer = false;
	fNetifEnabled = false;
	fEnableDisableInProgress = false;
	fDataDead = false;
	fCallbackCount = 0;

	fCommInterface = NULL;
	fDataInterface = NULL;
	
	fInPipe = NULL;
	fOutPipe = NULL;

	numFreeOutBufs = 0;
	for (int i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = NULL;
		outbufStack[i] = i;  // Value does not matter here.
	}
	for (int i = 0 ; i < N_IN_BUFS; i++) {
		inbufs[i].mdp = NULL;
	}

	rndisXid = 1;
	mtu = 0;
	
	return true;
}

void HoRNDIS::free() {
	LOG(V_DEBUG, ">");
	// Here, we shall free everything allocated by the 'init'.

	super::free();
}

bool HoRNDIS::start(IOService *provider) {
	LOG(V_DEBUG, ">");

	IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, provider);
	if (interface == NULL) {
		LOG(V_ERROR, "start: BUG we expected IOUSBHostInterface here, but did not get it");
		return false;
	}
	
	// Per comment in "IONetworkController.h", 'super::start' should be the
	// first method called in the overridden implementation. It allocates the
	// network queue for the interface. The rest of the networking
	// initialization will be done by 'createNetworkInterface', once USB
	// USB is ready.
	if(!super::start(provider)) {
		return false;
	}

	if (!openUSBInterfaces(interface)) {
		goto bailout;
	}
	
	// TODO(mikhailai): 'rndisInit' and from that point on needs more review.
	if (!rndisInit()) {
		goto bailout;
	}
	
	LOG(V_DEBUG, "done with RNDIS initialization: can start network interface");

	// Let's create the medium tables here, to avoid doing extra
	// steps in 'enable'. Also, comments recommend creating medium tables
	// in the 'setup' stage.
	const IONetworkMedium *primaryMedium;
	if (!createMediumTables(&primaryMedium) ||
		!setCurrentMedium(primaryMedium)) {
		goto bailout;
	}
	
	// Looks like everything's good... publish the interface!
	if (!createNetworkInterface()) {
		goto bailout;
	}
	
	LOG(V_DEBUG, "successful");
	return true;

bailout:
	stop(provider);
	return false;
}

bool HoRNDIS::willTerminate(IOService *provider, IOOptionBits options) {
	LOG(V_DEBUG, ">");
	// The 'willTerminate' is called when USB device disappears - the user
	// either disconnected the USB, or switched-off tethering. It's likely
	// that the pending read has already invoked a callback with unreachable
	// device or aborted status, and already terminated. If not, closing of
	// the USB Data interface would force it to abort.
	//
	// Note, per comments in 'IOUSBHostInterface.h' (for some later version
	// of MacOS SDK), this is the recommended place to close USB interfaces.
	disableNetworkQueue();
	closeUSBInterfaces();

	return super::willTerminate(provider, options);
}

void HoRNDIS::stop(IOService *provider) {
	LOG(V_DEBUG, ">");
	
	OSSafeReleaseNULL(fNetworkInterface);
	
	closeUSBInterfaces();  // Just in case - supposed to be closed by now.

	super::stop(provider);
}


// Convenience function: to retain and assign in one step:
template <class T> static inline T *retainT(T *ptr) {
	ptr->retain();
	return ptr;
}

bool HoRNDIS::openUSBInterfaces(IOUSBHostInterface *controlInterface) {
	// Make sure the the control interface is expected:
	if (!isRNDISControlInterface(controlInterface->getInterfaceDescriptor())) {
		LOG(V_ERROR, "BUG: expected control interface as a parameter");
		return false;
	}
	fCommInterface = retainT(controlInterface);
	if (!fCommInterface->open(this)) {
		LOG(V_ERROR, "could not open fCommInterface, bailing out");
		// Release 'comm' interface here, so 'stop' would not try to close it.
		OSSafeReleaseNULL(fCommInterface);
		return false;
	}
	
	{  // Now, find the data interface:
		const uint8_t controlIfNum = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;
		IOUSBHostDevice *device = controlInterface->getDevice();
	
		OSIterator* iterator = device->getChildIterator(gIOServicePlane);
		OSObject* candidate = NULL;
		while(iterator != NULL && (candidate = iterator->getNextObject()) != NULL) {
			IOUSBHostInterface* interfaceCandidate =
				OSDynamicCast(IOUSBHostInterface, candidate);
			if (interfaceCandidate == NULL) {
				continue;
			}
			const InterfaceDescriptor *intDesc = interfaceCandidate->getInterfaceDescriptor();
			// Note, also make sure data interface follows right after control:
			if (isCDCDataInterface(intDesc) && intDesc->bInterfaceNumber == controlIfNum + 1) {
				fDataInterface = retainT(interfaceCandidate);
				break;
			}
		}
		OSSafeReleaseNULL(iterator);
	}

	if (!fDataInterface) {
		LOG(V_ERROR, "could not find the data interface, despite seeing its descriptor");
		return false;
	}

	// WARNING, it is a WRONG idea to attach 'fDataInterface' as a second
	// provider, because both providers would be calling 'willTerminate', and
	// 'stop' methods, resulting in chaos.
	
	if (!fDataInterface->open(this)) {
		LOG(V_ERROR, "could not open fDataInterface, bailing out");
		// Release the 'fDataInterface' here, so 'stop' won't try to close it.
		OSSafeReleaseNULL(fDataInterface);
		return false;
	}
	
	{  // Get the pipes for the data interface:
		const EndpointDescriptor *candidate = NULL;
		const InterfaceDescriptor *intDesc = fDataInterface->getInterfaceDescriptor();
		const ConfigurationDescriptor *confDesc = fDataInterface->getConfigurationDescriptor();
		if (intDesc->bNumEndpoints != 2) {
			LOG(V_ERROR, "Expected 2 endpoints for Data Interface, got: %d", intDesc->bNumEndpoints);
			return false;
		}
		while((candidate = StandardUSB::getNextEndpointDescriptor(
					confDesc, intDesc, candidate)) != NULL) {
			const bool isEPIn =
				(candidate->bEndpointAddress & kEndpointDescriptorDirection) != 0;
			IOUSBHostPipe *&pipe = isEPIn ? fInPipe : fOutPipe;
			if (pipe == NULL) {
				// Note, 'copyPipe' already performs 'retain': must not call it again.
				pipe = fDataInterface->copyPipe(candidate->bEndpointAddress);
			}
		}
		if (fInPipe == NULL || fOutPipe == NULL) {
			LOG(V_ERROR, "Could not init IN/OUT pipes in the Data Interface");
			return false;
		}
	}
	
	return true;
}

void HoRNDIS::closeUSBInterfaces() {
	fReadyToTransfer = false;  // Interfaces are about to be closed.
	// Close the interfaces - this would abort the transfers (if present):
	if (fDataInterface) {
		fDataInterface->close(this);
	}
	if (fCommInterface) {
		fCommInterface->close(this);
	}

	OSSafeReleaseNULL(fInPipe);
	OSSafeReleaseNULL(fOutPipe);
	OSSafeReleaseNULL(fDataInterface);
	OSSafeReleaseNULL(fCommInterface);  // First one to open, last one to die.
}

IOService *HoRNDIS::probe(IOService *provider, SInt32 *score) {
	LOG(V_DEBUG, "came in with a score of %d", *score);
	
	// Driver matching algorithm: keep it simple for now. Assumptions:
	//  - Android device has only one USB configuration OR RNDIS interfaces
	//    would be in the active configuration.
	// Algorithm:
	//  - The Info.plist matchies on "224/1/3" - RNDIS control interface.
	//  - "probe" double-checks that and also makes sure the data interface is present.
	//  - The "start" can open the required interfaces right away - no need to open
	//    the device and set the configuration.
	IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, provider);
	if (interface == NULL) {
		LOG(V_ERROR, "unexpected provider class (wrong Info.plist)");
		return NULL;
	}
	
	if (!isRNDISControlInterface(interface->getInterfaceDescriptor())) {
		LOG(V_ERROR, "not RNDIS control interface (wrong Info.plist)");
		return NULL;
	}
	const uint8_t controlIfNum = interface->getInterfaceDescriptor()->bInterfaceNumber;
	
	// Now, search for data interface: if found, we're done:
	bool foundData = false;
	const ConfigurationDescriptor *confDesc = interface->getConfigurationDescriptor();
	const InterfaceDescriptor* intDesc = NULL;
	while((intDesc = StandardUSB::getNextInterfaceDescriptor(confDesc, intDesc)) != NULL) {
		// Just in case an Android device has several CDC data interfaces, we would only
		// pick the one that follows RIGHT AFTER the RNDIS interface:
		if (isCDCDataInterface(intDesc) &&
			intDesc->bInterfaceNumber == controlIfNum + 1) {
			foundData = true;
			break;
		}
	}
	
	if (foundData) {
		*score += 100000;
		return this;
	} else {
		// Did not find any interfaces we can use:
		LOG(V_DEBUG, "unexpected provider class or parameters: this device is not for us");
		return NULL;
	}
}

/***** Ethernet interface bits *****/

/* We need our own createInterface (overriding the one in IOEthernetController) because we need our own subclass of IOEthernetInterface.  Why's that, you say?  Well, we need that because that's the only way to set a different default MTU.  Sigh... */

bool HoRNDISInterface::init(IONetworkController * controller, int mtu) {
	maxmtu = mtu;
	if (IOEthernetInterface::init(controller) == false) {
		return false;
	}
	LOG(V_NOTE, "(network interface) starting up with MTU %d", mtu);
	setMaxTransferUnit(mtu);
	return true;
}

bool HoRNDISInterface::setMaxTransferUnit(UInt32 mtu) {
	if (mtu > maxmtu) {
		LOG(V_NOTE, "Excuse me, but I said you could have an MTU of %u, and you just tried to set an MTU of %d.  Good try, buddy.", maxmtu, mtu);
		return false;
	}
	IOEthernetInterface::setMaxTransferUnit(mtu);
	return true;
}

/* Overrides IOEthernetController::createInterface */
IONetworkInterface *HoRNDIS::createInterface() {
	LOG(V_DEBUG, ">");
	HoRNDISInterface * netif = new HoRNDISInterface;
	
	if (!netif) {
		return NULL;
	}
	
	if (!netif->init(this, mtu)) {
		netif->release();
		return NULL;
	}
	
	return netif;
}

bool HoRNDIS::createNetworkInterface() {
	LOG(V_DEBUG, "attaching and registering interface");
	
	// MTU is initialized before we get here, so this is a safe time to do this.
	if (!attachInterface((IONetworkInterface **)&fNetworkInterface, true)) {
		LOG(V_ERROR, "attachInterface failed?");	  
		return false;
	}
	LOG(V_PTR, "fNetworkInterface: %p", fNetworkInterface);
	
	fNetworkInterface->registerService();
	
	return true;	
}

/***** Interface enable and disable logic *****/
HoRNDIS::EnableDisableLocker::EnableDisableLocker(HoRNDIS *inInst)
		: inst(inInst), result(kIOReturnSuccess) {
	IOCommandGate *const gate = inst->getCommandGate();
	bool &statusVar = inst->fEnableDisableInProgress;
	// Wait until we exit the previously-entered enable or disable method:
	while (statusVar && !isInterrupted()) {
		LOG(V_DEBUG, "Delaying the repeated enable/disable call");
		result = gate->commandSleep(&statusVar);
	}
	statusVar = true;  // Mark the entry into enable or disable method.
}

HoRNDIS::EnableDisableLocker::~EnableDisableLocker() {
	bool &statusVar = inst->fEnableDisableInProgress;
	statusVar = false;
	inst->getCommandGate()->commandWakeup(&statusVar);
}

/* Contains buffer alloc and dealloc, notably.  Why do that here?  
   Not just because that's what Apple did. We don't want to consume these 
   resources when the interface is sitting disabled and unused. */
IOReturn HoRNDIS::enable(IONetworkInterface *netif) {
	IOReturn rtn = kIOReturnSuccess;

	// TODO(mikhailai): This function needs a better clean-up
	// in case of errors - probably factor-out some code from 'disable':

	LOG(V_DEBUG, "for interface '%s'", netif->getName());
	//Toggler entryGuard(&fEnableDisableInProgress);
	EnableDisableLocker locker(this);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (fNetifEnabled) {
		LOG(V_DEBUG, "Repeated enable call: returning success");
		return kIOReturnSuccess;
	}

	// TODO(iakhiaev): Do we even need the "callback count"? Seems like methods
	// such USB closing methods make sure the callbacks are complete anyway.

	if (fCallbackCount != 0) {
		LOG(V_ERROR, "Invalid state: fCallbackCount(=%d) != 0", fCallbackCount);
		return kIOReturnError;
	}

	if (!allocateResources()) {
		return kIOReturnNoMemory;
	}

	// Tell the other end to start transmitting.
	if (!rndisSetPacketFilter(RNDIS_DEFAULT_FILTER)) {
		goto bailout;
	}

	// We can now perform reads and writes between Network stack and USB device:
	fReadyToTransfer = true;
	
	// Kick off the read requests:
	for (int i = 0; i < N_IN_BUFS; i++) {
		pipebuf_t &inbuf = inbufs[i];
		inbuf.comp.owner = this;
		inbuf.comp.action = dataReadComplete;
		inbuf.comp.parameter = &inbuf;
	
		rtn = fInPipe->io(inbuf.mdp, (uint32_t)inbuf.mdp->getLength(),
			&inbuf.comp, 0);
		if (rtn != kIOReturnSuccess) {
			LOG(V_ERROR, "Failed to start the first read %d\n", rtn);
			goto bailout;
		}
		fCallbackCount++;
	}

	// Tell the world that the link is up...
	if (!setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
			getCurrentMedium(), getCurrentMedium()->getSpeed())) {
		LOG(V_ERROR, "Cannot set link status");
		rtn = kIOReturnError;
		goto bailout;
	}

	// ... and then listen for packets!
	getOutputQueue()->setCapacity(TRANSMIT_QUEUE_SIZE);
	getOutputQueue()->start();
	LOG(V_DEBUG, "txqueue started");

	// Now we can say we're alive.
	fNetifEnabled = true;
	LOG(V_DEBUG, "done for interface: '%s'", netif->getName());
	
	return kIOReturnSuccess;
	
bailout:
	LOG(V_ERROR, "setting up the pipes failed");
	releaseResources();
	return rtn;
}

void HoRNDIS::disableNetworkQueue() {
	// Disable the queue (no more outputPacket),
	// and then flush everything in the queue.
	getOutputQueue()->stop();
	getOutputQueue()->setCapacity(0);
	getOutputQueue()->flush();
}

IOReturn HoRNDIS::disable(IONetworkInterface * netif) {
	LOG(V_DEBUG, "for interface: '%s'", netif->getName());
	// TODO(mikhailai): Finish writing the comment:
	// Disable Algorithm:
	// When we get here, we may be in either of the two situations:
	// 1. We get here after 'willTerminate' call, when USB interfaces have
	//    already been closed.
	// 2. ... (TBW) ....

	EnableDisableLocker locker(this);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (!fNetifEnabled) {
		LOG(V_DEBUG, "Repeated call");
		return kIOReturnSuccess;
	}

	disableNetworkQueue();

	// Stop the the new transfers. The code below would cancel the pending ones:
	fReadyToTransfer = false;

	// TODO(mikhailai): Repeated enable/disable (ifconfig up/down) does not work: investigate!
	
	// If USB interfaces are still up, abort the reader and writer:
	if (fInPipe) {
		fInPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, this);
		fInPipe->clearStall(false);
	}
	if (fOutPipe) {
		fOutPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, this);
		fOutPipe->clearStall(false);
	}

	setLinkStatus(0, 0);

	// If the device has not been disconnected, ask it to stop xmitting:
	if (fCommInterface) {
		rndisSetPacketFilter(0);
	}
	
	// Release all resources
	releaseResources();

	// TODO(mikhailai): check if we really need this - maybe the USB APIs can
	// make sure the callbacks are terminated.
	// Currently, this is useful when 'disable' is called without USB
	// disconnect, e.g. using "sudo ifconfig en5 down".
	LOG(V_DEBUG, "Callback count: %d. If not zero, delaying ...",
		fCallbackCount);
	while (fCallbackCount > 0) {
		// No timeout: in our callbacks we trust!
		getCommandGate()->commandSleep(&fCallbackCount);
	}

	fNetifEnabled = false;
	LOG(V_DEBUG, "done for interface: %s", netif->getName());

	return kIOReturnSuccess;
}

bool HoRNDIS::createMediumTables(const IONetworkMedium **primary) {
	IONetworkMedium	*medium;
	
	OSDictionary *mediumDict = OSDictionary::withCapacity(1);
	if (mediumDict == NULL) {
		LOG(V_ERROR, "Cannot allocate OSDictionary");
		return false;
	}
	
	medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 480 * 1000000);
	IONetworkMedium::addMedium(mediumDict, medium);
	medium->release();  // 'mediumDict' holds a ref now.
	if (primary) {
		*primary = medium;
	}
	
	bool result = publishMediumDictionary(mediumDict);
	if (!result) {
		LOG(V_ERROR, "Cannot publish medium dictionary!");
	}

	// Per comment for 'publishMediumDictionary' in NetworkController.h, the
	// medium dictionary is copied and may be safely relseased after the call.
	mediumDict->release();
	
	return result;
}

bool HoRNDIS::allocateResources() {
	LOG(V_DEBUG, "Allocating %d input buffers (size=%d) and %d output "
		"buffers (size=%d)", N_IN_BUFS, IN_BUF_SIZE, N_OUT_BUFS, OUT_BUF_SIZE);
	
	// Grab a memory descriptor pointer for data-in.
	for (int i = 0; i < N_IN_BUFS; i++) {
		inbufs[i].mdp = IOBufferMemoryDescriptor::withCapacity(IN_BUF_SIZE, kIODirectionIn);
		if (!inbufs[i].mdp) {
			return false;
		}
		inbufs[i].mdp->setLength(IN_BUF_SIZE);
		LOG(V_PTR, "PTR: inbuf[%d].mdp: %p", i, inbufs[i].mdp);
	}

	// And a handful for data-out...
	for (int i = 0; i < N_OUT_BUFS; i++) {
		outbufs[i].mdp = IOBufferMemoryDescriptor::withCapacity(
			OUT_BUF_SIZE, kIODirectionOut);
		if (!outbufs[i].mdp) {
			LOG(V_ERROR, "allocate output descriptor failed");
			return false;
		}
		LOG(V_PTR, "PTR: outbufs[%d].mdp: %p", i, outbufs[i].mdp);
		outbufs[i].mdp->setLength(OUT_BUF_SIZE);
		outbufStack[i] = i;
	}
	numFreeOutBufs = N_OUT_BUFS;
	
	return true;
}

void HoRNDIS::releaseResources() {
	LOG(V_DEBUG, "releaseResources");

	fReadyToTransfer = false;  // No transfers without buffers.
	for (int i = 0; i < N_OUT_BUFS; i++) {
		OSSafeReleaseNULL(outbufs[i].mdp);
		outbufStack[i] = i;
	}
	numFreeOutBufs = 0;

	for (int i = 0; i < N_IN_BUFS; i++) {
		OSSafeReleaseNULL(inbufs[i].mdp);
	}
}

IOOutputQueue* HoRNDIS::createOutputQueue() {
	LOG(V_DEBUG, ">");
	// The gated Output Queue keeps things simple: everything is
	// serialized, no need to worry about locks or concurrency.
	// The device is not very fast, so the serial execution should be more
	// than capable of keeping up.
	// Note, if we ever switch to non-gated queue, we shall update the
	// 'outputPacket' to access the shared state using locks + update all the
	// other users of that state + may want to use locks for USB calls as well.
	return IOGatedOutputQueue::withTarget(this,
		getWorkLoop(), TRANSMIT_QUEUE_SIZE);
}

bool HoRNDIS::configureInterface(IONetworkInterface *netif) {
	LOG(V_DEBUG, ">");
	IONetworkData *nd;
	
	if (super::configureInterface(netif) == false) {
		LOG(V_ERROR, "super failed");
		return false;
	}
	
	nd = netif->getNetworkData(kIONetworkStatsKey);
	if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
		LOG(V_ERROR, "network statistics buffer unavailable?");
		return false;
	}
	
	LOG(V_PTR, "fpNetStats: %p", fpNetStats);
	
	return true;
}


/***** All-purpose IOKit network routines *****/

IOReturn HoRNDIS::getPacketFilters(const OSSymbol *group, UInt32 *filters) const {
	IOReturn	rtn = kIOReturnSuccess;
	
	if (group == gIOEthernetWakeOnLANFilterGroup) {
		*filters = 0;
	} else if (group == gIONetworkFilterGroup) {
		// We don't want to support multicast broadcast, promiscuous,
		// or other additional features.
		*filters = kIOPacketFilterUnicast;
		// | kIOPacketFilterBroadcast | kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
	} else {
		rtn = super::getPacketFilters(group, filters);
	}

	return rtn;
}

IOReturn HoRNDIS::getMaxPacketSize(UInt32 * maxSize) const {
	*maxSize = mtu;
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::selectMedium(const IONetworkMedium *medium) {
	LOG(V_DEBUG, ">");
	setSelectedMedium(medium);
	
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::getHardwareAddress(IOEthernetAddress *ea) {
	LOG(V_DEBUG, ">");
	UInt32	  i;
	void *buf;
	unsigned char *bp;
	int rlen = -1;
	int rv;
	
	buf = IOMalloc(RNDIS_CMD_BUF_SZ);
	if (!buf) {
		return kIOReturnNoMemory;
	}

	// TODO(iakhiaev): The function is broken: it returns a different sequence
	// every time: need to investigate.
	
	rv = rndisQuery(buf, OID_802_3_PERMANENT_ADDRESS, 48, (void **) &bp, &rlen);
	if (rv < 0) {
		LOG(V_ERROR, "getHardwareAddress OID failed?");
		IOFree(buf, RNDIS_CMD_BUF_SZ);
		return kIOReturnIOError;
	}
	LOG(V_DEBUG, "MAC Address %02x:%02x:%02x:%02x:%02x:%02x -- rlen %d",
	      bp[0], bp[1], bp[2], bp[3], bp[4], bp[5],
	      rlen);
	
	for (i=0; i<6; i++) {
		ea->bytes[i] = bp[i];
	}
	
	IOFree(buf, RNDIS_CMD_BUF_SZ);
	return kIOReturnSuccess;
}

// TODO(mikhailai): Test and possibly fix suspend and resume.

/*
IOReturn HoRNDIS::setPromiscuousMode(bool active) {
	// XXX This actually needs to get passed down to support 'real'
	//  RNDIS devices, but it will work okay for Android devices.
	
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::message(UInt32 type, IOService *provider, void *argument) {
	//IOReturn	ior;
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
		
		// Try to resurrect any dead reads.
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
		LOG(V_ERROR, ">>>>>>>>> Possibly un-updated messages!!!");
		LOG(V_NOTE, "unknown message type %08x", (unsigned int) type);
		break;
	}

	LOG(V_ERROR, "###################### Received the message: %d, ", type);
	return super::message(type, provider, argument);
}
*/


/***** Packet transmit logic *****/

static inline bool isTransferStopStatus(IOReturn rc) {
	// IOReturn indicating that we need to stop transfers:
	return rc == kIOReturnAborted || rc == kIOReturnNotResponding;
}

UInt32 HoRNDIS::outputPacket(mbuf_t packet, void *param) {
	IOReturn ior = kIOReturnSuccess;
	int poolIndx = N_OUT_BUFS;

	// Note, this function MAY or MAY NOT be protected by the IOCommandGate,
	// depending on the kind of OutputQueue used.
	// Here, we assume that IOCommandGate is used: no need to lock.

	if (!fReadyToTransfer) {
		// Technically, we must never be here, because we always disable the
		// queue before clearing 'fReadyToTransfer', but double-checking here
		// just in-case: better safe than sorry.
		LOG(V_DEBUG, "fReadyToTransfer=false: dropping packet "
			"(we shouldn't even be here)");
		freePacket(packet);
		return kIOReturnOutputDropped;
	}
	
	// Count the total size of this packet
	size_t pktlen = 0;
	for (mbuf_t m = packet; m; m = mbuf_next(m)) {
		pktlen += mbuf_len(m);
	}
	
	LOG(V_PACKET, "%ld bytes", pktlen);
	
	if (pktlen > (mtu + 14)) {
		LOG(V_ERROR, "packet too large (%ld bytes, but I told you you could have %d!)", pktlen, mtu);
		fpNetStats->outputErrors++;
		freePacket(packet);
		return kIOReturnOutputDropped;
	}

	if (numFreeOutBufs <= 0) {
		LOG(V_ERROR, "BUG: Ran out of buffers - stall did not work!");
		// Stall the queue and re-try the same packet later: don't release:
		return kIOOutputStatusRetry | kIOOutputCommandStall;
	}

	// Note, we don't decrement 'numFreeOutBufs' (commit to using that buffer)
	// until everything is successful.
	poolIndx = outbufStack[numFreeOutBufs - 1];
	if (poolIndx < 0 || poolIndx >= N_OUT_BUFS) {
		LOG(V_ERROR, "BUG: poolIndex out-of-bounds");
		freePacket(packet);
		return kIOReturnOutputDropped;
	}

	// Start filling in the send buffer
	struct rndis_data_hdr *hdr;
	hdr = (struct rndis_data_hdr *)outbufs[poolIndx].mdp->getBytesNoCopy();

	const uint32_t transmitLength = (uint32_t)(pktlen + sizeof(*hdr));
	outbufs[poolIndx].mdp->setLength(transmitLength);
	
	memset(hdr, 0, sizeof *hdr);
	hdr->msg_type = RNDIS_MSG_PACKET;
	hdr->msg_len = cpu_to_le32(pktlen + sizeof *hdr);
	hdr->data_offset = cpu_to_le32(sizeof(*hdr) - 8);
	hdr->data_len = cpu_to_le32(pktlen);
	mbuf_copydata(packet, 0, pktlen, hdr + 1);
	
	freePacket(packet);
	packet = NULL;
	
	// Now, fire it off!
	IOUSBHostCompletion *const comp = &outbufs[poolIndx].comp;
	comp->owner     = this;
	comp->parameter = (void *)(uintptr_t)poolIndx;
	comp->action    = dataWriteComplete;
	
	ior = fOutPipe->io(outbufs[poolIndx].mdp, transmitLength, comp);
	if (ior != kIOReturnSuccess && !isTransferStopStatus(ior)) {
		LOG(V_ERROR, "write failed: %08x", ior);
		if (ior == kUSBHostReturnPipeStalled) {
			// If we have pipe stall error, clear and retry.
			fOutPipe->clearStall(false);
			ior = fOutPipe->io(outbufs[poolIndx].mdp, transmitLength, comp);
		}
	}

	if (ior != kIOReturnSuccess) {
		if (isTransferStopStatus(ior)) {
			LOG(V_DEBUG, "WRITER: The device was possibly disconnected: ignoring the error");
		} else {
			LOG(V_ERROR, "write re-try failed as well: %08x", ior);
			fpNetStats->outputErrors++;
		}
		// Packet was already freed: just quit:
		return kIOReturnOutputDropped;
	}
	// Only here - when 'fOutPipe->io' has fired - we mark the buffer in-use:
	numFreeOutBufs--;
	fCallbackCount++;
	fpNetStats->outputPackets++;
	// If we ran out of free buffers, issue a stall command to the queue.
	// Note, this would be "we accept this packet, but don't give us more yet",
	// which is NOT the same as 'kIOReturnOutputStall'.
	const bool stallQueue = (numFreeOutBufs == 0);
	if (stallQueue) {
		LOG(V_PACKET, "Issuing stall command to the output queue");
	}
	return kIOOutputStatusAccepted |
		(stallQueue ? kIOOutputCommandStall : kIOOutputCommandNone);
}

void HoRNDIS::callbackExit() {
	fCallbackCount--;
	// Notify the 'disable' that may be waiting for callback count to reach 0:
	if (fCallbackCount <= 0) {
		LOG(V_DEBUG, "Notifying last callback exited");
		getCommandGate()->commandWakeup(&fCallbackCount);
	}
}

void HoRNDIS::dataWriteComplete(void *obj, void *param, IOReturn rc, UInt32 transferred) {
	HoRNDIS	*me = (HoRNDIS *)obj;
	unsigned long poolIndx = (unsigned long)param;
	
	poolIndx = (unsigned long)param;

	LOG(V_PACKET, "(rc %08x, poolIndx %ld)", rc, poolIndx);
	// Callback completed. We don't know when/if we launch another one:
	me->callbackExit();

	// Note, if 'fReadyToTransfer' is false, we shall not go further:
	// it's a good idea NOT to touch the 'outbufs'.
	if (isTransferStopStatus(rc) || !me->fReadyToTransfer) {
		LOG(V_DEBUG, "Data Write Aborted, or ready-to-transfer is cleared.");
		return;
	}

	if (rc != kIOReturnSuccess) {
		// Sigh.  Try to clean up.
		LOG(V_ERROR, "I/O error: %08x", rc);
		maybeClearPipeStall(rc, me->fOutPipe);
	}

	// Free the buffer: put the index back onto the stack:
	if (me->numFreeOutBufs >= N_OUT_BUFS) {
		LOG(V_ERROR, "BUG: more free buffers than was allocated");
		return;
	}

	me->outbufStack[me->numFreeOutBufs] = poolIndx;
	me->numFreeOutBufs++;
	// Unstall the queue whenever the number of free buffers goes 0->1.
	// I.e. we unstall it the moment we're able to write something into it:
	if (me->numFreeOutBufs == 1) {
		me->getOutputQueue()->service();
	}
}

void HoRNDIS::maybeClearPipeStall(IOReturn rc, IOUSBHostPipe *thePipe) {
	if (rc == kUSBHostReturnPipeStalled) {
		rc = thePipe->clearStall(true);
		if (rc != kIOReturnSuccess) {
			LOG(V_ERROR, "clear stall failed (trying to continue)");
		}
	}
}

/***** Packet receive logic *****/
void HoRNDIS::dataReadComplete(void *obj, void *param, IOReturn rc, UInt32 transferred) {
	HoRNDIS	*me = (HoRNDIS *)obj;
	pipebuf_t *inbuf = (pipebuf_t *)param;
	IOReturn ior;

	// Stop conditions. Not separating them out, since reacting to individual
	// ones would be very timing-sansitive.
	if (isTransferStopStatus(rc) || !me->fReadyToTransfer) {
		LOG(V_DEBUG, "READER STOPPED: USB device aborted or not responding, "
			"or 'fReadyToTransfer' flag is cleared.");
		me->callbackExit();
		return;
	}
	
	if (rc == kIOReturnSuccess) {
		// Got one?  Hand it to the back end.
		LOG(V_PACKET, "Reader(%ld), tid=%lld: %d bytes", inbuf - me->inbufs,
			thread_tid(current_thread()), transferred);
		me->receivePacket(inbuf->mdp->getBytesNoCopy(), transferred);
	} else {
		LOG(V_ERROR, "dataReadComplete: I/O error: %08x", rc);
		maybeClearPipeStall(rc, me->fInPipe);
	}
	
	// Queue the next one up.
	ior = me->fInPipe->io(inbuf->mdp, (uint32_t)inbuf->mdp->getLength(),
						&inbuf->comp);
	if (ior == kIOReturnSuccess) {
		return;  // Callback is in-progress.
	}

	LOG(V_ERROR, "failed to queue read: %08x", ior);
	if (ior == kUSBHostReturnPipeStalled) {
		me->fInPipe->clearStall(false);
		// Try to read again:
		ior = me->fInPipe->io(inbuf->mdp, (uint32_t)inbuf->mdp->getLength(),
							&inbuf->comp);
		if (ior == kIOReturnSuccess) {
			return;  // Callback is finally working!
		}
	}

	LOG(V_ERROR, "READER STOPPED: USB FAILURE, cannot recover");
	me->callbackExit();
	me->fDataDead = true;
}

/*!
 * Transfer the packet we've received to the MAC OS Network stack.
 */
void HoRNDIS::receivePacket(void *packet, UInt32 size) {
	mbuf_t m;
	UInt32 submit;
	IOReturn rv;
	
	LOG(V_PACKET, "packet sz %d", (int)size);
	
	while (size) {
		struct rndis_data_hdr *hdr = (struct rndis_data_hdr *)packet;
		uint32_t msg_len, data_ofs, data_len;
		
		if (size <= sizeof(struct rndis_data_hdr)) {
			LOG(V_ERROR, "receivePacket() on too small packet? (size %d)", size);
			return;
		}
		
		msg_len = le32_to_cpu(hdr->msg_len);
		data_ofs = le32_to_cpu(hdr->data_offset);
		data_len = le32_to_cpu(hdr->data_len);
		
		if (hdr->msg_type != RNDIS_MSG_PACKET) { // both are LE, so that's okay
			LOG(V_ERROR, "non-PACKET over data channel? (msg_type %08x)", hdr->msg_type);
			return;
		}
		
		if (msg_len > size) {
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
		LOG(V_PTR, "PTR: mbuf: %p", m);
		
		rv = mbuf_copyback(m, 0, data_len, (char *)packet + data_ofs + 8, MBUF_WAITOK);
		if (rv) {
			LOG(V_ERROR, "mbuf_copyback failed, rv %08x", rv);
			fpNetStats->inputErrors++;
			freePacket(m);
			return;
		}

		submit = fNetworkInterface->inputPacket(m, data_len);
		LOG(V_PACKET, "submitted pkt sz %d", data_len);
		fpNetStats->inputPackets++;
		
		size -= msg_len;
		packet = (char *)packet + msg_len;
	}
}


/***** RNDIS command logic *****/

IOReturn HoRNDIS::rndisCommand(struct rndis_msg_hdr *buf, int buflen) {
	int rc = kIOReturnSuccess;
	const uint8_t ifNum = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;

	// TODO(mikhailai): Get rid of this: copy back directly to our buffer.
	IOBufferMemoryDescriptor *rxdsc =
		IOBufferMemoryDescriptor::withCapacity(RNDIS_CMD_BUF_SZ, kIODirectionIn);
	LOG(V_PTR, "PTR: rxdsc: %p", rxdsc);

	if (buf->msg_type != RNDIS_MSG_HALT && buf->msg_type != RNDIS_MSG_RESET) {
		// No need to lock here: multi-threading does not even come close
		// (IOWorkLoop + IOGate are at our service):
		buf->request_id = cpu_to_le32(rndisXid++);
		if (!buf->request_id) {
			buf->request_id = cpu_to_le32(rndisXid++);
		}
		
		LOG(V_DEBUG, "Generated xid: %d", le32_to_cpu(buf->request_id));
	}
	
	{
		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionOut |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = le32_to_cpu(buf->msg_len);
	
		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, buf, bytes_transferred)) != kIOReturnSuccess ||
			bytes_transferred != rq.wLength) {
			LOG(V_DEBUG, "Device request send error");
			goto bailout;
		}
	}
	
	// Linux polls on the status channel, too; hopefully this shouldn't be needed if we're just talking to Android.
	
	// Now we wait around a while for the device to get back to us.
	// TODO(mikhailai): Do we need this stupid polling?
	int count;
	for (count = 0; count < 10; count++) {
		struct rndis_msg_hdr *rxbuf = (struct rndis_msg_hdr *) rxdsc->getBytesNoCopy();
		memset(rxbuf, 0, RNDIS_CMD_BUF_SZ);
		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionIn |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_GET_ENCAPSULATED_RESPONSE;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = RNDIS_CMD_BUF_SZ;
		
		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, rxdsc, bytes_transferred)) != kIOReturnSuccess) {
			goto bailout;
		}
		// TODO(mikhailai): Refactor this all: I think it can be WAY simpler!
		if (bytes_transferred < 8) {
			LOG(V_ERROR, "short read on control request?");
			IOSleep(20);
			continue;
		}
		
		if (rxbuf->msg_type == (buf->msg_type | RNDIS_MSG_COMPLETION)) {
			if (rxbuf->request_id == buf->request_id) {
				if (rxbuf->msg_type == RNDIS_MSG_RESET_C)
					break;
				if (rxbuf->status == RNDIS_STATUS_SUCCESS) {
					// ...and copy it out!
					LOG(V_DEBUG, "RNDIS command completed");
					memcpy(buf, rxbuf, bytes_transferred);
					break;
				}
				LOG(V_ERROR, "RNDIS command returned status %08x", rxbuf->status);
				rc = -1;
				break;
			} else {
				LOG(V_ERROR, "RNDIS return had incorrect xid?");
			}
		} else {
			if (rxbuf->msg_type == RNDIS_MSG_INDICATE) {
				LOG(V_ERROR, "unsupported: RNDIS_MSG_INDICATE");	
			} else if (rxbuf->msg_type == RNDIS_MSG_INDICATE) {
				LOG(V_ERROR, "unsupported: RNDIS_MSG_KEEPALIVE");
			} else {
				LOG(V_ERROR, "unexpected msg type %08x, msg_len %08x", rxbuf->msg_type, rxbuf->msg_len);
			}
		}
		
		IOSleep(20);
	}
	if (count == 10) {
		LOG(V_ERROR, "command timed out?");
		rc = kIOReturnTimeout;
	}
	
bailout:
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
	u.get->msg_len = cpu_to_le32(sizeof(*u.get) + in_len);
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
	
	if ((8 + off + len) > 1025) {
		goto fmterr;
	}
	if (*reply_len != -1 && len != *reply_len) {
		goto fmterr;
	}
	
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
	
	mtu = (uint32_t)(le32_to_cpu(u.init_c->mtu) - sizeof(struct rndis_data_hdr)
					 - 36  // hard_header_len on Linux
					 - 14  // ethernet headers
					 );
	if (mtu > MAX_MTU) {
		mtu = MAX_MTU;
	}
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
