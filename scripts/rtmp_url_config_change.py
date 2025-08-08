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
            mac = match.group(0).replace("-", "").replace(":", "")
            # Định dạng lại thành hex có dấu _
            mac = mac.lower()
            mac = "_".join([mac[i:i+2] for i in range(0, 12, 2)])
            return mac
    except Exception as e:
        print(f"Lỗi khi lấy MAC từ IP: {e}")
    return None

def generate_rtmp_url(mac_address):
    return f"rtmp://103.74.123.202:1935/live/topcam_{mac_address}?vhost=stream.topcam.ai.vn"

def parse_enabled_sources(lines):
    """
    Trả về danh sách các source section đang enable, theo thứ tự xuất hiện.
    Mỗi phần tử là tuple (section_name, rtsp_url)
    """
    enabled_sources = []
    current_section = None
    enable_flag = False
    rtsp_url = None
    for line in lines:
        section = re.match(r"\[(source\d+)\]", line.strip())
        if section:
            if current_section and enable_flag and rtsp_url:
                enabled_sources.append((current_section, rtsp_url))
            current_section = section.group(1)
            enable_flag = False
            rtsp_url = None
            continue
        if current_section:
            if line.strip().startswith("enable="):
                enable_flag = line.strip().split("=", 1)[1].strip() == "1"
            if line.strip().startswith("uri=rtsp://"):
                rtsp_url = line.strip().split("=", 1)[1]
    # Check last section
    if current_section and enable_flag and rtsp_url:
        enabled_sources.append((current_section, rtsp_url))
    return enabled_sources

def update_config_file(config_path):
    with open(config_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Lấy danh sách các source enable, theo thứ tự
    enabled_sources = parse_enabled_sources(lines)  # [(section_name, rtsp_url), ...]
    # Mapping: source_id (theo thứ tự enable) -> (section_name, rtsp_url)
    source_id_map = {i: src for i, src in enumerate(enabled_sources)}

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
            try:
                sink_source_id = int(line.strip().split("=", 1)[1])
            except Exception:
                sink_source_id = None

        # Nếu đang trong section sink, có source-id, gặp rtmp-url thì cập nhật
        if in_sink and sink_source_id is not None and line.strip().startswith("rtmp-url="):
            src_info = source_id_map.get(sink_source_id)
            if src_info:
                _, rtsp_url = src_info
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
                print(f"Không tìm thấy source enable cho {sink_section} (source-id={sink_source_id})")
        new_lines.append(line)

    with open(config_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)

if __name__ == "__main__":
    config_path = sys.argv[1] if len(sys.argv) > 1 else "./samples/configs/deepstream_app.txt"
    update_config_file(config_path)
