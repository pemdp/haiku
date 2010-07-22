/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Atis Elsts, the.kfx@gmail.com
 */


#include "ipv6_address.h"
#include "ipv6_utils.h"
#include "multicast.h"
#include "../../stack/domains.h"

#include <net_datalink.h>
#include <net_datalink_protocol.h>
#include <net_device.h>
#include <net_protocol.h>
#include <net_stack.h>
#include <NetBufferUtilities.h>
#include <ProtocolUtilities.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <util/AutoLock.h>
#include <util/list.h>
#include <util/DoublyLinkedList.h>
#include <util/MultiHashTable.h>

#include <netinet6/in6.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <new>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <utility>


#define TRACE_IPV6
#ifdef TRACE_IPV6
#	define TRACE(format, args...) \
		dprintf("IPv6 [%llu] " format "\n", system_time() , ##args)
#	define TRACE_SK(protocol, format, args...) \
		dprintf("IPv6 [%llu] %p " format "\n", system_time(), \
			protocol , ##args)
#else
#	define TRACE(args...)		do { } while (0)
#	define TRACE_SK(args...)	do { } while (0)
#endif


struct IPv6Header {
	struct ip6_hdr h;

	uint8 ProtocolVersion() const { return h.ip6_vfc & IPV6_VERSION_MASK; }
	uint8 ServiceType() const { return ntohl(h.ip6_flow) >> 20;}
 	uint16 PayloadLength() const { return ntohs(h.ip6_plen); }
	const in6_addr& Dst() const { return h.ip6_dst; }
	const in6_addr& Src() const { return h.ip6_src; }
	uint16 GetTransportHeaderOffset(net_buffer* buffer) const;
};

class RawSocket
	: public DoublyLinkedListLinkImpl<RawSocket>, public DatagramSocket<> {
public:
							RawSocket(net_socket* socket);
};

typedef DoublyLinkedList<RawSocket> RawSocketList;


typedef MulticastGroupInterface<IPv6Multicast> IPv6GroupInterface;
typedef MulticastFilter<IPv6Multicast> IPv6MulticastFilter;

struct MulticastStateHash {
	typedef std::pair<const in6_addr*, uint32> KeyType;
	typedef IPv6GroupInterface ValueType;

	size_t HashKey(const KeyType &key) const;
	size_t Hash(ValueType* value) const
		{ return HashKey(std::make_pair(&value->Address(),
			value->Interface()->index)); }
	bool Compare(const KeyType &key, ValueType* value) const
		{ return value->Interface()->index == key.second
			&& value->Address() == *key.first; }
	bool CompareValues(ValueType* value1, ValueType* value2) const
		{ return value1->Interface()->index == value2->Interface()->index
			&& value1->Address() == value2->Address(); }
	ValueType*& GetLink(ValueType* value) const { return value->HashLink(); }
};


struct ipv6_protocol : net_protocol {
	ipv6_protocol()
		:
		multicast_filter(this)
	{
	}

	RawSocket	*raw;
	uint8		service_type;
	uint8		time_to_live;
	uint8		multicast_time_to_live;
	uint8		receive_hoplimit;
	uint8		receive_pktinfo;
	struct sockaddr* interface_address; // for IPV6_MULTICAST_IF

	IPv6MulticastFilter multicast_filter;
};


static const int kDefaultTTL = 254;
static const int kDefaultMulticastTTL = 1;


extern net_protocol_module_info gIPv6Module;
	// we need this in ipv6_std_ops() for registering the AF_INET domain

net_stack_module_info* gStackModule;
net_buffer_module_info* gBufferModule;

static struct net_domain* sDomain;
static net_datalink_module_info* sDatalinkModule;
static net_socket_module_info* sSocketModule;
static RawSocketList sRawSockets;
static mutex sRawSocketsLock;
static mutex sMulticastGroupsLock;

typedef MultiHashTable<MulticastStateHash> MulticastState;
static MulticastState* sMulticastState;

static net_protocol_module_info* sReceivingProtocol[256];
static mutex sReceivingProtocolLock;


uint16
IPv6Header::GetTransportHeaderOffset(net_buffer* buffer) const
{
	uint16 offset = sizeof(struct ip6_hdr);
	uint8 next = h.ip6_nxt;

	// these are the extension headers that might be supported one day
	while (next == IPPROTO_HOPOPTS
		|| next == IPPROTO_ROUTING
		|| next == IPPROTO_FRAGMENT
		|| next == IPPROTO_ESP
		|| next == IPPROTO_AH
		|| next == IPPROTO_DSTOPTS) {
		struct ip6_ext extensionHeader;
		status_t status = gBufferModule->read(buffer, offset,
			&extensionHeader, sizeof(ip6_ext));
		if (status != B_OK)
			break;

		next = extensionHeader.ip6e_nxt;
		offset += extensionHeader.ip6e_len;
	}

	buffer->protocol = next;
	return offset;
}


RawSocket::RawSocket(net_socket* socket)
	:
	DatagramSocket<>("ipv6 raw socket", socket)
{
}


size_t
MulticastStateHash::HashKey(const KeyType &key) const
{
	size_t result = 0;
	result = jenkins_hashword((const uint32*)&key.first, 
		sizeof(in6_addr) / sizeof(uint32), result);
	result = jenkins_hashword(&key.second, 1, result);
	return result;
}


//	#pragma mark -


static inline void
dump_ipv6_header(IPv6Header &header)
{
#ifdef TRACE_IPV6
	char addrbuf[INET6_ADDRSTRLEN];
	dprintf("  version: %d\n", header.ProtocolVersion() >> 4);
	dprintf("  service_type: %d\n", header.ServiceType());
	dprintf("  payload_length: %d\n", header.PayloadLength());
	dprintf("  next_header: %d\n", header.h.ip6_nxt);
	dprintf("  hop_limit: %d\n", header.h.ip6_hops);
	dprintf("  source: %s\n", ip6_sprintf(&header.h.ip6_src, addrbuf));
	dprintf("  destination: %s\n", ip6_sprintf(&header.h.ip6_dst, addrbuf));
#endif
}


static status_t
deliver_multicast(net_protocol_module_info* module, net_buffer* buffer,
 	bool deliverToRaw, net_interface *interface)
{
	sockaddr_in6* multicastAddr = (sockaddr_in6*)buffer->destination;

	MulticastState::ValueIterator it = sMulticastState->Lookup(std::make_pair(
		&multicastAddr->sin6_addr, interface->index));

	while (it.HasNext()) {
		IPv6GroupInterface* state = it.Next();
		ipv6_protocol* ipproto = state->Parent()->Socket();

		if (deliverToRaw && ipproto->raw == NULL)
			continue;

		if (state->FilterAccepts(buffer)) {
			// TODO: do as in IPv4 code
			module->deliver_data(ipproto, buffer);
		}
	}

	return B_OK;
}


static status_t
deliver_multicast(net_protocol_module_info* module, net_buffer* buffer,
 	bool deliverToRaw)
{
	if (module->deliver_data == NULL)
		return B_OK;

	MutexLocker _(sMulticastGroupsLock);

	status_t status = B_OK;
	if (buffer->interface) {
		status = deliver_multicast(module, buffer, deliverToRaw,
			buffer->interface);
	} else {
		// REVIEWME: does this look ok?
		net_domain_private* domain = (net_domain_private*)sDomain;
		RecursiveLocker locker(domain->lock);

		net_interface* interface = NULL;
		while (true) {
			interface = (net_interface*)list_get_next_item(
				&domain->interfaces, interface);
			if (interface == NULL)
				break;

			status = deliver_multicast(module, buffer, deliverToRaw, interface);
			if (status < B_OK)
				break;
		}
	}
	return status;
}


static void
raw_receive_data(net_buffer* buffer)
{
	MutexLocker locker(sRawSocketsLock);

	if (sRawSockets.IsEmpty())
		return;

	TRACE("RawReceiveData(%i)", buffer->protocol);

	if ((buffer->flags & MSG_MCAST) != 0) {
		deliver_multicast(&gIPv6Module, buffer, true);
	} else {
		RawSocketList::Iterator iterator = sRawSockets.GetIterator();

		while (iterator.HasNext()) {
			RawSocket* raw = iterator.Next();

			if (raw->Socket()->protocol == buffer->protocol)
				raw->SocketEnqueue(buffer);
		}
	}
}


static inline sockaddr*
fill_sockaddr_in6(sockaddr_in6* target, const in6_addr &address)
{
	target->sin6_family = AF_INET6;
	target->sin6_len = sizeof(sockaddr_in6);
	target->sin6_port = 0;
	target->sin6_flowinfo = 0;
	memcpy(target->sin6_addr.s6_addr, address.s6_addr, sizeof(in6_addr));
	target->sin6_scope_id = 0;
	return (sockaddr*)target;
}


status_t
IPv6Multicast::JoinGroup(IPv6GroupInterface* state)
{
	MutexLocker _(sMulticastGroupsLock);

	sockaddr_in6 groupAddr;
	net_interface* interface = state->Interface();

	status_t status = interface->first_info->join_multicast(
		interface->first_protocol,
		fill_sockaddr_in6(&groupAddr, state->Address()));
	if (status != B_OK)
		return status;

	sMulticastState->Insert(state);
	return B_OK;
}


status_t
IPv6Multicast::LeaveGroup(IPv6GroupInterface* state)
{
	MutexLocker _(sMulticastGroupsLock);

	sMulticastState->Remove(state);

	sockaddr_in6 groupAddr;
	net_interface* interface = state->Interface();

	return interface->first_protocol->module->join_multicast(
		interface->first_protocol,
		fill_sockaddr_in6(&groupAddr, state->Address()));
}


static net_protocol_module_info*
receiving_protocol(uint8 protocol)
{
	net_protocol_module_info* module = sReceivingProtocol[protocol];
	if (module != NULL)
		return module;

	MutexLocker locker(sReceivingProtocolLock);

	module = sReceivingProtocol[protocol];
	if (module != NULL)
		return module;

	if (gStackModule->get_domain_receiving_protocol(sDomain, protocol,
			&module) == B_OK)
		sReceivingProtocol[protocol] = module;

	return module;
}


static status_t
ipv6_delta_group(IPv6GroupInterface* group, int option,
	net_interface* interface, const in6_addr* sourceAddr)
{
	switch (option) {
		case IPV6_JOIN_GROUP:
			return group->Add();
		case IPV6_LEAVE_GROUP:
			return group->Drop();
	}

	return B_ERROR;
}


static status_t
ipv6_delta_membership(ipv6_protocol* protocol, int option,
	net_interface* interface, const in6_addr* groupAddr,
	const in6_addr* sourceAddr)
{
	IPv6MulticastFilter &filter = protocol->multicast_filter;
	IPv6GroupInterface* state = NULL;
	status_t status = B_OK;

	switch (option) {
		// TODO: support more options
		case IPV6_JOIN_GROUP:
			status = filter.GetState(*groupAddr, interface, state, true);
			break;

		case IPV6_LEAVE_GROUP:
			filter.GetState(*groupAddr, interface, state, false);
			if (state == NULL)
				return EADDRNOTAVAIL;
			break;
	}

	if (status != B_OK)
		return status;

	status = ipv6_delta_group(state, option, interface, sourceAddr);
	filter.ReturnState(state);
	return status;
}


static status_t
ipv6_delta_membership(ipv6_protocol* protocol, int option,
	uint32 interfaceIndex, in6_addr* groupAddr, in6_addr* sourceAddr)
{
	net_interface* interface;

	// TODO: can the interface be unspecified?
	interface = sDatalinkModule->get_interface(sDomain, interfaceIndex);

	if (interface == NULL)
		return ENODEV;

	return ipv6_delta_membership(protocol, option, interface,
		groupAddr, sourceAddr);
}


static status_t
get_int_option(void* target, size_t length, int value)
{
	if (length != sizeof(int))
		return B_BAD_VALUE;

	return user_memcpy(target, &value, sizeof(int));
}


template<typename Type> static status_t
set_int_option(Type &target, const void* _value, size_t length)
{
	int value;

	if (length != sizeof(int))
		return B_BAD_VALUE;

	if (user_memcpy(&value, _value, sizeof(int)) != B_OK)
		return B_BAD_ADDRESS;

	target = value;
	return B_OK;
}


//	#pragma mark -


net_protocol*
ipv6_init_protocol(net_socket* socket)
{
	ipv6_protocol* protocol = new (std::nothrow) ipv6_protocol();
	if (protocol == NULL)
		return NULL;

	protocol->raw = NULL;
	protocol->service_type = 0;
	protocol->time_to_live = kDefaultTTL;
	protocol->multicast_time_to_live = kDefaultMulticastTTL;
	protocol->receive_hoplimit = 0;
	protocol->receive_pktinfo = 0;
	protocol->interface_address = NULL;
	return protocol;
}


status_t
ipv6_uninit_protocol(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	delete protocol->raw;
	delete protocol->interface_address;
	delete protocol;
	return B_OK;
}


/*!	Since open() is only called on the top level protocol, when we get here
	it means we are on a SOCK_RAW socket.
*/
status_t
ipv6_open(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	RawSocket* raw = new (std::nothrow) RawSocket(protocol->socket);
	if (raw == NULL)
		return B_NO_MEMORY;

	status_t status = raw->InitCheck();
	if (status != B_OK) {
		delete raw;
		return status;
	}

	TRACE_SK(protocol, "Open()");

	protocol->raw = raw;

	MutexLocker locker(sRawSocketsLock);
	sRawSockets.Add(raw);
	return B_OK;
}


status_t
ipv6_close(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	TRACE_SK(protocol, "Close()");

	MutexLocker locker(sRawSocketsLock);
	sRawSockets.Remove(raw);
	delete raw;
	protocol->raw = NULL;

	return B_OK;
}


status_t
ipv6_free(net_protocol* protocol)
{
	return B_OK;
}


status_t
ipv6_connect(net_protocol* protocol, const struct sockaddr* address)
{
	return B_ERROR;
}


status_t
ipv6_accept(net_protocol* protocol, struct net_socket** _acceptedSocket)
{
	return EOPNOTSUPP;
}


status_t
ipv6_control(net_protocol* _protocol, int level, int option, void* value,
	size_t* _length)
{
	if ((level & LEVEL_MASK) != IPPROTO_IPV6)
		return sDatalinkModule->control(sDomain, option, value, _length);

	return B_BAD_VALUE;
}


status_t
ipv6_getsockopt(net_protocol* _protocol, int level, int option, void* value,
	int* _length)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

 	if (level == IPPROTO_IPV6) {
		// TODO: support more of these options

		if (option == IPV6_MULTICAST_HOPS) {
			return get_int_option(value, *_length,
				protocol->multicast_time_to_live);
		}
		if (option == IPV6_MULTICAST_LOOP)
			return EOPNOTSUPP;
		if (option == IPV6_UNICAST_HOPS)
			return get_int_option(value, *_length, protocol->time_to_live);
		if (option == IPV6_V6ONLY)
			return EOPNOTSUPP;
		if (option == IPV6_RECVPKTINFO)
			return get_int_option(value, *_length, protocol->receive_pktinfo);
		if (option == IPV6_RECVHOPLIMIT)
			return get_int_option(value, *_length, protocol->receive_hoplimit);
		if (option == IPV6_JOIN_GROUP
			|| option == IPV6_LEAVE_GROUP)
			return EOPNOTSUPP;

		dprintf("IPv6::getsockopt(): get unknown option: %d\n", option);
		return ENOPROTOOPT;
	}

	return sSocketModule->get_option(protocol->socket, level, option, value,
		_length);
}


status_t
ipv6_setsockopt(net_protocol* _protocol, int level, int option,
	const void* value, int length)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

 	if (level == IPPROTO_IPV6) {
		// TODO: support more of these options

		if (option == IPV6_MULTICAST_IF) {
			if (length != sizeof(struct in6_addr))
				return B_BAD_VALUE;

			struct sockaddr_in6* address = new (std::nothrow) sockaddr_in6;
			if (address == NULL)
				return B_NO_MEMORY;

			if (user_memcpy(&address->sin6_addr, value, sizeof(in6_addr))
					!= B_OK) {
				delete address;
				return B_BAD_ADDRESS;
			}

			// Using the unspecifed address to remove the previous setting.
			if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr)) {
				delete address;
				delete protocol->interface_address;
				protocol->interface_address = NULL;
				return B_OK;
			}

			struct net_interface* interface
				= sDatalinkModule->get_interface_with_address(sDomain,
					(struct sockaddr*)address);
			if (interface == NULL) {
				delete address;
				return EADDRNOTAVAIL;
			}

			delete protocol->interface_address;
			protocol->interface_address = (struct sockaddr*)address;
			return B_OK;
		}
		if (option == IPV6_MULTICAST_HOPS) {
			return set_int_option(protocol->multicast_time_to_live,
				value, length);
		}
		if (option == IPV6_MULTICAST_LOOP)
			return EOPNOTSUPP;
		if (option == IPV6_UNICAST_HOPS)
			return set_int_option(protocol->time_to_live, value, length);
		if (option == IPV6_V6ONLY)
			return EOPNOTSUPP;
		if (option == IPV6_RECVPKTINFO)
			return set_int_option(protocol->receive_pktinfo, value, length);
		if (option == IPV6_RECVHOPLIMIT)
			return set_int_option(protocol->receive_hoplimit, value, length);
		if (option == IPV6_JOIN_GROUP || option == IPV6_LEAVE_GROUP) {
			ipv6_mreq mreq;
			if (length != sizeof(ipv6_mreq))
				return B_BAD_VALUE;
			if (user_memcpy(&mreq, value, sizeof(ipv6_mreq)) != B_OK)
				return B_BAD_ADDRESS;

			return ipv6_delta_membership(protocol, option, mreq.ipv6mr_interface,
				&mreq.ipv6mr_multiaddr, NULL);
		}

		dprintf("IPv6::setsockopt(): set unknown option: %d\n", option);
		return ENOPROTOOPT;
	}

	return sSocketModule->set_option(protocol->socket, level, option,
		value, length);
}


