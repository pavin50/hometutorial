#ifndef SNIFF_INLINE_C
#define SNIFF_INLINE_C

#if ( defined( __FreeBSD__ ) || defined ( __NetBSD__ ) )
# ifndef FREEBSD
#  define FREEBSD
# endif
#endif

#ifdef FREEBSD
#include <sys/types.h>
#endif

#include <syslog.h>
#include <net/ethernet.h>

#include "tcpreassembly.h"
#include "sniff.h"
#include "sniff_inline.h"


#ifndef DEBUG_ALL_PACKETS
#define DEBUG_ALL_PACKETS false
#endif

#ifndef MAX
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif


extern bool isSslIpPort(u_int32_t ip, u_int16_t port);


extern int opt_udpfrag;
extern int opt_ipaccount;
extern int opt_skinny;
extern int opt_dup_check;
extern int opt_dup_check_ipheader;
extern char *sipportmatrix;
extern char *httpportmatrix;
extern char *webrtcportmatrix;
extern TcpReassembly *tcpReassemblyHttp;
extern TcpReassembly *tcpReassemblyWebrtc;
extern unsigned int defrag_counter;
extern unsigned int duplicate_counter;


#if SNIFFER_INLINE_FUNCTIONS
inline 
#endif
iphdr2 *convertHeaderIP_GRE(iphdr2 *header_ip) {
	char gre[8];
	uint16_t a, b;
	// if anyone know how to make network to hostbyte nicely, redesign this
	a = ntohs(*(uint16_t*)((char*)header_ip + sizeof(iphdr2)));
	b = ntohs(*(uint16_t*)((char*)header_ip + sizeof(iphdr2) + 2));
	memcpy(gre, &a, 2);
	memcpy(gre + 2, &b, 2);
	struct gre_hdr *grehdr = (struct gre_hdr *)gre;			
	if(grehdr->version == 0 and (grehdr->protocol == 0x6558 || grehdr->protocol == 0x88BE)) {
		// 0x6558 - GRE          - header size 8 bytes
		// 0x88BE - GRE & ERSPAN - headers size 8 + 8 bytes
		struct ether_header *header_eth = (struct ether_header *)((char*)header_ip + sizeof(iphdr2) + (grehdr->protocol == 0x88BE ? 16 : 8));
		unsigned int vlanoffset;
		u_int16_t protocol = 0;
		if(header_eth->ether_type == 129) {
			// VLAN tag
			vlanoffset = 0;
			do {
				protocol = *(u_int16_t*)((char*)header_eth + sizeof(ether_header) + vlanoffset + 2);
				vlanoffset += 4;
			} while(protocol == 129);
			//XXX: this is very ugly hack, please do it right! (it will work for "08 00" which is IPV4 but not for others! (find vlan_header or something)
		} else {
			vlanoffset = 0;
			protocol = header_eth->ether_type;
		}
		if(protocol == 8) {
			header_ip = (struct iphdr2 *) ((char*)header_eth + sizeof(ether_header) + vlanoffset);
		} else {
			return(NULL);
		}
	} else if(grehdr->version == 0 and grehdr->protocol == 0x800) {
		header_ip = (struct iphdr2 *) ((char*)header_ip + sizeof(iphdr2) + 4);
	} else {
		return(NULL);
	}
	return(header_ip);
}

