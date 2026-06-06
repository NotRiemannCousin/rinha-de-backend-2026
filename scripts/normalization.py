import json
import struct

with open('data/references.json', 'r') as file:
    data = json.loads(file.read())

with open('data/dataset.bin', 'wb') as output:
    for v in data:
        output.write(struct.pack('<16f', *(v['vector'] + [0.0, 0.0])))

    for v in data:
        output.write(struct.pack('<?', v['label'] == 'legit'))