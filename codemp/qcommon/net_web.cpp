/*
===========================================================================
Copyright (C) 2026, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// Web replacement for net_ip.cpp. Browsers can't send UDP, so every datagram
// goes through one WebSocket to a relay (tools/web/server.js), which sends it
// on as UDP and passes replies back.
//
// Relay protocol, one WebSocket binary message each:
//   client -> relay  0x00 ip[4] port[2, big endian] payload   send a datagram
//   client -> relay  0x01 ip[4] hostname                      ip stands for hostname
//   relay -> client  0x00 ip[4] port[2, big endian] payload   received datagram
//   relay -> client  0x02 text                                message for the console
//
// Browsers can't resolve host names either: a host name gets a placeholder
// address from 198.18.0.0/15 (reserved for benchmarking, never routed), and
// the relay is told which name it stands for.

#include "qcommon/qcommon.h"

#include <emscripten.h>
#include <sys/select.h>

static cvar_t	*net_enabled;
static cvar_t	*net_forcenonlocal;
static cvar_t	*net_relay;
static cvar_t	*net_dropsim;

static qboolean networkingEnabled = qfalse;

#define MAX_HOSTNAMES	1024
static char		*hostNames[MAX_HOSTNAMES];
static int		numHostNames;

#define PLACEHOLDER_NET0	198
#define PLACEHOLDER_NET1	18

//=============================================================================

EM_JS( void, NetWeb_Open, ( const char *url ), {
	var net = Module.ojkNet || (Module.ojkNet = { names: {}, queue: [], pending: [] });
	var relay = UTF8ToString(url);
	if (!relay) {
		var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
		relay = proto + '//' + location.host + '/relay';
	}
	net.url = relay;
	net.connect = function () {
		if (net.socket && net.socket.readyState <= 1)
			return;
		var socket = new WebSocket(net.url);
		socket.binaryType = 'arraybuffer';
		socket.onopen = function () {
			// tell a new relay connection about all host names first
			for (var ip in net.names)
				socket.send(net.names[ip]);
			for (var i = 0; i < net.pending.length; i++)
				socket.send(net.pending[i]);
			net.pending = [];
		};
		socket.onmessage = function (event) {
			var data = new Uint8Array(event.data);
			if (data.length >= 7 && data[0] === 0) {
				net.queue.push(data);
			} else if (data.length >= 1 && data[0] === 2) {
				out('relay: ' + new TextDecoder().decode(data.subarray(1)));
			}
		};
		socket.onclose = function () {
			if (net.socket === socket)
				net.socket = null;
		};
		socket.onerror = function () {
			err('relay: can\'t reach ' + net.url);
		};
		net.socket = socket;
	};
	net.send = function (message) {
		net.connect();
		if (net.socket.readyState === 1)
			net.socket.send(message);
		else if (net.pending.length < 256)
			net.pending.push(message);
	};
	out('Using network relay ' + net.url);
} );

EM_JS( void, NetWeb_Close, ( void ), {
	var net = Module.ojkNet;
	if (!net)
		return;
	if (net.socket)
		net.socket.close();
	net.socket = null;
	net.queue = [];
	net.pending = [];
} );

EM_JS( void, NetWeb_Send, ( const uint8_t *ip, int port, const void *data, int length ), {
	var message = new Uint8Array(7 + length);
	message[0] = 0;
	message.set(HEAPU8.subarray(ip, ip + 4), 1);
	message[5] = (port >> 8) & 0xff;
	message[6] = port & 0xff;
	message.set(HEAPU8.subarray(data, data + length), 7);
	Module.ojkNet.send(message);
} );

EM_JS( void, NetWeb_NameAddress, ( const uint8_t *ip, const char *hostName ), {
	var name = new TextEncoder().encode(UTF8ToString(hostName));
	var message = new Uint8Array(5 + name.length);
	message[0] = 1;
	message.set(HEAPU8.subarray(ip, ip + 4), 1);
	message.set(name, 5);
	var net = Module.ojkNet;
	net.names[HEAPU8.subarray(ip, ip + 4).join('.')] = message;
	net.send(message);
} );

// Returns the payload length, or -1 when there is nothing to read.
EM_JS( int, NetWeb_Receive, ( uint8_t *ip, int *port, void *buffer, int maxLength ), {
	var net = Module.ojkNet;
	if (!net || !net.queue.length)
		return -1;
	var data = net.queue.shift();
	HEAPU8.set(data.subarray(1, 5), ip);
	HEAP32[port >> 2] = (data[5] << 8) | data[6];
	var length = data.length - 7;
	HEAPU8.set(data.subarray(7, 7 + Math.min(length, maxLength)), buffer);
	return length;
} );

//=============================================================================

char *NET_ErrorString( void ) {
	return (char *)"no error";
}

/*
=============
Sys_StringToAdr
=============
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a ) {
	unsigned int b[4];
	char end;

	memset( a, 0, sizeof( *a ) );
	a->type = NA_IP;

	if ( sscanf( s, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &end ) == 4
		&& b[0] < 256 && b[1] < 256 && b[2] < 256 && b[3] < 256 )
	{
		for ( int i = 0; i < 4; i++ )
			a->ip[i] = (byte)b[i];
		return qtrue;
	}

	if ( !s[0] || strlen( s ) > 253 )
		return qfalse;

	// host name: hand out a placeholder address and let the relay resolve it
	int index;
	for ( index = 0; index < numHostNames; index++ )
	{
		if ( !Q_stricmp( hostNames[index], s ) )
			break;
	}

	if ( index == numHostNames )
	{
		if ( numHostNames == MAX_HOSTNAMES )
		{
			Com_Printf( "Sys_StringToAdr: too many host names\n" );
			return qfalse;
		}
		hostNames[numHostNames++] = CopyString( s );
	}

	a->ip[0] = PLACEHOLDER_NET0;
	a->ip[1] = PLACEHOLDER_NET1 + ( ( index + 1 ) >> 16 );
	a->ip[2] = ( ( index + 1 ) >> 8 ) & 0xff;
	a->ip[3] = ( index + 1 ) & 0xff;

	if ( networkingEnabled )
		NetWeb_NameAddress( a->ip, s );

	return qtrue;
}

/*
==================
NET_GetPacket

Receive one packet
==================
*/
qboolean NET_GetPacket( netadr_t *net_from, msg_t *net_message, fd_set *fdr ) {
	if ( !networkingEnabled )
		return qfalse;

	int port;
	int length = NetWeb_Receive( net_from->ip, &port, net_message->data, net_message->maxsize );
	if ( length < 0 )
		return qfalse;

	net_from->type = NA_IP;
	net_from->port = BigShort( (short)port );

	if ( length >= net_message->maxsize ) {
		Com_Printf( "Oversize packet from %s\n", NET_AdrToString( net_from ) );
		return qfalse;
	}

	net_message->readcount = 0;
	net_message->cursize = length;
	return qtrue;
}

