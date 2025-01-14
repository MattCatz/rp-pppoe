/***********************************************************************
*
* discovery.c
*
* Perform PPPoE discovery
*
* Copyright (C) 1999-2015 by Roaring Penguin Software Inc.
* Copyright (C) 2018-2023 Dianne Skoll
*
* SPDX-License-Identifier: GPL-2.0-or-later
*
***********************************************************************/

#define _GNU_SOURCE 1

#include "config.h"

#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <signal.h>

#include "pppoe.h"

#ifdef PLUGIN
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDDEF_H 1
#include "pppd/pppd.h"
#include "pppd/fsm.h"
#include "pppd/lcp.h"

#ifdef PPPD_VERSION
/* New-style pppd API */
int persist = 1;
#endif

#else
int persist = 0;
#endif

/* Structure used by parseForHostUniq */
struct HostUniqInfo {
    char *hostUniq;
    int forMe;
};

/* Calculate time remaining until *expire_at into *tv, returns 0 if now >= *expire_at */
static int
time_left(struct timeval *tv, struct timeval *expire_at)
{
    struct timeval now;

    if (gettimeofday(&now, NULL) < 0) {
	fatalSys("gettimeofday (time_left)");
    }
    tv->tv_sec = expire_at->tv_sec - now.tv_sec;
    tv->tv_usec = expire_at->tv_usec - now.tv_usec;
    if (tv->tv_usec < 0) {
	tv->tv_usec += 1000000;
	if (tv->tv_sec) {
	    tv->tv_sec--;
	} else {
	    /* Timed out */
	    return 0;
	}
    }
    if (tv->tv_sec <= 0 && tv->tv_usec <= 0) {
	/* Timed out */
	return 0;
    }

    return 1;
}

/**********************************************************************
*%FUNCTION: parseForHostUniq
*%ARGUMENTS:
* type -- tag type
* len -- tag length
* data -- tag data.
* extra -- user-supplied pointer.  This is assumed to be a pointer to a
*          HostUniqInfo structure
*%RETURNS:
* Nothing
*%DESCRIPTION:
* If a HostUnique tag is found which matches our PID, sets *extra to 1.
***********************************************************************/
static void
parseForHostUniq(uint16_t type, uint16_t len, unsigned char *data,
		 void *extra)
{
    struct HostUniqInfo *hi = (struct HostUniqInfo *) extra;
    if (!hi->hostUniq) return;

    if (type == TAG_HOST_UNIQ && len == strlen(hi->hostUniq) && !memcmp(data, hi->hostUniq, len)) {
	hi->forMe = 1;
    }
}

/**********************************************************************
*%FUNCTION: packetIsForMe
*%ARGUMENTS:
* conn -- PPPoE connection info
* packet -- a received PPPoE packet
*%RETURNS:
* 1 if packet is for this PPPoE daemon; 0 otherwise.
*%DESCRIPTION:
* If we are using the Host-Unique tag, verifies that packet contains
* our unique identifier.
***********************************************************************/
static int
packetIsForMe(PPPoEConnection *conn, PPPoEPacket *packet)
{
    struct HostUniqInfo hi;

    /* If packet is not directed to our MAC address, forget it */
    if (memcmp(packet->ethHdr.h_dest, conn->myEth, ETH_ALEN)) return 0;

    /* If we're not using the Host-Unique tag, then accept the packet */
    if (!conn->hostUniq) return 1;

    hi.hostUniq = conn->hostUniq;
    hi.forMe = 0;
    parsePacket(packet, parseForHostUniq, &hi);
    return hi.forMe;
}

