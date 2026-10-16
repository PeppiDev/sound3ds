# Evaluación: migración de SoundCloud a YouTube Music

**Fecha:** 2026-10-16
**Estado:** evaluación; no se ha implementado la migración.

## Resumen

No conviene reemplazar ahora el proveedor actual por YouTube Music. La documentación pública de Google describe YouTube Data API para buscar y consultar metadatos de vídeos, pero no ofrece una API pública de YouTube Music que entregue a una aplicación de terceros un flujo PCM/MP3 para decodificar y reproducir como lo necesita Sound3DS. La reproducción documentada de contenido de YouTube se realiza mediante el reproductor oficial; extraer sus URLs internas de medios no es una interfaz soportada.

Fuentes oficiales consultadas:

- [YouTube Data API: búsqueda](https://developers.google.com/youtube/v3/docs/search/list) y [recurso de vídeos](https://developers.google.com/youtube/v3/docs/videos).
- [Requisitos de funcionalidad mínima para el reproductor integrado](https://developers.google.com/youtube/terms/required-minimum-functionality).
- [Políticas para desarrolladores de YouTube API Services](https://developers.google.com/youtube/terms/developer-policies).

## Encaje técnico con 3DS/2DS

La API de datos podría ayudar a buscar títulos y obtener metadatos, sujeto a credenciales de Google Cloud, cuota y requisitos de la API. No resuelve el punto crítico: la Nintendo 3DS no dispone de un navegador moderno en el que integrar el reproductor oficial de YouTube como parte de esta interfaz nativa, y el reproductor oficial no entrega a Sound3DS PCM para alimentar el backend CSND.

Una solución basada en extraer o reconstruir URLs de reproducción internas no es una integración estable ni una API pública soportada. Además, el audio de YouTube puede usar formatos y flujos que requieren negociación, decodificación y controles distintos del MP3 progresivo que acepta actualmente el motor.

## Recomendación

Mantener el proveedor de streaming actual mientras se estabiliza y valida el pipeline MP3/CSND. Si se desea explorar YouTube en el futuro, limitar primero un prototipo a búsqueda de metadatos mediante la API oficial y evaluar una experiencia de reproducción que use el reproductor oficial en una plataforma compatible. Para 3DS, priorizar fuentes que publiquen un flujo de audio reproducible mediante una interfaz documentada y formatos que el dispositivo pueda decodificar.

## Alcance de esta conclusión

Esta es una evaluación de documentación pública y adecuación técnica, no una prueba de integración ni una afirmación de que todos los servicios tengan la misma disponibilidad en cada región. La migración no forma parte de la versión 0.2.0.
