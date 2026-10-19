# Historial de cambios

## [Sin publicar]

### Añadido
- Búsqueda de playlists: junto a la barra de búsqueda de SoundCloud hay dos botones, **Canciones** y **Playlists**. Las playlists se buscan sin cuenta y solo aparecen las de usuarios (los álbumes oficiales casi siempre son avances de 30 s).
- Playlist abierta como un álbum: arriba se ven su carátula, nombre, creador, cantidad de canciones y duración; abajo sus canciones en orden. (A) reproduce desde la elegida y sigue con las siguientes; la flecha o (B) vuelve a los resultados. Se cargan hasta 500 canciones por playlist.
- **Descargar** la playlist completa: queda en `sdmc:/music/SoundCloud/<playlist>/` y aparece en Local como un álbum con el nombre, el creador y la carátula de la playlist, ordenado por su posición. Los avances de 30 s se saltan y al final se informa cuántas se bajaron.
- La cola de descargas se guarda en la SD tras cada canción: si se corta (salir, HOME, apagar la consola, sin Wi-Fi o sin espacio), al volver a abrir Sound3DS pregunta si reanudarla o descartarla. Sin conexión o sin espacio la cola queda en pausa en vez de perderse.
- Las descargas guardan el ID de SoundCloud en la etiqueta (`TXXX:SOUNDCLOUD_ID`). Dentro de un mismo álbum una canción solo se salta si es exactamente la misma pista; otra versión con el mismo nombre se guarda como "Nombre (2)".
- **Cuenta de SoundCloud**: se conecta con [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share) en el PC y Sound3DS QR en la consola, que la guarda en `sdmc:/3ds/sound3ds/account.json`. Sound3DS renueva la sesión solo (al abrir si tiene más de 20 h, mientras sigue abierto y tras un 401) y guarda el token nuevo sin dejar el archivo a medias. Si SoundCloud la rechaza, se borra y se pide volver a conectar; si solo falta red, se reintenta más tarde.
- Configuración tiene una fila "Cuenta de SoundCloud" con el usuario y **Desconectar** (borra la sesión de la SD tras confirmar), o cómo conectarla. Con una cuenta conectada su nombre aparece en la barra de arriba.
- Con la cuenta conectada, SoundCloud suma pestañas bajo la búsqueda: **Buscar**, **Likes**, **Playlists** (propias y guardadas, también las privadas) y **Feed** (canciones nuevas y reposts de quienes sigues, con la marca "repost"). Se cambian tocándolas o con ZL/ZR en New 3DS y New 2DS XL; tocar la pestaña activa o el título recarga la lista. Se cargan de a 50 y al llegar a la última página se piden más. Las canciones se reproducen y descargan como las de la búsqueda, y las playlists se abren y descargan como un álbum.
- **Recibir desde el PC** (Configuración): Sound3DS QR ahora vive dentro de Sound3DS. Abre la cámara exterior y lee el QR de [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share): las canciones del PC van a la cola de descargas (en segundo plano, con avance, reanudables y sin cortar la música) y aparecen en Local; el QR de **Conectar SoundCloud** deja la cuenta conectada al instante, con la advertencia de seguridad. La cámara se apaga al cerrar, en HOME o en reposo, y libera su memoria.

### Cambiado
- Configuración se cierra con la X de arriba o con (B); la fila "Cerrar" dejó su lugar a "Recibir desde el PC".
- La biblioteca local admite hasta 2000 canciones (antes 300) y las listas hasta 500 elementos.
- La cola de descargas ya no tiene límite de 16 canciones.

### Documentación
- Nueva sección sobre [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share), el proyecto compañero para pasar música desde el PC con un QR.

## [0.6.0] - 2026-10-18

### Añadido
- Descargas de SoundCloud: el botón con la flecha (junto al del minijuego) o (Y) baja la canción enfocada a `sdmc:/music/SoundCloud/`. Se descargan de a una en segundo plano (cola de hasta 16) mientras sigue sonando la música; el botón muestra el avance y las ya descargadas llevan un ✓ en la lista. Cada archivo lleva etiqueta ID3v2.3 con título, artista, álbum "SoundCloud", número de pista, duración y la carátula en 300x300. Las previews de 30 s no se descargan. SoundCloud hoy solo entrega AAC por HLS, así que se guardan como `.aac` (ADTS); si una pista trae MP3 progresivo se guarda como `.mp3`.
- Cover flow en el modo Local: la pantalla de arriba muestra los álbumes en perspectiva con reflejo, agrupados por la etiqueta de álbum (TALB) que se pone con mp3tag o similares, sin carpetas. Abajo van las canciones del álbum enfocado, ordenadas por número de pista. La cruceta izquierda/derecha (o el analógico) cambia de álbum; arriba/abajo y (A) eligen la canción. El álbum de las descargas va primero con su carátula naranja de SoundCloud; las pistas sin álbum quedan al final en "Sin álbum". La lupa filtra por título, artista o álbum.
- Carátulas: se leen de la imagen embebida (APIC, JPEG o PNG), se reducen a 128x128 en un hilo de baja prioridad empezando por el álbum enfocado y se guardan en `sdmc:/3ds/sound3ds/covers/` para abrir al instante la próxima vez. Los álbumes sin carátula reciben una portada de color con una corchea y su nombre.
- El modo Local reproduce también `.aac` y usa la duración de la etiqueta TLEN cuando existe.
- Banner del menú HOME nuevo: el cubo mascota en 3D, quieto en su pose de baile, con el título debajo (renderizado con Blender; `tools/banner/`). Al elegir la app suena un jingle propio: el salto del cubo, un arpegio tipo marimba y un destello.
- Configuración (engranaje, junto a la paleta en el menú y en la fila de botones del reproductor): idioma español / inglés para toda la interfaz y los avisos, y "LED RGB al reproducir", que ilumina el LED de notificación (el de StreetPass) en un arcoíris mientras suena música y lo apaga al pausar o salir. Usa el MCU (`mcu::HWC`), así que funciona en toda la familia: 3DS, 3DS XL, 2DS, New 3DS, New 3DS XL y New 2DS XL. Se guarda en `sdmc:/3ds/sound3ds/settings.ini`.
- Banner 3D en el menú HOME (modelo CGFX animado, con la bandera `extendedbanner`) y jingle con instrumentos y voz.

