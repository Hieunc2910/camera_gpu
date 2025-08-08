import json
import base64
from PIL import Image
import numpy as np

with open('log.json', 'r') as f:
    logs = json.load(f)

def parse_time(s):
    from datetime import datetime
    try:
        return datetime.strptime(s, "%d-%m-%Y %H:%M:%S")
    except:
        return None

def parse_image_header(base64_data):
    """Parse header từ base64 string"""
    try:
        if ':' in base64_data:
            parts = base64_data.split(':', 3)
            if len(parts) >= 4:
                width = int(parts[0])
                height = int(parts[1])
                mode = parts[2]
                data = parts[3]
                return width, height, mode, data
        return None, None, None, base64_data
    except Exception as e:
        print(f"[ERROR] Parse header failed: {e}")
        return None, None, None, base64_data


latest_entry = None
latest_time = None
for entry in logs:
    ts = entry.get('timestamp', '')
    t = parse_time(ts)
    if t and (latest_time is None or t > latest_time):
        latest_time = t
        latest_entry = entry

if latest_entry:
    b64 = latest_entry.get('face_image', '')
    ts = latest_entry.get('timestamp', '')
    sid = latest_entry.get('student_id', '')

    if b64:
        try:
            width, height, mode, image_data = parse_image_header(b64)

            if width and height:
                print(f"[INFO] Detected image: {width}x{height}, mode: {mode}")

                # Decode base64
                img_bytes = base64.b64decode(image_data)
                print(f"[DEBUG] Data length: {len(img_bytes)}, Expected: {width * height * 4}")

                img = Image.frombytes('RGBA', (width, height), img_bytes)
                img = img.convert('RGB')

                out_name = f'output_face_{sid}_{ts.replace(":", "-").replace(" ", "_")}.png'
                img.save(out_name)
                img.show()

        except Exception as e:
            print(f"Lỗi: {e}")
    else:
        print("Không có ảnh base64.")
else:
    print("Không tìm thấy entry hợp lệ.")