#if SNIFFER_INLINE_FUNCTIONS
inline 
#endif
bool parseEtherHeader(int pcapLinklayerHeaderType, u_char* packet,
		      sll_header *&header_sll, ether_header *&header_eth, u_int &header_ip_offset, int &protocol, int *vlan) {
	if(vlan) {
		*vlan = -1;
	}
	switch(pcapLinklayerHeaderType) {
		case DLT_LINUX_SLL:
			header_sll = (sll_header*)packet;
			if(header_sll->sll_protocol == 129) {
				// VLAN tag
				header_ip_offset = 0;
				u_int16_t _protocol;
				do {
					if(vlan) {
						*vlan = htons(*(u_int16_t*)(packet + sizeof(sll_header) + header_ip_offset)) & 0xFFF;
					}
					_protocol = *(u_int16_t*)(packet + sizeof(sll_header) + header_ip_offset + 2);
					header_ip_offset += 4;
				} while(_protocol == 129);
				protocol = htons(_protocol);
			} else {
				header_ip_offset = 0;
				protocol = htons(header_sll->sll_protocol);
			}
			header_ip_offset += sizeof(sll_header);
			break;
		case DLT_EN10MB:
			header_eth = (ether_header*)packet;
			if(header_eth->ether_type == 129) {
				// VLAN tag
				header_ip_offset = 0;
				u_int16_t _protocol;
				do {
					if(vlan) {
						*vlan = htons(*(u_int16_t*)(packet + sizeof(ether_header) + header_ip_offset)) & 0xFFF;
					}
					_protocol = *(u_int16_t*)(packet + sizeof(ether_header) + header_ip_offset + 2);
					header_ip_offset += 4;
				} while(_protocol == 129);
				protocol = htons(_protocol);
				//XXX: this is very ugly hack, please do it right! (it will work for "08 00" which is IPV4 but not for others! (find vlan_header or something)
			} else if(htons(header_eth->ether_type) == 0x88A8) {
				// IEEE 8021ad
				header_ip_offset = 4;
				protocol = htons(*(u_int16_t*)(packet + sizeof(ether_header) + 2));
			} else {
				header_ip_offset = 0;
				protocol = htons(header_eth->ether_type);
			}
			header_ip_offset += sizeof(ether_header);
			break;
		case DLT_RAW:
			header_ip_offset = 0;
			protocol = ETHERTYPE_IP;
			break;
		case DLT_IEEE802_11_RADIO:
			header_ip_offset = 52;
			protocol = ETHERTYPE_IP;
			break;
		case DLT_NULL:
			header_ip_offset = 4;
			protocol = ETHERTYPE_IP;
			break;
		default:
			return(false);
	}
	return(true);
}

