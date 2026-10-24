# Sound3DS 🎵

[English](README.md) | **Español**

### Reproductor de Audio Estético para Nintendo 3DS / 2DS (Optimizado para Old 3DS)

**Sound3DS** es un reproductor de música (MP3, AAC y FLAC) y cliente de streaming para Nintendo 3DS/2DS: SoundCloud, tu propio servidor Navidrome/Subsonic y Bandcamp. El objetivo de rendimiento es mantener una interfaz fluida en Old 3DS/2DS; el rendimiento y la salida de audio deben validarse en hardware real.

## 📲 Instalar con QR
<p align="center">
  <a href="https://github.com/PeppiDev/sound3ds/releases/latest/download/sound3ds.cia">
    <img src="docs/qr-cia.png" alt="QR para instalar sound3ds.cia con FBI" width="220">
  </a>
</p>

En la consola abre **FBI → Remote Install → Scan QR Code** y escanea el código: descarga e instala el `.cia` de la última versión publicada. Al terminar, Sound3DS aparece en el menú HOME. Requiere conexión Wi-Fi y una consola con CFW (Luma3DS).

<p align="center">
  <img src="docs/screenshot-soundcloud.png" alt="Sound3DS reproduciendo pyro (Slowed) By ZERTAL desde SoundCloud" width="420">
  <br>
  <em>Reproduciendo «pyro (Slowed) By ZERTAL» en streaming desde SoundCloud</em>
</p>

---

## 🌟 Características Principales

1. **Cuatro fuentes de música** en el menú principal:
   - **SoundCloud**: busca canciones y playlists y, con tu cuenta conectada, tus **Likes**, **Playlists** y **Feed**. Canciones completas por HLS + AAC; las previews de 30 s se marcan "30s".
   - **Tu servidor propio**: **Navidrome** o cualquier servidor **Subsonic** (Airsonic, Gonic…), con búsqueda, favoritos, álbumes, al azar y descargas. Suena cualquier formato: el servidor lo convierte a MP3.
   - **Bandcamp**: busca y recorre Populares, Destacados y Novedades (solo escuchar, sin descargas).
   - **Local**: tu música de la tarjeta SD.
