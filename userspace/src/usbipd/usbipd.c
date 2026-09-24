/*
 *
 * Copyright (C) 2005-2007 Takahiro Hirofuchi
 */

#include <ws2tcpip.h>
#include <iphlpapi.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <signal.h>

#include "usbipd.h"

#include "usbip_network.h"
#include "getopt.h"
#include "usbip_windows.h"

#undef  PROGNAME
#define PROGNAME "usbipd"

#define MAIN_LOOP_TIMEOUT 1
#define SERVICE_NAME "usbipd"
#define SERVICE_DISPLAY_NAME "USB/IP Device Server"
#define DISCOVERY_PORT 3241
#define DISCOVERY_MAGIC "USBIP-WIN7-DISCOVERY/1"
#define DISCOVERY_INTERVAL_MS 2000

extern SOCKET *get_listen_sockfds(int family);
extern void accept_request(SOCKET *sockfds, fd_set *pfds);

static const char usbip_version_string[] = PACKAGE_STRING;

static const char usbipd_help_string[] =
	"usage: usbipd [options]\n"
	"\n"
	"	-4, --ipv4\n"
	"		Bind to IPv4. Default is both.\n"
	"\n"
	"	-6, --ipv6\n"
	"		Bind to IPv6. Default is both.\n"
	"\n"
	"	-d, --debug\n"
	"		Print debugging information.\n"
	"\n"
	"	-tPORT, --tcp-port PORT\n"
	"		Listen on TCP/IP port PORT.\n"
	"\n"
	"	-s, --service\n"
	"		Run in Windows service mode.\n"
	"\n"
	"	    --install-service\n"
	"		Install the server as an automatic Windows service.\n"
	"\n"
	"	    --uninstall-service\n"
	"		Stop and remove the Windows service.\n"
	"\n"
	"	-h, --help\n"
	"		Print this help.\n"
	"\n"
	"	-v, --version\n"
	"		Show version.\n";

static enum {
	cmd_standalone_mode = 1,
	cmd_service_mode,
	cmd_install_service,
	cmd_uninstall_service,
	cmd_help,
	cmd_version
} cmd = cmd_standalone_mode;

static int	family = AF_UNSPEC;
static HANDLE	stop_event;
static SOCKET	*listen_sockfds;
static SOCKET	discovery_sockfd = INVALID_SOCKET;
static DWORD	discovery_last_send;
static volatile LONG	stop_requested;

static SERVICE_STATUS		service_status;
static SERVICE_STATUS_HANDLE	service_status_handle;

static void
usbipd_help(void)
{
	printf("%s\n", usbipd_help_string);
}

static void
request_stop(void)
{
	InterlockedExchange(&stop_requested, TRUE);
	if (stop_event != NULL)
		SetEvent(stop_event);
	usbipd_forwarders_request_stop();
	usbipd_print_relay_request_stop();
}

static void
signal_handler(int i)
{
	dbg("received '%d' signal", i);
	request_stop();
}

static void
set_signal(void)
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
}

static int
setup_fds(SOCKET *sockfds, fd_set *pfds)
{
	int	i;

	FD_ZERO(pfds);
	for (i = 0; sockfds[i] != INVALID_SOCKET; i++) {
		FD_SET(sockfds[i], pfds);
	}
	return i;
}

static void
close_listen_sockfds(void)
{
	int	i;

	if (listen_sockfds == NULL)
		return;

	for (i = 0; listen_sockfds[i] != INVALID_SOCKET; i++)
		closesocket(listen_sockfds[i]);
	free(listen_sockfds);
	listen_sockfds = NULL;
}

static void
close_discovery_sender(void)
{
	if (discovery_sockfd != INVALID_SOCKET) {
		closesocket(discovery_sockfd);
		discovery_sockfd = INVALID_SOCKET;
	}
}

static int
open_discovery_sender(void)
{
	BOOL enabled = TRUE;

	discovery_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (discovery_sockfd == INVALID_SOCKET) {
		err("failed to create discovery socket: err: %d", WSAGetLastError());
		return 1;
	}

	if (setsockopt(discovery_sockfd, SOL_SOCKET, SO_BROADCAST,
		(const char *)&enabled, sizeof(enabled)) == SOCKET_ERROR) {
		err("failed to enable discovery broadcast: err: %d", WSAGetLastError());
		close_discovery_sender();
		return 1;
	}
	return 0;
}

static void
send_discovery_packet_to(ULONG address, const char *packet, int packet_length)
{
	SOCKADDR_IN destination;

	ZeroMemory(&destination, sizeof(destination));
	destination.sin_family = AF_INET;
	destination.sin_port = htons(DISCOVERY_PORT);
	destination.sin_addr.s_addr = htonl(address);
	sendto(discovery_sockfd, packet, packet_length, 0,
		(const SOCKADDR *)&destination, sizeof(destination));
}

