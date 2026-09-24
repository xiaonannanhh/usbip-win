#ifndef USBRELAY_STANDARD_PROTOCOL_H
#define USBRELAY_STANDARD_PROTOCOL_H

/*
 * USBRelay Standard TCP/IP Server protocol.
 *
 * This protocol is intentionally independent from the older USBRELAY:
 * protocol.  Each local Windows print queue is assigned one RAW TCP port.
 * A client creates a Windows Standard TCP/IP Port and sends RAW bytes to:
 *
 *     <server-computer-name>:<assigned-port>
 */
#define USBRELAY_STANDARD_DISCOVERY_PORT 3251
#define USBRELAY_STANDARD_DISCOVERY_MAGIC "USBRELAY-STANDARD/1"
#define USBRELAY_STANDARD_QUERY_MAGIC "USBRELAY-STANDARD-QUERY/1"

#define USBRELAY_STANDARD_PORT_BASE 9100
#define USBRELAY_STANDARD_PORT_LAST 9199
#define USBRELAY_STANDARD_PORT_COUNT \
	(USBRELAY_STANDARD_PORT_LAST - USBRELAY_STANDARD_PORT_BASE + 1)

#define USBRELAY_STANDARD_REGISTRY_PATH \
	L"SOFTWARE\\USBRelay\\StandardServer"
#define USBRELAY_STANDARD_PORTS_REGISTRY_PATH \
	L"SOFTWARE\\USBRelay\\StandardServer\\Ports"
#define USBRELAY_STANDARD_CONFIG_MUTEX_NAME \
	L"Local\\USBRelay-Standard-Server-Config"
#define USBRELAY_STANDARD_CORE_MUTEX_NAME \
	L"Local\\USBRelay-Standard-Server-Core"
#define USBRELAY_STANDARD_STOP_EVENT_NAME \
	L"Local\\USBRelay-Standard-Server-Stop"
#define USBRELAY_STANDARD_UI_MUTEX_NAME \
	L"Local\\USBRelay-Standard-Server-UI"
#define USBRELAY_STANDARD_LOG_MUTEX_NAME \
	L"Local\\USBRelay-Standard-Server-TaskLog"

#define USBRELAY_STANDARD_TASK_NAME \
	L"USBRelay-Standard-Server-Autostart"
#define USBRELAY_STANDARD_SHORTCUT_NAME \
	L"打印机内网共享服务端"

/*
 * Discovery packets are UTF-8 text datagrams.  A packet starts with the
 * discovery magic followed by CRLF, then contains simple key=value lines.
 * Printer names, drivers and original ports are percent-encoded.  A server
 * sends one header packet and one packet per published printer.  Clients
 * should treat the UDP source address as authoritative when the optional
 * address field is 0.0.0.0.
 */
#define USBRELAY_STANDARD_DISCOVERY_PACKET_MAX 4096

#endif
