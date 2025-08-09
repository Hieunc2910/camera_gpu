import sys
import subprocess
import re
import platform
import uuid
from typing import Dict, Optional


def get_ip_from_rtsp(rtsp_url: str) -> Optional[str]:
    """Extract IP address from RTSP URL"""
    match = re.search(r"rtsp://(?:.*@)?([\d\.]+):", rtsp_url)
    return match.group(1) if match else None


def get_mac_from_ip(ip: str) -> Optional[str]:
    """Get MAC address from IP using ARP table"""
    try:
        # Ping to ensure IP is in ARP table
        ping_cmd = ["ping", "-c", "1", ip] if platform.system() != "Windows" else ["ping", "-n", "1", ip]
        subprocess.run(ping_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)

        # Get ARP entry
        arp_cmd = ["arp", "-n", ip] if platform.system() != "Windows" else ["arp", "-a", ip]
        arp_output = subprocess.check_output(arp_cmd, encoding="utf-8", timeout=5)

        # More robust MAC address regex that handles different formats
        mac_patterns = [
            r"([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}",  # Standard format
            r"([0-9a-fA-F]{2}\.){5}[0-9a-fA-F]{2}",  # Cisco format
        ]

        for pattern in mac_patterns:
            match = re.search(pattern, arp_output)
            if match:
                mac = match.group(0).replace("-", "").replace(":", "").replace(".", "")
                mac = mac.lower()
                mac = "_".join([mac[i:i + 2] for i in range(0, 12, 2)])
                return mac

    except subprocess.TimeoutExpired:
        print(f"Timeout khi ping/arp IP {ip}")
    except subprocess.CalledProcessError as e:
        print(f"Lỗi khi chạy lệnh cho IP {ip}: {e}")
    except Exception as e:
        print(f"Lỗi không xác định khi lấy MAC từ IP {ip}: {e}")

    return None


def get_local_mac_address() -> Optional[str]:
    """Get the MAC address of the local machine"""
    try:
        # Get MAC address using uuid.getnode()
        mac = uuid.getnode()
        # Convert to hex and format as expected
        mac_hex = f"{mac:012x}"
        # Format as expected (with underscores between pairs)
        mac_formatted = "_".join([mac_hex[i:i+2] for i in range(0, 12, 2)])
        return mac_formatted.lower()
    except Exception as e:
        print(f"Lỗi khi lấy MAC address của máy local: {e}")
        return None


def generate_rtmp_url(mac_address: str, sink_id: Optional[str] = None) -> str:
    """Generate RTMP URL from MAC address, optionally with sink identifier"""
    base_url = f"rtmp://103.74.123.202:1935/live/topcam_{mac_address}"
    if sink_id:
        base_url += f"_{sink_id}"
    return base_url + "?vhost=stream.topcam.ai.vn"


def generate_rtmp_url_for_v4l2(dev_node: str, sink_id: Optional[str] = None) -> str:
    """Generate RTMP URL for V4L2 source using local PC's MAC address"""
    # Get the actual MAC address of the local PC
    local_mac = get_local_mac_address()
    if local_mac:
        base_url = f"rtmp://103.74.123.202:1935/live/topcam_{local_mac}"
    else:
        # Fallback to generic identifier if MAC can't be retrieved
        base_url = f"rtmp://103.74.123.202:1935/live/topcam_v4l2Mac"

    if sink_id:
        base_url += f"_{sink_id}"
    return base_url + "?vhost=stream.topcam.ai.vn"


def parse_enabled_sources(lines: list) -> Dict[int, dict]:
    """
    Parse config file to find enabled sources and their info.
    Returns a dict mapping source section number to its info dict:
    {idx: {"type": <int>, "uri": <str>, "dev_node": <str>}}
    """
    enabled_sources = {}
    current_section = None
    enable_flag = False
    source_type = None
    rtsp_url = None
    dev_node = None

    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):  # Skip empty lines and comments
            continue

        section = re.match(r"\[(source(\d+))\]", line)
        if section:
            # Save previous section if it was enabled
            if current_section and enable_flag:
                idx = int(current_section.replace("source", ""))
                enabled_sources[idx] = {
                    "type": source_type,
                    "uri": rtsp_url,
                    "dev_node": dev_node
                }

            # Reset for new section
            current_section = section.group(1)
            enable_flag = False
            source_type = None
            rtsp_url = None
            dev_node = None
            continue

        # Check if we're in a source section
        if current_section:
            if line.startswith("enable="):
                enable_flag = line.split("=", 1)[1].strip() == "1"
            elif line.startswith("type="):
                try:
                    source_type = int(line.split("=", 1)[1].strip())
                except ValueError:
                    source_type = None
            elif line.startswith("uri="):
                rtsp_url = line.split("=", 1)[1].strip()
                if "rtsp://" in rtsp_url:
                    print(f"📡 Found RTSP URL in {current_section}: {rtsp_url}")
            elif line.startswith("camera-v4l2-dev-node="):
                dev_node = line.split("=", 1)[1].strip()
                print(f"📹 Found V4L2 dev node in {current_section}: {dev_node}")

    # Don't forget the last section
    if current_section and enable_flag:
        idx = int(current_section.replace("source", ""))
        enabled_sources[idx] = {
            "type": source_type,
            "uri": rtsp_url,
            "dev_node": dev_node
        }

    return enabled_sources


