import json
from math import hypot
import struct

with open('data/references.json', 'r') as file:
    with open('data/output', 'wb') as output:
        
        data = json.loads(file.read())
    
        for v in data:
            length = hypot(*v['vector'])
            
            normalized = [el / length for el in v['vector']]
            is_legit = v['label'] == 'legit'
            
            # O Mac Mini 2014 tb é little endian, nn vou fazer essa burrada dnv kk
            output.write(struct.pack('<14f?', *normalized, is_legit))
    