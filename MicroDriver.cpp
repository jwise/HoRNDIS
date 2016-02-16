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

#include <IOKit/IOKitKeys.h>
#include <IOKit/usb/USBSpec.h>

/* This is only available in the userspace IOKit.framework's usb/IOUSBLib.h, for some reason.  So instead: */
#define kIOUSBInterfaceClassName  "IOUSBInterface"

#define MYNAME "MicroDriver"
#define V_PTR 0
#define V_DEBUG 1
#define V_NOTE 2
#define V_ERROR 3

#define DEBUGLEVEL V_NOTE
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog(MYNAME ": %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOService

OSDefineMetaClassAndStructors(MicroDriver, IOService);
OSDefineMetaClassAndStructors(MicroDriverUSBDevice, MicroDriver);

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

bool MicroDriverUSBDevice::start(IOService *provider) {
	IOUSBDevice *dev;
	
	LOG(V_DEBUG, "start, as IOUSBDevice");

	dev = OSDynamicCast(IOUSBDevice, provider);
	if (!dev) {
		LOG(V_ERROR, "cast to IOUSBDevice failed?");
		return false;
	}
	
	fpDevice = dev;
	
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
	
	fpDevice->retain();
	if (!fpDevice->open(this)) {
		LOG(V_ERROR, "could not open the device at all?");
		goto bailout0;
	}
	fpDevice->release();
	
	if (!openInterfaces())
		goto bailout;
	
	LOG(V_ERROR, "Would have been successful, had we gotten this far.");

bailout:
	fpDevice->close(this);
	fpDevice = NULL;
bailout0:
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

IOService *MicroDriver::matchOne(uint32_t cl, uint32_t subcl, uint32_t proto) {
	OSDictionary *dict;
	OSDictionary *propertyDict;
	OSNumber *num;
	IOService *svc;
	
	dict = IOService::serviceMatching(kIOUSBInterfaceClassName);
	if (!dict) {
		LOG(V_ERROR, "could not create matching dict?");
		return NULL;
	}
	
	propertyDict = OSDictionary::withCapacity(3);
	
	num = OSNumber::withNumber((uint64_t)cl, 32); /* XXX error check */
	propertyDict->setObject(kUSBInterfaceClass, num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)subcl, 32); /* XXX error check */
	propertyDict->setObject(kUSBInterfaceSubClass, num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)proto, 32); /* XXX error check */
	propertyDict->setObject(kUSBInterfaceProtocol, num);
	num->release();
	
	dict->setObject(kIOPropertyMatchKey, propertyDict);
	propertyDict->release();
	
	LOG(V_NOTE, "OK, here we go waiting for a matching service with the new propertyDict");
	svc = IOService::waitForMatchingService(dict, 1 * 1000000000 /* i.e., 1 sec */);
	LOG(V_NOTE, "and we are back, having matched exactly %p", svc);
	
	dict->release();
	
	return svc;
}

bool MicroDriver::openInterfaces() {
	IOUSBFindInterfaceRequest req;
	IOReturn rc;
	IOService *datasvc;
	
	/* Set up the device's configuration. */
	if (fpDevice->SetConfiguration(this, ctrlconfig /* config #0 */, true /* start matching */) != kIOReturnSuccess) {
		LOG(V_ERROR, "failed to set configuration %d?", ctrlconfig);
		goto bailout0;
	}
	
	/* Go looking for the comm interface. */
	datasvc = matchOne(ctrlclass, ctrlsubclass, ctrlprotocol);
	if (!datasvc) {
		LOG(V_ERROR, "control: waitForMatchingService(%d, %d, %d) matched nothing?", ctrlclass, ctrlsubclass, ctrlprotocol);
		goto bailout0;
	}

	fCommInterface = OSDynamicCast(IOUSBInterface, datasvc);
	if (!fCommInterface) {
		LOG(V_ERROR, "RNDIS control interface not available?");
		goto bailout0;
	}

	rc = fCommInterface->open(this);
	if (!rc) {
		LOG(V_ERROR, "could not open RNDIS control interface?");
		goto bailout1;
	}
	
	/* Go looking for the data interface.  */
	datasvc = matchOne(10, 0, 0);
	
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
	/* LOG(V_ERROR, "openInterfaces: I woke up having been called to look at interface #%d, which has class 0x%02x, subclass 0x%02x, protocol 0x%02x", fpInterface->GetInterfaceNumber(), fpInterface->GetInterfaceClass(), fpInterface->GetInterfaceSubClass(), fpInterface->GetInterfaceProtocol()); */
	
	return false;
}

IOService *MicroDriver::probe(IOService *provider, SInt32 *score) {
	LOG(V_NOTE, "probe: came in with a score of %d\n", *score);
	IOUSBDevice *dev = OSDynamicCast(IOUSBDevice, provider); /* XXX: error check */
	
	/* Do we even have one of the things that we can match on?  If not, return NULL.  Either 2/255/2, or 224/3/1.  */
	const IOUSBConfigurationDescriptor *cd;
	
	cd = dev->GetFullConfigurationDescriptor(0);
	if (!cd) {
		LOG(V_ERROR, "probe: failed to get a configuration descriptor for configuration 0?");
		return NULL;
	}
	
	/* And look for one of the options. */
	IOUSBInterfaceDescriptor *descout;
	IOUSBFindInterfaceRequest req;
	bool found = false;
	
	ctrlconfig = cd->bConfigurationValue;
	
	req.bInterfaceClass = 2;
	req.bInterfaceSubClass = 2;
	req.bInterfaceProtocol = 255;
	if (dev->FindNextInterfaceDescriptor(cd, NULL, &req, &descout) == kIOReturnSuccess) {
		LOG(V_NOTE, "probe: looks like we're good (2/2/255)");
		ctrlclass = 2;
		ctrlsubclass = 2;
		ctrlprotocol = 255;
		found = true;
	}

	req.bInterfaceClass = 224;
	req.bInterfaceSubClass = 1;
	req.bInterfaceProtocol = 3;
	if (dev->FindNextInterfaceDescriptor(cd, NULL, &req, &descout) == kIOReturnSuccess) {
		ctrlclass = 224;
		ctrlsubclass = 1;
		ctrlprotocol = 3;
		LOG(V_NOTE, "probe: looks like we're good (224/1/3)");
		found = true;
	}
	
	if (!found) {
		LOG(V_NOTE, "probe: this composite device is not for us");
		return NULL;
	}
	
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