status_t
ipv6_bind(net_protocol* protocol, const sockaddr* _address)
{
	if (_address->sa_family != AF_INET6)
		return EAFNOSUPPORT;

	const sockaddr_in6* address = (const sockaddr_in6*)_address;

	// only INADDR_ANY and addresses of local interfaces are accepted:
	if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr)
		|| IN6_IS_ADDR_MULTICAST(&address->sin6_addr)
 		|| sDatalinkModule->is_local_address(sDomain, _address, NULL, NULL)) {
		memcpy(&protocol->socket->address, address, sizeof(sockaddr_in6));
		protocol->socket->address.ss_len = sizeof(sockaddr_in6);
			// explicitly set length, as our callers can't be trusted to
			// always provide the correct length!
		return B_OK;
	}

	return B_ERROR;
		// address is unknown on this host
}


status_t
ipv6_unbind(net_protocol* protocol, struct sockaddr* address)
{
	// nothing to do here
	return B_OK;
}


status_t
ipv6_listen(net_protocol* protocol, int count)
{
	return EOPNOTSUPP;
}


status_t
ipv6_shutdown(net_protocol* protocol, int direction)
{
	return EOPNOTSUPP;
}


static uint8
ip6_select_hoplimit(net_protocol* _protocol, net_buffer* buffer)
{
	// TODO: the precedence should be as follows:
	// 1. Hoplimit value specified via ioctl.
	// 2. (If the outgoing interface is detected) the current
	//     hop limit of the interface specified by router advertisement.
	// 3. The system default hoplimit.
	
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	const bool isMulticast = buffer->flags & MSG_MCAST;

	if (protocol) {
		return isMulticast ? protocol->multicast_time_to_live
			: protocol->time_to_live;
 	}
	return isMulticast ? kDefaultMulticastTTL : kDefaultTTL;
}


