#ifndef I18N_H
#define I18N_H

// Idioma de la interfaz. Los textos del código están en español; L() devuelve la
// traducción al inglés cuando corresponde (búsqueda por contenido, así también
// sirve para textos ya copiados a buffers). Las cadenas de formato se traducen
// antes de formatear: snprintf(buf, n, L("%d canciones"), n).

typedef enum { LANG_ES = 0, LANG_EN = 1 } Lang;

void i18n_set(Lang lang);
Lang i18n_get(void);
const char* L(const char* es);

#endif // I18N_H
