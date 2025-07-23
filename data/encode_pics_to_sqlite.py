import sqlite3
import numpy as np
import cv2
import os

from engine_wrapper import TRTInference

def auto_encode_pics(db_path="./data/face_recognition.db", pics_folder="./data/pics"):
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    cursor.execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL);")
    cursor.execute("CREATE TABLE IF NOT EXISTS face_encodings (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, encoding BLOB NOT NULL, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);")
    cursor.execute("CREATE TABLE IF NOT EXISTS recognition_log (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, similarity REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);")
    conn.commit()

    engine_path = "./models/arcface/w600k_r50.engine"
    engine = TRTInference(engine_path)
    input_name = 'input.1'
    expected_shape = (1, 3, 112, 112)

    processed = 0
    for filename in os.listdir(pics_folder):
        if filename.lower().endswith(('.jpg', '.jpeg', '.png', '.bmp')):
            name = os.path.splitext(filename)[0]
            img_path = os.path.join(pics_folder, filename)
            img = cv2.imread(img_path)
            if img is None:
                continue
            resized = cv2.resize(img, (112, 112))
            rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
            normalized = (rgb.astype(np.float32) - 127.5) / 127.5
            input_data = np.expand_dims(np.transpose(normalized, (2, 0, 1)), axis=0)
            engine.context.set_binding_shape(engine.engine.get_binding_index(input_name), expected_shape)
            outputs = engine.infer({input_name: input_data})
            # Ensure input shape is always (1, 3, 112, 112)
            input_data = input_data.reshape(expected_shape)
            try:
                outputs = engine.infer({input_name: input_data})
                embedding = list(outputs.values())[0][0]
                embedding = embedding / np.linalg.norm(embedding)
                embedding_blob = embedding.astype(np.float32).tobytes()
                cursor.execute("INSERT OR IGNORE INTO users (name) VALUES (?)", (name,))
                cursor.execute("INSERT INTO face_encodings (name, encoding) VALUES (?, ?)", (name, embedding_blob))
                conn.commit()
                print(f"✅ {name}")
                processed += 1
            except Exception as e:
                print(f"❌ {name}: {e}")

    conn.close()
    print(f"\n🎉 Done! Processed {processed} images")
    return processed > 0

if __name__ == "__main__":
    print("🧠 Auto Encode Pictures to SQLite")
    print("=" * 40)
    success = auto_encode_pics()
    if success:
        print("✅ Database ready!")
    else:
        print("❌ No images processed!")
