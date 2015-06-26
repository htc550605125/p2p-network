//
// Created by htc on 15-6-26.
//

#ifndef NETWORK_TIMEOUT_H
#define NETWORK_TIMEOUT_H

#include <sys/time.h>
#include <stdint.h>

void timeout_init();

int timeout_register(struct timeval* timeout, void (*handler)(void* data));

int timeout_cancel(int timeout_id);

int timeout_dispatch();

#endif //NETWORK_TIMEOUT_H
