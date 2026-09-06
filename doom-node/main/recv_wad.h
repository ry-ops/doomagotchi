#pragma once
#include <stdbool.h>

// One-time bring-up helper. Blocks receiving a file over the serial console
// and writing it to dest_path on the (already-mounted) SD card. Returns true
// on a verified transfer. See recv_wad.c for the wire protocol / host script.
bool recv_wad_over_serial(const char *dest_path);
