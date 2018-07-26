/* HoRNDIS.cpp
 * Implementation of IOKit-derived classes
 * HoRNDIS, a RNDIS driver for Mac OS X
 *
 *   Copyright (c) 2012 Joshua Wise.
 *   Copyright (c) 2018 Mikhail Iakhiaev
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
#include <libkern/version.h>
#include <IOKit/IOKitKeys.h>

#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
// May be useful for supporting suspend/resume:
// #include <IOKit/pwr_mgt/RootDomain.h>


#define V_PTR 0
#define V_PACKET 1
#define V_DEBUG 2
#define V_NOTE 3
#define V_ERROR 4

// The XCode "Debug" build is now more verbose:
#if DEBUG == 1
	#define DEBUGLEVEL V_DEBUG
#else
	#define DEBUGLEVEL V_NOTE
#endif
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog("HoRNDIS: %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define super IOEthernetController

OSDefineMetaClassAndStructors(HoRNDIS, IOEthernetController);
OSDefineMetaClassAndStructors(HoRNDISInterface, IOEthernetInterface);

/* 
================================================================
DESCRIPTION OF DEVICE DRIVER MATCHING (+ Info.plist description)
================================================================
The HoRNDIS driver classes are only instantiated when the MacOS matches 
"IOKitPersonalities" dictionary entries to the existing devices. The matching 
can be done 2 based on two different provider classes:
 
 - IOUSBHostInterface - matches an interface under a USB device. Here, we match
   based on the interface class/subclass/protocol. In order for the matching to
   work, some other driver has to open the USB device and call 
   "setConfiguration" method with matchInterfaces=true.
   The interface matching works out-of-the-box for interfaces under USB
   Composite Devices (class/subclass/protocol are 0/0/0), since there is an OS 
   driver that opens such devices for matching.
 
 - IOUSBHostDevice - match is performed on the whole device. Here, we match 
   based on class/subclass/protocol. The start method needs to call 
   'setConfiguration' in order for any IOUSBHostInterface instances under the 
   device to become available. If it specifies 'matchInterfaces', matching is 
   then performed on newly-created IOUSBHostInterfaces.

OUR APPROACH:
We'll match based on either device or interface, and let the probe and start 
methods handle the difference. Not calling setConfiguration with matchInterfaces
for now: if it's not a USB composite device, consider that we own it.
 
Subsequent logic:
After MacOS finds a match based on Info.plist, it instantiates the driver class,
and calls the 'probe' method that looks at descriptors and decides if this is 
really the device we care about (e.g. fine-grained filtering), and sets the 
"probeXxx" variables. Then, 'start' method calls 'openUSBInterfaces' that 
blindly follows the "probeXxx" variables to get the needed IOUSBHostInterface 
values from the opened device.

==========================
||  DEVICE VARIATIONS
==========================
This section must document ALL different variations of the devices that we
may be dealing with, so we have the whole picture when updating the "probe"
code or "Info.plist". Notation:
 * Device: 224 / 0 / 0
   - bDeviceClass / bDeviceSubClass / bDeviceProtocol
 * Interface Associaton[2]: 224 / 1 / 3
   - [bInterfaceCount]:  bFunctionClass / bFunctionSubClass / bFunctionProtocol
 * Interface: 224 / 3 / 1
   -  bInterfaceClass / bInterfaceSubClass / bInterfaceProtocol

[*] "Stock" Android. I believe most Android phone tethering should behave
    this way
    * USBCompositeDevice: 0 / 0 / 0
      - InterfaceAssociation[2] 224 / 1 / 3
        - ControlInterface: 224 / 1 / 3
        - DataInterface:     10 / 0 / 0
    * Info.plist entry: RNDISControlStockAndroid(interface)

[*] Linux USB Gadget drivers. Location:
    <LINUX_KERNEL>/drivers/usb/gadget/function/f_rndis.c
    These show up in various embedded Linux boards, such as Beagle Board,
    Analog Devices PlutoSDR, etc.
    * USBCompositeDevice: 0 / 0 / 0
      - InterfaceAssociation[2]: Configurable (e.g. 2/6/0, 239/4/1).
        - ControlInterface:  2 / 2 / 255
        - DataInterface:    10 / 0 / 0
    * Info.plist entry: RNDISControlLinuxGadget(interface)

[*] Wireless Controller Device (class 224). Some Samsung phones (e.g. S7 Edge) 
    specify device class 224 for tethering, instead of just being a USB
    composite device. The rest is the same as in "stock" Android.
    Note, the other Samsung phones (e.g. S8) behave like other Android devices.
	* Device: 224 / 0 / 0
	  - (same as "Stock" Android)

[*] Composite Device, using 0xEF/4/1 for RNDIS control: Nokia 7 Plus (issue #88)
    Also may apply to Sony Xperia XZ.
    This matches "RNDIS over Ethernet" specification given here:
	http://www.usb.org/developers/defined_class/#BaseClassEFh
    * USBCompositeDevice: 0 / 0 / 0
	  - InterfaceAssociation[2]: 239 / 4 / 1
        - ControlInterface: 239 / 4 / 1
        - DataInterface:     10 / 0 / 0
	* Info.plist entry: RNDISControlMiscDeviceRoE(interface)
*/