2. **Biblioteca local con cover flow**: MP3, AAC y **FLAC** (hasta 24-bit / 96 kHz en una Old 3DS), agrupados por álbum según sus etiquetas y mostrados como el cover flow del iPod. Se lee en segundo plano y queda indexada en la SD, así que abre al instante después de la primera vez. Incluye el álbum **Todas las canciones**, el álbum **Favoritos** (el botón de la estrella) y tus propias **playlists**, armadas en la misma consola (solo referencias, guardadas como `.m3u`).
3. **Descargas a la SD**: canciones y playlists completas de SoundCloud, y canciones y álbumes de tu servidor, con etiquetas y carátula. La cola corre en segundo plano y se reanuda si se corta.
4. **Controles de reproducción**: aleatorio y repetir (no / la lista / la canción), adelantar, el botón de los audífonos de cable con micrófono (1 toque pausa, 2 siguiente, 3 anterior) y **modo reposo**: cierra la tapa y la música sigue, con L/R para controlarla.
5. **[Sound3DS Share](https://github.com/PeppiDev/sound3ds-share)** en el PC: envía canciones por Wi-Fi con un QR (los FLAC hi-res se convierten en el camino) y conecta tu cuenta de SoundCloud, tu servidor o Bandcamp. Tus contraseñas nunca llegan a la consola.
6. **Temas y extras**:
   - **Modern Glow**: Estética cyberpunk/minimalista en modo oscuro con acentos violeta/neón y barras continuas con antialiasing.
   - **Y2K Classic Tunes**: Estilo reproductores de los años 2000 (iTunes clásico / Winamp), con fondo de aluminio cepillado, visor estilo LCD retro fósforo verde y analizador de espectro con segmentos LED discretos (Verde/Amarillo/Rojo).
   - Un visualizador de espectro reactivo, un cubo mascota que baila con la música, el minijuego *Cubo vs. Ruido* y el LED de notificación en arcoíris mientras suena música.
7. **Por dentro** (pensado para Old 3DS / 2DS):
   - Decodificación en la consola: MP3 con `minimp3`, AAC con Helix AAC (punto fijo) y FLAC con `dr_flac` en el segundo núcleo de la CPU.
   - Audio por **NDSP** (preferido, sin cortes y con pausa real; necesita `sdmc:/3ds/dspfirm.cdc`, que se obtiene con [DSP1](https://github.com/zoogie/DSP1)) con respaldo automático por **CSND**. La pantalla de arriba muestra el backend y la carga del decodificador.
   - Audio, red, biblioteca y carátulas corren en sus propios hilos, así que la interfaz nunca se traba y cada transferencia se puede cancelar.
   - FFT entera de 128 puntos (<0,04 ms por cuadro) para el visualizador de 16 bandas.

---

## 🎮 Controles

### Menú principal
- **Pantalla superior**: la mascota, un cubo con ojitos cuadrados que reacciona a lo que haces (y se duerme si no haces nada).
- **Pantalla inferior**: elige **SoundCloud**, **Local**, **Servidor** o **Bandcamp** con la cruceta + (A) o tocando la tarjeta. La paleta cambia el tema y la píldora "sonando ahora" lleva a la lista de lo que suena.

### Reproductor (SoundCloud y Local)
- **Pantalla superior**: carátula, título, artista, tiempo, ecualizador y la mascota pequeña.
- **Barra de búsqueda**: en SoundCloud busca; en Local filtra tus canciones (vacío = todas).
- **Lista**: resultados de SoundCloud de a 50 (las listas de la cuenta piden más al llegar al final) y hasta 2000 canciones en Local, en páginas de 5.
- **Pestañas de la cuenta** (con la cuenta conectada): **Buscar · Likes · Playlists · Feed**, bajo la barra de búsqueda. Tocar la pestaña activa o el título recarga la lista.
- **Barra de progreso**: arrastra y suelta para saltar.
- **Transporte**: aleatorio, anterior, reproducir/pausa, siguiente y repetir (no → toda la lista → esta canción). El aleatorio mezcla la lista desde la que estás escuchando (un álbum, una playlist, tus Likes…) sin repetir canciones hasta que suenen todas.
- **Abajo**: paleta de tema, minijuego, descarga (SoundCloud y tu servidor) o **agregar a playlist** (Local), configuración, la **estrella** de favoritos, paginador y casa para volver al menú.
- **En Local**, la pantalla de arriba es el cover flow de álbumes y la lista muestra las canciones del álbum enfocado. El primer álbum, **Todas las canciones**, tiene toda tu biblioteca (o lo que coincida con el filtro).

### Botones físicos
- **Cruceta arriba / abajo**: recorrer canciones (o tarjetas en el menú)
- **Cruceta izquierda / derecha**: página anterior / siguiente
- **(A)**: reproducir la canción enfocada, o pausar/reanudar la que suena
- **(B)**: volver al menú
- **(L) / (R)**: pista anterior / siguiente
- **(ZL) / (ZR)** (New 3DS y New 2DS XL): pestaña anterior / siguiente de la cuenta
- **(Y)**: descargar la canción enfocada (SoundCloud y tu servidor); en Local, agregarla a una playlist
- **(X)**: marcar como favorita la canción que suena (estrella)
- **(SELECT)**: alternar tema visual
- **(START)**: salir
- **Volumen**: deslizador físico de la consola

### Configuración
El botón del **engranaje** (en el menú, junto a la paleta, y en el reproductor, junto al de descarga) abre la configuración:
- **Idioma**: inglés (por defecto) o español.
- **LED RGB al reproducir**: el LED de notificación (el de StreetPass) brilla en arcoíris mientras suena música. Funciona en todos los modelos de la familia 3DS.
- **L/R con la tapa cerrada**: **Toque** (1 toque pausa, 2 toques cambian de canción), **Mantener** (mantener L o R 1 segundo cambia de canción y L+R juntos pausa, para que no se aprieten en el bolsillo) o **Desactivados**.
- **Conexiones**: tu cuenta de SoundCloud, tu servidor y tu cuenta de Bandcamp, cada una con el botón **Desconectar** (borra la sesión de la SD).
- **Recibir desde el PC**: abre la cámara para escanear el QR de Sound3DS Share (canciones o la cuenta).

Se navega tocando o con la cruceta y **(A)**; **(B)** o la **X** de arriba cierran. Los cambios se guardan en `sdmc:/3ds/sound3ds/settings.ini`.

### Minijuego
El botón del **mando** (junto a la paleta) cambia la pantalla de arriba entre el reproductor y *Cubo vs. Ruido*, sin cortar la música:
- **Analógico**: mover el cubo y esquivar los bloques de ruido (salen más con los graves)
- **(X)**: impulso rápido; mientras dura no te golpean
- **(Y)**: onda expansiva que limpia el ruido cercano (se carga juntando 4 notas)

Tienes tres vidas; al perder se muestran los puntos y el récord, y la partida vuelve a empezar sola. Si lo ocultas, se congela hasta que lo vuelvas a abrir.

### En reposo
Si cierras la tapa (o activas el interruptor de reposo de la 2DS) con música sonando, la app sigue reproduciendo con las pantallas apagadas. El sonido sale por los parlantes, o por los audífonos si están conectados. Por defecto, los controles son un toque de **(L)** o **(R)** para pausar/reanudar, doble toque de **(L)** para la anterior y doble toque de **(R)** para la siguiente (en Configuración se pueden cambiar a *Mantener* o desactivar); al abrirla todo vuelve a la normalidad. Sin música, la consola se suspende como siempre.

### Botón de los audífonos
Los audífonos de cable con micrófono y botón (los típicos de celular) funcionan como en el celular, con la tapa abierta o cerrada:
- **1 toque**: pausa / reanudar
- **2 toques**: siguiente canción
- **3 toques**: canción anterior

### Música local y cover flow
Arrastra tus MP3, AAC y FLAC a `sdmc:/music/` (o envíalos por Wi-Fi con [Sound3DS Share](#-sound3ds-share-conectar-tu-cuenta-y-pasar-música-desde-el-pc)), sin organizar carpetas: el modo Local los agrupa por **álbum** según sus etiquetas. Antes de copiarlos, ponles los metadatos con [Mp3tag](https://www.mp3tag.de/) o similar:
- **Título**, **Artista**, **Álbum** y **N.º de pista** (el orden dentro del álbum)
- **Artista del álbum** si el disco tiene varios artistas
- **Carátula** embebida (JPEG o PNG): es la que se ve en el cover flow

La pantalla de arriba muestra los álbumes como el cover flow del iPod y la de abajo las canciones del álbum enfocado. **Cruceta izquierda/derecha** (o el analógico) cambia de álbum; **arriba/abajo** y **(A)** eligen la canción. Las canciones sin álbum quedan en "Sin álbum" y, sin etiquetas, el título y el artista salen del nombre `Artista - Título.mp3`.

### Descargar de SoundCloud
En SoundCloud, enfoca una canción y toca el botón con la **flecha** (o pulsa **(Y)**): se descarga a `sdmc:/music/SoundCloud/` en segundo plano, con sus metadatos y carátula, y aparece en el álbum **SoundCloud** del modo Local. Las ya descargadas llevan un ✓. Las que SoundCloud solo ofrece como preview de 30 s no se pueden descargar.

Una **playlist** abierta tiene el botón **Descargar**: baja todas sus canciones a `sdmc:/music/SoundCloud/<playlist>/` y aparece en Local como un álbum con su nombre y carátula. La cola se guarda en la SD: si sales, vas a HOME, se apaga la consola o se corta el Wi-Fi, al volver a abrir Sound3DS pregunta si reanudarla.

### Cuenta de SoundCloud
Para ver tus **likes, playlists y feed** en la consola:

1. En el PC abre [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share) y toca **Conectar SoundCloud**. Se abre la página oficial de SoundCloud: inicia sesión como siempre (también con Google o Apple).
2. Share muestra un QR. En la consola abre **Configuración → Recibir desde el PC** y escanéalo.
3. Listo: aparecen las pestañas **Likes · Playlists · Feed** y tu nombre arriba. La sesión se renueva sola.

**Importante:** Sound3DS guarda tu sesión de SoundCloud en la tarjeta SD. Cualquiera que copie la SD podría usar tu cuenta: no compartas la tarjeta y usa **Desconectar** en Configuración si vas a prestarla o venderla. La app solo **lee** de tu cuenta (no publica, no compra ni cambia nada). No es una integración oficial de SoundCloud: usa la misma API que su web y podría dejar de funcionar si SoundCloud la cambia. Si la sesión vence, solo hay que volver a conectarla.

### Favoritos y playlists propias (Local)
- **Favoritos**: el botón de la **estrella** (junto al engranaje) o **(X)** marca la canción que suena. En Local tus favoritas forman el álbum **Favoritos** al inicio del cover flow; en tu servidor propio la estrella se guarda en el mismo servidor.
- **Playlists**: en Local, enfoca una canción y pulsa **(Y)** o el botón **lista +** de la fila de abajo. Elige **+ Nueva playlist…** (el nombre se escribe con el teclado de la consola) o una de tus playlists; las que ya tienen la canción llevan un ✓. Tus playlists aparecen en el cover flow después de Favoritos y suenan en su orden (aleatorio y repetir funcionan dentro de ellas).
- Dentro de una playlist, el mismo botón permite quitar la canción, agregarla a otra playlist, renombrar la playlist o borrarla (las canciones siguen en la SD).
- Las playlists solo guardan **referencias**: cada una es un archivo `.m3u` en `sdmc:/3ds/sound3ds/playlists/` con una ruta por línea, así que casi no ocupan espacio. También se pueden crear o editar en el PC; las canciones que ya no están en la SD se saltan. Hasta 32 playlists de 500 canciones.

### FLAC
El modo Local reproduce **FLAC** además de MP3 y AAC, con sus etiquetas (título, artista, álbum, pista) y su carátula embebida. En una Old 3DS/2DS suena bien la calidad CD (16-bit / 44,1 kHz) y el hi-res hasta **24-bit / 96 kHz**; los de más de 96 kHz (por ejemplo 192 kHz) son demasiado pesados y se saltan con un aviso. No se pierde nada al convertirlos: la 3DS sale a unos 32,7 kHz / 16-bit de todos modos. [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share) convierte solo los FLAC de más de 48 kHz a 16-bit / 44,1 o 48 kHz al enviarlos (suenan igual en la consola y pesan unas 5 veces menos).

### Servidor propio (Navidrome / Subsonic)
Si tienes tu propio servidor de música con la API de Subsonic (**Navidrome**, Airsonic, Gonic…), la tarjeta **Servidor** del menú lo abre:

1. En el PC abre [Sound3DS Share](https://github.com/PeppiDev/sound3ds-share), toca **Conectar servidor** y escribe la dirección (por ejemplo `http://192.168.1.10:4533`), tu usuario y contraseña. Share los prueba contra el servidor.
2. Share muestra un QR. En la consola abre **Configuración → Recibir desde el PC** y escanéalo.
3. Listo: **Servidor** en el menú tiene las pestañas **Buscar · Favoritos · Álbumes · Al azar**.

- La consola nunca recibe tu contraseña: solo un token (el estándar de Subsonic, `md5(contraseña + sal)`). Se guarda en `sdmc:/3ds/sound3ds/server.json`; **Desconectar** en Configuración lo borra.
- El servidor convierte cada canción a **MP3 192 kbps** mientras escuchas (FLAC, Opus, lo que tengas), así que siempre suena en la 3DS. Se puede adelantar.
- La **estrella** marca la canción como favorita en el servidor (aparece en la pestaña Favoritos y en tus otras apps de Subsonic).
- **Descargas**: canciones y álbumes completos se bajan en MP3 192 kbps con etiquetas y carátula a `sdmc:/music/<servidor>/` (por ejemplo `sdmc:/music/Navidrome/`). Las sueltas forman el álbum **Navidrome** en Local, con su propia portada; los álbumes conservan su nombre y carátula.
- **Fuera de casa**: la consola solo necesita llegar a la dirección que conectaste. En casa basta la IP local. Para escuchar fuera, publica el servidor detrás de un proxy inverso con **HTTPS** (Caddy, nginx, un Cloudflare Tunnel…) o una VPN como Tailscale, y conecta esa dirección. Evita abrir el puerto del servidor a internet por **HTTP** sin cifrar: el token y tu música viajarían a la vista.

### Bandcamp
La tarjeta **Bandcamp** es para escuchar y descubrir, sin cuenta: **Buscar** (canciones o álbumes), **Populares**, **Destacados** (álbumes) y **Novedades**, del Descubrir de Bandcamp. Suena el mismo MP3 de 128 kbps que Bandcamp deja escuchar a cualquiera; las canciones que el artista solo permite comprando se saltan.
- **Sin descargas**: Bandcamp vive de que la gente compre música, así que Sound3DS no descarga de ahí. Compra el álbum en Bandcamp si quieres tenerlo (después puedes pasarlo a la consola con Share).
- **Cuenta (experimental)**: Share puede conectar tu cuenta de Bandcamp (**Conectar Bandcamp**) y entonces la pestaña del medio pasa a ser **Tu colección**. En nuestras pruebas no funcionó de forma confiable (la colección puede aparecer vacía), así que tómalo como experimental.
- Bandcamp no tiene una API pública para escuchar: Sound3DS usa las mismas rutas que su web, así que podría dejar de funcionar si Bandcamp las cambia.

---

## 📲 Sound3DS Share: conectar tu cuenta y pasar música desde el PC
<p align="center">
  <a href="https://github.com/PeppiDev/sound3ds-share">
    <img src="https://raw.githubusercontent.com/PeppiDev/sound3ds-share/main/docs/share-pc.png" alt="Sound3DS Share en el PC" width="640">
  </a>
</p>

**[Sound3DS Share](https://github.com/PeppiDev/sound3ds-share)** es la app compañera para el PC (Windows o Linux). **Es necesaria para conectar tu cuenta de SoundCloud** (pasos en [Cuenta de SoundCloud](#cuenta-de-soundcloud)) y además sirve para **llenar el modo Local sin sacar la tarjeta SD**:

1. Abre **Sound3DS Share**, explora tus carpetas y elige canciones. Se agrupan por álbum con su carátula y etiquetas, y puedes corregir lo que falte (álbum, artista, número de pista, carátula) antes de enviar.
2. Toca **Generar QR**.
3. En la consola abre **Configuración → Recibir desde el PC** y escanéalo: las canciones se descargan por Wi-Fi a `sdmc:/music` en segundo plano (sin cortar la música) y aparecen en **Local**, cada una en su álbum del cover flow.

Las descargas e instrucciones de Sound3DS Share están en su repositorio: **https://github.com/PeppiDev/sound3ds-share** (la app Sound3DS QR aparte sigue funcionando, pero ya no hace falta).

---

## 🛠️ Compilación y Ejecución

El proyecto utiliza un contenedor aislado con **Podman** y el toolchain oficial `devkitpro/devkitarm`.

### 1. Compilar el binario `.3dsx`
```bash
./scripts/build.sh
```

### 2. Ejecutar en el emulador Azahar (Citra Fork)
```bash
./scripts/run_azahar.sh
```

### 3. Probar en el PC el camino HLS/AAC
```bash
tests/host/run.sh <carpeta con playlist.m3u8, init.mp4 y seg0.m4s, seg1.m4s...>
```
Compila el demuxer y Helix AAC para el PC, decodifica los segmentos y compara el resultado con ffmpeg (debe superar 40 dB de SNR).

### 4. Generar el `.cia`
```bash
./scripts/build_cia.sh
```
Necesita en el equipo `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR)) y `bannertool`; por defecto los busca en `~/.local/share/3ds-tools/` (se pueden indicar con `MAKEROM=` y `BANNERTOOL=`). El ícono, el banner y su sonido están en `cia/`.

### 5. Instalar en tu consola física (Old 3DS / 2DS / New 3DS)
- **Como `.cia`**: sirve el archivo con un servidor local (por ejemplo `serve_cia.py`) y, en FBI, usa *Remote Install → Scan QR Code*. Queda en el menú HOME.
- **Como `.3dsx`**: copia `sound3ds.3dsx` y `sound3ds.smdh` a `sdmc:/3ds/sound3ds/` y ábrelo desde el **Homebrew Launcher**.

Los binarios no se guardan en el repositorio; cada versión se publica en GitHub Releases.

---

## 📌 Estado Actual y Próximos Pasos

- Probado en hardware real: Old 2DS, New 3DS XL y New 2DS XL (Luma3DS). Falta probar el respaldo CSND en consola (Azahar no lo reproduce).
- El streaming soporta MP3 progresivo y HLS con AAC-LC en fMP4. Las pistas cifradas (DRM) de SoundCloud no se pueden reproducir.
- Carga del decodificador AAC medida por un usuario en una Old 3DS real: estable en 12-13 % (la pantalla superior la muestra en porcentaje).
- FLAC en una Old 2DS: 16-bit / 44,1 kHz y 24-bit / 96 kHz suenan bien (se decodifica en el segundo núcleo); 192 kHz no alcanza.
- La colección de la cuenta de Bandcamp es experimental y no apareció en nuestras pruebas.
- El `client_id` de SoundCloud está fijo en el código y la verificación SSL está desactivada; se revisarán al evaluar proveedores.
- La evaluación de migración a YouTube Music está en [`docs/YOUTUBE_MUSIC_EVALUATION.md`](docs/YOUTUBE_MUSIC_EVALUATION.md).

## 🤖 Uso de IA

Este proyecto se hizo con ayuda de IA (Claude).

## 🙌 Créditos y licencias de terceros

- [minimp3](https://github.com/lieff/minimp3) (CC0) para MP3, [Helix AAC](https://github.com/pschatzmann/arduino-libhelix) de RealNetworks (RPSL) para AAC, [stb_image](https://github.com/nothings/stb) (dominio público / MIT) para carátulas , [quirc](https://github.com/dlbeer/quirc) (ISC) para leer QR y [dr_flac](https://github.com/mackron/dr_libs) (dominio público / MIT-0) para FLAC.
- [devkitPro](https://devkitpro.org): libctru, citro2d/citro3d, libcurl, mbedTLS y jansson.
- Los textos de las licencias de terceros están en [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## 📄 Licencia

Sound3DS se publica bajo la [licencia MIT](LICENSE). Los componentes de terceros mantienen sus propias licencias (ver [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)); el decodificador Helix AAC de `source/helix-aac/` está bajo la RealNetworks Public Source License.

## Versionado y notas de cambios

Consulta [`CHANGELOG.md`](CHANGELOG.md) para el historial. Esta versión registra el trabajo de audio, el cambio de pista y el diagnóstico de errores; no declara compatibilidad universal de formatos ni una migración de proveedor terminada.