status_t
ipv6_send_routed_data(net_protocol* _protocol, struct net_route* route,
	net_buffer* buffer)
{
	if (route == NULL)
		return B_BAD_VALUE;

	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	net_interface* interface = route->interface;
	uint8 protocolNumber;
	if (protocol != NULL && protocol->socket != NULL)
		protocolNumber = protocol->socket->protocol;
	else
		protocolNumber = buffer->protocol;

	TRACE_SK(protocol, "SendRoutedData(%p, %p [%ld bytes])", route, buffer,
		buffer->size);

	sockaddr_in6& source = *(sockaddr_in6*)buffer->source;
	sockaddr_in6& destination = *(sockaddr_in6*)buffer->destination;

	buffer->flags &= ~(MSG_BCAST | MSG_MCAST);

	if (IN6_IS_ADDR_UNSPECIFIED(&destination.sin6_addr))
		return EDESTADDRREQ;

	if (IN6_IS_ADDR_MULTICAST(&destination.sin6_addr))
 		buffer->flags |= MSG_MCAST;

	uint16 dataLength = buffer->size;

	// Add IPv6 header

	NetBufferPrepend<ip6_hdr> header(buffer);
	if (header.Status() != B_OK)
		return header.Status();

	if (buffer->size > 0xffff)
		return EMSGSIZE;

	uint32 flowinfo = 0;
		// TODO: fill in the flow id from somewhere
	if (protocol) {
		// fill in traffic class
		flowinfo |= htonl(protocol->service_type << 20);
	}
	// set lower 28 bits
	header->ip6_flow = htonl(flowinfo) & IPV6_FLOWINFO_MASK;
	// set upper 4 bits
	header->ip6_vfc |= IPV6_VERSION;
	header->ip6_plen = htons(dataLength);
	header->ip6_nxt = protocolNumber;
	header->ip6_hlim = ip6_select_hoplimit(protocol, buffer);
	memcpy(&header->ip6_src, &source.sin6_addr, sizeof(in6_addr));
	memcpy(&header->ip6_dst, &destination.sin6_addr, sizeof(in6_addr));

	header.Sync();

	// write the checksum for ICMPv6 sockets
	if (protocolNumber == IPPROTO_ICMPV6
		&& dataLength >= sizeof(struct icmp6_hdr)) {
		NetBufferField<uint16, 
			sizeof(ip6_hdr) + offsetof(icmp6_hdr, icmp6_cksum)>
			icmpChecksum(buffer);
		// first make sure the existing checksum is zero
		*icmpChecksum = 0;
		icmpChecksum.Sync();

		uint16 checksum = gBufferModule->checksum(buffer, sizeof(ip6_hdr),
			buffer->size - sizeof(ip6_hdr), false);
		checksum = ipv6_checksum(&header->ip6_src,
			&header->ip6_dst, dataLength, protocolNumber,
			checksum);
		*icmpChecksum = checksum;
	}

	char addrbuf[INET6_ADDRSTRLEN];
	TRACE_SK(protocol, "  SendRoutedData(): destination: %s",
		ip6_sprintf(&destination.sin6_addr, addrbuf));

	uint32 mtu = route->mtu ? route->mtu : interface->mtu;
	if (buffer->size > mtu) {
		// we need to fragment the packet
		return EMSGSIZE; // TODO
		//return send_fragments(protocol, route, buffer, mtu);
	}

	return sDatalinkModule->send_data(route, buffer);
}


