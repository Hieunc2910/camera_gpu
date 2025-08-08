import sys
import subprocess
import re

def get_ip_from_rtsp(rtsp_url):
    match = re.search(r"rtsp://(?:.*@)?([\d\.]+):", rtsp_url)
    return match.group(1) if match else None

def get_mac_from_ip(ip):
    try:
        subprocess.run(["ping", "-n", "1", ip], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        arp_output = subprocess.check_output(["arp", "-a", ip], encoding="utf-8")
        match = re.search(r"([0-9a-fA-F]{2}[-:]){5}[0-9a-fA-F]{2}", arp_output)
        if match:
            return match.group(0).replace("-", "").replace(":", "")
    except Exception as e:
        print(f"Lỗi khi lấy MAC từ IP: {e}")
    return None

def generate_rtmp_url(mac_address):
    return f"rtmp://103.74.123.202:1935/live/topcam_{mac_address}?vhost=stream.topcam.ai.vn"

def parse_sources(lines):
    """Trả về dict: source_id -> rtsp_url"""
    sources = {}
    current_section = None
    for line in lines:
        section = re.match(r"\[(source\d+)\]", line.strip())
        if section:
            current_section = section.group(1)
            continue
        if current_section and line.strip().startswith("uri=rtsp://"):
            sources[current_section] = line.strip().split("=", 1)[1]
    return sources

def update_config_file(config_path):
    with open(config_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Bước 1: Lấy mapping source_id -> rtsp_url
    sources = parse_sources(lines)  # {'source1': 'rtsp://...', ...}

    # Bước 2: Duyệt từng section [sinkX], tìm source-id, cập nhật đúng rtmp-url
    new_lines = []
    in_sink = False
    sink_source_id = None
    sink_section = None

    for idx, line in enumerate(lines):
        section = re.match(r"\[(sink\d+)\]", line.strip())
        if section:
            in_sink = True
            sink_section = section.group(1)
            sink_source_id = None
        elif re.match(r"\[.*\]", line.strip()):
            in_sink = False
            sink_section = None
            sink_source_id = None

        if in_sink and line.strip().startswith("source-id="):
            sink_source_id = int(line.strip().split("=", 1)[1])

        # Nếu đang trong section sink, có source-id, gặp rtmp-url thì cập nhật
        if in_sink and sink_source_id is not None and line.strip().startswith("rtmp-url="):
            source_key = f"source{sink_source_id}"
            rtsp_url = sources.get(source_key)
            if rtsp_url:
                ip = get_ip_from_rtsp(rtsp_url)
                if ip:
                    mac = get_mac_from_ip(ip)
                    if mac:
                        rtmp_url = generate_rtmp_url(mac)
                        print(f"Đã cập nhật {sink_section} (source-id={sink_source_id}) với MAC {mac}: {rtmp_url}")
                        line = f"rtmp-url={rtmp_url}\n"
                    else:
                        print(f"Không lấy được MAC cho {sink_section} (source-id={sink_source_id})")
                else:
                    print(f"Không lấy được IP cho {sink_section} (source-id={sink_source_id})")
            else:
                print(f"Không tìm thấy RTSP URL cho {sink_section} (source-id={sink_source_id})")
        new_lines.append(line)

    with open(config_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)

if __name__ == "__main__":
    config_path = sys.argv[1] if len(sys.argv) > 1 else "./samples/configs/deepstream_app.txt"
    update_config_file(config_path)