import sqlite3
import faiss
import numpy as np
import os
import json

def populate_faiss_from_db(db_path="students_local.db", faiss_path="faiss.index", labels_path="labels.txt"):
    """
    Reads face vectors from the students table, builds a FAISS index, and saves labels.
    """
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        cursor.execute("SELECT id, full_name, code_student, vector_face FROM students ORDER BY id ASC")
        rows = cursor.fetchall()
        conn.close()

        if not rows:
            print("No face vectors found in the database. FAISS index will not be created.")
            feature_dim = 512
            index = faiss.IndexFlatIP(feature_dim)
            faiss.write_index(index, faiss_path)
            with open(labels_path, "w") as f:
                pass
            print(f"Created empty FAISS index: {faiss_path} and labels file: {labels_path}")
            return False

        labels = []
        encodings = []
        for id, full_name, code_student, vector_face in rows:
            print(f"Raw vector_face for {full_name}: {vector_face}")  # Debug print
            if not vector_face or vector_face == "null":
                continue
            try:
                encoding_np = np.array(json.loads(vector_face), dtype=np.float32)
                labels.append((id, full_name, code_student))
                encodings.append(encoding_np)
            except Exception as e:
                print(f"Error parsing vector_face for {full_name}: {e}")

        if not encodings:
            print("No valid encodings found.")
            return False

        all_encodings = np.stack(encodings)
        faiss.normalize_L2(all_encodings)
        feature_dim = all_encodings.shape[1]

        if os.path.exists(faiss_path):
            try:
                index = faiss.read_index(faiss_path)
                if index.d != feature_dim:
                    print(f"Existing FAISS index dimension ({index.d}) does not match current feature dimension ({feature_dim}). Creating a new index.")
                    index = faiss.IndexFlatIP(feature_dim)
                else:
                    index.reset()
            except Exception as e:
                print(f"Error loading existing FAISS index: {e}. Creating a new one.")
                index = faiss.IndexFlatIP(feature_dim)
        else:
            index = faiss.IndexFlatIP(feature_dim)

        index.add(all_encodings)
        print(f"Added {index.ntotal} encodings to FAISS index.")
        faiss.write_index(index, faiss_path)
        print(f"FAISS index saved to: {faiss_path}")

        with open(labels_path, "w") as f:
            for id, full_name, code_student in labels:
                f.write(f"{id},{full_name},{code_student}\n")
        print(f"Labels saved to: {labels_path}")

        return True

    except Exception as e:
        print(f"Error populating FAISS from database: {e}")
        return False

if __name__ == "__main__":
    populate_faiss_from_db(
        db_path="../students_local.db",
        faiss_path="./faiss.index",
        labels_path="./labels.txt"
    )