status_t
ipv6_send_data(net_protocol* _protocol, net_buffer* buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	TRACE_SK(protocol, "SendData(%p [%ld bytes])", buffer, buffer->size);

	sockaddr_in6* destination = (sockaddr_in6*)buffer->destination;

	// handle IPV6_MULTICAST_IF
	if (IN6_IS_ADDR_MULTICAST(&destination->sin6_addr)
			&& protocol->interface_address != NULL) {
		net_interface* interface
			= sDatalinkModule->get_interface_with_address(sDomain,
				protocol->interface_address);
		if (interface == NULL || (interface->flags & IFF_UP) == 0)
			return EADDRNOTAVAIL;

		buffer->interface = interface;

		net_route* route = sDatalinkModule->get_route(sDomain,
			interface->address);
		if (route == NULL)
			return ENETUNREACH;

		return sDatalinkModule->send_data(route, buffer);
	}

	return sDatalinkModule->send_datagram(protocol, sDomain, buffer);
}


ssize_t
ipv6_send_avail(net_protocol* protocol)
{
	return B_ERROR;
}


status_t
ipv6_read_data(net_protocol* _protocol, size_t numBytes, uint32 flags,
	net_buffer** _buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	TRACE_SK(protocol, "ReadData(%lu, 0x%lx)", numBytes, flags);

	return raw->SocketDequeue(flags, _buffer);
}