/**********************************************************************
*%FUNCTION: parsePADOTags
*%ARGUMENTS:
* type -- tag type
* len -- tag length
* data -- tag data
* extra -- extra user data.  Should point to a PacketCriteria structure
*          which gets filled in according to selected AC name and service
*          name.
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Picks interesting tags out of a PADO packet
***********************************************************************/
static void
parsePADOTags(uint16_t type, uint16_t len, unsigned char *data,
	      void *extra)
{
    struct PacketCriteria *pc = (struct PacketCriteria *) extra;
    PPPoEConnection *conn = pc->conn;
    int i;
#ifdef PLUGIN
    uint16_t mru;
#endif

    switch(type) {
    case TAG_AC_NAME:
	pc->seenACName = 1;
	if (conn->printACNames) {
	    printf("Access-Concentrator: %.*s\n", (int) len, data);
	}
	if (conn->acName && len == strlen(conn->acName) &&
	    !strncmp((char *) data, conn->acName, len)) {
	    pc->acNameOK = 1;
	}
	break;
    case TAG_SERVICE_NAME:
	pc->seenServiceName = 1;
	if (conn->printACNames && len > 0) {
	    printf("       Service-Name: %.*s\n", (int) len, data);
	}
	if (conn->serviceName && len == strlen(conn->serviceName) &&
	    !strncmp((char *) data, conn->serviceName, len)) {
	    pc->serviceNameOK = 1;
	}
	break;
    case TAG_AC_COOKIE:
	if (conn->printACNames) {
	    printf("Got a cookie:");
	    /* Print first 20 bytes of cookie */
	    for (i=0; i<len && i < 20; i++) {
		printf(" %02x", (unsigned) data[i]);
	    }
	    if (i < len) printf("...");
	    printf("\n");
	}
	conn->cookie.type = htons(type);
	conn->cookie.length = htons(len);
	memcpy(conn->cookie.payload, data, len);
	break;
    case TAG_RELAY_SESSION_ID:
	if (conn->printACNames) {
	    printf("Got a Relay-ID:");
	    /* Print first 20 bytes of relay ID */
	    for (i=0; i<len && i < 20; i++) {
		printf(" %02x", (unsigned) data[i]);
	    }
	    if (i < len) printf("...");
	    printf("\n");
	}
	conn->relayId.type = htons(type);
	conn->relayId.length = htons(len);
	memcpy(conn->relayId.payload, data, len);
	break;
    case TAG_SERVICE_NAME_ERROR:
	if (conn->printACNames) {
	    printf("Got a Service-Name-Error tag: %.*s\n", (int) len, data);
	} else {
	    pktLogErrs("PADO", type, len, data, extra);
	    pc->gotError = 1;
	    if (!persist) {
		exit(EXIT_FAILURE);
	    }
	}
	break;
    case TAG_AC_SYSTEM_ERROR:
	if (conn->printACNames) {
	    printf("Got a System-Error tag: %.*s\n", (int) len, data);
	} else {
	    pktLogErrs("PADO", type, len, data, extra);
	    pc->gotError = 1;
	    if (!persist) {
		exit(EXIT_FAILURE);
	    }
	}
	break;
    case TAG_GENERIC_ERROR:
	if (conn->printACNames) {
	    printf("Got a Generic-Error tag: %.*s\n", (int) len, data);
	} else {
	    pktLogErrs("PADO", type, len, data, extra);
	    pc->gotError = 1;
	    if (!persist) {
		exit(EXIT_FAILURE);
	    }
	}
	break;
#ifdef PLUGIN
    case TAG_PPP_MAX_PAYLOAD:
	if (len == sizeof(mru)) {
	    memcpy(&mru, data, sizeof(mru));
	    mru = ntohs(mru);
	    if (mru >= ETH_PPPOE_MTU) {
		if (lcp_allowoptions[0].mru > mru) lcp_allowoptions[0].mru = mru;
		if (lcp_wantoptions[0].mru > mru) lcp_wantoptions[0].mru = mru;
		conn->seenMaxPayload = 1;
	    }
	}
	break;
#endif
    }
}

