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
		goto bailout;
	}
	
	if (!openInterfaces())
		goto bailout;
	
	LOG(V_ERROR, "Would have been successful, had we gotten this far.");

bailout:
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
	
	if (fpDevice) {
		fpDevice->close(this);
		fpDevice->release();
		fpDevice = NULL;
	}

	super::stop(provider);
}

/***** Matching, interface acquisition, and such. *****/

/* THIS IS NOT A PLACE OF HONOR.
 * NO HIGHLY ESTEEMED DEED IS COMMEMORATED HERE.
 * [...]
 * THIS PLACE IS A MESSAGE AND PART OF A SYSTEM OF MESSAGES.
 * WHAT IS HERE IS DANGEROUS AND REPULSIVE TO US.
 * THIS MESSAGE IS A WARNING ABOUT DANGER.
 * [...]
 * WE CONSIDERED OURSELVES TO BE A POWERFUL CULTURE.
 *
 *   -- excerpted from "Expert Judgment on Markers to Deter Inadvertent
 *      Human Intrusion into the Waste Isolation Pilot Plant", Sandia
 *      National Laboratories report SAND92-1382 / UC-721
 */

IOService *MicroDriver::matchOne(uint32_t cl, uint32_t subcl, uint32_t proto) {
	OSDictionary *dict;
	OSDictionary *propertyDict;
	OSNumber *num;
	IOService *svc;
	
	dict = IOService::serviceMatching(kIOUSBInterfaceClassName);
	if (!dict)
		goto nodict;
	
	propertyDict = OSDictionary::withCapacity(3);
	if (!propertyDict)
		goto noprop;
	
	num = OSNumber::withNumber((uint64_t)cl, 32);
	if (!num)
		goto nonum;
	propertyDict->setObject(kUSBInterfaceClass, num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)subcl, 32);
	if (!num)
		goto nonum;
	propertyDict->setObject(kUSBInterfaceSubClass, num);
	num->release();
	
	num = OSNumber::withNumber((uint64_t)proto, 32);
	if (!num)
		goto nonum;
	propertyDict->setObject(kUSBInterfaceProtocol, num);
	num->release();
	
	dict->setObject(kIOPropertyMatchKey, propertyDict);
	propertyDict->release();
	
	svc = IOService::waitForMatchingService(dict, 1 * 1000000000 /* i.e., 1 sec */);
	if (!svc)
		LOG(V_NOTE, "timed out matching a %d/%d/%d", cl, subcl, proto);
	
	dict->release();
	
	return svc;

nonum:
	propertyDict->release();
noprop:
	dict->release();
nodict:	
	LOG(V_ERROR, "low memory error in matchOne(%d, %d, %d)", cl, subcl, proto);
	return NULL;
}

bool MicroDriver::openInterfaces() {
	IOUSBFindInterfaceRequest req;
	IOReturn rc;
	IOService *datasvc;
	
	/* Set up the device's configuration. */
	if (fpDevice->SetConfiguration(this, ctrlconfig, true /* start matching */) != kIOReturnSuccess) {
		LOG(V_ERROR, "failed to set configuration %d?", ctrlconfig);
		goto bailout0;
	}
	
	/* Go looking for the comm interface. */
	datasvc = matchOne(ctrlclass, ctrlsubclass, ctrlprotocol);
	if (!datasvc) {
		LOG(V_ERROR, "control interface: waitForMatchingService(%d, %d, %d) matched nothing?", ctrlclass, ctrlsubclass, ctrlprotocol);
		goto bailout0;
	}

	fCommInterface = OSDynamicCast(IOUSBInterface, datasvc);
	if (!fCommInterface) {
		LOG(V_ERROR, "RNDIS control interface not available?");
		datasvc->release();
		goto bailout0;
	}

	rc = fCommInterface->open(this);
	if (!rc) {
		LOG(V_ERROR, "could not open RNDIS control interface?");
		goto bailout1;
	}
	/* fCommInterface has been retained already, since it came from
	 * waitForMatchingService.  */
	
	/* Go looking for the data interface.
	 *
	 * This takes a little more doing, since we need the one that comes
	 * /immediately after/ the fCommInterface -- otherwise, we could end
	 * up stealing the data interface for, for example, a CDC ACM
	 * device.  But, the complication is that it might or might not have
	 * been created yet.  And worse, if it gets created just after we
	 * look, we might look for it and fail, and then
	 * waitForMatchingService could *still* fail, because it gets
	 * created just before waitForMatchingService is called!
	 *
	 * The synchronization primitive you are looking for here is called
	 * a "condition variable".
	 *
	 * Grumble.
	 */
#if 0
	req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
	req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
#else
	req.bInterfaceClass    = 0x0A;
	req.bInterfaceSubClass = 0x00;
	req.bInterfaceProtocol = 0x00;
#endif
	req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
	
	int counter;
	
	counter = 10;
	while ((fDataInterface = fpDevice->FindNextInterface(fCommInterface, &req)) == NULL && counter--) {
		datasvc = matchOne(0x0A, 0x00, 0x00 /* req.bInterfaceClass, req.bInterfaceSubClass, req.bInterfaceProtocol */);
		if (datasvc) {
			/* We could have a winner, but it might be something
			 * else.  Let FindNextInterface deal with it for us. 
			 * Technically we should probably use an interface
			 * association descriptor, but I don't think that's
			 * exposed readily, so ...  */
			datasvc->release();
		} else {
			/* Not a winner, and we never will be - it's been a
			 * whole second!  */
			break;
		}
	}
	
	if (counter <= 0) {
		LOG(V_ERROR, "data interface: timed out after ten attempts to find an fDataInterface; waitForMatchingService() gave us something, but FindNextInterface couldn't find it?");
	}

	/* Deal with that pesky race condition by looking /just once more/. */
	if (!fDataInterface) {
		fDataInterface = fpDevice->FindNextInterface(fCommInterface, &req);
	}
	
	if (!fDataInterface) {
		LOG(V_ERROR, "data interface: we never managed to find a friend :(");
		goto bailout2;
	}
	
	LOG(V_ERROR, "data interface: okay, I got one, and it was a 0x%02x/0x%02x/0x%02x", fDataInterface->GetInterfaceClass(), fDataInterface->GetInterfaceSubClass(), fDataInterface->GetInterfaceProtocol());
	
	rc = fDataInterface->open(this);
	if (!rc) {
		LOG(V_ERROR, "could not open RNDIS data interface?");
		goto bailout3;
	}
	
	if (fDataInterface->GetNumEndpoints() < 2) {
		LOG(V_ERROR, "not enough endpoints on data interface?");
		goto bailout4;
	}

	/* Of course, since we got this one from FindNextInterface, not from
	 * waitForMatchingService, we have to do lifecycle management of
	 * this thing on our own.
	 *
	 * I don't believe that this is safe, but race conditions can be
	 * avoided by appropriate haste in critical sections, I suppose.
	 */
	fDataInterface->retain();
	
	/* And we're done!  Wasn't that easy?*/
	return true;
	
bailout5:
	fDataInterface->release();
bailout4:
	fDataInterface->close(this);
bailout3:
	fDataInterface = NULL;
bailout2:
	fCommInterface->close(this);
bailout1:
	fCommInterface->release();
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