static void
broadcast_discovery(void)
{
	IP_ADAPTER_INFO *adapters;
	IP_ADAPTER_INFO *adapter;
	ULONG buffer_size = 0;
	ULONG result;
	char computer_name[256];
	char packet[512];
	wchar_t computer_name_wide[256];
	DWORD computer_name_length = (DWORD)(sizeof(computer_name_wide) /
		sizeof(computer_name_wide[0]));
	int converted_length;
	int packet_length;
	ULONG directed[16];
	unsigned int directed_count = 0;
	unsigned int i;

	if (discovery_sockfd == INVALID_SOCKET)
		return;

	if (!GetComputerNameW(computer_name_wide, &computer_name_length)) {
		wcsncpy_s(computer_name_wide,
			sizeof(computer_name_wide) / sizeof(computer_name_wide[0]),
			L"USB/IP Server", _TRUNCATE);
	}
	converted_length = WideCharToMultiByte(CP_UTF8, 0, computer_name_wide,
		-1, computer_name, sizeof(computer_name), NULL, NULL);
	if (converted_length <= 0) {
		strcpy_s(computer_name, sizeof(computer_name), "USB/IP Server");
	}

	packet_length = snprintf(packet, sizeof(packet),
		DISCOVERY_MAGIC "\r\n"
		"name=%s\r\n"
		"port=%s\r\n"
		"printers=%d\r\n"
		"print_port=%d\r\n",
		computer_name, usbip_port_string,
		usbipd_print_relay_running() ? 1 : 0, USBIPD_PRINT_PORT);
	if (packet_length <= 0 || packet_length >= (int)sizeof(packet))
		return;

	result = GetAdaptersInfo(NULL, &buffer_size);
	if (result == ERROR_BUFFER_OVERFLOW && buffer_size != 0) {
		adapters = (IP_ADAPTER_INFO *)HeapAlloc(GetProcessHeap(),
			HEAP_ZERO_MEMORY, buffer_size);
		if (adapters != NULL) {
			result = GetAdaptersInfo(adapters, &buffer_size);
			if (result == ERROR_SUCCESS) {
				for (adapter = adapters; adapter != NULL;
					adapter = adapter->Next) {
					IP_ADDR_STRING *address_entry =
						&adapter->IpAddressList;

					if (adapter->Type == MIB_IF_TYPE_LOOPBACK)
						continue;
					while (address_entry != NULL) {
						ULONG address = ntohl(inet_addr(
							address_entry->IpAddress.String));
						ULONG mask = ntohl(inet_addr(
							address_entry->IpMask.String));
						ULONG broadcast;

						address_entry = address_entry->Next;
						if (address == INADDR_NONE || mask == INADDR_NONE ||
							mask == 0)
							continue;
						broadcast = address | ~mask;
						for (i = 0; i < directed_count; i++) {
							if (directed[i] == broadcast)
								break;
						}
						if (i != directed_count)
							continue;
						if (directed_count <
							sizeof(directed) / sizeof(directed[0])) {
							directed[directed_count++] = broadcast;
							send_discovery_packet_to(broadcast,
								packet, packet_length);
						}
					}
				}
			}
			HeapFree(GetProcessHeap(), 0, adapters);
		}
	}

	send_discovery_packet_to(INADDR_BROADCAST, packet, packet_length);
}

static void
broadcast_discovery_tick(void)
{
	DWORD now = GetTickCount();

	if (discovery_last_send != 0 &&
		(DWORD)(now - discovery_last_send) < DISCOVERY_INTERVAL_MS)
		return;
	discovery_last_send = now;
	broadcast_discovery();
}

