#ifndef NET_HTTP_H
#define NET_HTTP_H

// GET de JSON compartido entre los módulos de red (lo implementa network.c)

#include <stdbool.h>
#include <stddef.h>
#include <jansson.h>

// Descarga url (hasta max_size bytes) y la interpreta como JSON. NULL con el motivo en
// err; *http_code recibe el código HTTP (0 si no hubo respuesta).
json_t* net_get_json(const char* url, size_t max_size, volatile bool* cancel, long* http_code, char* err,
                     size_t err_sz);

#endif // NET_HTTP_H
