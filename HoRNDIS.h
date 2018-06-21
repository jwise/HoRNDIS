/* HoRNDIS.h
 * Declaration of IOKit-derived classes
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

#include <machine/limits.h>			/* UINT_MAX */
#include <libkern/OSByteOrder.h>
#include <libkern/OSTypes.h>

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMessage.h>

#include <IOKit/usb/IOUSBHostFamily.h>
#include <IOKit/usb/IOUSBHostPipe.h>

extern "C"
{
	#include <sys/param.h>
	#include <sys/mbuf.h>
}

#define cpu_to_le32(x) OSSwapHostToLittleInt32(x)
#define le32_to_cpu(x) OSSwapLittleToHostInt32(x)

#define TRANSMIT_QUEUE_SIZE     256
#define OUT_BUF_SIZE			4096
// Use large input buffer(s): some Android versions like to make large
// transfers, and we need to read whole thing in a single IOUSBHostPipe::io
// call; else, we'd have to merge buffers, and that's a real pain in the butt.
#define IN_BUF_SIZE				16384

#define N_OUT_BUFS         4
// The N_IN_BUFS value should either be 1 or 2.
// 2 - double-buffering enabled, 1 - double-buffering disabled: single reader.
// NOTE: surprisingly, single-buffer overall performs better, probably due to
// less contention on the USB2 bus, which is half-duplex.
#define N_IN_BUFS			1
#define MAX_MTU 1536

/***** RNDIS definitions -- from linux/include/linux/usb/rndis_host.h ****/

#define RNDIS_CMD_BUF_SZ 1052

struct rndis_msg_hdr {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
} __attribute__((packed));

struct rndis_data_hdr {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t data_offset;
	uint32_t data_len;
	
	uint32_t oob_data_offset;
	uint32_t oob_data_len;
	uint32_t num_oob;
	uint32_t packet_data_offset;
	
	uint32_t packet_data_len;
	uint32_t vc_handle;
	uint32_t reserved;
} __attribute__((packed));

struct rndis_query {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t oid;
	uint32_t len;
	uint32_t offset;
	uint32_t handle;
} __attribute__((packed));

struct rndis_query_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
	uint32_t len;
	uint32_t offset;
} __attribute__((packed));

struct rndis_init {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t major_version;
	uint32_t minor_version;
	uint32_t mtu;
} __attribute__((packed));

struct rndis_init_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
	uint32_t major_version;
	uint32_t minor_version;
	uint32_t device_flags;
	uint32_t medium;
	uint32_t max_packets_per_message;
	uint32_t mtu;
	uint32_t packet_alignment;
	uint32_t af_list_offset;
	uint32_t af_list_size;
} __attribute__((packed));

struct rndis_set {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t oid;
	uint32_t len;
	uint32_t offset;
	uint32_t handle;
} __attribute__((packed));

struct rndis_set_c {
	uint32_t msg_type;
	uint32_t msg_len;
	uint32_t request_id;
	uint32_t status;
} __attribute__((packed));

#define RNDIS_MSG_COMPLETION                    cpu_to_le32(0x80000000)
#define RNDIS_MSG_PACKET                        cpu_to_le32(0x00000001) /* 1-N packets */
#define RNDIS_MSG_INIT                          cpu_to_le32(0x00000002)
#define RNDIS_MSG_INIT_C                        (RNDIS_MSG_INIT|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_HALT                          cpu_to_le32(0x00000003)
#define RNDIS_MSG_QUERY                         cpu_to_le32(0x00000004)
#define RNDIS_MSG_QUERY_C                       (RNDIS_MSG_QUERY|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_SET                           cpu_to_le32(0x00000005)
#define RNDIS_MSG_SET_C                         (RNDIS_MSG_SET|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_RESET                         cpu_to_le32(0x00000006)
#define RNDIS_MSG_RESET_C                       (RNDIS_MSG_RESET|RNDIS_MSG_COMPLETION)
#define RNDIS_MSG_INDICATE                      cpu_to_le32(0x00000007)
#define RNDIS_MSG_KEEPALIVE                     cpu_to_le32(0x00000008)
#define RNDIS_MSG_KEEPALIVE_C                   (RNDIS_MSG_KEEPALIVE|RNDIS_MSG_COMPLETION)

