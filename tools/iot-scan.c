/*
 * iot-scan: An IoT Scanning Tool
 *
 * Copyright (C) 2017 Fernando Gont <fgont@si6networks.com>
 *
 * Programmed by Fernando Gont for SI6 Networks <https://www.si6networks.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Build with: make iot-scan
 *
 * It requires that the libpcap library be installed on your system.
 *
 * Please send any bug reports to Fernando Gont <fgont@si6networks.com>
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <netdb.h>
#include <pcap.h>

#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

#include "iot-scan.h"
#include "iot-toolkit.h"
#include "libiot.h"

/* #define DEBUG */

/* Function prototypes */
void				print_help(void);
void				usage(void);
int					process_config_file(const char *);


struct nodes{
	unsigned int	max;
	unsigned int	n;
	struct in_addr  *node;
};

unsigned int 		create_local_nodes(struct nodes *);
void				destroy_local_nodes(struct nodes *);
void				add_to_local_nodes(struct nodes *, struct in_addr *);
unsigned int		is_in_local_nodes(struct nodes *, struct in_addr *);



/* Used for router discovery */
struct iface_data			idata;

/* Variables used for learning the default router */
struct ether_addr			router_ether, rs_ether;

struct in6_addr				randprefix;
unsigned char				randpreflen;

bpf_u_int32				my_netmask;
bpf_u_int32				my_ip;
struct bpf_program		pcap_filter;
char 					dev[64], errbuf[PCAP_ERRBUF_SIZE];
unsigned char			buffer[BUFFER_SIZE], buffrh[MIN_IPV6_HLEN + MIN_TCP_HLEN];
char			readbuff[BUFFER_SIZE], sendbuff[BUFFER_SIZE];
ssize_t					nreadbuff, nsendbuff;
char					line[LINE_BUFFER_SIZE];
unsigned char			*v6buffer, *ptr, *startofprefixes;
char					*pref;
    
struct ether_header		*ethernet;
unsigned int			ndst=0;

char					*lasts, *rpref;
char					*charptr;

size_t					nw;
unsigned long			ul_res, ul_val;
unsigned int			i, j, startrand;
unsigned int			skip;
unsigned char			dstpreflen;

uint16_t				mask;

char 					plinkaddr[ETHER_ADDR_PLEN], pv4addr[INET_ADDRSTRLEN];
char 					pv6addr[INET6_ADDRSTRLEN];
unsigned char 			verbose_f=FALSE;
unsigned char 			dstaddr_f=FALSE, timestamps_f=FALSE, scan_local_f=FALSE;



unsigned char			dst_f=FALSE, end_f=FALSE, endpscan_f=FALSE;
unsigned char			donesending_f=FALSE;
uint16_t				srcport, dstport;
uint32_t				scan_type;
char					scan_type_f=FALSE;
unsigned long			pktinterval, rate;
unsigned int			packetsize;

struct prefixv4_entry	prefix;

char					*charstart, *charend, *lastcolon;
unsigned int			nsleep;
int						sel;
fd_set					sset, rset, wset, eset;
struct timeval			curtime, pcurtime, lastprobe;
struct tm				pcurtimetm;

unsigned int			retrans;