ssize_t
ipv6_read_avail(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	return raw->AvailableData();
}


struct net_domain*
ipv6_get_domain(net_protocol* protocol)
{
	return sDomain;
}


size_t
ipv6_get_mtu(net_protocol* protocol, const struct sockaddr* address)
{
	net_route* route = sDatalinkModule->get_route(sDomain, address);
	if (route == NULL)
		return 0;

	size_t mtu;
	if (route->mtu != 0)
		mtu = route->mtu;
	else
		mtu = route->interface->mtu;

	sDatalinkModule->put_route(sDomain, route);
	// TODO: what about extension headers?
	// this function probably shoud be changed in calling places, not here
	return mtu - sizeof(ip6_hdr);
}


status_t
ipv6_receive_data(net_buffer* buffer)
{
	TRACE("ReceiveData(%p [%ld bytes])", buffer, buffer->size);

	NetBufferHeaderReader<IPv6Header> bufferHeader(buffer);
	if (bufferHeader.Status() != B_OK)
		return bufferHeader.Status();

	IPv6Header &header = bufferHeader.Data();
	// dump_ipv6_header(header);

	if (header.ProtocolVersion() != IPV6_VERSION)
		return B_BAD_TYPE;

	uint16 packetLength = header.PayloadLength() + sizeof(ip6_hdr);
	if (packetLength > buffer->size)
		return B_BAD_DATA;

	// lower layers notion of Broadcast or Multicast have no relevance to us
	buffer->flags &= ~(MSG_BCAST | MSG_MCAST);

	sockaddr_in6 destination;
	fill_sockaddr_in6(&destination, header.Dst());

	if (IN6_IS_ADDR_MULTICAST(&destination.sin6_addr)) {
		buffer->flags |= MSG_MCAST;
	} else {
		// test if the packet is really for us
		if (!sDatalinkModule->is_local_address(sDomain, (sockaddr*)&destination,
				&buffer->interface, NULL)
			&& !sDatalinkModule->is_local_link_address(sDomain, true,
				buffer->destination, &buffer->interface)) {
			char srcbuf[INET6_ADDRSTRLEN];
			char dstbuf[INET6_ADDRSTRLEN];
			TRACE("  ReceiveData(): packet was not for us %s -> %s",
				ip6_sprintf(&header.Src(), srcbuf),
				ip6_sprintf(&header.Dst(), dstbuf));
			return B_ERROR;
		}
	}

	// set net_buffer's source/destination address
	fill_sockaddr_in6((struct sockaddr_in6*)buffer->source, header.Src());
	memcpy(buffer->destination, &destination, sizeof(sockaddr_in6));

	// get the transport protocol and transport header offset
	uint16 transportHeaderOffset = header.GetTransportHeaderOffset(buffer);
	uint8 protocol = buffer->protocol;

	buffer->hoplimit = header.h.ip6_hlim;

	// remove any trailing/padding data
	status_t status = gBufferModule->trim(buffer, packetLength);
	if (status != B_OK)
		return status;

	//
	// TODO: check for fragmentation
	//

	gBufferModule->remove_header(buffer, transportHeaderOffset);

	raw_receive_data(buffer);

	net_protocol_module_info* module = receiving_protocol(protocol);
	if (module == NULL) {
		// no handler for this packet
		return EAFNOSUPPORT;
	}

	if ((buffer->flags & MSG_MCAST) != 0) {
		// Unfortunely historical reasons dictate that the IP multicast
		// model be a little different from the unicast one. We deliver
		// this frame directly to all sockets registered with interest
		// for this multicast group.
		return deliver_multicast(module, buffer, false);
	}

	return module->receive_data(buffer);
}