/*
==================
Sys_SendPacket
==================
*/
void Sys_SendPacket( int length, const void *data, const netadr_t *to ) {
	if ( !networkingEnabled )
		return;

	if ( to->type != NA_IP ) {
		// no broadcasts: the relay would have to send them on its own network
		if ( to->type != NA_BROADCAST )
			Com_Error( ERR_FATAL, "Sys_SendPacket: bad address type" );
		return;
	}

	NetWeb_Send( to->ip, (unsigned short)BigShort( to->port ), data, length );
}

/*
==================
Sys_IsLANAddress

LAN clients will have their rate var ignored
==================
*/
qboolean Sys_IsLANAddress( const netadr_t *adr ) {
	if ( !net_forcenonlocal )
		net_forcenonlocal = Cvar_Get( "net_forcenonlocal", "0", 0 );

	if ( net_forcenonlocal && net_forcenonlocal->integer )
		return qfalse;

	// a browser only runs a local (loopback) server; everything else is
	// reached through the relay
	return (qboolean)( adr->type == NA_LOOPBACK );
}

/*
==================
Sys_ShowIP
==================
*/
void Sys_ShowIP( void ) {
	Com_Printf( "No local IP in a browser; packets go through the relay %s\n",
		net_relay && net_relay->string[0] ? net_relay->string : "of this page's host" );
}

//=============================================================================

static qboolean NET_GetCvars( void ) {
	int modified;

	net_enabled = Cvar_Get( "net_enabled", "1", CVAR_LATCH | CVAR_ARCHIVE_ND );
	modified = net_enabled->modified;
	net_enabled->modified = qfalse;

	net_forcenonlocal = Cvar_Get( "net_forcenonlocal", "0", CVAR_LATCH | CVAR_ARCHIVE_ND );
	modified += net_forcenonlocal->modified;
	net_forcenonlocal->modified = qfalse;

	net_relay = Cvar_Get( "net_relay", "", CVAR_LATCH | CVAR_ARCHIVE_ND,
		"WebSocket URL of the UDP relay, e.g. wss://example.com/relay (default: /relay on the page's host)" );
	modified += net_relay->modified;
	net_relay->modified = qfalse;

	net_dropsim = Cvar_Get( "net_dropsim", "", CVAR_TEMP );

	return modified ? qtrue : qfalse;
}

/*
====================
NET_Config
====================
*/
void NET_Config( qboolean enableNetworking ) {
	qboolean modified = NET_GetCvars();

	if ( !net_enabled->integer )
		enableNetworking = qfalse;

	if ( enableNetworking == networkingEnabled && !modified )
		return;

	if ( networkingEnabled )
		NetWeb_Close();

	networkingEnabled = enableNetworking;

	if ( networkingEnabled ) {
		// the connection is made on the first packet sent
		NetWeb_Open( net_relay->string );
		for ( int i = 0; i < numHostNames; i++ ) {
			netadr_t a;
			Sys_StringToAdr( hostNames[i], &a );
		}
	}
}

/*
====================
NET_Init
====================
*/
void NET_Init( void ) {
	NET_Config( qtrue );

	Cmd_AddCommand( "net_restart", NET_Restart_f, "Restart the networking sub-system" );
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown( void ) {
	if ( !networkingEnabled ) {
		return;
	}

	NET_Config( qfalse );
}

/*
====================
NET_Event

Called from NET_Sleep to handle the packets that have arrived.
====================
*/
static void NET_Event( void )
{
	byte bufData[MAX_MSGLEN + 1];
	netadr_t from;
	msg_t netmsg;

	while ( 1 )
	{
		MSG_Init( &netmsg, bufData, sizeof( bufData ) );

		if ( !NET_GetPacket( &from, &netmsg, NULL ) )
			break;

		if ( net_dropsim->value > 0.0f && net_dropsim->value <= 100.0f )
		{
			// com_dropsim->value percent of incoming packets get dropped.
			if ( rand() < (int)( ( (double)RAND_MAX ) / 100.0 * (double)net_dropsim->value ) )
				continue;          // drop this packet
		}

		if ( com_sv_running->integer )
			Com_RunAndTimeServerPacket( &from, &netmsg );
		else
			CL_PacketEvent( &from, &netmsg );
	}
}

/*
====================
NET_Sleep

Handles the packets that have arrived. The browser runs the frame loop, so
this never waits.
====================
*/
void NET_Sleep( int msec ) {
	NET_Event();
}

/*
====================
NET_Restart_f
====================
*/
void NET_Restart_f( void ) {
	NET_Config( qtrue );
}
