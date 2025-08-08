import sys
import subprocess
import re

def get_ip_from_rtsp(rtsp_url):
    match = re.search(r"rtsp://(?:.*@)?([\d\.]+):", rtsp_url)
    return match.group(1) if match else None

def get_mac_from_ip(ip):
    try:
        # Ping để IP xuất hiện trong bảng ARP
        subprocess.run(["ping", "-n", "1", ip], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Lấy bảng ARP
        arp_output = subprocess.check_output(["arp", "-a", ip], encoding="utf-8")
        # Tìm MAC address trong output
        match = re.search(r"([0-9a-fA-F]{2}[-:]){5}[0-9a-fA-F]{2}", arp_output)
        if match:
            return match.group(0).replace("-", "").replace(":", "")
    except Exception as e:
        print(f"Lỗi khi lấy MAC từ IP: {e}")
    return None

def generate_rtmp_url(mac_address):
    return f"rtmp://103.74.123.202:1935/live/topcam_{mac_address}?vhost=stream.topcam.ai.vn"

def update_config_file(config_path):
    with open(config_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Tìm RTSP URL trong file config
    rtsp_url = None
    for line in lines:
        if line.strip().startswith("uri=rtsp://"):
            rtsp_url = line.strip().split("=", 1)[1]
            break

    if not rtsp_url:
        print("Không tìm thấy RTSP URL trong file config.")
        return

    ip = get_ip_from_rtsp(rtsp_url)
    if not ip:
        print("Không lấy được IP từ RTSP URL.")
        return

    mac = get_mac_from_ip(ip)
    if not mac:
        print("Không lấy được MAC từ IP camera.")
        return

    rtmp_url = generate_rtmp_url(mac)
    updated = False

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