### Cambiado
- Anterior/siguiente en Local recorre el álbum desde el que se eligió la canción, aunque se esté mirando otro.

## [0.5.0] - 2026-10-16

### Añadido
- Menú principal con dos fuentes: **SoundCloud** (el reproductor de streaming) y **Local** (MP3 de la tarjeta SD). Se elige con la cruceta + (A) o tocando la tarjeta; (B) o el botón de casa vuelven al menú. Junto a la paleta de tema, una píldora muestra lo que suena y lleva de vuelta a esa lista.
- Modo Local: busca MP3 en `sdmc:/music` y `sdmc:/3ds/sound3ds/music` (hasta 3 niveles de subcarpetas, máx. 300), lee título y artista de las etiquetas ID3v2 (o del nombre `Artista - Título.mp3`), calcula la duración (cabecera Xing/Info para VBR) y permite adelantar. La lupa filtra la lista.
- Música en reposo: con música sonando, cerrar la tapa (o activar el interruptor de reposo de la 2DS) ya no suspende la app. Las pantallas se apagan, el audio sigue por los parlantes (o los audífonos si están conectados) y el Wi-Fi se mantiene. Un toque de (L) o (R) pausa/reanuda, doble toque de (L) va a la anterior y de (R) a la siguiente; START, (B) y el táctil se ignoran hasta abrirla.
- Minijuego "Cubo vs. Ruido": el botón del mando, junto a la paleta en SoundCloud y Local, alterna la pantalla superior entre el reproductor y el juego. Con el analógico el cubo esquiva bloques de ruido (aparecen más con los graves de la canción) y junta notas; (X) da un impulso invulnerable y (Y) lanza una onda que limpia el ruido cercano tras juntar 4 notas. Tres vidas; al perder muestra puntos y récord (guardado en `sdmc:/3ds/sound3ds/game_best.txt`) y vuelve a empezar solo. Mientras está oculto queda congelado y al volver hay una cuenta corta.
- Registro de diagnóstico en `sdmc:/3ds/sound3ds/debug.log` (reposo, menú HOME, salida).
- Estado "cargando" de la mascota: desde que se elige una canción (o se adelanta) hasta que suena, y si el stream se queda sin datos, se queda pensando junto a un spinner. En Modern es un cometa cian de una sola pieza; en Y2K, el clásico de 8 puntos que avanza a saltos.
- Mascota: un cubo con dos ojitos cuadrados, morado en Modern Glow y verde en Y2K. Duerme con ZzZ tras 10 s sin interacción, mira una nube en el menú de SoundCloud o una tarjeta SD en el de Local, mueve los ojos mientras busca y baila con los graves cuando suena música. Grande en el menú y pequeño en el reproductor.

### Cambiado
- La app arranca en el menú. Se quitaron el audio demo y las cuatro pistas de ejemplo.
- Una búsqueda nueva ya no interrumpe la canción que suena.

### Corregido
- Las animaciones usan un reloj relativo al arranque: `osGetTime()` en `float` perdía la parte fraccionaria.

## [0.4.0] - 2026-10-16

### Añadido
- Reproducción de canciones completas de SoundCloud en HLS + AAC, decodificadas en la consola: lista m3u8, segmentos fMP4 (`fmp4.c`, `hls.c`) y decodificador Helix AAC en punto fijo (`source/helix-aac`, RPSL) con el camino ARMv6 en ensamblador.
- La búsqueda elige por pista la mejor transcodificación: MP3 completo, luego HLS AAC, luego la preview MP3 de 30 s.
- La barra de progreso en HLS salta al segmento que contiene el instante elegido.
- Indicador de carga del decodificador en la pantalla superior (por ejemplo `NDSP AAC 6%`).
- Prueba en el PC del camino HLS/AAC (`tests/host/run.sh`), comparada contra ffmpeg.
- Ícono de batería en la cabecera: porcentaje exacto vía `mcu::HWC` (o 5 niveles vía `ptm:u`), rojo bajo 20 % y con rayo al cargar.
- Resultados paginados: hasta 50 por búsqueda en páginas de 5, con paginador táctil y la cruceta (arriba/abajo recorre canciones, izquierda/derecha cambia de página; (A) reproduce la enfocada).
- Íconos vectoriales: anterior/siguiente y reproducir/pausa en botones circulares, paleta de colores para cambiar el tema y lupa con `: search` en la barra de búsqueda.