static int
run_server(void)
{
	fd_set	fds;
	int	n_sockfds;

	if (init_socket() != 0)
		return 1;

	stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (stop_event == NULL) {
		err("failed to create stop event");
		cleanup_socket();
		return 1;
	}
	if (!usbipd_forwarders_init()) {
		err("failed to initialize forwarders");
		CloseHandle(stop_event);
		stop_event = NULL;
		cleanup_socket();
		return 1;
	}
	if (!usbipd_print_relay_init())
		err("failed to initialize print relay on TCP %d", USBIPD_PRINT_PORT);

	set_signal();

	info("starting " PROGNAME " (%s)", usbip_version_string);

	listen_sockfds = get_listen_sockfds(family);
	if (listen_sockfds == NULL) {
		err("failed to open a listening socket");
		request_stop();
		usbipd_forwarders_cleanup();
		CloseHandle(stop_event);
		stop_event = NULL;
		cleanup_socket();
		return 1;
	}

	if (open_discovery_sender() != 0)
		err("discovery broadcast is disabled");
	else
		info("publishing device server discovery on UDP %d", DISCOVERY_PORT);

	n_sockfds = setup_fds(listen_sockfds, &fds);
	while (!stop_requested && !usbipd_is_stopping()) {
		struct timeval	timeout;
		int rc;

		timeout.tv_sec = MAIN_LOOP_TIMEOUT;
		timeout.tv_usec = 0;
		fds.fd_count = n_sockfds;
		rc = select(n_sockfds, &fds, NULL, NULL, &timeout);
		if (rc == SOCKET_ERROR) {
			if (!stop_requested)
				err("failed to select: err: %d", WSAGetLastError());
			break;
		}
		else if (rc > 0 && !stop_requested) {
			accept_request(listen_sockfds, &fds);
		}
		if (!stop_requested)
			broadcast_discovery_tick();
	}

	info("shutting down " PROGNAME);
	request_stop();
	close_discovery_sender();
	close_listen_sockfds();
	if (!usbipd_forwarders_wait(30000))
		err("timed out waiting for active forwarders");
	usbipd_forwarders_cleanup();
	if (!usbipd_print_relay_wait(10000))
		err("timed out waiting for print relay");
	usbipd_print_relay_cleanup();
	CloseHandle(stop_event);
	stop_event = NULL;
	cleanup_socket();

	return 0;
}

static void
report_service_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
	if (service_status_handle == NULL)
		return;

	service_status.dwCurrentState = state;
	service_status.dwWin32ExitCode = exit_code;
	service_status.dwWaitHint = wait_hint;
	service_status.dwControlsAccepted =
		(state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;

	if (state == SERVICE_START_PENDING)
		service_status.dwCheckPoint++;
	else
		service_status.dwCheckPoint = 0;

	SetServiceStatus(service_status_handle, &service_status);
}

static DWORD WINAPI
service_ctrl_handler(DWORD control, DWORD event_type, LPVOID event_data, LPVOID context)
{
	UNREFERENCED_PARAMETER(event_type);
	UNREFERENCED_PARAMETER(event_data);
	UNREFERENCED_PARAMETER(context);

	switch (control) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
		request_stop();
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

static void WINAPI
service_main(DWORD argc, LPSTR *argv)
{
	int rc;

	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	service_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl_handler, NULL);
	if (service_status_handle == NULL) {
		err("RegisterServiceCtrlHandlerEx failed: %lu", GetLastError());
		return;
	}

	memset(&service_status, 0, sizeof(service_status));
	service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	report_service_status(SERVICE_START_PENDING, NO_ERROR, 30000);
	report_service_status(SERVICE_RUNNING, NO_ERROR, 0);

	rc = run_server();

	service_status.dwServiceSpecificExitCode = rc;
	report_service_status(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR,
		rc == 0 ? 0 : 30000);
}

static int
run_service(void)
{
	SERVICE_TABLE_ENTRYA service_table[] = {
		{ (LPSTR)SERVICE_NAME, service_main },
		{ NULL, NULL }
	};

	if (!StartServiceCtrlDispatcherA(service_table)) {
		DWORD error = GetLastError();

		if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
			err("run usbipd --service only from the Windows service controller");
		else
			err("StartServiceCtrlDispatcher failed: %lu", error);
		return 1;
	}
	return 0;
}

static int
install_service(void)
{
	SC_HANDLE	manager;
	SC_HANDLE	service;
	char		path[MAX_PATH];
	char		command[MAX_PATH + 64];
	SERVICE_DESCRIPTIONA description;
	int		rc = 1;

	if (GetModuleFileNameA(NULL, path, sizeof(path)) == 0) {
		err("GetModuleFileName failed: %lu", GetLastError());
		return 1;
	}

	if (strcmp(usbip_port_string, "3240") == 0)
		snprintf(command, sizeof(command), "\"%s\" --service", path);
	else
		snprintf(command, sizeof(command), "\"%s\" --service --tcp-port %s",
			path, usbip_port_string);

	manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	if (manager == NULL) {
		err("OpenSCManager failed: %lu", GetLastError());
		return 1;
	}

	service = CreateServiceA(manager, SERVICE_NAME, SERVICE_DISPLAY_NAME,
		SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL, command, NULL, NULL, NULL, NULL, NULL);
	if (service == NULL) {
		DWORD error = GetLastError();

		if (error != ERROR_SERVICE_EXISTS) {
			err("CreateService failed: %lu", error);
			goto out_manager;
		}

		service = OpenServiceA(manager, SERVICE_NAME, SERVICE_ALL_ACCESS);
		if (service == NULL) {
			err("OpenService failed: %lu", GetLastError());
			goto out_manager;
		}
		if (!ChangeServiceConfigA(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
			SERVICE_NO_CHANGE, command, NULL, NULL, NULL, NULL, NULL,
			SERVICE_DISPLAY_NAME)) {
			err("ChangeServiceConfig failed: %lu", GetLastError());
			goto out_service;
		}
	}

	description.lpDescription = "Shares USB devices over the USB/IP protocol.";
	ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &description);

	if (!StartServiceA(service, 0, NULL)) {
		DWORD error = GetLastError();

		if (error == ERROR_SERVICE_ALREADY_RUNNING)
			info("service is already running");
		else
			err("service installed, but StartService failed: %lu", error);
	}
	else {
		info("service installed and started");
	}
	rc = 0;

