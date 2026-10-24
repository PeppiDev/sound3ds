#include "i18n.h"
#include <stdint.h>
#include <string.h>

static volatile Lang s_lang = LANG_EN;

static const char* const s_pairs[][2] = {
    // Barra superior y reproductor
    {"PAUSA", "PAUSED"},
    {"SIN STREAM", "NO STREAM"},
    {"Cargando...", "Loading..."},
    {"Buscando...", "Searching..."},
    {"filtrar", "filter"},
    {"Nada sonando", "Nothing playing"},
    {"Toca la lupa para buscar", "Tap the magnifier to search"},
    {"Copia tus MP3 a sdmc:/music", "Copy your MP3s to sdmc:/music"},
    {"Nada coincide con el filtro", "Nothing matches the filter"},
    // Cover flow
    {"Ningún álbum coincide con el filtro", "No album matches the filter"},
    {"No hay música en la tarjeta SD", "No music on the SD card"},
    {"Copia tus MP3 con etiquetas a sdmc:/music", "Copy your tagged MP3s to sdmc:/music"},
    {"o descárgalos desde SoundCloud", "or download them from SoundCloud"},
    {"canción", "song"},
    {"canciones", "songs"},
    {"D-pad izq./der.: álbum - arriba/abajo: canción - A: reproducir", "D-pad left/right: album - up/down: song - A: play"},
    {"Sin álbum", "No album"},
    {"Todas las canciones", "All songs"},
    {"Varios artistas", "Various artists"},
    {"Descargas", "Downloads"},
    // Minijuego
    // Cuenta de SoundCloud
    {"Cuenta de SoundCloud", "SoundCloud account"},
    {"Desconectar", "Disconnect"},
    {"Cancelar", "Cancel"},
    {"¿Desconectar la cuenta?", "Disconnect the account?"},
    {"Se borrará la sesión guardada en la tarjeta SD.", "The session saved on the SD card will be deleted."},
    {"Venció: reconéctala desde Sound3DS Share", "Expired: reconnect it from Sound3DS Share"},
    {"Conéctala desde Sound3DS Share en el PC", "Connect it from Sound3DS Share on your PC"},
    {"Cuenta de SoundCloud desconectada", "SoundCloud account disconnected"},
    // Recibir desde el PC
    {"Recibir desde el PC", "Receive from PC"},
    {"Apunta la cámara al código QR", "Point the camera at the QR code"},
    {"QR detectado", "QR found"},
    {"Iniciando la cámara...", "Starting the camera..."},
    {"No se pudo abrir la cámara", "Couldn't open the camera"},
    {"Recibiendo la cuenta de SoundCloud...", "Receiving the SoundCloud account..."},
    {"Leyendo la lista del PC...", "Reading the list from the PC..."},
    {"En el PC abre Sound3DS Share", "On the PC open Sound3DS Share"},
    {"elige canciones o toca Conectar SoundCloud", "pick songs or click Connect SoundCloud"},
    {"Toca Generar QR", "Click Generate QR"},
    {"o espera el QR de la cuenta", "or wait for the account QR"},
    {"Apunta la cámara al QR", "Point the camera at the QR"},
    {"la cámara de afuera, a unos 20 cm", "outer camera, about 20 cm away"},
    {"Conectando con Sound3DS Share...", "Connecting to Sound3DS Share..."},
    {"La música sigue sonando mientras tanto", "Music keeps playing meanwhile"},
    {"Importante", "Important"},
    {"Sound3DS guardará tu sesión de SoundCloud en la tarjeta SD. Cualquiera que copie la SD podría usar tu cuenta. No compartas la tarjeta y usa Desconectar en Configuración si vas a prestarla o venderla.",
     "Sound3DS will store your SoundCloud session on the SD card. Anyone who copies the SD card could use your account. Don't share the card, and use Disconnect in Settings if you lend or sell it."},
    {"Las canciones se bajan en segundo plano", "Songs download in the background"},
    {"Puedes seguir escuchando música. El avance se ve en el botón de descarga y al terminar aparecen en Local, cada una en su álbum.",
     "You can keep listening. Progress shows on the download button, and when they finish they show up in Local, each in its album."},
    {"Deja Sound3DS Share abierto en el PC hasta que terminen.", "Keep Sound3DS Share open on the PC until they finish."},
    {"Qué revisar:", "What to check:"},
    {"- Sound3DS Share sigue abierto en el PC", "- Sound3DS Share is still open on the PC"},
    {"- PC y consola en la misma red Wi-Fi", "- PC and console on the same Wi-Fi"},
    {"- El firewall del PC deja pasar la conexión", "- The PC firewall allows the connection"},
    {"- El QR de la cuenta dura 10 minutos y sirve una vez", "- The account QR lasts 10 minutes and works once"},
    {"Escanear otro", "Scan another"},
    {"Volver", "Back"},
    {"Listo", "Done"},
    {"Cuenta conectada", "Account connected"},
    {"Tus Likes, Playlists y Feed ya están en SoundCloud", "Your Likes, Playlists and Feed are now in SoundCloud"},
    {"No se pudo conectar la cuenta", "Couldn't connect the account"},
    {"No se pudo recibir", "Couldn't receive"},
    {"Inténtalo de nuevo", "Try again"},
    {"%d canciones en camino", "%d songs on the way"},
    {"Ya estaban todas en la SD", "They were all on the SD card already"},
    {"%d ya estaban en la SD", "%d were already on the SD card"},
    {"Se guardan en sdmc:/music", "Saved to sdmc:/music"},
    {"Aparecen en Local, cada una en su álbum", "They show up in Local, each in its album"},
    {"El QR ya se usó o venció. Toca Conectar SoundCloud de nuevo.", "The QR was already used or expired. Click Connect SoundCloud again."},
    {"No se pudo conectar con el PC. ¿Misma red Wi-Fi?", "Couldn't reach the PC. Same Wi-Fi network?"},
    {"No se pudo leer la lista del PC", "Couldn't read the list from the PC"},
    {"El QR no trae canciones .mp3, .aac ni .flac", "The QR has no .mp3, .aac or .flac songs"},
    {"Respuesta inválida de Sound3DS Share", "Invalid response from Sound3DS Share"},
    {"No se pudo guardar en la tarjeta SD", "Couldn't save to the SD card"},
    {"Desde el PC: %d canciones listas en Local", "From the PC: %d songs ready in Local"},
    {"Desde el PC: %d de %d descargadas", "From the PC: %d of %d downloaded"},
    {"No se pudo bajar del PC: %.50s", "Couldn't download from the PC: %.50s"},
    {"Ya estaba en la SD: %.60s", "Already on the SD card: %.60s"},
    {"Leyendo tu música...", "Reading your music..."},
    {"%d canciones encontradas", "%d songs found"},
    {"Buscar", "Search"},
    {"Tus likes", "Your likes"},
    {"Tus playlists", "Your playlists"},
    {"Toca el título para cargar", "Tap the title to load"},
    {"No se pudo cargar", "Couldn't load"},
    {"Todavía no tienes likes", "No likes yet"},
    {"No tienes playlists guardadas", "No saved playlists"},
    {"Tu feed no tiene canciones nuevas", "No new tracks in your feed"},
    {"Conecta tu cuenta desde Sound3DS Share", "Connect your account from Sound3DS Share"},
    {"Vuelve a conectar SoundCloud desde Sound3DS Share", "Reconnect SoundCloud from Sound3DS Share"},
    {"Récord %d", "Best %d"},
    {"¿Listo?", "Ready?"},
    {"Analógico mover - X impulso - Y onda", "Circle Pad move - X dash - Y wave"},
    {"Fin de la partida", "Game over"},
    {"Puntos %d", "Score %d"},
    {"Nuevo récord", "New record"},
    {"Sigue intentando", "Keep trying"},
    {"X para jugar otra vez", "X to play again"},
    // Mascota y menú
    {"Zzz... toca algo para despertarlo", "Zzz... touch something to wake it up"},
    {"Streaming desde SoundCloud", "Streaming from SoundCloud"},
    {"Música de tu tarjeta SD", "Music from your SD card"},
    {"Cargando la canción...", "Loading the song..."},
    {"¡Qué temazo!", "What a banger!"},
    {"Elige qué quieres escuchar", "Choose what to listen to"},
    {"¿Qué quieres escuchar?", "What do you want to hear?"},
    {"Busca y escucha en streaming", "Search and stream"},
    {"MP3 en sdmc:/music", "MP3s in sdmc:/music"},
    // Avisos
    {"%d canciones en %d álbumes", "%d songs in %d albums"},
    {"No se pudo iniciar la pista", "Couldn't start the track"},
    {"Buscar en SoundCloud", "Search SoundCloud"},
    {"Filtrar (vacio = todas)", "Filter (empty = all)"},
    {"Descargando: %.70s", "Downloading: %.70s"},
    {"Ya está en la cola de descargas", "Already in the download queue"},
    {"Ya está en tu álbum SoundCloud", "Already in your SoundCloud album"},
    {"Solo hay una preview de 30 s: no se descarga", "Only a 30 s preview: can't download"},
    {"No se pudo poner en la cola", "Couldn't queue it"},
    {"La busqueda fallo", "Search failed"},
    {"%d de %d son previews de 30 s", "%d of %d are 30 s previews"},
    {" (%d en cola)", " (%d queued)"},
    {"No llego audio", "No audio arrived"},
    {"FLAC de +96 kHz: conviertelo en Share", "FLAC over 96 kHz: convert it in Share"},
    // Red y descargas
    {"SoundCloud respondio HTTP %ld", "SoundCloud returned HTTP %ld"},
    {"Red: %s", "Network: %s"},
    {"Respuesta invalida (linea %d)", "Invalid response (line %d)"},
    {"%d resultados, ninguno reproducible", "%d results, none playable"},
    {"Sin resultados", "No results"},
    {"Lista HLS: %s", "HLS playlist: %s"},
    {"Lista HLS no compatible", "Unsupported HLS playlist"},
    {"HLS sin segmento de inicio", "HLS without an init segment"},
    {"Formato de audio no compatible", "Unsupported audio format"},
    {"Segmento %d danado", "Segment %d is damaged"},
    {"Segmento %d dañado", "Segment %d is damaged"},
    {"No se pudo obtener el stream", "Couldn't get the stream"},
    {"No se pudo escribir en la SD", "Couldn't write to the SD card"},
    {"Ya estaba descargada: %.60s", "Already downloaded: %.60s"},
    {"No se pudo obtener: %.60s", "Couldn't fetch: %.60s"},
    {"No se pudo crear el archivo en la SD", "Couldn't create the file on the SD card"},
    {"Descargada: %.70s", "Downloaded: %.70s"},
    {"Descarga cancelada", "Download cancelled"},
    {"Falló la descarga%s%s", "Download failed%s%s"},
    {"No se pudo abrir el archivo", "Couldn't open the file"},
    {"No se pudo crear el hilo de lectura", "Couldn't create the reader thread"},
    // Configuración
    {"Configuración", "Settings"},
    {"Idioma", "Language"},
    {"LED RGB al reproducir", "RGB LED while playing"},
    {"L/R con la tapa cerrada", "L/R with the lid closed"},
    {"1 toque: pausa - 2: cambiar", "1 tap: pause - 2: skip"},
    {"Mantén 1 s - L+R: pausa", "Hold 1 s - L+R: pause"},
    {"Usa los audífonos", "Use your headphones"},
    {"Favoritos", "Favorites"},
    {"Álbumes", "Albums"},
    {"Al azar", "Random"},
    {"ÁLBUM", "ALBUM"},
    {"Servidor", "Server"},
    {"Próximamente", "Coming soon"},
    {"Streaming en línea", "Online streaming"},
    {"Tu propio servidor de música", "Your own music server"},
    {"Tu música de la SD", "Your music on the SD card"},
    {"Conéctalo desde Share", "Connect it from Share"},
    {"Conecta tu servidor desde Sound3DS Share", "Connect your server from Sound3DS Share"},
    {"Bandcamp llega pronto", "Bandcamp is coming soon"},
    {"Las descargas desde el servidor llegan pronto", "Downloads from the server are coming soon"},
    {"Toca la lupa para buscar en tu servidor", "Tap the magnifier to search your server"},
    {"Toca la lupa para buscar álbumes", "Tap the magnifier to search albums"},
    {"álbumes", "albums"},
    {"No tienes canciones con estrella", "You have no starred songs"},
    {"El servidor no tiene álbumes", "The server has no albums"},
    {"El servidor no tiene canciones", "The server has no songs"},
    {"Datos del servidor incompletos", "Incomplete server details"},
    {"No se pudo conectar con el servidor", "Couldn't connect to the server"},
    {"El servidor rechazó el usuario o la contraseña", "The server rejected the user or password"},
    {"El servidor respondió con un error", "The server returned an error"},
    {"No se pudo guardar en la SD", "Couldn't save to the SD card"},
    {"No tiene canciones reproducibles", "It has no playable songs"},
    {"El servidor respondio HTTP %ld", "The server returned HTTP %ld"},
    {"Servidor conectado", "Server connected"},
    {"Ábrelo desde Servidor en el menú", "Open it from Server in the menu"},
    {"No se pudo conectar el servidor", "Couldn't connect the server"},
    {"El QR ya se usó o venció. Genera uno nuevo en Share.", "This QR was already used or expired. Make a new one in Share."},
    {"Conectando el servidor...", "Connecting the server..."},
    {"Tu servidor quedó conectado", "Your server is connected"},
    {"Ábrelo desde la tarjeta Servidor del menú. Sound3DS guarda un token de acceso en la SD, nunca tu contraseña; puedes desconectarlo en Configuración.", "Open it from the Server card in the menu. Sound3DS stores an access token on the SD card, never your password; you can disconnect it in Settings."},
    {"SoundCloud: conéctala desde Sound3DS Share", "SoundCloud: connect it from Sound3DS Share"},
    {"Servidor: conéctalo desde Sound3DS Share", "Server: connect it from Sound3DS Share"},
    {"¿Desconectar el servidor?", "Disconnect the server?"},
    {"Se borrará el token guardado en la tarjeta SD.", "The token saved on the SD card will be deleted."},
    {"Servidor desconectado", "Server disconnected"},
    {"Reproduce una canción para marcarla", "Play a song to star it"},
    {"Agregada a Favoritos", "Added to Favorites"},
    {"Quitada de Favoritos", "Removed from Favorites"},
    {"En SoundCloud usa los Likes", "On SoundCloud, use Likes"},
    {"El servidor no guardó la estrella", "The server didn't save the star"},
    {"Toque", "Tap"},
    {"Mantener", "Hold"},
    {"Desactivados", "Off"},
    {"Fin de la lista", "End of the list"},
    {"Aleatorio activado", "Shuffle on"},
    {"Aleatorio desactivado", "Shuffle off"},
    {"Repetir: no", "Repeat: off"},
    {"Repetir: la lista", "Repeat: all"},
    {"Repetir: la canción", "Repeat: one"},
    {"El LED de notificación brilla en arcoíris", "The notification LED glows in a rainbow"},
    {"Activado", "On"},
    {"Desactivado", "Off"},
    {"Cerrar", "Close"},
    {"Toca una opción o usa la cruceta y A - B: volver", "Tap an option or use the D-pad and A - B: back"},
    {"Idioma: Español", "Language: English"},
    {"LED RGB activado", "RGB LED on"},
    {"LED RGB desactivado", "RGB LED off"},
    // Playlists y cola de descargas
    {"Cargando playlist...", "Loading playlist..."},
    {"%d canciones - %lu min", "%d songs - %lu min"},
    {"%d no disponibles", "%d unavailable"},
    {"Descargar", "Download"},
    {"Reanudar", "Resume"},
    {"Descartar", "Discard"},
    {"Descargas pendientes", "Pending downloads"},
    {"Hay una descarga sin terminar: %s. ¿Quieres reanudarla?", "There's an unfinished download: %s. Resume it?"},
    {"%.60s (%d de %d)", "%.60s (%d of %d)"},
    {"%d canciones en cola", "%d songs queued"},
    {"Playlist %.60s: %d canciones descargadas", "Playlist %.60s: %d songs downloaded"},
    {"Playlist %.50s: %d de %d descargadas", "Playlist %.50s: %d of %d downloaded"},
    {"Sin espacio en la SD: descargas en pausa", "SD card is full: downloads paused"},
    {"Sin conexión: descargas en pausa", "No connection: downloads paused"},
    {"Ya está en el álbum %.60s", "Already in the album %.60s"},
    {"Descargando %d canciones (%d omitidas)", "Downloading %d songs (%d skipped)"},
    {"Descargando %d canciones de la playlist", "Downloading %d songs from the playlist"},
    {"La playlist ya se está descargando", "The playlist is already downloading"},
    {"Ya estaba todo descargado", "Everything was already downloaded"},
    {"Abre la playlist para descargarla", "Open the playlist to download it"},
    {"Buscar playlists en SoundCloud", "Search playlists on SoundCloud"},
    {"Toca la lupa para buscar playlists", "Tap the magnifier to search playlists"},
    {"No se pudo abrir la playlist", "Couldn't open the playlist"},
    {"La playlist no tiene canciones reproducibles", "The playlist has no playable songs"},
    {"Descargando %d de %d", "Downloading %d of %d"},
    {"Reanudando descargas", "Resuming downloads"},
    {"Descargas descartadas", "Downloads discarded"},
};

#define NPAIRS (sizeof(s_pairs) / sizeof(s_pairs[0]))
#define HSIZE 512   // tabla hash por contenido (potencia de 2, más del doble de entradas)

static int16_t s_hash[HSIZE];
static int s_ready = 0;

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void build(void) {
    for (int i = 0; i < HSIZE; i++) s_hash[i] = -1;
    for (int i = 0; i < (int)NPAIRS; i++) {
        uint32_t k = fnv(s_pairs[i][0]) & (HSIZE - 1);
        while (s_hash[k] >= 0) k = (k + 1) & (HSIZE - 1);
        s_hash[k] = (int16_t)i;
    }
    s_ready = 1;
}

void i18n_set(Lang lang) {
    s_lang = lang;
}

Lang i18n_get(void) {
    return s_lang;
}

const char* L(const char* es) {
    if (s_lang == LANG_ES || !es || !*es) return es;
    if (!s_ready) build();
    uint32_t k = fnv(es) & (HSIZE - 1);
    while (s_hash[k] >= 0) {
        if (strcmp(s_pairs[s_hash[k]][0], es) == 0) return s_pairs[s_hash[k]][1];
        k = (k + 1) & (HSIZE - 1);
    }
    return es;
}
