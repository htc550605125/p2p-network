//
// Created by htc on 15-6-25.
//

#ifndef NETWORK_DATA_H
#define NETWORK_DATA_H

#include "bt_parse.h"
#include "peer.h"

void data_init();

int data_load_chunk(char* hash, char* dest);

int data_save_chunk(char* hash, char* data);

int data_request_chunks(int chunk_n, char* required, char* out);

#endif //NETWORK_DATA_H