/**********************************************************************
*%FUNCTION: parsePADSTags
*%ARGUMENTS:
* type -- tag type
* len -- tag length
* data -- tag data
* extra -- extra user data (pointer to PPPoEConnection structure)
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Picks interesting tags out of a PADS packet
***********************************************************************/
static void
parsePADSTags(uint16_t type, uint16_t len, unsigned char *data,
	      void *extra)
{
#ifdef PLUGIN
    uint16_t mru;
#endif
    PPPoEConnection *conn = (PPPoEConnection *) extra;
    switch(type) {
    case TAG_SERVICE_NAME:
	syslog(LOG_DEBUG, "PADS: Service-Name: '%.*s'", (int) len, data);
	break;
    case TAG_GENERIC_ERROR:
    case TAG_AC_SYSTEM_ERROR:
    case TAG_SERVICE_NAME_ERROR:
	pktLogErrs("PADS", type, len, data, extra);
	conn->PADSHadError = 1;
	break;
    case TAG_RELAY_SESSION_ID:
	conn->relayId.type = htons(type);
	conn->relayId.length = htons(len);
	memcpy(conn->relayId.payload, data, len);
	break;
#ifdef PLUGIN
    case TAG_PPP_MAX_PAYLOAD:
	if (len == sizeof(mru)) {
	    memcpy(&mru, data, sizeof(mru));
	    mru = ntohs(mru);
	    if (mru >= ETH_PPPOE_MTU) {
		if (lcp_allowoptions[0].mru > mru) lcp_allowoptions[0].mru = mru;
		if (lcp_wantoptions[0].mru > mru) lcp_wantoptions[0].mru = mru;
		conn->seenMaxPayload = 1;
	    }
	}
	break;
#endif
    }
}

/***********************************************************************
*%FUNCTION: sendPADI
*%ARGUMENTS:
* conn -- PPPoEConnection structure
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Sends a PADI packet
***********************************************************************/
static void
sendPADI(PPPoEConnection *conn)
{
    PPPoEPacket packet;
    unsigned char *cursor = packet.payload;
    PPPoETag *svc = (PPPoETag *) (&packet.payload);
    uint16_t namelen = 0;
    uint16_t plen;
    int omit_service_name = 0;

    if (conn->serviceName) {
	namelen = (uint16_t) strlen(conn->serviceName);
	if (!strcmp(conn->serviceName, "NO-SERVICE-NAME-NON-RFC-COMPLIANT")) {
	    omit_service_name = 1;
	}
    }

    /* Set destination to Ethernet broadcast address */
    memset(packet.ethHdr.h_dest, 0xFF, ETH_ALEN);
    memcpy(packet.ethHdr.h_source, conn->myEth, ETH_ALEN);

    packet.ethHdr.h_proto = htons(Eth_PPPOE_Discovery);
    packet.vertype = PPPOE_VER_TYPE(1, 1);
    packet.code = CODE_PADI;
    packet.session = 0;

    if (!omit_service_name) {
	plen = TAG_HDR_SIZE + namelen;
	CHECK_ROOM(cursor, packet.payload, plen);

	svc->type = TAG_SERVICE_NAME;
	svc->length = htons(namelen);

	if (conn->serviceName) {
	    memcpy(svc->payload, conn->serviceName, strlen(conn->serviceName));
	}
	cursor += namelen + TAG_HDR_SIZE;
    } else {
	plen = 0;
    }

    /* If we're using Host-Uniq, copy it over */
    if (conn->hostUniq) {
	PPPoETag hostUniq;
	int len = (int) strlen(conn->hostUniq);
	hostUniq.type = htons(TAG_HOST_UNIQ);
	hostUniq.length = htons(len);
	CHECK_ROOM(cursor, packet.payload, len + TAG_HDR_SIZE);
	memcpy(hostUniq.payload, conn->hostUniq, len);
	memcpy(cursor, &hostUniq, len + TAG_HDR_SIZE);
	cursor += len + TAG_HDR_SIZE;
	plen += len + TAG_HDR_SIZE;
    }

#ifdef PLUGIN
#ifndef MIN
#define MIN(x,y) ( (x) < (y) ? (x) : (y) )
#endif
    /* Add our maximum MTU/MRU */
    if (MIN(lcp_allowoptions[0].mru, lcp_wantoptions[0].mru) > ETH_PPPOE_MTU) {
	PPPoETag maxPayload;
	uint16_t mru = htons(MIN(lcp_allowoptions[0].mru, lcp_wantoptions[0].mru));
	maxPayload.type = htons(TAG_PPP_MAX_PAYLOAD);
	maxPayload.length = htons(sizeof(mru));
	CHECK_ROOM(cursor, packet.payload, sizeof(mru) + TAG_HDR_SIZE);
	memcpy(maxPayload.payload, &mru, sizeof(mru));
	memcpy(cursor, &maxPayload, sizeof(mru) + TAG_HDR_SIZE);
	cursor += sizeof(mru) + TAG_HDR_SIZE;
	plen += sizeof(mru) + TAG_HDR_SIZE;
    }
#endif

    packet.length = htons(plen);

    sendPacket(conn, conn->discoverySocket, &packet, (int) (plen + HDR_SIZE));
#ifdef DEBUGGING_ENABLED
    if (conn->debugFile) {
	dumpPacket(conn->debugFile, &packet, "SENT");
	fprintf(conn->debugFile, "\n");
	fflush(conn->debugFile);
    }
#endif
}