out_service:
	CloseServiceHandle(service);
out_manager:
	CloseServiceHandle(manager);
	return rc;
}

static int
uninstall_service(void)
{
	SC_HANDLE	manager;
	SC_HANDLE	service;
	SERVICE_STATUS	status;
	int		rc = 0;

	manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		err("OpenSCManager failed: %lu", GetLastError());
		return 1;
	}

	service = OpenServiceA(manager, SERVICE_NAME,
		SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
	if (service == NULL) {
		err("OpenService failed: %lu", GetLastError());
		CloseServiceHandle(manager);
		return 1;
	}

	if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
		int	i;

		for (i = 0; i < 30; i++) {
			Sleep(1000);
			if (!QueryServiceStatus(service, &status) ||
				status.dwCurrentState == SERVICE_STOPPED)
				break;
		}
	}

	if (!DeleteService(service)) {
		err("DeleteService failed: %lu", GetLastError());
		rc = 1;
	}
	else {
		info("service stopped and removed");
	}

	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return rc;
}

static BOOL
parse_args(int argc, char *argv[])
{
	const struct option longopts[] = {
	{ "ipv4",              no_argument,       NULL, '4' },
	{ "ipv6",              no_argument,       NULL, '6' },
	{ "debug",             no_argument,       NULL, 'd' },
	{ "device",            no_argument,       NULL, 'e' },
	{ "pid",               optional_argument, NULL, 'P' },
	{ "tcp-port",          required_argument, NULL, 't' },
	{ "service",           no_argument,       NULL, 's' },
	{ "install-service",   no_argument,       NULL, 'I' },
	{ "uninstall-service", no_argument,       NULL, 'U' },
	{ "help",              no_argument,       NULL, 'h' },
	{ "version",           no_argument,       NULL, 'v' },
	{ NULL,                0,                 NULL,  0 }
	};
	BOOL	ipv4 = FALSE, ipv6 = FALSE;

	for (;;) {
		int	opt;

		opt = getopt_long(argc, argv, "46Ddst:IUhv", longopts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case '4':
			ipv4 = TRUE;
			break;
		case '6':
			ipv6 = TRUE;
			break;
		case 'd':
			usbip_use_debug = 1;
			break;
		case 's':
			cmd = cmd_service_mode;
			break;
		case 'I':
			cmd = cmd_install_service;
			break;
		case 'U':
			cmd = cmd_uninstall_service;
			break;
		case 'h':
			cmd = cmd_help;
			break;
		case 't':
			usbip_setup_port_number(optarg);
			break;
		case 'v':
			cmd = cmd_version;
			break;
		case '?':
			usbipd_help();
		default:
			return FALSE;
		}
	}

	/*
	 * To suppress warnings on systems with bindv6only disabled
	 * (default), we use separate sockets for IPv6 and IPv4 and set
	 * IPV6_V6ONLY on the IPv6 sockets.
	 */
	if (ipv4 && !ipv6)
		family = AF_INET;
	else if (!ipv4 && ipv6)
		family = AF_INET6;
	return TRUE;
}

int
main(int argc, char *argv[])
{
	usbip_use_stderr = 1;

	if (!parse_args(argc, argv))
		return EXIT_FAILURE;

	switch (cmd) {
	case cmd_standalone_mode:
		return run_server();
	case cmd_service_mode:
		return run_service();
	case cmd_install_service:
		return install_service();
	case cmd_uninstall_service:
		return uninstall_service();
	case cmd_version:
		printf(PROGNAME " (%s)\n", usbip_version_string);
		return EXIT_SUCCESS;
	case cmd_help:
		usbipd_help();
		return EXIT_SUCCESS;
	default:
		usbipd_help();
		return EXIT_FAILURE;
	}
}