int main(int argc, char **argv){
	extern char				*optarg;
	int						r;
	struct addrinfo			hints, *res, *aiptr;
	struct target_ipv6		target;
	struct timeval			timeout;
	void					*voidptr;
	const int				on=1;
	struct sockaddr_in		sockaddr_in, sockaddr_from, sockaddr_to;
	socklen_t				sockaddrfrom_len;
	struct json				*json1, *json2, *json3;
	struct json_value		json_value;
	char					*alias, *dev_name, *type, *model;
	struct nodes			nodes;

	char edimax_man[EDIMAX_MAN_LEN+1], edimax_model[EDIMAX_MOD_LEN+1], edimax_version[EDIMAX_VER_LEN+1], edimax_display[EDIMAX_DIS_LEN+1];
	struct edimax_discover_response *edimax;

	unsigned long			rx_timer=500000;

	static struct option longopts[] = {
		{"interface", required_argument, 0, 'i'},
		{"dst-address", required_argument, 0, 'd'},
		{"local-scan", no_argument, 0, 'L'},
		{"retrans", required_argument, 0, 'x'},
		{"timeout", required_argument, 0, 'O'},
		{"type", required_argument, 0, 't'},
		{"verbose", no_argument, 0, 'v'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0,  0 }
	};

	char shortopts[]= "i:d:Lx:O:t:vh";

	char option;

	if(argc<=1){
		usage();
		exit(EXIT_FAILURE);
	}

	srandom(time(NULL));

	init_iface_data(&idata);
	idata.local_retrans=2;

	while((r=getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		option= r;

		switch(option) {
			case 'i':  /* Interface */
				strncpy(idata.iface, optarg, IFACE_LENGTH-1);
				idata.iface[IFACE_LENGTH-1]=0;
				idata.ifindex= if_nametoindex(idata.iface);
				idata.iface_f=TRUE;
				break;

			case 'd':	/* IPv6 Destination Address/Prefix */
				/* The '-d' option contains a domain name */
				if((charptr = strtok_r(optarg, "/", &lasts)) == NULL){
					puts("Error in Destination Address");
					exit(EXIT_FAILURE);
				}

				strncpy(target.name, charptr, NI_MAXHOST);
				target.name[NI_MAXHOST-1]=0;

				if((charptr = strtok_r(NULL, " ", &lasts)) != NULL){
					prefix.len = atoi(charptr);
		
					if(prefix.len > 32){
						puts("Prefix length error in IP Destination Address");
						exit(EXIT_FAILURE);
					}
				}
				else{
					prefix.len= 32;
				}

				memset(&hints, 0, sizeof(hints));
				hints.ai_family= AF_INET;
				hints.ai_canonname = NULL;
				hints.ai_addr = NULL;
				hints.ai_next = NULL;
				hints.ai_socktype= SOCK_DGRAM;

				if( (target.res = getaddrinfo(target.name, NULL, &hints, &res)) != 0){
					printf("Unknown Destination '%s': %s\n", target.name, gai_strerror(target.res));
					exit(1);
				}

				for(aiptr=res; aiptr != NULL; aiptr=aiptr->ai_next){
					if(aiptr->ai_family != AF_INET)
							continue;

					if(aiptr->ai_addrlen != sizeof(struct sockaddr_in))
						continue;

					if(aiptr->ai_addr == NULL)
						continue;

					prefix.ip= ( (struct sockaddr_in *)aiptr->ai_addr)->sin_addr;
				}

				freeaddrinfo(res);

				idata.dstaddr= prefix.ip;				
				idata.dstaddr_f= TRUE;
				dst_f=TRUE;
				break;
	    
			case 'L':
				scan_local_f=TRUE;
				break;

			case 'x':
				idata.local_retrans=atoi(optarg);
				break;

			case 'O':
				idata.local_timeout=atoi(optarg);
				break;

			case 't':
				scan_type_f= TRUE;

				if(strncmp(optarg, "cams", strlen("cams")) == 0 || strncmp(optarg, "cameras", strlen("cameras")) == 0){
					scan_type= scan_type | SCAN_IP_CAMERAS;
				}
				else if(strncmp(optarg, "plugs", strlen("plugs")) == 0 || strncmp(optarg, "smartplugs", strlen("smartplugs")) == 0){
					scan_type= scan_type | SCAN_SMART_PLUGS;
				}
				else if(strncmp(optarg, "all", strlen("all")) == 0){
					scan_type= scan_type | SCAN_ALL;
				}
				else{
					puts("Unknown device type in '-t' option");
					exit(EXIT_FAILURE);
				}

				break;

			case 'v':	/* Be verbose */
				idata.verbose_f++;
				break;
		
			case 'h':	/* Help */
				print_help();
				exit(EXIT_FAILURE);
				break;

			default:
				usage();
				exit(EXIT_FAILURE);
				break;
		
		} /* switch */
	} /* while(getopt) */

	/*
	    XXX: This is rather ugly, but some local functions need to check for verbosity, and it was not warranted
	    to pass &idata as an argument
	 */
	verbose_f= idata.verbose_f;

	if(geteuid()){
		puts("iot-scan needs superuser privileges to run");
		exit(EXIT_FAILURE);
	}

	if(scan_local_f && !idata.iface_f){
		/* XXX This should later allow to just specify local scan and automatically choose an interface */
		puts("Must specify the network interface with the -i option when a local scan is selected");
		exit(EXIT_FAILURE);
	}

	if(!dst_f && !scan_local_f){
		puts("Must specify either a destination prefix ('-d'), or a local scan ('-L')");
		exit(EXIT_FAILURE);
	}

	release_privileges();

	if(get_local_addrs(&idata) == FAILURE){
		puts("Error obtaining list of local interfaces and addresses");
		exit(EXIT_FAILURE);
	}

/*	debug_print_iflist(&(idata.iflist)); */

	if(!scan_type_f){
		scan_type= SCAN_IP_CAMERAS | SCAN_SMART_PLUGS;
	}

	if(scan_local_f){
		/* If an interface was specified, we select an IPv4 address from such interface */
		if(idata.iface_f){
			if( (voidptr=find_v4addr_for_iface(&(idata.iflist), idata.iface)) == NULL){
				printf("No IPv4 address for interface %s\n", idata.iface);
				exit(EXIT_FAILURE);
			}

			idata.srcaddr= *((struct in_addr *) voidptr);
		}
		else{
			if( (voidptr=find_v4addr(&(idata.iflist))) == NULL){
				puts("No IPv4 address available on local host");
				exit(EXIT_FAILURE);
			}

			idata.srcaddr= *((struct in_addr *)voidptr);
		}

		if( (idata.fd=socket(AF_INET, SOCK_DGRAM, 0)) == -1){
			puts("Could not create socket");
			exit(EXIT_FAILURE);
		}

		if( setsockopt(idata.fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) == -1){
			puts("Error while setting SO_BROADCAST socket option");
			exit(EXIT_FAILURE);
		}

		memset(&sockaddr_in, 0, sizeof(sockaddr_in));
		sockaddr_in.sin_family= AF_INET;
		sockaddr_in.sin_port= 0;  /* Allow Sockets API to set an ephemeral port */
		sockaddr_in.sin_addr= idata.srcaddr;

		if(bind(idata.fd, (struct sockaddr *) &sockaddr_in, sizeof(sockaddr_in)) == -1){
			puts("Error bind()ing socket to local address");
			exit(EXIT_FAILURE);
		}


		/* TP-Link Smart plugs */
		if(scan_type | SCAN_SMART_PLUGS){
			retrans=0;
			if(!create_local_nodes(&nodes)){
				puts("Not enough memory");
				exit(EXIT_FAILURE);
			}

			memset(&sockaddr_to, 0, sizeof(sockaddr_to));
			sockaddr_to.sin_family= AF_INET;
			sockaddr_to.sin_port= htons(TP_LINK_SMART_PORT);

			memset(&sockaddr_from, 0, sizeof(sockaddr_from));
			sockaddr_from.sin_family= AF_INET;
			sockaddrfrom_len=sizeof(sockaddr_from);


			if ( inet_pton(AF_INET, IP_LIMITED_MULTICAST, &(sockaddr_to.sin_addr)) <= 0){
				puts("inet_pton(): Error setting multicast address");
				exit(EXIT_FAILURE);
			}

			FD_ZERO(&sset);
			FD_SET(idata.fd, &sset);

			lastprobe.tv_sec= 0;	
			lastprobe.tv_usec=0;
			idata.pending_write_f=TRUE;	

			/* The end_f flag is set after the last probe has been sent and a timeout period has elapsed.
			   That is, we give responses enough time to come back
			 */
			while(!end_f){
				rset= sset;
				wset= sset;
				eset= sset;

				if(!donesending_f){
					/* This is the retransmission timer */
					timeout.tv_sec=  rx_timer/1000000;
					timeout.tv_usec= rx_timer%1000000;
				}
				else{
					/* XXX: This should use the parameter from command line */
					timeout.tv_sec= idata.local_timeout;
					timeout.tv_usec=0;
				}

				/*
					Check for readability and exceptions. We only check for writeability if there is pending data
					to send.
				 */
				if((sel=select(idata.fd+1, &rset, (idata.pending_write_f?&wset:NULL), &eset, &timeout)) == -1){
					if(errno == EINTR){
						continue;
					}
					else{
						perror("iot-scan:");
						exit(EXIT_FAILURE);
					}
				}

				if(gettimeofday(&curtime, NULL) == -1){
					if(idata.verbose_f)
						perror("iot-scan");

					exit(EXIT_FAILURE);
				}

				/* Check whether we have finished probing all targets */
				if(donesending_f){
					/*
					   Just wait for SELECT_TIMEOUT seconds for any incoming responses.
					*/

					if(is_time_elapsed(&curtime, &lastprobe, idata.local_timeout * 1000000)){
						end_f=TRUE;
					}
				}


				if(sel && FD_ISSET(idata.fd, &rset)){
					/* XXX: Process response packet */

					if( (nreadbuff = recvfrom(idata.fd, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}


					if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					tp_link_decrypt((unsigned char *)readbuff, nreadbuff);

					alias= NULL_STRING;
					dev_name= NULL_STRING;
					type= NULL_STRING;
					model= NULL_STRING;

					/* Get to system:get_sysinfo */
					if( (json1=json_get_objects(readbuff, (nreadbuff))) != NULL){
						if( json_get_value(json1, &json_value, "\"system\"")){
							if( (json2=json_get_objects(json_value.value, json_value.len)) != NULL){
								if( json_get_value(json2, &json_value, "\"get_sysinfo\"")){
									if( (json3=json_get_objects(json_value.value, json_value.len)) != NULL){
										json_remove_quotes(json3);
										if( json_get_value(json3, &json_value, "type")){
											type=json_value.value;
										}
										if( json_get_value(json3, &json_value, "model")){
											model=json_value.value;
										}
										if( json_get_value(json3, &json_value, "dev_name")){
											dev_name=json_value.value;
										}
										if( json_get_value(json3, &json_value, "alias")){
											alias=json_value.value;
										}	

										if( !is_in_local_nodes(&nodes, &(sockaddr_from.sin_addr))){
											add_to_local_nodes(&nodes, &(sockaddr_from.sin_addr));
											printf("%s # %s: TP-Link %s: %s: \"%s\"\n", pv4addr, type, model, dev_name, alias);
										}
									}
								}
							}
						}
					}
				}


				if(!donesending_f && !idata.pending_write_f && is_time_elapsed(&curtime, &lastprobe, rx_timer)){
					idata.pending_write_f=TRUE;
					continue;
				}

				if(!donesending_f && idata.pending_write_f && FD_ISSET(idata.fd, &wset)){
					idata.pending_write_f=FALSE;

					/* XXX: SEND PROBE PACKET */
					nsendbuff= Strnlen(TP_LINK_SMART_DISCOVER, MAX_TP_COMMAND_LENGTH);
					memcpy(sendbuff, TP_LINK_SMART_DISCOVER, nsendbuff);
					tp_link_crypt((unsigned char *)sendbuff, nsendbuff);

					if( sendto(idata.fd, sendbuff, nsendbuff, 0, (struct sockaddr *) &sockaddr_to, sizeof(sockaddr_to)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if(gettimeofday(&lastprobe, NULL) == -1){
						if(idata.verbose_f)
							perror("iot-scan");

						exit(EXIT_FAILURE);
					}

					retrans++;

					if(retrans >= idata.local_retrans)
						donesending_f= 1;

				}


				if(FD_ISSET(idata.fd, &eset)){
					if(idata.verbose_f)
						puts("iot-scaner: Found exception on descriptor");

					exit(EXIT_FAILURE);
				}
			}

			destroy_local_nodes(&nodes);
		}


		/* Edimax SmartPLugs */
		if(scan_type | SCAN_SMART_PLUGS){
			retrans=0;

			if(!create_local_nodes(&nodes)){
				puts("Not enough memory");
				exit(EXIT_FAILURE);
			}

			donesending_f=FALSE;
			end_f=FALSE;
			memset(&sockaddr_to, 0, sizeof(sockaddr_to));
			sockaddr_to.sin_family= AF_INET;
			sockaddr_to.sin_port= htons(EDIMAX_SMART_PLUG_SERVICE_PORT);

			memset(&sockaddr_from, 0, sizeof(sockaddr_from));
			sockaddr_from.sin_family= AF_INET;
			sockaddrfrom_len=sizeof(sockaddr_from);


			if ( inet_pton(AF_INET, IP_LIMITED_MULTICAST, &(sockaddr_to.sin_addr)) <= 0){
				puts("inet_pton(): Error setting multicast address");
				exit(EXIT_FAILURE);
			}

			FD_ZERO(&sset);
			FD_SET(idata.fd, &sset);

			lastprobe.tv_sec= 0;	
			lastprobe.tv_usec=0;
			idata.pending_write_f=TRUE;	

			/* The end_f flag is set after the last probe has been sent and a timeout period has elapsed.
			   That is, we give responses enough time to come back
			 */
			while(!end_f){
				rset= sset;
				wset= sset;
				eset= sset;

				if(!donesending_f){
					/* This is the retransmission timer */
					timeout.tv_sec=  rx_timer/1000000;
					timeout.tv_usec= rx_timer%1000000;
				}
				else{
					/* XXX: This should use the parameter from command line */
					timeout.tv_sec= idata.local_timeout;
					timeout.tv_usec=0;
				}

				/*
					Check for readability and exceptions. We only check for writeability if there is pending data
					to send.
				 */
				if((sel=select(idata.fd+1, &rset, (idata.pending_write_f?&wset:NULL), &eset, &timeout)) == -1){
					if(errno == EINTR){
						continue;
					}
					else{
						perror("iot-scan:");
						exit(EXIT_FAILURE);
					}
				}

				if(gettimeofday(&curtime, NULL) == -1){
					if(idata.verbose_f)
						perror("iot-scan");

					exit(EXIT_FAILURE);
				}

				/* Check whether we have finished probing all targets */
				if(donesending_f){
					/*
					   Just wait for SELECT_TIMEOUT seconds for any incoming responses.
					*/

					if(is_time_elapsed(&curtime, &lastprobe, idata.local_timeout * 1000000)){
						end_f=TRUE;
					}
				}


				if(sel && FD_ISSET(idata.fd, &rset)){
					/* XXX: Process response packet */

					if( (nreadbuff = recvfrom(idata.fd, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if( nreadbuff == sizeof(struct edimax_discover_response)){
						if( !is_in_local_nodes(&nodes, &(sockaddr_from.sin_addr))){
							add_to_local_nodes(&nodes, &(sockaddr_from.sin_addr));

							if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
								perror("iot-scan: ");
								exit(EXIT_FAILURE);
							}

							edimax= (struct edimax_discover_response *) readbuff;

							strncpy(edimax_man, edimax->manufacturer, EDIMAX_MAN_LEN);
							edimax_man[EDIMAX_MAN_LEN]=0;

							strncpy(edimax_model, edimax->model, EDIMAX_MOD_LEN);
							edimax_model[EDIMAX_MOD_LEN]=0;

							strncpy(edimax_version, edimax->version, EDIMAX_VER_LEN);
							edimax_version[EDIMAX_VER_LEN]=0;

							strncpy(edimax_display, edimax->displayname, EDIMAX_DIS_LEN);
							edimax_display[EDIMAX_DIS_LEN]=0;

							printf("%s # smartplug: %s %s %s: \"%s\"\n", pv4addr, edimax_man, edimax_model, edimax_version, edimax_display);
						}
					}

				}


				if(!donesending_f && !idata.pending_write_f && is_time_elapsed(&curtime, &lastprobe, rx_timer)){
					idata.pending_write_f=TRUE;
					continue;
				}

				if(!donesending_f && idata.pending_write_f && FD_ISSET(idata.fd, &wset)){
					idata.pending_write_f=FALSE;

					/* XXX: SEND PROBE PACKET */

					/* XXX: Will not happen, but still check in case code is changed */
					if(sizeof(GENIUS_IP_CAMERA_DISCOVER) > sizeof(sendbuff)){
						puts("Internal buffer too short");
						exit(EXIT_FAILURE);
					}

					nsendbuff= sizeof(EDIMAX_SMART_PLUG_DISCOVER);
					memcpy(sendbuff, EDIMAX_SMART_PLUG_DISCOVER, nsendbuff);

					if( sendto(idata.fd, sendbuff, nsendbuff, 0, (struct sockaddr *) &sockaddr_to, sizeof(sockaddr_to)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if(gettimeofday(&lastprobe, NULL) == -1){
						if(idata.verbose_f)
							perror("iot-scan");

						exit(EXIT_FAILURE);
					}

					retrans++;

					if(retrans >= idata.local_retrans)
						donesending_f= 1;

				}


				if(FD_ISSET(idata.fd, &eset)){
					if(idata.verbose_f)
						puts("iot-scan: Found exception on descriptor");

					exit(EXIT_FAILURE);
				}
			}

			destroy_local_nodes(&nodes);
		}




		/* TP-Link IP Cameras */
		if(scan_type | SCAN_IP_CAMERAS){
			retrans=0;

			if(!create_local_nodes(&nodes)){
				puts("Not enough memory");
				exit(EXIT_FAILURE);
			}

			donesending_f=FALSE;
			end_f=FALSE;
			memset(&sockaddr_to, 0, sizeof(sockaddr_to));
			sockaddr_to.sin_family= AF_INET;
			sockaddr_to.sin_port= htons(TP_LINK_IP_CAMERA_TDDP_PORT);

			memset(&sockaddr_from, 0, sizeof(sockaddr_from));
			sockaddr_from.sin_family= AF_INET;
			sockaddrfrom_len=sizeof(sockaddr_from);


			if ( inet_pton(AF_INET, IP_LIMITED_MULTICAST, &(sockaddr_to.sin_addr)) <= 0){
				puts("inet_pton(): Error setting multicast address");
				exit(EXIT_FAILURE);
			}

			FD_ZERO(&sset);
			FD_SET(idata.fd, &sset);

			lastprobe.tv_sec= 0;	
			lastprobe.tv_usec=0;
			idata.pending_write_f=TRUE;	

			/* The end_f flag is set after the last probe has been sent and a timeout period has elapsed.
			   That is, we give responses enough time to come back
			 */
			while(!end_f){
				rset= sset;
				wset= sset;
				eset= sset;

				if(!donesending_f){
					/* This is the retransmission timer */
					timeout.tv_sec=  rx_timer/1000000;
					timeout.tv_usec= rx_timer%1000000;
				}
				else{
					/* XXX: This should use the parameter from command line */
					timeout.tv_sec= idata.local_timeout;
					timeout.tv_usec=0;
				}

				/*
					Check for readability and exceptions. We only check for writeability if there is pending data
					to send.
				 */
				if((sel=select(idata.fd+1, &rset, (idata.pending_write_f?&wset:NULL), &eset, &timeout)) == -1){
					if(errno == EINTR){
						continue;
					}
					else{
						perror("iot-scan:");
						exit(EXIT_FAILURE);
					}
				}

				if(gettimeofday(&curtime, NULL) == -1){
					if(idata.verbose_f)
						perror("iot-scan");

					exit(EXIT_FAILURE);
				}

				/* Check whether we have finished probing all targets */
				if(donesending_f){
					/*
					   Just wait for SELECT_TIMEOUT seconds for any incoming responses.
					*/

					if(is_time_elapsed(&curtime, &lastprobe, idata.local_timeout * 1000000)){
						end_f=TRUE;
					}
				}


				if(sel && FD_ISSET(idata.fd, &rset)){
					/* XXX: Process response packet */

					if( (nreadbuff = recvfrom(idata.fd, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}


					if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					/* Compare response with known one */
					if(nreadbuff == sizeof(TP_LINK_IP_CAMERA_RESPONSE)){
						if(memcmp(readbuff, TP_LINK_IP_CAMERA_RESPONSE, nreadbuff) == 0){
							if( !is_in_local_nodes(&nodes, &(sockaddr_from.sin_addr))){
								add_to_local_nodes(&nodes, &(sockaddr_from.sin_addr));
								printf("%s # camera: TP-Link IP camera\n", pv4addr);
							}
						}
					}

				}


				if(!donesending_f && !idata.pending_write_f && is_time_elapsed(&curtime, &lastprobe, rx_timer)){
					idata.pending_write_f=TRUE;
					continue;
				}

				if(!donesending_f && idata.pending_write_f && FD_ISSET(idata.fd, &wset)){
					idata.pending_write_f=FALSE;

					/* XXX: SEND PROBE PACKET */

					/* XXX: Will not happen, but still check in case code is changed */
					if(sizeof(TP_LINK_IP_CAMERA_DISCOVER) > sizeof(sendbuff)){
						puts("Internal buffer too short");
						exit(EXIT_FAILURE);
					}

					nsendbuff= sizeof(TP_LINK_IP_CAMERA_DISCOVER);
					memcpy(sendbuff, TP_LINK_IP_CAMERA_DISCOVER, nsendbuff);

					if( sendto(idata.fd, sendbuff, nsendbuff, 0, (struct sockaddr *) &sockaddr_to, sizeof(sockaddr_to)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if(gettimeofday(&lastprobe, NULL) == -1){
						if(idata.verbose_f)
							perror("iot-scan");

						exit(EXIT_FAILURE);
					}

					retrans++;

					if(retrans >= idata.local_retrans)
						donesending_f= 1;

				}


				if(FD_ISSET(idata.fd, &eset)){
					if(idata.verbose_f)
						puts("iot-scan: Found exception on descriptor");

					exit(EXIT_FAILURE);
				}
			}

			destroy_local_nodes(&nodes);
		}


		/* Genius IP cameras */
		if(scan_type | SCAN_IP_CAMERAS){
			retrans=0;

			if(!create_local_nodes(&nodes)){
				puts("Not enough memory");
				exit(EXIT_FAILURE);
			}

			donesending_f=FALSE;
			end_f=FALSE;
			memset(&sockaddr_to, 0, sizeof(sockaddr_to));
			sockaddr_to.sin_family= AF_INET;
			sockaddr_to.sin_port= htons(GENIUS_IP_CAMERA_SERVICE_PORT);

			memset(&sockaddr_from, 0, sizeof(sockaddr_from));
			sockaddr_from.sin_family= AF_INET;
			sockaddrfrom_len=sizeof(sockaddr_from);


			if ( inet_pton(AF_INET, IP_LIMITED_MULTICAST, &(sockaddr_to.sin_addr)) <= 0){
				puts("inet_pton(): Error setting multicast address");
				exit(EXIT_FAILURE);
			}

			FD_ZERO(&sset);
			FD_SET(idata.fd, &sset);

			lastprobe.tv_sec= 0;	
			lastprobe.tv_usec=0;
			idata.pending_write_f=TRUE;	

			/* The end_f flag is set after the last probe has been sent and a timeout period has elapsed.
			   That is, we give responses enough time to come back
			 */
			while(!end_f){
				rset= sset;
				wset= sset;
				eset= sset;

				if(!donesending_f){
					/* This is the retransmission timer */
					timeout.tv_sec=  rx_timer/1000000;
					timeout.tv_usec= rx_timer%1000000;
				}
				else{
					/* XXX: This should use the parameter from command line */
					timeout.tv_sec= idata.local_timeout;
					timeout.tv_usec=0;
				}

				/*
					Check for readability and exceptions. We only check for writeability if there is pending data
					to send.
				 */
				if((sel=select(idata.fd+1, &rset, (idata.pending_write_f?&wset:NULL), &eset, &timeout)) == -1){
					if(errno == EINTR){
						continue;
					}
					else{
						perror("iot-scan:");
						exit(EXIT_FAILURE);
					}
				}

				if(gettimeofday(&curtime, NULL) == -1){
					if(idata.verbose_f)
						perror("iot-scan");

					exit(EXIT_FAILURE);
				}

				/* Check whether we have finished probing all targets */
				if(donesending_f){
					/*
					   Just wait for SELECT_TIMEOUT seconds for any incoming responses.
					*/

					if(is_time_elapsed(&curtime, &lastprobe, idata.local_timeout * 1000000)){
						end_f=TRUE;
					}
				}


				if(sel && FD_ISSET(idata.fd, &rset)){
					/* XXX: Process response packet */

					if( (nreadbuff = recvfrom(idata.fd, readbuff, sizeof(readbuff), 0, (struct sockaddr *)&sockaddr_from, &sockaddrfrom_len)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}
	/* puts("Got response"); */

					if(inet_ntop(AF_INET, &(sockaddr_from.sin_addr), pv4addr, sizeof(pv4addr)) == NULL){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if(nreadbuff == sizeof(GENIUS_IP_CAMERA_RESPONSE) && ntohs(sockaddr_from.sin_port) == GENIUS_IP_CAMERA_SENDING_PORT){
						if( !is_in_local_nodes(&nodes, &(sockaddr_from.sin_addr))){
							add_to_local_nodes(&nodes, &(sockaddr_from.sin_addr));
							printf("%s # camera: Genius IP camera\n", pv4addr);
						}
					}
				}


				if(!donesending_f && !idata.pending_write_f && is_time_elapsed(&curtime, &lastprobe, rx_timer)){
					idata.pending_write_f=TRUE;
					continue;
				}

				if(!donesending_f && idata.pending_write_f && FD_ISSET(idata.fd, &wset)){
					idata.pending_write_f=FALSE;

					/* XXX: SEND PROBE PACKET */

					/* XXX: Will not happen, but still check in case code is changed */
					if(sizeof(GENIUS_IP_CAMERA_DISCOVER) > sizeof(sendbuff)){
						puts("Internal buffer too short");
						exit(EXIT_FAILURE);
					}

					nsendbuff= sizeof(GENIUS_IP_CAMERA_DISCOVER);
					memcpy(sendbuff, GENIUS_IP_CAMERA_DISCOVER, nsendbuff);

					if( sendto(idata.fd, sendbuff, nsendbuff, 0, (struct sockaddr *) &sockaddr_to, sizeof(sockaddr_to)) == -1){
						perror("iot-scan: ");
						exit(EXIT_FAILURE);
					}

					if(gettimeofday(&lastprobe, NULL) == -1){
						if(idata.verbose_f)
							perror("iot-scan");

						exit(EXIT_FAILURE);
					}

					retrans++;

					if(retrans >= idata.local_retrans)
						donesending_f= 1;

				}


				if(FD_ISSET(idata.fd, &eset)){
					if(idata.verbose_f)
						puts("iot-scan: Found exception on descriptor");

					exit(EXIT_FAILURE);
				}
			}

			destroy_local_nodes(&nodes);
		}

	}

	exit(EXIT_SUCCESS);
}




/*
 * Function: match_strings()
 *
 * Checks whether one string "matches" within another string
 */

int match_strings(char *buscar, char *buffer){
	unsigned int buscars, buffers;
	unsigned int i=0, j=0;

	buscars= Strnlen(buscar, MAX_IEEE_OUIS_LINE_SIZE);
	buffers= Strnlen(buffer, MAX_IEEE_OUIS_LINE_SIZE);

	if(buscars > buffers)
		return(0);

	while(i <= (buffers - buscars)){
		j=0;

		while(j < buscars){
			if(toupper((int) ((unsigned char)buscar[j])) != toupper((int) ((unsigned char)buffer[i+j])))
				break;

			j++;
		}

		if(j >= buscars)
			return(1);

		i++;
	}

	return(0);
}





/*
 * Function: usage()
 *
 * Prints the syntax of the iot-scan tool
 */

void usage(void){
	puts("usage: iot-scan (-L | -d) [-i INTERFACE] [-v] [-h]");
}


/*
 * Function: print_help()
 *
 * Prints help information for the iot-scan tool
 */

void print_help(void){
	puts(SI6_TOOLKIT);
	puts( "iot-scan: An IoT scanning tool\n");
	usage();
    
	puts("\nOPTIONS:\n"
	     "  --interface, -i             Network interface\n"
	     "  --dst-address, -d           Destination Range or Prefix\n"
	     "  --local-scan, -L            Scan the local subnet\n"
	     "  --retrans, -x               Number of retransmissions of each probe\n"
	     "  --timeout, -O               Timeout in seconds (default: 1 second)\n"
		 "  --type, -t                  Target device type\n"
	     "  --help, -h                  Print help for the iot-scan tool\n"
	     "  --verbose, -v               Be verbose\n"
	     "\n"
	     " Programmed by Fernando Gont for SI6 Networks <http://www.si6networks.com>\n"
	     " Please send any bug reports to <fgont@si6networks.com>\n"
	);
}



/*
 * Function: create_local_nodes()
 *
 * Creates structure for discarding duplicate nodes
 */

unsigned int create_local_nodes(struct nodes *nodes){
#define MAX_LOCAL_NODES 65535

	nodes->max= MAX_LOCAL_NODES;
	nodes->n=0;

	if( (nodes->node= malloc(sizeof(struct in_addr) * MAX_LOCAL_NODES)) == NULL)
		return FALSE;

	return TRUE;
}


/*
 * Function: destroy_local_nodes()
 *
 * Destroys structure for discarding duplicate nodes
 */

void destroy_local_nodes(struct nodes *nodes){
#define MAX_LOCAL_NODES 65535

	nodes->max=0;
	nodes->n=0;
	free(nodes->node);
	return;
}


/*
 * Function: add_to_local_nodes()
 *
 * Adds node to structure of local nodes
 */

void add_to_local_nodes(struct nodes *nodes, struct in_addr *node){
	if(nodes->n >= nodes->max)
		return;

	if(nodes->node == NULL)
		return;

	nodes->node[nodes->n]= *node;
	(nodes->n)++;
	return;
}


/*
 * Function: is_in_local_nodes()
 *
 * Check if node is in list of local nodes
 */

unsigned int is_in_local_nodes(struct nodes *nodes, struct in_addr *node){
	unsigned int i;

	for(i=0; i < nodes->n; i++){
		if( nodes->node[i].s_addr == node->s_addr)
			return TRUE;
	}

	return FALSE;
}




