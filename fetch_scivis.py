#!/usr/bin/env python3

import os
import sys
import requests
import json
from urllib.parse import urlparse
from argparse import ArgumentParser

sizeof = {'uint8' : 1, 'uint16' : 2, 'int16' : 2, 'float32' : 4, 'float64' : 8} # bytes

parser = ArgumentParser(description='Fetch a scientific visualization dataset')
group  = parser.add_mutually_exclusive_group()
group.add_argument('-l', '--list', dest='do_list', action='store_true', help='List available datasets')
group.add_argument('-d', '--dataset', type=str, help='Name of the dataset to fetch')
args = parser.parse_args()

r = requests.get("https://klacansky.com/open-scivis-datasets/datasets.json")
index = r.json()

if args.do_list:
  print('Available datasets:')
  for x in index.values():
    name = x['name'].lower().replace(' ', '_')
    dims = [int(d) for d in x['size']]
    size = dims[0] * dims[1] * dims[2] * sizeof[x['type']] / 1048576
    print(f'  {name}', end=' ')
    print(f'({size/1024:.0f} GB)' if size > 1024 else f'({size:.0f} MB)' if size > 1 else '(<1 MB)')
  sys.exit(0)

meta = [x for x in index.values() if x["name"].lower().replace(' ', '_') == args.dataset][0]
print(json.dumps(meta, indent=4))

with open(args.dataset + ".json", "w") as f:
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

