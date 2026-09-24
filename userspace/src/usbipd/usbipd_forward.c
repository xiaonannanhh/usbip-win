#include "usbipd.h"

#include "usbip_forward.h"

typedef struct _forwarder_ctx {
	struct _forwarder_ctx	*next;
	usbip_forward_cancel_t	cancel;
	HANDLE			hdev;
	SOCKET			sockfd;
} forwarder_ctx_t;

static CRITICAL_SECTION	forwarders_lock;
static HANDLE		forwarders_done_event;
static forwarder_ctx_t	*forwarders_head;
static LONG		forwarders_count;
static volatile LONG	forwarders_stopping;
static BOOL		forwarders_initialized;

static void
unlink_forwarder(forwarder_ctx_t *pctx)
{
	forwarder_ctx_t	**pp;

	pp = &forwarders_head;
	while (*pp != NULL) {
		if (*pp == pctx) {
			*pp = pctx->next;
			forwarders_count--;
			if (forwarders_count == 0)
				SetEvent(forwarders_done_event);
			break;
		}
		pp = &(*pp)->next;
	}
}

static DWORD WINAPI
forwarder_thread(LPVOID ctx)
{
	forwarder_ctx_t	*pctx = (forwarder_ctx_t *)ctx;

	dbg("stub forwarding started");
	usbip_forward((HANDLE)pctx->sockfd, pctx->hdev, TRUE, &pctx->cancel);

	closesocket(pctx->sockfd);
	CloseHandle(pctx->hdev);

	EnterCriticalSection(&forwarders_lock);
	unlink_forwarder(pctx);
	LeaveCriticalSection(&forwarders_lock);

	usbip_forward_cancel_cleanup(&pctx->cancel);
	free(pctx);

	dbg("stub forwarding stopped");
	return 0;
}

BOOL
usbipd_forwarders_init(void)
{
	forwarders_head = NULL;
	forwarders_count = 0;
	forwarders_stopping = FALSE;
	forwarders_initialized = FALSE;

	InitializeCriticalSection(&forwarders_lock);
	forwarders_done_event = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (forwarders_done_event == NULL) {
		DeleteCriticalSection(&forwarders_lock);
		return FALSE;
	}
	forwarders_initialized = TRUE;
	return TRUE;
}

void
usbipd_forwarders_cleanup(void)
{
	if (!forwarders_initialized)
		return;
	usbipd_forwarders_request_stop();
	if (!usbipd_forwarders_wait(5000)) {
		err("forwarders did not stop; preserving synchronization objects until process exit");
		return;
	}
	CloseHandle(forwarders_done_event);
	forwarders_done_event = NULL;
	DeleteCriticalSection(&forwarders_lock);
	forwarders_initialized = FALSE;
}

BOOL
usbipd_forwarder_start(HANDLE hdev, SOCKET sockfd)
{
	forwarder_ctx_t	*pctx;
	HANDLE		thread;

	if (!forwarders_initialized)
		return FALSE;

	pctx = (forwarder_ctx_t *)calloc(1, sizeof(forwarder_ctx_t));
	if (pctx == NULL) {
		err("forwarder: out of memory");
		return FALSE;
	}
	if (!usbip_forward_cancel_init(&pctx->cancel)) {
		err("forwarder: failed to create cancel event");
		free(pctx);
		return FALSE;
	}
	pctx->hdev = hdev;
	pctx->sockfd = sockfd;

	EnterCriticalSection(&forwarders_lock);
	if (forwarders_stopping) {
		LeaveCriticalSection(&forwarders_lock);
		usbip_forward_cancel_cleanup(&pctx->cancel);
		free(pctx);
		return FALSE;
	}
	pctx->next = forwarders_head;
	forwarders_head = pctx;
	forwarders_count++;
	ResetEvent(forwarders_done_event);
	LeaveCriticalSection(&forwarders_lock);

	thread = CreateThread(NULL, 0, forwarder_thread, pctx, CREATE_SUSPENDED, NULL);
	if (thread == NULL) {
		err("forwarder: CreateThread error: %lx", GetLastError());
		EnterCriticalSection(&forwarders_lock);
		unlink_forwarder(pctx);
		LeaveCriticalSection(&forwarders_lock);
		usbip_forward_cancel_cleanup(&pctx->cancel);
		free(pctx);
		return FALSE;
	}
	if (ResumeThread(thread) == (DWORD)-1) {
		err("forwarder: ResumeThread error: %lx", GetLastError());
		TerminateThread(thread, 1);
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
		EnterCriticalSection(&forwarders_lock);
		unlink_forwarder(pctx);
		LeaveCriticalSection(&forwarders_lock);
		usbip_forward_cancel_cleanup(&pctx->cancel);
		free(pctx);
		return FALSE;
	}
	CloseHandle(thread);
	return TRUE;
}

void
usbipd_forwarders_request_stop(void)
{
	forwarder_ctx_t	*pctx;

	InterlockedExchange(&forwarders_stopping, TRUE);
	if (!forwarders_initialized)
		return;

	EnterCriticalSection(&forwarders_lock);
	for (pctx = forwarders_head; pctx != NULL; pctx = pctx->next)
		usbip_forward_cancel_request(&pctx->cancel);
	LeaveCriticalSection(&forwarders_lock);
}

BOOL
usbipd_forwarders_wait(DWORD timeout_ms)
{
	if (!forwarders_initialized)
		return TRUE;

	EnterCriticalSection(&forwarders_lock);
	if (forwarders_count == 0) {
		LeaveCriticalSection(&forwarders_lock);
		return TRUE;
	}
	LeaveCriticalSection(&forwarders_lock);

	return WaitForSingleObject(forwarders_done_event, timeout_ms) == WAIT_OBJECT_0;
}

BOOL
usbipd_is_stopping(void)
{
	return forwarders_stopping != 0;
}
