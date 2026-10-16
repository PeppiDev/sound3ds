#ifndef DEBUGLOG_H
#define DEBUGLOG_H

// Registro de eventos en sdmc:/3ds/sound3ds/debug.log para diagnosticar en la
// consola lo que el emulador no reproduce (reposo, menú HOME). Cada línea abre,
// escribe y cierra el archivo para que sobreviva a un cuelgue.

void dlog_init(void);
void dlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif // DEBUGLOG_H