#if SNIFFER_INLINE_FUNCTIONS
inline 
#endif
int pcapProcess(sHeaderPacket **header_packet, int pushToStack_queue_index,
		bool enableDefrag, bool enableCalcMD5, bool enableDedup, bool enableDump,
		pcapProcessData *ppd, int pcapLinklayerHeaderType, pcap_dumper_t *pcapDumpHandle, const char *interfaceName) {
 
	extern BogusDumper *bogusDumper;
	static u_long lastTimeLogErrBadIpHeader = 0;
	
	if((*header_packet)->_detect_headers & 0x01) {
		ppd->header_ip_offset = (*header_packet)->_header_ip_first_offset;
		ppd->protocol = (*header_packet)->_eth_protocol;
		ppd->header_ip = (iphdr2*)(HPP(*header_packet) + ppd->header_ip_offset);
	} else if(parseEtherHeader(pcapLinklayerHeaderType, HPP(*header_packet),
				   ppd->header_sll, ppd->header_eth, ppd->header_ip_offset, ppd->protocol)) {
		(*header_packet)->_detect_headers |= 0x01;
		(*header_packet)->_header_ip_first_offset = ppd->header_ip_offset;
		(*header_packet)->_eth_protocol = ppd->protocol;
		if(ppd->protocol != ETHERTYPE_IP) {
			if(sverb.tcpreplay) {
				if(ppd->protocol == 0) {
					ppd->header_ip_offset += 2;
					ppd->protocol = ETHERTYPE_IP;
				} else {
					return(0);
				}
			} else {
				static int info_tcpreplay = 0;
				if(ppd->protocol == 0 && !info_tcpreplay && interfaceName && !strcmp(interfaceName, "lo")) {
					syslog(LOG_ERR, "BAD PROTOCOL (not ipv4) IN %s (dlt %d) - TRY VERBOSE OPTION tcpreplay", interfaceName, pcapLinklayerHeaderType);
					info_tcpreplay = 1;
				}
				return(0);
			}
		}
		ppd->header_ip = (iphdr2*)(HPP(*header_packet) + ppd->header_ip_offset);
		if(ppd->header_ip->version != 4) {
			if(interfaceName) {
				if(bogusDumper) {
					bogusDumper->dump(HPH(*header_packet), HPP(*header_packet), pcapLinklayerHeaderType, interfaceName);
				}
				u_long actTime = getTimeMS(HPH(*header_packet));
				if(actTime - 1000 > lastTimeLogErrBadIpHeader) {
					syslog(LOG_ERR, "BAD HEADER_IP: %s: bogus ip header version %i", interfaceName, ppd->header_ip->version);
					lastTimeLogErrBadIpHeader = actTime;
				}
			}
			return(0);
		}
		if(htons(ppd->header_ip->tot_len) + ppd->header_ip_offset > HPH(*header_packet)->len) {
			if(interfaceName) {
				if(bogusDumper) {
					bogusDumper->dump(HPH(*header_packet), HPP(*header_packet), pcapLinklayerHeaderType, interfaceName);
				}
				u_long actTime = getTimeMS(HPH(*header_packet));
				if(actTime - 1000 > lastTimeLogErrBadIpHeader) {
					syslog(LOG_ERR, "BAD HEADER_IP: %s: bogus ip header length %i, len %i", interfaceName, htons(ppd->header_ip->tot_len), HPH(*header_packet)->len);
					lastTimeLogErrBadIpHeader = actTime;
				}
			}
			return(0);
		}
	} else {
		syslog(LOG_ERR, "BAD DATALINK %s: datalink number [%d] is not supported", interfaceName ? interfaceName : "---", pcapLinklayerHeaderType);
		return(0);
	}
	
	if(enableDefrag) {
		//if UDP defrag is enabled process only UDP packets and only SIP packets
		if(opt_udpfrag) {
			int foffset = ntohs(ppd->header_ip->frag_off);
			if ((foffset & IP_MF) || ((foffset & IP_OFFSET) > 0)) {
				if(htons(ppd->header_ip->tot_len) + ppd->header_ip_offset > HPH(*header_packet)->caplen) {
					if(interfaceName) {
						if(bogusDumper) {
							bogusDumper->dump(HPH(*header_packet), HPP(*header_packet), pcapLinklayerHeaderType, interfaceName);
						}
						u_long actTime = getTimeMS(HPH(*header_packet));
						if(actTime - 1000 > lastTimeLogErrBadIpHeader) {
							syslog(LOG_ERR, "BAD FRAGMENTED HEADER_IP: %s: bogus ip header length %i, caplen %i", interfaceName, htons(ppd->header_ip->tot_len), HPH(*header_packet)->caplen);
							lastTimeLogErrBadIpHeader = actTime;
						}
					}
					//cout << "pcapProcess exit 001" << endl;
					return(0);
				}
				// packet is fragmented
				if(handle_defrag(ppd->header_ip, header_packet, &ppd->ipfrag_data, pushToStack_queue_index)) {
					// packets are reassembled
					ppd->header_ip = (iphdr2*)(HPP(*header_packet) + ppd->header_ip_offset);
					if(sverb.defrag) {
						defrag_counter++;
						cout << "*** DEFRAG 1 " << defrag_counter << endl;
					}
				} else {
					//cout << "pcapProcess exit 002" << endl;
					return(0);
				}
			}
		}
	}
	
	bool nextPass;
	do {
		nextPass = false;
		u_int first_header_ip_offset = ppd->header_ip_offset;
		if(ppd->header_ip->protocol == IPPROTO_IPIP) {
			// ip in ip protocol
			ppd->header_ip = (iphdr2*)((char*)ppd->header_ip + sizeof(iphdr2));
			ppd->header_ip_offset += sizeof(iphdr2);
		} else if(ppd->header_ip->protocol == IPPROTO_GRE) {
			// gre protocol
			iphdr2 *header_ip = convertHeaderIP_GRE(ppd->header_ip);
			if(header_ip) {
				ppd->header_ip = header_ip;
				ppd->header_ip_offset = (u_char*)header_ip - HPP(*header_packet);
				nextPass = true;
			} else {
				if(opt_ipaccount == 0) {
					//cout << "pcapProcess exit 004" << endl;
					return(0);
				}
			}
		} else {
			break;
		}
		if(enableDefrag) {
			//if UDP defrag is enabled process only UDP packets and only SIP packets
			if(opt_udpfrag && ppd->header_ip->protocol == IPPROTO_UDP) {
				int foffset = ntohs(ppd->header_ip->frag_off);
				if ((foffset & IP_MF) || ((foffset & IP_OFFSET) > 0)) {
					// packet is fragmented
					if(handle_defrag(ppd->header_ip, header_packet, &ppd->ipfrag_data, pushToStack_queue_index)) {
						// packets are reassembled
						iphdr2 *first_header_ip = (iphdr2*)(HPP(*header_packet) + first_header_ip_offset);

						// turn off frag flag in the first IP header
						first_header_ip->frag_off = 0;

						// turn off frag flag in the second IP header
						ppd->header_ip = (iphdr2*)(HPP(*header_packet) + ppd->header_ip_offset);
						ppd->header_ip->frag_off = 0;

						// update lenght of the first ip header to the len of the second IP header since it can be changed due to reassemble
						first_header_ip->tot_len = htons(ntohs(ppd->header_ip->tot_len) + (ppd->header_ip_offset - first_header_ip_offset));

						if(sverb.defrag) {
							defrag_counter++;
							cout << "*** DEFRAG 2 " << defrag_counter << endl;
						}
					} else {
						//cout << "pcapProcess exit 003" << endl;
						return(0);
					}
				}
			}
		}
	} while(nextPass);
	(*header_packet)->_header_ip_offset = ppd->header_ip_offset;
	
	if(enableDefrag) {
		// if IP defrag is enabled, run each 10 seconds cleaning 
		if(opt_udpfrag && (ppd->ipfrag_lastprune + 10) < HPH(*header_packet)->ts.tv_sec) {
			ipfrag_prune(HPH(*header_packet)->ts.tv_sec, 0, &ppd->ipfrag_data, pushToStack_queue_index);
			ppd->ipfrag_lastprune = HPH(*header_packet)->ts.tv_sec;
			//TODO it would be good to still pass fragmented packets even it does not contain the last semant, the ipgrad_prune just wipes all unfinished frags
		}
	}
	
	bool enableReturnZeroInCheckData = !opt_udpfrag || enableDefrag || enableCalcMD5 || enableDedup || enableDump;

	ppd->header_udp = &ppd->header_udp_tmp;
	if (ppd->header_ip->protocol == IPPROTO_UDP) {
		// prepare packet pointers 
		ppd->header_udp = (udphdr2*) ((char*) ppd->header_ip + sizeof(*ppd->header_ip));
		ppd->data = (char*) ppd->header_udp + sizeof(*ppd->header_udp);
		ppd->datalen = (int)(HPH(*header_packet)->caplen - ((u_char*)ppd->data - HPP(*header_packet))); 
		ppd->traillen = (int)(HPH(*header_packet)->caplen - ((u_char*)ppd->header_ip - HPP(*header_packet))) - ntohs(ppd->header_ip->tot_len);
		ppd->istcp = 0;
	} else if (ppd->header_ip->protocol == IPPROTO_TCP) {
		ppd->istcp = 1;
		// prepare packet pointers 
		ppd->header_tcp = (tcphdr2*) ((char*) ppd->header_ip + sizeof(*ppd->header_ip));
		ppd->data = (char*) ppd->header_tcp + (ppd->header_tcp->doff * 4);
		ppd->datalen = (int)(HPH(*header_packet)->caplen - ((u_char*)ppd->data - HPP(*header_packet))); 
		if (!(sipportmatrix[htons(ppd->header_tcp->source)] || sipportmatrix[htons(ppd->header_tcp->dest)]) &&
		    !(opt_enable_http && (httpportmatrix[htons(ppd->header_tcp->source)] || httpportmatrix[htons(ppd->header_tcp->dest)]) &&
		      (tcpReassemblyHttp->check_ip(htonl(ppd->header_ip->saddr)) || tcpReassemblyHttp->check_ip(htonl(ppd->header_ip->daddr)))) &&
		    !(opt_enable_webrtc && (webrtcportmatrix[htons(ppd->header_tcp->source)] || webrtcportmatrix[htons(ppd->header_tcp->dest)]) &&
		      (tcpReassemblyWebrtc->check_ip(htonl(ppd->header_ip->saddr)) || tcpReassemblyWebrtc->check_ip(htonl(ppd->header_ip->daddr)))) &&
		    !(opt_enable_ssl && 
		      (isSslIpPort(htonl(ppd->header_ip->saddr), htons(ppd->header_tcp->source)) ||
		       isSslIpPort(htonl(ppd->header_ip->daddr), htons(ppd->header_tcp->dest)))) &&
		    !(opt_skinny && (htons(ppd->header_tcp->source) == 2000 || htons(ppd->header_tcp->dest) == 2000))) {
			// not interested in TCP packet other than SIP port
			if(opt_ipaccount == 0 && !DEBUG_ALL_PACKETS && enableReturnZeroInCheckData) {
				//cout << "pcapProcess exit 005" << endl;
				return(0);
			}
		}

		ppd->header_udp->source = ppd->header_tcp->source;
		ppd->header_udp->dest = ppd->header_tcp->dest;
	} else {
		//packet is not UDP and is not TCP, we are not interested, go to the next packet (but if ipaccount is enabled, do not skip IP
		ppd->datalen = 0;
		if(opt_ipaccount == 0 && !DEBUG_ALL_PACKETS && enableReturnZeroInCheckData) {
			//cout << "pcapProcess exit 006 / protocol: " << (int)ppd->header_ip->protocol << endl;
			return(0);
		}
	}

	if(ppd->datalen < 0 && enableReturnZeroInCheckData) {
		//cout << "pcapProcess exit 007" << endl;
		return(0);
	}

	if(enableCalcMD5 || enableDedup) {
		// check for duplicate packets (md5 is expensive operation - enable only if you really need it
		if(ppd->datalen > 0 && opt_dup_check && ppd->prevmd5s != NULL && (ppd->traillen < ppd->datalen) &&
		   !(ppd->istcp && opt_enable_http && (httpportmatrix[htons(ppd->header_tcp->source)] || httpportmatrix[htons(ppd->header_tcp->dest)])) &&
		   !(ppd->istcp && opt_enable_webrtc && (webrtcportmatrix[htons(ppd->header_tcp->source)] || webrtcportmatrix[htons(ppd->header_tcp->dest)])) &&
		   !(ppd->istcp && opt_enable_ssl && (isSslIpPort(htonl(ppd->header_ip->saddr), htons(ppd->header_tcp->source)) || isSslIpPort(htonl(ppd->header_ip->daddr), htons(ppd->header_tcp->dest))))) {
			if(enableCalcMD5) {
				MD5_Init(&ppd->ctx);
				if(opt_dup_check_ipheader) {
					// check duplicates based on full ip header and data 
					MD5_Update(&ppd->ctx, ppd->header_ip, MIN(ppd->datalen - ((char*)ppd->header_ip - ppd->data), ntohs(ppd->header_ip->tot_len)));
				} else {
					// check duplicates based only on data (without ip header and without UDP/TCP header). Duplicate packets 
					// will be matched regardless on IP 
					MD5_Update(&ppd->ctx, ppd->data, MAX(0, (unsigned long)ppd->datalen - ppd->traillen));
				}
				MD5_Final((unsigned char*)(*header_packet)->_md5, &ppd->ctx);
			}
			if(enableDedup && (*header_packet)->_md5[0]) {
				if(memcmp((*header_packet)->_md5, ppd->prevmd5s + (*(*header_packet)->_md5 * MD5_DIGEST_LENGTH), MD5_DIGEST_LENGTH) == 0) {
					//printf("dropping duplicate md5[%s]\n", md5);
					duplicate_counter++;
					if(sverb.dedup) {
						cout << "*** DEDUP " << duplicate_counter << endl;
					}
					return(0);
				}
				memcpy(ppd->prevmd5s+(*(*header_packet)->_md5 * MD5_DIGEST_LENGTH), (*header_packet)->_md5, MD5_DIGEST_LENGTH);
			}
		}
	}
	
	if(enableDump) {
		if(pcapDumpHandle) {
			pcap_dump((u_char*)pcapDumpHandle, HPH(*header_packet), HPP(*header_packet));
		}
	}
	
	return(1);
}

#endif