// Detects the 224/1/3 - stock Android RNDIS control interface.
static inline bool isRNDISControlStockAndroid(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 224  // Wireless Controller
		&& idesc->bInterfaceSubClass == 1  // Radio Frequency
		&& idesc->bInterfaceProtocol == 3;  // RNDIS protocol
}

// Miscellaneous Device (0xEF), RNDIS over Ethernet: some phones, see above.
static inline bool isRNDISControlMiscDeviceRoE(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 239  // Miscellaneous Device
		&& idesc->bInterfaceSubClass == 4  // RNDIS?
		&& idesc->bInterfaceProtocol == 1;  // RNDIS over Ethernet
}

// Detects RNDIS control on BeagleBoard and possibly other embedded Linux devices.
static inline bool isRNDISControlLinuxGadget(const InterfaceDescriptor *idesc) {
	return idesc->bInterfaceClass == 2  // Communications / CDC Control
		&& idesc->bInterfaceSubClass == 2  // Abstract (modem)
		&& idesc->bInterfaceProtocol == 255;  // Vendor Specific (RNDIS).
}

// Any of the above RNDIS control interface.
static inline bool isRNDISControlInterface(const InterfaceDescriptor *idesc) {
	return isRNDISControlStockAndroid(idesc)
		|| isRNDISControlLinuxGadget(idesc)
		|| isRNDISControlMiscDeviceRoE(idesc);
}

// Detects the class 10 - CDC data interface.
static inline bool isCDCDataInterface(const InterfaceDescriptor *idesc) {
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

	fProbeConfigVal = 0;
	fProbeCommIfNum = 0;

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
	maxOutTransferSize = 0;
	
	return true;
}

void HoRNDIS::free() {
	// Here, we shall free everything allocated by the 'init'.

	LOG(V_NOTE, "driver instance terminated");  // For the default level
	super::free();
}

/*
==================================================
INTERFACE PROLIFERATION AND PROVIDER CLASS NAME
==================================================
PROBLEM:
 Every time you connect the same Android device (or somewhat more rarely),
 MacOS creates a new entry under "Network" configurations tab. These entries
 keep on coming on and on, polluting the configuration.
 
ROOT CAUSE:
 Android devices randomly-generate Ethernet MAC address for
 RNDIS interface, so the system may think there is a new device
 every time you connect an Android phone, and may create a new
 network interface every such time.
 Luckily, it does extra check when Network Provider is a USB device:
 in that case, it would match based on USB data, creating an entry like:
		<key>SCNetworkInterfaceInfo</key>
		<dict>
			<key>USB Product Name</key>
			<string>Pixel 2</string>
			<key>UserDefinedName</key>
			<string>Pixel 2</string>
			<key>idProduct</key>
			<integer>20195</integer>
			...
 In: /Library/Preferences/SystemConfiguration/NetworkInterfaces.plist
 When that works, interfaces do not proliferate (at least in most cases).

PROBLEM CAUSE:
 The MacOS network daemon (or whatever it is) looks at interface's 
 "IOProviderClass" to see if it's USB Device. Unfortunately, it may not pick
 up all the names, e.g. it may trigger off the old "IOUSBDevice", but not
 the new "IOUSBHostDevice". This problem is present in El Capitan for both
 device and the interface, and seems to be present in later systems for 
 IOUSBDevice.
 
FIX/HACK:
 The IOUSBHostDevice and IOUSBHostInterface providers actually specify
 the "IOClassNameOverride" that gives the old name. We just take this value
 and update our "IOProviderClass" to that.
*/