/**********************************************************************
*%FUNCTION: waitForPADO
*%ARGUMENTS:
* conn -- PPPoEConnection structure
* timeout -- how long to wait (in seconds)
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Waits for a PADO packet and copies useful information
***********************************************************************/
static void
waitForPADO(PPPoEConnection *conn, int timeout)
{
    fd_set readable;
    int r;
    struct timeval tv;
    struct timeval expire_at;

    PPPoEPacket packet;
    int len;

    struct PacketCriteria pc;
    pc.conn          = conn;
#ifdef PLUGIN
    conn->seenMaxPayload = 0;
#endif

    if (gettimeofday(&expire_at, NULL) < 0) {
	fatalSys("gettimeofday (waitForPADO)");
    }
    expire_at.tv_sec += timeout;

    do {
        if(!time_left(&tv, &expire_at)) {
            /* Timed out */
            return;
        }

        FD_ZERO(&readable);
        FD_SET(conn->discoverySocket, &readable);

        while(1) {
            r = select(conn->discoverySocket+1, &readable, NULL, NULL, &tv);
            if (r >= 0 || errno != EINTR) break;
        }
        if (r < 0) {
            fatalSys("select (waitForPADO)");
        }
        if (r == 0) {
            /* Timed out */
            return;
        }

	/* Get the packet */
	receivePacket(conn->discoverySocket, &packet, &len);

	/* Check length */
	if (ntohs(packet.length) + HDR_SIZE > len) {
	    syslog(LOG_ERR, "Bogus PPPoE length field (%u)",
		   (unsigned int) ntohs(packet.length));
	    continue;
	}

#ifdef DEBUGGING_ENABLED
	if (conn->debugFile) {
	    dumpPacket(conn->debugFile, &packet, "RCVD");
	    fprintf(conn->debugFile, "\n");
	    fflush(conn->debugFile);
	}
#endif
	/* If it's not for us, loop again */
	if (!packetIsForMe(conn, &packet)) continue;

	if (packet.code == CODE_PADO) {
	    if (BROADCAST(packet.ethHdr.h_source)) {
		printErr("Ignoring PADO packet from broadcast MAC address");
		continue;
	    }
#ifdef PLUGIN
	    if (conn->req_peer
		&& memcmp(packet.ethHdr.h_source, conn->req_peer_mac, ETH_ALEN) != 0) {
		warn("Ignoring PADO packet from wrong MAC address");
		continue;
	    }
#endif
	    pc.gotError = 0;
	    pc.seenACName    = 0;
	    pc.seenServiceName = 0;
	    pc.acNameOK      = (conn->acName)      ? 0 : 1;
	    pc.serviceNameOK = (conn->serviceName) ? 0 : 1;

            if (conn->printACNames && (conn->numPADOs > 0)) {
                printf("\n");
            }
	    parsePacket(&packet, parsePADOTags, &pc);
	    if (pc.gotError) {
		printErr("Error in PADO packet");
		continue;
	    }

	    if (!pc.seenACName) {
		printErr("Ignoring PADO packet with no AC-Name tag");
		continue;
	    }
	    if (!pc.seenServiceName) {
		printErr("Ignoring PADO packet with no Service-Name tag");
		continue;
	    }
	    conn->numPADOs++;
	    if (pc.acNameOK && pc.serviceNameOK) {
		memcpy(conn->peerEth, packet.ethHdr.h_source, ETH_ALEN);
		if (conn->printACNames) {
		    printf("AC-Ethernet-Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
			   (unsigned) conn->peerEth[0],
			   (unsigned) conn->peerEth[1],
			   (unsigned) conn->peerEth[2],
			   (unsigned) conn->peerEth[3],
			   (unsigned) conn->peerEth[4],
			   (unsigned) conn->peerEth[5]);
		    continue;
		}
		conn->discoveryState = STATE_RECEIVED_PADO;
		break;
	    }
	}
    } while (conn->discoveryState != STATE_RECEIVED_PADO);
}