status_t
ipv6_deliver_data(net_protocol* _protocol, net_buffer* buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (protocol->raw == NULL)
		return B_ERROR;

	return protocol->raw->SocketEnqueue(buffer);
}


status_t
ipv6_error(uint32 code, net_buffer* data)
{
	return B_ERROR;
}


status_t
ipv6_error_reply(net_protocol* protocol, net_buffer* causedError, uint32 code,
	void* errorData)
{
	return B_ERROR;
}


ssize_t
ipv6_process_ancillary_data_no_container(net_protocol* protocol,
	net_buffer* buffer, void* msgControl, size_t msgControlLen)
{
	ssize_t bytesWritten = 0;

	if (((ipv6_protocol*)protocol)->receive_hoplimit != 0) {
		TRACE("receive_hoplimit");

		if (msgControlLen < CMSG_SPACE(sizeof(int)))
			return B_NO_MEMORY;

		cmsghdr* messageHeader = (cmsghdr*)((char*)msgControl + bytesWritten);
		messageHeader->cmsg_len = CMSG_LEN(sizeof(int));
		messageHeader->cmsg_level = IPPROTO_IPV6;
		messageHeader->cmsg_type = IPV6_HOPLIMIT;

		int hoplimit = buffer->hoplimit;
		memcpy(CMSG_DATA(messageHeader), &hoplimit, sizeof(int));

		bytesWritten += CMSG_SPACE(sizeof(int));
		msgControlLen -= CMSG_SPACE(sizeof(int));
	}

	if (((ipv6_protocol*)protocol)->receive_pktinfo != 0) {
		TRACE("receive_pktinfo");

		if (msgControlLen < CMSG_SPACE(sizeof(struct in6_pktinfo)))
			return B_NO_MEMORY;

		cmsghdr* messageHeader = (cmsghdr*)((char*)msgControl + bytesWritten);
		messageHeader->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
		messageHeader->cmsg_level = IPPROTO_IPV6;
		messageHeader->cmsg_type = IPV6_PKTINFO;

		struct in6_pktinfo pi;
		memcpy(&pi.ipi6_addr,
			&((struct sockaddr_in6*)buffer->destination)->sin6_addr,
		 	sizeof(struct in6_addr));
		// REVIEWME: assume buffer->interface cannot be NULL
		pi.ipi6_ifindex = buffer->interface->index;
		memcpy(CMSG_DATA(messageHeader), &pi, sizeof(struct in6_pktinfo));

		bytesWritten += CMSG_SPACE(sizeof(struct in6_pktinfo));
		msgControlLen -= CMSG_SPACE(sizeof(struct in6_pktinfo));
	}

	return bytesWritten;
}


