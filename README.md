# A Mini-app Example for Scientific Visualization Using OSPRay

Dependencies:

- OSPRay 2.0
- TBB
- SDL2
- GLM. For now please use my [fork of GLM](https://github.com/Twinklebear/glm) which provides
    some CMake utilities.
- VTK (optional) for computing explicit triangle isosurfaces for testing
- OpenVisus (optional) for loading IDX volumes

Use the provided "fetch_scivis.py" script to fetch a dataset and its
JSON metadata from [OpenScivisDatasets](https://klacansky.com/open-scivis-datasets/).
The script requires the [requests](https://requests.readthedocs.io/en/master/) library.

