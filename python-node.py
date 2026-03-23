import io
import base64
import hashlib
import zlib
import struct
import json

def crc32(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF

def create_png(width, height, pixels):
    def png_chunk(chunk_type, data):
        chunk_len = struct.pack('>I', len(data))
        chunk_crc = struct.pack('>I', crc32(chunk_type + data))
        return chunk_len + chunk_type + data + chunk_crc
    
    signature = b'\x89PNG\r\n\x1a\n'
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    ihdr = png_chunk(b'IHDR', ihdr_data)
    
    raw_data = b''
    for row in pixels:
        raw_data += b'\x00' + bytes(row)
    
    compressed = zlib.compress(raw_data, 9)
    idat = png_chunk(b'IDAT', compressed)
    iend = png_chunk(b'IEND', b'')
    
    return signature + ihdr + idat + iend

width = 800
height = 480

pixels = []
for y in range(height):
    row = []
    for x in range(width):
        row.extend([255, 255, 255])
    pixels.append(row)

rect_w = 300
rect_h = 50
rect_x = (width - rect_w) // 2
rect_y = (height - rect_h) // 2

for y in range(rect_y, rect_y + rect_h):
    for x in range(rect_x, rect_x + rect_w):
        pixels[y][x*3:x*3+3] = [0, 0, 0]

png_data = create_png(width, height, pixels)
sha256_hash = hashlib.sha256(png_data).hexdigest()
base64_image = base64.b64encode(png_data).decode('utf-8')

result = {"image": base64_image, "checksum": sha256_hash}
print(json.dumps(result))