//	#pragma mark -


status_t
init_ipv6()
{
	mutex_init(&sRawSocketsLock, "raw sockets");
	mutex_init(&sMulticastGroupsLock, "IPv6 multicast groups");
	mutex_init(&sReceivingProtocolLock, "IPv6 receiving protocols");

	status_t status;

	sMulticastState = new MulticastState();
	if (sMulticastState == NULL) {
		status = B_NO_MEMORY;
		goto err;
	}

	status = sMulticastState->Init();
	if (status != B_OK)
		goto err;

	new (&sRawSockets) RawSocketList;
		// static initializers do not work in the kernel,
		// so we have to do it here, manually
		// TODO: for modules, this shouldn't be required

	status = gStackModule->register_domain_protocols(AF_INET6, SOCK_RAW, 0,
		NET_IPV6_MODULE_NAME, NULL);
	if (status != B_OK)
		goto err;

	status = gStackModule->register_domain(AF_INET6, "internet6", &gIPv6Module,
		&gIPv6AddressModule, &sDomain);
	if (status != B_OK)
		goto err;

	TRACE("init_ipv6: OK\n");
	return B_OK;

err:
	delete sMulticastState;
	mutex_destroy(&sReceivingProtocolLock);
	mutex_destroy(&sMulticastGroupsLock);
	mutex_destroy(&sRawSocketsLock);
	TRACE("init_ipv6: error, status=%u\n", status);
	return status;
}


