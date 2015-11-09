//
//  CompositeDevice.cpp
//  HoRNDIS
//
//  Created by Andrew Querol on 11/8/15.
//  Copyright © 2015 Joshua Wise. All rights reserved.
//

#include "HoRNDIS.h"
#include <IOKit/usb/USBSpec.h>

#define super HoRNDIS

OSDefineMetaClassAndStructors(HoRNDISUSBComposite, HoRNDIS);

bool HoRNDISUSBComposite::attach(IOService *provider) {
	// We can do custom logic here if needed to expilicity not allow it to attach and fail matching, for now this is a stub
	return super::attach(provider);
}

/**
 * Provider = device that passed passive matching
 * Score = The current score that this driver has
 */
IOService *HoRNDISUSBComposite::probe(IOService *provider, SInt32 *score) {
	// Is this a usb device provider? If yes this does nothing, but if not it'll return null causing the probe to fail.
	return OSDynamicCast(IOUSBDevice, provider);
}

void HoRNDISUSBComposite::detach(IOService *provider) {
	// We can do custom logic here if needed, for now this is a stub
	return super::detach(provider);
}

// We are the best match, setup driver
bool HoRNDISUSBComposite::start(IOService *provider) {
	IOUSBDevice *usbDevice = OSDynamicCast(IOUSBDevice, provider);
	if (!usbDevice) {
		LOG(V_ERROR, "HoRNDISUSBDevice: Provider is not an IOUSBDevice, this is impossible!");
		// Tell the kernel to try the next highest score driver
		return false;
	}
	IOUSBFindInterfaceRequest req;
	req.bInterfaceClass = kIOUSBFindInterfaceDontCare; //kUSBCommunicationControlInterfaceClass;
	req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
	req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
	
	IOUSBInterface *controlInterface = nullptr;
	IOUSBInterface *dataInterface = nullptr;
	
	// Find the interfaces
	OSIterator *deviceInterfaceIterator = usbDevice->CreateInterfaceIterator(&req);
	for (OSObject *entry = deviceInterfaceIterator->getNextObject(); entry != nullptr && (controlInterface == nullptr || dataInterface == nullptr); entry = deviceInterfaceIterator->getNextObject()) {
		IOUSBInterface *usbInterface = OSDynamicCast(IOUSBInterface, entry);
		if (!usbInterface) {
			continue;
		}
		
		if (usbInterface->GetInterfaceClass() == kUSBCommunicationControlInterfaceClass || usbInterface->GetInterfaceClass() == kUSBWirelessControllerInterfaceClass) {
			controlInterface = usbInterface;
		} else if (usbInterface->GetInterfaceClass() == kUSBCommunicationDataInterfaceClass) {
			dataInterface = usbInterface;
		}
	}

	deviceInterfaceIterator->release();
	return (controlInterface != nullptr && dataInterface != nullptr) ? super::start(usbDevice, controlInterface, dataInterface) : false;
}

void HoRNDISUSBComposite::stop(IOService *provider) {
	// Something is telling us that we need to stop, forward to the Ethernet driver
	super::stop(provider);
}