import requests
import json
import time
import random
import sqlite3
from datetime import datetime
import os

# Sử dụng API mới
SERVER_URL = "https://topcam.ai.vn/apis/syncStudentsAPI"
LAST_UPDATED_URL = "https://topcam.ai.vn/apis/lastUpdatedStudentAPI"
TOKEN = "P8Hg4ukCiRI3NUMDvKmscEFnL0OtzT1752552517"
DB_FILE = "../students_local.db"


def initialize_database():
    """
    Hàm này đảm bảo file database và bảng 'students' tồn tại.
    Nó sẽ tự động tạo file và bảng nếu chúng chưa có.
    """
    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute("""
                  CREATE TABLE IF NOT EXISTS students (
                      id INTEGER PRIMARY KEY,
                      full_name TEXT NOT NULL,
                      code_student TEXT,
                      vector_face TEXT,
                      updated_at TEXT
                  )
                  """)
        conn.commit()
        conn.close()
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
        c.execute("DELETE FROM students")
        student_data = [
            (
                s.get("id"),
                s.get("full_name"),
                s.get("code_student"),
                s.get("vector_face"),
                s.get("updated_at")
            )
            for s in students
        ]
        c.executemany(
            "INSERT INTO students (id, full_name, code_student, vector_face, updated_at) VALUES (?, ?, ?, ?, ?)",
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
        delay = random.randint(0, 10)
        print(f"Đợi {delay} giây trước khi lấy danh sách từ server...")
        time.sleep(delay)

        print(f"Đang lấy thời gian cập nhật gần nhất từ: {LAST_UPDATED_URL}")
        last_updated_resp = requests.get(LAST_UPDATED_URL, timeout=15)
        last_updated_resp.raise_for_status()
        last_updated_data = last_updated_resp.json()
        # After last_updated_data = last_updated_resp.json()
        print(f"last_updated_data type: {type(last_updated_data)}, value: {last_updated_data}")

        if isinstance(last_updated_data, dict):
            data_field = last_updated_data.get("data", {})
            if isinstance(data_field, dict):
                last_updated_at = data_field.get("last_updated_at")
            elif isinstance(data_field, list) and data_field:
                last_updated_at = data_field[0].get("last_updated_at")
            else:
                last_updated_at = None
        else:
            last_updated_at = None
        print(f"Thời gian cập nhật gần nhất: {last_updated_at}")

        print(f"Đang kết nối tới server: {SERVER_URL}")
        response = requests.post(SERVER_URL, data={"token": TOKEN}, timeout=15)
        response.raise_for_status()
        data = response.json()
        if data.get("code") != 1:
            print(f"API trả về không thành công: {data.get('mess', 'Không có thông báo lỗi')}")
            return

        students = data.get("data", [])

        filtered_students = [
            s for s in students if s.get("id") and s.get("full_name")
        ]

        if not filtered_students:
            print("Không nhận được dữ liệu học sinh hợp lệ từ server.")
            return

        save_to_sqlite(filtered_students)

    except requests.exceptions.Timeout:
        print("Lỗi: Hết thời gian chờ khi kết nối đến server.")
    except requests.exceptions.RequestException as e:
        print(f"Lỗi mạng hoặc kết nối khi lấy danh sách: {e}")
    except json.JSONDecodeError:
        print("Lỗi: Không thể phân tích dữ liệu JSON từ server.")
    except Exception as e:
        print(f"Đã xảy ra lỗi không xác định: {e}")

def show_database():
    """Hiển thị toàn bộ dữ liệu trong bảng students."""
    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute("SELECT * FROM students")
        rows = c.fetchall()
        conn.close()
        print("Dữ liệu hiện tại trong bảng students:")
        for row in rows:
            print(row)
    except sqlite3.Error as e:
        print(f"Lỗi khi đọc database: {e}")

if __name__ == "__main__":
    if initialize_database():
        fetch_students()
    show_database()

