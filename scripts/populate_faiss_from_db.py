import sqlite3
import faiss
import numpy as np
import os

def populate_faiss_from_db(db_path="data/face_recognition.db", faiss_path="faiss.index", labels_path="labels.txt"):
    """
    Reads face encodings from SQLite database, builds an in-memory FAISS index,
    and then saves it to a specified FAISS index file along with corresponding labels.
    """
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()

        # Retrieve name and encoding from face_encodings table
        cursor.execute("SELECT name, encoding FROM face_encodings ORDER BY id ASC")
        rows = cursor.fetchall()
        conn.close()

        if not rows:
            print(" No face encodings found in the database. FAISS index will not be created.")
            # Ensure an empty FAISS index and labels file are created if nothing exists
            try:
                # Assuming feature_dim is 512 as per your feature_extractor.py
                # This might need to be dynamic or passed as a parameter if your model changes
                feature_dim = 512
                index = faiss.IndexFlatIP(feature_dim)
                faiss.write_index(index, faiss_path)
                with open(labels_path, "w") as f:
                    pass # Create an empty labels file
                print(f" Created empty FAISS index: {faiss_path} and labels file: {labels_path}")
            except Exception as e:
                print(f" Error creating empty FAISS index/labels file: {e}")
            return False


        names = []
        encodings = []
        for name, encoding_blob in rows:
            names.append(name)
            # Convert bytes back to numpy array (assuming float32 was used for storage)
            encoding_np = np.frombuffer(encoding_blob, dtype=np.float32)
            encodings.append(encoding_np)

        # Convert list of numpy arrays to a single 2D numpy array
        all_encodings = np.array(encodings, dtype=np.float32)

        # Normalize L2 before adding to FAISS (important for Inner Product search)
        faiss.normalize_L2(all_encodings)

        # Determine feature dimension from the first encoding
        feature_dim = all_encodings.shape[1]

        # Check if FAISS index file already exists
        if os.path.exists(faiss_path):
            try:
                index = faiss.read_index(faiss_path)
                # Check if the dimensions match
                if index.d != feature_dim:
                    print(f" Existing FAISS index dimension ({index.d}) does not match current feature dimension ({feature_dim}). Creating a new index.")
                    index = faiss.IndexFlatIP(feature_dim)
                else:
                    print(f"Loaded existing FAISS index from {faiss_path}.")
                    # Clear existing data if you always want to rebuild from scratch from DB
                    index.reset()
                    print("Cleared existing data in FAISS index.")
            except Exception as e:
                print(f"Error loading existing FAISS index: {e}. Creating a new one.")
                index = faiss.IndexFlatIP(feature_dim)
        else:
            index = faiss.IndexFlatIP(feature_dim) # Use Inner Product for cosine similarity

        # Add all encodings to the FAISS index
        index.add(all_encodings)
        print(f"Added {index.ntotal} encodings to FAISS index.")

        # Save the FAISS index to file
        faiss.write_index(index, faiss_path)
        print(f"FAISS index saved to: {faiss_path}")

        # Save the corresponding labels to labels.txt
        with open(labels_path, "w") as f:
            for name in names:
                f.write(f"{name}\n")
        print(f"Labels saved to: {labels_path}")

        return True

    except Exception as e:
        print(f"Error populating FAISS from database: {e}")
        return False

if __name__ == "__main__":

    # Example usage:
    populate_faiss_from_db(
        db_path="./face_recognition.db",
        faiss_path="./faiss.index",
        labels_path="./labels.txt"
    )

