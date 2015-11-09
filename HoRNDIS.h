/* HoRNDIS.h
 * Declaration of IOKit-derived classes
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

#define MYNAME "HoRNDIS"
#define V_PTR 0
#define V_DEBUG 1
#define V_NOTE 2
#define V_ERROR 3

#define DEBUGLEVEL V_NOTE
#define LOG(verbosity, s, ...) do { if (verbosity >= DEBUGLEVEL) IOLog(MYNAME ": %s: " s "\n", __func__, ##__VA_ARGS__); } while(0)

#define cpu_to_le32(x) (uint32_t)OSSwapHostToLittleInt32(x)
#define le32_to_cpu(x) (uint32_t)OSSwapLittleToHostInt32(x)

#define TRANSMIT_QUEUE_SIZE     256
#define MAX_BLOCK_SIZE		PAGE_SIZE

#define kPipeStalled       1

#define N_OUT_BUFS         16
#define OUT_BUF_MAX_TRIES  10 /* 50ms total */
#define OUT_BUF_WAIT_TIME  5000000 /* ns */

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
	bool inuse;
	IOBufferMemoryDescriptor *mdp;
	void *buf;
	IOUSBCompletion comp;
} pipebuf_t;

class HoRNDIS : public IOEthernetController {
	OSDeclareDefaultStructors(HoRNDIS);	// Constructor & Destructor stuff

private:
	IOEthernetInterface *fNetworkInterface;
	IONetworkStats *fpNetStats;
	
	OSDictionary *fMediumDict;

	bool fNetifEnabled;
	bool fDataDead;
	 
	IOUSBInterface *fCommInterface;
	IOUSBInterface *fDataInterface;
	
	IOUSBPipe *fInPipe;
	IOUSBPipe *fOutPipe;
	
	IOLock *xid_lock;
	uint32_t xid;
	uint32_t mtu;
	
	IOLock *outbuf_lock;
	pipebuf_t outbufs[N_OUT_BUFS];
	static void dataWriteComplete(void *obj, void *param, IOReturn ior, UInt32 remaining);

	pipebuf_t inbuf;
	static void dataReadComplete(void *obj, void *param, IOReturn ior, UInt32 remaining);

	bool rndisInit();
	int rndisCommand(struct rndis_msg_hdr *buf, int buflen);
	int rndisQuery(void *buf, uint32_t oid, uint32_t in_len, void **reply, int *reply_len);
	bool rndisSetPacketFilter(uint32_t filter);
	
	bool createMediumTables(void);
	bool allocateResources(void);
	void releaseResources(void);
	bool openEndpoints();
	bool createNetworkInterface(void);
	UInt32 outputPacket(mbuf_t pkt, void *param);
	IOReturn clearPipeStall(IOUSBPipe *thePipe);
	void receivePacket(void *packet, UInt32 size);
	
	IOWorkLoop *workloop;

public:
	// IOKit overrides
	virtual bool init(OSDictionary *properties = 0);
	virtual void free(void);
	
	// Called once matched and chosen as best provider
	virtual bool start(IOUSBDevice *device, IOUSBInterface *control, IOUSBInterface *data);
	virtual void stop(IOService *provider);
	
	virtual bool createWorkLoop();
	virtual IOWorkLoop *getWorkLoop() const;
	virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0);

	// IOEthernetController overrides
	virtual IOReturn enable(IONetworkInterface *netif);
	virtual IOReturn disable(IONetworkInterface *netif);
	virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters ) const;
	virtual IOReturn getMaxPacketSize(UInt32 * maxSize) const;
	virtual IOReturn selectMedium(const IONetworkMedium *medium);
	virtual IOReturn getHardwareAddress(IOEthernetAddress *addr);
	virtual IOReturn setPromiscuousMode(bool active);
	virtual IOOutputQueue *createOutputQueue(void);
	virtual bool configureInterface(IONetworkInterface *netif);
	virtual IONetworkInterface *createInterface();
};

/* If there are other ways to get access to a device, we probably want them here. */
class HoRNDISUSBComposite : public HoRNDIS {
	OSDeclareDefaultStructors(HoRNDISUSBComposite);
public:
	virtual bool start(IOService *provider);
	virtual bool attach(IOService * provider);
	virtual IOService *probe(IOService *provider, SInt32 *score);
	virtual void detach(IOService * provider);
	virtual void stop(IOService *provider);
};
