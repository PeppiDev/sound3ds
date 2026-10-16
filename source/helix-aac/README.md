# Helix AAC (decodificador AAC en punto fijo)

Decodificador AAC-LC de RealNetworks (Helix DNA, 2005), distribuido bajo la
RealNetworks Public Source License (RPSL); ver la cabecera de cada archivo.

Copiado de `src/libhelix-aac` de
https://github.com/pschatzmann/arduino-libhelix (commit f6e55a3).

Cambios para Sound3DS:
- Sin los archivos `sbr*.c` / `sbr.h`: solo se usa AAC-LC (SoundCloud entrega `mp4a.40.2`).
- `assembly.h`: se habilita (e incluye `<stdlib.h>` para `abs`) el camino GCC/ARM (`smull`, `smlal`, `ssat`, `rev`), que
  arduino-libhelix desactiva para Cortex-M; el ARM11 de la 3DS (ARMv6K) los soporta.
- `fft.c` se renombró a `aacfft.c` para no chocar con el `fft.c` del visualizador.
- `compat/` reemplaza las cabeceras de Arduino (`ConfigHelix.h`, `utils/helix_pgm.h`,
  `utils/helix_memory.h`) por equivalentes estándar.
