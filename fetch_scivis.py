#!/usr/bin/env python3

import os
import sys
import requests
import json

if len(sys.argv) != 2:
    print("Usage: ./fetch_scivis.py <dataset name>")
    print("The dataset name should be all lowercase and without spaces to match the website")
    sys.exit(1)

r = requests.get("http://sci.utah.edu/~klacansky/cdn/open-scivis-datasets/{}/{}.json".format(sys.argv[1], sys.argv[1]))
meta = r.json()
print(json.dumps(meta, indent=4))

with open(sys.argv[1] + ".json", "w") as f:
    f.write(json.dumps(meta, indent=4))

volume_file = os.path.basename(meta["url"])
if not os.path.isfile(volume_file):
    print("Fetching volume from {}".format(meta["url"]))
    r = requests.get(meta["url"])
    data = bytes(r.content)
    with open(volume_file, "wb") as f:
        f.write(data)
else:
    print("File {} already exists, not re-downloading".format(volume_file))