### Cambiado
- La búsqueda usa jansson para leer el JSON de SoundCloud en vez de buscar texto.
- La salida de audio sigue la frecuencia de la pista (44,1 o 48 kHz).
- Nada se reproduce solo: ni al abrir la app ni al terminar una búsqueda.
- Se quitó el control de volumen de la app: queda al máximo y el nivel lo fija el deslizador físico de la consola. Su lugar lo ocupa el paginador.
- El botón de tema pasa a ser solo el ícono de paleta.
- Ecualizador rediseñado y más bajo para no chocar con la carátula: barras redondeadas con degradado de graves a agudos, picos en píldora y reflejo (Modern); visor LED empotrado de 12 segmentos con LEDs apagados tenues (Y2K).
- Los círculos se dibujan como abanicos de triángulos: Azahar no dibuja `C2D_DrawCircleSolid` (el vinilo de la carátula tampoco se veía).

### Corregido
- Elegir un resultado sin stream MP3 reproducía el audio demo en su lugar.
- Los errores de búsqueda y de stream se muestran en pantalla.

## [0.3.0] - 2026-10-16

### Añadido
- Backend de audio NDSP con cola de buffers sin huecos y pausa real; CSND queda como respaldo automático cuando falta `sdmc:/3ds/dspfirm.cdc`.
- Indicador del backend activo, "Buscando..." durante la búsqueda y "Cargando..." mientras llega la carátula.
- Adelantar con la barra de progreso: al soltarla, el stream se reinicia desde el byte proporcional con HTTP Range (reutiliza la URL resuelta del CDN y la vuelve a resolver si caducó); en el demo se salta dentro del MP3 incrustado.
- Empaquetado `.cia` con `scripts/build_cia.sh`, ícono, banner y sonido propios (título `000400000F5D3D00`).
- Paso automático a la siguiente pista al terminar un stream; si una pista no entrega audio se muestra "SIN STREAM" y (A) la reintenta.

### Corregido
- Cortes periódicos de audio: CSND ahora reproduce un anillo PCM en bucle en lugar de bloques sueltos con silencio entre ellos.
- La búsqueda y la descarga de carátulas ya no congelan la interfaz; las transferencias se cancelan al cambiar de pista y el stream se corta si la conexión queda colgada.
- Colores de la carátula (orden de bytes de `GPU_RGBA8`).
- La lista muestra los 5 resultados de búsqueda; títulos y artistas se recortan por ancho real y ya no se superponen.
- El tiempo transcurrido sigue al audio reproducido en lugar del reloj.
- Lectura del buffer MP3 con `memcpy` y salto directo de etiquetas ID3.

### Cambiado
- `sound3ds.3dsx` y `sound3ds.smdh` dejan de versionarse; los binarios se publican en GitHub Releases.
- La búsqueda pide 20 pistas en un buffer de 256 KB (antes 35 en 128 KB).
- Se quitó `romfs/demo.mp3`, que se empaquetaba sin usarse; el demo sigue incrustado desde `data/demo.bin`.

### Pendiente de validar
- Reproducción continua con NDSP y CSND en una Old 3DS/2DS física.
- Orientación de la carátula descargada.

## [0.2.0] - 2026-10-16

### Añadido
- Diagnóstico en pantalla cuando CSND no puede inicializarse, con el código o causa informada por el servicio.
- Buffers estéreo separados para emitir audio PCM mediante dos canales CSND y limpieza de caché antes de reproducirlos.
- Manejo de muestras MP3 restantes entre bloques para evitar descartar el final de una trama al llenar un buffer.

### Corregido
- Al elegir otra pista se detiene el flujo anterior y se descartan los buffers pendientes, evitando que el audio de la canción previa se mezcle con la nueva.
- El audio de demostración queda reservado al modo de demostración y no reemplaza silenciosamente una búsqueda sin stream compatible.
- La transferencia de red reintenta alimentar el ring buffer cuando está lleno y se detiene limpiamente al cancelar el flujo.
- Se restableció la terminación nula de títulos y artistas copiados a los modelos de reproducción.

### Documentación
- Se actualizó el README para describir el backend y los límites de validación sin afirmar compatibilidad universal ni 60 FPS garantizados.
- Se añadió una evaluación de la alternativa YouTube Music y de su encaje con la plataforma 3DS.

### Pendiente de validar
- Reproducción continua y cambio entre canciones en Azahar y en una Old 3DS/2DS física.
- Diferentes respuestas de streaming, cortes de red, pausado y reanudación.
- El backend actual acepta MP3 progresivo; AAC/HLS no está implementado en esta versión.
