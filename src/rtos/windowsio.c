#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "FreeRTOS.h"
#include "queue.h"

#define PRINT_QUEUE_LENGTH 2
#define PRINT_BUFFER_SIZE 2048

typedef struct {
	char buffer[PRINT_BUFFER_SIZE];
} PrintMessage_t;

static queue_t xPrintQueue = NULL;

static HANDLE hPrintThread = NULL;

/*
 * Only used as a notification flag.
 * FreeRTOS threads set it.
 * The Windows thread clears it.
 */
static volatile LONG lPrintPending = 0;

/* ---------------------------------------------------------- */
/* Native Windows printer thread                              */
/* ---------------------------------------------------------- */

static DWORD WINAPI prvPrintThread(void* pvParameters) {
	PrintMessage_t message;
	HANDLE stdoutHandle;

	(void)pvParameters;

	stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	for (;;) {
		/*
		 * Wait until a FreeRTOS thread tells us there
		 * may be something in the queue.
		 */
		while (InterlockedCompareExchange(&lPrintPending, 0, 0) == 0) {
			Sleep(1);
		}

		/*
		 * Drain everything currently queued.
		 */
		while (queue_recv(xPrintQueue, &message, 0) == pdTRUE) {
			DWORD written;

			WriteFile(stdoutHandle, message.buffer, (DWORD)strlen(message.buffer), &written, NULL);
		}

		/*
		 * Clear notification.
		 */
		InterlockedExchange(&lPrintPending, 0);

		/*
		 * Close the race where a producer queued something
		 * between our final queue_recv() and clearing the flag.
		 */
		if (queue_get_waiting(xPrintQueue) != 0) {
			InterlockedExchange(&lPrintPending, 1);
		}
	}

	return 0;
}

/* ---------------------------------------------------------- */
/* Initialization                                             */
/* ---------------------------------------------------------- */

BaseType_t xPortWindowsPrintInit(void) {
	if (xPrintQueue != NULL) {
		return pdPASS;
	}

	xPrintQueue = queue_create(PRINT_QUEUE_LENGTH, sizeof(PrintMessage_t));

	if (xPrintQueue == NULL) {
		return pdFAIL;
	}

	hPrintThread = CreateThread(NULL, 0, prvPrintThread, NULL, 0, NULL);

	if (hPrintThread == NULL) {
		queue_delete(xPrintQueue);
		xPrintQueue = NULL;

		return pdFAIL;
	}

	return pdPASS;
}

/* ---------------------------------------------------------- */
/* Common printf implementation                               */
/* ---------------------------------------------------------- */

static int prvWindowsPrint(const char* format, va_list args) {
	PrintMessage_t message;
	int result;

	if (xPrintQueue == NULL) {
		// print without queue, this means we haven't stated scheduler yet so io operations are fine
		return vprintf(format, args);
	}

	result = vsnprintf(message.buffer, sizeof(message.buffer), format, args);

	if (result < 0) {
		return -1;
	}

	/*
	 * Never block the FreeRTOS task waiting for the
	 * printer. If full, drop the message.
	 */
	if (queue_append(xPrintQueue, &message, 0) != pdTRUE) {
		return -1;
	}

	/*
	 * This is NOT a Windows API.
	 *
	 * It is just an atomic Windows compiler/CPU primitive
	 * used as a cross-thread flag.
	 */
	InterlockedExchange(&lPrintPending, 1);

	return result;
}

/* ---------------------------------------------------------- */
/* printf / fprintf                                           */
/* ---------------------------------------------------------- */

int printf(const char* format, ...) {
	va_list args;
	int result;

	va_start(args, format);
	result = prvWindowsPrint(format, args);
	va_end(args);

	return result;
}

int fprintf(FILE* stream, const char* format, ...) {
	va_list args;
	int result;

	(void)stream;

	va_start(args, format);
	result = prvWindowsPrint(format, args);
	va_end(args);

	return result;
}