bool HoRNDIS::start(IOService *provider) {
	LOG(V_DEBUG, ">");

	// Per comment in "IONetworkController.h", 'super::start' should be the
	// first method called in the overridden implementation. It allocates the
	// network queue for the interface. The rest of the networking
	// initialization will be done by 'createNetworkInterface', once USB
	// USB is ready.
	if(!super::start(provider)) {
		return false;
	}

	{  // Fixing the Provider class name.
		// See "INTERFACE PROLIFERATION AND PROVIDER CLASS NAME" description.
		OSObject *providerClass = provider->getProperty("IOClassNameOverride");
		if (providerClass) {
			setProperty(kIOProviderClassKey, providerClass);
		}
	}

	if (!openUSBInterfaces(provider)) {
		goto bailout;
	}

	if (!rndisInit()) {
		goto bailout;
	}

	// NOTE: The RNDIS spec mandates the usage of Keep Alive timer; however,
	// the Android does not seem to be missing its absense, so there is
	// probably no use in implementing it.

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

	// This call is based on traces of Thunderbolt Ethernet driver.
	// That driver calls 'setLinkStatus(0x1)' before interface publish 
	// callback (which happens after 'start').
	setLinkStatus(kIONetworkLinkValid);
	
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
	//
	// This happens before ::stop, but after some of the read jobs fail
	// with kIOReturnNotResponding (and some of the writers might fail,
	// too).  ::disable happens sometime after we get done here, too --
	// potentially invoked by super::willTerminate.
	
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

bool HoRNDIS::openUSBInterfaces(IOService *provider) {
	if (fProbeConfigVal == 0) {
		// Must have been set by 'probe' before 'start' function call:
		LOG(V_ERROR, "'fProbeConfigVal' has not been set, bailing out");
		return false;
	}

	IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
	if (device) {
		// Set the device configuration, so we can start looking at the interfaces:
		if (device->setConfiguration(fProbeConfigVal, false) != kIOReturnSuccess) {
			LOG(V_ERROR, "Cannot set the USB Device configuration");
			return false;
		}
	} else {
		IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, provider);
		if (iface == NULL) {
			LOG(V_ERROR, "start: BUG unexpected provider class");
			return false;
		}
		device = iface->getDevice();
		// Make sure it's the one we care about:
		bool match = iface->getConfigurationDescriptor()->bConfigurationValue == fProbeConfigVal
			&& iface->getInterfaceDescriptor()->bInterfaceNumber == fProbeCommIfNum;
		if (!match) {
			LOG(V_ERROR, "BUG! Did we see a different provider in probe?");
			return false;
		}
	}

	{  // Now, find the interfaces:
		OSIterator *iterator = device->getChildIterator(gIOServicePlane);
		OSObject *obj = NULL;
		while(iterator != NULL && (obj = iterator->getNextObject()) != NULL) {
			IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, obj);
			if (iface == NULL) {
				continue;
			}
			if (iface->getConfigurationDescriptor()->bConfigurationValue !=
					fProbeConfigVal) {
				continue;
			}
			const InterfaceDescriptor *desc = iface->getInterfaceDescriptor();
			uint8_t ifaceNum = desc->bInterfaceNumber;
			if (!fCommInterface && ifaceNum == fProbeCommIfNum) {
				LOG(V_DEBUG, "Found control interface: %d/%d/%d, opening",
					desc->bInterfaceClass, desc->bInterfaceSubClass,
					desc->bInterfaceProtocol);
				if (!iface->open(this)) {
					LOG(V_ERROR, "Could not open RNDIS control interface");
					return false;
				}
				// Note, we retain AFTER opening the interface, because once
				// 'fCommInterface' is set, the 'closeUSBInterfaces' would
				// always try to close it before releasing:
				fCommInterface = retainT(iface);
			} else if (ifaceNum == fProbeCommIfNum + 1) {
				LOG(V_DEBUG, "Found data interface: %d/%d/%d, opening",
					desc->bInterfaceClass, desc->bInterfaceSubClass,
					desc->bInterfaceProtocol);
				if (!iface->open(this)) {
					LOG(V_ERROR, "Could not open RNDIS data interface");
					return false;
				}
				// open before retain, see above:
				fDataInterface = retainT(iface);
				break;  // We should be done by now.
			}
		}
		OSSafeReleaseNULL(iterator);
	}

	// WARNING, it is a WRONG idea to attach 'fDataInterface' as a second
	// provider, because both providers would be calling 'willTerminate', and
	// 'stop' methods, resulting in chaos.

	if (!fCommInterface || !fDataInterface) {
		LOG(V_ERROR, "could not find the required interfaces, despite seeing "
			"their descriptors during 'probe' method call");
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
	{  // Check if this is a device-based matching:
		IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
		if (device) {
			return probeDevice(device, score);
		}
	}
	
	IOUSBHostInterface *controlIf = OSDynamicCast(IOUSBHostInterface, provider);
 	if (controlIf == NULL) {
		LOG(V_ERROR, "unexpected provider class (wrong Info.plist)");
		return NULL;
	}

	const InterfaceDescriptor *desc = controlIf->getInterfaceDescriptor();
	LOG(V_DEBUG, "Interface-based matching, probing for device '%s', "
		"interface %d/%d/%d", controlIf->getDevice()->getName(),
		desc->bInterfaceClass, desc->bInterfaceSubClass,
		desc->bInterfaceProtocol);
	if (!isRNDISControlInterface(controlIf->getInterfaceDescriptor())) {
		LOG(V_ERROR, "not RNDIS control interface (wrong Info.plist)");
		return NULL;
	}

	const ConfigurationDescriptor *configDesc =
		controlIf->getConfigurationDescriptor();
	const InterfaceDescriptor *dataDesc =
		StandardUSB::getNextInterfaceDescriptor(configDesc, desc);
	bool match = isCDCDataInterface(dataDesc) &&
		(dataDesc->bInterfaceNumber == desc->bInterfaceNumber + 1);
	if (!match) {
		LOG(V_DEBUG, "Could not find CDC data interface right after control");
		return NULL;
	}
	fProbeConfigVal = configDesc->bConfigurationValue;
	fProbeCommIfNum = desc->bInterfaceNumber;
	*score += 100000;
	return this;
}