status_t
uninit_ipv6()
{
	mutex_lock(&sReceivingProtocolLock);

	// put all the domain receiving protocols we gathered so far
	for (uint32 i = 0; i < 256; i++) {
		if (sReceivingProtocol[i] != NULL)
			gStackModule->put_domain_receiving_protocol(sDomain, i);
	}

	delete sMulticastState;

	gStackModule->unregister_domain(sDomain);
	mutex_unlock(&sReceivingProtocolLock);

	mutex_destroy(&sMulticastGroupsLock);
	mutex_destroy(&sRawSocketsLock);
	mutex_destroy(&sReceivingProtocolLock);

	return B_OK;
}


static status_t
ipv6_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return init_ipv6();
		case B_MODULE_UNINIT:
			return uninit_ipv6();

		default:
			return B_ERROR;
	}
}


net_protocol_module_info gIPv6Module = {
	{
		NET_IPV6_MODULE_NAME,
		0,
		ipv6_std_ops
	},
	NET_PROTOCOL_ATOMIC_MESSAGES,

	ipv6_init_protocol,
	ipv6_uninit_protocol,
	ipv6_open,
	ipv6_close,
	ipv6_free,
	ipv6_connect,
	ipv6_accept,
	ipv6_control,
	ipv6_getsockopt,
	ipv6_setsockopt,
	ipv6_bind,
	ipv6_unbind,
	ipv6_listen,
	ipv6_shutdown,
	ipv6_send_data,
	ipv6_send_routed_data,
	ipv6_send_avail,
	ipv6_read_data,
	ipv6_read_avail,
	ipv6_get_domain,
	ipv6_get_mtu,
	ipv6_receive_data,
	ipv6_deliver_data,
	ipv6_error,
	ipv6_error_reply,
	NULL,		// add_ancillary_data()
	NULL,		// process_ancillary_data()
	ipv6_process_ancillary_data_no_container,
	NULL,		// send_data_no_buffer()
	NULL		// read_data_no_buffer()
};

module_dependency module_dependencies[] = {
	{NET_STACK_MODULE_NAME, (module_info**)&gStackModule},
	{NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule},
	{NET_DATALINK_MODULE_NAME, (module_info**)&sDatalinkModule},
	{NET_SOCKET_MODULE_NAME, (module_info**)&sSocketModule},
	{}
};

module_info* modules[] = {
	(module_info*)&gIPv6Module,
	NULL
};