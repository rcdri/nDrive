/* nDrive - OS "Document Received" notification. Copyright (C) 2026 Rodrigo. GPL v3+. */
#ifndef NDRIVE_NOTIFY_H
#define NDRIVE_NOTIFY_H

#include <stdbool.h>

/* Locate the OS routine (any context; scans OS code). */
bool notify_init(void);
/* Show the TI link "Document Received" dialog for a calculator path such as
   "/documents/Examples/x.tns". Runs the OS dialog (modal, OS event task). */
bool notify_received(const char *path);

/* The OS routine that builds the archive of its log files; NULL if not found.
   Call with a 64-byte buffer holding "/documents/NspireLogs.zip": the buffer
   then holds the path of the archive. */
typedef void (*NotifyLogsFn)(char *path);
NotifyLogsFn notify_logs_fn(void);

#endif
