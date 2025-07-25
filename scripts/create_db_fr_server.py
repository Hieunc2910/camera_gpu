import requests
import json
import time
import random
import sqlite3
from datetime import datetime
import os

SERVER_URL = "https://python.topcam.ai.vn/api/student/list"
DB_FILE = "../students_local.db"


def initialize_database():
    """
    Hàm này đảm bảo file database và bảng 'students' tồn tại.
    Nó sẽ tự động tạo file và bảng nếu chúng chưa có.
    """
    try:
        # sqlite3.connect sẽ tự động tạo file DB nếu nó chưa tồn tại.
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()

        # Lệnh 'CREATE TABLE IF NOT EXISTS' đảm bảo bảng chỉ được tạo nếu chưa có.
        c.execute("""
                  CREATE TABLE IF NOT EXISTS students (
                      id INTEGER PRIMARY KEY,
                      full_name TEXT NOT NULL,
                      vector_face TEXT
                  )
                  """)

        conn.commit()
        conn.close()
        # print(f"Cơ sở dữ liệu '{DB_FILE}' đã sẵn sàng.")
        return True
    except sqlite3.Error as e:
        print(f"Lỗi khi khởi tạo database: {e}")
        return False


def save_to_sqlite(students):
    """Lưu danh sách học sinh vào cơ sở dữ liệu SQLite cục bộ."""
    if not students:
        print("Không có học sinh nào để lưu.")
        return

    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()

        # Xóa dữ liệu cũ trước khi chèn dữ liệu mới để đồng bộ
        c.execute("DELETE FROM students")

        # Sử dụng executemany để chèn dữ liệu hiệu quả hơn
        student_data = [
            (s["id"], s["full_name"], json.dumps(s.get("vector_face")))
            for s in students
        ]
        c.executemany(
            "INSERT INTO students (id, full_name, vector_face) VALUES (?, ?, ?)",
            student_data
        )

        conn.commit()
        conn.close()
        print(f"Đã lưu thành công {len(students)} học sinh vào database cục bộ '{DB_FILE}'")

    except sqlite3.Error as e:
        print(f"Lỗi khi lưu vào SQLite: {e}")


def fetch_students():
    """Lấy danh sách học sinh từ server và lưu vào DB nếu trong khung giờ cho phép."""
    now = datetime.now()

    # # Cho phép chạy vào 1h và 13h
    # if not (now.hour == 1 or now.hour == 13):
    #     print("Không nằm trong khung giờ cập nhật (1h hoặc 13h), dừng lại.")
    #     return

    try:
        # Thêm một khoảng chờ ngẫu nhiên để tránh các request đồng thời
        delay = random.randint(0, 1000)
        print(f"Đợi {delay} giây trước khi lấy danh sách từ server...")
        time.sleep(delay)

        print(f"Đang kết nối tới server: {SERVER_URL}")
        response = requests.get(SERVER_URL, timeout=15)  # Tăng timeout
        response.raise_for_status()  # Ném lỗi nếu status code là 4xx hoặc 5xx

        data = response.json()
        if not data.get("success"):
            print(f"API trả về không thành công: {data.get('message', 'Không có thông báo lỗi')}")
            return

        students = data.get("data", [])

        # Lọc ra những học sinh có đủ thông tin id và full_name
        filtered_students = [
            s for s in students if s.get("id") and s.get("full_name")
        ]

        if not filtered_students:
            print("Không nhận được dữ liệu học sinh hợp lệ từ server.")
            return

        # Gọi hàm lưu trữ
        save_to_sqlite(filtered_students)

    except requests.exceptions.Timeout:
        print("Lỗi: Hết thời gian chờ khi kết nối đến server.")
    except requests.exceptions.RequestException as e:
        print(f"Lỗi mạng hoặc kết nối khi lấy danh sách: {e}")
    except json.JSONDecodeError:
        print("Lỗi: Không thể phân tích dữ liệu JSON từ server.")
    except Exception as e:
        print(f"Đã xảy ra lỗi không xác định: {e}")


if __name__ == "__main__":
    # Bước 1: Khởi tạo và đảm bảo database đã sẵn sàng.
    if initialize_database():
        # Bước 2: Nếu database sẵn sàng, tiến hành lấy dữ liệu.
        fetch_students()
