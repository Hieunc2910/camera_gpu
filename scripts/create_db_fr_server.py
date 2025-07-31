import subprocess
import requests
import json
import sqlite3
from datetime import datetime
import os

SYNC_URL = "https://topcam.ai.vn/apis/syncStudentsAPI"
LAST_UPDATED_URL = "https://topcam.ai.vn/apis/lastUpdatedStudentAPI"
TOKEN = "P8Hg4ukCiRI3NUMDvKmscEFnL0OtzT1752552517"
DB_FILE = "./students_local.db"

def initialize_database():
    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute(
            """
            CREATE TABLE IF NOT EXISTS students
            (
                id INTEGER PRIMARY KEY,
                label TEXT,
                vector_face TEXT
            )
            """
        )
        conn.commit()
        print(f"Database đã được khởi tạo thành công tại '{DB_FILE}'")
        conn.close()
        return True
    except sqlite3.Error as e:
        print(f"Lỗi khi khởi tạo database: {e}")
        return False

def save_to_sqlite(students):
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
                s.get("label"),
                s.get("vector_face")

            )
            for s in students
        ]
        c.executemany(
            "INSERT INTO students (id, label, vector_face) VALUES (?, ?, ?)",
            student_data
        )
        conn.commit()
        conn.close()
        print(f"Đã lưu thành công {len(students)} học sinh vào database cục bộ '{DB_FILE}'")
    except sqlite3.Error as e:
        print(f"Lỗi khi lưu vào SQLite: {e}")

def get_last_updated_local():
    try:
        conn = sqlite3.connect(DB_FILE)
        c = conn.cursor()
        c.execute("SELECT MAX(id) FROM students")
        max_id = c.fetchone()[0]
        conn.close()
        return max_id
    except Exception:
        return None

def fetch_last_updated_server():
    try:
        headers = {"Authorization": f"Bearer {TOKEN}"}
        response = requests.post(LAST_UPDATED_URL, timeout=10)
        response.raise_for_status()
        data = response.json()
        # If data["data"] is a list, handle accordingly
        data_field = data.get("data")
        if isinstance(data_field, list) and data_field:
            last_updated = data_field[0].get("last_updated_at")
        elif isinstance(data_field, dict):
            last_updated = data_field.get("last_updated_at")
        else:
            last_updated = None
        print(f"Thời gian cập nhật gần nhất trên server: {last_updated}")
        return last_updated
    except Exception as e:
        print(f"Lỗi khi lấy thời gian cập nhật từ server: {e}")
        return None

def fetch_students():
    try:
        headers = {"Authorization": f"Bearer {TOKEN}"}
        print(f"Đang kết nối tới server: {SYNC_URL}")
        response = requests.post(SYNC_URL, json={"token": TOKEN}, timeout=30)
        response.raise_for_status()
        data = response.json()

        students_data = data.get("data", [])
        if isinstance(students_data, dict):
            students = [students_data]
        elif isinstance(students_data, list):
            students = students_data
        else:
            students = []

        # Filter only id, label, vector_face
        filtered_students = [
            {
                "id": s.get("id"),
                "label": s.get("label"),
                "vector_face": s.get("vector_face"),
            }
            for s in students
        ]

        if not filtered_students:
            print("Không nhận được dữ liệu học sinh từ server.")
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

if __name__ == "__main__":
    # Bước 1: Khởi tạo và đảm bảo database đã sẵn sàng.
    if initialize_database():
        # Bước 2: Kiểm tra thời gian cập nhật gần nhất trên server
        last_updated_server = fetch_last_updated_server()

        # Bước 3: Nếu cần đồng bộ, tiến hành lấy dữ liệu.
        fetch_students()

        # Bước 4: Gọi tiếp script populate_faiss_from_db.py
        try:
            print("Đang cập nhật FAISS index từ database...")
            subprocess.run(
                ["python3", "./scripts/populate_faiss_from_db.py"],
                check=True
            )
        except Exception as e:
            print(f"Lỗi khi gọi populate_faiss_from_db.py: {e}")
