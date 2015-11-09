/* MicroDriver.h
 * Declaration of IOKit-derived classes
 * MicroDriver, a not-a-driver for Mac OS X
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

#include <machine/limits.h>			/* UINT_MAX */
#include <libkern/OSByteOrder.h>
#include <libkern/OSTypes.h>

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMessage.h>

#include <IOKit/pwr_mgt/RootDomain.h>

#include <IOKit/usb/IOUSBBus.h>
#include <IOKit/usb/IOUSBNub.h>
#include <IOKit/usb/IOUSBDevice.h>
#include <IOKit/usb/IOUSBPipe.h>
#include <IOKit/usb/USB.h>
#include <IOKit/usb/IOUSBInterface.h>

#include <UserNotification/KUNCUserNotifications.h>

extern "C"
{
	#include <sys/param.h>
	#include <sys/mbuf.h>
}

#define cpu_to_le32(x) (uint32_t)OSSwapHostToLittleInt32(x)
#define le32_to_cpu(x) (uint32_t)OSSwapLittleToHostInt32(x)

class MicroDriver : public IOService {
	OSDeclareDefaultStructors(MicroDriver);	// Constructor & Destructor stuff

private:
	bool fTerminate; // being terminated now (i.e., device being unplugged)
		
	IOUSBInterface *fCommInterface;
	IOUSBInterface *fDataInterface;
	
	bool openInterfaces();
	
public:
	IOUSBDevice *fpDevice;
	IOUSBInterface *fpInterface;

	// IOKit overrides
	virtual bool init(OSDictionary *properties = 0);
	virtual bool start(IOService *provider);
	virtual void stop(IOService *provider);
	virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0);
	virtual IOService *probe(IOService *provider, SInt32 *score);

	virtual IOReturn getHardwareAddress(IOEthernetAddress * addrP);
};

/* If there are other ways to get access to a device, we probably want them here. */
class MicroDriverUSBInterface : public MicroDriver {
	OSDeclareDefaultStructors(MicroDriverUSBInterface);
public:
	virtual bool start(IOService *provider);
};

class MicroDriverUSBDevice : public MicroDriver {
	OSDeclareDefaultStructors(MicroDriverUSBDevice);
public:
	virtual bool start(IOService *provider);
};