/***********************************************************************
*%FUNCTION: sendPADR
*%ARGUMENTS:
* conn -- PPPoE connection structur
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Sends a PADR packet
***********************************************************************/
static void
sendPADR(PPPoEConnection *conn)
{
    PPPoEPacket packet;
    PPPoETag *svc = (PPPoETag *) packet.payload;
    unsigned char *cursor = packet.payload;

    uint16_t namelen = 0;
    uint16_t plen;

    if (conn->serviceName) {
	namelen = (uint16_t) strlen(conn->serviceName);
    }
    plen = TAG_HDR_SIZE + namelen;
    CHECK_ROOM(cursor, packet.payload, plen);

    memcpy(packet.ethHdr.h_dest, conn->peerEth, ETH_ALEN);
    memcpy(packet.ethHdr.h_source, conn->myEth, ETH_ALEN);

    packet.ethHdr.h_proto = htons(Eth_PPPOE_Discovery);
    packet.vertype = PPPOE_VER_TYPE(1, 1);
    packet.code = CODE_PADR;
    packet.session = 0;

    svc->type = TAG_SERVICE_NAME;
    svc->length = htons(namelen);
    if (conn->serviceName) {
	memcpy(svc->payload, conn->serviceName, namelen);
    }
    cursor += namelen + TAG_HDR_SIZE;

    /* If we're using Host-Uniq, copy it over */
    if (conn->hostUniq) {
	PPPoETag hostUniq;
	int len = (int) strlen(conn->hostUniq);
	hostUniq.type = htons(TAG_HOST_UNIQ);
	hostUniq.length = htons(len);
	CHECK_ROOM(cursor, packet.payload, len + TAG_HDR_SIZE);
	memcpy(hostUniq.payload, conn->hostUniq, len);
	memcpy(cursor, &hostUniq, len + TAG_HDR_SIZE);
	cursor += len + TAG_HDR_SIZE;
	plen += len + TAG_HDR_SIZE;
    }

    /* Copy cookie and relay-ID if needed */
    if (conn->cookie.type) {
	CHECK_ROOM(cursor, packet.payload,
		   ntohs(conn->cookie.length) + TAG_HDR_SIZE);
	memcpy(cursor, &conn->cookie, ntohs(conn->cookie.length) + TAG_HDR_SIZE);
	cursor += ntohs(conn->cookie.length) + TAG_HDR_SIZE;
	plen += ntohs(conn->cookie.length) + TAG_HDR_SIZE;
    }

    if (conn->relayId.type) {
	CHECK_ROOM(cursor, packet.payload,
		   ntohs(conn->relayId.length) + TAG_HDR_SIZE);
	memcpy(cursor, &conn->relayId, ntohs(conn->relayId.length) + TAG_HDR_SIZE);
	cursor += ntohs(conn->relayId.length) + TAG_HDR_SIZE;
	plen += ntohs(conn->relayId.length) + TAG_HDR_SIZE;
    }

#ifdef PLUGIN
    /* Add our maximum MTU/MRU */
    if (MIN(lcp_allowoptions[0].mru, lcp_wantoptions[0].mru) > ETH_PPPOE_MTU) {
	PPPoETag maxPayload;
	uint16_t mru = htons(MIN(lcp_allowoptions[0].mru, lcp_wantoptions[0].mru));
	maxPayload.type = htons(TAG_PPP_MAX_PAYLOAD);
	maxPayload.length = htons(sizeof(mru));
	CHECK_ROOM(cursor, packet.payload, sizeof(mru) + TAG_HDR_SIZE);
	memcpy(maxPayload.payload, &mru, sizeof(mru));
	memcpy(cursor, &maxPayload, sizeof(mru) + TAG_HDR_SIZE);
	cursor += sizeof(mru) + TAG_HDR_SIZE;
	plen += sizeof(mru) + TAG_HDR_SIZE;
    }
#endif

    packet.length = htons(plen);
    sendPacket(conn, conn->discoverySocket, &packet, (int) (plen + HDR_SIZE));
#ifdef DEBUGGING_ENABLED
    if (conn->debugFile) {
	dumpPacket(conn->debugFile, &packet, "SENT");
	fprintf(conn->debugFile, "\n");
	fflush(conn->debugFile);
    }
#endif
}

