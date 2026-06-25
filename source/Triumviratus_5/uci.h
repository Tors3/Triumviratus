#pragma once
#ifndef UCI_H
#define UCI_H

#include "defs.h"

extern int parse_move(char* move_string);
extern void parse_position(char* command);
extern void reset_time_control();
extern void parse_go(char* command);
extern void uci_loop();

#endif
