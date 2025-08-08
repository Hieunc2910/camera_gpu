
def init_face_recognition_database(db_path="../data/face_recognition.db"):
    import sqlite3, os

    try:
        db_dir = os.path.dirname(os.path.abspath(db_path))
        if not os.path.exists(db_dir) and db_dir:
            os.makedirs(db_dir)
            print(f"✅ Created directory: {db_dir}")

        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        print(f"🗄️ Initializing database: {os.path.abspath(db_path)}")

        # 1. users table
        cursor.execute(
            "CREATE TABLE IF NOT EXISTS users ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT UNIQUE NOT NULL, "
            "full_name TEXT, "
            "department TEXT, "
            "role TEXT, "
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "is_active BOOLEAN DEFAULT 1);"
        )
        print("✅ Created 'users' table")

        # 2. face_encodings table
        cursor.execute(
            "CREATE TABLE IF NOT EXISTS face_encodings ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "encoding BLOB NOT NULL, "
            "user_id INTEGER, "
            "image_path TEXT, "
            "quality_score REAL DEFAULT 0.0, "
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE);"
        )
        print("✅ Created 'face_encodings' table")

        # 3. recognition_log table
        cursor.execute(
            "CREATE TABLE IF NOT EXISTS recognition_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "similarity REAL NOT NULL, "
            "confidence REAL, "
            "detection_source TEXT DEFAULT 'camera', "
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "session_id TEXT, "
            "bbox_coords TEXT, "
            "user_id INTEGER, "
            "FOREIGN KEY(user_id) REFERENCES users(id));"
        )
        print("✅ Created 'recognition_log' table")

        # 4. access_sessions table
        cursor.execute(
            "CREATE TABLE IF NOT EXISTS access_sessions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "start_time DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "end_time DATETIME, "
            "duration_minutes REAL, "
            "status TEXT DEFAULT 'ACTIVE', "
            "entry_count INTEGER DEFAULT 1, "
            "last_seen DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "session_id TEXT UNIQUE, "
            "user_id INTEGER, "
            "FOREIGN KEY(user_id) REFERENCES users(id));"
        )
        print("✅ Created 'access_sessions' table")

        # Indexes
        indexes = [
            "CREATE INDEX IF NOT EXISTS idx_face_encodings_name ON face_encodings(name);",
            "CREATE INDEX IF NOT EXISTS idx_recognition_log_timestamp ON recognition_log(timestamp);",
            "CREATE INDEX IF NOT EXISTS idx_recognition_log_name ON recognition_log(name);",
            "CREATE INDEX IF NOT EXISTS idx_access_sessions_name ON access_sessions(name);",
            "CREATE INDEX IF NOT EXISTS idx_access_sessions_status ON access_sessions(status);",
            "CREATE INDEX IF NOT EXISTS idx_users_name ON users(name);",
            "CREATE INDEX IF NOT EXISTS idx_users_active ON users(is_active);"
        ]
        for index_sql in indexes:
            cursor.execute(index_sql)
        print("✅ Created indexes")

        # View
        cursor.execute(
            "CREATE VIEW IF NOT EXISTS user_stats AS "
            "SELECT u.id, u.name, u.full_name, u.department, "
            "COUNT(DISTINCT fe.id) as encoding_count, "
            "COUNT(DISTINCT rl.id) as recognition_count, "
            "MAX(rl.timestamp) as last_recognition, u.is_active "
            "FROM users u "
            "LEFT JOIN face_encodings fe ON u.id = fe.user_id "
            "LEFT JOIN recognition_log rl ON u.id = rl.user_id "
            "GROUP BY u.id, u.name, u.full_name, u.department, u.is_active;"
        )
        print("✅ Created view 'user_stats'")

        # Sample data
        cursor.execute("SELECT COUNT(*) FROM users")
        if cursor.fetchone()[0] == 0:
            sample_users = [
                ('admin', 'Administrator', 'IT', 'admin'),
                ('guest', 'Guest User', 'General', 'user'),
                ('Unknown', 'Unknown Person', 'N/A', 'unknown')
            ]
            cursor.executemany(
                "INSERT INTO users (name, full_name, department, role) VALUES (?, ?, ?, ?)",
                sample_users
            )
            print("✅ Added sample data")

        conn.commit()
        conn.close()
        print(f"✅ Database initialized: {db_path}")
        return True

    except Exception as e:
        print(f"❌ Database initialization error: {e}")
        return False
if __name__ == "__main__":
    init_face_recognition_database()