IOService *HoRNDIS::probeDevice(IOUSBHostDevice *device, SInt32 *score) {
	const DeviceDescriptor *desc = device->getDeviceDescriptor();
	LOG(V_DEBUG, "Device-based matching, probing: '%s', %d/%d/%d",
		device->getName(), desc->bDeviceClass, desc->bDeviceSubClass,
		desc->bDeviceProtocol);
	// Look through all configurations and find the one we want:
	for (int i = 0; i < desc->bNumConfigurations; i++) {
		const ConfigurationDescriptor *configDesc =
			device->getConfigurationDescriptor(i);
		if (configDesc == NULL) {
			LOG(V_ERROR, "Cannot get device's configuration descriptor");
			return NULL;
		}
		int controlIfNum = INT16_MAX;  // Definitely invalid interface number.
		bool foundData = false;
		const InterfaceDescriptor *intDesc = NULL;
		while((intDesc = StandardUSB::getNextInterfaceDescriptor(configDesc, intDesc)) != NULL) {
			// If this is a device-level match, check for the control interface:
			if (isRNDISControlInterface(intDesc)) {  // Just check them all.
				controlIfNum = intDesc->bInterfaceNumber;
				continue;
			}
		
			// We check for data interface AND make sure it follows directly the
			// control interface. Note the condition below would only trigger
			// if we previously found an appropriate 'controlIfNum':
			if (isCDCDataInterface(intDesc) &&
				intDesc->bInterfaceNumber == controlIfNum + 1) {
				foundData = true;
				break;
			}
		}
		if (foundData) {
			// We've found it! Save the information and return:
			fProbeConfigVal = configDesc->bConfigurationValue;
			fProbeCommIfNum = controlIfNum;
			*score += 10000;
			return this;
		}
	}

	// Did not find any interfaces we can use:
	LOG(V_DEBUG, "The device '%s' does not contain the required interfaces: "
			"it is not for us", device->getName());
	return NULL;
}

/***** Ethernet interface bits *****/

/* We need our own createInterface (overriding the one in IOEthernetController) 
 * because we need our own subclass of IOEthernetInterface.  Why's that, you say?
 * Well, we need that because that's the only way to set a different default MTU,
 * because it seems like MacOS code just assumes that any EthernetController
 * driver must be able to handle al leaset 1500-byte Ethernet payload.
 * Sigh...
 * The MTU-limiting code may never come into play though, because the devices
 * I've seen have "max_transfer_size" large enough to accomodate a max-length 
 * Ethernet frames. */