def update_config_file(config_path: str) -> None:
    """Update config file with generated RTMP URLs"""
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Không tìm thấy file config: {config_path}")
        return
    except Exception as e:
        print(f"Lỗi khi đọc file config: {e}")
        return

    # Get mapping of enabled sources
    enabled_sources = parse_enabled_sources(lines)
    print(f"Tìm thấy {len(enabled_sources)} source được enable")

    # Cache MAC addresses to avoid repeated ARP lookups
    ip_to_mac_cache = {}

    new_lines = []
    in_sink = False
    sink_source_id = None
    sink_section = None
    updates_made = 0

    for line in lines:
        original_line = line
        line_stripped = line.strip()

        # Check for section headers
        section = re.match(r"\[(sink\d+)\]", line_stripped)
        if section:
            in_sink = True
            sink_section = section.group(1)
            sink_source_id = None
        elif re.match(r"\[.*\]", line_stripped):
            in_sink = False
            sink_section = None
            sink_source_id = None

        # Parse source-id in sink sections
        if in_sink and line_stripped.startswith("source-id="):
            try:
                sink_source_id = int(line_stripped.split("=", 1)[1])
            except (ValueError, IndexError):
                sink_source_id = None
                print(f"Invalid source-id format in {sink_section}: {line_stripped}")

        # Update rtmp-url lines
        if (in_sink and sink_source_id is not None and
            line_stripped.startswith("rtmp-url=")):

            source_info = enabled_sources.get(sink_source_id)
            if source_info:
                if source_info["type"] == 4 and source_info["uri"] and "rtsp://" in source_info["uri"]:
                    # Handle RTSP source (type 4)
                    ip = get_ip_from_rtsp(source_info["uri"])
                    if ip:
                        # Use cache to avoid repeated ARP lookups
                        if ip in ip_to_mac_cache:
                            mac = ip_to_mac_cache[ip]
                        else:
                            mac = get_mac_from_ip(ip)
                            ip_to_mac_cache[ip] = mac

                        if mac:
                            rtmp_url = generate_rtmp_url(mac)
                            print(f"✅ Updated {sink_section} (source-id={sink_source_id}) with MAC {mac}")
                            line = f"rtmp-url={rtmp_url}\n"
                            updates_made += 1
                        else:
                            print(f"❌ Could not get MAC for {sink_section} (IP: {ip})")
                    else:
                        print(f"❌ Could not extract IP from RTSP URL for {sink_section}")
                elif source_info["type"] == 1 and source_info["dev_node"] is not None:
                    # Handle V4L2 source (type 1)
                    rtmp_url = generate_rtmp_url_for_v4l2(source_info["dev_node"])
                    print(f"✅ Updated {sink_section} (source-id={sink_source_id}) with V4L2 dev node {source_info['dev_node']}")
                    line = f"rtmp-url={rtmp_url}\n"
                    updates_made += 1
                else:
                    print(f"⚠️  No valid RTSP or V4L2 source found for {sink_section} (source-id={sink_source_id})")
            else:
                print(f"⚠️  No enabled source found for {sink_section} (source-id={sink_source_id})")

        new_lines.append(line)

    # Write updated config
    try:
        with open(config_path, "w", encoding="utf-8") as f:
            f.writelines(new_lines)
        print(f"\n✅ Hoàn thành! Đã cập nhật {updates_made} RTMP URL(s)")
    except Exception as e:
        print(f"❌ Lỗi khi ghi file config: {e}")


if __name__ == "__main__":
    config_path = sys.argv[1] if len(sys.argv) > 1 else "./samples/configs/deepstream_app.txt"
    print(f"Đang xử lý file config: {config_path}")
    update_config_file(config_path)
