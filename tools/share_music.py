#!/usr/bin/env python3
"""Comparte canciones del PC con la app Sound3DS QR.

Uso:
    python3 tools/share_music.py cancion.mp3 otra.mp3 "carpeta con música/"

Sirve los archivos .mp3/.aac por HTTP en la red local y muestra un QR. En la
consola, abre Sound3DS QR y escanéalo: las canciones se descargan a
sdmc:/music y aparecen en Sound3DS > Local.

El QR apunta a una lista (/sound3ds.txt) con una URL por canción. Solo se
sirven los archivos indicados, nada más del equipo.
"""
import http.server
import os
import shutil
import socket
import subprocess
import sys
import urllib.parse

PORT = int(os.environ.get("PORT", "8080"))
AUDIO_EXT = (".mp3", ".aac")


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 1))   # no envía nada: solo elige la interfaz de la LAN
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith(AUDIO_EXT)]
        elif p.lower().endswith(AUDIO_EXT) and os.path.isfile(p):
            files.append(p)
        else:
            print(f"  (se omite {p}: no es .mp3/.aac)")
    return files


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    files = collect(sys.argv[1:])
    if not files:
        print("No hay archivos .mp3/.aac para compartir.")
        sys.exit(1)

    ip = lan_ip()
    base = f"http://{ip}:{PORT}"
    routes = {}
    for i, f in enumerate(files):
        name = os.path.basename(f)
        routes[f"/f/{i}/{urllib.parse.quote(name)}"] = f
    listing = "\n".join(base + r for r in routes) + "\n"

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/sound3ds.txt":
                body = listing.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                print(f"  {self.client_address[0]} leyó la lista ({len(files)} canciones)")
                return
            f = routes.get(path)
            if not f:
                self.send_error(404)
                return
            size = os.path.getsize(f)
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg" if f.lower().endswith(".mp3") else "audio/aac")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(f, "rb") as fh:
                shutil.copyfileobj(fh, self.wfile, 64 * 1024)
            print(f"  enviado: {os.path.basename(f)} ({size // 1024} KB)")

        def log_message(self, *args):
            pass

    url = base + "/sound3ds.txt"
    png = os.path.join(os.path.dirname(os.path.abspath(__file__)), "share_qr.png")
    print(f"\nCompartiendo {len(files)} canciones en {url}\n")
    if shutil.which("qrencode"):
        subprocess.run(["qrencode", "-t", "ANSIUTF8", "-m", "2", url])
        subprocess.run(["qrencode", "-s", "10", "-m", "3", "-o", png, url])
        print(f"QR también guardado en: {png}")
    else:
        print("Instala qrencode para ver el QR aquí; mientras, genera uno con esta URL.")
    print("\nEn la consola: Sound3DS QR -> apunta al QR.  Ctrl+C para terminar.\n")
    try:
        http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nListo.")


if __name__ == "__main__":
    main()