#define RNDIS_STATUS_SUCCESS                    cpu_to_le32(0x00000000)
#define RNDIS_STATUS_FAILURE                    cpu_to_le32(0xc0000001)
#define RNDIS_STATUS_INVALID_DATA               cpu_to_le32(0xc0010015)
#define RNDIS_STATUS_NOT_SUPPORTED              cpu_to_le32(0xc00000bb)
#define RNDIS_STATUS_MEDIA_CONNECT              cpu_to_le32(0x4001000b)
#define RNDIS_STATUS_MEDIA_DISCONNECT           cpu_to_le32(0x4001000c)
#define RNDIS_STATUS_MEDIA_SPECIFIC_INDICATION  cpu_to_le32(0x40010012)

#define RNDIS_PHYSICAL_MEDIUM_UNSPECIFIED       cpu_to_le32(0x00000000)
#define RNDIS_PHYSICAL_MEDIUM_WIRELESS_LAN      cpu_to_le32(0x00000001)
#define RNDIS_PHYSICAL_MEDIUM_CABLE_MODEM       cpu_to_le32(0x00000002)
#define RNDIS_PHYSICAL_MEDIUM_PHONE_LINE        cpu_to_le32(0x00000003)
#define RNDIS_PHYSICAL_MEDIUM_POWER_LINE        cpu_to_le32(0x00000004)
#define RNDIS_PHYSICAL_MEDIUM_DSL               cpu_to_le32(0x00000005)
#define RNDIS_PHYSICAL_MEDIUM_FIBRE_CHANNEL     cpu_to_le32(0x00000006)
#define RNDIS_PHYSICAL_MEDIUM_1394              cpu_to_le32(0x00000007)
#define RNDIS_PHYSICAL_MEDIUM_WIRELESS_WAN      cpu_to_le32(0x00000008)
#define RNDIS_PHYSICAL_MEDIUM_MAX               cpu_to_le32(0x00000009)

#define OID_802_3_PERMANENT_ADDRESS             cpu_to_le32(0x01010101)
#define OID_GEN_MAXIMUM_FRAME_SIZE              cpu_to_le32(0x00010106)
#define OID_GEN_CURRENT_PACKET_FILTER           cpu_to_le32(0x0001010e)
#define OID_GEN_PHYSICAL_MEDIUM                 cpu_to_le32(0x00010202)

/* packet filter bits used by OID_GEN_CURRENT_PACKET_FILTER */
#define RNDIS_PACKET_TYPE_DIRECTED              cpu_to_le32(0x00000001)
#define RNDIS_PACKET_TYPE_MULTICAST             cpu_to_le32(0x00000002)
#define RNDIS_PACKET_TYPE_ALL_MULTICAST         cpu_to_le32(0x00000004)
#define RNDIS_PACKET_TYPE_BROADCAST             cpu_to_le32(0x00000008)
#define RNDIS_PACKET_TYPE_SOURCE_ROUTING        cpu_to_le32(0x00000010)
#define RNDIS_PACKET_TYPE_PROMISCUOUS           cpu_to_le32(0x00000020)
#define RNDIS_PACKET_TYPE_SMT                   cpu_to_le32(0x00000040)
#define RNDIS_PACKET_TYPE_ALL_LOCAL             cpu_to_le32(0x00000080)
#define RNDIS_PACKET_TYPE_GROUP                 cpu_to_le32(0x00001000)
#define RNDIS_PACKET_TYPE_ALL_FUNCTIONAL        cpu_to_le32(0x00002000)
#define RNDIS_PACKET_TYPE_FUNCTIONAL            cpu_to_le32(0x00004000)
#define RNDIS_PACKET_TYPE_MAC_FRAME             cpu_to_le32(0x00008000)

/* default filter used with RNDIS devices */
#define RNDIS_DEFAULT_FILTER ( \
        RNDIS_PACKET_TYPE_DIRECTED | \
        RNDIS_PACKET_TYPE_BROADCAST | \
        RNDIS_PACKET_TYPE_ALL_MULTICAST | \
        RNDIS_PACKET_TYPE_PROMISCUOUS)

#define USB_CDC_SEND_ENCAPSULATED_COMMAND       0x00
#define USB_CDC_GET_ENCAPSULATED_RESPONSE       0x01

/***** Actual class definitions *****/

typedef struct {
	IOBufferMemoryDescriptor *mdp;
	IOUSBHostCompletion comp;
} pipebuf_t;