bool HoRNDISInterface::init(IONetworkController *controller, int mtu) {
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
	HoRNDISInterface *netif = new HoRNDISInterface;
	
	if (!netif) {
		return NULL;
	}

	int mtuLimit = maxOutTransferSize
		- (int)sizeof(rndis_data_hdr)
		- 14;  // Size of ethernet header (no QLANs). Checksum is not included.

	if (!netif->init(this, min(ETHERNET_MTU, mtuLimit))) {
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

	// The 'registerService' should be called by 'attachInterface' (with second
	// parameter set to true). No need to do it here.
	
	return true;	
}

/***** Interface enable and disable logic *****/
HoRNDIS::ReentryLocker::ReentryLocker(IOCommandGate *inGate, bool &inGuard)
		: gate(inGate), entryGuard(inGuard), result(kIOReturnSuccess) {
	// Note, please see header comment for the motivation behind
	// 'ReentryLocker' and its high-level functionality.
	// Wait until we exit the previously-entered enable or disable method:
	while (entryGuard) {
		LOG(V_DEBUG, "Delaying the re-entered call");
		result = gate->commandSleep(&entryGuard);
		// If "commandSleep" has failed, stop immediately, and don't
		// touch the 'entryGuard':
		if (isInterrupted()) {
			return;
		}
	}
	entryGuard = true;  // Mark the entry into one of the protected methods.
}

HoRNDIS::ReentryLocker::~ReentryLocker() {
	if (!isInterrupted()) {
		entryGuard = false;
		gate->commandWakeup(&entryGuard);
	}
}

static IOReturn loopClearPipeStall(IOUSBHostPipe *pipe) {
	IOReturn rc = kUSBHostReturnPipeStalled;
	int count = 0;
	// For some reason, 'clearStall' may keep on returning
	// kUSBHostReturnPipeStalled many times, before finally returning success
	// (Android keeps on sending packtes, each generating a stall?).
	const int NUM_RETRIES = 1000;
	for (; count < NUM_RETRIES && rc == kUSBHostReturnPipeStalled; count++) {
		rc = pipe->clearStall(true);
	}
	LOG(V_DEBUG, "Called 'clearStall' %d times", count);
	return rc;
}

/*!
 * Calls 'pipe->io', and if there is a stall, tries to clear that 
 * stall and calls it again.
 */
static inline IOReturn robustIO(IOUSBHostPipe *pipe, pipebuf_t *buf,
	uint32_t len) {
	IOReturn rc = pipe->io(buf->mdp, len, &buf->comp);
	if (rc == kUSBHostReturnPipeStalled) {
		LOG(V_DEBUG, "USB Pipe is stalled. Trying to clear ...");
		rc = loopClearPipeStall(pipe);
		// If clearing the stall succeeded, try the IO operation again:
		if (rc == kIOReturnSuccess) {
			LOG(V_DEBUG, "Cleared USB Stall, Retrying the operation");
			rc = pipe->io(buf->mdp, len, &buf->comp);
		}
	}
	return rc;
}

/* Contains buffer alloc and dealloc, notably.  Why do that here?  
   Not just because that's what Apple did. We don't want to consume these 
   resources when the interface is sitting disabled and unused. */
IOReturn HoRNDIS::enable(IONetworkInterface *netif) {
	IOReturn rtn = kIOReturnSuccess;

	LOG(V_DEBUG, "begin for thread_id=%lld", thread_tid(current_thread()));
	ReentryLocker locker(this, fEnableDisableInProgress);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (fNetifEnabled) {
		LOG(V_DEBUG, "Repeated call (thread_id=%lld), returning success",
			thread_tid(current_thread()));
		return kIOReturnSuccess;
	}

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

	// The pipe stall clearning is not needed for the first "enable" call after
	// pugging in the device, but it becomes necessary when "disable" is called
	// after that, followed by another "enable". This happens when user runs
	// "sudo ifconfig <netif> down", followed by "sudo ifconfig <netif> up"
	LOG(V_DEBUG, "Clearing potential Pipe stalls on Input and Output pipes");
	loopClearPipeStall(fInPipe);
	loopClearPipeStall(fOutPipe);

	// We can now perform reads and writes between Network stack and USB device:
	fReadyToTransfer = true;
	
	// Kick off the read requests:
	for (int i = 0; i < N_IN_BUFS; i++) {
		pipebuf_t &inbuf = inbufs[i];
		inbuf.comp.owner = this;
		inbuf.comp.action = dataReadComplete;
		inbuf.comp.parameter = &inbuf;

		rtn = robustIO(fInPipe, &inbuf, (uint32_t)inbuf.mdp->getLength());
		if (rtn != kIOReturnSuccess) {
			LOG(V_ERROR, "Failed to start the first read: %08x\n", rtn);
			goto bailout;
		}
		fCallbackCount++;
	}

	// Tell the world that the link is up...
	if (!setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
			getCurrentMedium())) {
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
	LOG(V_NOTE, "completed (thread_id=%lld): RNDIS network interface '%s' "
		"should be live now", thread_tid(current_thread()), netif->getName());
	
	return kIOReturnSuccess;
	
bailout:
	disableImpl();
	return rtn;
}

void HoRNDIS::disableNetworkQueue() {
	// Disable the queue (no more outputPacket),
	// and then flush everything in the queue.
	getOutputQueue()->stop();
	getOutputQueue()->flush();
	getOutputQueue()->setCapacity(0);
}

IOReturn HoRNDIS::disable(IONetworkInterface *netif) {
	LOG(V_DEBUG, "begin for thread_id=%lld", thread_tid(current_thread()));
	// This function can be called as a consequence of:
	//  1. USB Disconnect
	//  2. Some action, while the device is up and running
	//     (e.g. "ifconfig en6 down").
	// In the second case, we'll need to do more cleanup:
	// ask the RNDIS device to stop transmitting, and abort the callbacks.
	//

	ReentryLocker locker(this, fEnableDisableInProgress);
	if (locker.isInterrupted()) {
		LOG(V_ERROR, "Waiting interrupted");
		return locker.getResult();
	}

	if (!fNetifEnabled) {
		LOG(V_DEBUG, "Repeated call (thread_id=%lld)", thread_tid(current_thread()));
		return kIOReturnSuccess;
	}

	disableImpl();

	LOG(V_DEBUG, "completed (thread_id=%lld)", thread_tid(current_thread()));
	return kIOReturnSuccess;
}

void HoRNDIS::disableImpl() {
	disableNetworkQueue();

	// Stop the the new transfers. The code below would cancel the pending ones:
	fReadyToTransfer = false;

	// If the device has not been disconnected, ask it to stop xmitting:
	if (fCommInterface) {
		rndisSetPacketFilter(0);
	}

	// Again, based on Thunderbolt Ethernet controller traces.
	// It sets the link status to 0x1 in the disable call:
	setLinkStatus(kIONetworkLinkValid, 0);

	// If USB interfaces are still up, abort the reader and writer:
	if (fInPipe) {
		fInPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	if (fOutPipe) {
		fOutPipe->abort(IOUSBHostIOSource::kAbortSynchronous,
			kIOReturnAborted, NULL);
	}
	// Make sure all the callbacks have exited:
	LOG(V_DEBUG, "Callback count: %d. If not zero, delaying ...",
		fCallbackCount);
	while (fCallbackCount > 0) {
		// No timeout: in our callbacks we trust!
		getCommandGate()->commandSleep(&fCallbackCount);
	}
	LOG(V_DEBUG, "All callbacks exited");

	// Release all resources
	releaseResources();

	fNetifEnabled = false;
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

IOOutputQueue *HoRNDIS::createOutputQueue() {
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
		*filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast
			| kIOPacketFilterPromiscuous | kIOPacketFilterMulticast
			| kIOPacketFilterMulticastAll;
	} else {
		rtn = super::getPacketFilters(group, filters);
	}

	return rtn;
}

IOReturn HoRNDIS::getMaxPacketSize(UInt32 *maxSize) const {
	IOReturn rc = super::getMaxPacketSize(maxSize);
	if (rc != kIOReturnSuccess) {
		return rc;
	}
	// The max packet size is limited by RNDIS max transfer size:
	*maxSize = min(*maxSize, maxOutTransferSize - sizeof(rndis_data_hdr));
	LOG(V_DEBUG, "returning %d", *maxSize);
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
	
	buf = IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!buf) {
		return kIOReturnNoMemory;
	}

	// WARNING: Android devices may randomly-generate RNDIS MAC address.
	// The function may return different results for the same device.

	rv = rndisQuery(buf, OID_802_3_PERMANENT_ADDRESS, 48, (void **) &bp, &rlen);
	if (rv < 0) {
		LOG(V_ERROR, "getHardwareAddress OID failed?");
		IOFreeAligned(buf, RNDIS_CMD_BUF_SZ);
		return kIOReturnIOError;
	}
	LOG(V_DEBUG, "MAC Address %02x:%02x:%02x:%02x:%02x:%02x -- rlen %d",
	      bp[0], bp[1], bp[2], bp[3], bp[4], bp[5],
	      rlen);
	
	for (i=0; i<6; i++) {
		ea->bytes[i] = bp[i];
	}
	
	IOFreeAligned(buf, RNDIS_CMD_BUF_SZ);
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setMulticastMode(bool active) {
	// For 'real' RNDIS devices, this should toggle
	// RNDIS_PACKET_TYPE_ALL_MULTICAST or RNDIS_PACKET_TYPE_MULTICAST
	// via 'rndisSetPacketFilter', but Android/Linux kernel
	// doesn't care, so why should we?
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setMulticastList(IOEthernetAddress *addrs,
	                            UInt32             count) {
	// "Honey Badger don't care". We're using MULTICAST_ALL flag: everything
	// gets passed through.
	return kIOReturnSuccess;
}

IOReturn HoRNDIS::setPromiscuousMode(bool active) {
	// Similar to 'setMulticastMode'.
	// XXX This actually needs to get passed down to support 'real'
	//  RNDIS devices, but it will work okay for Android devices.
	return kIOReturnSuccess;
}

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

	const uint32_t transmitLength = (uint32_t)(pktlen + sizeof(rndis_data_hdr));
	
	if (transmitLength > maxOutTransferSize) {
		LOG(V_ERROR, "packet too large (%ld bytes, maximum can transmit %ld)",
			pktlen, maxOutTransferSize - sizeof(rndis_data_hdr));
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
	
	ior = robustIO(fOutPipe, &outbufs[poolIndx], transmitLength);
	if (ior != kIOReturnSuccess) {
		if (isTransferStopStatus(ior)) {
			LOG(V_DEBUG, "WRITER: The device was possibly disconnected: ignoring the error");
		} else {
			LOG(V_ERROR, "write failed: %08x", ior);
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
		// Write error. In case of pipe stall,
		// we shall clear it on the next transmit.
		LOG(V_ERROR, "I/O error: %08x", rc);
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
	}
	
	// Queue the next one up.
	ior = robustIO(me->fInPipe, inbuf, (uint32_t)inbuf->mdp->getLength());
	if (ior == kIOReturnSuccess) {
		return;  // Callback is in-progress.
	}

	LOG(V_ERROR, "READER STOPPED: USB failure trying to read: %08x", ior);
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
	if (!fCommInterface) {  // Safety: make sure 'fCommInterface' is valid.
		LOG(V_ERROR, "fCommInterface is NULL, bailing out");
		return kIOReturnError;
	}
	const uint8_t ifNum = fCommInterface->getInterfaceDescriptor()->bInterfaceNumber;

	if (buf->msg_type != RNDIS_MSG_HALT && buf->msg_type != RNDIS_MSG_RESET) {
		// No need to lock here: multi-threading does not even come close
		// (IOWorkLoop + IOGate are at our service):
		buf->request_id = cpu_to_le32(rndisXid++);
		if (!buf->request_id) {
			buf->request_id = cpu_to_le32(rndisXid++);
		}
		
		LOG(V_DEBUG, "Generated xid: %d", le32_to_cpu(buf->request_id));
	}
	const uint32_t old_msg_type = buf->msg_type;
	const uint32_t old_request_id = buf->request_id;
	
	{
		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionOut |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = le32_to_cpu(buf->msg_len);
	
		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, buf, bytes_transferred)) != kIOReturnSuccess) {
			LOG(V_DEBUG, "Device request send error");
			return rc;
		}
		if (bytes_transferred != rq.wLength) {
			LOG(V_DEBUG, "Incomplete device transfer");
			return kIOReturnError;
		}
	}

	// The RNDIS control messages are done via 'deviceRequest' - issue control
	// transfers on the device's default endpoint. Per [MSDN-RNDISUSB], if
	// a device is not ready (for some reason) to reply with the actual data,
	// it shall send a one-byte reply indicating an error, rather than stall
	// the control pipe. The retry loop below is a hackish way of waiting
	// for the reply.
	//
	// Per [MSDN-RNDISUSB], once the driver sends a OUT device transfer, it
	// should wait for a notification on the interrupt endpoint from
	// fCommInterface, and only then perform a device request to retrieve
	// the result. Whether Android does that correctly is something I need to
	// investigate.
	//
	// Also, RNDIS specifies that the device may be sending
	// REMOTE_NDIS_INDICATE_STATUS_MSG on its own. How much this applies to
	// Android or embedded Linux devices needs to be investigated.
	//
	// Reference:
	// https://docs.microsoft.com/en-us/windows-hardware/drivers/network/control-channel-characteristics

	// Now we wait around a while for the device to get back to us.
	int count;
	for (count = 0; count < 10; count++) {
		DeviceRequest rq;
		rq.bmRequestType = kDeviceRequestDirectionIn |
			kDeviceRequestTypeClass | kDeviceRequestRecipientInterface;
		rq.bRequest = USB_CDC_GET_ENCAPSULATED_RESPONSE;
		rq.wValue = 0;
		rq.wIndex = ifNum;
		rq.wLength = RNDIS_CMD_BUF_SZ;

		// Make sure 'fCommInterface' was not taken away from us while
		// we were doing synchronous IO:
		if (!fCommInterface) {
			LOG(V_ERROR, "fCommInterface was closed, bailing out");
			return kIOReturnError;
		}
		uint32_t bytes_transferred;
		if ((rc = fCommInterface->deviceRequest(rq, buf, bytes_transferred)) != kIOReturnSuccess) {
			return rc;
		}

		if (bytes_transferred < 12) {
			LOG(V_ERROR, "short read on control request?");
			IOSleep(20);
			continue;
		}
		
		if (buf->msg_type == (old_msg_type | RNDIS_MSG_COMPLETION)) {
			if (buf->request_id == old_request_id) {
				if (buf->msg_type == RNDIS_MSG_RESET_C) {
					// This is probably incorrect: the RESET_C does not have
					// 'request_id', but we don't issue resets => don't care.
					break;
				}
				if (buf->status != RNDIS_STATUS_SUCCESS) {
					LOG(V_ERROR, "RNDIS command returned status %08x",
						le32_to_cpu(buf->status));
					rc = kIOReturnError;
					break;
				}
				if (le32_to_cpu(buf->msg_len) != bytes_transferred) {
					LOG(V_ERROR, "Message Length mismatch: expected: %d, actual: %d",
						le32_to_cpu(buf->msg_len), bytes_transferred);
					rc = kIOReturnError;
					break;
				}
				LOG(V_DEBUG, "RNDIS command completed");
				break;
			} else {
				LOG(V_ERROR, "RNDIS return had incorrect xid?");
			}
		} else {
			if (buf->msg_type == RNDIS_MSG_INDICATE) {
				LOG(V_ERROR, "unsupported: RNDIS_MSG_INDICATE");	
			} else if (buf->msg_type == RNDIS_MSG_INDICATE) {
				LOG(V_ERROR, "unsupported: RNDIS_MSG_KEEPALIVE");
			} else {
				LOG(V_ERROR, "unexpected msg type %08x, msg_len %08x",
					le32_to_cpu(buf->msg_type), le32_to_cpu(buf->msg_len));
			}
		}
		
		IOSleep(20);
	}
	if (count == 10) {
		LOG(V_ERROR, "command timed out?");
		return kIOReturnTimeout;
	}

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
	
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "RNDIS_MSG_QUERY failure? %08x", rc);
		return rc;
	}
	
	off = le32_to_cpu(u.get_c->offset);
	len = le32_to_cpu(u.get_c->len);
	LOG(V_DEBUG, "RNDIS query completed");
	
	if ((8 + off + len) > RNDIS_CMD_BUF_SZ) {
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
		struct rndis_msg_hdr *hdr;
		struct rndis_init *init;
		struct rndis_init_c *init_c;
	} u;
	
	u.hdr = (rndis_msg_hdr *)IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!u.hdr) {
		LOG(V_ERROR, "out of memory?");
		return false;
	}
	
	u.init->msg_type = RNDIS_MSG_INIT;
	u.init->msg_len = cpu_to_le32(sizeof *u.init);
	u.init->major_version = cpu_to_le32(1);
	u.init->minor_version = cpu_to_le32(0);
	// This is the maximum USB transfer the device is allowed to make to host:
	u.init->max_transfer_size = IN_BUF_SIZE;
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "INIT not successful?");
		IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
		return false;
	}

	if (fCommInterface) {  // Safety: don't accesss 'fCommInterface if NULL.
		LOG(V_NOTE, "'%s': ver=%d.%d, max_packets_per_transfer=%d, "
			"max_transfer_size=%d, packet_alignment=2^%d",
			fCommInterface->getDevice()->getName(),
			le32_to_cpu(u.init_c->major_version),
			le32_to_cpu(u.init_c->minor_version),
			le32_to_cpu(u.init_c->max_packets_per_transfer),
			le32_to_cpu(u.init_c->max_transfer_size),
			le32_to_cpu(u.init_c->packet_alignment));
	}

	maxOutTransferSize = le32_to_cpu(u.init_c->max_transfer_size);
	// For now, let's limit the maxOutTransferSize by the Output Buffer size.
	// If we implement transmitting multiple PDUs in a single USB transfer,
	// we may want to size the output buffers based on
	// "u.init_c->max_transfer_size".
	maxOutTransferSize = min(maxOutTransferSize, OUT_BUF_SIZE);
	
	IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
	
	return true;
}

