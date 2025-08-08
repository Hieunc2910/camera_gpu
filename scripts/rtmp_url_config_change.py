import uuid
import sys

def get_mac_address():
    mac = uuid.getnode()
    mac_str = ''.join(['{:02x}'.format((mac >> ele) & 0xff) for ele in range(40, -1, -8)])
    return mac_str

def generate_rtmp_url(mac_address):
    return f"rtmp://103.74.123.202:1935/live/topcam_{mac_address}?vhost=stream.topcam.ai.vn"

def update_config_file(config_path):
    mac = get_mac_address()
    rtmp_url = generate_rtmp_url(mac)
    updated = False

    with open(config_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    for i, line in enumerate(lines):
        if line.strip().startswith("rtmp-url="):
            lines[i] = f"rtmp-url={rtmp_url}\n"
            updated = True

    if updated:
        with open(config_path, "w", encoding="utf-8") as f:
            f.writelines(lines)
        print(f"✅ Đã cập nhật rtmp-url trong {config_path}: {rtmp_url}")
    else:
        print("⚠️ Không tìm thấy dòng rtmp-url trong file config.")

if __name__ == "__main__":
    config_path = sys.argv[1] if len(sys.argv) > 1 else "./samples/configs/deepstream_app.txt"
    update_config_file(config_path)