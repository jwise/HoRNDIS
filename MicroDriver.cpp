/* MicroDriver.cpp
 * Implementation of IOKit-derived classes
 * MicroDriver, a RNDIS driver for Mac OS X
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

#include "MicroDriver.h"

#define MYNAME "MicroDriver"
#define V_PTR 0
#define V_DEBUG 1
#define V_NOTE 2
#define V_ERROR 3

#define DEBUGLEVEL V_NOTE
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog(MYNAME ": %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOService

OSDefineMetaClassAndStructors(MicroDriver, IOService);
OSDefineMetaClassAndStructors(MicroDriverUSBInterface, MicroDriver);

bool MicroDriver::init(OSDictionary *properties) {
	LOG(V_NOTE, "MicroDriver not-really-tethering driver for Mac OS X, by Joshua Wise");
	
	if (super::init(properties) == false) {
		LOG(V_ERROR, "initialize super failed");
		return false;
	}
	
	LOG(V_PTR, "PTR: I am: %p", this);
	
	fCommInterface = NULL;
	fDataInterface = NULL;
	
	return true;
}

/* IOKit class wrappers */

bool MicroDriverUSBInterface::start(IOService *provider) {
	IOUSBInterface *intf;
	
	LOG(V_DEBUG, "start, as IOUSBInterface");

	intf = OSDynamicCast(IOUSBInterface, provider);
	if (!intf) {
		LOG(V_ERROR, "cast to IOUSBInterface failed?");
		return false;
	}
	
	fpDevice = intf->GetDevice();
	fpInterface = intf;
	
	return MicroDriver::start(provider);
}

bool MicroDriver::start(IOService *provider) {
	LOG(V_DEBUG, "start");
	
	if(!super::start(provider))
		return false;

	if (!fpDevice) {
		stop(provider);
		return false;
	}

	if (!openInterfaces())
		goto bailout;
	
	LOG(V_ERROR, "Would have been successful, had we gotten this far.");

bailout:
	fpDevice->close(this);
	fpDevice = NULL;
	stop(provider);
	return false;
}

void MicroDriver::stop(IOService *provider) {
	LOG(V_DEBUG, "stop");
	
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

	super::stop(provider);
}

bool MicroDriver::openInterfaces() {
	IOUSBFindInterfaceRequest req;
	IOReturn rc;
	OSDictionary *dict;
	OSNumber *num;
	IOService *datasvc;
	
	fCommInterface = fpInterface;
	
	fCommInterface->retain();
	rc = fCommInterface->open(this);
	fCommInterface->release();
	if (!rc) {
		LOG(V_ERROR, "could not open RNDIS control interface?");
		goto bailout1;
	}
	
	/* Go looking for the data interface.  */
	dict = IOService::serviceMatching("IOUSBInterface");
	if (!dict) {
		LOG(V_ERROR, "could not create matching dict?");
		goto bailout2;
	}
	
	num = OSNumber::withNumber((uint64_t)10, 32); /* XXX error check */
	dict->setObject("bInterfaceClass", num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)0, 32); /* XXX error check */
	dict->setObject("bInterfaceSubClass", num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)0, 32); /* XXX error check */
	dict->setObject("bInterfaceProtocol", num);
	num->release();
	
	LOG(V_NOTE, "OK, here we go waiting for a matching service");
	datasvc = IOService::waitForMatchingService(dict, 1000000000 /* i.e., 1 sec */);
	LOG(V_NOTE, "and we are back, having matched exactly %p", datasvc);
	
	dict->release();
	
	if (!datasvc) {
		LOG(V_ERROR, "waitForMatchingService matched nothing?");
		goto bailout2;
	}
	
	fDataInterface = OSDynamicCast(IOUSBInterface, datasvc);
	if (!fDataInterface) {
		LOG(V_ERROR, "RNDIS data interface not available?");
		goto bailout2;
	}
	LOG(V_PTR, "PTR: fDataInterface: %p", fDataInterface);
	
	rc = fDataInterface->open(this);
	if (!rc) {
		LOG(V_ERROR, "could not open RNDIS data interface?");
		goto bailout3;
	}
	
	if (fDataInterface->GetNumEndpoints() < 2) {
		LOG(V_ERROR, "not enough endpoints on data interface?");
		goto bailout4;
	}
	
	fCommInterface->retain();
	fDataInterface->retain();
	
	
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
bailout0:
	/* Show what interfaces we saw. */
	req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
	
	IOUSBInterface *ifc = NULL;
	LOG(V_ERROR, "openInterfaces: before I fail, here are all the interfaces that I saw, in case you care ...");
	while ((ifc = fpDevice->FindNextInterface(ifc, &req))) {
		LOG(V_ERROR, "openInterfaces:   class 0x%02x, subclass 0x%02x, protocol 0x%02x", ifc->GetInterfaceClass(), ifc->GetInterfaceSubClass(), ifc->GetInterfaceProtocol());
	}
	LOG(V_ERROR, "openInterfaces: I woke up having been called to look at interface #%d, which has class 0x%02x, subclass 0x%02x, protocol 0x%02x", fpInterface->GetInterfaceNumber(), fpInterface->GetInterfaceClass(), fpInterface->GetInterfaceSubClass(), fpInterface->GetInterfaceProtocol());
	
	return false;
}

IOService *MicroDriver::probe(IOService *provider, SInt32 *score) {
	LOG(V_NOTE, "probe: came in with a score of %d\n", *score);
	*score += 10000;
	return this;
}


/***** Interface enable and disable logic *****/
IOReturn MicroDriver::message(UInt32 type, IOService *provider, void *argument) {
	switch (type) {
	case kIOMessageServiceIsTerminated:
		LOG(V_NOTE, "kIOMessageServiceIsTerminated");
		
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

IOReturn MicroDriver::getHardwareAddress(IOEthernetAddress * addrP) {
	return kIOReturnUnsupported;
}