bool HoRNDIS::rndisSetPacketFilter(uint32_t filter) {
	union {
		struct rndis_msg_hdr *hdr;
		struct rndis_set *set;
		struct rndis_set_c *set_c;
	} u;
	int rc;
	
	u.hdr = (rndis_msg_hdr *)IOMallocAligned(RNDIS_CMD_BUF_SZ, sizeof(void *));
	if (!u.hdr) {
		LOG(V_ERROR, "out of memory?");
		return false;;
	}
	
	memset(u.set, 0, sizeof *u.set);
	u.set->msg_type = RNDIS_MSG_SET;
	u.set->msg_len = cpu_to_le32(4 + sizeof *u.set);
	u.set->oid = OID_GEN_CURRENT_PACKET_FILTER;
	u.set->len = cpu_to_le32(4);
	u.set->offset = cpu_to_le32((sizeof *u.set) - 8);
	*(uint32_t *)(u.set + 1) = filter;
	
	rc = rndisCommand(u.hdr, RNDIS_CMD_BUF_SZ);
	if (rc != kIOReturnSuccess) {
		LOG(V_ERROR, "SET not successful?");
		IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
		return false;
	}
	
	IOFreeAligned(u.hdr, RNDIS_CMD_BUF_SZ);
	
	return true;
}