/**********************************************************************
*%FUNCTION: waitForPADS
*%ARGUMENTS:
* conn -- PPPoE connection info
* timeout -- how long to wait (in seconds)
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Waits for a PADS packet and copies useful information
***********************************************************************/
static void
waitForPADS(PPPoEConnection *conn, int timeout)
{
    fd_set readable;
    int r;
    struct timeval tv;
    struct timeval expire_at;

    PPPoEPacket packet;
    int len;

    if (gettimeofday(&expire_at, NULL) < 0) {
	fatalSys("gettimeofday (waitForPADS)");
    }
    expire_at.tv_sec += timeout;

    do {
        if(!time_left(&tv, &expire_at)) {
            /* Timed out */
            return;
        }

        FD_ZERO(&readable);
        FD_SET(conn->discoverySocket, &readable);

        while(1) {
            r = select(conn->discoverySocket+1, &readable, NULL, NULL, &tv);
            if (r >= 0 || errno != EINTR) break;
        }
        if (r < 0) {
            fatalSys("select (waitForPADS)");
        }
        if (r == 0) {
            /* Timed out */
            return;
        }

	/* Get the packet */
	receivePacket(conn->discoverySocket, &packet, &len);

	/* Check length */
	if (ntohs(packet.length) + HDR_SIZE > len) {
	    syslog(LOG_ERR, "Bogus PPPoE length field (%u)",
		   (unsigned int) ntohs(packet.length));
	    continue;
	}

#ifdef DEBUGGING_ENABLED
	if (conn->debugFile) {
	    dumpPacket(conn->debugFile, &packet, "RCVD");
	    fprintf(conn->debugFile, "\n");
	    fflush(conn->debugFile);
	}
#endif
	/* If it's not from the AC, it's not for me */
	if (memcmp(packet.ethHdr.h_source, conn->peerEth, ETH_ALEN)) continue;

	/* If it's not for us, loop again */
	if (!packetIsForMe(conn, &packet)) continue;

	/* Is it PADS?  */
	if (packet.code == CODE_PADS) {
	    /* Parse for goodies */
	    conn->PADSHadError = 0;
	    parsePacket(&packet, parsePADSTags, conn);
	    if (!conn->PADSHadError) {
		conn->discoveryState = STATE_SESSION;
		break;
	    }
	}
    } while (conn->discoveryState != STATE_SESSION);

    /* Don't bother with ntohs; we'll just end up converting it back... */
    conn->session = packet.session;

    syslog(LOG_INFO, "PPP session is %d (0x%x)", (int) ntohs(conn->session),
	   (unsigned int) ntohs(conn->session));

    /* RFC 2516 says session id MUST NOT be zero or 0xFFFF */
    if (ntohs(conn->session) == 0 || ntohs(conn->session) == 0xFFFF) {
	syslog(LOG_ERR, "Access concentrator used a session value of %x -- the AC is violating RFC 2516", (unsigned int) ntohs(conn->session));
    }
}

