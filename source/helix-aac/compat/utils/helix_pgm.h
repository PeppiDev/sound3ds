// Sound3DS: en la 3DS las tablas constantes se leen directo de memoria.
#pragma once
#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
