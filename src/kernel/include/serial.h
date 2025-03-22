#include "system.h"
#include "types.h"

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

// 4 -> COM1, 3 -> COM2
#define SERIAL_IRQ 4

#ifndef SERIAL_H
#define SERIAL_H

Spinlock LOCK_DEBUGF;

void serial_send(int device, char out);
char serial_recv_async(int device);
char serial_recv(int device);
int  serial_rcvd(int device);
void initiateSerial();
void debug(char c, void *arg);
int  debugf(const char *format, ...);

#endif