/**********************************************************************
*%FUNCTION: discovery
*%ARGUMENTS:
* conn -- PPPoE connection info structure
*%RETURNS:
* Nothing
*%DESCRIPTION:
* Performs the PPPoE discovery phase
***********************************************************************/
void
discovery(PPPoEConnection *conn)
{
    int padiAttempts;
    int padrAttempts;
    int timeout = conn->discoveryTimeout;

    /* Skip discovery? */
    if (conn->skipDiscovery) {
	conn->discoveryState = STATE_SESSION;
	if (conn->killSession) {
	    sendPADT(conn, "RP-PPPoE: Session killed manually");
	    exit(EXIT_SUCCESS);
	}
	return;
    }

  SEND_PADI:
    padiAttempts = 0;
    do {
	padiAttempts++;
	if (padiAttempts > MAX_PADI_ATTEMPTS) {
	    printErr("Timeout waiting for PADO packets");
	    if (persist) {
		padiAttempts = 0;
		timeout = conn->discoveryTimeout;
	    } else {
                break;
	    }
	}
	sendPADI(conn);
	conn->discoveryState = STATE_SENT_PADI;
	waitForPADO(conn, timeout);

	/* If we're just probing for access concentrators, don't do
	   exponential backoff.  This reduces the time for an unsuccessful
	   probe to 15 seconds. */
	if (!conn->printACNames) {
	    timeout *= 2;
	}
	if (conn->printACNames && conn->numPADOs) {
	    break;
	}
    } while (conn->discoveryState == STATE_SENT_PADI);

    /* If we're only printing access concentrator names, we're done */
    if (conn->printACNames) {
        if (conn->numPADOs) {
            exit(EXIT_SUCCESS);
        } else {
            exit(EXIT_FAILURE);
        }
    }

    timeout = conn->discoveryTimeout;
    padrAttempts = 0;
    do {
	padrAttempts++;
	if (padrAttempts > MAX_PADI_ATTEMPTS) {
	    printErr("Timeout waiting for PADS packets");
	    if (persist) {
		padrAttempts = 0;
		timeout = conn->discoveryTimeout;
		/* Go back to sending PADI again */
		goto SEND_PADI;
	    } else {
		return;
	    }
	}
	sendPADR(conn);
	conn->discoveryState = STATE_SENT_PADR;
	waitForPADS(conn, timeout);
	timeout *= 2;
    } while (conn->discoveryState == STATE_SENT_PADR);

#ifdef PLUGIN
    if (!conn->seenMaxPayload) {
	/* RFC 4638: MUST limit MTU/MRU to 1492 */
	if (lcp_allowoptions[0].mru > ETH_PPPOE_MTU) lcp_allowoptions[0].mru = ETH_PPPOE_MTU;
	if (lcp_wantoptions[0].mru > ETH_PPPOE_MTU)  lcp_wantoptions[0].mru = ETH_PPPOE_MTU;
    }
#endif
    /* We're done. */
    conn->discoveryState = STATE_SESSION;
    return;
}
