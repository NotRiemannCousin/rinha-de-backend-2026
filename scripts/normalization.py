import json
import numpy as np
from cuml.cluster import KMeans as cuKMeans

print("A carregar os dados...")
with open('data/references.json', 'r') as file:
    data = json.loads(file.read())

vectors = np.array([v['vector'] + [0.0, 0.0] for v in data], dtype=np.float32)
labels = np.array([v['label'] == 'legit' for v in data], dtype=np.bool_)

best_k = 1280
print(f"Treinando K={best_k} na GPU (cuML)...")

kmeans = cuKMeans(n_clusters=best_k, random_state=42)
cluster_ids = kmeans.fit_predict(vectors)
centroids = kmeans.cluster_centers_.astype(np.float32)

sort_idx = np.argsort(cluster_ids)
sorted_vectors = vectors[sort_idx]
sorted_labels = labels[sort_idx]
sorted_cluster_ids = cluster_ids[sort_idx]

print("A guardar binários...")
with open('data/dataset.bin', 'wb') as f:
    f.write(sorted_vectors.tobytes())
    f.write(sorted_labels.tobytes())

starts = np.zeros(best_k, dtype=np.uint32)
ends = np.zeros(best_k, dtype=np.uint32)

for i in range(best_k):
    indices = np.where(sorted_cluster_ids == i)[0]
    if indices.size > 0:
        starts[i] = indices[0]
        ends[i] = indices[-1] + 1

with open('data/indexes.bin', 'wb') as f:
    f.write(centroids.tobytes())
    f.write(starts.tobytes())
    f.write(ends.tobytes())

print("Concluído.")