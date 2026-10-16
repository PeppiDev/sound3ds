// Sound3DS: el decodificador reserva su estado con malloc/free estándar.
#pragma once
#include <stdlib.h>
#define helix_malloc(size) malloc(size)
#define helix_free(ptr) free(ptr)