class HoRNDIS : public IOEthernetController {
	OSDeclareDefaultStructors(HoRNDIS);	// Constructor & Destructor stuff

private:
	/*!
	 * This class protects "enable" and "disable" calls against re-entry.
	 * Unlike start/stop calls, that are triggered by a single provider,
	 * the enable/disable calls can be triggered by potentially more than one
	 * interface client, as well as user's actions, e.g. "ifconfig en6 up" -
	 * again by potentially multiple "ifconfig" processes running in parallel.
	 * Even though the calls to this class are protected by the IOCommandGate,
	 * synchronous USB transfers release the gate (by using 
	 * IOCommandGate::commandSleep), allowing another enable/disable call
	 * to "sneak in". This class allows to delay additional enable/disable
	 * calls until the first one completes.
	 */
	class EnableDisableLocker {
	public:
		EnableDisableLocker(HoRNDIS *inInst);
		~EnableDisableLocker();
		IOReturn getResult() const { return result; }
		bool isInterrupted() const { return result != kIOReturnSuccess; }
	private:
		HoRNDIS *const inst;
		IOReturn result;
	};
		
	IOEthernetInterface *fNetworkInterface;
	IONetworkStats *fpNetStats;

	bool fReadyToTransfer;  // Ready to transmit: Android <-> MAC.
	// Set to true when 'enable' succeeds, and
	// set to false when 'disable' succeeds:
	bool fNetifEnabled;
	bool fEnableDisableInProgress;  // Guards against re-entry
	bool fDataDead;
	// fCallbackCount is the number of callbacks concurrently running
	// (possibly offset by a certain value).
	//  - Every successful async API call shall "fCallbackCount++".
	//  - Every time we exit the completion without making another call:
	//    'callbackExit()'.
	int fCallbackCount;
	
	// USB Communication:
	IOUSBHostInterface *fCommInterface;
	IOUSBHostInterface *fDataInterface;
	
	IOUSBHostPipe *fInPipe;
	IOUSBHostPipe *fOutPipe;
	
	uint32_t rndisXid;  // RNDIS request_id count.
	uint32_t mtu;  // Computed by 'rdisInit', based on device reply.

	pipebuf_t outbufs[N_OUT_BUFS];
	// Allow double-buffering to enable the best hardware utilization:
	pipebuf_t inbufs[N_IN_BUFS];
	uint16_t outbufStack[N_OUT_BUFS];
	int numFreeOutBufs;

	void callbackExit();
	static void dataWriteComplete(void *obj, void *param, IOReturn ior, UInt32 transferred);
	static void dataReadComplete(void *obj, void *param, IOReturn ior, UInt32 transferred);

	bool rndisInit();
	IOReturn rndisCommand(struct rndis_msg_hdr *buf, int buflen);
	int rndisQuery(void *buf, uint32_t oid, uint32_t in_len, void **reply, int *reply_len);
	bool rndisSetPacketFilter(uint32_t filter);

	bool openUSBInterfaces(IOUSBHostInterface *controlInterface);
	void closeUSBInterfaces();
	void disableNetworkQueue();
	void disableImpl();

	bool createMediumTables(const IONetworkMedium **primary);
	bool allocateResources(void);
	void releaseResources(void);
	bool createNetworkInterface(void);

	void receivePacket(void *packet, UInt32 size);

public:
	// IOKit overrides
	virtual bool init(OSDictionary *properties = 0) override;
	virtual void free() override;
	virtual IOService *probe(IOService *provider, SInt32 *score) override;
	virtual bool start(IOService *provider) override;
	virtual bool willTerminate(IOService * provider, IOOptionBits options) override;
	virtual void stop(IOService *provider) override;

	// virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) override;
	
	// IOEthernetController overrides
	virtual IOOutputQueue *createOutputQueue(void) override;
	virtual IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
	virtual IOReturn getMaxPacketSize(UInt32 * maxSize) const override;
	virtual IOReturn getPacketFilters(const OSSymbol *group,
									  UInt32 *filters ) const  override;
	virtual IONetworkInterface *createInterface() override;
	virtual bool configureInterface(IONetworkInterface *netif) override;
	
	virtual IOReturn enable(IONetworkInterface *netif) override;
	virtual IOReturn disable(IONetworkInterface *netif) override;
	virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
	//virtual IOReturn setPromiscuousMode(bool active) override;
	virtual UInt32 outputPacket(mbuf_t pkt, void *param) override;
};

class HoRNDISInterface : public IOEthernetInterface {
	OSDeclareDefaultStructors(HoRNDISInterface);
	int maxmtu;
public:
	virtual bool init(IONetworkController * controller, int mtu);
	virtual bool setMaxTransferUnit(UInt32 mtu) override;
};
